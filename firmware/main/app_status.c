// app_status.c
#include "app_status.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static app_status_t s_status;
static SemaphoreHandle_t s_lock;

void app_status_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_lock = xSemaphoreCreateMutex();
    app_status_set_state(APP_IDLE);
}

app_status_t *app_status_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return &s_status;
}

void app_status_unlock(void)
{
    xSemaphoreGive(s_lock);
}

void app_status_snapshot(app_status_t *out)
{
    app_status_t *s = app_status_lock();
    *out = *s;
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    app_status_unlock();
}

const char *app_state_name(app_state_t st)
{
    switch (st) {
    case APP_IDLE:       return "idle";
    case APP_PRE_RECORD: return "pre_record";
    case APP_RECORDING:  return "recording";
    case APP_UPLOADING:  return "uploading";
    case APP_COOLDOWN:   return "cooldown";
    case APP_SLEEPING:   return "sleeping";
    case APP_ERROR:      return "error";
    default:             return "unknown";
    }
}

void app_status_set_state(app_state_t st)
{
    app_status_t *s = app_status_lock();
    s->state = st;
    strncpy(s->state_text, app_state_name(st), sizeof(s->state_text) - 1);
    s->state_text[sizeof(s->state_text) - 1] = '\0';
    app_status_unlock();
}
