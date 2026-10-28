// pins.h —— 引脚规划表, 与 wire.md 保持一致
#pragma once

// ---- INMP441 (I2S, 接收) ----
#define PIN_I2S_WS        4    // WS  / LRCLK
#define PIN_I2S_BCK       5    // SCK / BCLK
#define PIN_I2S_DIN       6    // SD  / DATA (ESP32 输入)

// ---- MicroSD (SPI2) ----
#define PIN_SD_CS         10
#define PIN_SD_MOSI       11
#define PIN_SD_SCLK       12
#define PIN_SD_MISO       13

// ---- LM393 声音检测 (RTC GPIO, 支持 EXT1 深睡唤醒) ----
#define PIN_SND_DETECT    15   // DO, 检测到声音时拉低

// ---- 状态指示 & 手动触发 ----
#define PIN_LED           47
#define PIN_BUTTON        41   // 按下接 GND

#define SD_MOUNT_POINT    "/sdcard"
