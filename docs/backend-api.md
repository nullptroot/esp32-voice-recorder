# 设备端 → 后端 接口契约（完整版）

> 适用固件：`esp32/firmware`（recbean 1.0）
> 适用验收服务：`esp32/backend-min/app.py`
> 后端正式实现（转写 / 日记 / 清理）不在本轮验收范围，本文件给出**设备端已经固定下来的全部协议细节**，后端按此实现即可兼容，无需改固件。

---

## 0. 总览

| 方法 | 路径 | 必须 | 说明 |
|---|---|---|---|
| `POST` | `/upload` | ✅ | 上传单个 WAV 分段 + 元数据（multipart/form-data） |
| `GET` | `/health` | ✅ | 健康检查（设备网页的「服务器检查」按钮、上传前置检查） |
| `GET` | `/api/uploads` | 可选 | 列出已接收文件（验收/排障） |
| `GET` | `/api/uploads/{id}` | 可选 | 单条上传详情 |
| `GET` | `/` | 可选 | 验收用浏览页面 |
| `POST` | `/jobs/transcribe` | 后续 | 见 §8（本轮不实现） |
| `POST` | `/jobs/summarize` | 后续 | 见 §8（本轮不实现） |
| `POST` | `/jobs/cleanup` | 后续 | 见 §8（本轮不实现） |

设备端只用前两个接口即可跑通完整验收链路，其余为后端自用/运维接口。

**传输层约定**

- 纯 HTTP/1.1，**不使用 chunked 编码**，设备端在 `Content-Length` 中给出精确长度；
- 设备端单次请求超时 20 s，服务器应在 20 s 内返回；
- 服务器地址在设备网页中配置为 `server_url`（例：`http://192.168.5.36:8000`），设备会自动拼接 `/upload`、`/health`；
- 设备端**不做 TLS 校验**（可配置 https，但测试网络里是明文 http），正式部署建议在内网或加反向代理；
- 无鉴权头要求；设备附带 `X-Device-Id` 头（与 form 字段 `device_id` 相同），如后端要鉴权可基于它做白名单。

---

## 1. POST /upload

### 1.1 请求

```http
POST /upload HTTP/1.1
Host: 192.168.5.36:8000
Content-Type: multipart/form-data; boundary=----RecBeanBoundary7MA4YWxkTrZu0gW
X-Device-Id: esp32s3-001
Content-Length: <精确长度>

------RecBeanBoundary7MA4YWxkTrZu0gW
Content-Disposition: form-data; name="file"; filename="mic1_2026-10-26_10-00-00_part1.wav"
Content-Type: audio/wav

<WAV 二进制>
------RecBeanBoundary7MA4YWxkTrZu0gW
Content-Disposition: form-data; name="device_id"

esp32s3-001
------RecBeanBoundary7MA4YWxkTrZu0gW
Content-Disposition: form-data; name="recorded_at"

2026-10-26T10:00:00+08:00
------RecBeanBoundary7MA4YWxkTrZu0gW
Content-Disposition: form-data; name="time_trusted"

true
------RecBeanBoundary7MA4YWxkTrZu0gW
Content-Disposition: form-data; name="session_id"

session-0007
------RecBeanBoundary7MA4YWxkTrZu0gW
Content-Disposition: form-data; name="sequence"

1
------RecBeanBoundary7MA4YWxkTrZu0gW
Content-Disposition: form-data; name="monotonic_ms"

12345678
------RecBeanBoundary7MA4YWxkTrZu0gW--
```

> 注意：**`file` 字段永远排在最前面**，其后才是文本字段，然后才是结束 boundary。后端解析器（含 `python-multipart`）都能处理任意顺序，但若自己写流式解析请按此顺序设计。

### 1.2 字段定义

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `file` | file | ✅ | WAV 文件，见 §3 音频格式。文件名规则见 §4 |
| `device_id` | string | ✅ | 设备标识，默认 `esp32s3-001`，网页可改；用于幂等/归属 |
| `recorded_at` | string \| 空 | ✅ | ISO-8601 带时区，如 `2026-10-26T10:00:00+08:00`。**时间不可信时为空字符串**（见 §5） |
| `time_trusted` | `"true"` \| `"false"` | ✅ | NTP 是否已同步。为 `false` 时 `recorded_at` 必为空 |
| `session_id` | string | ✅ | 会话编号，形如 `session-0007`（每轮唤醒/每次录音递增） |
| `sequence` | int | ✅ | 该会话内的分段序号，从 1 开始 |
| `monotonic_ms` | int | 可选 | 设备上电后的单调毫秒数（`esp_timer`），用于时间不可信时排序 |

### 1.3 成功响应

```http
HTTP/1.1 200 OK
Content-Type: application/json

{
  "id": "4f0b5c0e-6a3a-4a53-9d6c-1b0f5f7c2a11",
  "status": "stored",
  "duplicate": false,
  "stored_path": "2026/10/26/4f0b..._mic1_2026-10-26_10-00-00_part1.wav",
  "sha256": "9f2c...",
  "time_trusted": true
}
```

**设备端判定规则**：HTTP 状态码 `2xx` **且** 响应体可读 → 视为成功。设备不解析 `id`/`status` 的具体值（`duplicate` 仅用于日志），随后写 `<文件名>.up` 标记文件，因此：

- `status` 字段必须是 `stored`（或后端自定义的等价成功值亦可，设备不校验）；
- `duplicate: true` 请务必使用与首次上传**相同**的 `id`，便于后端去重与设备端重试一致。

### 1.4 失败响应与重试语义

| 情况 | 应返回 | 设备行为 |
|---|---|---|
| 服务器未启动 / 断网 | — | 指数退避重试，单文件最多 `upload_max_retry` 次（默认 5），退避 2s→4s→8s→16s→32s（上限 60s） |
| `4xx`（请求格式错） | 400 + `{"detail": "..."}` | 视为失败并重试；**如果格式错是固件 bug，会一直重试**，后端请先 `200` 落盘再异步校验，不要用 400 拒绝合法文件 |
| `5xx` / 超时 | 500 | 重试，失败文件保留在 SD 卡，下次开机/按键/录音后自动补传 |
| 重复上传 | 200 + 同一 `id` | 设备视为成功，写 `.up` 标记 |

设备端保证：**上传成功前绝不删除本地文件**。失败文件会一直留在 SD 卡，直到上传成功或按保留策略被清理。

### 1.5 幂等建议

- 幂等键：`(device_id, sha256(file))`（推荐，验收服务即如此实现）；
- 备用键：`(device_id, filename, size_bytes)`；
- 重复请求必须返回**同一 id**，且不得产生第二份落盘文件。

---

## 2. GET /health

```http
GET /health HTTP/1.1
```

```json
{ "status": "ok", "time": "2026-10-26T10:00:00+08:00", "uploads": 42 }
```

- HTTP 200 即视为健康；设备网页「服务器检查」按钮展示该结果；
- 建议实现为轻量查询（不要在此做转写）。

---

## 3. 音频格式

| 项 | 值 |
|---|---|
| 容器 | WAV (RIFF) |
| 编码 | PCM 16-bit 小端 |
| 采样率 | 16000 Hz（网页可改 8000/16000/32000） |
| 声道 | 1（单声道） |
| 分段 | 默认 900 s（15 分钟），网页可改（验收建议 20–60 s） |
| 单文件大小 | ≈ 112.5 MB/小时 @16k/16bit/mono |
| 头 | 标准 44 字节；设备先写占位头，关闭时回填 `RIFF` 与 `data` 长度 |

后端校验建议：`RIFF`/`WAVE` 魔数 + `fmt` 块 16 字节 PCM + `data` 块长度与文件大小一致。设备在异常断电时可能留下**头部长度未回填**的文件（`data` size = 0），后端可用文件实际大小减去 44 字节兜底修正。

---

## 4. 文件命名与元数据规则

### 4.1 时间可信

```text
mic1_YYYY-MM-DD_HH-MM-SS_partN.wav
例: mic1_2026-10-26_10-00-00_part1.wav
```

- 时间取该分段**开始时刻**（本地时区 Asia/Shanghai，`+08:00`）；
- `N` 从 1 开始，同一会话内递增；
- 同一会话的分段 = 一个连续录音（默认 15 分钟切一段），后端聚合时应对相邻 `partN`/`partN+1` 做头尾重叠或 LLM 合并。

### 4.2 时间不可信

```text
mic1_session-0007_part1.wav
```

- `session-XXXX` 来自 NVS 持久计数器，每次录音循环 +1，重启不清零；
- 顺序由 `sequence` 与 `monotonic_ms` 保证；
- 后端应使用 `received_at`（服务器接收时间）推算日期，并在转写/日记中标记「时间不精确」。

### 4.3 设备端伴随文件（不上传，仅本地）

| 文件 | 内容 |
|---|---|
| `<name>.wav.json` | 该分段的元数据（与上传字段一致） |
| `<name>.wav.up` | 上传成功标记（含服务器响应原文），有它即不再重复上传 |

后端不需要理解这两个后缀，但**如果后端将来支持设备「查询已上传」接口，可按 `.up` 语义设计**。

---

## 5. 时间可信度语义

| `time_trusted` | `recorded_at` | 后端处理 |
|---|---|---|
| `true` | 非空 ISO-8601 | 直接采用；仍建议与 `received_at` 做偏差校验（设备可能错时区） |
| `false` | `""`（空串） | 用 `received_at` 推算日期；在转写/日记标记「时间不精确」 |

设备端逻辑：
1. 开机连上 WiFi 后做一次 SNTP（`ntp.aliyun.com`，超时 10 s）；
2. 深睡期间 RTC 保留系统时间，唤醒后一般无需重新对时；
3. 对时失败 → `time_trusted=false`，文件名回退为 `session-XXXX`；
4. 网页「时间」徽标实时显示当前处于哪种状态。

---

## 6. SQLite 表结构（验收服务已实现，正式后端可沿用）

```sql
CREATE TABLE uploads (
    id            TEXT PRIMARY KEY,      -- uuid
    device_id     TEXT NOT NULL,
    filename      TEXT NOT NULL,
    stored_path   TEXT NOT NULL,         -- 相对 storage/audio 的路径
    size_bytes    INTEGER NOT NULL,
    sha256        TEXT NOT NULL,
    recorded_at   TEXT,                  -- 时间不可信时为 NULL
    time_trusted  INTEGER NOT NULL DEFAULT 1,
    session_id    TEXT,
    sequence      INTEGER,
    monotonic_ms  INTEGER,
    received_at   TEXT NOT NULL,
    client_ip     TEXT,
    status        TEXT NOT NULL DEFAULT 'stored',
    wav_valid     INTEGER NOT NULL DEFAULT 1
);
CREATE UNIQUE INDEX idx_dedup ON uploads(device_id, sha256);
```

后续需要补的表（转写/日记）：

```sql
CREATE TABLE transcribe_jobs (
    id TEXT PRIMARY KEY, upload_id TEXT, engine TEXT, status TEXT,
    transcript_path TEXT, error TEXT, retry_count INTEGER,
    created_at TEXT, updated_at TEXT
);
CREATE TABLE journal_jobs (
    id TEXT PRIMARY KEY, date TEXT, status TEXT, journal_path TEXT,
    error TEXT, retry_count INTEGER, created_at TEXT, updated_at TEXT
);
```

---

## 7. 验收流程（与设备端一一对应）

```bash
# 1) 启动极简服务
cd esp32/backend-min
./run.sh                       # 监听 0.0.0.0:8000

# 2) 健康检查
curl -s http://127.0.0.1:8000/health

# 3) 设备端: 浏览器打开 http://<设备IP>/ → 点「开始录音」→ 说话 10s → 「停止」
#    或点「立即上传」把 SD 卡里积压的文件补传

# 4) 查看服务器落盘
curl -s http://127.0.0.1:8000/api/uploads | python3 -m json.tool
ls -lh backend-min/storage/audio/*/*/*/

# 5) 幂等验证: 重复上传同一文件, 应返回同一 id 且 duplicate=true
```

设备端验收点：

| # | 验收项 | 通过判据 |
|---|---|---|
| 1 | 录音链路 | 网页状态 `recording`，SD 卡出现 `mic1_*_part1.wav`，可播放、时长正确 |
| 2 | 分段 | 设 `segment_seconds=20`，长时间录音产生 `part1/2/3...`，每段约 20 s |
| 3 | 上传 | 服务器 `storage/audio/YYYY/MM/DD/` 出现文件，`/api/uploads` 有记录 |
| 4 | 重试 | 停掉服务器再录，文件留在 SD；恢复服务器后「立即上传」补传成功 |
| 5 | 时间回退 | 关掉 NTP（断网/改错时间）→ 文件名 `mic1_session-XXXX_partN.wav`，`time_trusted=false` |
| 6 | 幂等 | 重复上传不产生第二份文件 |
| 7 | 深睡 | `bench_mode=false` + `sleep_enable=true` → 录音/上传后进深睡，LM393 声音可唤醒 |

---

## 8. 后续后端接口（本轮不实现，仅冻结契约）

### POST /jobs/transcribe

```json
{ "date": "2026-10-26", "engine": "funasr-sensevoice-small" }
```
```json
{ "queued": 12, "status": "running" }
```

### POST /jobs/summarize

```json
{ "date": "2026-10-26", "model": "qwen3:8b" }
```
```json
{ "date": "2026-10-26", "journal_path": "storage/journal/2026-10-26.md" }
```

### POST /jobs/cleanup

```json
{ "retention_days": 7 }
```
```json
{ "deleted_files": 12, "freed_bytes": 134217728 }
```

---

## 9. 设备端已实现、后端需要注意的行为清单

1. **上传顺序是「文件在前、字段在后」**；
2. **`Content-Length` 精确**，不吃 chunked；
3. 单文件最多重试 5 次，退避 2/4/8/16/32 s（上限 60 s），失败后整批停止，等待下次触发；
4. 上传成功后写 `.up` 标记，不会重复上传；除非 `delete_after_upload=true` 才删除本地文件；
5. `keep_days` 清理只在时间可信时执行；
6. 录音期间 WiFi 关闭（bench 模式例外），因此后端**不要依赖设备长期在线**，也不要做长连接/服务端主动推送；
7. 设备不做音频转码，上传即原始 WAV；
8. 15 分钟分段边界可能有句子断裂，契约上由 `sequence` 保证顺序，由后端做重叠合并。
