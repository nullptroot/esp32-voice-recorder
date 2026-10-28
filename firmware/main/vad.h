// vad.h —— 轻量能量 + 过零率人声检测 (纯 C, 可在 PC 上单测)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VAD_HIST_LEN 500   // 与 vad.c 保持一致: 10 秒 @ 20ms

typedef struct {
    int   threshold_db;   // 高于噪声底判定阈值
    float noise_db;       // 当前噪声底 (dBFS, 滑动窗最小值 + 3dB)
    int   primed;         // 噪声底是否已稳定
    int   frames;         // 已处理帧数
    float last_db;        // 最近一帧能量
    float last_zcr;       // 最近一帧过零率

    float hist[VAD_HIST_LEN];
    int   hist_idx;
    int   hist_cnt;
} vad_t;

// thr_db: 高于噪声底多少 dB 算人声 (建议 6~12)
void vad_init(vad_t *v, int thr_db);

// 处理一帧 (建议 20ms, 320 样本 @16kHz)
// 返回 1 = 该帧判定为人声, 0 = 非人声
int vad_frame(vad_t *v, const int16_t *samples, size_t n);

#ifdef __cplusplus
}
#endif
