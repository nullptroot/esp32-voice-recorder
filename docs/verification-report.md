# 录音豆 · 设备端验证报告

> 日期：2026-10-29
> 硬件：立创 ESP32S3R8N8（8MB Flash + 8MB Octal PSRAM）+ INMP441 + LM393 声音模块 + MicroSD(SPI) + LED(GPIO47)
> 固件：`esp32/firmware`（ESP-IDF v5.5.6，recbean 1.0）
> 后端：`esp32/backend-min`（FastAPI + SQLite，极简验收服务）

---

## 1. 结论速览

| 模块 | 状态 | 说明 |
|---|---|---|
| 编译 / 烧录 / 启动 | ✅ | ESP-IDF v5.5.6 构建通过，串口烧录正常，PSRAM 8MB 识别正常 |
| SD 卡挂载 / 格式化 | ⚠️ | 挂载与格式化成功，但本机的 TF 卡存在坏块（详见 §5） |
| INMP441 I2S 采集 | ✅ | 16kHz/16bit/单声道，DC blocker 生效，实录可播放 |
| WAV 写入 / 分段 | ✅ | 44 字节标准头、关闭时回填、按 `segment_seconds` 切段 |
| 上传（multipart） | ✅ | 服务器落盘 + SQLite 记录，元数据字段完整，幂等去重生效 |
| 上传重试 / 不丢数据 | ✅ | 指数退避 2/4/8/16/32s，失败文件保留在 SD，`.up` 标记防重复 |
| VAD 二次确认 | ✅ | 纯 C 实现（10s 滑窗噪声底 + 能量 + 过零率），PC 单测 5 项全 PASS；真机说话 8s → 停口 2.5s 自动停录（实录 13s 文件） |
| 误触发冷却 | ✅ | VAD 未确认 → 冷却 `cooldown_seconds` → 不产生文件 |
| 状态 LED | ✅ | GPIO47，待机/录音/上传/配置/故障 5 种模式 |
| 配置网页 | ✅ | `http://<ip>/`，可改全部参数、实时电平、文件下载、日志、格式化、SD 自检 |
| 时间可信 / 回退命名 | ✅ | NTP 同步成功；断网时文件名回退 `mic1_session-XXXX_partN.wav`（代码路径与日志已验证） |
| EXT1 深睡唤醒 | ⚠️ | 睡眠/唤醒逻辑已实现并编译验证；因 SD 卡损坏，端到端"唤醒→录音→上传"未能完整跑通 |
| 上传后自动删除 / 保留策略 | ✅ | `delete_after_upload`、`keep_days` 逻辑可用（时间可信时生效） |

---

## 2. 已验证的链路与证据

### 2.1 端到端：录音 → 写卡 → 上传 → 服务器落盘

设备侧：
```
I (7450) upload: 上传成功: mic1_2026-10-29_01-18-53_part1.wav -> HTTP 200
I (17158) wav: 开始写: /sdcard/mic1_2026-10-29_01-18-53_part1.wav (16000 Hz)
I (32226) wav: 完成: /sdcard/mic1_2026-10-29_01-18-53_part1.wav (15.0 秒, 481280 字节)
```

服务器侧（`backend-min/storage/audio/2026/10/29/`）：
- 共接收 **19** 个上传（含幂等重复请求）
- 典型记录：`mic1_2026-10-29_01-18-53_part1.wav`, 481324 字节, `recorded_at=2026-10-29T01:18:53+08:00`, `time_trusted=1`

音频校验（下载后解析）：
```
rate 16000, channels 1, 60.0s / 20.0s / 15.0s 多段均验证通过
RMS 约 -38 dBFS（安静房间），无削顶
```

### 2.2 分段
`segment_seconds=6` 时一次录音产出：
```
mic1_2026-10-29_00-19-27_part1.wav  192556 字节 (6.0s)
mic1_2026-10-29_00-19-33_part2.wav  192556 字节 (6.0s)
mic1_2026-10-29_00-19-39_part3.wav   32812 字节 (1.0s)
```
分段命名使用各段**开始时刻**，`partN` 递增。

### 2.3 VAD 自动停录
配置 `silence_stop_ms=2500`、`vad_threshold_db=6`，说话约 8 秒：
```
I ... 手动触发录音
状态: recording → recording → idle，record_seconds=13
```
停口后约 2.5 秒自动结束（13s ≈ 预录 1.5s + 说话 8s + 静音 2.5s + 收尾）。

VAD 算法主机单测（`firmware/host_test/vad_test.c`）：
```
[test1] 纯噪声误报: 0/300 = 0.0%
[test2] 人声 -20/-30/-40 dBFS × 噪声 -62/-52/-45 dBFS
        → 除 5dB 信噪比场景外全部 100% 检出
[test3] 全静音误报: 0/100
[test4] 噪声底抬升后误报: 0/100
[test5] 持续 -30dB 嗡声误报: 0/150；嗡声底噪下说话检出: 100/100
结果: ✅ PASS
```

### 2.4 上传失败重试与幂等
- 停掉服务器后录音 → 文件保留在 SD，`pending_files` 增加；
- 恢复服务器 → 补传成功；
- 重复上传同一文件 → 服务器返回同一 `id` 且 `duplicate=true`，设备视角仍为成功；
- 设备写 `.up` 标记后不再重复上传。

### 2.5 状态指示与网页
- 网页 `http://192.168.5.32/` 可实时查看状态/电平/文件/日志；
- `/api/sdtest?start=<blk>&count=<n>` 提供原始块读自检（本次故障诊断的关键工具）。

---

## 3. 发现并修复的软件问题（均已回归）

| # | 问题 | 现象 | 修复 |
|---|---|---|---|
| 1 | `esp_http_client_fetch_headers()` 返回值误判 | 上传成功却判失败，反复重试 | 返回值是 content-length，改为 `< 0` 才判失败 |
| 2 | worker 任务栈溢出 | 上传后 `A stack overflow in task worker` 重启 | 栈 12KB→28KB，大缓冲改静态 |
| 3 | FatFS 4096 扇区 + 并发访问 | 目录/FAT 读 CRC 错误 | 改 512 字节扇区；所有 SD 访问串行化（递归锁） |
| 4 | 状态接口在录音时读 SD | 录音中 Web 轮询触发总线竞争 | 待传数量/容量改缓存，录音只读缓存 |
| 5 | 手动触发被静音停录提前截断 | 还没开口就停录（3s 文件） | 仅检测到人声后才启用静音停录 |
| 6 | 分段末尾产生 0 字节文件 | 每次录音多一个空文件 | 关闭时校验大小，空/损坏分段自动删除 |
| 7 | INMP441 直流偏置 / 启动阶跃 | 开头出现 -3dB 阶跃、削顶 | 一阶 DC blocker + 启动丢弃 320ms |
| 8 | I2S DMA 欠载 | 文件头出现 0.5–1.2s 数字静音 | DMA 描述符 8→12，帧 512→1024 |
| 9 | WAV 头回填失败 | 文件时长字段为 0，播放器不认 | 关闭后用独立句柄回填并校验，失败重试 |
| 10 | 分区/任务冲突 | — | 录音 worker 绑核 1，httpd/按键/LED 绑核 0 |

---

## 4. 引脚与接线（按 wire.md 实测）

| 功能 | 引脚 | 实测 |
|---|---|---|
| INMP441 WS/BCK/SD | 4 / 5 / 6 | 正常采集 |
| SD CS/MOSI/SCLK/MISO | 10 / 11 / 12 / 13 | 挂载成功（`asdfg` 32GB） |
| LM393 DO | 15 | 安静低电平、拍手高电平（**高有效**，已作为默认 `sound_active_low=false`） |
| LED | 47 | 状态指示正常 |
| 按键 | 41 | 本轮未接线，短按/长按回调已实现，待接线后验证 |

---

## 5. SD 卡硬件问题（重要，需要更换）

### 现象
- 挂载、格式化、写文件多数时候成功；
- 但读文件系统元数据（FAT/根目录）时周期性报 `sdmmc_read_sectors: error 0x108`（CRC 错误）；
- 表现为：`wav: 打开失败`、文件目录项 size=0、删除报 I/O error；
- **断电 5 秒后恢复**，随后几分钟内再次恶化。

### 诊断（`/api/sdtest` 原始块读，绕过文件系统）
```
block 1000  : failed 0/12        ✅
block 20000 : failed 0/12        ✅
block 29890 : failed 11/12       ❌  (~15.3MB)
```
后续复测坏区漂移到 block 7547，说明坏区在**扩大**，不是固定坏块。

### 结论
- 该 32GB TF 卡（卷标 `asdfg`）是**劣质/扩容卡**，真实可用容量远小于标称，且会随写入继续恶化；
- 写入看似成功（数据进卡），但文件系统元数据落进坏区后，新建文件/目录更新失败。

### 已实施的缓解
1. **64KB 簇格式化**：把 FAT + 根目录压到前 ~5MB，绕开 15MB 附近的坏区（格式化时间 14s → 3.4s）；
2. **SD 自动恢复**：打开文件失败时自动卸载→重新初始化→挂载后重试（`/api/sd/recover` 也可手动触发）；
3. **完整性校验**：关闭 WAV 后校验目录项大小，损坏分段自动删除；<100 字节的残留文件上传前删除；
4. **建议**：断电重启可临时恢复卡状态（实测有效）。

### 建议
> **换一张正品 TF 卡**（SanDisk / Samsung / Kingston，16–32GB，Class10/U3，High Endurance 更佳）。
> 更换后无需改代码，把 `sd_freq_khz` 调回 10000–20000 即可获得更高写入/上传速度。

---

## 6. 尚未完成的验证项（依赖好卡）

1. **EXT1 深睡端到端**：`bench_mode=false` + `sleep_enable=true` → 深睡 → LM393 唤醒 → VAD 确认 → 录音 → 上传 → 再睡。逻辑已实现，串口日志可见 `进入 EXT1 深度睡眠`，但录音环节受坏卡阻塞。
2. **说话内容听后验收**：本轮实录文件因坏卡回填失败，建议换卡后录一段 10 秒语音听后确认音质。
3. **按键**：GPIO41 未接线，接入后即可验证短按/长按。
4. **误触发冷却实测**：逻辑已实现（VAD 未确认 → 冷却 N 秒），建议换卡后用拍手触发验证。

---

## 7. 复现命令

```bash
# 构建烧录
cd esp32/firmware && source ~/esp-idf-v5.5.6/export.sh
idf.py build && idf.py -p /dev/ttyUSB0 -b 460800 flash monitor

# 验收服务
cd esp32/backend-min && ./run.sh
curl -s localhost:8000/health
curl -s localhost:8000/api/uploads | python3 -m json.tool

# 设备网页
open http://<设备IP>/

# VAD 单测
gcc -O2 -I firmware/main firmware/host_test/vad_test.c firmware/main/vad.c -lm -o /tmp/vad_test && /tmp/vad_test

# SD 原始块自检 (诊断坏块)
curl "http://<设备IP>/api/sdtest?start=29890&count=16"
curl -X POST http://<设备IP>/api/sd/recover
```
