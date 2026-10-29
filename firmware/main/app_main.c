// app_main.c —— 级联唤醒状态机: 唤醒 → VAD 确认 → 录音 → 上传 → 深睡
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "app_status.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_button.h"
#include "logbuf.h"
#include "net_wifi.h"
#include "pins.h"
#include "recorder.h"
#include "sd_store.h"
#include "time_sync.h"
#include "uploader.h"
#include "web_config.h"

static const char *TAG = "main";

typedef enum {
    TRIG_RECORD_AUTO = 0,
    TRIG_RECORD_MANUAL,
    TRIG_UPLOAD,
} trigger_t;

static QueueHandle_t s_q = NULL;
static bool s_busy = false;
static volatile bool s_reboot_pending = false;
static bool s_wake_from_ext1 = false;

// 配置窗口: 非 bench 模式下, 上电/复位后先开这么久的网页, 没人管再进深睡。
// 否则一旦设成 bench=false, 网页就再也进不去 (深睡会关 WiFi + httpd)。
#define CFG_WINDOW_S        120    // 秒; 0 = 关闭上电配置窗口
#define RST_STREAK_LIMIT    3      // 连续快速复位多少次 -> 强制配置模式
#define RST_STREAK_CLEAR_S  15     // 正常运行这么久就认为不是"连续复位"

static bool    s_cfg_window = false;
static int64_t s_cfg_window_end_us = 0;

static void enter_deep_sleep(void);

static void enqueue(trigger_t t)
{
    if (s_q) xQueueSend(s_q, &t, 0);
}

// ---------------- 按键 ----------------
static void on_button_short(void)
{
    app_status_t st;
    app_status_snapshot(&st);
    if (st.state == APP_RECORDING || st.state == APP_PRE_RECORD) {
        recorder_request_stop();
    } else {
        enqueue(TRIG_RECORD_MANUAL);
    }
}

static void on_button_long(void)
{
    app_status_t st;
    app_status_snapshot(&st);
    if (st.state == APP_RECORDING) {
        recorder_request_stop();
    } else {
        enqueue(TRIG_UPLOAD);
    }
}

// ---------------- LM393 电平监视 (调试/标定阈值用) ----------------
static void snd_monitor_task(void *arg)
{
    int prev = gpio_get_level(PIN_SND_DETECT);
    while (1) {
        int lvl = gpio_get_level(PIN_SND_DETECT);
        if (lvl != prev) {
            bool act = app_config_get()->sound_active_low ? (lvl == 0) : (lvl != 0);
            ESP_LOGI(TAG, "LM393 DO: %d -> %d (%s)", prev, lvl, act ? "检测到声音" : "安静");
            app_status_t *s = app_status_lock();
            s->sound_pin = lvl;
            s->sound_hits++;
            app_status_unlock();
            prev = lvl;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------------- 网页动作 ----------------
static void on_web_action(web_action_t act, void *user)
{
    (void)user;
    switch (act) {
    case WEB_ACT_RECORD: enqueue(TRIG_RECORD_MANUAL); break;
    case WEB_ACT_STOP:   recorder_request_stop(); break;
    case WEB_ACT_UPLOAD: enqueue(TRIG_UPLOAD); break;
    case WEB_ACT_HEALTH: break;   // 由 http 处理函数直接完成
    case WEB_ACT_REBOOT: s_reboot_pending = true; break;
    }
}

// ---------------- 配置窗口 ----------------
// 深睡模式 (bench=false + sleep=true) 下 WiFi/httpd 都是关的, 网页进不去。
// 这里提供两个"重新进网页"的入口:
//   1) 每次上电/复位 (非 LM393 唤醒) 自动开 CFG_WINDOW_S 秒配置窗口;
//   2) 连续快速复位 RST_STREAK_LIMIT 次 -> 强制本轮开配置窗口 (深睡中也能救回来)。
static void cfg_window_open(int secs)
{
    s_cfg_window = true;
    s_cfg_window_end_us = esp_timer_get_time() + (int64_t)secs * 1000000;
    web_config_start(on_web_action, NULL);
    led_set(LED_CONFIG);
    STATUS_SET(cfg_window_left, (uint16_t)secs);
    ESP_LOGW(TAG, "配置窗口: %d 秒内可打开 http://<ip>/ 改配置, 之后自动进深睡", secs);
}

static uint32_t cfg_window_left(void)
{
    if (!s_cfg_window) return 0;
    int64_t left = (s_cfg_window_end_us - esp_timer_get_time()) / 1000000;
    return left > 0 ? (uint32_t)left : 0;
}

static bool cfg_window_active(void)
{
    if (!s_cfg_window) return false;
    if (esp_timer_get_time() < s_cfg_window_end_us) return true;
    s_cfg_window = false;
    STATUS_SET(cfg_window_left, 0);
    ESP_LOGI(TAG, "配置窗口结束");
    return false;
}

static void rst_streak_clear_cb(void *arg)
{
    (void)arg;
    app_config_rst_streak_clear();
}

// ---------------- 电源 / 深睡 ----------------
// LM393 当前是否处于"检测到声音"的有效电平
static bool sound_active(void)
{
    int lvl = gpio_get_level(PIN_SND_DETECT);
    return app_config_get()->sound_active_low ? (lvl == 0) : (lvl != 0);
}

static void enter_deep_sleep(void)
{
    // 睡眠前必须回到"无声音"电位, 否则会立刻被再次唤醒 (死循环)
    for (int i = 0; i < 30 && sound_active(); i++) {
        ESP_LOGW(TAG, "GPIO%d 仍处于有效电平, 等待安静 (%d/30)", PIN_SND_DETECT, i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (sound_active()) {
        ESP_LOGW(TAG, "持续有声音, 仍然进入睡眠 (可能立即被唤醒)");
    }
    ESP_LOGI(TAG, "进入 EXT1 深度睡眠, 唤醒脚 GPIO%d (低电平触发)", PIN_SND_DETECT);

    // 正常进睡 -> 清掉"连续复位"计数, 避免深睡唤醒被误判成连按复位
    app_config_rst_streak_clear();

    web_config_stop();
    net_wifi_disconnect();
    sd_store_unmount();
    led_set(LED_OFF);
    gpio_set_level(PIN_LED, 0);

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_SND_DETECT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_hold_en(PIN_SND_DETECT);
    gpio_deep_sleep_hold_en();

    esp_sleep_ext1_wakeup_mode_t mode = app_config_get()->sound_active_low
                                            ? ESP_EXT1_WAKEUP_ANY_LOW
                                            : ESP_EXT1_WAKEUP_ANY_HIGH;
    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_SND_DETECT, mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ext1 唤醒配置失败: %s", esp_err_to_name(err));
    }
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    app_status_set_state(APP_SLEEPING);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_deep_sleep_start();
}

// ---------------- 录音 + 上传 一个完整周期 ----------------
static void do_record_cycle(rec_trigger_t trig)
{
    const app_config_t *c = app_config_get();
    bool wifi_was_on = net_wifi_connected();

    if (c->wifi_off_while_recording && wifi_was_on && !c->bench_mode) {
        ESP_LOGI(TAG, "录音前关闭 WiFi (减少射频干扰)");
        net_wifi_disconnect();
    } else if (c->wifi_off_while_recording && c->bench_mode) {
        ESP_LOGI(TAG, "bench 模式: 录音期间保持 WiFi, 便于网页观测/停止");
    }

    rec_summary_t sum;
    esp_err_t err = recorder_run(trig, &sum);
    if (err == ESP_OK && sum.parts > 0) {
        ESP_LOGI(TAG, "本周期录音完成: %d 段 / %u 秒", sum.parts, (unsigned)sum.total_seconds);
        if (c->upload_on_record) {
            upload_report_t rep;
            ESP_LOGI(TAG, "开始自动上传...");
            uploader_run(&rep);
            ESP_LOGI(TAG, "上传结果: 成功 %d / 失败 %d / 待传 %d",
                     rep.uploaded, rep.failed, rep.pending_total);
        }
    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "VAD 判定为误触发, 未产生录音");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "录音失败: %s", esp_err_to_name(err));
    }

    // 上传后需要保持 WiFi 时恢复
    if (!c->bench_mode && c->wifi_off_while_recording && wifi_was_on) {
        // 非 bench: 交给周期结束逻辑决定
    }
    led_set(LED_IDLE);
}

static void do_upload(void)
{
    upload_report_t rep;
    esp_err_t err = uploader_run(&rep);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "上传完成: %d 成功 / %d 失败", rep.uploaded, rep.failed);
    } else {
        ESP_LOGW(TAG, "上传未完成: %s", rep.last_message);
        STATUS_STR(last_error, rep.last_message);
    }
    led_set(LED_IDLE);
}

static void worker_task(void *arg)
{
    trigger_t t;
    for (;;) {
        if (s_reboot_pending) {
            vTaskDelay(pdMS_TO_TICKS(300));
            ESP_LOGW(TAG, "网页请求重启");
            esp_restart();
        }
        if (xQueueReceive(s_q, &t, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }
        app_status_t st;
        app_status_snapshot(&st);
        if (s_busy || st.state == APP_RECORDING || st.state == APP_PRE_RECORD ||
            st.state == APP_UPLOADING) {
            ESP_LOGW(TAG, "设备忙 (%s), 忽略触发 %d", st.state_text, (int)t);
            continue;
        }
        s_busy = true;

        if (t == TRIG_UPLOAD) {
            do_upload();
        } else {
            rec_trigger_t rt = (t == TRIG_RECORD_AUTO) ? REC_TRIG_AUTO : REC_TRIG_BUTTON;
            do_record_cycle(rt);
        }
        s_busy = false;

        const app_config_t *c = app_config_get();
        // 注意: 必须等队列里的触发都处理完再睡。LM393 唤醒时 "开机补传 + 录音确认"
        // 会连续入队, 若这里直接睡会把后面的录音触发丢掉 (曾导致唤醒后不录音)。
        // 配置窗口内也不睡, 否则用户改到一半网页就没了。
        if (!c->bench_mode && c->sleep_enable && uxQueueMessagesWaiting(s_q) == 0 &&
            !cfg_window_active()) {
            enter_deep_sleep();
        }
    }
}

void app_main(void)
{
    logbuf_init();
    app_status_init();
    ESP_LOGI(TAG, "==== 录音豆 (recbean) 启动 ====");

    esp_reset_reason_t rr = esp_reset_reason();
    // 注意: esp_sleep_get_wakeup_cause() 在软件重启/上电后仍可能保留上次深睡的唤醒源,
    // 必须同时要求复位原因 = 深睡唤醒, 否则"网页重启"会被误判成 LM393 唤醒而触发录音。
    s_wake_from_ext1 = (rr == ESP_RST_DEEPSLEEP) &&
                       (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1);
    ESP_LOGI(TAG, "复位原因: %d, 深睡唤醒: %s", (int)rr, s_wake_from_ext1 ? "EXT1 (LM393)" : "否");

    app_config_init();
    const app_config_t *cfg = app_config_get();

    // 连续快速复位检测 (深睡后网页进不去时的救命入口)
    uint32_t streak = app_config_rst_streak_bump();
    bool force_cfg = (streak >= RST_STREAK_LIMIT);
    if (force_cfg) {
        app_config_rst_streak_clear();
        ESP_LOGW(TAG, "连续 %u 次快速复位 -> 强制进入配置模式", (unsigned)streak);
    } else {
        static esp_timer_handle_t streak_timer = NULL;
        const esp_timer_create_args_t targs = {
            .callback = rst_streak_clear_cb,
            .name = "rst_streak",
        };
        if (streak_timer == NULL && esp_timer_create(&targs, &streak_timer) == ESP_OK) {
            esp_timer_start_once(streak_timer, (int64_t)RST_STREAK_CLEAR_S * 1000000);
        }
    }

    time_sync_init();
    app_status_set_state(APP_IDLE);

    led_button_init(on_button_short, on_button_long);
    led_set(LED_IDLE);

    // LM393 DO 输入
    gpio_config_t snd = {
        .pin_bit_mask = 1ULL << PIN_SND_DETECT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&snd);
    ESP_LOGI(TAG, "LM393 DO = GPIO%d, 当前电平=%d", PIN_SND_DETECT, gpio_get_level(PIN_SND_DETECT));

    // SD
    esp_err_t sd_err = sd_store_mount(cfg->format_sd_if_mount_failed);
    if (sd_err != ESP_OK) {
        STATUS_STR(last_error, "SD 挂载失败");
        led_set(LED_ERROR);
        ESP_LOGE(TAG, "SD 不可用, 录音功能受限");
    }

    if (sd_store_mounted()) {
        sd_store_pending_refresh();
    }

    // WiFi
    net_wifi_init();
    if (cfg->wifi_ssid[0]) {
        esp_err_t werr = net_wifi_connect(20000);
        if (werr == ESP_OK) {
            if (!time_sync_trusted()) {
                ESP_LOGI(TAG, "首次对时 (NTP)...");
                time_sync_wait(10000);
            }
            if (cfg->bench_mode) {
                web_config_start(on_web_action, NULL);
                led_set(LED_CONFIG);
            } else if (!s_wake_from_ext1 && CFG_WINDOW_S > 0) {
                // 非 bench 且不是被声音唤醒 (即上电/复位/连按复位):
                // 先给一个配置窗口, 否则设成深睡后网页就再也进不来。
                // 连按 3 次复位 = 加长窗口, 方便慢慢配。
                cfg_window_open(force_cfg ? 300 : CFG_WINDOW_S);
            }
        } else {
            ESP_LOGW(TAG, "WiFi 连接失败, 仅本地录音");
            STATUS_STR(last_error, "WiFi 连接失败");
        }
    }

    s_q = xQueueCreate(8, sizeof(trigger_t));
    xTaskCreatePinnedToCore(worker_task, "worker", 28672, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(snd_monitor_task, "snd", 3072, NULL, 4, NULL, 0);

    // 启动后立即补传或处理唤醒触发
    if (cfg->upload_on_boot && sd_store_mounted() && net_wifi_connected()) {
        enqueue(TRIG_UPLOAD);
    }
    if (s_wake_from_ext1) {
        ESP_LOGI(TAG, "由 LM393 唤醒, 进入录音确认流程");
        enqueue(TRIG_RECORD_AUTO);
    }

    // 非 bench 且启用深睡: 唤醒触发处理完就回到睡眠 (功耗优先)
    // 但上电/复位后的配置窗口内要保持唤醒, 让用户有时间改配置。
    if (!cfg->bench_mode && cfg->sleep_enable && !s_wake_from_ext1 && !cfg_window_active()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        enter_deep_sleep();
    }

    // 主循环: 维护状态显示; 配置窗口结束后自动收摊进深睡
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!cfg->bench_mode && cfg->sleep_enable) {
            STATUS_SET(cfg_window_left, (uint16_t)cfg_window_left());  // 每秒刷新倒计时
            if (s_busy) continue;                 // 正在录音/上传, 等它做完
            if (cfg_window_active()) continue;    // 配置窗口内保持网页可用
            ESP_LOGI(TAG, "配置窗口已结束, 进入深睡");
            enter_deep_sleep();
        }
        if (!web_config_running() && net_wifi_connected()) {
            web_config_start(on_web_action, NULL);
        }
        app_status_t st;
        app_status_snapshot(&st);
        if (st.state == APP_IDLE && led_get() != LED_CONFIG) {
            led_set(LED_IDLE);
        }
    }
}
