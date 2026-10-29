# 录音豆项目 · 交接文档

> 交接时间：2026-10-29
> 交接人：本轮实现（AI）+ 你
> 代码位置：`/home/mr4/esp32`（远端 192.168.5.36，工作区根目录）
> 设备：立创 ESP32S3R8N8（8MB Flash / 8MB PSRAM），当前 IP `192.168.5.32`（DHCP，可能变）

---

## 0. 一句话现状

设备端软件（录音 → VAD → 分段写卡 → 上传 → 网页配置）**已实现并在真机跑通**，
**2026-10-29 晚已复验完毕**：SD「坏块」是误判（模块 VCC 需 5V），换供电后同一张卡读写全好；
深睡 → LM393 唤醒 → 录音 → 自动停录 → 上传 → 再睡 **端到端 22.5 秒跑通**。
后端仍只有「极简验收服务」（落盘 + SQLite + 去重），完整转写/日记链路还没做。
当前没有阻塞项，剩下的都是待做项（按键未接线、VAD 一小时误触发标定、功耗实测、后端链路）。

---

## 0.5 新会话怎么接手（5 分钟上手）

### 读文档顺序
1. `README.md` —— 工程结构、构建/烧录、验收清单（3 分钟）
2. `docs/verification-report.md` —— 已经做了什么、修了哪些坑、SD 卡故障证据（5 分钟）
3. `docs/backend-api.md` —— 设备↔后端契约（后续做后端必读）
4. `docs/web-ui.md` —— 网页每个参数怎么用、怎么调
5. 本文件 §6 待办 + §7 调试手册 —— 接着干什么

### 环境自检（30 秒）
```bash
source ~/esp-idf-v5.5.6/export.sh && idf.py --version     # 期望: ESP-IDF v5.5.6
curl -s http://127.0.0.1:8000/health || echo "后端没起: cd esp32/backend-min && ./run.sh"
curl -s http://192.168.5.32/api/status | head -c 200 || echo "设备不在线，见下"
```

### 设备不在线时
1. 串口看日志：`idf.py -p /dev/ttyUSB0 monitor`（打开串口会复位板子，属正常）；
2. 串口里找 `wifi: 获得 IP:`，IP 可能已变（DHCP）；
3. 都没输出 → 检查是否插的是 `/dev/ttyUSB0`（CH340，控制台在 UART0）、USB 是否供电；
4. SD 卡锁死时设备仍能启动，只是录音失败（网页仍可访问，看 `/api/log`）。

### 代码版本
仓库已初始化 git，首个提交：`d2a03a5 录音豆 ESP32-S3 固件 + 极简验收后端 + 文档（首版交接）`。
后续改动建议每完成一项就提交一次：
```bash
cd /home/mr4/esp32 && git add -A && git commit -m "说明这次改了什么"
```

---

## 1. 交付物地图

```
/home/mr4/esp32
├── README.md                  # 速览：构建/烧录/验收清单/已知问题
├── wire.md                    # 接线表（已补 LED=47、按键=41；LM393 实测高有效）
├── hardware-device.md         # 原始需求文档（设备侧）
├── backend-server.md          # 原始需求文档（后端侧）
├── docs/
│   ├── backend-api.md         # ⭐ 后端需要实现的完整接口协议（对外交付）
│   ├── web-ui.md              # ⭐ 设备网页使用说明 + 每个参数的含义/建议值
│   └── verification-report.md # ⭐ 本轮验证报告（证据 + 修复清单 + SD 故障诊断）
├── firmware/                  # ESP-IDF 工程（v5.5.6）
│   ├── CMakeLists.txt / sdkconfig.defaults / partitions.csv
│   ├── main/                  # 30 个源文件，见 §4
│   └── host_test/vad_test.c   # VAD 算法 PC 单测（不依赖硬件）
├── backend-min/               # 极简验收服务（FastAPI + SQLite）
│   ├── app.py / requirements.txt / run.sh
│   └── storage/{audio/,uploads.db}
└── .venv-backend/             # 后端依赖虚拟环境（已装好）
```

### 版本管理
仓库已 `git init` 并提交首版（`d2a03a5`，46 个文件）；`build/`、`sdkconfig`、venv、`storage/` 已在 `.gitignore` 中。

### 当前运行中的进程（重启机器后会没）

| 进程 | 地址 | 启动方式 |
|---|---|---|
| 极简后端 | `http://192.168.5.36:8000` | `cd backend-min && ./run.sh` |
| 设备网页 | `http://192.168.5.32/` | 固件自带 httpd，随设备启动 |

```bash
# 检查后端
curl -s http://127.0.0.1:8000/health
curl -s http://127.0.0.1:8000/api/uploads | python3 -m json.tool
# 检查设备
curl -s http://192.168.5.32/api/status | python3 -m json.tool
```

---

## 2. 环境与常用命令

### 2.1 工具链（已安装，勿重复安装）

| 内容 | 位置 |
|---|---|
| ESP-IDF v5.5.6 | `~/esp-idf-v5.5.6`（`source export.sh` 后可用 `idf.py`） |
| Xtensa 工具链 / cmake / ninja | `~/.espressif/tools` |
| esptool（独立 venv） | `~/.venv-esp/bin/esptool.py` |
| 后端 venv | `/home/mr4/esp32/.venv-backend` 与 `backend-min/.venv-backend` |

### 2.2 构建与烧录

```bash
cd /home/mr4/esp32/firmware
source ~/esp-idf-v5.5.6/export.sh
idf.py build                      # 产物 build/recbean.bin（约 1.1MB）
idf.py -p /dev/ttyUSB0 -b 460800 flash
idf.py -p /dev/ttyUSB0 monitor    # 115200，Ctrl+] 退出
```

- 首次全量构建约 2–3 分钟，增量构建约 40–60 秒；自动化环境里建议 `nohup ... &` 后轮询 `build.log`，避免长命令超时。
- 确认烧进去的是新固件：串口首行 `boot: compile time ...` 应与本次 `idf.py build` 的时间一致，或核对 `build/recbean.bin` 的 SHA256 与启动日志里的 `ELF file SHA256`。
- 串口：`/dev/ttyUSB0` = 板载 CH340（UART0 控制台）；`/dev/ttyACM0` = 原生 USB-Serial/JTAG，**同一颗芯片**，两者都能烧。
- ⚠️ **打开串口会触发 DTR/RTS 复位板子**（属正常现象，抓完整启动日志时注意）。
- ⚠️ 若烧录报 `port busy`：先杀掉占用串口的进程（例如自己开的 `mon.py`）。

### 2.3 设备网页

浏览器打开 `http://192.168.5.32/`（IP 见串口日志 `wifi: 获得 IP`）。
可改：WiFi/密码、后端地址、设备 ID、采样率、增益、分段、最长录音、预录窗口、
静音停录、冷却、VAD 阈值、上传策略、保留天数、SD 频率、bench/深睡、LM393 极性。
按钮：开始/停止录音、立即上传、服务器健康检查、扫描 WiFi、格式化 SD、重启。

### 2.4 常用调试接口（设备自带）

| 接口 | 用途 |
|---|---|
| `GET /api/status` | 状态/电平/SD/待传/最近上传（**录音中也可安全调用**，走缓存） |
| `GET /api/log` | 最近 100 条设备日志（排障主力） |
| `GET /api/files` | SD 上 WAV 列表（读目录，录音中别频繁调） |
| `GET /api/file?name=xxx.wav` | 下载 WAV 到本机 |
| `GET /api/sdtest?start=29890&count=16` | **原始块读自检**（绕过文件系统找坏块） |
| `POST /api/sd/recover` | 卸载→重新初始化→挂载（卡锁死时不用断电） |
| `POST /api/format` | `{"confirm":"YES"}` 格式化（20MHz 下约 3.5s / 16KB 簇约 14s） |

### 2.5 主机单测（VAD，无需硬件）

```bash
cd /home/mr4/esp32/firmware/host_test
gcc -O2 -I ../main vad_test.c ../main/vad.c -lm -o /tmp/vad_test && /tmp/vad_test
```

---

## 3. 架构与数据流

```text
LM393(GPIO15,高有效) ──EXT1──► 唤醒(或网页/按键触发)
                                 │
                                 ▼
                    ┌── 预录 1.5s(PSRAM 环形缓冲) ──┐
                    │   VAD 二次确认(连续3帧=60ms)    │
                    └── 未确认 ─► 冷却 cooldown ─► 深睡
                                 │ 确认
                                 ▼
        循环: I2S 读 1024 样本(64ms) → 20ms 帧 VAD → 写 WAV
              ├─ 分段满 segment_seconds → 关文件+写 .json 元数据 → 开新段
              ├─ 说话后静音 ≥ silence_stop_ms → 停录
              ├─ 达到 max_record_seconds → 停录
              └─ 写失败/空间不足 → 停录并记录错误
                                 │
                                 ▼
        关闭文件 → 校验目录项大小 → 回填 WAV 头 → (损坏则删除)
                                 │
                                 ▼
        上传(开 WiFi) → multipart POST /upload → 成功写 .up 标记
              ├─ 失败: 指数退避重试(2/4/8/16/32s)，文件保留
              └─ delete_after_upload / keep_days 策略
                                 │
                                 ▼
                     非 bench 模式 → EXT1 深睡
```

任务绑定：录音 worker 固定 **core 1**；httpd / LED / 按键 / SD 监视在 **core 0**。

---

## 4. 代码结构与改动导航

| 文件 | 行数 | 职责 | 常见改动点 |
|---|---:|---|---|
| `main/pins.h` | 22 | 引脚定义 | 换引脚改这里 |
| `main/app_main.c` | ~320 | 启动、触发队列、worker、深睡 | `worker_task`、`enter_deep_sleep`、`snd_monitor_task` |
| `main/recorder.c` | 441 | 一次录音循环（预录/VAD/分段/元数据） | `recorder_run`、`SPEECH_CONFIRM_FRAMES`、`MIN_FREE_BYTES` |
| `main/vad.c` `vad.h` | 89 | VAD 算法（滑窗最小值噪声底） | 阈值/窗口/ZCR；**换 VADNet 只改这两个文件** |
| `main/mic_i2s.c` | 168 | INMP441 采集 + 增益 + DC blocker | `mic_read` 的移位、`DC_BLOCKER_A`、DMA 深度 |
| `main/wav_writer.c` | 197 | WAV 写 + 关闭时回填头 + 校验 | `patch_header`、`wav_close` |
| `main/sd_store.c` | 439 | 挂载/格式化/列表/删除/锁/恢复 | `mount_inner`（`allocation_unit_size`）、`sd_store_recover`、递归锁 |
| `main/uploader.c` | 400 | multipart 上传 + 退避 + `.up` | `do_post`（字段/边界）、`uploader_rule` 重试次数 |
| `main/net_wifi.c` | 169 | WiFi STA | 连接超时、扫描 |
| `main/time_sync.c` | 81 | NTP + 时间可信度 | 服务器名、`MIN_VALID_EPOCH` |
| `main/app_config.c` | ~330 | NVS 配置 + JSON 序列化 + 钳位 | 加参数：结构体 + 默认值 + JSON + 钳位 4 处 |
| `main/app_status.c` | 59 | 状态快照（带锁） | 新字段：`app_status.h` + 初始化 |
| `main/web_config.c` | 431 | httpd + 全部 REST | 新接口：`uris[]` 注册 + handler |
| `main/led_button.c` | 121 | LED 模式 + 按键短/长按 | LED 闪烁节奏 |
| `main/logbuf.c` | 87 | 日志环形缓冲（`/api/log` 数据源） | — |
| `main/index.html` | — | 配置网页前端（EMBED_TXTFILES 编进固件） | 表单字段 + `fillConfig/saveConfig` 列表 |
| `backend-min/app.py` | 263 | 极简验收服务 | 落盘路径、DB 字段、去重键 |

### 加一个配置项的最短路径

1. `app_config.h`：结构体加字段 + `CFG_VERSION` +1（版本变了会重置为默认值）
2. `app_config.c`：`app_config_defaults` / `clamp_cfg` / `app_config_to_json` / `app_config_from_json` 四处
3. `index.html`：表单控件 + 两处 JS 字段名数组
4. 使用处：`app_config_get()->新字段`

---

## 5. 当前参数与默认值

| 配置项 | 当前值 | 说明 |
|---|---|---|
| `wifi_ssid` / `wifi_pass` | `FreshTomato24` / `******` | 保存在 NVS |
| `server_url` | `http://192.168.5.36:8000` | 不带结尾斜杠 |
| `device_id` | `esp32s3-001` | 上传元数据 |
| `sample_rate` | 16000 | 8k/16k/32k 可选 |
| `gain_shift` | 3（+18dB） | 0=不放大；实测 INMP441 偏弱，3 较合适；4 会削顶 |
| `segment_seconds` | 900 | 验收时改小（如 15/20） |
| `max_record_seconds` | 1800 | 单次录音上限 |
| `pre_record_ms` | 1500 | 预录窗口 / VAD 确认窗口 |
| `silence_stop_ms` | 5000 | 0=不定时停；**只在检测到人声后才计时** |
| `cooldown_seconds` | 20 | 误触发冷却 |
| `vad_threshold_db` | 12 | 高于噪声底多少 dB 算人声（10-29 实测本房间 8 太灵敏，12 才稳）；6 更灵敏 |
| `upload_on_record / on_boot` | true / true | 录完即传 / 开机补传 |
| `upload_max_retry` / `backoff_ms` | 5 / 2000 | 指数退避 |
| `delete_after_upload` / `keep_days` | false / 7 | 保留策略（时间可信时生效） |
| `bench_mode` | true | 常醒 + 网页常驻；**false 才会深睡** |
| `sleep_enable` | false | 与 bench_mode 同时满足才深睡 |
| `sound_active_low` | **true** | 10-29 实测本 LM393 模块**低有效**（安静高、有声 10-20ms 低脉冲），与 10-28 记录相反 |
| `sd_freq_khz` | 20000（NVS）| 代码默认 1000（更稳）；SD 模块 VCC 接 5V 后 20000 稳定 |
| `format_sd_if_mount_failed` | true | 挂载失败自动格式化（会清卡） |

---

## 6. 待办清单（按优先级）

### P0-1 换 TF 卡并复测全链路 ⭐ 已完成（结论：不是坏卡，是供电）

**结论（2026-10-29 晚）**：把 SD 模块 VCC 从 3V3 改接 **5V**（模块板载 3.3V LDO）后，同一张卡
0–100MB 原始扇区扫描 **0 错误**，格式化 3.6s，60s 录音、连续 10 次录音、上传全部通过。
详见 `docs/verification-report.md` §8。**不需要换卡。**

**复现验收步骤**（以后换卡/换模块照做）：
1. `sd_freq_khz=20000` → 网页格式化 SD；
2. `python3 tools/sd_sweep.py 192.168.5.32 0 200000` 应 `失败块数 0`；
3. 录 60s，文件大小 ≈ `60*32000+44`，`tools/wav_check.py` 正常；
4. `POST /api/upload` → 后端 `/api/uploads` 有记录、`wav_valid=1`；
5. `python3 tools/rec_test.py 192.168.5.32 10 10` 连续 10 次全部 OK。

### P0-2 EXT1 深睡 + 唤醒端到端 ✅ 已完成（2026-10-29 23:16）

**实测时间线**（唤醒 → 上传 22.3s）：唤醒 → 关 WiFi → VAD 确认 1.5s → 录音 16s → 静音 5s 停录 →
连 WiFi 上传 (HTTP 200 stored) → 再睡。完整日志见 `docs/verification-report.md` §8.3。

**注意（本轮修的坑）**：唤醒时会同时入队「开机补传」+「录音确认」，worker 必须等队列空才睡，
否则录音触发被吞（表现为「唤醒后 3 秒又睡了」）。代码已修（`git log` 见 `fix: 修复唤醒后不录音…`）。

**重跑方法**：
1. 网页设 `bench_mode=false`、`sleep_enable=true` → 重启；
2. 串口应出现 `进入 EXT1 深度睡眠, 唤醒脚 GPIO15 (低电平触发)`，随后设备离线；
3. 拍手/说话 → `复位原因: 8, 深睡唤醒: EXT1 (LM393)` → 录音 → 上传 → 再睡；
4. 记录：唤醒到上传耗时、是否误唤醒。

**验收**：从拍手到文件出现在服务器 < 30s（✅ 22.3s）；静置 10 分钟无误唤醒（❌ 未测，见 P1-3）。

### P0-3 深睡后重新进入网页 ✅ 已完成（2026-10-29 23:40）

**问题**：`bench_mode=false` 后设备深睡，WiFi + httpd 全关，网页再也进不去；原来只能重烧固件。

**实现**（`app_main.c`）：
1. **上电/复位配置窗口**：非 bench 模式下，每次启动（非 LM393 唤醒）自动开 **120 秒**网页，倒计时结束自动深睡；
2. **三连复位**：15 秒内连续复位 3 次 → 本轮 **300 秒**配置窗口（NVS 计数器 `rst_streak`，正常进睡/运行 15 秒清零）；
3. `/api/status` 新增 `cfg_window_left`，页面顶部显示 `⚙ 配置窗口: 87s 后进深睡`。

**实测**：正常上电 → 120s 窗口 → 125.6s 时 `配置窗口结束 → 进入 EXT1 深度睡眠`；
网页重启 3 次 → `连续 3 次快速复位 -> 强制进入配置模式`，`cfg_window_left=297`。

**用法**：深睡后想改配置，就拔插 USB（或按 RST），120 秒内打开 `http://<ip>/`；要更久就连按 3 次复位。

### P1-1 按键接线与验证

**怎么做**：GPIO41 ↔ 按键 ↔ GND（内部上拉）。短按（<1s）开始/停止录音；长按（≥2s）立即上传。
**验收**：短按能起停，长按触发补传，日得有 `按键短按/长按` 日志。

### P1-2 说话录音质量验收（听感）

**怎么做**：`gain_shift=3`，正常距离说 10 秒，下载 WAV 听；
若偏小→4（注意削顶），若爆音→2；同时看 `/api/status` 的 `level_db`（安静约 -50，说话 -25 左右）。
**验收**：人声清晰、无明显底噪/削顶、时长正确。

### P1-3 VAD 阈值标定 + 误触发率（部分完成）

**已测（2026-10-29 晚）**：本房间噪声底约 -50dB、尖峰 -33dB，`vad_threshold_db=8` 时几乎每 0.5s
都被判成人声 → 静音停录永远到不了 5s（录音会一直跑到 `max_record_seconds`）。
改成 **12dB** + 固件「连续 3 帧人声才刷新说话时刻」后，16s 音频能正常在静音 5s 后停录。

**还差**：
1. 静置 1 小时，统计 `sound_hits`（LM393 跳变）与是否产生文件；
2. 误触发多：调 LM393 电位器 / `vad_threshold_db` 12→14 / `cooldown_seconds` 20→60；
3. 目标是「1 小时误触发 ≤ 2 次，真实说话 100% 触发」。

### P1-4 时间不可信链路实测 ⚠️ 原方案有坑

原方案「网页把 WiFi 密码改错」会让设备彻底离线，**改回来没有通道**（网页也没了）。要做请改用
「关掉路由器 / 改 SSID」的方式，或临时烧一版强制 `time_sync_trusted()==false` 的固件。
文件名回退 `mic1_session-XXXX_partN.wav`、后端 `time_trusted=0`、`recorded_at=NULL` 的代码路径已存在但未实测。

### P2-1 用 esp-sr VADNet 替换轻量 VAD（可选升级）

- 只需替换 `vad.c/vad.h`，保持 `vad_init(v, thr_db)` / `vad_frame(v, samples, n)` 语义；
- 需要：拉 `espressif/esp-sr` 组件、加 `srmodels` 模型分区（约 2–4MB，注意 factory 分区目前 4MB）、
  `esp_sr` 初始化 + `vadnet` 推理；
- 预期收益：噪声环境下误检更低；代价：Flash/RAM 增加、首次加载延迟。

### P2-2 后端完整实现（转写/日记/清理）

协议已冻结在 `docs/backend-api.md` §8：
`POST /jobs/transcribe`、`POST /jobs/summarize`、`POST /jobs/cleanup`；
表结构、Prompt、选型（FunASR SenseVoice / faster-whisper 备选；Ollama + qwen3:8b）见
`backend-server.md`。**设备端不需要改动**。

### P2-3 上传成功后的删除/保留策略实测

设 `delete_after_upload=true` / `keep_days=7`，验证：
上传成功即删本地、`.up`/`.json` 一并清理、按天清理只删过期文件。

### P3-1 功耗实测（画板前必须）

- EXT1 深睡电流（130µA 还是 1.5mA 级别）；
- 唤醒→推理周期能量（社区参考 1525 mJ/次）；
- 对比不同 GPIO 上下拉策略；1000mAh 电池估算续航。
- 工具：USB 电流表或 INA219；串口日志记录唤醒次数。

### P3-2 收尾项：`vad_enable` 与网页鉴权

1. **`vad_enable` 目前是预留开关**（`app_config` 有字段、网页有勾选框，但 `recorder.c` 始终初始化 VAD）。
   二选一：实现「关掉后不做静音停录/二次确认」或从网页与配置里移除，避免误导。
2. **设备网页无鉴权**：同网段任何人都能改配置、下载录音。正式部署建议加反向代理基本认证，
   或给 httpd 加一个简单的 `X-Auth-Token` 校验（配置项 + 页面带 header）。

### P3-3 多设备与鉴权（后端侧）

设备已发送 `X-Device-Id` 头与 `device_id` 字段，可直接做白名单/鉴权；无 TLS。

---

## 7. 调试手册（现象 → 原因 → 处理）

| 现象 | 可能原因 | 处理 |
|---|---|---|
| 文件 0 字节 / 时长字段 0 | SD 坏块导致目录项/头部回填失败 | `/api/sdtest` 找坏块 → `POST /api/sd/recover` → 断电 → 换卡 |
| `sdmmc_read_sectors: error 0x108/0x109` | SD 总线 CRC（坏块/信号） | 降 `sd_freq_khz`（20000→1000）、缩短杜邦线、换卡 |
| 录音开始就失败 `wav: 打开失败` | 目录区读失败 | 同上；固件已自动 `sd_store_recover()` 重试 |
| 上传后设备重启 / `stack overflow in task worker` | 栈不够 | worker 栈已设 28KB，如再加逻辑需同步加大 |
| 上传成功却报失败并重试 | `esp_http_client_fetch_headers()` 返回值误判 | 已修为 `< 0` 判失败；勿改回 `!= ESP_OK` |
| 录音中网页卡住 | 状态接口读了 SD（f_getfree） | 已改缓存；新接口勿在 `/api/status` 里读 SD |
| 录音 3 秒就停 | `silence_stop_ms` 在没有语音时就计时 | 已改为「检测到人声后才计时」 |
| 说话没反应/录音太短 | 增益太小或 VAD 阈值高 | `gain_shift` 3→4；`vad_threshold_db` 8→6 |
| 声音削顶失真 | 增益过大 | `gain_shift` 4→2 |
| 文件开头 0.5s 数字静音 | I2S DMA 欠载 | DMA 已加深（12×1024）；勿在录音中做长阻塞 SD 操作 |
| 文件开头有阶跃 | 直流偏置 | 已加 DC blocker + 启动丢弃 320ms |
| LM393 拍手无反应 | 接线/供电/电位器/极性 | 看串口 `LM393 DO: 0 -> 1`；网页「LM393 DO」实时电平；调电位器；`sound_active_low` 设置 |
| 深睡后立刻醒 | GPIO15 停在有效电平 | 睡眠前等安静（代码已等 30s 并告警） |
| 网页打不开 | 非 bench 模式 / WiFi 未连 / IP 变了 / 正在深睡 | 串口看 IP；**深睡时网页一定打不开**：拔插 USB 或按 RST，启动后有 120 秒配置窗口；15 秒内连按 3 次复位 = 300 秒配置窗口（页面顶部有倒计时徽标） |
| 串口没日志 | 用的是 ACM0 但控制台在 UART0 | 换 `/dev/ttyUSB0`，或改 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` |
| 一打开串口设备就重启 | DTR/RTS 复位 | 正常现象；抓日志时用 `reset_cap.py` 式脚本 |
| 格式化很久/网页卡死 | 1MHz 下 32GB FAT 写入极慢（>3 分钟） | 先设 `sd_freq_khz=20000` 再格式化，或选 64KB 簇 |
| WiFi 连不上 | SSID/密码错 / 信号弱 | 网页扫描 WiFi；日志 `wifi: 断开 (reason=...)` |

---

## 8. 不能踩的坑（改动约束）

1. **SD 访问必须走 `sd_store` 的锁**（`sd_store_lock/unlock` 或对应 API）。
   这是本轮最痛的坑：并发访问会让 SPI 事务错乱，表现为随机 CRC 错误。
   自己新写 `fopen/fread/fwrite` 时也要包锁。
2. **`/api/status` 里不要读 SD**（用 `sd_store_info_cached()` / `sd_store_pending_cached()`）。
3. **worker 栈 28KB**：新增 cJSON/HTTP 逻辑时注意，必要时再加大。
4. **`esp_http_client_fetch_headers()` 返回 content-length**，`<0` 才是失败。
5. **WAV 头不要用已打开的流回填**（SD 上会失败），用 `patch_header()` 独立句柄 + 校验。
6. **`CFG_VERSION` 变了会重置 NVS 配置**（WiFi/参数回默认），升级前告知用户。
7. **改分区大小**（比如给 esp-sr 模型腾地方）需要改 `partitions.csv` + 重新烧分区表。
8. **网页前端字段名必须与配置 JSON 键一致**，否则保存静默失效。
9. **`vad_enable` 是预留开关**（当前不生效），改 VAD 行为要改 `recorder.c` 里对 `vad_frame()` 的使用方式。

---

## 9. 出厂重置与环境还原

### 只清配置（保留固件）
```bash
source ~/esp-idf-v5.5.6/export.sh
esptool.py -p /dev/ttyUSB0 erase_region 0x9000 0x6000   # 擦 NVS，重启后回默认配置
```

### 全擦（含固件，之后再烧）
```bash
esptool.py -p /dev/ttyUSB0 erase_flash
idf.py -p /dev/ttyUSB0 -b 460800 flash
```

### 后端数据重置
```bash
cd /home/mr4/esp32/backend-min && pkill -f "uvicorn app:app"; rm -rf storage; ./run.sh
```

---

## 10. 已知良好启动日志（对照用）

```text
I (311) octal_psram: vendor id    : 0x0d (AP)
I (354) esp_psram: Found 8MB PSRAM device
I (921) main: ==== 录音豆 (recbean) 启动 ====
I (926) main: 复位原因: 1, 深睡唤醒: 否
I (938) cfg: 配置已从 NVS 载入 (ssid=..., server=http://192.168.5.36:8000, seg=900s)
I (940) main: LM393 DO = GPIO15, 当前电平=0
I (944) sd: SD SPI 时钟: 20000 kHz
I (1023) sd: SD 挂载成功: /sdcard
I (2918) wifi: 获得 IP: 192.168.5.32
I (2920) main: 首次对时 (NTP)...
I (3513) time: NTP 对时成功: 2026-10-29 00:15:16 (TZ=CST-8)
I (3515) web: 配置网页已启动: http://<ip>/
```
若 `SD 挂载失败` → 见 §7；若 `WiFi 连接失败` → 检查 SSID/密码/路由器。

### 30 分钟能做完的快速分诊
| 时间 | 做什么 |
|---|---|
| 0–5 min | 环境自检 + 设备在线确认 + 网页打开 |
| 5–15 min | 录 15s → 下载试听 → 上传 → 后端 `/api/uploads` 看到记录 |
| 15–25 min | 若 SD 报错：`/api/sdtest` 定位 → `/api/sd/recover` 或断电 |
| 25–30 min | 把结果记到 `docs/verification-report.md`，更新 git 提交 |

---

## 11. 交付前最后一次状态（2026-10-29 23:20 更新）

- 固件：**已含 4 个新修复**（唤醒后不录音 / WAV 头校验误报 / duplicate 日志 / 静音停录），
  已编译烧录（`build/recbean.bin`），设备在线 `192.168.5.32`，**bench 模式**；
- 配置：`segment=900 / silence=5000 / gain=3 / sd_freq=20000 / bench=true / sleep=false /
  vad=12dB / sound_active_low=true / max_record=1800`；
- 硬件：**SD 模块 VCC 已改接 5V**（关键修复）；LM393 电位器已重调，实测低有效；
- 后端：运行在 `192.168.5.36:8000`，本轮已接收 16 个文件（存储目录里都是本轮真实录音，可保留）；
- 复核命令：
  ```bash
  cd /home/mr4/esp32
  curl -s http://192.168.5.32/api/status | python3 -m json.tool
  curl -s http://127.0.0.1:8000/api/uploads | python3 -m json.tool
  tail -50 logs/serial9.log        # 本轮端到端串口日志
  ```

### 本轮改动清单（git）

```
5a83f50 fix: 静音停录改为连续 3 帧人声才刷新说话时刻
c53b80c fix: 修复唤醒后不录音、WAV 头校验误判、上传 duplicate 日志误报
```
新增 `tools/`（serlog / sd_sweep / rec_test / wav_check / cfg_set）与 `artifacts/`（本轮录音样本）。

---

## 12. 参考

- 需求：`hardware-device.md`、`backend-server.md`
- 接线：`wire.md`
- 后端协议：`docs/backend-api.md`
- 网页用法：`docs/web-ui.md`
- 验证报告（含证据与修复清单）：`docs/verification-report.md`
- 快速上手：`README.md`
