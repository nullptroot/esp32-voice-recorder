## 2. 接收服务

用 FastAPI 自己实现即可，不需要现成框架。

```text
backend/
  app/
    main.py          # FastAPI 入口
    upload.py        # POST /upload
    transcribe.py    # FunASR / faster-whisper / whisper.cpp 调用
    summarize.py     # Ollama 调用
    storage/
      audio/         # 原始 WAV
      transcripts/   # 转写文本 JSON/TXT
      journal/       # 日记 Markdown
  scripts/
    daily_job.py     # 按日聚合 + 生成日记
```

最小 API：

- `POST /upload`：接收 WAV，落盘，写入 SQLite 任务表。
- `GET /health`：健康检查。
- 可选 `POST /jobs/transcribe`：手动触发某日转写。
- 可选 `POST /jobs/summarize`：手动触发某日日记生成。
- 可选 `POST /jobs/cleanup`：按保留策略清理原始音频。

### 2.1 上传接口契约

设备端发送：

```http
POST /upload
Content-Type: multipart/form-data

file: mic1_2026-10-26_10-00-00_part1.wav
device_id: esp32s3-001
recorded_at: 2026-10-26T10:00:00+08:00
time_trusted: true
session_id: session-0007
sequence: 1
```

时间不可信时：

```http
file: mic1_session-0007_part1.wav
device_id: esp32s3-001
recorded_at: null
time_trusted: false
session_id: session-0007
sequence: 1
monotonic_ms: 12345678
```

后端返回：

```json
{ "id": "uuid", "status": "stored" }
```

### 2.2 SQLite 任务表建议字段

```text
uploads:
  id, device_id, filename, stored_path, recorded_at, time_trusted,
  session_id, sequence, monotonic_ms, received_at, status

transcribe_jobs:
  id, upload_id, engine, status, transcript_path, error, retry_count, created_at, updated_at

journal_jobs:
  id, date, status, journal_path, error, retry_count, created_at, updated_at
```

---

## 3. 语音转写：中文优先的选型修正

### 3.1 选型结论

原方案将 `whisper.cpp` 和 `faster-whisper` 并列为“两条 CPU 可行路线”，并建议先用 faster-whisper 跑通。评审后修正为：

- **中文优先候选：FunASR（SenseVoice-Small 或 Paraformer）**。
- **备选：faster-whisper**，Python 集成方便，但中文准确率不如 FunASR。
- **轻量/多语言备选：whisper.cpp**，中文 CER 普遍偏高，不再作为中文正式路线。
- **实时流式转写** `WhisperLiveKit` 当前不需要，不作为必需。

### 3.2 数据依据与实测建议

社区实测数据（CPU，中文）显示：

- `whisper.cpp` 中文 CER 普遍在 22–31%；
- `faster-whisper small` 中文 CER 约 16.2%，`medium` 约 14.9%，但 `medium` 在 CPU 上 RTF 约 2.15，处理 1 小时音频需要约 2 小时；
- FunASR 的 `SenseVoice-Small` 中文 CER 约 7.81%，速度约 17.2× 实时；`Paraformer` CER 约 10.18%，速度约 20.9× 实时。

因此建议：

1. 在正式接入前，用实际录音环境的 3–5 段 1 分钟音频做 **FunASR vs faster-whisper small** 对比；
2. 人工计算 CER 或主观评估可读性；
3. 如果 FunASR 明显更好，直接作为主转写引擎；
4. 避免后续日记 Prompt 针对 whisper 的转写错误做无谓调优。

### 3.3 工程封装建议

- `transcribe.py` 抽象统一接口：
  ```python
  def transcribe(wav_path: str, engine: str = "funasr") -> dict:
      ...
  ```
- 输出统一 JSON：
  ```json
  {
    "engine": "funasr-sensevoice-small",
    "text": "...",
    "segments": [],
    "time_trusted": true,
    "warnings": []
  }
  ```
- 相邻 15 分钟文件转写时，头尾各重叠 1–2 秒，或在聚合阶段由 LLM 合并边界不完整句子。
- 转写失败进入重试队列，不阻塞其他文件。

---

## 4. 日记生成：Ollama + 本地 LLM

- 运行时：`Ollama`，提供本地 LLM 接口。
- 模型建议：`qwen3:8b` 或同级别 8B 模型，16GB 内存普通电脑可运行。
- 调用方式：`POST http://localhost:11434/api/generate`。

按日聚合转写文本后，传入 Prompt：

```text
你是一个个人日记助手。以下是我今天的语音记录转写，按时间顺序排列。
请以第一人称写一篇日记，包含：
1. 今天的主要活动（按时间线，2-4 句）
2. 值得记住的想法或决定（如有，列出）
3. 待办或承诺（如有，列出）
如果某些片段信息量很低，可以合并或省略。不要编造转写中没有的内容。
如果转写中存在“时间不精确”的标记，请在日记中注明时间可能不准确。
如果相邻片段在边界处句子不完整，请尝试合并理解，但不要编造。

转写：
{transcript}
```

输出写入：

```text
backend/storage/journal/YYYY-MM-DD.md
```

### 4.1 数据保留与隐私

- 原始 WAV：转写完成后保留 N 天（例如 7–30 天），由 `POST /jobs/cleanup` 或定时任务删除。
- 转写文本：可长期保留，便于追溯。
- 日记 Markdown：长期保留。
- 提供手动删除某日原始音频和转写文本的接口。
- 日记中标注“时间不精确”来源，避免误导。

---

## 5. 后端参考项目与真实性核对

| 项目 / 仓库名 | 状态 | 用途与注意点 |
|---|---|---|
| `ggerganov/whisper.cpp` | ✅ 存在 | C/C++ 纯 CPU Whisper 推理，轻量，但中文准确率短板明确，不作为中文正式路线。 |
| `litongjava/whisper-cpp-server` | ✅ 存在，但维护活跃度较低 | 最后推送约一年前，星标 62。只作接口参考，生产使用需自行评估稳定性。 |
| `SYSTRAN/faster-whisper` | ✅ 存在 | 基于 CTranslate2，CPU 可用，Python 集成方便。中文准确率可用但不如 FunASR。 |
| `andyjjrt/faster-whisper-server` / `fedirz/faster-whisper-server` | ✅ 存在 | 提供 OpenAI 兼容 `/v1/audio/transcriptions`，基于 FastAPI + faster-whisper，便于切换后端。 |
| FunASR / SenseVoice / Paraformer | ✅ 存在且活跃 | 中文 CER 和 CPU 速度优于 whisper 系，建议作为中文优先候选，需在验证阶段实测。 |
| `WhisperLiveKit` | ✅ 存在 | 实时流式转写，当前不需要，未来实时场景参考。 |
| `Ollama` | ✅ 存在 | 本地 LLM 运行时，`qwen3:8b` 等模型本地推理。 |
| `voxmd` | ✅ 存在 | PyPI 有 `voxmd`，GitHub 为 `rvnztolentino/voxmd`，whisper + Ollama + Obsidian Markdown。偏成品工具，借鉴 Prompt 设计可以，集成需自己重写。 |
| `RecallMate` | ✅ 存在，但有 GPU 依赖偏差 | 仓库 `AWT-SRIRAM/RECALLMATE` 存在，使用 faster-whisper + Ollama + `qwen2.5:7b`。但 faster-whisper 配置是 CUDA + float16，CPU 回退只是兜底，不是 CPU 批处理优化参考。 |
| `solstone` | ✅ 存在 | `solpbc/solstone` 做实体提取和知识图谱，属于重型方案，首版不引入。 |

**后端结论**：参考项目真实存在，没有虚构。主要修正点是中文转写选型、`RecallMate` 的 GPU 倾向、`whisper-cpp-server` 维护状态，以及 15 分钟边界处理。

---

## 6. 后端验证顺序

1. **接收链路**：FastAPI `/upload` 接收文件，确认落盘、SQLite 任务表写入、返回 200。
2. **重试与幂等**：模拟重复上传、上传中断，确认不丢文件、不重复转写。
3. **转写对比**：用 3–5 段实际录音，分别跑 FunASR 和 faster-whisper small，对比中文 CER/可读性和 CPU 耗时。
4. **边界处理**：对相邻 15 分钟文件做头尾重叠转写，确认边界句子合并效果。
5. **时间不精确链路**：模拟 `time_trusted=false`，确认后端推算日期并在转写/日记中标记。
6. **日记生成**：按日聚合转写文本，调用 Ollama 生成 Markdown 日记，检查是否编造内容。
7. **数据保留**：验证 `POST /jobs/cleanup` 按保留策略删除原始 WAV，转写文本和日记保留。
8. **全链路自动化**：定时任务转写和生成日记，失败可重试、可查询。

---

## 7. 后端主要风险与对策

| 风险 | 对策 |
|---|---|
| 中文转写准确率不足 | 优先测试 FunASR SenseVoice/Paraformer；faster-whisper 作备选；whisper.cpp 不作中文正式路线 |
| CPU 转写太慢 | 批处理而非实时；使用较小模型；FunASR 速度优先；必要时再考虑 GPU |
| 15 分钟边界句子断裂 | 相邻文件头尾重叠 1–2 秒；LLM 聚合时合并不完整句子 |
| LLM 日记编造内容 | Prompt 明确“不要编造转写中没有的内容”；保留转写原文可追溯；时间不精确时注明 |
| 时间不可信导致日期错乱 | 后端根据接收时间或单调时钟推算日期；任务和日记标记“时间不精确” |
| 原始音频隐私风险 | 转写后保留 N 天自动删除；提供手动清理接口；日记和转写文本可追溯 |
| 任务失败丢数据 | SQLite 任务表 + 重试 + 状态查询；上传成功前设备端不删除本地文件 |
| 参考项目维护风险 | `whisper-cpp-server` 只作接口参考；`RecallMate` 不作为 CPU 优化参考；核心逻辑自己拼接最小系统 |

---

## 8. 后端当前结论

- 接收服务：FastAPI + SQLite 任务表，最小 API 跑通上传、转写、日记、清理。
- 转写路线：**中文优先 FunASR（SenseVoice-Small / Paraformer）**，faster-whisper 备选，whisper.cpp 仅轻量/多语言备选。
- 日记生成：Ollama + `qwen3:8b`，Prompt 明确不编造，时间不精确时注明。
- 数据处理：原始 WAV 保留 N 天后自动删除，转写文本和日记长期保留，提供手动清理。
- 所有关键环节都有真实开源项目可参考，但建议每段只借鉴核心逻辑，自己拼接最小系统。
```

---

以上两个文档已经互相引用，接口契约保持一致。你可以直接保存为 `hardware-device.md` 和 `backend-server.md` 使用。