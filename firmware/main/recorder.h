// recorder.h —— 一次录音循环: 预录 → VAD 二次确认 → 分段写 WAV → 元数据
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REC_TRIG_EXT1 = 0,   // LM393 唤醒 (需要 VAD 二次确认)
    REC_TRIG_AUTO,
    REC_TRIG_BUTTON,     // 显式触发, 直接录
    REC_TRIG_WEB,
} rec_trigger_t;

typedef struct {
    int      parts;          // 本会话写出的分段数
    uint32_t total_seconds;
    uint32_t written_bytes;
    uint16_t session_id;
    bool     voice_confirmed; // EXT1/AUTO 触发时 VAD 是否确认到人声
    bool     aborted;
    char     last_file[96];
} rec_summary_t;

// 阻塞执行一次完整录音循环
esp_err_t recorder_run(rec_trigger_t trig, rec_summary_t *out);
// 请求提前停止 (按键/网页)
void recorder_request_stop(void);
bool recorder_stop_requested(void);

#ifdef __cplusplus
}
#endif
