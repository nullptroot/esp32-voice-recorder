// web_config.h —— 设备内置配置网页 (httpd)
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEB_ACT_RECORD = 0,
    WEB_ACT_STOP,
    WEB_ACT_UPLOAD,
    WEB_ACT_HEALTH,
    WEB_ACT_REBOOT,
    WEB_ACT_SLEEP,
} web_action_t;

typedef void (*web_action_cb_t)(web_action_t act, void *user);

esp_err_t web_config_start(web_action_cb_t cb, void *user);
void      web_config_stop(void);
bool      web_config_running(void);

#ifdef __cplusplus
}
#endif
