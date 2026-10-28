// sd_store.h —— MicroSD (SPI) 挂载 / 文件列表 / 清理
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_NAME_LEN 96

typedef struct {
    char     name[SD_NAME_LEN];
    uint32_t size;
    bool     uploaded;
    uint32_t mtime;      // 0 表示时间未知
} sd_file_t;

// 供 wav_writer 等直接使用 stdio 的模块串行化访问 SD
void      sd_store_lock(void);
void      sd_store_unlock(void);

esp_err_t sd_store_mount(bool format_if_mount_failed);
esp_err_t sd_store_format(void);
void      sd_store_unmount(void);
// CRC 错误后卡可能锁死: 卸载并重新初始化卡, 通常无需断电即可恢复
esp_err_t sd_store_recover(void);
bool      sd_store_mounted(void);
esp_err_t sd_store_info(uint64_t *total_bytes, uint64_t *free_bytes);
void      sd_store_info_cached(uint64_t *total_bytes, uint64_t *free_bytes);

int       sd_store_list(sd_file_t *out, int max, bool only_pending);
int       sd_store_count_pending(void);
int       sd_store_pending_refresh(void);   // 扫描并更新缓存
int       sd_store_pending_cached(void);    // 读缓存 (不碰 SD, 可在录音中安全调用)
esp_err_t sd_store_delete(const char *name);
esp_err_t sd_store_write_text(const char *name, const char *text);
esp_err_t sd_store_read_text(const char *name, char *buf, size_t buflen);
esp_err_t sd_store_remove_old(uint16_t keep_days);

// 诊断: 原始块读取自检 (绕过文件系统)
// start_block..start_block+count-1, 返回失败块数, failed 中保存失败块号 (最多 max_failed)
int sd_store_raw_read_test(uint32_t start_block, uint32_t count,
                           uint32_t *failed, int max_failed);
int sd_store_raw_read_test_n(uint32_t start_block, uint32_t count, uint32_t nsec,
                             uint32_t *failed, int max_failed);

#ifdef __cplusplus
}
#endif
