// led_button.c
#include "led_button.h"
#include "pins.h"
#include "app_status.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";
static volatile led_mode_t s_mode = LED_OFF;
static btn_short_cb_t s_short_cb = NULL;
static btn_long_cb_t s_long_cb = NULL;

void led_set(led_mode_t mode)
{
    s_mode = mode;
}

led_mode_t led_get(void)
{
    return s_mode;
}

bool button_is_pressed(void)
{
    return gpio_get_level(PIN_BUTTON) == 0;
}

static void led_task(void *arg)
{
    int tick = 0;
    gpio_set_level(PIN_LED, 0);
    while (1) {
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

        // 板载 LED (GPIO48, 低电平点亮): 只跟录音状态绑定 —— 录音/预录时常亮, 其余全灭。
        // 单独给个"一眼能看出来"的指示, 不参与上面的闪烁语义。
        app_status_t st;
        app_status_snapshot(&st);
        bool rec = (st.state == APP_RECORDING || st.state == APP_PRE_RECORD);
        gpio_set_level(PIN_LED_ONBOARD, rec ? 0 : 1);

        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void button_task(void *arg)
{
    bool prev = false;
    int held_ticks = 0;
    bool long_fired = false;

    while (1) {
        bool now = button_is_pressed();
        if (now && !prev) {
            held_ticks = 0;
            long_fired = false;
        }
        if (now) {
            held_ticks++;
            if (!long_fired && held_ticks * 20 >= 2000) {
                long_fired = true;
                ESP_LOGI(TAG, "按键长按 (>=2s)");
                if (s_long_cb) s_long_cb();
            }
        }
        if (!now && prev) {
            int ms = held_ticks * 20;
            if (!long_fired && ms >= 40) {
                ESP_LOGI(TAG, "按键短按 (%dms)", ms);
                if (s_short_cb) s_short_cb();
            }
        }
        prev = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void led_button_init(btn_short_cb_t short_cb, btn_long_cb_t long_cb)
{
    s_short_cb = short_cb;
    s_long_cb = long_cb;

    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(PIN_LED, 0);

    // 板载 LED (GPIO48): 低电平点亮, 初始化先熄灭
    gpio_config_t onb_cfg = {
        .pin_bit_mask = 1ULL << PIN_LED_ONBOARD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&onb_cfg);
    gpio_set_level(PIN_LED_ONBOARD, 1);

    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    xTaskCreatePinnedToCore(led_task, "led", 2560, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 5, NULL, 0);
}
