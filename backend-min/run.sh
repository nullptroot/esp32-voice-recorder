#!/usr/bin/env bash
# 启动极简验收服务
set -e
cd "$(dirname "$0")"
if [ ! -x .venv-backend/bin/python ]; then
  echo "[*] 创建虚拟环境..."
  python3 -m venv .venv-backend
  ./.venv-backend/bin/pip install -q -r requirements.txt
fi
HOST="${HOST:-0.0.0.0}"
PORT="${PORT:-8000}"
echo "[*] 监听 http://${HOST}:${PORT}  (存储目录: $(pwd)/storage)"
exec ./.venv-backend/bin/python -m uvicorn app:app --host "$HOST" --port "$PORT" --log-level info
