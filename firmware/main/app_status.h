// app_status.h —— 全局运行状态快照 (供网页 /api/status 与日志使用)
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_IDLE = 0,
    APP_PRE_RECORD,
    APP_RECORDING,
    APP_UPLOADING,
    APP_COOLDOWN,
    APP_SLEEPING,
    APP_ERROR,
} app_state_t;

typedef struct {
    app_state_t state;
    char        state_text[24];

    bool        wifi_connected;
    char        ip[24];
    bool        time_trusted;

    uint32_t    session_id;
    uint32_t    uptime_s;
    uint32_t    cycle_id;          // 每次唤醒/触发 +1

    bool        sd_mounted;
    uint64_t    sd_total_bytes;
    uint64_t    sd_free_bytes;

    int         pending_files;
    uint32_t    recorded_files;
    uint32_t    uploaded_files;
    uint32_t    failed_uploads;

    int16_t     level_db;          // 最近一帧 RMS dBFS
    bool        vad_speech;
    uint16_t    record_seconds;    // 当前文件已录秒数

    int         sound_pin;         // GPIO15 当前电平
    uint32_t    sound_hits;        // 电平跳变次数 (调试 LM393 用)

    char        last_error[128];
    char        last_upload[128];
    int64_t     started_ms;        // 当前录音文件开始时刻 (uptime ms)
} app_status_t;

void app_status_init(void);
app_status_t *app_status_lock(void);
void app_status_unlock(void);
void app_status_snapshot(app_status_t *out);
void app_status_set_state(app_state_t st);
const char *app_state_name(app_state_t st);

#define STATUS_SET(field, value)                 \
    do {                                         \
        app_status_t *s_ = app_status_lock();    \
        s_->field = (value);                     \
        app_status_unlock();                     \
    } while (0)

#define STATUS_STR(field, value)                          \
    do {                                                  \
        app_status_t *s_ = app_status_lock();             \
        strncpy(s_->field, (value), sizeof(s_->field) - 1); \
        s_->field[sizeof(s_->field) - 1] = '\0';          \
        app_status_unlock();                              \
    } while (0)

#ifdef __cplusplus
}
#endif
