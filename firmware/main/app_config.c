// app_config.c
#include "app_config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";
static const char *NVS_NS = "recbean";
static const char *KEY_CFG = "cfg";
static const char *KEY_BOOT_SESSION = "bsession";

static app_config_t s_cfg;
static nvs_handle_t s_nvs = 0;
static bool s_inited = false;

static void safe_copy(char *dst, size_t dstlen, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dstlen - 1);
    dst[dstlen - 1] = '\0';
}

static void clamp_cfg(app_config_t *c)
{
    if (c->sample_rate < 8000 || c->sample_rate > 48000) c->sample_rate = 16000;
    if (c->gain_shift > 4) c->gain_shift = 0;
    if (c->segment_seconds < 5) c->segment_seconds = 900;
    if (c->segment_seconds > 3600) c->segment_seconds = 3600;
    if (c->max_record_seconds < 5) c->max_record_seconds = 5;
    if (c->max_record_seconds > 7200) c->max_record_seconds = 7200;
    if (c->pre_record_ms < 300) c->pre_record_ms = 300;
    if (c->pre_record_ms > 10000) c->pre_record_ms = 10000;
    if (c->silence_stop_ms > 60000) c->silence_stop_ms = 60000;
    if (c->cooldown_seconds > 3600) c->cooldown_seconds = 3600;
    if (c->vad_threshold_db < 2) c->vad_threshold_db = 2;
    if (c->vad_threshold_db > 40) c->vad_threshold_db = 40;
    if (c->upload_max_retry < 1) c->upload_max_retry = 1;
    if (c->upload_max_retry > 20) c->upload_max_retry = 20;
    if (c->upload_backoff_ms < 200) c->upload_backoff_ms = 200;
    if (c->upload_backoff_ms > 60000) c->upload_backoff_ms = 60000;
    if (c->keep_days > 3650) c->keep_days = 3650;
    if (c->sd_freq_khz < 1000 || c->sd_freq_khz > 40000) c->sd_freq_khz = 1000;
    c->wifi_ssid[DEVICE_MAX_SSID] = '\0';
    c->wifi_pass[DEVICE_MAX_PASS] = '\0';
    c->server_url[DEVICE_MAX_URL] = '\0';
    c->device_id[DEVICE_MAX_DEVID] = '\0';
    c->version = CFG_VERSION;
}

void app_config_defaults(app_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = CFG_VERSION;
    safe_copy(c->wifi_ssid, sizeof(c->wifi_ssid), "FreshTomato24");
    safe_copy(c->wifi_pass, sizeof(c->wifi_pass), "mrchen613");
    safe_copy(c->server_url, sizeof(c->server_url), "http://192.168.5.36:8000");
    safe_copy(c->device_id, sizeof(c->device_id), "esp32s3-001");

    c->sample_rate = 16000;
    c->gain_shift = 2;   // INMP441 输出偏弱, 默认 +12dB (可网页调整)
    c->segment_seconds = 900;       // 15 分钟
    c->max_record_seconds = 1800;
    c->pre_record_ms = 1500;
    c->silence_stop_ms = 5000;
    c->cooldown_seconds = 20;

    c->vad_threshold_db = 8;
    c->vad_enable = true;

    c->upload_on_record = true;
    c->upload_on_boot = true;
    c->delete_after_upload = false;
    c->keep_days = 7;
    c->upload_max_retry = 5;
    c->upload_backoff_ms = 2000;

    c->sleep_enable = false;        // 验收阶段: 默认常醒
    c->bench_mode = true;
    c->wifi_off_while_recording = true;

    c->format_sd_if_mount_failed = true;
    c->sound_active_low = false;
    c->sd_freq_khz = 1000;
}

esp_err_t app_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 失败: %s", esp_err_to_name(err));
        return err;
    }

    app_config_defaults(&s_cfg);

    size_t len = sizeof(s_cfg);
    app_config_t tmp;
    err = nvs_get_blob(s_nvs, KEY_CFG, &tmp, &len);
    if (err == ESP_OK && len == sizeof(app_config_t) && tmp.version == CFG_VERSION) {
        s_cfg = tmp;
        clamp_cfg(&s_cfg);
        ESP_LOGI(TAG, "配置已从 NVS 载入 (ssid=%s, server=%s, seg=%us)",
                 s_cfg.wifi_ssid, s_cfg.server_url, s_cfg.segment_seconds);
    } else {
        ESP_LOGW(TAG, "无有效配置 (err=%s len=%u), 使用默认值并保存",
                 esp_err_to_name(err), (unsigned)len);
        clamp_cfg(&s_cfg);
        app_config_save(&s_cfg);
    }
    s_inited = true;
    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    return &s_cfg;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (!s_inited && s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    app_config_t tmp = *cfg;
    clamp_cfg(&tmp);
    esp_err_t err = nvs_set_blob(s_nvs, KEY_CFG, &tmp, sizeof(tmp));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_commit(s_nvs);
    return err;
}

esp_err_t app_config_set(app_config_t *cfg)
{
    app_config_t tmp = *cfg;
    clamp_cfg(&tmp);
    esp_err_t err = app_config_save(&tmp);
    if (err == ESP_OK) {
        s_cfg = tmp;
        ESP_LOGI(TAG, "配置已更新: ssid=%s server=%s seg=%us vad=%ddB sleep=%d bench=%d",
                 s_cfg.wifi_ssid, s_cfg.server_url, s_cfg.segment_seconds,
                 s_cfg.vad_threshold_db, (int)s_cfg.sleep_enable, (int)s_cfg.bench_mode);
    }
    return err;
}

cJSON *app_config_to_json(const app_config_t *cfg)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "version", cfg->version);
    cJSON_AddStringToObject(o, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(o, "wifi_pass", cfg->wifi_pass);
    cJSON_AddStringToObject(o, "server_url", cfg->server_url);
    cJSON_AddStringToObject(o, "device_id", cfg->device_id);
    cJSON_AddNumberToObject(o, "sample_rate", cfg->sample_rate);
    cJSON_AddNumberToObject(o, "gain_shift", cfg->gain_shift);
    cJSON_AddNumberToObject(o, "segment_seconds", cfg->segment_seconds);
    cJSON_AddNumberToObject(o, "max_record_seconds", cfg->max_record_seconds);
    cJSON_AddNumberToObject(o, "pre_record_ms", cfg->pre_record_ms);
    cJSON_AddNumberToObject(o, "silence_stop_ms", cfg->silence_stop_ms);
    cJSON_AddNumberToObject(o, "cooldown_seconds", cfg->cooldown_seconds);
    cJSON_AddNumberToObject(o, "vad_threshold_db", cfg->vad_threshold_db);
    cJSON_AddBoolToObject(o, "vad_enable", cfg->vad_enable);
    cJSON_AddBoolToObject(o, "upload_on_record", cfg->upload_on_record);
    cJSON_AddBoolToObject(o, "upload_on_boot", cfg->upload_on_boot);
    cJSON_AddBoolToObject(o, "delete_after_upload", cfg->delete_after_upload);
    cJSON_AddNumberToObject(o, "keep_days", cfg->keep_days);
    cJSON_AddNumberToObject(o, "upload_max_retry", cfg->upload_max_retry);
    cJSON_AddNumberToObject(o, "upload_backoff_ms", cfg->upload_backoff_ms);
    cJSON_AddBoolToObject(o, "sleep_enable", cfg->sleep_enable);
    cJSON_AddBoolToObject(o, "bench_mode", cfg->bench_mode);
    cJSON_AddBoolToObject(o, "wifi_off_while_recording", cfg->wifi_off_while_recording);
    cJSON_AddBoolToObject(o, "format_sd_if_mount_failed", cfg->format_sd_if_mount_failed);
    cJSON_AddBoolToObject(o, "sound_active_low", cfg->sound_active_low);
    cJSON_AddNumberToObject(o, "sd_freq_khz", cfg->sd_freq_khz);
    return o;
}

static bool json_str(const cJSON *root, const char *key, char *dst, size_t dstlen)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(it) && it->valuestring) {
        safe_copy(dst, dstlen, it->valuestring);
        return true;
    }
    return false;
}

static bool json_num(const cJSON *root, const char *key, int *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(it)) { *out = it->valueint; return true; }
    if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) {
        *out = atoi(it->valuestring);
        return true;
    }
    return false;
}

static bool json_bool(const cJSON *root, const char *key, bool *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(it)) { *out = cJSON_IsTrue(it); return true; }
    if (cJSON_IsNumber(it)) { *out = it->valueint != 0; return true; }
    if (cJSON_IsString(it)) {
        const char *s = it->valuestring;
        *out = (strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0 || strcasecmp(s, "on") == 0);
        return true;
    }
    return false;
}

esp_err_t app_config_from_json(const cJSON *root, app_config_t *cfg,
                               char *err, size_t errlen)
{
    if (!cJSON_IsObject(root)) {
        if (err) snprintf(err, errlen, "body 必须是 JSON 对象");
        return ESP_ERR_INVALID_ARG;
    }
    app_config_t c = s_cfg;
    int iv = 0;
    bool bv = false;

    json_str(root, "wifi_ssid", c.wifi_ssid, sizeof(c.wifi_ssid));
    json_str(root, "wifi_pass", c.wifi_pass, sizeof(c.wifi_pass));
    json_str(root, "server_url", c.server_url, sizeof(c.server_url));
    json_str(root, "device_id", c.device_id, sizeof(c.device_id));

    if (json_num(root, "sample_rate", &iv)) c.sample_rate = (uint16_t)iv;
    if (json_num(root, "gain_shift", &iv)) c.gain_shift = (uint8_t)iv;
    if (json_num(root, "segment_seconds", &iv)) c.segment_seconds = (uint16_t)iv;
    if (json_num(root, "max_record_seconds", &iv)) c.max_record_seconds = (uint16_t)iv;
    if (json_num(root, "pre_record_ms", &iv)) c.pre_record_ms = (uint16_t)iv;
    if (json_num(root, "silence_stop_ms", &iv)) c.silence_stop_ms = (uint16_t)iv;
    if (json_num(root, "cooldown_seconds", &iv)) c.cooldown_seconds = (uint16_t)iv;
    if (json_num(root, "vad_threshold_db", &iv)) c.vad_threshold_db = (int16_t)iv;
    if (json_num(root, "keep_days", &iv)) c.keep_days = (uint16_t)iv;
    if (json_num(root, "upload_max_retry", &iv)) c.upload_max_retry = (uint8_t)iv;
    if (json_num(root, "upload_backoff_ms", &iv)) c.upload_backoff_ms = (uint16_t)iv;

    if (json_bool(root, "vad_enable", &bv)) c.vad_enable = bv;
    if (json_bool(root, "upload_on_record", &bv)) c.upload_on_record = bv;
    if (json_bool(root, "upload_on_boot", &bv)) c.upload_on_boot = bv;
    if (json_bool(root, "delete_after_upload", &bv)) c.delete_after_upload = bv;
    if (json_bool(root, "sleep_enable", &bv)) c.sleep_enable = bv;
    if (json_bool(root, "bench_mode", &bv)) c.bench_mode = bv;
    if (json_bool(root, "wifi_off_while_recording", &bv)) c.wifi_off_while_recording = bv;
    if (json_bool(root, "format_sd_if_mount_failed", &bv)) c.format_sd_if_mount_failed = bv;
    if (json_bool(root, "sound_active_low", &bv)) c.sound_active_low = bv;
    if (json_num(root, "sd_freq_khz", &iv)) c.sd_freq_khz = (uint16_t)iv;

    if (c.server_url[0] == '\0') {
        if (err) snprintf(err, errlen, "server_url 不能为空");
        return ESP_ERR_INVALID_ARG;
    }
    if (c.device_id[0] == '\0') {
        if (err) snprintf(err, errlen, "device_id 不能为空");
        return ESP_ERR_INVALID_ARG;
    }
    *cfg = c;
    return ESP_OK;
}

// ---- 会话编号 (时间不可信时的回退命名) ----
static uint32_t s_boot_session = 0;

uint32_t app_config_boot_session(void)
{
    return s_boot_session;
}

void app_config_set_boot_session(uint32_t v)
{
    s_boot_session = v;
}

uint32_t app_config_session_id(void)
{
    uint32_t v = 0;
    if (nvs_get_u32(s_nvs, KEY_BOOT_SESSION, &v) != ESP_OK) {
        v = 0;
    }
    return v;
}

void app_config_session_bump(void)
{
    uint32_t v = app_config_session_id() + 1;
    nvs_set_u32(s_nvs, KEY_BOOT_SESSION, v);
    nvs_commit(s_nvs);
    s_boot_session = v;
}
