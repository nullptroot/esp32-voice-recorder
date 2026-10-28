// time_sync.h —— NTP 对时 + 时间可信度 (不可信时回退会话编号命名)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void      time_sync_init(void);       // 设置 TZ=Asia/Shanghai
esp_err_t time_sync_start(void);      // 启动 SNTP (可重复调用)
bool      time_sync_trusted(void);    // 已同步且系统时间 >= 2024-01-01
esp_err_t time_sync_wait(uint32_t timeout_ms);

// 例: 2026-10-26T10:00:00+08:00
void time_sync_format_iso8601(char *buf, size_t len, time_t t);
// 例: 2026-10-26_10-00-00 (用于文件名)
void time_sync_format_fname(char *buf, size_t len, time_t t);

#ifdef __cplusplus
}
#endif
