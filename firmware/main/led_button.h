// led_button.h —— 录音状态 LED + 手动触发按键
#pragma once

#include <stdbool.h>
#include <stdint.h>

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

// 进入深睡前调用: 冻结 LED 任务, 之后不再改 GPIO (避免和 hold 抢引脚)
void led_freeze(void);

// 主按键 (PIN_BUTTON=GPIO41): 短按启停录音, 长按补传
void led_button_init(btn_short_cb_t short_cb, btn_long_cb_t long_cb);
// 额外按键 (例如 PIN_BUTTON_CFG=GPIO14): 同一套短按/长按语义
// ignore_ms: 启动后忽略这段时间内的按压 (用于"按键唤醒"时避免把手上的按压当成短按)
void led_button_init_extra(int gpio_num, btn_short_cb_t short_cb, btn_long_cb_t long_cb,
                           uint32_t ignore_ms);
bool button_is_pressed(void);

#ifdef __cplusplus
}
#endif
