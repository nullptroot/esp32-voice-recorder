// web_config.c —— 设备配置网页与 REST 接口
#include "web_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app_config.h"
#include "app_status.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_button.h"
#include "logbuf.h"
#include "net_wifi.h"
#include "recorder.h"
#include "sd_store.h"
#include "pins.h"
#include "time_sync.h"
#include "uploader.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static const char *TAG = "web";
static httpd_handle_t s_server = NULL;
static web_action_cb_t s_cb = NULL;
static void *s_cb_user = NULL;

static void send_json(httpd_req_t *req, cJSON *root, int status)
{
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status == 200 ? "200 OK"
                                            : (status == 400 ? "400 Bad Request" : "500 Internal Server Error"));
    if (out) {
        httpd_resp_sendstr(req, out);
        free(out);
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"json alloc failed\"}");
    }
}

static int read_body(httpd_req_t *req, char *buf, size_t len)
{
    int total = req->content_len;
    if (total <= 0) {
        buf[0] = '\0';
        return 0;
    }
    if ((size_t)total >= len) total = (int)len - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) break;
        got += r;
    }
    buf[got] = '\0';
    return got;
}

// ---------------- 状态 ----------------
static void status_to_json(cJSON *o)
{
    app_status_t st;
    app_status_snapshot(&st);
    const app_config_t *c = app_config_get();

    cJSON_AddStringToObject(o, "state", st.state_text);
    cJSON_AddBoolToObject(o, "wifi_connected", st.wifi_connected);
    cJSON_AddStringToObject(o, "ip", st.ip);
    cJSON_AddBoolToObject(o, "time_trusted", st.time_trusted || time_sync_trusted());
    cJSON_AddNumberToObject(o, "session_id", st.session_id);
    cJSON_AddNumberToObject(o, "uptime_s", st.uptime_s);
    cJSON_AddBoolToObject(o, "sd_mounted", sd_store_mounted());
    uint64_t total = 0, freeb = 0;
    sd_store_info_cached(&total, &freeb);
    cJSON_AddNumberToObject(o, "sd_total_mb", (double)total / 1048576.0);
    cJSON_AddNumberToObject(o, "sd_free_mb", (double)freeb / 1048576.0);
    cJSON_AddNumberToObject(o, "pending_files", sd_store_pending_cached());
    cJSON_AddNumberToObject(o, "recorded_files", st.recorded_files);
    cJSON_AddNumberToObject(o, "uploaded_files", st.uploaded_files);
    cJSON_AddNumberToObject(o, "failed_uploads", st.failed_uploads);
    cJSON_AddNumberToObject(o, "level_db", st.level_db);
    cJSON_AddBoolToObject(o, "vad_speech", st.vad_speech);
    cJSON_AddNumberToObject(o, "record_seconds", st.record_seconds);
    cJSON_AddStringToObject(o, "last_error", st.last_error);
    cJSON_AddStringToObject(o, "last_upload", st.last_upload);
    cJSON_AddStringToObject(o, "device_id", c->device_id);
    cJSON_AddStringToObject(o, "server_url", c->server_url);
    cJSON_AddStringToObject(o, "firmware", "recbean 1.0");
    char ip[24] = "";
    if (net_wifi_ip(ip, sizeof(ip)) == ESP_OK) {
        cJSON_AddStringToObject(o, "ip", ip);
    }
    cJSON_AddNumberToObject(o, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "sound_pin", gpio_get_level(PIN_SND_DETECT));
    cJSON_AddNumberToObject(o, "sound_hits", st.sound_hits);
    cJSON_AddBoolToObject(o, "sound_active_low", c->sound_active_low);
    cJSON_AddNumberToObject(o, "cfg_window_left", st.cfg_window_left);
}

static esp_err_t h_index(httpd_req_t *req)
{
    size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    status_to_json(o);
    send_json(req, o, 200);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "config", app_config_to_json(app_config_get()));
    send_json(req, o, 200);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    char body[2048];
    int n = read_body(req, body, sizeof(body));
    cJSON *root = n > 0 ? cJSON_Parse(body) : NULL;
    cJSON *resp = cJSON_CreateObject();

    if (!root) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "请求体不是合法 JSON");
        send_json(req, resp, 400);
        cJSON_Delete(resp);
        return ESP_OK;
    }

    app_config_t cfg = *app_config_get();
    char err[96] = "";
    esp_err_t e = app_config_from_json(root, &cfg, err, sizeof(err));
    cJSON_Delete(root);
    if (e != ESP_OK) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", err);
        send_json(req, resp, 400);
        cJSON_Delete(resp);
        return ESP_OK;
    }
    e = app_config_set(&cfg);
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    if (e != ESP_OK) {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(e));
    }
    cJSON_AddItemToObject(resp, "config", app_config_to_json(app_config_get()));
    send_json(req, resp, e == ESP_OK ? 200 : 500);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t h_log(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "lines");
    logbuf_to_json(arr);
    send_json(req, o, 200);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_files(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "files");
    static sd_file_t files[64];
    int n = sd_store_list(files, 64, false);
    sd_store_pending_refresh();
    for (int i = 0; i < n; i++) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", files[i].name);
        cJSON_AddNumberToObject(f, "size", files[i].size);
        cJSON_AddBoolToObject(f, "uploaded", files[i].uploaded);
        cJSON_AddItemToArray(arr, f);
    }
    cJSON_AddNumberToObject(o, "count", n);
    send_json(req, o, 200);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_download(httpd_req_t *req)
{
    char q[128] = "";
    char name[SD_NAME_LEN] = "";
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, q, sizeof(q));
        char val[SD_NAME_LEN];
        if (httpd_query_key_value(q, "name", val, sizeof(val)) == ESP_OK) {
            strncpy(name, val, sizeof(name) - 1);
        }
    }
    if (name[0] == '\0' || strstr(name, "..") || strchr(name, '/')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }
    char path[SD_NAME_LEN + 32];
    snprintf(path, sizeof(path), "%s/%.90s", SD_MOUNT_POINT, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }
    struct stat st;
    size_t size = 0;
    if (fstat(fileno(f), &st) == 0) size = (size_t)st.st_size;
    char disp[160];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%.90s\"", name);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Content-Length", NULL);
    static char buf[4096];
    size_t total = 0;
    esp_err_t err = ESP_OK;
    while (total < size) {
        size_t rd = fread(buf, 1, sizeof(buf), f);
        if (rd == 0) break;
        if (httpd_resp_send_chunk(req, buf, rd) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        total += rd;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t h_action(httpd_req_t *req)
{
    const char *uri = req->uri;
    web_action_t act;
    if (strstr(uri, "record/start")) act = WEB_ACT_RECORD;
    else if (strstr(uri, "record/stop")) act = WEB_ACT_STOP;
    else if (strstr(uri, "upload")) act = WEB_ACT_UPLOAD;
    else if (strstr(uri, "health")) act = WEB_ACT_HEALTH;
    else if (strstr(uri, "reboot")) act = WEB_ACT_REBOOT;
    else {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "未知动作");
        send_json(req, o, 400);
        cJSON_Delete(o);
        return ESP_OK;
    }

    char body[256];
    read_body(req, body, sizeof(body));

    if (act == WEB_ACT_RECORD) {
        app_status_t st;
        app_status_snapshot(&st);
        if (st.state == APP_RECORDING || st.state == APP_PRE_RECORD) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", false);
            cJSON_AddStringToObject(o, "error", "正在录音");
            send_json(req, o, 400);
            cJSON_Delete(o);
            return ESP_OK;
        }
    }
    if (act == WEB_ACT_STOP) {
        recorder_request_stop();
    }
    if (s_cb) s_cb(act, s_cb_user);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "action", uri);
    send_json(req, o, 200);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_health(httpd_req_t *req)
{
    char msg[192] = "";
    esp_err_t err = uploader_health(msg, sizeof(msg));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddStringToObject(o, "message", msg);
    send_json(req, o, err == ESP_OK ? 200 : 500);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_scan(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "aps");
    int n = net_wifi_scan(arr);
    cJSON_AddNumberToObject(o, "count", n);
    send_json(req, o, 200);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_sdtest(httpd_req_t *req)
{
    char q[128] = "";
    uint32_t start = 0, count = 256, nsec = 1;
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, q, sizeof(q));
        char v[24];
        if (httpd_query_key_value(q, "start", v, sizeof(v)) == ESP_OK) start = (uint32_t)strtoul(v, NULL, 0);
        if (httpd_query_key_value(q, "count", v, sizeof(v)) == ESP_OK) count = (uint32_t)strtoul(v, NULL, 0);
        if (httpd_query_key_value(q, "nsec", v, sizeof(v)) == ESP_OK) nsec = (uint32_t)strtoul(v, NULL, 0);
    }
    if (count > 4096) count = 4096;
    uint32_t failed[64];
    int nfail = sd_store_raw_read_test_n(start, count, nsec, failed, 64);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "start", start);
    cJSON_AddNumberToObject(o, "count", count);
    cJSON_AddNumberToObject(o, "nsec", nsec);
    cJSON_AddNumberToObject(o, "failed", nfail);
    cJSON *arr = cJSON_AddArrayToObject(o, "failed_blocks");
    for (int i = 0; i < nfail && i < 64; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(failed[i]));
    }
    send_json(req, o, 200);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_recover(httpd_req_t *req)
{
    esp_err_t err = sd_store_recover();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddStringToObject(o, "result", esp_err_to_name(err));
    send_json(req, o, 200);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_format(httpd_req_t *req)
{
    char body[128];
    read_body(req, body, sizeof(body));
    cJSON *o = cJSON_CreateObject();
    if (strstr(body, "YES") == NULL) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "需要确认: body 须包含 YES");
        send_json(req, o, 400);
        cJSON_Delete(o);
        return ESP_OK;
    }
    esp_err_t err = sd_store_format();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    send_json(req, o, err == ESP_OK ? 200 : 500);
    cJSON_Delete(o);
    return ESP_OK;
}

esp_err_t web_config_start(web_action_cb_t cb, void *user)
{
    if (s_server) return ESP_OK;
    s_cb = cb;
    s_cb_user = user;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 12288;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_index },
        { .uri = "/api/status", .method = HTTP_GET, .handler = h_status },
        { .uri = "/api/config", .method = HTTP_GET, .handler = h_config_get },
        { .uri = "/api/config", .method = HTTP_POST, .handler = h_config_post },
        { .uri = "/api/log", .method = HTTP_GET, .handler = h_log },
        { .uri = "/api/files", .method = HTTP_GET, .handler = h_files },
        { .uri = "/api/file", .method = HTTP_GET, .handler = h_download },
        { .uri = "/api/sdtest", .method = HTTP_GET, .handler = h_sdtest },
        { .uri = "/api/health", .method = HTTP_GET, .handler = h_health },
        { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = h_scan },
        { .uri = "/api/format", .method = HTTP_POST, .handler = h_format },
        { .uri = "/api/sd/recover", .method = HTTP_POST, .handler = h_recover },
        { .uri = "/api/record/start", .method = HTTP_POST, .handler = h_action },
        { .uri = "/api/record/stop", .method = HTTP_POST, .handler = h_action },
        { .uri = "/api/upload", .method = HTTP_POST, .handler = h_action },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = h_action },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "配置网页已启动: http://<ip>/");
    led_set(LED_CONFIG);
    return ESP_OK;
}

void web_config_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "配置网页已停止");
}

bool web_config_running(void)
{
    return s_server != NULL;
}
