// wav_writer.c
#include "wav_writer.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "sd_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wav";

#define WAV_HDR_SIZE 44

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}

esp_err_t wav_open(wav_writer_t *w, const char *path, uint32_t sample_rate)
{
    memset(w, 0, sizeof(*w));
    strncpy(w->path, path, sizeof(w->path) - 1);
    w->sample_rate = sample_rate;
    w->start_uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);

    sd_store_lock();
    // SD 模块偶发 CRC 错误时, 重试打开往往能成功
    FILE *fp = NULL;
    for (int attempt = 0; attempt < 4 && !fp; attempt++) {
        fp = fopen(path, "wb");
        if (!fp) {
            ESP_LOGW(TAG, "打开失败 (第 %d 次): %s", attempt + 1, path);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
    if (!fp) {
        // 卡可能因 CRC 错误锁死: 尝试重新初始化后再打开一次
        ESP_LOGW(TAG, "打开失败, 尝试恢复 SD 卡: %s", path);
        sd_store_unlock();
        if (sd_store_recover() == ESP_OK) {
            sd_store_lock();
            fp = fopen(path, "wb");
            sd_store_unlock();
        }
        if (!fp) {
            ESP_LOGE(TAG, "打开失败: %s", path);
            return ESP_FAIL;
        } else {
            w->fp = fp;
            goto write_header;
        }
    }
    w->fp = fp;

write_header:
    ;
    uint8_t hdr[WAV_HDR_SIZE];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, "RIFF", 4);
    put_u32(hdr + 4, 0);                  // 回填
    memcpy(hdr + 8, "WAVEfmt ", 8);
    put_u32(hdr + 16, 16);                // fmt chunk size
    put_u16(hdr + 20, 1);                 // PCM
    put_u16(hdr + 22, 1);                 // mono
    put_u32(hdr + 24, sample_rate);
    put_u32(hdr + 28, sample_rate * 2);   // byte rate
    put_u16(hdr + 32, 2);                 // block align
    put_u16(hdr + 34, 16);                // bits per sample
    memcpy(hdr + 36, "data", 4);
    put_u32(hdr + 40, 0);                 // 回填

    if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        ESP_LOGE(TAG, "写头失败: %s", path);
        fclose(fp);
        sd_store_unlock();
        w->fp = NULL;
        return ESP_FAIL;
    }
    sd_store_unlock();
    w->open = true;
    ESP_LOGI(TAG, "开始写: %s (%u Hz)", path, (unsigned)sample_rate);
    return ESP_OK;
}

esp_err_t wav_write(wav_writer_t *w, const int16_t *samples, size_t count)
{
    if (!w->open || !w->fp) return ESP_ERR_INVALID_STATE;
    sd_store_lock();
    size_t bytes = count * sizeof(int16_t);
    size_t written = 0;
    int tries = 0;
    for (; tries < 3; tries++) {
        written = fwrite(samples, 1, bytes, w->fp);
        if (written == bytes) {
            break;
        }
        ESP_LOGW(TAG, "写入不完整 (%u/%u), 第 %d 次重试: %s",
                 (unsigned)written, (unsigned)bytes, tries + 1, w->path);
        vTaskDelay(pdMS_TO_TICKS(30));
        // 清掉错误标志, 允许 FatFS 重新同步
        clearerr(w->fp);
        fseek(w->fp, 0, SEEK_END);
    }
    if (written != bytes) {
        sd_store_unlock();
        ESP_LOGE(TAG, "写数据失败: %s (want %u, got %u)", w->path,
                 (unsigned)bytes, (unsigned)written);
        return ESP_FAIL;
    }
    w->data_bytes += (uint32_t)bytes;
    sd_store_unlock();
    return ESP_OK;
}

// 用独立句柄回填 WAV 头 (SD 上对已打开流做 fseek+fwrite 偶发失败)
static esp_err_t patch_header(const char *path, uint32_t data_bytes)
{
    sd_store_lock();
    FILE *f = fopen(path, "r+b");
    if (!f) {
        sd_store_unlock();
        ESP_LOGE(TAG, "回填: 无法重新打开 %s", path);
        return ESP_FAIL;
    }
    uint8_t buf[4];
    put_u32(buf, WAV_HDR_SIZE - 8 + data_bytes);
    bool ok = (fseek(f, 4, SEEK_SET) == 0 && fwrite(buf, 1, 4, f) == 4);
    put_u32(buf, data_bytes);
    ok = ok && (fseek(f, 40, SEEK_SET) == 0 && fwrite(buf, 1, 4, f) == 4);
    if (fflush(f) != 0) ok = false;
    fclose(f);

    // 校验
    if (ok) {
        FILE *r = fopen(path, "rb");
        if (r) {
            uint8_t chk[8] = {0};
            if (fseek(r, 4, SEEK_SET) == 0) {
                size_t n = fread(chk, 1, 8, r);
                uint32_t riff = chk[0] | (chk[1] << 8) | (chk[2] << 16) | ((uint32_t)chk[3] << 24);
                uint32_t data = chk[4] | (chk[5] << 8) | (chk[6] << 16) | ((uint32_t)chk[7] << 24);
                if (n != 8 || data != data_bytes || riff == 0) {
                    ok = false;
                }
            } else {
                ok = false;
            }
            fclose(r);
        }
    }
    sd_store_unlock();
    if (!ok) {
        ESP_LOGE(TAG, "回填 WAV 头失败: %s (data=%u)", path, (unsigned)data_bytes);
    }
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t wav_close(wav_writer_t *w)
{
    if (!w->open || !w->fp) return ESP_ERR_INVALID_STATE;
    FILE *fp = (FILE *)w->fp;
    sd_store_lock();
    if (fflush(fp) != 0) {
        ESP_LOGW(TAG, "fflush 失败: %s", w->path);
    }
    int rc = fclose(fp);
    sd_store_unlock();
    w->fp = NULL;
    w->open = false;
    if (rc != 0) {
        ESP_LOGE(TAG, "关闭文件失败: %s", w->path);
        return ESP_FAIL;
    }

    esp_err_t perr = patch_header(w->path, w->data_bytes);
    if (perr != ESP_OK) {
        // 再试一次 (劣质卡偶发写失败)
        vTaskDelay(pdMS_TO_TICKS(120));
        perr = patch_header(w->path, w->data_bytes);
    }
    ESP_LOGI(TAG, "完成: %s (%.1f 秒, %u 字节)%s", w->path,
             w->sample_rate ? (double)w->data_bytes / (2.0 * w->sample_rate) : 0.0,
             (unsigned)w->data_bytes, perr == ESP_OK ? "" : " [头部回填失败]");
    return perr;
}

uint32_t wav_seconds(const wav_writer_t *w)
{
    if (w->sample_rate == 0) return 0;
    return w->data_bytes / (2 * w->sample_rate);
}
