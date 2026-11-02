# 录音豆后端 · 验收报告（2026-11-02）

环境：腾讯云 `<SERVER_IP>`（2 vCPU / 3.6GB / 无 GPU），Ubuntu 24.04，Docker 29.6。
执行时间：2026-11-02 22:36 ~ 23:20。

---

## 1. FunASR 可用性（单独验证）

镜像 `recbean-asr:1.0`，模型经 ModelScope 下载（1.2GB）：

| 模型 | 用途 |
|---|---|
| `speech_paraformer-large_asr_nat-zh-cn-16k-common-vocab8404-pytorch` (840MB) | 中文 ASR |
| `speech_fsmn_vad_zh-cn-16k-common-pytorch` (278MB) | VAD 切句 |
| `punc_ct-transformer_zh-cn-common-vocab272727-pytorch` (278MB) | 标点恢复 |

**加载耗时**：15.9s；**常驻内存**：2.32 → 2.82GB（加载+推理后）。

| 测试音频 | 时长 | 识别结果 | 耗时 | RTF |
|---|---|---|---|---|
| asr_example.wav | 5.55s | `欢迎大家来体验达摩院推出的语音识别型儿。` | 4.28s | 0.772 |
| paraformer_example2.wav | 13.05s | `正是因为存在绝对正义，所以我们接受现实的相对正义，但是不要因为现实的相对正义，我们就认为这个世界没有正义。因为如果当你认为这个世界没有正义。` | 1.84s | 0.141 |

> 结论：识别准确（两个样本均为人可读的正确中文，含准确标点），速度在 2 核 CPU 上 RTF 0.14~0.77，**15 分钟录音约需 2~12 分钟**。

## 2. 设备协议兼容（backend-api.md）

用 `scripts/sim_device.py` 精确复刻固件行为（file 字段最前、显式 Content-Length、11 个字段、`X-Device-Id`、20s 超时）：

| 验收项 | 判据 | 结果 |
|---|---|---|
| `/health` | 200 + `status:ok` | ✅ |
| `/upload` 上传 | 2xx + 可解析 JSON | ✅ 27~60ms |
| 幂等 | 重复上传同一 id + `duplicate:true` | ✅ |
| 时间可信 | `time_trusted=false` → `recorded_at=NULL` | ✅ |
| 日期推算 | 用 `received_at` 得到 `day` | ✅ |
| 落盘 | `storage/audio/YYYY/MM/DD/<uuid>_<filename>` | ✅ |

## 3. 转写流水线

- 上传返回后**异步**转写，不占用设备 20s 超时；
- 失败自动重试（默认 3 次，可配）；
- 单文件串行处理（2 核机器避免打满）；
- 相邻分段边界去重（difflib 最长公共子串，阈值 8 字）；
- 结果落 `transcripts` 表 + 原始 WAV 可按 id 播放。

## 4. 大模型总结

- 接口：OpenAI 兼容 `/chat/completions`，本次用腾讯 TokenHub `deepseek-v4.1-flash-1017`；
- 输出结构化 Markdown：今日概述 / 时间线（`**HH:MM**`）/ 想法与决定 / 待办与承诺 / 亮点摘录；
- 生成耗时 3.8~7.0s（131 字符输入）；
- 支持长文本分块（>12000 字先分段摘要再汇总）；
- 时间不精确片段带 `⚠️时间不精确` 标记并提示模型。
- 兼容 `reasoning_content` 推理字段、`/vN` 版本段 Base URL。

## 5. 推送

| 渠道 | 验证方式 | 结果 |
|---|---|---|
| Bark | 真实发送 | ✅ `{"code":200,"message":"success"}` |
| ntfy | 真实发送（含中文标题） | ✅ `{"event":"message","title":"🎙 录音豆 · 推送测试"}` |
| Server酱 | 域名可达 + 协议实现 | ⏳ 待填 sendkey |
| PushPlus | 域名可达 + 协议实现 | ⏳ 待填 token |
| 钉钉 | 域名可达 + HMAC 加签实现 | ⏳ 待填 webhook |
| 飞书 | 域名可达 + HMAC 加签实现（卡片+文本兜底） | ⏳ 待填 webhook |
| 企业微信 | 域名可达 + markdown 实现 | ⏳ 待填 webhook |

## 6. 配置 Web

7 个页面全部可用；API Key 脱敏（回显 `******`、回写不覆盖真实值）已验证；
保存即生效、重启后配置与数据均保留（实测）。

## 7. 全链路自动化验证（最终一次，干净数据库）

```
✅ 1. GET /health — uploads=0
✅ 2. POST /upload（file 最前 + 11 字段） — 0.028s
✅ 3. 幂等（重复上传同一 id）
✅ 4. 时间不可信（recorded_at=NULL，用接收时间推算日期）
✅ 5. FunASR 转写 — 20 字
✅ 6. 大模型生成每日总结 — 5.8s
✅ 7. 多渠道推送 — {"bark": true}
结果: 7/7 通过
```

## 8. 未完成 / 阻塞

| 项 | 说明 |
|---|---|
| **公网可达性** | `curl http://<SERVER_IP>:8010/health` **超时**，需腾讯云安全组放行 TCP 8010 |
| 设备真机联调 | 依赖上一条，放行后即可用真机验证 |
| 5 个推送渠道实发 | 需要你的 key/token/webhook |
