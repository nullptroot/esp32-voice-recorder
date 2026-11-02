#!/usr/bin/env bash
# 录音豆后端 · 一键部署（国内服务器友好）
#   ./deploy/start.sh          构建（如需要）并启动全部服务
#   ./deploy/start.sh --rebuild 强制重建镜像
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)
COMPOSE="docker compose -f deploy/docker-compose.yml"

echo "==> 工作目录: $ROOT"
mkdir -p data/models data/storage/audio data/storage/journal

if ! docker image inspect recbean-backend:1.0 >/dev/null 2>&1 || [[ "${1:-}" == "--rebuild" ]]; then
  echo "==> 构建后端镜像"
  docker build -f deploy/Dockerfile.backend -t recbean-backend:1.0 .
fi
if ! docker image inspect recbean-asr:1.0 >/dev/null 2>&1 || [[ "${1:-}" == "--rebuild" ]]; then
  echo "==> 构建 ASR 镜像（FunASR + CPU torch，首次约 5~10 分钟）"
  docker build -f deploy/Dockerfile.asr -t recbean-asr:1.0 deploy/
fi

echo "==> 启动服务"
$COMPOSE up -d --remove-orphans

echo "==> 等待后端健康检查"
for i in $(seq 1 40); do
  if curl -fsS -m 3 http://127.0.0.1:8010/health >/dev/null 2>&1; then
    echo "✅ 后端就绪: http://127.0.0.1:8010/health"
    break
  fi
  sleep 3
done

echo
echo "管理后台:  http://<公网IP>:8010/admin   (默认 admin / recbean，请首登后修改)"
echo "设备上传:  http://<公网IP>:8010/upload  (设备网页 server_url 填 http://<公网IP>:8010)"
echo "状态查看:  docker logs -f recbean-backend"
echo "ASR 日志:  docker logs -f recbean-asr  (首次启动下载模型，约 3~8 分钟)"
