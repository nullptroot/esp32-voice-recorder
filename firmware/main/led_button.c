// led_button.c
#include "led_button.h"
#include "pins.h"
#include "app_status.h"

#include <stdio.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";
static volatile led_mode_t s_mode = LED_OFF;
static volatile bool s_frozen = false;

void led_set(led_mode_t mode)
{
    s_mode = mode;
}

led_mode_t led_get(void)
{
    return s_mode;
}

void led_freeze(void)
{
    s_frozen = true;
}

bool button_is_pressed(void)
{
    return gpio_get_level(PIN_BUTTON) == 0;
}

static void led_task(void *arg)
{
    (void)arg;
    int tick = 0;
    gpio_set_level(PIN_LED, 0);
    while (1) {
        if (s_frozen) {
            // 深睡前已冻结: 不再碰任何 LED 引脚, 让 enter_deep_sleep() 独占设置 hold
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        led_mode_t m = s_mode;
        int level = 0;
        switch (m) {
        case LED_OFF:
            level = 0;
            break;
        case LED_IDLE:
            level = ((tick / 5) % 2) ? 1 : 0;   // 100ms 步进 -> 1s 周期慢闪
            break;
        case LED_RECORD:
            level = ((tick / 2) % 2) ? 1 : 0;   // 5Hz 快闪
            break;
        case LED_UPLOAD:
            level = ((tick % 10) < 3) ? 1 : 0;  // 每秒 3 个 100ms 亮
            break;
        case LED_CONFIG:
            level = 1;
            break;
        case LED_ERROR:
            level = ((tick % 20) < 1) ? 1 : 0;
            break;
        }
        gpio_set_level(PIN_LED, level);

        // 板载 LED (GPIO48) 已按用户要求停用: 不再跟随录音状态, 保持熄灭。
        // 注意: 深睡时 GPIO48 悬空、LED 阳极接 3V3, 漏电流仍会把它点亮; 软件无法
        // 完全关断, 需要在 GPIO48 与 3V3 之间加 10k 上拉 (硬件修法)。
        // 这里不再写 GPIO48, 避免录音时亮灯造成"隐私指示"误解。

        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

typedef struct {
    int             pin;
    btn_short_cb_t  short_cb;
    btn_long_cb_t   long_cb;
    int64_t         ignore_until_us;
} btn_ctx_t;

static void button_task(void *arg)
{
    btn_ctx_t *ctx = (btn_ctx_t *)arg;
    bool prev = false;
    int held_ticks = 0;
    bool long_fired = false;

    while (1) {
        bool now = (gpio_get_level((gpio_num_t)ctx->pin) == 0);
        int64_t now_us = esp_timer_get_time();

        // 唤醒后的静默期: 只跟踪状态, 不发事件。这样"按住按键唤醒"不会被误判成长按。
        if (now_us < ctx->ignore_until_us) {
            prev = now;
            held_ticks = 0;
            long_fired = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (now && !prev) {
            held_ticks = 0;
            long_fired = false;
        }
        if (now) {
            held_ticks++;
            if (!long_fired && held_ticks * 20 >= 2000) {
                long_fired = true;
                ESP_LOGI(TAG, "按键 GPIO%d 长按 (>=2s)", ctx->pin);
                if (ctx->long_cb) ctx->long_cb();
            }
        }
        if (!now && prev) {
            int ms = held_ticks * 20;
            if (!long_fired && ms >= 40) {
                ESP_LOGI(TAG, "按键 GPIO%d 短按 (%dms)", ctx->pin, ms);
                if (ctx->short_cb) ctx->short_cb();
            }
        }
        prev = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void button_gpio_config(int pin)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);
}

static void button_start(int pin, btn_short_cb_t short_cb, btn_long_cb_t long_cb,
                         uint32_t ignore_ms)
{
    static btn_ctx_t ctx_primary;
    static btn_ctx_t ctx_extra;
    btn_ctx_t *ctx = (pin == PIN_BUTTON) ? &ctx_primary : &ctx_extra;

    ctx->pin = pin;
    ctx->short_cb = short_cb;
    ctx->long_cb = long_cb;
    ctx->ignore_until_us = esp_timer_get_time() + (int64_t)ignore_ms * 1000;

    button_gpio_config(pin);
    char name[12];
    snprintf(name, sizeof(name), "btn%d", pin);
    xTaskCreatePinnedToCore(button_task, name, 3072, ctx, 5, NULL, 0);
}

void led_button_init(btn_short_cb_t short_cb, btn_long_cb_t long_cb)
{
    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(PIN_LED, 0);

    // 板载 LED (GPIO48): 低电平点亮。已停用为录音指示灯, 初始化后保持高电平(灭)。
    // 使能内部上拉作为"输出高失效/引脚悬空"时的兜底。
    gpio_config_t onb_cfg = {
        .pin_bit_mask = 1ULL << PIN_LED_ONBOARD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&onb_cfg);
    gpio_set_level(PIN_LED_ONBOARD, 1);

    button_start(PIN_BUTTON, short_cb, long_cb, 0);

    xTaskCreatePinnedToCore(led_task, "led", 2560, NULL, 4, NULL, 0);
}

void led_button_init_extra(int gpio_num, btn_short_cb_t short_cb, btn_long_cb_t long_cb,
                           uint32_t ignore_ms)
{
    button_start(gpio_num, short_cb, long_cb, ignore_ms);
}
