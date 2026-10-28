// mic_i2s.h —— INMP441 I2S 采集
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mic_init(uint32_t sample_rate, uint8_t gain_shift);
esp_err_t mic_start(void);
esp_err_t mic_stop(void);
void      mic_deinit(void);
bool      mic_is_running(void);

// 读取最多 max_samples 个 int16 单声道样本; timeout_ms 为等待超时
esp_err_t mic_read(int16_t *dst, size_t max_samples, size_t *out_samples, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
