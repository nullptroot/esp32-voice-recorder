// net_wifi.h —— WiFi STA 管理
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t net_wifi_init(void);
esp_err_t net_wifi_connect(uint32_t timeout_ms);
esp_err_t net_wifi_disconnect(void);
bool      net_wifi_connected(void);
esp_err_t net_wifi_ip(char *buf, size_t len);
int       net_wifi_scan(cJSON *arr);   // 返回扫描到的 AP 数量

#ifdef __cplusplus
}
#endif
