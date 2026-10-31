// time_sync.c
#include "time_sync.h"

#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "app_config.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "time";
static bool s_sntp_started = false;
static volatile bool s_synced = false;
static bool s_valid = false;
// 本次启动的时间来自 NVS (断电重启, 误差 = 断电时长), 只能算"近似时间"
static bool s_from_nvs = false;

#define MIN_VALID_EPOCH 1704067200  // 2024-01-01T00:00:00Z

static bool sys_time_ok(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec >= MIN_VALID_EPOCH;
}

static void log_now(const char *what)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_now);
    ESP_LOGW(TAG, "%s: %s (TZ=CST-8)", what, buf);
}

void time_sync_init(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    if (sys_time_ok()) {
        // 深睡唤醒: RTC 时钟一直在走, 时间仍然准确
        s_valid = true;
        return;
    }

    // 断电 / 复位后系统时间归零: 用上一次保存的墙钟时间兜底。
    // 误差 = 断电时长, 但至少文件名有日期, 不至于回退成 session-XXXX。
    uint32_t last = app_config_last_epoch();
    if (last >= MIN_VALID_EPOCH) {
        struct timeval tv = { .tv_sec = (time_t)last, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        s_valid = true;
        s_from_nvs = true;
        log_now("从 NVS 恢复时间 (近似, 联网后 NTP 校正)");
    } else {
        ESP_LOGW(TAG, "系统时间不可信且无历史时间, 文件名将回退为 session-XXXX");
    }
}

esp_err_t time_sync_start(void)
{
    if (!s_sntp_started) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
        cfg.start = true;
        cfg.smooth_sync = false;
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SNTP 初始化失败: %s", esp_err_to_name(err));
            return err;
        }
        s_sntp_started = true;
    }
    return ESP_OK;
}

bool time_sync_valid(void)
{
    if (!s_valid && sys_time_ok()) s_valid = true;
    return s_valid;
}

bool time_sync_synced(void)
{
    return s_synced;
}

bool time_sync_trusted(void)
{
    return time_sync_valid() && !s_from_nvs;
}

const char *time_sync_source(void)
{
    if (s_synced) return "ntp";
    if (!time_sync_valid()) return "none";
    if (s_from_nvs) return "restored";
    return "rtc";
}

esp_err_t time_sync_wait(uint32_t timeout_ms)
{
    time_sync_start();
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK) {
        s_synced = true;
        s_valid = true;
        s_from_nvs = false;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        app_config_set_last_epoch((uint32_t)tv.tv_sec);
        log_now("NTP 对时成功");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "NTP 对时失败: %s", esp_err_to_name(err));
    return err;
}

void time_sync_save(void)
{
    if (!time_sync_valid()) return;
    time_t now = time(NULL);
    if (now >= MIN_VALID_EPOCH) {
        app_config_set_last_epoch((uint32_t)now);
    }
}

void time_sync_format_iso8601(char *buf, size_t len, time_t t)
{
    struct tm tm_v;
    localtime_r(&t, &tm_v);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S+08:00", &tm_v);
}

void time_sync_format_fname(char *buf, size_t len, time_t t)
{
    struct tm tm_v;
    localtime_r(&t, &tm_v);
    strftime(buf, len, "%Y-%m-%d_%H-%M-%S", &tm_v);
}
