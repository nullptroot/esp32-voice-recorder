"""录音豆后端 · 转写流水线

- 调用 FunASR 容器（HTTP）把 WAV 转成文本；
- 串行处理，避免 2 核机器被并发打满；
- 失败自动重试（次数可配），转写结果存 SQLite；
- 相邻分段合并时做“边界去重”，避免 15 分钟切段处重复诵读。
"""
from __future__ import annotations

import difflib
import os
import threading
import time
import traceback
from pathlib import Path
from typing import Any

import httpx

from . import config, db

STATE_LOCK = threading.Lock()
STATE: dict[str, Any] = {
    "running": False,
    "queue": 0,
    "current": None,
    "last_error": None,
    "last_finished_at": None,
    "total_done": 0,
    "total_failed": 0,
}
_WAKE = threading.Event()
_WORKER: threading.Thread | None = None


def audio_abs_path(stored_path: str) -> Path:
    return Path(os.environ.get("AUDIO_DIR", "/data/storage/audio")) / stored_path


def wav_duration(path: Path) -> float | None:
    """解析 WAV 头得到时长（不依赖音频库）。"""
    try:
        import wave

        with wave.open(str(path), "rb") as w:
            n = w.getnframes()
            sr = w.getframerate() or 16000
            return round(n / sr, 2)
    except Exception:  # noqa: BLE001
        try:
            import soundfile as sf

            info = sf.info(str(path))
            return round(info.frames / info.samplerate, 2)
        except Exception:  # noqa: BLE001
            return None


def fix_wav_header(path: Path) -> bool:
    """设备异常断电可能留下 data size=0 的头，按实际文件大小修正。"""
    try:
        with open(path, "r+b") as f:
            head = f.read(44)
            if len(head) < 44 or head[:4] != b"RIFF":
                return False
            import struct

            data_size = struct.unpack("<I", head[40:44])[0]
            actual = path.stat().st_size - 44
            if data_size == actual:
                return True
            f.seek(4)
            f.write(struct.pack("<I", 36 + actual))
            f.seek(40)
            f.write(struct.pack("<I", actual))
            return True
    except Exception:  # noqa: BLE001
        return False


def asr_health() -> dict[str, Any]:
    url = (config.get("asr", "base_url") or "").rstrip("/")
    try:
        with httpx.Client(timeout=10) as cli:
            r = cli.get(f"{url}/health")
            r.raise_for_status()
            d = r.json()
            d["reachable"] = True
            return d
    except Exception as e:  # noqa: BLE001
        return {"reachable": False, "error": f"{type(e).__name__}: {e}"}


def call_asr(wav: Path, hotword: str = "") -> dict[str, Any]:
    url = (config.get("asr", "base_url") or "").rstrip("/")
    timeout = float(config.get("asr", "timeout") or 900)
    batch = int(config.get("asr", "batch_size_s") or 300)
    with open(wav, "rb") as f:
        files = {"file": (wav.name, f, "audio/wav")}
        data = {"hotword": hotword or "", "batch_size_s": str(batch)}
        with httpx.Client(timeout=timeout) as cli:
            r = cli.post(f"{url}/transcribe", files=files, data=data)
    if r.status_code >= 400:
        raise RuntimeError(f"ASR HTTP {r.status_code}: {r.text[:400]}")
    return r.json()


def transcribe_upload(upload: dict[str, Any], force: bool = False) -> dict[str, Any]:
    uid = upload["id"]
    if not force and upload.get("transcribe_status") == "done":
        return {"status": "skipped", "reason": "already done"}
    path = audio_abs_path(upload["stored_path"])
    if not path.exists():
        db.set_transcribe(uid, "failed", f"文件不存在: {path}", bump=True)
        return {"status": "failed", "error": "file missing"}

    if not upload.get("duration_s"):
        fix_wav_header(path)
        dur = wav_duration(path)
        if dur:
            db.set_duration(uid, dur)
    else:
        dur = upload.get("duration_s")

    db.set_transcribe(uid, "running", None, bump=True)
    t0 = time.time()
    with STATE_LOCK:
        STATE["current"] = upload.get("filename")
    try:
        res = call_asr(path, config.get("asr", "hotword") or "")
        text = (res.get("text") or "").strip()
        warnings: list[str] = []
        if not upload.get("time_trusted"):
            warnings.append("时间不精确（使用服务器接收时间推算日期）")
        if not text:
            warnings.append("未识别到语音内容")
        db.upsert_transcript(
            {
                "upload_id": uid,
                "device_id": upload.get("device_id"),
                "day": upload.get("day"),
                "engine": res.get("engine") or config.get("asr", "engine_name"),
                "text": text,
                "segments": res.get("segments") or [],
                "duration_s": res.get("duration") or dur,
                "elapsed_s": res.get("elapsed") or round(time.time() - t0, 2),
                "warnings": warnings,
            }
        )
        if res.get("duration"):
            db.set_duration(uid, res["duration"])
        db.set_transcribe(uid, "done", None if text else "未识别到语音内容")
        with STATE_LOCK:
            STATE["total_done"] += 1
            STATE["last_finished_at"] = db.now_iso()
            STATE["current"] = None
        return {"status": "done", "chars": len(text), "elapsed": round(time.time() - t0, 2)}
    except Exception as e:  # noqa: BLE001
        traceback.print_exc()
        err = f"{type(e).__name__}: {e}"
        max_retry = int(config.get("asr", "max_retry") or 3)
        attempts = int(upload.get("transcribe_attempts") or 0) + 1
        status = "failed" if attempts >= max_retry else "pending"
        db.set_transcribe(uid, status, err)
        with STATE_LOCK:
            STATE["total_failed"] += 1
            STATE["last_error"] = err
            STATE["current"] = None
        return {"status": "failed", "error": err, "attempts": attempts}
    finally:
        with STATE_LOCK:
            STATE["current"] = None


def merge_day_text(items: list[dict[str, Any]]) -> str:
    """把一天的转写按时间排序拼接，并在分段时间重叠处去重。"""
    out: list[str] = []
    prev_by_session: dict[str, str] = {}
    for it in items:
        text = " ".join((it.get("text") or "").split())
        if not text:
            continue
        sess = it.get("session_id") or ""
        prev_text = prev_by_session.get(sess, "")
        if prev_text:
            text = _dedup_boundary(prev_text, text)
        if text:
            ts = (it.get("effective_at") or it.get("received_at") or "")[11:16]
            flag = "" if it.get("time_trusted") else "（时间不精确）"
            out.append(f"[{ts}{flag}] {text}")
            prev_by_session[sess] = text
    return "\n".join(out)


def _dedup_boundary(prev: str, cur: str) -> str:
    """去掉 cur 开头与 prev 结尾重复的部分（录音分段边界重叠）。"""
    tail = prev[-40:]
    head = cur[:40]
    sm = difflib.SequenceMatcher(None, tail, head)
    m = sm.find_longest_match(0, len(tail), 0, len(head))
    if m.size >= 8:
        return cur[m.size:]
    return cur


# ------------------------------------------------------------------ worker
def start_worker() -> None:
    global _WORKER
    if _WORKER and _WORKER.is_alive():
        return
    _WORKER = threading.Thread(target=_loop, name="transcribe-worker", daemon=True)
    _WORKER.start()


def kick() -> None:
    _WAKE.set()


def _loop() -> None:
    time.sleep(5)
    while True:
        try:
            max_retry = int(config.get("asr", "max_retry") or 3)
            pending = db.uploads_pending_transcribe(limit=20, max_retry=max_retry)
            with STATE_LOCK:
                STATE["queue"] = len(pending)
            if not pending:
                _WAKE.wait(timeout=15)
                _WAKE.clear()
                continue
            if not config.get("asr", "auto_transcribe", default=True):
                _WAKE.wait(timeout=30)
                _WAKE.clear()
                continue
            with STATE_LOCK:
                STATE["running"] = True
            for up in pending:
                if not config.get("asr", "auto_transcribe", default=True):
                    break
                transcribe_upload(up)
        except Exception:  # noqa: BLE001
            traceback.print_exc()
            time.sleep(10)
        finally:
            with STATE_LOCK:
                STATE["running"] = False


def state() -> dict[str, Any]:
    with STATE_LOCK:
        d = dict(STATE)
    d["asr"] = asr_health()
    return d
