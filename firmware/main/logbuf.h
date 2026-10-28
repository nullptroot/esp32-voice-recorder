// logbuf.h —— 环形日志缓冲, 供设备网页 /api/log 查看
#pragma once

#include <stddef.h>
#include "cJSON.h"

#define LOGBUF_LINES 100
#define LOGBUF_LINE_LEN 200

void logbuf_init(void);
void logbuf_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logbuf_to_json(cJSON *arr);
