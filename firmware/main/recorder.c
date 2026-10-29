// recorder.c —— 录音循环实现
#include "recorder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "app_config.h"
#include "app_status.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_button.h"
#include "mic_i2s.h"
#include "pins.h"
#include "sd_store.h"
#include "time_sync.h"
#include "vad.h"
#include "wav_writer.h"

static const char *TAG = "rec";

#define READ_CHUNK            1024          // 每次从 I2S 读 1024 样本 (64ms @16k)
#define FRAME_SAMPLES         320           // VAD 帧长 20ms @16k
#define SPEECH_CONFIRM_FRAMES 3             // 连续 3 帧(60ms) 人声才确认
#define MIN_FREE_BYTES        (50ULL * 1024 * 1024)
#define LOW_FREE_BYTES        (10ULL * 1024 * 1024)

static volatile bool s_stop_req = false;
static int16_t s_read_buf[READ_CHUNK];

void recorder_request_stop(void)
{
    s_stop_req = true;
}

bool recorder_stop_requested(void)
{
    return s_stop_req;
}

typedef struct {
    wav_writer_t wav;
    bool   open;
    int    part_index;
    uint32_t samples;
} part_t;

static esp_err_t open_part(part_t *p, int part_index, const char *session_str,
                           uint32_t rate, const char **out_name)
{
    static char s_fname[SD_NAME_LEN];
    if (time_sync_trusted()) {
        char ts[32];
        time_sync_format_fname(ts, sizeof(ts), time(NULL));
        snprintf(s_fname, sizeof(s_fname), "mic1_%s_part%d.wav", ts, part_index);
    } else {
        snprintf(s_fname, sizeof(s_fname), "mic1_%s_part%d.wav", session_str, part_index);
    }
    char path[SD_NAME_LEN + 32];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, s_fname);

    esp_err_t err = wav_open(&p->wav, path, rate);
    if (err != ESP_OK) return err;
    p->open = true;
    p->part_index = part_index;
    p->samples = 0;
    if (out_name) *out_name = s_fname;
    return ESP_OK;
}

// 校验落盘大小: SD 偶发坏块会让目录项 size 变成 0, 必须识别并丢弃
static bool part_size_ok(const char *wav_name, uint32_t data_bytes)
{
    if (!wav_name) return false;
    char path[SD_NAME_LEN + 32];
    snprintf(path, sizeof(path), "%s/%.90s", SD_MOUNT_POINT, wav_name);
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "校验失败, 文件不存在: %s", wav_name);
        return false;
    }
    if ((uint32_t)st.st_size < data_bytes + 44) {
        ESP_LOGE(TAG, "文件大小异常 (期望 >= %u, 实际 %u): %s",
                 (unsigned)(data_bytes + 44), (unsigned)st.st_size, wav_name);
        return false;
    }
    return true;
}

static void drop_empty_part(const char *wav_name, uint32_t bytes)
{
    if (bytes > 0 || !wav_name) return;
    ESP_LOGW(TAG, "分段无数据, 删除空文件: %s", wav_name);
    sd_store_delete(wav_name);
}

static void write_meta(const char *wav_name, int part_index, uint32_t seconds,
                       bool trusted, const char *session_str, time_t rec_time,
                       uint32_t rate)
{
    char meta_name[SD_NAME_LEN + 8];
    snprintf(meta_name, sizeof(meta_name), "%s.json", wav_name);
    char iso[48] = "null";
    if (trusted) {
        char t[40];
        time_sync_format_iso8601(t, sizeof(t), rec_time);
        snprintf(iso, sizeof(iso), "\"%s\"", t);
    }
    char json[512];
    snprintf(json, sizeof(json),
             "{\"device_id\":\"%s\",\"recorded_at\":%s,\"time_trusted\":%s,"
             "\"session_id\":\"%s\",\"sequence\":%d,\"monotonic_ms\":%lld,"
             "\"duration_sec\":%u,\"sample_rate\":%u,\"channels\":1,\"bits\":16}",
             app_config_get()->device_id, iso, trusted ? "true" : "false",
             session_str, part_index,
             (long long)(esp_timer_get_time() / 1000),
             (unsigned)seconds, (unsigned)rate);
    sd_store_write_text(meta_name, json);
}

static void set_status_state(app_state_t st)
{
    app_status_set_state(st);
}

esp_err_t recorder_run(rec_trigger_t trig, rec_summary_t *out)
{
    const app_config_t *cfg = app_config_get();
    if (!sd_store_mounted()) {
        ESP_LOGE(TAG, "SD 未挂载, 无法录音");
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total = 0, freeb = 0;
    sd_store_info(&total, &freeb);
    if (freeb < MIN_FREE_BYTES) {
        ESP_LOGE(TAG, "SD 可用空间不足: %llu MB", (unsigned long long)(freeb / 1048576));
        STATUS_STR(last_error, "SD 空间不足");
        return ESP_ERR_NO_MEM;
    }

    memset(out, 0, sizeof(*out));
    s_stop_req = false;

    uint32_t rate = cfg->sample_rate;
    uint16_t session_id = (uint16_t)(app_config_session_id() + 1);
    app_config_session_bump();
    char session_str[24];
    snprintf(session_str, sizeof(session_str), "session-%04u", (unsigned)session_id);
    out->session_id = session_id;

    esp_err_t err = mic_init(rate, cfg->gain_shift);
    if (err != ESP_OK) {
        STATUS_STR(last_error, "I2S 初始化失败");
        return err;
    }
    err = mic_start();
    if (err != ESP_OK) {
        STATUS_STR(last_error, "I2S 启动失败");
        mic_deinit();
        return err;
    }

    bool need_confirm = (trig == REC_TRIG_EXT1 || trig == REC_TRIG_AUTO);
    size_t pre_target = (size_t)((uint64_t)rate * cfg->pre_record_ms / 1000);
    if (pre_target > (size_t)rate * 10) {
        pre_target = (size_t)rate * 10;
    }
    int16_t *prebuf = heap_caps_malloc(pre_target * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!prebuf) {
        prebuf = malloc(pre_target * sizeof(int16_t));
    }
    if (!prebuf) {
        ESP_LOGE(TAG, "预录缓冲分配失败");
        mic_stop();
        mic_deinit();
        return ESP_ERR_NO_MEM;
    }
    size_t pre_count = 0;
    size_t pre_start = 0;   // 环形缓冲逻辑起点

    vad_t vad;
    vad_init(&vad, cfg->vad_threshold_db);

    STATUS_SET(record_seconds, 0);
    STATUS_SET(level_db, -100);

    int consec_speech = 0;
    bool confirmed = !need_confirm;

    int64_t phase_start = esp_timer_get_time();
    int64_t confirm_deadline = phase_start + (int64_t)cfg->pre_record_ms * 1000 + 3000000;

    part_t part = {0};
    const char *cur_name = NULL;
    int part_index = 0;
    time_t part_time = time(NULL);
    bool trusted = time_sync_trusted();

    // ---------- 阶段 1: 预录 + VAD 二次确认 ----------
    if (need_confirm) {
        set_status_state(APP_PRE_RECORD);
        led_set(LED_IDLE);
        ESP_LOGI(TAG, "预录/人声确认中 (窗口 %.1fs, 阈值 %ddB, 触发源=%d)",
                 cfg->pre_record_ms / 1000.0, cfg->vad_threshold_db, (int)trig);
    } else {
        set_status_state(APP_RECORDING);
        led_set(LED_RECORD);
        ESP_LOGI(TAG, "手动触发录音 (触发源=%d)", (int)trig);
    }

    while (!confirmed) {
        if (s_stop_req) {
            out->aborted = true;
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        size_t got = 0;
        mic_read(s_read_buf, READ_CHUNK, &got, 2000);
        if (got == 0) {
            if (esp_timer_get_time() > confirm_deadline) {
                ESP_LOGW(TAG, "预录阶段无数据 (I2S 超时), 放弃");
                break;
            }
            continue;
        }
        for (size_t i = 0; i < got; i++) {
            size_t idx = pre_count < pre_target ? pre_count : pre_start;
            prebuf[idx] = s_read_buf[i];
            if (pre_count < pre_target) {
                pre_count++;
            } else {
                pre_start = (pre_start + 1) % pre_target;
            }
        }
        for (size_t off = 0; off + FRAME_SAMPLES <= got; off += FRAME_SAMPLES) {
            int sp = vad_frame(&vad, s_read_buf + off, FRAME_SAMPLES);
            STATUS_SET(level_db, (int16_t)vad.last_db);
            STATUS_SET(vad_speech, sp != 0);
            consec_speech = sp ? consec_speech + 1 : 0;
            // 需要预录缓冲已填满, 保证文件头部包含人声起始前的音频
            if (consec_speech >= SPEECH_CONFIRM_FRAMES && pre_count >= pre_target) {
                confirmed = true;
                out->voice_confirmed = true;
                ESP_LOGI(TAG, "VAD 确认人声 (能量 %.1f dB, 噪声底 %.1f dB)",
                         vad.last_db, vad.noise_db);
                break;
            }
        }
        if (esp_timer_get_time() > confirm_deadline && !confirmed) {
            ESP_LOGW(TAG, "VAD 未确认人声, 判定误触发");
            break;
        }
    }

    if (!confirmed) {
        uint32_t cd = cfg->cooldown_seconds;
        ESP_LOGW(TAG, "误触发冷却 %us", (unsigned)cd);
        set_status_state(APP_COOLDOWN);
        led_set(LED_OFF);
        int64_t until = esp_timer_get_time() + (int64_t)cd * 1000000;
        while (esp_timer_get_time() < until && !s_stop_req) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    // ---------- 阶段 2: 正式录音 ----------
    set_status_state(APP_RECORDING);
    led_set(LED_RECORD);
    part_index = 1;
    if (open_part(&part, part_index, session_str, rate, &cur_name) != ESP_OK) {
        STATUS_STR(last_error, "创建 WAV 失败");
        err = ESP_FAIL;
        goto cleanup;
    }
    if (pre_count > 0) {
        if (pre_count < pre_target) {
            wav_write(&part.wav, prebuf, pre_count);
            part.samples += pre_count;
        } else {
            size_t first = pre_target - pre_start;
            wav_write(&part.wav, prebuf + pre_start, first);
            if (pre_start > 0) {
                wav_write(&part.wav, prebuf, pre_start);
            }
            part.samples += pre_target;
        }
    }

    // 只有检测到过人声后才启用静音停录, 否则手动触发会在开口前就停掉
    bool speech_seen = false;
    int consec_sp = 0;   // 连续人声帧计数 (用来滤掉瞬态噪声)
    int64_t last_speech_us = 0;
    int64_t rec_start_us = esp_timer_get_time();
    bool write_failed = false;
    uint32_t space_check_counter = 0;

    while (!s_stop_req) {
        size_t got = 0;
        esp_err_t rerr = mic_read(s_read_buf, READ_CHUNK, &got, 2000);
        if (rerr != ESP_OK || got == 0) {
            ESP_LOGE(TAG, "I2S 读超时/失败: %s", esp_err_to_name(rerr));
            STATUS_STR(last_error, "I2S 读取超时");
            break;
        }

        if (part.open && wav_write(&part.wav, s_read_buf, got) != ESP_OK) {
            ESP_LOGE(TAG, "SD 写入失败, 停止录音");
            STATUS_STR(last_error, "SD 写入失败");
            write_failed = true;
            break;
        }
        part.samples += got;

        for (size_t off = 0; off + FRAME_SAMPLES <= got; off += FRAME_SAMPLES) {
            int sp = vad_frame(&vad, s_read_buf + off, FRAME_SAMPLES);
            STATUS_SET(level_db, (int16_t)vad.last_db);
            STATUS_SET(vad_speech, sp != 0);
            // 只有连续 3 帧 (60ms) 判定为人声才刷新"最近说话时刻":
            // 键盘/鼠标/开关门等瞬态噪声单帧能量也会超过阈值, 若单帧就刷新,
            // 静音停录永远等不到 5s (实测房间噪声每 0.5s 就有一个尖峰)。
            consec_sp = sp ? consec_sp + 1 : 0;
            if (consec_sp >= SPEECH_CONFIRM_FRAMES) {
                speech_seen = true;
                last_speech_us = esp_timer_get_time();
            }
        }

        uint32_t part_sec = part.samples / rate;
        STATUS_SET(record_seconds, (uint16_t)part_sec);

        if (part_sec >= cfg->segment_seconds) {
            uint32_t secs = part_sec;
            uint32_t bytes = part.wav.data_bytes;
            char closed_name[SD_NAME_LEN];
            strncpy(closed_name, cur_name ? cur_name : "", sizeof(closed_name) - 1);
            closed_name[sizeof(closed_name) - 1] = '\0';
            wav_close(&part.wav);
            part.open = false;
            if (bytes > 0 && part_size_ok(closed_name, bytes)) {
                write_meta(closed_name, part_index, secs, trusted, session_str, part_time, rate);
                out->parts++;
                out->total_seconds += secs;
                out->written_bytes += bytes;
                strncpy(out->last_file, closed_name, sizeof(out->last_file) - 1);
            } else if (bytes > 0) {
                ESP_LOGE(TAG, "分段损坏, 删除: %s", closed_name);
                sd_store_delete(closed_name);
                STATUS_STR(last_error, "SD 写坏, 已丢弃损坏分段");
            }
            // 已接近最长录音时长时不再开新分段, 避免产生 0 字节文件
            if ((esp_timer_get_time() - rec_start_us) / 1000000 +
                    (int64_t)cfg->segment_seconds > (int64_t)cfg->max_record_seconds) {
                ESP_LOGI(TAG, "剩余时间不足以再录满一段, 结束");
                break;
            }
            part_index++;
            part_time = time(NULL);
            trusted = time_sync_trusted();
            if (open_part(&part, part_index, session_str, rate, &cur_name) != ESP_OK) {
                STATUS_STR(last_error, "分段创建失败");
                write_failed = true;
                break;
            }
            ESP_LOGI(TAG, "分段切换: 第 %d 段", part_index);
        }

        if (cfg->silence_stop_ms > 0 && speech_seen) {
            int64_t silent_ms = (esp_timer_get_time() - last_speech_us) / 1000;
            if (silent_ms >= cfg->silence_stop_ms) {
                ESP_LOGI(TAG, "静音 %lld ms, 停止录音", (long long)silent_ms);
                break;
            }
        }
        if ((esp_timer_get_time() - rec_start_us) / 1000000 >= cfg->max_record_seconds) {
            ESP_LOGI(TAG, "达到最长录音时长 %us, 停止", cfg->max_record_seconds);
            break;
        }
        if (++space_check_counter >= 160) {   // 每 ~10s
            space_check_counter = 0;
            uint64_t t2 = 0, f2 = 0;
            sd_store_info_cached(&t2, &f2);
            if (f2 < LOW_FREE_BYTES) {
                ESP_LOGW(TAG, "SD 空间偏低 (%llu MB), 停止录音",
                         (unsigned long long)(f2 / 1048576));
                STATUS_STR(last_error, "SD 空间偏低");
                break;
            }
        }
    }

    if (part.open) {
        uint32_t secs = part.samples / rate;
        uint32_t bytes = part.wav.data_bytes;
        char closed_name[SD_NAME_LEN];
        strncpy(closed_name, cur_name ? cur_name : "", sizeof(closed_name) - 1);
        closed_name[sizeof(closed_name) - 1] = '\0';
        wav_close(&part.wav);
        part.open = false;
        if (bytes == 0) {
            drop_empty_part(closed_name, bytes);
        } else if (!part_size_ok(closed_name, bytes)) {
            ESP_LOGE(TAG, "分段损坏, 删除: %s", closed_name);
            sd_store_delete(closed_name);
            STATUS_STR(last_error, "SD 写坏, 已丢弃损坏分段");
            out->aborted = true;
            err = ESP_FAIL;
        } else {
            write_meta(closed_name, part_index, secs, trusted, session_str, part_time, rate);
            out->parts++;
            out->total_seconds += secs;
            out->written_bytes += bytes;
            strncpy(out->last_file, closed_name, sizeof(out->last_file) - 1);
        }
    }
    if (write_failed) {
        out->aborted = true;
        err = ESP_FAIL;
    } else {
        err = ESP_OK;
    }
    ESP_LOGI(TAG, "录音结束: %d 段 / %u 秒 / %u 字节",
             out->parts, (unsigned)out->total_seconds, (unsigned)out->written_bytes);

cleanup:
    led_set(LED_OFF);
    mic_stop();
    mic_deinit();
    free(prebuf);
    set_status_state(APP_IDLE);
    STATUS_SET(vad_speech, false);
    if (out->parts > 0) {
        app_status_t *s = app_status_lock();
        s->recorded_files += out->parts;
        app_status_unlock();
    }
    return err;
}
