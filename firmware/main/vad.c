// vad.c —— 轻量 VAD: 滑动窗最小值噪声底 + 能量门限 + 过零率门限
// 设计要点:
//   1. 噪声底 = 最近 3 秒窗口内能量最小值 + 3dB (最小统计量法),
//      比"只降不升"的指数平滑更抗环境噪声/工频嗡声;
//   2. 过零率过滤高频电路噪声 (zcr > 0.46 基本不是人声);
//   3. 绝对门限 -62 dBFS, 避免极安静环境放大本底;
// 升级 esp-sr VADNet 时只需替换本文件, 保持 vad_init/vad_frame 接口不变。
#include "vad.h"

#include <math.h>
#include <string.h>

#define VAD_ABS_FLOOR_DB   (-62.0f)
#define VAD_ZCR_MIN        0.003f
#define VAD_ZCR_MAX        0.46f
#define VAD_NOISE_OFFSET   2.0f    // 最小值之上加 2dB 作为噪声底
#define VAD_HIST_LEN       500     // 10 秒 @ 20ms 帧 (窗口需长于人声段落)

void vad_init(vad_t *v, int thr_db)
{
    memset(v, 0, sizeof(*v));
    v->threshold_db = thr_db;
    v->noise_db = -75.0f;
    v->primed = 0;
}

static void hist_push(vad_t *v, float db)
{
    v->hist[v->hist_idx] = db;
    v->hist_idx = (v->hist_idx + 1) % VAD_HIST_LEN;
    if (v->hist_cnt < VAD_HIST_LEN) v->hist_cnt++;
}

static float hist_min(const vad_t *v)
{
    float m = 0.0f;
    int first = 1;
    for (int i = 0; i < v->hist_cnt; i++) {
        float d = v->hist[i];
        if (first || d < m) { m = d; first = 0; }
    }
    return m;
}

int vad_frame(vad_t *v, const int16_t *samples, size_t n)
{
    if (n < 32) {
        return 0;
    }

    double sum_sq = 0.0;
    int zc = 0;
    int16_t prev = samples[0];
    for (size_t i = 0; i < n; i++) {
        double x = (double)samples[i];
        sum_sq += x * x;
        if (i > 0) {
            if ((prev < 0 && samples[i] >= 0) || (prev >= 0 && samples[i] < 0)) {
                zc++;
            }
        }
        prev = samples[i];
    }

    double rms = sqrt(sum_sq / (double)n);
    if (rms < 1.0) rms = 1.0;
    float db = 20.0f * log10f((float)(rms / 32768.0));
    float zcr = (float)zc / (float)(n - 1);

    v->last_db = db;
    v->last_zcr = zcr;
    v->frames++;

    hist_push(v, db);
    v->noise_db = hist_min(v) + VAD_NOISE_OFFSET;
    if (!v->primed && v->hist_cnt >= 10) {
        v->primed = 1;
    }

    if (db < VAD_ABS_FLOOR_DB) {
        return 0;
    }
    if (zcr < VAD_ZCR_MIN || zcr > VAD_ZCR_MAX) {
        return 0;
    }
    // 窗口还没填满时用更保守的判定 (噪声底可能偏高)
    float margin = (v->hist_cnt < 100) ? 3.0f : 0.0f;
    return (db > v->noise_db + (float)v->threshold_db + margin) ? 1 : 0;
}
