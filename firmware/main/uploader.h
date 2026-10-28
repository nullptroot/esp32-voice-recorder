// uploader.h —— 批量上传待传 WAV (multipart/form-data, 指数退避重试)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  pending_total;
    int  attempted;
    int  uploaded;
    int  duplicates;
    int  failed;
    int  skipped;
    char last_message[160];
} upload_report_t;

// 确保 WiFi 连接后, 上传 SD 卡里所有待传文件 (无 .up 标记的 .wav)
esp_err_t uploader_run(upload_report_t *rep);

// 上传单个文件 (文件名相对 SD 根目录)
esp_err_t uploader_upload_one(const char *name, char *msg, size_t msglen,
                              bool *out_duplicate);

// 检查服务器健康状态, 结果写入 msg
esp_err_t uploader_health(char *msg, size_t msglen);

#ifdef __cplusplus
}
#endif
