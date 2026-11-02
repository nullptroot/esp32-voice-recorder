#!/bin/bash
# 启动前确保模型可用（首次启动从 ModelScope 下载，挂载到 /models 持久化）
set -euo pipefail

MODEL_PARAFORMER="${ASR_MODEL_PARAFORMER:-iic/speech_paraformer-large_asr_nat-zh-cn-16k-common-vocab8404-pytorch}"
MODEL_VAD="${ASR_MODEL_VAD:-iic/speech_fsmn_vad_zh-cn-16k-common-pytorch}"
MODEL_PUNC="${ASR_MODEL_PUNC:-iic/punc_ct-transformer_zh-cn-common-vocab272727-pytorch}"
MODELS_ROOT="${ASR_MODEL_DIR:-/models}"

export MODELSCOPE_CACHE="${MODELS_ROOT}/ms-cache"
export FUNASR_HOME="${MODELS_ROOT}/.funasr"

echo "[entry] MODELSCOPE_CACHE=$MODELSCOPE_CACHE"

if [ "${SKIP_MODEL_DOWNLOAD:-0}" != "1" ]; then
python - <<'PY'
import os, sys
from modelscope.hub.snapshot_download import snapshot_download
root = os.environ.get("MODELSCOPE_CACHE", "/models/ms-cache")
models = [
    os.environ.get("ASR_MODEL_PARAFORMER", "iic/speech_paraformer-large_asr_nat-zh-cn-16k-common-vocab8404-pytorch"),
    os.environ.get("ASR_MODEL_VAD", "iic/speech_fsmn_vad_zh-cn-16k-common-pytorch"),
    os.environ.get("ASR_MODEL_PUNC", "iic/punc_ct-transformer_zh-cn-common-vocab272727-pytorch"),
]
for m in models:
    print(f"[entry] ensure model: {m}", flush=True)
    p = snapshot_download(m, cache_dir=root)
    print(f"[entry]   -> {p}", flush=True)
PY
fi

exec python /srv/asr_server.py
