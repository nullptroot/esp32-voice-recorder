# 录音豆 · 后端（转写 / 总结 / 推送 / 配置 Web）

ESP32-S3「录音豆」的后端正式实现，部署在一台带 Docker 的国内云服务器（`<SERVER_IP>`）。
代码位于本仓库 `backend/`，固件位于 `firmware/`。

```
设备 ──POST /upload──▶ 后端(8010) ──▶ SQLite + 原始 WAV
                          │
                          ├─▶ FunASR 容器(10095) 本地转写（paraformer-zh + VAD + 标点）
                          ├─▶ 大模型 OpenAI 兼容 API 每日总结（DeepSeek / 通义 / Kimi / 智谱 / Ollama …）
                          └─▶ 推送：Bark · Server酱 · PushPlus · ntfy · 钉钉 · 飞书 · 企业微信
```

- 设备协议严格遵循 [`../docs/backend-api.md`](../docs/backend-api.md)，**固件无需改动**。
- 全部配置可在 Web 上改，改完即时生效（无需重启）。
- **不含任何真实凭据**：配置项全貌见 [`config.example.json`](config.example.json)，
  部署安全规范见 [`SECURITY.md`](SECURITY.md)（`data/` 已被 gitignore）。
- 国内网络适配：Docker Hub 走国内镜像、pip 走清华/腾讯镜像、模型走 ModelScope、前端零 CDN 依赖。

---

## 1. 快速开始

```bash
cd /opt/recbean
./deploy/start.sh            # 构建 + 启动（首次构建 ASR 镜像约 5~10 分钟）
```

| 入口 | 地址 |
|---|---|
| 管理后台 | `http://<SERVER_IP>:8010/admin`（默认 `admin` / `recbean`，**请立即修改**） |
| 设备上传 | `http://<SERVER_IP>:8010/upload`（设备网页 `server_url` 填 `http://<SERVER_IP>:8010`） |
| 健康检查 | `http://<SERVER_IP>:8010/health` |
| API 文档 | `http://<SERVER_IP>:8010/api/docs` |

> ⚠️ 需要在腾讯云控制台 **安全组放行 TCP 8010**，否则公网访问不到（本机 `curl` 不受影响）。

## 2. 配置指引

### 2.1 大模型（Web → 大模型总结）
填三项即可，支持任何 OpenAI 兼容服务：

| 服务 | Base URL | 模型示例 |
|---|---|---|
| DeepSeek（推荐，便宜好用） | `https://api.deepseek.com/v1` | `deepseek-chat` |
| 通义千问（DashScope 兼容模式） | `https://dashscope.aliyuncs.com/compatible-mode/v1` | `qwen-plus` |
| Kimi | `https://api.moonshot.cn/v1` | `moonshot-v1-8k` |
| 智谱 GLM | `https://open.bigmodel.cn/api/paas/v4` | `glm-4-flash`（免费） |
| OpenAI | `https://api.openai.com/v1` | `gpt-4o-mini` |
| 本机 Ollama | `http://host.docker.internal:11434/v1` | `qwen2.5:7b` |

填完点「用当前表单测试连通」→ 通过后「保存」。API Key 保存后显示为 `******`，不会回显明文。

### 2.2 推送渠道（Web → 推送）
每个渠道独立开关，可同时启用多个，支持单独「发送测试」。

| 渠道 | 需要填 | 获取方式 |
|---|---|---|
| **Bark**（iOS） | Device Key | iPhone 装 Bark → 复制链接里的 key；自建填自己服务地址 |
| **Server 酱** | SendKey（`sctp…`） | https://sct.ftqq.com/ |
| **PushPlus** | Token | https://www.pushplus.plus/ |
| **ntfy** | Topic（+可选 token） | https://ntfy.sh/ ，App 订阅同名 topic |
| **钉钉机器人** | Webhook（+可选加签 Secret） | 群设置 → 群机器人 → 自定义 |
| **飞书机器人** | Webhook（+可选签名校验 Secret） | 群设置 → 群机器人 → 自定义 |
| **企业微信** | Webhook | 群设置 → 群机器人添加 |

「公开访问地址」填 `http://<SERVER_IP>:8010`，推送里的链接可直接在手机上打开日记。

### 2.3 每日总结（Web → 大模型总结 → 每日自动总结）
- 默认每天 **22:00** 自动生成"当天"日记并推送；
- 可改时间、可改成总结"前一天"；
- 当天没有录音则跳过；已有日记则只推送不重生成；
- 也可在「手动生成」里选日期随时生成 / 生成并推送。

## 3. 目录结构

```
backend/
├── app/                    # 后端主服务（FastAPI）
│   ├── main.py             # 设备接口 + 管理 API + 调度器
│   ├── config.py           # 配置读写（JSON，Key 脱敏）
│   ├── db.py               # SQLite（uploads / transcripts / journals / push_logs）
│   ├── transcribe.py       # FunASR 调用 + 队列 + 失败重试
│   ├── llm.py              # OpenAI 兼容调用 + 日记 Prompt
│   ├── push.py             # 7 个推送渠道
│   └── templates/admin.html# 管理后台（单文件，无外部依赖）
├── deploy/
│   ├── Dockerfile.backend  # 后端镜像
│   ├── Dockerfile.asr      # FunASR 镜像（cpu torch + funasr）
│   ├── asr_server.py       # FunASR HTTP 服务
│   ├── asr_entrypoint.sh   # 启动前从 ModelScope 拉模型
│   ├── docker-compose.yml
│   └── start.sh
├── scripts/
│   ├── sim_device.py       # 设备上传模拟器（按固件协议精确复刻）
│   └── e2e_test.py         # 全链路验证
├── data/                   # 运行数据（挂载卷，不入镜像）
│   ├── config.json         # 业务配置（含 Key，权限 600）——不入库
│   ├── samples/            # 公开示例音频（FunASR/ModelScope 官方样例）——入库
│   ├── storage/audio/      # 原始 WAV（YYYY/MM/DD/uuid_文件名.wav）——不入库
│   ├── storage/journal/    # 日记 Markdown + JSON——不入库
│   ├── storage/recbean.db  # SQLite——不入库
│   └── models/             # FunASR 模型缓存（ModelScope，约 1.2GB）——不入库
└── docs/                   # 部署文档、进度、交接、验收报告
```

## 3.1 配置从哪来

首次部署任选其一：

1. **Web 配置（推荐）**：打开 `/admin` 逐页填写，保存即写入 `data/config.json`；
2. **环境变量初始化**：在 `deploy/docker-compose.yml` 取消注释 `LLM_BASE_URL` / `LLM_MODEL` / `LLM_API_KEY` / `WEB_PASSWORD`；
3. 复制模板手动改：`cp config.example.json data/config.json` 后编辑。

> `data/config.json` 含明文 Key（权限 600），**已排除在 git 之外**，不要上传到任何代码托管平台。

## 4. 接口一览

### 设备接口（与固件契约一致）
| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/upload` | multipart：`file` 在**最前**，其后 `device_id` `recorded_at` `time_trusted` `session_id` `sequence` `monotonic_ms` `time_source`；幂等键 `(device_id, sha256)`；< 100ms 返回（转写异步） |
| GET | `/health` | `{"status":"ok","time":...,"uploads":N}` |
| GET | `/api/uploads` | 列表（`limit` `offset` `day` `transcribe_status` `device_id`） |
| GET | `/api/uploads/{id}` | 详情（含转写文本） |
| GET | `/api/audio/{id}` | 原始音频（浏览器直接播放） |
| POST | `/jobs/transcribe` | `{"date":"2026-11-02","force":false}` |
| POST | `/jobs/summarize` | `{"date":"2026-11-02","push":true}` |
| POST | `/jobs/cleanup` | `{"retention_days":7}` |

### 管理接口（Basic Auth）
`GET/PUT /api/config` · `GET /api/status` · `POST /api/asr/test` · `POST /api/asr/transcribe/{id}` · `POST /api/llm/test` · `POST /api/push/test/{channel}` · `GET /api/push/logs` · `GET /api/days` · `GET/POST /api/journal(s)` · `POST /api/summarize` · `POST /api/push/{date}` · `POST /api/cleanup`

## 5. 运维

```bash
cd /opt/recbean
docker compose -f deploy/docker-compose.yml ps          # 状态
docker logs -f recbean-backend                          # 后端日志（上传/转写/总结/推送）
docker logs -f recbean-asr                              # ASR 日志（模型下载、RTF）
docker compose -f deploy/docker-compose.yml restart     # 重启
docker compose -f deploy/docker-compose.yml down        # 停止
curl -s http://127.0.0.1:8010/health                    # 健康检查
```

数据都在 `data/` 下，删容器不丢数据。

## 6. 验证

```bash
python3 scripts/sim_device.py --server http://127.0.0.1:8010 \
    --wav data/samples/paraformer_example2.wav --repeat 2     # 上传 + 幂等
python3 scripts/e2e_test.py --server http://127.0.0.1:8010 \
    --wav data/samples/sensevoice_zh.mp3 \
    --wav2 data/samples/paraformer_example2.wav                          # 全链路
```

## 7. 常见问题

| 现象 | 处理 |
|---|---|
| `docker pull` 卡住 | 国内直连 Docker Hub 不通，构建脚本已默认用本地已有镜像；换基础镜像时加前缀 `docker.m.daocloud.io/library/` |
| ASR 日志 `ConnectionError` / 模型下载慢 | 首次启动需从 ModelScope 下 ~1GB 模型，2 核机器约 3~8 分钟，`/health` 里 `loaded` 变 true 即可用 |
| 转写很慢 | 2 核 CPU，15 分钟录音约 5~15 分钟转完；`batch_size_s` 调到 600 可略快但更吃内存 |
| 大模型 401/404 | 检查 Base URL 是否带 `/v1`、模型 ID 是否正确；用「用当前表单测试连通」定位 |
| 推送收不到 | 逐渠道点「发送测试」看返回；钉钉/飞书开了加签就必须填 Secret |
| 设备连不上服务器 | 安全组放行 8010；设备网页 `server_url` 填 `http://<SERVER_IP>:8010`（不带 `/upload`） |
