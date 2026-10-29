# 录音豆（ESP32-S3）固件 + 极简验收服务

全候录录音豆的设备端软件实现与验证工程。硬件接线见 [`wire.md`](wire.md)，
功能需求见 [`hardware-device.md`](hardware-device.md)，后端接口契约见
[`docs/backend-api.md`](docs/backend-api.md)。

```
esp32/
├── wire.md                       # 接线表（本轮补充了 LED / 按键）
├── hardware-device.md            # 需求文档
├── backend-server.md             # 后端设计文档（原稿）
├── firmware/                     # ESP-IDF v5.5 固件
│   ├── main/
│   │   ├── app_main.c            # 级联唤醒状态机
│   │   ├── recorder.c            # 预录 → VAD 确认 → 分段写 WAV
│   │   ├── vad.c                 # 能量 + 过零率 VAD（可在 PC 单测）
│   │   ├── mic_i2s.c             # INMP441 采集
│   │   ├── wav_writer.c          # WAV 写入 / 头回填
│   │   ├── sd_store.c            # SD 挂载 / 格式化 / 文件列表 / 清理
│   │   ├── uploader.c            # multipart 上传 + 指数退避
│   │   ├── net_wifi.c            # WiFi STA
│   │   ├── time_sync.c           # NTP + 时间可信度回退
│   │   ├── web_config.c          # 设备内置配置网页 + REST
│   │   ├── app_config.c          # NVS 配置
│   │   ├── app_status.c          # 运行状态快照
│   │   ├── led_button.c          # 状态 LED + 按键
│   │   └── index.html            # 配置网页（编译进固件）
│   └── host_test/vad_test.c      # PC 端 VAD 单测
├── backend-min/                  # 极简验收服务 (FastAPI + SQLite)
└── docs/
    ├── backend-api.md            # 后端需要实现的完整接口协议
    ├── verification-report.md    # 本轮验证报告
    └── handover.md               # 交接文档（待办、调试手册、坑位）
```

---

## 1. 功能实现对照（hardware-device.md）

| 需求 | 实现 |
|---|---|
| LM393 模拟检测唤醒 | GPIO15，EXT1 低电平唤醒（`esp_sleep_enable_ext1_wakeup_io`） |
| VAD 二次确认 | `vad.c`：自适应噪声底 + 能量门限 + 过零率；连续 3 帧才确认（VADNet 留作升级位，接口不变） |
| 误触发冷却窗口 | `cooldown_seconds`，VAD 未确认 → 冷却 → 不产生文件 |
| 16k/16bit/单声道 WAV | `wav_writer.c`，44 字节标准头，关闭时回填长度 |
| 15 分钟分段 | `segment_seconds`（默认 900，可网页改） |
| 时间不可信回退 | NTP 失败 → `mic1_session-XXXX_partN.wav` + `time_trusted=false` |
| 上传重试 / 不丢数据 | 指数退避 2/4/8/16/32s；成功前不删文件；`.up` 标记 |
| 录音时关 WiFi | `wifi_off_while_recording`（bench 模式例外） |
| 任务绑核 | 录音 worker 固定 core 1，httpd/LED/按键在 core 0 |
| 录音状态 LED | GPIO47，慢闪/快闪/双闪/常亮语义见 wire.md |
| 板载录音指示灯 | GPIO48（板载 LED，低电平点亮）：录音时亮、其余灭，见 wire.md §4.1 |
| SD 写失败 / 低空间 | 写失败即停并记录错误；<50MB 拒录，<10MB 停录 |
| 录音状态网页 | GPIO47 LED + 内置网页实时状态 |
| 深度睡眠 | bench 模式默认常醒；`bench_mode=false` + `sleep_enable=true` 才真睡 |

> 与原文档的差异：VAD 先用轻量能量/过零率实现（本轮约定），`vad.c` 已隔离，
> 后续替换 esp-sr VADNet 只需保持 `vad_init/vad_frame` 接口。
> 另新增设备内置配置网页（用户本轮要求），可热改全部参数，无需重编译。

---

## 2. 编译与烧录

```bash
# 一次性安装 ESP-IDF v5.5.6（本机已装于 ~/esp-idf-v5.5.6）
cd ~/esp-idf-v5.5.6 && ./install.sh esp32s3

cd /home/mr4/esp32/firmware
source ~/esp-idf-v5.5.6/export.sh
idf.py set-target esp32s3      # 只需一次
idf.py build
idf.py -p /dev/ttyUSB0 -b 460800 flash monitor
```

- 串口：板载 CH340 → `/dev/ttyUSB0`；原生 USB → `/dev/ttyACM0`（同一颗芯片，二者皆可烧录）
- 控制台日志走 UART0（115200）
- 首次启动若 SD 卡未格式化会尝试 FAT32 格式化（`format_sd_if_mount_failed` 默认开）
- 默认配置：WiFi `FreshTomato24`，后端 `http://192.168.5.36:8000`，bench 模式常醒

## 3. 设备配置网页

设备连上 WiFi 后，浏览器打开 `http://<设备IP>/`（IP 打印在串口日志，也可在路由器里看）。
可改：WiFi、后端地址、设备 ID、采样率/增益、分段时长、VAD 阈值、静音停录、
冷却时间、上传策略、保留天数、bench/深睡模式；并可在线开始/停止录音、立即上传、
格式化 SD、查看实时电平和文件列表、设备日志。

## 4. 极简验收服务

```bash
cd backend-min
./run.sh            # http://0.0.0.0:8000
curl -s localhost:8000/health
```

## 5. 已知问题（本轮实测发现）

### ~~SD 卡坏块~~ → 已定位为**供电问题**（2026-10-29 晚已修复）
本机这张 32GB TF 卡（卷标 `asdfg`）曾被判为「漂移坏块」：原始块读自检 `/api/sdtest` 显示
某些块（先 29895 附近，后 7547 附近）稳定 CRC 报错，断电 5 秒可临时恢复、几分钟后复发。

**真因**：SD 卡模块板载 3.3V LDO，VCC 接 3V3 时压差不足 → 卡供电不稳 → 随机 CRC / 写失败。
**VCC 改接 5V 后**，同一张卡 0–100MB 扫描 **0 错误**，格式化/60s 录音/连续 10 次录音/上传全部通过。

固件里为坏卡做的缓解（64KB 簇、`sd_store_recover()`、头部回填校验重试、损坏分段自动删除）保留，
对好卡也无副作用。

详见 [`docs/verification-report.md`](docs/verification-report.md) §8。

## 6. 验证清单

| # | 场景 | 操作 | 判据 |
|---|---|---|---|
| 1 | VAD 算法 | `gcc -O2 -I firmware/main firmware/host_test/vad_test.c firmware/main/vad.c -lm -o /tmp/vad_test && /tmp/vad_test` | 全部 PASS |
| 2 | SD 挂载 | 上电看日志 | `SD 挂载成功`，打印容量 |
| 3 | I2S 采集 | 网页「开始录音」，说话 10s | 状态 `recording`，实时电平随说话跳动 |
| 4 | WAV 正确性 | 下载/取出 `mic1_*.wav` | `ffprobe` 显示 16000Hz/mono/s16，可播放 |
| 5 | 分段 | `segment_seconds=20`，录 65s | 生成 part1/2/3，各约 20s |
| 6 | VAD 误触发 | 不放声音，按「开始录音」 | 5s 静音后自动停（`silence_stop_ms`） |
| 7 | 上传 | 「立即上传」 | 服务器 `backend-min/storage/audio/` 出现文件 |
| 8 | 重试 | 关服务再录，再开服务上传 | 文件保留，补传成功 |
| 9 | 时间回退 | WiFi 改为错误 SSID | 文件名 `mic1_session-XXXX_partN.wav`，`time_trusted=false` |
| 10 | 深睡 | bench 关 + sleep 开 + 重启 | 日志 `进入 EXT1 深度睡眠`；发声唤醒后自动录音 |

> 2026-10-29 晚实测：2/4/6/7/10 均 ✅；连续 10 次录音 10/10 ✅；深睡唤醒→上传 22.3s ✅。
> 未测：5（分段，昨天 6s 分段已验证过）、8（重试，10-28 已验证）、9（见 P1-4 的坑，慎测）。
