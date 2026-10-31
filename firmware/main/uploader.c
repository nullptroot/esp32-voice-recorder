// uploader.c
#include "uploader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app_config.h"
#include "app_status.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_button.h"
#include "net_wifi.h"
#include "pins.h"
#include "sd_store.h"
#include "time_sync.h"

static const char *TAG = "upload";
static const char *BOUNDARY = "----RecBeanBoundary7MA4YWxkTrZu0gW";

typedef struct {
    char device_id[40];
    char recorded_at[48];   // 空字符串表示 null
    bool time_trusted;
    char time_source[16];   // ntp / rtc / restored / filename / none
    char session_id[32];
    int  sequence;
    long long monotonic_ms;
    long long duration_sec;
} file_meta_t;

static void meta_default(const char *name, file_meta_t *m)
{
    memset(m, 0, sizeof(*m));
    const app_config_t *c = app_config_get();
    strncpy(m->device_id, c->device_id, sizeof(m->device_id) - 1);
    strncpy(m->session_id, "session-0000", sizeof(m->session_id) - 1);
    m->time_trusted = false;
    strncpy(m->time_source, "none", sizeof(m->time_source) - 1);
    m->sequence = 0;
    m->monotonic_ms = 0;

    // 文件名回退解析: mic1_2026-10-26_10-00-00_part3.wav
    const char *p = strstr(name, "mic1_");
    if (p) {
        p += 5;
        if (strlen(p) >= 19 && p[4] == '-' && p[7] == '-' && p[10] == '_' &&
            p[13] == '-' && p[16] == '-') {
            char iso[48];
            snprintf(iso, sizeof(iso), "%.4s-%.2s-%.2sT%.2s:%.2s:%.2s+08:00",
                     p, p + 5, p + 8, p + 11, p + 14, p + 17);
            strncpy(m->recorded_at, iso, sizeof(m->recorded_at) - 1);
            m->time_trusted = true;
            strncpy(m->time_source, "filename", sizeof(m->time_source) - 1);
        } else if (strncmp(p, "session-", 8) == 0) {
            const char *q = strchr(p, '_');
            size_t n = q ? (size_t)(q - p) : strlen(p);
            if (n >= sizeof(m->session_id)) n = sizeof(m->session_id) - 1;
            memcpy(m->session_id, p, n);
            m->session_id[n] = '\0';
        }
    }
    const char *part = strstr(name, "_part");
    if (part) {
        m->sequence = atoi(part + 5);
    }
}

static void meta_load(const char *name, file_meta_t *m)
{
    meta_default(name, m);
    char meta_name[SD_NAME_LEN + 8];
    snprintf(meta_name, sizeof(meta_name), "%s.json", name);
    char buf[600];
    if (sd_store_read_text(meta_name, buf, sizeof(buf)) != ESP_OK) {
        return;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) return;
    cJSON *it = cJSON_GetObjectItem(root, "device_id");
    if (cJSON_IsString(it) && it->valuestring[0]) {
        strncpy(m->device_id, it->valuestring, sizeof(m->device_id) - 1);
    }
    it = cJSON_GetObjectItem(root, "recorded_at");
    if (cJSON_IsString(it)) {
        strncpy(m->recorded_at, it->valuestring, sizeof(m->recorded_at) - 1);
    } else {
        m->recorded_at[0] = '\0';
    }
    it = cJSON_GetObjectItem(root, "time_trusted");
    m->time_trusted = cJSON_IsTrue(it);
    it = cJSON_GetObjectItem(root, "time_source");
    if (cJSON_IsString(it) && it->valuestring[0]) {
        strncpy(m->time_source, it->valuestring, sizeof(m->time_source) - 1);
    }
    it = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(it)) {
        strncpy(m->session_id, it->valuestring, sizeof(m->session_id) - 1);
    }
    it = cJSON_GetObjectItem(root, "sequence");
    if (cJSON_IsNumber(it)) m->sequence = it->valueint;
    it = cJSON_GetObjectItem(root, "monotonic_ms");
    if (cJSON_IsNumber(it)) m->monotonic_ms = (long long)it->valuedouble;
    it = cJSON_GetObjectItem(root, "duration_sec");
    if (cJSON_IsNumber(it)) m->duration_sec = (long long)it->valuedouble;
    cJSON_Delete(root);
}

static int build_text_fields(char *buf, size_t len, const file_meta_t *m)
{
    int n = 0;
    n += snprintf(buf + n, len - n,
                  "\r\n--%s\r\nContent-Disposition: form-data; name=\"device_id\"\r\n\r\n%s",
                  BOUNDARY, m->device_id);
    n += snprintf(buf + n, len - n,
                  "\r\n--%s\r\nContent-Disposition: form-data; name=\"recorded_at\"\r\n\r\n%s",
                  BOUNDARY, m->recorded_at);
    n += snprintf(buf + n, len - n,
                  "\r\n--%s\r\nContent-Disposition: form-data; name=\"time_trusted\"\r\n\r\n%s",
                  BOUNDARY, m->time_trusted ? "true" : "false");
    n += snprintf(buf + n, len - n,
                  "\r\n--%s\r\nContent-Disposition: form-data; name=\"time_source\"\r\n\r\n%s",
                  BOUNDARY, m->time_source[0] ? m->time_source : "none");
    n += snprintf(buf + n, len - n,
                  "\r\n--%s\r\nContent-Disposition: form-data; name=\"session_id\"\r\n\r\n%s",
                  BOUNDARY, m->session_id);
    n += snprintf(buf + n, len - n,
                  "\r\n--%s\r\nContent-Disposition: form-data; name=\"sequence\"\r\n\r\n%d",
                  BOUNDARY, m->sequence);
    n += snprintf(buf + n, len - n,
                  "\r\n--%s\r\nContent-Disposition: form-data; name=\"monotonic_ms\"\r\n\r\n%lld",
                  BOUNDARY, m->monotonic_ms);
    n += snprintf(buf + n, len - n,
                  "\r\n--%s--\r\n", BOUNDARY);
    return n;
}

static esp_err_t do_post(const char *url, const char *name, uint32_t file_size,
                         const file_meta_t *m, int *out_status, char *body,
                         size_t bodylen)
{
    char path[SD_NAME_LEN + 32];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开文件失败: %s", path);
        return ESP_FAIL;
    }

    static char file_hdr[320];
    int file_hdr_len = snprintf(file_hdr, sizeof(file_hdr),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", BOUNDARY, name);

    static char text_fields[1024];
    int text_len = build_text_fields(text_fields, sizeof(text_fields), m);
    long long content_len = (long long)file_hdr_len + file_size + text_len;

    esp_http_client_config_t hc = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) {
        fclose(f);
        return ESP_FAIL;
    }
    char ct[96];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", BOUNDARY);
    esp_http_client_set_header(cli, "Content-Type", ct);
    esp_http_client_set_header(cli, "X-Device-Id", m->device_id);

    esp_err_t err = esp_http_client_open(cli, content_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_open 失败: %s (%s)", esp_err_to_name(err), url);
        esp_http_client_cleanup(cli);
        fclose(f);
        return err;
    }
    if (esp_http_client_write(cli, file_hdr, file_hdr_len) != file_hdr_len) {
        err = ESP_FAIL;
        goto done;
    }
    {
        static char iobuf[4096];
        size_t total = 0;
        while (total < file_size) {
            size_t want = file_size - total;
            if (want > sizeof(iobuf)) want = sizeof(iobuf);
            size_t rd = fread(iobuf, 1, want, f);
            if (rd == 0) {
                ESP_LOGE(TAG, "读文件提前结束: %s", name);
                err = ESP_FAIL;
                goto done;
            }
            int w = esp_http_client_write(cli, iobuf, rd);
            if (w != (int)rd) {
                ESP_LOGE(TAG, "上传写失败 (%d/%u)", w, (unsigned)rd);
                err = ESP_FAIL;
                goto done;
            }
            total += rd;
        }
    }
    if (esp_http_client_write(cli, text_fields, text_len) != text_len) {
        err = ESP_FAIL;
        goto done;
    }

    // 注意: fetch_headers 返回的是响应 content-length (>0 表示成功), 不是 esp_err_t
    int64_t resp_len = esp_http_client_fetch_headers(cli);
    if (resp_len < 0) {
        ESP_LOGE(TAG, "fetch_headers 失败: %lld", (long long)resp_len);
        err = ESP_FAIL;
        goto done;
    }
    int status = esp_http_client_get_status_code(cli);
    int rd = esp_http_client_read(cli, body, bodylen - 1);
    if (rd < 0) rd = 0;
    body[rd] = '\0';
    *out_status = status;
    err = ESP_OK;

done:
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    fclose(f);
    return err;
}

esp_err_t uploader_upload_one(const char *name, char *msg, size_t msglen,
                              bool *out_duplicate)
{
    const app_config_t *c = app_config_get();
    if (out_duplicate) *out_duplicate = false;

    char path[SD_NAME_LEN + 32];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, name);
    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(msg, msglen, "文件不存在: %s", name);
        return ESP_ERR_NOT_FOUND;
    }

    file_meta_t m;
    meta_load(name, &m);

    char url[DEVICE_MAX_URL + 24];
    const char *base = c->server_url;
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/') bl--;
    snprintf(url, sizeof(url), "%.*s/upload", (int)bl, base);

    int attempt = 0;
    int max_attempt = c->upload_max_retry;
    uint32_t backoff = c->upload_backoff_ms;
    esp_err_t last_err = ESP_FAIL;

    while (attempt < max_attempt) {
        attempt++;
        char body[512];
        int status = 0;
        esp_err_t err = do_post(url, name, (uint32_t)st.st_size, &m, &status, body, sizeof(body));
        if (err == ESP_OK && status >= 200 && status < 300) {
            // 注意: 响应体里永远有 "duplicate" 这个键, 必须看它的值是否为 true
            bool dup = strstr(body, "\"duplicate\":true") != NULL ||
                       strstr(body, "\"duplicate\": true") != NULL;
            if (out_duplicate) *out_duplicate = dup;
            if (msglen) {
                snprintf(msg, msglen, "HTTP %d %s", status,
                         dup ? "(duplicate)" : "(stored)");
            }
            // 写 .up 标记, 内含服务器返回体, 便于审计
            char up_name[SD_NAME_LEN + 8];
            snprintf(up_name, sizeof(up_name), "%s.up", name);
            char up_json[640];
            snprintf(up_json, sizeof(up_json),
                     "{\"uploaded_at\":%lld,\"http_status\":%d,\"response\":%s}",
                     (long long)time(NULL), status,
                     body[0] ? body : "null");
            sd_store_write_text(up_name, up_json);
            STATUS_STR(last_upload, name);
            return ESP_OK;
        }
        last_err = (err != ESP_OK) ? err : ESP_ERR_INVALID_RESPONSE;
        ESP_LOGW(TAG, "%s 第 %d/%d 次上传失败 (http=%d, err=%s)",
                 name, attempt, max_attempt, status, esp_err_to_name(last_err));
        if (attempt < max_attempt) {
            uint32_t wait = backoff;
            if (wait > 60000) wait = 60000;
            ESP_LOGI(TAG, "退避 %u ms 后重试", (unsigned)wait);
            vTaskDelay(pdMS_TO_TICKS(wait));
            backoff *= 2;
        }
    }
    if (msglen) snprintf(msg, msglen, "上传失败 (尝试 %d 次, %s)", attempt, esp_err_to_name(last_err));
    return last_err;
}

esp_err_t uploader_health(char *msg, size_t msglen)
{
    const app_config_t *c = app_config_get();
    char url[DEVICE_MAX_URL + 24];
    size_t bl = strlen(c->server_url);
    while (bl > 0 && c->server_url[bl - 1] == '/') bl--;
    snprintf(url, sizeof(url), "%.*s/health", (int)bl, c->server_url);

    esp_http_client_config_t hc = { .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 5000 };
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) return ESP_FAIL;
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    if (err == ESP_OK && status == 200) {
        snprintf(msg, msglen, "健康检查 OK (HTTP 200) %s", url);
    } else {
        snprintf(msg, msglen, "健康检查失败 (http=%d, %s) %s", status,
                 esp_err_to_name(err), url);
        err = ESP_FAIL;
    }
    esp_http_client_cleanup(cli);
    return err;
}

esp_err_t uploader_run(upload_report_t *rep)
{
    memset(rep, 0, sizeof(*rep));
    const app_config_t *c = app_config_get();

    if (!net_wifi_connected()) {
        ESP_LOGI(TAG, "上传前连接 WiFi...");
        esp_err_t werr = net_wifi_connect(15000);
        if (werr != ESP_OK) {
            snprintf(rep->last_message, sizeof(rep->last_message), "WiFi 未连接");
            STATUS_STR(last_error, "上传: WiFi 未连接");
            return werr;
        }
    }

    rep->pending_total = sd_store_pending_refresh();
    if (rep->pending_total == 0) {
        snprintf(rep->last_message, sizeof(rep->last_message), "没有待上传文件");
        return ESP_OK;
    }

    app_status_set_state(APP_UPLOADING);
    led_set(LED_UPLOAD);
    ESP_LOGI(TAG, "开始上传 %d 个文件 -> %s", rep->pending_total, c->server_url);

    sd_file_t files[32];
    int n = 0;
    while ((n = sd_store_list(files, 32, true)) > 0) {
        bool progress = false;
        for (int i = 0; i < n; i++) {
            if (files[i].size < 100) {
                ESP_LOGW(TAG, "文件过小/损坏, 删除: %s (%u 字节)", files[i].name, files[i].size);
                sd_store_delete(files[i].name);
                rep->skipped++;
                progress = true;
                continue;
            }
            char msg[160] = "";
            bool dup = false;
            esp_err_t err = uploader_upload_one(files[i].name, msg, sizeof(msg), &dup);
            rep->attempted++;
            if (err == ESP_OK) {
                rep->uploaded++;
                if (dup) rep->duplicates++;
                progress = true;
                ESP_LOGI(TAG, "上传成功: %s -> %s", files[i].name, msg);
                app_status_t *s = app_status_lock();
                s->uploaded_files++;
                app_status_unlock();
                if (c->delete_after_upload) {
                    sd_store_delete(files[i].name);
                }
            } else {
                rep->failed++;
                snprintf(rep->last_message, sizeof(rep->last_message), "%.70s: %.60s",
                         files[i].name, msg);
                STATUS_STR(last_error, msg);
                ESP_LOGE(TAG, "上传失败: %s -> %s", files[i].name, msg);
            }
        }
        if (!progress) {
            snprintf(rep->last_message, sizeof(rep->last_message),
                     "%d 个文件上传失败, 保留待下次重试", rep->failed);
            break;
        }
    }

    if (c->keep_days > 0) {
        sd_store_remove_old(c->keep_days);
    }
    sd_store_pending_refresh();

    app_status_set_state(APP_IDLE);
    led_set(LED_IDLE);
    ESP_LOGI(TAG, "上传结束: 成功 %d, 失败 %d, 重复 %d",
             rep->uploaded, rep->failed, rep->duplicates);
    return (rep->failed == 0) ? ESP_OK : ESP_FAIL;
}
