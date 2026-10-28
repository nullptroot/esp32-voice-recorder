// net_wifi.c
#include "net_wifi.h"
#include "app_config.h"
#include "app_status.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_evt = NULL;
static bool s_inited = false;
static bool s_started = false;

#define BIT_CONNECTED   BIT0
#define BIT_FAIL        BIT1

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "断开 (reason=%d)", d ? d->reason : -1);
            if (s_evt) xEventGroupClearBits(s_evt, BIT_CONNECTED);
            STATUS_SET(wifi_connected, false);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "获得 IP: " IPSTR, IP2STR(&e->ip_info.ip));
        char ip[24];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        STATUS_STR(ip, ip);
        STATUS_SET(wifi_connected, true);
        if (s_evt) xEventGroupSetBits(s_evt, BIT_CONNECTED);
    }
}

esp_err_t net_wifi_init(void)
{
    if (s_inited) return ESP_OK;
    s_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_inited = true;
    return ESP_OK;
}

esp_err_t net_wifi_connect(uint32_t timeout_ms)
{
    if (!s_inited) {
        esp_err_t err = net_wifi_init();
        if (err != ESP_OK) return err;
    }
    const app_config_t *c = app_config_get();
    if (c->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "未配置 WiFi SSID");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, c->wifi_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, c->wifi_pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config 失败: %s", esp_err_to_name(err));
        return err;
    }
    if (!s_started) {
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            ESP_LOGE(TAG, "wifi_start 失败: %s", esp_err_to_name(err));
            return err;
        }
        s_started = true;
    }

    ESP_LOGI(TAG, "连接 WiFi: %s (超时 %u ms)", c->wifi_ssid, (unsigned)timeout_ms);
    xEventGroupClearBits(s_evt, BIT_CONNECTED);
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_CONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_CONNECTED) {
        ESP_LOGI(TAG, "WiFi 已连接");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "WiFi 连接超时");
    return ESP_ERR_TIMEOUT;
}

esp_err_t net_wifi_disconnect(void)
{
    if (!s_started) return ESP_OK;
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "disconnect: %s", esp_err_to_name(err));
    }
    STATUS_SET(wifi_connected, false);
    STATUS_STR(ip, "");
    return ESP_OK;
}

bool net_wifi_connected(void)
{
    if (!s_evt) return false;
    return (xEventGroupGetBits(s_evt) & BIT_CONNECTED) != 0;
}

esp_err_t net_wifi_ip(char *buf, size_t len)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return ESP_ERR_NOT_FOUND;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) return ESP_FAIL;
    if (ip.ip.addr == 0) return ESP_ERR_INVALID_STATE;
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}

int net_wifi_scan(cJSON *arr)
{
    if (!s_inited) return 0;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return 0;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;
    wifi_ap_record_t *recs = calloc(num ? num : 1, sizeof(wifi_ap_record_t));
    if (!recs) return 0;
    uint16_t got = num;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        free(recs);
        return 0;
    }
    for (int i = 0; i < got; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(o, "auth", recs[i].authmode);
        cJSON_AddItemToArray(arr, o);
    }
    free(recs);
    return got;
}
