// mic_i2s.c —— INMP441 (I2S, 32bit slot, 单声道左通道)
#include "mic_i2s.h"
#include "pins.h"

#include <string.h>
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "mic";
static i2s_chan_handle_t s_rx = NULL;
static bool s_running = false;
static uint32_t s_rate = 16000;
static uint8_t s_gain = 0;
static int32_t s_raw_buf[512];
// 一阶高通 (DC blocker): y[n] = x[n] - x[n-1] + a*y[n-1]
static float s_dc_prev_x = 0.0f;
static float s_dc_prev_y = 0.0f;
#define DC_BLOCKER_A 0.995f

esp_err_t mic_init(uint32_t sample_rate, uint8_t gain_shift)
{
    if (s_rx) {
        if (s_rate == sample_rate && s_gain == gain_shift) {
            return ESP_OK;
        }
        mic_deinit();
    }
    s_rate = sample_rate;
    s_gain = gain_shift > 4 ? 4 : gain_shift;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 12;
    chan_cfg.dma_frame_num = 1024;
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.auto_clear_before_cb = false;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCK,
            .ws = PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    // INMP441 的 L/R 已接 GND => 数据在左声道
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode 失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    ESP_LOGI(TAG, "I2S 初始化完成: %u Hz, gain_shift=%u, bclk=%d ws=%d din=%d",
             (unsigned)sample_rate, s_gain, PIN_I2S_BCK, PIN_I2S_WS, PIN_I2S_DIN);
    return ESP_OK;
}

esp_err_t mic_start(void)
{
    if (!s_rx) return ESP_ERR_INVALID_STATE;
    if (s_running) return ESP_OK;
    esp_err_t err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) {
        s_running = true;
        s_dc_prev_x = 0.0f;
        s_dc_prev_y = 0.0f;
        // 丢弃前 ~320ms, 并让 DC blocker 收敛 (避免文件头出现启动阶跃)
        int16_t tmp[512];
        size_t got = 0;
        for (int i = 0; i < 10; i++) {
            mic_read(tmp, sizeof(tmp) / sizeof(tmp[0]), &got, 100);
        }
        ESP_LOGI(TAG, "I2S 已启动");
    } else {
        ESP_LOGE(TAG, "i2s_channel_enable 失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t mic_stop(void)
{
    if (!s_rx || !s_running) return ESP_OK;
    esp_err_t err = i2s_channel_disable(s_rx);
    s_running = false;
    ESP_LOGI(TAG, "I2S 已停止");
    return err;
}

void mic_deinit(void)
{
    if (!s_rx) return;
    if (s_running) {
        i2s_channel_disable(s_rx);
        s_running = false;
    }
    i2s_del_channel(s_rx);
    s_rx = NULL;
}

bool mic_is_running(void)
{
    return s_running;
}

esp_err_t mic_read(int16_t *dst, size_t max_samples, size_t *out_samples, uint32_t timeout_ms)
{
    if (!s_rx || !s_running) return ESP_ERR_INVALID_STATE;
    size_t want_frames = max_samples;
    size_t chunk = want_frames > (sizeof(s_raw_buf) / sizeof(s_raw_buf[0]))
                       ? (sizeof(s_raw_buf) / sizeof(s_raw_buf[0]))
                       : want_frames;
    size_t total = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (total < want_frames) {
        size_t frames = want_frames - total;
        if (frames > chunk) frames = chunk;
        size_t bytes_read = 0;
        int64_t remain_ms = (deadline - esp_timer_get_time()) / 1000;
        if (remain_ms <= 0) break;
        esp_err_t err = i2s_channel_read(s_rx, s_raw_buf, frames * sizeof(int32_t),
                                         &bytes_read, (uint32_t)remain_ms);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            *out_samples = total;
            return err;
        }
        size_t got = bytes_read / sizeof(int32_t);
        // INMP441: 24bit 数据左对齐在 32bit 字里 -> 右移 16 位得到 16bit
        // gain_shift 额外左移作为数字增益, 带饱和
        int shift = 16 - (int)s_gain;
        for (size_t i = 0; i < got; i++) {
            int64_t v = (int64_t)s_raw_buf[i] >> shift;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            // DC blocker, 去掉 INMP441 的直流偏置与启动阶跃
            float x = (float)v;
            float y = x - s_dc_prev_x + DC_BLOCKER_A * s_dc_prev_y;
            s_dc_prev_x = x;
            s_dc_prev_y = y;
            int32_t o = (int32_t)y;
            if (o > 32767) o = 32767;
            if (o < -32768) o = -32768;
            dst[total + i] = (int16_t)o;
        }
        total += got;
        if (got == 0) break;
    }
    *out_samples = total;
    return total > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}
