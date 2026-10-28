// led_button.h —— 录音状态 LED + 手动触发按键
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_OFF = 0,
    LED_IDLE,      // 慢闪: 待机
    LED_RECORD,    // 快闪: 正在录音
    LED_UPLOAD,    // 双闪: 正在上传
    LED_CONFIG,    // 常亮: 配网/网页模式
    LED_ERROR,     // 常灭+周期性三闪: 故障
} led_mode_t;

typedef void (*btn_short_cb_t)(void);
typedef void (*btn_long_cb_t)(void);

void led_set(led_mode_t mode);
led_mode_t led_get(void);

// 短按 < 1s, 长按 >= 2s
void led_button_init(btn_short_cb_t short_cb, btn_long_cb_t long_cb);
bool button_is_pressed(void);

#ifdef __cplusplus
}
#endif
