// logbuf.c
#include "logbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

typedef struct {
    uint32_t uptime_ms;
    char text[LOGBUF_LINE_LEN];
} logline_t;

static logline_t s_lines[LOGBUF_LINES];
static int s_head = 0;   // 下一个写入位置
static int s_count = 0;
static SemaphoreHandle_t s_lock = NULL;
static vprintf_like_t s_orig_vprintf = NULL;

static void push_line(const char *text)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    logline_t *l = &s_lines[s_head];
    l->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    strncpy(l->text, text, LOGBUF_LINE_LEN - 1);
    l->text[LOGBUF_LINE_LEN - 1] = '\0';
    s_head = (s_head + 1) % LOGBUF_LINES;
    if (s_count < LOGBUF_LINES) s_count++;
    if (s_lock) xSemaphoreGive(s_lock);
}

static int log_tee(const char *fmt, va_list ap)
{
    char buf[LOGBUF_LINE_LEN];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n > 0) {
        // 去掉行尾换行再入缓冲
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }
        if (len > 0) push_line(buf);
    }
    if (s_orig_vprintf) {
        return s_orig_vprintf(fmt, ap);
    }
    return vprintf(fmt, ap);
}

void logbuf_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_orig_vprintf = esp_log_set_vprintf(log_tee);
}

void logbuf_add(const char *fmt, ...)
{
    char buf[LOGBUF_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    push_line(buf);
}

void logbuf_to_json(cJSON *arr)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    int start = (s_count == LOGBUF_LINES) ? s_head : 0;
    for (int i = 0; i < s_count; i++) {
        int idx = (start + i) % LOGBUF_LINES;
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_lines[idx].text));
    }
    if (s_lock) xSemaphoreGive(s_lock);
}
