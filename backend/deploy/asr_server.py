#!/usr/bin/env python3
"""
录音豆 · FunASR 转写服务（容器内运行）

对外 HTTP 接口（只在内网 docker 网络暴露，主服务调用）：
  GET  /health        -> {"status": "ok", "model": "...", "loaded": true}
  POST /transcribe    -> multipart: file=<wav> [hotword=<str>]
                         返回 {"text": "...", "segments": [...], "engine": "...", "elapsed": 1.2}

设计要点：
  - 模型懒加载（首次调用或启动预加载），避免容器启动即占 2GB 内存；
  - 长音频（15 分钟分段）按配置的 chunk 切分，防止内存峰值；
  - 输出带时间戳的 segments，便于前端/总结按时间线组织。
"""
from __future__ import annotations

import os
import sys
import time
import json
import re
import tempfile
import threading
import traceback
from pathlib import Path

import numpy as np
from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import JSONResponse

MODEL_PARAFORMER = os.environ.get(
    "ASR_MODEL_PARAFORMER",
    "iic/speech_paraformer-large_asr_nat-zh-cn-16k-common-vocab8404-pytorch",
)
MODEL_VAD = os.environ.get("ASR_MODEL_VAD", "iic/speech_fsmn_vad_zh-cn-16k-common-pytorch")
MODEL_PUNC = os.environ.get("ASR_MODEL_PUNC", "iic/punc_ct-transformer_zh-cn-common-vocab272727-pytorch")
ENGINE_NAME = os.environ.get("ASR_ENGINE_NAME", "funasr-paraformer-zh")
DEVICE = os.environ.get("ASR_DEVICE", "cpu")
PRELOAD = os.environ.get("ASR_PRELOAD", "1") == "1"
THREADS = int(os.environ.get("ASR_NUM_THREADS", "2"))

MODELSCOPE_CACHE = os.environ.get("MODELSCOPE_CACHE", "/models/ms-cache")
os.environ.setdefault("MODELSCOPE_CACHE", MODELSCOPE_CACHE)
os.environ.setdefault("FUNASR_HOME", "/models/.funasr")

app = FastAPI(title="recbean-funasr", version="1.0.0")

_model = None
_model_lock = threading.Lock()
_load_error: str | None = None
_infer_lock = threading.Lock()  # FunASR cpu 推理串行，避免 2 核打满抖动


def _resolve_model(name: str) -> str:
    """支持本地缓存目录（ModelScope 快照）或模型 id。"""
    p = Path(MODELSCOPE_CACHE) / "models" / name.replace("/", "--")
    # modelscope 1.x 缓存结构：<cache>/models/<org>/<name> 或 <org>--<name>
    for cand in (
        Path(MODELSCOPE_CACHE) / name,
        Path(MODELSCOPE_CACHE) / "hub" / name,
        Path(MODELSCOPE_CACHE) / "hub" / (name.replace("/", "--")),
        p,
    ):
        if cand.is_dir():
            return str(cand)
    return name  # 回退到模型 id，由 funasr 自行解析（需联网）


def get_model():
    global _model, _load_error
    if _model is not None:
        return _model
    with _model_lock:
        if _model is not None:
            return _model
        t0 = time.time()
        try:
            from funasr import AutoModel

            kwargs = dict(
                model=_resolve_model(MODEL_PARAFORMER),
                vad_model=_resolve_model(MODEL_VAD),
                punc_model=_resolve_model(MODEL_PUNC),
                device=DEVICE,
                disable_update=True,
                log_level="WARNING",
            )
            # FunASR 1.2.x 用 ncpu 控制 intra-op 线程
            try:
                _model = AutoModel(ncpu=THREADS, **kwargs)
            except TypeError:
                _model = AutoModel(**kwargs)
            _load_error = None
            print(f"[asr] model loaded in {time.time()-t0:.1f}s", flush=True)
        except Exception as e:  # noqa: BLE001
            _load_error = f"{type(e).__name__}: {e}"
            traceback.print_exc()
            raise
    return _model


def _load_audio(path: str) -> np.ndarray:
    import soundfile as sf

    data, sr = sf.read(path, dtype="float32", always_2d=True)
    mono = data.mean(axis=1)
    if sr != 16000:
        # 线性插值重采样（录音豆固定 16k，这里是兜底）
        n = int(len(mono) * 16000 / sr)
        idx = np.linspace(0, len(mono) - 1, n, dtype=np.float64)
        mono = np.interp(idx, np.arange(len(mono)), mono).astype(np.float32)
    return mono


_PUNC = re.compile(r"[。！？!?；;，,、]")


def _clean(text: str) -> str:
    return re.sub(r"<\|[^|]*\|>", "", text or "").strip()


@app.get("/health")
async def health():
    return JSONResponse(
        {
            "status": "ok",
            "engine": ENGINE_NAME,
            "loaded": _model is not None,
            "load_error": _load_error,
            "device": DEVICE,
            "models": {"asr": MODEL_PARAFORMER, "vad": MODEL_VAD, "punc": MODEL_PUNC},
        }
    )


@app.post("/preload")
async def preload():
    try:
        get_model()
        return {"status": "ok", "loaded": True}
    except Exception as e:  # noqa: BLE001
        raise HTTPException(500, str(e))


@app.post("/transcribe")
async def transcribe(
    file: UploadFile = File(...),
    hotword: str = Form(""),
    batch_size_s: int = Form(300),
):
    t0 = time.time()
    suffix = Path(file.filename or "a.wav").suffix or ".wav"
    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as tmp:
        while True:
            chunk = await file.read(1 << 20)
            if not chunk:
                break
            tmp.write(chunk)
        tmp_path = tmp.name

    try:
        audio = _load_audio(tmp_path)
        duration = len(audio) / 16000.0
        if duration < 0.2:
            return {
                "text": "",
                "segments": [],
                "engine": ENGINE_NAME,
                "duration": round(duration, 2),
                "elapsed": round(time.time() - t0, 2),
                "warning": "audio too short",
            }
        model = get_model()
        kwargs = dict(
            input=audio,
            batch_size_s=max(60, int(batch_size_s)),
            hotword=hotword or None,
            return_raw_text=False,
        )
        with _infer_lock:
            res = model.generate(**kwargs)
        items = res if isinstance(res, list) else [res]
        texts, segments = [], []
        for it in items:
            txt = _clean(it.get("text", ""))
            if txt:
                texts.append(txt)
            for s in it.get("sentence_info", []) or []:
                st = s.get("start", 0) / 1000.0
                et = s.get("end", 0) / 1000.0
                segments.append(
                    {
                        "start": round(st, 2),
                        "end": round(et, 2),
                        "text": _clean(s.get("text", "")),
                    }
                )
        text = "".join(texts).strip()
        elapsed = time.time() - t0
        rtf = round(elapsed / duration, 3) if duration > 0 else None
        print(
            f"[asr] {file.filename} dur={duration:.1f}s elapsed={elapsed:.1f}s rtf={rtf} chars={len(text)}",
            flush=True,
        )
        return {
            "text": text,
            "segments": segments,
            "engine": ENGINE_NAME,
            "duration": round(duration, 2),
            "elapsed": round(elapsed, 2),
            "rtf": rtf,
        }
    except HTTPException:
        raise
    except Exception as e:  # noqa: BLE001
        traceback.print_exc()
        raise HTTPException(500, f"{type(e).__name__}: {e}")
    finally:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass


@app.post("/transcribe_json")
async def transcribe_json(payload: dict):
    """接收 {"path": "/audio/..."} —— 主服务与 ASR 共享挂载卷时可免二次上传。"""
    p = (payload or {}).get("path")
    if not p or not os.path.isfile(p):
        raise HTTPException(400, f"file not found: {p}")
    with open(p, "rb") as f:
        data = f.read()

    class _U:
        filename = os.path.basename(p)

        async def read(self, n=-1):
            nonlocal _pos
            chunk = data[_pos : _pos + (n if n > 0 else len(data))]
            _pos += len(chunk)
            return chunk

    _pos = 0
    return await transcribe(
        file=_U(),
        hotword=payload.get("hotword", ""),
        batch_size_s=int(payload.get("batch_size_s", 300)),
    )


def _preload_bg():
    try:
        get_model()
    except Exception as e:  # noqa: BLE001
        print(f"[asr] preload failed: {e}", flush=True)


if __name__ == "__main__":
    import uvicorn

    port = int(os.environ.get("ASR_PORT", "10095"))
    if PRELOAD:
        threading.Thread(target=_preload_bg, daemon=True).start()
    uvicorn.run(app, host="0.0.0.0", port=port, log_level="info", timeout_keep_alive=120)
