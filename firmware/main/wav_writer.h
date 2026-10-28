// wav_writer.h —— 16bit PCM WAV 写入 (写时占位头, 关闭时回填大小)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void    *fp;           // FILE*
    char     path[160];
    uint32_t sample_rate;
    uint32_t data_bytes;
    uint32_t start_uptime_ms;
    bool     open;
} wav_writer_t;

esp_err_t wav_open(wav_writer_t *w, const char *path, uint32_t sample_rate);
esp_err_t wav_write(wav_writer_t *w, const int16_t *samples, size_t count);
esp_err_t wav_close(wav_writer_t *w);
uint32_t  wav_seconds(const wav_writer_t *w);

#ifdef __cplusplus
}
#endif
