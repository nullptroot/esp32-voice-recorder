# 敏感信息规范（上传 GitHub 前必读）

本仓库**只放代码与模板**，任何真实凭据、服务器地址、录音数据都不入库。

## 1. 不入库的内容（已在 .gitignore 中排除）

| 路径 | 内容 | 说明 |
|---|---|---|
| `data/` | `config.json`、`storage/audio/*`、`storage/journal/*`、`recbean.db`、`models/` | 含**明文 API Key / 推送 Key**、真实录音、转写与日记、1.2GB 模型 |

`data/config.json` 权限 600，里面有：
- `llm.api_key`（大模型 Key）
- `push.channels.*` 里各渠道的 key / sendkey / token / webhook / secret
- `web.password`（后台登录密码）

> ⚠️ 如果哪天误把 `data/` 提交了，**删掉文件不等于安全**，必须同时**作废并重新生成所有 Key**。

## 2. 部署时怎么配置

两种方式，任选：

1. **（推荐）Web 配置**：启动后打开 `/admin`，在对应页面填写，保存即写入 `data/config.json`。
2. **环境变量初始化**（首次部署）：`deploy/docker-compose.yml` 里取消注释 `LLM_BASE_URL` / `LLM_MODEL` / `LLM_API_KEY` / `WEB_PASSWORD` 等。

配置项全貌见 `config.example.json`（值为空或默认值，可直接提交）。

## 3. 代码里必须保持的约定

- 前端回显敏感字段时显示 `******`（`config.public_config()` 负责脱敏）；
- 回写 `******` 时视为"未修改"，不能覆盖真实值（`config._unmask()`）；
- 日志/异常信息里不要把 Key 打出来；
- 文档里只写"已在 Web 中配置"，不写具体 Key。

## 4. 上传前自检命令

```bash
cd backend

# 1) 确认 data/ 等敏感目录没被跟踪（只允许 data/samples/ 里的官方样例）
git ls-files | grep -E "^(data|.*\.db|.*\.json$)" && echo "⚠️ 有敏感文件被跟踪" || echo "✅ 无敏感文件被跟踪"

# 2) 扫描已跟踪文件里的常见密钥形态
git grep -nP -- "sk-[A-Za-z0-9]{12,}|ghp_|github_pat_|-----BEGIN|\d{1,3}(\.\d{1,3}){3}:[0-9]{4}" 

# 3) 扫描提交历史（关键：删了文件历史里还在）
git log --all -p | grep -nP "sk-[A-Za-z0-9]{12,}|ghp_|github_pat_|-----BEGIN"
```

三条都干净才可以 push。

## 5. 本轮已做的脱敏（2026-11-02）

| 项目 | 处理 |
|---|---|
| Bark Device Key | 从 `docs/handover-2026-11-02.md` 移除，改为"已在 Web 中配置" |
| 服务器公网 IP | 全部替换为 `<SERVER_IP>` 占位符（md / html） |
| 录音、转写、日记、数据库 | 只存在于 `data/`，已被 gitignore（`data/` 下仅 `samples/` 的两个官方示例音频入库） |
| 新增 | `config.example.json`（全空默认值）、本文件 |
