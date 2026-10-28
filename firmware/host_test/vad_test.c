// vad_test.c —— PC 端 VAD 算法单测 (不依赖 ESP-IDF)
// 构建: gcc -O2 -I ../main vad_test.c ../main/vad.c -lm -o vad_test && ./vad_test
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vad.h"

#define RATE 16000
#define FRAME 320            // 20 ms
#define PI 3.14159265358979

static double rnd(void) { return (double)rand() / RAND_MAX; }

// 生成一帧: 白噪声 + 可选的“类人声”谐波 + 幅度包络
static void gen_frame(int16_t *out, int voiced, double speech_db, double noise_db)
{
    double amp_n = pow(10.0, noise_db / 20.0) * 32768.0;
    double amp_s = voiced ? pow(10.0, speech_db / 20.0) * 32768.0 : 0.0;
    for (int i = 0; i < FRAME; i++) {
        double t = (double)(rand() % 100000) / 100000.0;
        double n = (t * 2.0 - 1.0) * amp_n;   // 均匀白噪声
        double s = 0.0;
        if (voiced) {
            double ph = 2.0 * PI * i / RATE;
            // 基频 + 谐波, 模拟 120Hz 人声 / 以及 700Hz 共振峰
            s = amp_s * (0.55 * sin(ph * 130.0) +
                         0.30 * sin(ph * 260.0) +
                         0.10 * sin(ph * 780.0) +
                         0.05 * sin(ph * 1600.0));
        }
        double v = n + s;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }
}

int main(void)
{
    srand(1234);
    int16_t frame[FRAME];
    vad_t v;

    // ---- 测试 1: 纯噪声不应误报 ----
    int fp = 0, total = 0;
    vad_init(&v, 8);
    // 先喂 3 秒本底噪声做自适应
    for (int i = 0; i < 550; i++) {
        gen_frame(frame, 0, 0, -52.0);
        vad_frame(&v, frame, FRAME);
    }
    for (int i = 0; i < 300; i++) {          // 6 秒
        gen_frame(frame, 0, 0, -52.0);
        if (vad_frame(&v, frame, FRAME)) fp++;
        total++;
    }
    double fp_rate = (double)fp / total;
    printf("[test1] 纯噪声误报: %d/%d = %.1f%%\n", fp, total, fp_rate * 100);

    // ---- 测试 2: 人声帧检出率 (不同电平/不同噪声底) ----
    double levels[] = { -20.0, -30.0, -40.0 };
    double noises[] = { -62.0, -52.0, -45.0 };
    int all_ok = 1;
    for (int a = 0; a < 3; a++) {
        for (int b = 0; b < 3; b++) {
            int hit = 0, n = 0;
            vad_init(&v, 8);
            for (int i = 0; i < 550; i++) {
                gen_frame(frame, 0, 0, noises[b]);
                vad_frame(&v, frame, FRAME);
            }
            for (int i = 0; i < 100; i++) {
                gen_frame(frame, 1, levels[a], noises[b]);
                if (vad_frame(&v, frame, FRAME)) hit++;
                n++;
            }
            double rate = (double)hit / n;
            printf("[test2] 人声 %.0f dBFS / 噪声 %.0f dBFS -> 检出 %.1f%%\n",
                   levels[a], noises[b], rate * 100);
            if (rate < 0.90) {
                // 人声低于噪声底时不可能检出, 属于预期
                if (levels[a] - noises[b] > 10.0) {
                    printf("        ✗ 失败: 信噪比 %.0f dB 应可检出\n", levels[a] - noises[b]);
                    all_ok = 0;
                }
            }
        }
    }

    // ---- 测试 3: 静音帧不应被判定为人声 ----
    int silent_fp = 0;
    vad_init(&v, 8);
    for (int i = 0; i < 100; i++) {
        memset(frame, 0, sizeof(frame));
        if (vad_frame(&v, frame, FRAME)) silent_fp++;
    }
    printf("[test3] 全静音误报: %d/100\n", silent_fp);

    // ---- 测试 4: 噪声底自适应 (环境从 -52 变到 -38 后不应持续误报) ----
    int after_adapt = 0;
    vad_init(&v, 8);
    for (int i = 0; i < 550; i++) { gen_frame(frame, 0, 0, -52.0); vad_frame(&v, frame, FRAME); }
    for (int i = 0; i < 550; i++) { gen_frame(frame, 0, 0, -38.0); vad_frame(&v, frame, FRAME); } // 11s 适应
    for (int i = 0; i < 100; i++) {
        gen_frame(frame, 0, 0, -38.0);
        if (vad_frame(&v, frame, FRAME)) after_adapt++;
    }
    printf("[test4] 噪声底抬升后误报: %d/100\n", after_adapt);

    // ---- 测试 5: 持续工频/环境嗡声不应被判为人声 ----
    int hum_fp = 0;
    vad_init(&v, 6);
    for (int i = 0; i < 600; i++) {   // 12s 填充窗口
        gen_frame(frame, 0, 0, -30.0);
        vad_frame(&v, frame, FRAME);
    }
    for (int i = 0; i < 150; i++) {
        gen_frame(frame, 0, 0, -30.0);
        if (vad_frame(&v, frame, FRAME)) hum_fp++;
    }
    printf("[test5] 持续 -30dB 嗡声误报: 150 帧中 %d\n", hum_fp);
    // 在 -30dB 嗡声底噪上说话 (speech -22dB) 应能检出
    int hum_speech = 0;
    for (int i = 0; i < 100; i++) {
        gen_frame(frame, 1, -20.0, -30.0);
        if (vad_frame(&v, frame, FRAME)) hum_speech++;
    }
    printf("[test5] 嗡声底噪下说话检出: %d/100\n", hum_speech);

    int pass = (fp_rate < 0.10) && all_ok && (silent_fp == 0) && (after_adapt < 10) &&
               (hum_fp < 10) && (hum_speech > 70);
    printf("\n结果: %s\n", pass ? "✅ PASS" : "❌ FAIL");
    return pass ? 0 : 1;
}
