// sd_store.c
#include "sd_store.h"
#include "app_config.h"
#include "pins.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;
static bool s_bus_inited = false;

// FatFS 操作串行化: 录音写入与网页/上传读取并发时会破坏 SPI 事务,
// 因此所有对外接口都在同一把递归锁下执行。
static SemaphoreHandle_t s_mtx = NULL;
static uint64_t s_total_cache = 0;
static uint64_t s_free_cache = 0;
static int s_pending_cache = 0;

static void sd_lock(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateRecursiveMutex();
    }
    if (s_mtx) xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
}

static void sd_unlock(void)
{
    if (s_mtx) xSemaphoreGiveRecursive(s_mtx);
}

void sd_store_lock(void) { sd_lock(); }
void sd_store_unlock(void) { sd_unlock(); }

static esp_err_t bus_init(void)
{
    if (s_bus_inited) return ESP_OK;
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8192,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize 失败: %s", esp_err_to_name(err));
        return err;
    }
    s_bus_inited = true;
    // 廉价 SD 模块 + 杜邦线: MISO/CS 上拉可提高读稳定性
    gpio_set_pull_mode(PIN_SD_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_CS, GPIO_PULLUP_ONLY);
    return ESP_OK;
}

static esp_err_t mount_inner(bool format_if_mount_failed)
{
    if (s_mounted) return ESP_OK;
    esp_err_t bus_err = bus_init();
    if (bus_err != ESP_OK) return bus_err;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    uint32_t f = app_config_get()->sd_freq_khz;
    if (f < 1000) f = 1000;
    host.max_freq_khz = (int)f;
    ESP_LOGI(TAG, "SD SPI 时钟: %u kHz", (unsigned)f);

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files = 8,
        // 64KB 簇: 把 FAT/根目录压到前 ~5MB, 绕开劣质卡在中段可能存在的坏区
        .allocation_unit_size = 64 * 1024,
    };
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SPI2_HOST;

    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mnt, &s_card);
    if (err == ESP_OK) {
        s_mounted = true;
        sdmmc_card_print_info(stdout, s_card);
        ESP_LOGI(TAG, "SD 挂载成功: %s", SD_MOUNT_POINT);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "SD 挂载失败: %s", esp_err_to_name(err));
    if (!format_if_mount_failed) {
        return err;
    }

    ESP_LOGW(TAG, "尝试格式化为 FAT32 后挂载 (会清空卡内数据)...");
    mnt.format_if_mount_failed = true;
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mnt, &s_card);
    if (err == ESP_OK) {
        s_mounted = true;
        ESP_LOGI(TAG, "SD 格式化并挂载成功: %s", SD_MOUNT_POINT);
        uint64_t total = 0, freeb = 0;
        if (sd_store_info(&total, &freeb) == ESP_OK) {
            ESP_LOGI(TAG, "容量: 总 %llu MB, 可用 %llu MB",
                     (unsigned long long)(total / (1024 * 1024)),
                     (unsigned long long)(freeb / (1024 * 1024)));
        }
    } else {
        ESP_LOGE(TAG, "格式化后挂载仍失败: %s", esp_err_to_name(err));
        s_card = NULL;
    }
    return err;
}

esp_err_t sd_store_mount(bool format_if_mount_failed)
{
    sd_lock();
    esp_err_t err = mount_inner(format_if_mount_failed);
    // 挂载后填充容量缓存, 之后网页只读缓存, 不再实时 f_getfree
    if (err == ESP_OK) {
        uint64_t total = 0, freeb = 0;
        if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
            s_total_cache = total;
            s_free_cache = freeb;
        }
    }
    sd_unlock();
    return err;
}

esp_err_t sd_store_format(void)
{
    sd_lock();
    if (!s_card) {
        sd_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "用户触发格式化 SD 卡...");
    esp_err_t err = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "格式化失败: %s", esp_err_to_name(err));
    } else {
        uint64_t total = 0, freeb = 0;
        if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
            s_total_cache = total;
            s_free_cache = freeb;
        }
    }
    sd_unlock();
    return err;
}

void sd_store_unmount(void)
{
    sd_lock();
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_mounted = false;
        ESP_LOGI(TAG, "SD 已卸载");
    }
    sd_unlock();
}

esp_err_t sd_store_recover(void)
{
    sd_lock();
    if (s_mounted) {
        ESP_LOGW(TAG, "SD 恢复: 卸载文件系统...");
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_mounted = false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGW(TAG, "SD 恢复: 重新初始化并挂载...");
    esp_err_t err = mount_inner(false);
    if (err == ESP_OK) {
        uint64_t total = 0, freeb = 0;
        if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
            s_total_cache = total;
            s_free_cache = freeb;
        }
        ESP_LOGW(TAG, "SD 恢复成功");
    } else {
        ESP_LOGE(TAG, "SD 恢复失败: %s", esp_err_to_name(err));
    }
    sd_unlock();
    return err;
}

bool sd_store_mounted(void)
{
    return s_mounted;
}

esp_err_t sd_store_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    sd_lock();
    uint64_t total = 0, freeb = 0;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_mounted) {
        err = esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb);
        if (err == ESP_OK) {
            s_total_cache = total;
            s_free_cache = freeb;
        }
    }
    if (total_bytes) *total_bytes = (err == ESP_OK) ? total : s_total_cache;
    if (free_bytes) *free_bytes = (err == ESP_OK) ? freeb : s_free_cache;
    sd_unlock();
    return err;
}

void sd_store_info_cached(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (total_bytes) *total_bytes = s_total_cache;
    if (free_bytes) *free_bytes = s_free_cache;
}

static bool has_upload_marker(const char *name)
{
    char path[SD_NAME_LEN + 32];
    snprintf(path, sizeof(path), "%s/%.80s.up", SD_MOUNT_POINT, name);
    struct stat st;
    return stat(path, &st) == 0;
}

int sd_store_list(sd_file_t *out, int max, bool only_pending)
{
    sd_lock();
    int n = 0;
    if (!s_mounted || max <= 0) {
        sd_unlock();
        return 0;
    }
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "opendir 失败: %s", strerror(errno));
        sd_unlock();
        return 0;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n < max) {
        size_t len = strlen(de->d_name);
        if (len < 5) continue;
        if (strcasecmp(de->d_name + len - 4, ".wav") != 0) continue;
        bool up = has_upload_marker(de->d_name);
        if (only_pending && up) continue;

        char path[SD_NAME_LEN + 32];
        snprintf(path, sizeof(path), "%s/%.90s", SD_MOUNT_POINT, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        strncpy(out[n].name, de->d_name, SD_NAME_LEN - 1);
        out[n].name[SD_NAME_LEN - 1] = '\0';
        out[n].size = (uint32_t)st.st_size;
        out[n].uploaded = up;
        out[n].mtime = (uint32_t)st.st_mtime;
        n++;
    }
    closedir(dir);
    sd_unlock();
    return n;
}

int sd_store_count_pending(void)
{
    sd_lock();
    int total = 0;
    if (s_mounted) {
        DIR *dir = opendir(SD_MOUNT_POINT);
        if (dir) {
            struct dirent *de;
            while ((de = readdir(dir)) != NULL) {
                size_t len = strlen(de->d_name);
                if (len < 5) continue;
                if (strcasecmp(de->d_name + len - 4, ".wav") != 0) continue;
                if (!has_upload_marker(de->d_name)) total++;
            }
            closedir(dir);
        }
    }
    sd_unlock();
    return total;
}

int sd_store_pending_refresh(void)
{
    s_pending_cache = sd_store_count_pending();
    return s_pending_cache;
}

int sd_store_pending_cached(void)
{
    return s_pending_cache;
}

esp_err_t sd_store_delete(const char *name)
{
    sd_lock();
    if (!s_mounted) {
        sd_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    char path[SD_NAME_LEN + 32];
    snprintf(path, sizeof(path), "%s/%.90s", SD_MOUNT_POINT, name);
    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "删除失败 %s: %s", path, strerror(errno));
        sd_unlock();
        return ESP_FAIL;
    }
    char marker[SD_NAME_LEN + 32];
    snprintf(marker, sizeof(marker), "%s/%.90s.up", SD_MOUNT_POINT, name);
    unlink(marker);
    char meta[SD_NAME_LEN + 32];
    snprintf(meta, sizeof(meta), "%s/%.90s.json", SD_MOUNT_POINT, name);
    unlink(meta);
    ESP_LOGI(TAG, "已删除: %s", name);
    sd_unlock();
    return ESP_OK;
}

esp_err_t sd_store_write_text(const char *name, const char *text)
{
    sd_lock();
    if (!s_mounted) {
        sd_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    char path[SD_NAME_LEN + 32];
    snprintf(path, sizeof(path), "%s/%.90s", SD_MOUNT_POINT, name);
    FILE *f = NULL;
    for (int attempt = 0; attempt < 3 && !f; attempt++) {
        f = fopen(path, "wb");
        if (!f) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!f) {
        sd_unlock();
        return ESP_FAIL;
    }
    size_t len = strlen(text);
    size_t w = fwrite(text, 1, len, f);
    int rc = fclose(f);
    sd_unlock();
    return (w == len && rc == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t sd_store_read_text(const char *name, char *buf, size_t buflen)
{
    sd_lock();
    if (!s_mounted || buflen == 0) {
        sd_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    char path[SD_NAME_LEN + 32];
    snprintf(path, sizeof(path), "%s/%.90s", SD_MOUNT_POINT, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        sd_unlock();
        return ESP_FAIL;
    }
    size_t n = fread(buf, 1, buflen - 1, f);
    fclose(f);
    buf[n] = '\0';
    sd_unlock();
    return ESP_OK;
}

int sd_store_raw_read_test(uint32_t start_block, uint32_t count,
                           uint32_t *failed, int max_failed)
{
    return sd_store_raw_read_test_n(start_block, count, 1, failed, max_failed);
}

int sd_store_raw_read_test_n(uint32_t start_block, uint32_t count, uint32_t nsec,
                             uint32_t *failed, int max_failed)
{
    sd_lock();
    if (!s_card) {
        sd_unlock();
        return -1;
    }
    if (nsec < 1) nsec = 1;
    if (nsec > 32) nsec = 32;
    static uint8_t buf[512 * 32];
    int nfail = 0;
    uint32_t step = nsec;
    for (uint32_t blk = start_block; blk < start_block + count; blk += step) {
        esp_err_t err = sdmmc_read_sectors(s_card, buf, blk, nsec);
        if (err != ESP_OK) {
            if (nfail < max_failed && failed) failed[nfail] = blk;
            nfail++;
        }
    }
    sd_unlock();
    return nfail;
}

esp_err_t sd_store_remove_old(uint16_t keep_days)
{
    sd_lock();
    if (!s_mounted || keep_days == 0) {
        sd_unlock();
        return ESP_OK;
    }
    time_t now = time(NULL);
    if (now < 1700000000) {
        ESP_LOGW(TAG, "系统时间不可信, 跳过按天清理");
        sd_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    time_t cutoff = now - (time_t)keep_days * 86400;
    sd_file_t files[64];
    int n = sd_store_list(files, 64, false);
    int removed = 0;
    for (int i = 0; i < n; i++) {
        if (files[i].mtime != 0 && (time_t)files[i].mtime < cutoff) {
            if (sd_store_delete(files[i].name) == ESP_OK) removed++;
        }
    }
    ESP_LOGI(TAG, "按天清理: 删除 %d 个文件", removed);
    sd_unlock();
    return ESP_OK;
}
