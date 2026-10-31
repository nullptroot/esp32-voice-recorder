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
static volatile bool s_busy = false;
static volatile bool s_reboot_pending = false;
static volatile bool s_sleep_pending = false;      // 网页"立即深睡"
static volatile bool s_cfg_rearm_req = false;      // 配置键: 打开/延长配置窗口

static bool s_wake_from_ext1 = false;
static bool s_wake_from_sound = false;
static bool s_wake_from_button = false;

// 配置窗口: 深睡模式 (bench=false + sleep=true) 下 WiFi/httpd 都是关的, 网页进不去。
//   - 上电/复位 (非按键/声音唤醒): 开 CFG_WINDOW_BOOT_S 秒;
//   - 配置键 (GPIO14) 唤醒或短按: 开/延长 CFG_WINDOW_WAKE_S 秒。
// 窗口内即使 WiFi 没连上也保持唤醒并持续重连, 不会"第三次复位也直接睡"。
#define CFG_WINDOW_BOOT_S   120
#define CFG_WINDOW_WAKE_S   300
#define CFG_WINDOW_EXTEND_S 300

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

// 配置键 (GPIO14): 短按打开/延长配置窗口; 长按补传。
// 深睡时按下 GPIO14 会由 EXT1 直接唤醒并进入配置窗口 (见 app_main)。
static void on_cfg_button_short(void)
{
    const app_config_t *c = app_config_get();
    if (c->bench_mode) {
        ESP_LOGI(TAG, "配置键短按: bench 模式网页常驻, 忽略");
        return;
    }
    s_cfg_rearm_req = true;
    ESP_LOGW(TAG, "配置键短按: 打开/延长配置窗口");
}

static void on_cfg_button_long(void)
{
    ESP_LOGW(TAG, "配置键长按: 补传待上传文件");
    enqueue(TRIG_UPLOAD);
}

// 网页保存配置后调用: 把配置窗口再延长 5 分钟, 避免"改到一半设备睡了"。
void app_cfg_window_touch(void)
{
    // 由 httpd 任务调用, 只置请求标志, 窗口计时统一在主任务里改
    if (!app_config_get()->bench_mode) s_cfg_rearm_req = true;
}

// ---------------- LM393 电平监视 (调试/标定阈值用) ----------------
static void snd_monitor_task(void *arg)
{
    (void)arg;
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
    case WEB_ACT_SLEEP:  s_sleep_pending = true; break;
    }
}

// ---------------- 配置窗口 ----------------
// 只由主任务调用 (app_main / 主循环), 避免跨任务读写 64 位时间戳。
static void cfg_window_arm(int secs)
{
    s_cfg_window = true;
    s_cfg_window_end_us = esp_timer_get_time() + (int64_t)secs * 1000000;
    STATUS_SET(cfg_window_left, (uint16_t)secs);
    ESP_LOGW(TAG, "配置窗口: %d 秒内可打开 http://<ip>/ 改配置, 之后自动进深睡", secs);
}

static void cfg_window_open(int secs)
{
    cfg_window_arm(secs);
    web_config_start(on_web_action, NULL);
    led_set(LED_CONFIG);
}

static uint32_t cfg_window_left(void)
{
    if (!s_cfg_window) return 0;
    int64_t left = (s_cfg_window_end_us - esp_timer_get_time()) / 1000000;
    if (left <= 0) {
        s_cfg_window = false;
        STATUS_SET(cfg_window_left, 0);
        ESP_LOGI(TAG, "配置窗口结束");
        return 0;
    }
    return (uint32_t)left;
}

static bool cfg_window_active(void)
{
    return cfg_window_left() > 0;
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
    ESP_LOGI(TAG, "进入 EXT1 深度睡眠, 唤醒脚 GPIO%d (声音) / GPIO%d (配置键), 低电平触发",
             PIN_SND_DETECT, PIN_BUTTON_CFG);

    time_sync_save();     // 断电后仍能恢复近似墙钟时间

    web_config_stop();
    net_wifi_disconnect();
    sd_store_unmount();

    // 冻结 LED 任务后再动引脚, 避免 100ms 轮询把 GPIO48 又拉低
    led_set(LED_OFF);
    led_freeze();
    vTaskDelay(pdMS_TO_TICKS(150));
    gpio_set_level(PIN_LED, 0);

    // ---- 板载 LED (GPIO48, 低电平点亮) 深睡保持熄灭 ----
    // 深睡时数字域掉电: 输出寄存器会丢, 若睡眠配置开"输出", 会把引脚驱动到 0 ->
    // LED 反而常亮。正确做法是用睡眠上拉 (SLP_PU, 常电域), 不输出:
    //   SLP_SEL=1 + 睡眠方向=输入 + 睡眠上拉 -> 深睡期间引脚被拉到 3V3, LED 灭。
    // CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=y 已在启动时置位 SLP_SEL, 这里显式再设一遍。
    gpio_sleep_sel_en(PIN_LED_ONBOARD);
    gpio_sleep_set_direction(PIN_LED_ONBOARD, GPIO_MODE_INPUT);
    gpio_sleep_set_pull_mode(PIN_LED_ONBOARD, GPIO_PULLUP_ONLY);
    gpio_set_level(PIN_LED_ONBOARD, 1);
    esp_err_t e_onb = gpio_hold_en(PIN_LED_ONBOARD);
    if (e_onb != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d hold 失败: %s", PIN_LED_ONBOARD, esp_err_to_name(e_onb));
    }

    // LM393 声音脚 + 配置键: 输入 + 内部上拉, hold 住配置 (EXT1 唤醒用)
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << PIN_SND_DETECT) | (1ULL << PIN_BUTTON_CFG),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);
    gpio_hold_en(PIN_SND_DETECT);
    gpio_hold_en(PIN_BUTTON_CFG);
    gpio_deep_sleep_hold_en();

    esp_sleep_ext1_wakeup_mode_t mode = app_config_get()->sound_active_low
                                            ? ESP_EXT1_WAKEUP_ANY_LOW
                                            : ESP_EXT1_WAKEUP_ANY_HIGH;
    uint64_t wake_mask = (1ULL << PIN_SND_DETECT) | (1ULL << PIN_BUTTON_CFG);
    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(wake_mask, mode);
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
    (void)arg;
    trigger_t t;
    for (;;) {
        if (s_reboot_pending) {
            time_sync_save();
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
        // 睡眠统一交给主任务判断: 要同时看配置窗口/队列/忙状态, 且不能跨任务改窗口计时
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
    uint64_t ext1_mask = 0;
    if (s_wake_from_ext1) {
        ext1_mask = esp_sleep_get_ext1_wakeup_status();
        s_wake_from_sound = (ext1_mask & (1ULL << PIN_SND_DETECT)) != 0;
        s_wake_from_button = (ext1_mask & (1ULL << PIN_BUTTON_CFG)) != 0;
    }
    ESP_LOGI(TAG, "复位原因: %d, 深睡唤醒: %s (mask=0x%llx, 声音=%d, 配置键=%d)",
             (int)rr, s_wake_from_ext1 ? "EXT1" : "否",
             (unsigned long long)ext1_mask, s_wake_from_sound, s_wake_from_button);

    app_config_init();
    const app_config_t *cfg = app_config_get();

    // 断电/UART 复位后系统时间归零: 用 NVS 里上次的墙钟时间兜底 (近似)
    time_sync_init();
    app_status_set_state(APP_IDLE);

    led_button_init(on_button_short, on_button_long);
    // 配置键: 深睡时按下会 EXT1 唤醒; 唤醒后 2.5s 内忽略按压, 避免把手上的这次按压
    // 误判成长按/短按。
    led_button_init_extra(PIN_BUTTON_CFG, on_cfg_button_short, on_cfg_button_long,
                          s_wake_from_button ? 2500 : 0);
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
    ESP_LOGI(TAG, "按键: 录音/补传=GPIO%d, 配置/唤醒=GPIO%d",
             PIN_BUTTON, PIN_BUTTON_CFG);

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

    // WiFi / netif 先初始化, 这样即使 WiFi 还没连上 httpd 也能先起来。
    net_wifi_init();

    // 队列/任务尽早创建: 配置窗口刚打开时网页动作也能入队 (否则 s_q 还是 NULL)
    s_q = xQueueCreate(8, sizeof(trigger_t));
    xTaskCreatePinnedToCore(worker_task, "worker", 28672, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(snd_monitor_task, "snd", 3072, NULL, 4, NULL, 0);

    // 配置窗口: 先开窗口再连 WiFi —— 连不上 WiFi 也保持唤醒, 窗口内持续重试,
    // 这样"断网时想改配置"不会被直接送进深睡。
    bool want_window = false;
    if (cfg->bench_mode) {
        web_config_start(on_web_action, NULL);
        led_set(LED_CONFIG);
    } else if (s_wake_from_button) {
        cfg_window_open(CFG_WINDOW_WAKE_S);
        want_window = true;
    } else if (!s_wake_from_sound) {
        cfg_window_open(CFG_WINDOW_BOOT_S);
        want_window = true;
    }

    if (cfg->wifi_ssid[0]) {
        esp_err_t werr = net_wifi_connect(20000);
        if (werr == ESP_OK) {
            if (!time_sync_trusted()) {
                ESP_LOGI(TAG, "首次对时 (NTP)...");
                time_sync_wait(10000);
            }
            time_sync_save();
        } else {
            ESP_LOGW(TAG, "WiFi 连接失败, 仅本地录音%s", want_window ? " (配置窗口内会继续重试)" : "");
            STATUS_STR(last_error, "WiFi 连接失败");
        }
    }

    // 启动后补传 / 唤醒触发
    if (cfg->upload_on_boot && sd_store_mounted()) {
        enqueue(TRIG_UPLOAD);
    }
    if (s_wake_from_sound && !s_wake_from_button) {
        ESP_LOGI(TAG, "由 LM393 唤醒, 进入录音确认流程");
        enqueue(TRIG_RECORD_AUTO);
    }
    if (s_wake_from_button) {
        ESP_LOGI(TAG, "由配置键唤醒: 本次只开配置窗口, 不录音");
    }

    // 主循环: 维护状态显示 + 配置窗口 + WiFi/NTP 重试 + 深睡决策
    int idle_ticks = 0;
    int64_t last_time_save_us = esp_timer_get_time();
    int64_t last_ntp_try_us = 0;
    int64_t last_wifi_try_us = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const app_config_t *c = app_config_get();

        // 配置键请求 / 网页保存请求 (都只置标志, 这里统一改计时)
        if (s_cfg_rearm_req) {
            s_cfg_rearm_req = false;
            if (!c->bench_mode) {
                if (!s_cfg_window) {
                    cfg_window_open(CFG_WINDOW_WAKE_S);
                } else {
                    s_cfg_window_end_us = esp_timer_get_time() +
                                          (int64_t)CFG_WINDOW_EXTEND_S * 1000000;
                    STATUS_SET(cfg_window_left, (uint16_t)CFG_WINDOW_EXTEND_S);
                    ESP_LOGW(TAG, "配置窗口已延长至 %d 秒", CFG_WINDOW_EXTEND_S);
                }
                if (!web_config_running()) {
                    web_config_start(on_web_action, NULL);
                }
                led_set(LED_CONFIG);
            }
        }

        if (c->bench_mode) s_cfg_window = false;

        // 配置窗口内确保网页服务在跑 (httpd_start 失败/被意外停止时重试)
        if (!c->bench_mode && s_cfg_window && !web_config_running()) {
            web_config_start(on_web_action, NULL);
            led_set(LED_CONFIG);
        }

        uint32_t left = cfg_window_left();
        STATUS_SET(cfg_window_left, (uint16_t)left);
        if (left == 0 && led_get() == LED_CONFIG) {
            led_set(LED_IDLE);
        }

        // 配置窗口内 WiFi 掉线 -> 重连 (断网改配置场景)
        if (cfg_window_active() && !net_wifi_connected()) {
            int64_t now = esp_timer_get_time();
            if (now - last_wifi_try_us > 10 * 1000000LL) {
                last_wifi_try_us = now;
                ESP_LOGI(TAG, "配置窗口内 WiFi 未连接, 重试...");
                net_wifi_connect(10000);
            }
        }

        // 时间不可信 -> 有网就重试 NTP (比如断电后先用 NVS 近似时间, 联网后校正)
        if (net_wifi_connected() && !time_sync_trusted()) {
            int64_t now = esp_timer_get_time();
            if (now - last_ntp_try_us > 60 * 1000000LL) {
                last_ntp_try_us = now;
                time_sync_wait(5000);
            }
        }

        // 每 10 分钟把时间落盘一次, 供断电后恢复
        if (time_sync_valid() &&
            esp_timer_get_time() - last_time_save_us > 600 * 1000000LL) {
            last_time_save_us = esp_timer_get_time();
            time_sync_save();
        }

        if (c->bench_mode && !web_config_running()) {
            web_config_start(on_web_action, NULL);
            led_set(LED_CONFIG);
        }

        // 网页"立即深睡": 不管 bench 与否都执行 (测试/省电用)
        if (s_sleep_pending) {
            if (s_busy) continue;
            s_sleep_pending = false;
            ESP_LOGW(TAG, "网页请求立即进入深睡");
            enter_deep_sleep();
        }

        // 自动深睡: 非 bench + 允许睡眠 + 不忙 + 队列空 + 配置窗口已结束
        if (!c->bench_mode && c->sleep_enable) {
            if (!s_busy && uxQueueMessagesWaiting(s_q) == 0 && !s_cfg_window) {
                idle_ticks++;
            } else {
                idle_ticks = 0;
            }
            // 连续 2 秒空闲才睡, 避免和 worker 刚取走队列的瞬间抢跑
            if (idle_ticks >= 2) {
                ESP_LOGI(TAG, "空闲, 进入深睡");
                enter_deep_sleep();
            }
        } else {
            idle_ticks = 0;
        }

        app_status_t st;
        app_status_snapshot(&st);
        if (st.state == APP_IDLE && led_get() != LED_CONFIG) {
            led_set(LED_IDLE);
        }
    }
}
