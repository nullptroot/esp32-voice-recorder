// app_config.h —— 运行期配置 (NVS 持久化, 可通过设备网页热改)
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_MAX_SSID    32
#define DEVICE_MAX_PASS    64
#define DEVICE_MAX_URL     160
#define DEVICE_MAX_DEVID   32

#define CFG_VERSION        5

typedef struct {
    uint16_t version;

    // 网络
    char     wifi_ssid[DEVICE_MAX_SSID + 1];
    char     wifi_pass[DEVICE_MAX_PASS + 1];
    char     server_url[DEVICE_MAX_URL + 1];   // 例: http://192.168.5.36:8000
    char     device_id[DEVICE_MAX_DEVID + 1];  // 例: esp32s3-001

    // 录音
    uint16_t sample_rate;        // 默认 16000
    uint8_t  gain_shift;         // 额外左移位数 (0..4), 0 表示不加增益
    uint16_t segment_seconds;    // 分段时长, 默认 900 (15 分钟)
    uint16_t max_record_seconds; // 单次录音最长, 默认 1800
    uint16_t pre_record_ms;      // 预录/VAD 确认窗口, 默认 1500
    uint16_t silence_stop_ms;    // 静音多久停录, 默认 5000
    uint16_t cooldown_seconds;   // 误触发冷却, 默认 20

    // VAD
    int16_t  vad_threshold_db;   // 高于噪声底多少 dB 判定为人声, 默认 8
    bool     vad_enable;

    // 上传
    bool     upload_on_record;   // 录完立即上传
    bool     upload_on_boot;     // 开机扫描补传
    bool     delete_after_upload;
    uint16_t keep_days;          // 0 = 不按天数清理
    uint8_t  upload_max_retry;   // 单文件最大尝试次数, 默认 5
    uint16_t upload_backoff_ms;  // 首次退避, 默认 2000

    // 电源 / 模式
    bool     sleep_enable;       // 启用 EXT1 深睡 (bench 模式下任然会被抑制)
    bool     bench_mode;         // 常量醒 + 网页可访问, 关闭自动深睡
    bool     wifi_off_while_recording;

    // 存储
    bool     format_sd_if_mount_failed;

    // SD 卡 SPI 时钟 (kHz), 杜邦线长/模块差时降低到 10000 或 4000
    uint16_t sd_freq_khz;

    // LM393 极性: true=声音时输出低电平 (常见模块), false=声音时输出高电平
    bool     sound_active_low;
} app_config_t;

esp_err_t app_config_init(void);
const app_config_t *app_config_get(void);
esp_err_t app_config_save(const app_config_t *cfg);
esp_err_t app_config_set(app_config_t *cfg);     // 校验 + 保存 + 生效
void app_config_defaults(app_config_t *cfg);

cJSON *app_config_to_json(const app_config_t *cfg);
esp_err_t app_config_from_json(const cJSON *root, app_config_t *cfg,
                               char *err, size_t errlen);

// 运行时只读派生信息
uint32_t app_config_session_id(void);
void     app_config_session_bump(void);
uint32_t app_config_boot_session(void);
void     app_config_set_boot_session(uint32_t v);

// 最近一次可用的墙钟时间 (epoch 秒, NVS 持久化)。
// 用途: 断电/UART 复位后 RTC 时间归零, 用上次已知时间兜底, 避免录音文件名回退成
// session-XXXX。该时间不包含断电时长, 属于"近似时间", 联网后由 NTP 校正。
uint32_t app_config_last_epoch(void);
void     app_config_set_last_epoch(uint32_t epoch);

#ifdef __cplusplus
}
#endif
