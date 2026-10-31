// time_sync.h —— NTP 对时 + 时间可信度 + 断电重启后的时间恢复
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 设置 TZ=Asia/Shanghai; 若系统时间 < 2024-01-01, 从 NVS 恢复上次已知时间 (近似)
void      time_sync_init(void);
esp_err_t time_sync_start(void);      // 启动 SNTP (可重复调用)

// 系统时间是否可用 (>= 2024-01-01)。用于文件名, 包括"从 NVS 恢复的近似时间"。
bool      time_sync_valid(void);
// 本次启动是否已由 NTP 校准成功
bool      time_sync_synced(void);
// 时间是否可信: 可用且不是"断电恢复的近似时间" (NTP 或深睡 RTC 保持)
bool      time_sync_trusted(void);
// "ntp" / "rtc" / "restored" / "none"
const char *time_sync_source(void);

esp_err_t time_sync_wait(uint32_t timeout_ms);
// 把当前可用时间写回 NVS (深睡前/定时/重启前调用)
void      time_sync_save(void);

// 例: 2026-10-26T10:00:00+08:00
void time_sync_format_iso8601(char *buf, size_t len, time_t t);
// 例: 2026-10-26_10-00-00 (用于文件名)
void time_sync_format_fname(char *buf, size_t len, time_t t);

#ifdef __cplusplus
}
#endif
