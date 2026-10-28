// time_sync.c
#include "time_sync.h"

#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "time";
static bool s_sntp_started = false;
static volatile bool s_synced = false;

#define MIN_VALID_EPOCH 1704067200  // 2024-01-01T00:00:00Z

void time_sync_init(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
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

bool time_sync_trusted(void)
{
    if (!s_synced) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (tv.tv_sec < MIN_VALID_EPOCH) {
            return false;
        }
    }
    return true;
}

esp_err_t time_sync_wait(uint32_t timeout_ms)
{
    time_sync_start();
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK) {
        s_synced = true;
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_now);
        ESP_LOGI(TAG, "NTP 对时成功: %s (TZ=CST-8)", buf);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "NTP 对时失败: %s", esp_err_to_name(err));
    return err;
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
