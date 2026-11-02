#!/usr/bin/env python3
"""录音豆后端 · 主服务 (FastAPI)

设备协议（严格按 docs/backend-api.md）：
  POST /upload      接收 WAV + 元数据，幂等（device_id + sha256）
  GET  /health      健康检查（轻量）
  GET  /api/uploads 文件列表（兼容验收服务）
后台功能：
  FunASR 转写 -> 大模型每日总结 -> 多渠道推送
管理端（Web）：
  /admin 配置页（ASR / 大模型 / 推送 / 调度 / 保留策略），含上传、转写、日记、推送测试
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import secrets
import shutil
import threading
import time
import traceback
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

from fastapi import Depends, FastAPI, File, Form, HTTPException, Request, UploadFile
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, PlainTextResponse
from fastapi.staticfiles import StaticFiles

from . import config, db, llm, push, transcribe

TZ = timezone(timedelta(hours=8))
DATA_DIR = Path(os.environ.get("DATA_DIR", "/data"))
STORAGE_DIR = Path(os.environ.get("STORAGE_DIR", str(DATA_DIR / "storage")))
AUDIO_DIR = Path(os.environ.get("AUDIO_DIR", str(STORAGE_DIR / "audio")))
JOURNAL_DIR = Path(os.environ.get("JOURNAL_DIR", str(STORAGE_DIR / "journal")))
UPLOAD_TOKEN = os.environ.get("UPLOAD_TOKEN", "").strip()

for d in (STORAGE_DIR, AUDIO_DIR, JOURNAL_DIR):
    d.mkdir(parents=True, exist_ok=True)

db.CURRENT_DB = Path(os.environ.get("DB_PATH", str(STORAGE_DIR / "recbean.db")))
db.init(db.CURRENT_DB)
config.load()

app = FastAPI(title="recbean-backend", version="1.0.0", docs_url="/api/docs", openapi_url="/api/openapi.json")


# ============================================================ 工具函数
def now_iso() -> str:
    return datetime.now(TZ).isoformat(timespec="seconds")


def safe_name(name: str) -> str:
    name = os.path.basename(name or "upload.wav")
    name = re.sub(r"[^A-Za-z0-9._-]", "_", name)
    return name[:120] or "upload.wav"


def parse_bool(v: Any, default: bool = True) -> bool:
    if v is None:
        return default
    return str(v).strip().lower() in ("1", "true", "yes", "on")


def parse_optional(v: Any) -> str | None:
    if v is None:
        return None
    s = str(v).strip()
    if s == "" or s.lower() in ("null", "none", "undefined"):
        return None
    return s


def to_day(iso: str | None) -> str | None:
    if not iso:
        return None
    try:
        dt = datetime.fromisoformat(iso.replace("Z", "+00:00"))
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=TZ)
        return dt.astimezone(TZ).strftime("%Y-%m-%d")
    except ValueError:
        return iso[:10]


def wav_check(head: bytes) -> bool:
    return len(head) >= 12 and head[0:4] == b"RIFF" and head[8:12] == b"WAVE"


def check_device_token(request: Request) -> None:
    if not config.get("device", "require_token", default=False):
        return
    allowed = config.get("device", "tokens", default=[]) or []
    tok = request.headers.get("X-Upload-Token") or ""
    if tok not in allowed:
        raise HTTPException(401, "设备上传 token 无效（X-Upload-Token）")


# ============================================================ 设备接口
@app.get("/health")
async def health() -> JSONResponse:
    """设备网页「服务器检查」用：保持轻量，不做转写查询。"""
    try:
        n = db.count_uploads()
    except Exception:  # noqa: BLE001
        n = -1
    return JSONResponse({"status": "ok", "time": now_iso(), "uploads": n, "service": "recbean"})


@app.post("/upload")
async def upload(
    request: Request,
    file: UploadFile = File(...),
    device_id: str = Form(...),
    recorded_at: str | None = Form(None),
    time_trusted: str | None = Form(None),
    session_id: str | None = Form(None),
    sequence: str | None = Form(None),
    monotonic_ms: str | None = Form(None),
    time_source: str | None = Form(None),
):
    check_device_token(request)
    # 设备端 20s 超时：先落盘返回，转写在后台进行（见 backend-api.md §1.4）
    filename = safe_name(file.filename or "upload.wav")
    tmp = AUDIO_DIR / f".{uuid.uuid4().hex}.part"
    sha = hashlib.sha256()
    size = 0
    head = b""
    t0 = time.time()

    with open(tmp, "wb") as out:
        while True:
            chunk = await file.read(1 << 20)
            if not chunk:
                break
            if len(head) < 12:
                head += chunk[: 12 - len(head)]
            out.write(chunk)
            sha.update(chunk)
            size += len(chunk)
    digest = sha.hexdigest()

    dup = db.find_duplicate(device_id, digest)
    if dup:
        tmp.unlink(missing_ok=True)
        return JSONResponse(
            {
                "id": dup["id"],
                "status": "stored",
                "duplicate": True,
                "stored_path": dup["stored_path"],
                "sha256": digest,
                "time_trusted": bool(dup["time_trusted"]),
            }
        )

    rid = str(uuid.uuid4())
    day_dir = datetime.now(TZ).strftime("%Y/%m/%d")
    rel = f"{day_dir}/{rid}_{filename}"
    dest = AUDIO_DIR / rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp.replace(dest)

    recorded_at_v = parse_optional(recorded_at)
    trusted = parse_bool(time_trusted) and recorded_at_v is not None
    if not trusted:
        recorded_at_v = None
    received = now_iso()
    # 时间不可信 -> 用服务器接收时间推算业务日期（backend-api.md §5）
    effective = recorded_at_v if trusted else received

    def _int(v: Any) -> int | None:
        try:
            return int(v) if v not in (None, "", "null") else None
        except (TypeError, ValueError):
            return None

    db.insert_upload(
        {
            "id": rid,
            "device_id": device_id,
            "filename": filename,
            "stored_path": rel,
            "size_bytes": size,
            "sha256": digest,
            "recorded_at": recorded_at_v,
            "time_trusted": 1 if trusted else 0,
            "session_id": parse_optional(session_id),
            "sequence": _int(sequence),
            "monotonic_ms": _int(monotonic_ms),
            "received_at": received,
            "client_ip": request.client.host if request.client else None,
            "status": "stored",
            "wav_valid": 1 if wav_check(head) else 0,
            "time_source": parse_optional(time_source),
            "effective_at": effective,
            "day": to_day(effective),
            "duration_s": None,
            "transcribe_status": "pending",
        }
    )
    try:
        transcribe.kick()
    except Exception:  # noqa: BLE001
        pass
    print(
        f"[upload] {device_id} {filename} {size/1024:.0f}KB trusted={trusted} "
        f"day={to_day(effective)} ok_in={time.time()-t0:.2f}s",
        flush=True,
    )
    return JSONResponse(
        {
            "id": rid,
            "status": "stored",
            "duplicate": False,
            "stored_path": rel,
            "sha256": digest,
            "time_trusted": trusted,
        }
    )


@app.get("/api/uploads")
async def list_uploads(limit: int = 50, offset: int = 0, day: str | None = None,
                       transcribe_status: str | None = None, device_id: str | None = None):
    limit = max(1, min(limit, 500))
    items = db.list_uploads(limit, offset, day, transcribe_status, device_id)
    return JSONResponse({"count": len(items), "total": db.count_uploads(), "items": items})


@app.get("/api/uploads/{uid}")
async def get_upload(uid: str):
    row = db.get_upload(uid)
    if not row:
        raise HTTPException(404, "not found")
    d = dict(row)
    t = db.get_transcript(uid)
    d["transcript"] = dict(t) if t else None
    return JSONResponse(d)


@app.get("/api/audio/{uid}")
async def get_audio(uid: str):
    row = db.get_upload(uid)
    if not row:
        raise HTTPException(404, "not found")
    p = transcribe.audio_abs_path(row["stored_path"])
    if not p.exists():
        raise HTTPException(404, "file missing")
    return FileResponse(str(p), media_type="audio/wav", filename=row["filename"])


# ============================================================ 任务接口（契约 §8）
@app.post("/jobs/transcribe")
async def job_transcribe(payload: dict | None = None):
    payload = payload or {}
    day = payload.get("date")
    if day:
        rows = db.list_uploads(limit=500, day=day)
    else:
        rows = db.uploads_pending_transcribe(limit=200, max_retry=999)
    queued = 0
    for r in rows:
        if r.get("transcribe_status") != "done" or payload.get("force"):
            db.set_transcribe(r["id"], "pending", None)
            queued += 1
    transcribe.kick()

    def _run():
        for r in rows:
            up = db.get_upload(r["id"])
            if up:
                transcribe.transcribe_upload(dict(up), force=bool(payload.get("force")))

    threading.Thread(target=_run, daemon=True).start()
    return {"queued": queued, "status": "running", "date": day}


@app.post("/jobs/summarize")
async def job_summarize(payload: dict | None = None):
    payload = payload or {}
    date = payload.get("date") or (datetime.now(TZ).date().isoformat())
    try:
        res = summarize_date(date, push_after=bool(payload.get("push", False)))
        return {
            "date": date,
            "journal_path": f"storage/journal/{date}.md",
            "markdown": res["markdown"],
            "model": res.get("model"),
            "push": res.get("push"),
        }
    except Exception as e:  # noqa: BLE001
        traceback.print_exc()
        raise HTTPException(500, f"{type(e).__name__}: {e}")


@app.post("/jobs/cleanup")
async def job_cleanup(payload: dict | None = None):
    payload = payload or {}
    days = int(payload.get("retention_days") or config.get("retention", "audio_days") or 30)
    return cleanup_audio(days)


def cleanup_audio(days: int) -> dict[str, Any]:
    cutoff = datetime.now(TZ) - timedelta(days=days)
    deleted, freed = 0, 0
    for up in db.list_uploads(limit=5000):
        p = transcribe.audio_abs_path(up["stored_path"])
        if not p.exists():
            continue
        try:
            ts = datetime.fromisoformat((up.get("effective_at") or up["received_at"]).replace("Z", "+00:00"))
        except ValueError:
            continue
        if ts.tzinfo is None:
            ts = ts.replace(tzinfo=TZ)
        if ts < cutoff:
            try:
                freed += p.stat().st_size
                p.unlink()
                deleted += 1
            except OSError:
                pass
    return {"deleted_files": deleted, "freed_bytes": freed, "retention_days": days}


# ============================================================ 总结
_summary_locks: dict[str, threading.Lock] = {}
_summary_locks_guard = threading.Lock()


def _lock_for(date: str) -> threading.Lock:
    with _summary_locks_guard:
        return _summary_locks.setdefault(date, threading.Lock())


def summarize_date(date: str, push_after: bool = False, force_push: bool = False) -> dict[str, Any]:
    lock = _lock_for(date)
    if not lock.acquire(timeout=600):
        raise RuntimeError("该日总结正在生成中")
    try:
        items = db.transcripts_for_day(date)
        if not items:
            raise RuntimeError(f"{date} 没有录音/转写内容，已跳过（不调用大模型）")
        if not any((i.get("text") or "").strip() for i in items):
            raise RuntimeError(f"{date} 的录音没有可用的转写文本，已跳过（不调用大模型）")
        db.upsert_journal({"date": date, "status": "running", "source_files": len(items)})
        try:
            res = llm.summarize_day(items, date)
        except Exception as e:  # noqa: BLE001
            old = db.get_journal(date)
            db.upsert_journal(
                {
                    "date": date,
                    "status": "failed",
                    "error": f"{type(e).__name__}: {e}",
                    "attempts": int((old["attempts"] if old else 0) or 0) + 1,
                }
            )
            raise
        md = res["markdown"]
        header = f"# {date} 语音日记\n\n> 由录音豆后端自动生成 · 模型 `{res.get('model')}` · {now_iso()}\n\n"
        full = header + md + "\n"
        JOURNAL_DIR.mkdir(parents=True, exist_ok=True)
        (JOURNAL_DIR / f"{date}.md").write_text(full, "utf-8")
        (JOURNAL_DIR / f"{date}.json").write_text(
            json.dumps({**res, "items": [{k: v for k, v in i.items() if k != "segments_json"} for i in items]},
                       ensure_ascii=False, indent=2),
            "utf-8",
        )
        old = db.get_journal(date)
        db.upsert_journal(
            {
                "date": date,
                "status": "done",
                "markdown": full,
                "summary": res["summary"],
                "model": res.get("model"),
                "llm_base_url": res.get("llm_base_url"),
                "source_chars": res.get("source_chars"),
                "source_files": res.get("source_files"),
                "error": None,
                "attempts": int((old["attempts"] if old else 0) or 0) + 1,
            }
        )
        out: dict[str, Any] = {**res, "markdown": full}
        if push_after:
            out["push"] = do_push(date, force=force_push)
        return out
    finally:
        lock.release()


def do_push(date: str, force: bool = False) -> dict[str, Any]:
    j = db.get_journal(date)
    if not j:
        raise RuntimeError(f"{date} 还没有总结，不推送")
    if j["status"] != "done" and not force:
        raise RuntimeError(f"{date} 总结状态为 {j['status']}，不推送")
    if not (j["summary"] or "").strip() and not (j["markdown"] or "").strip():
        raise RuntimeError(f"{date} 总结内容为空，不推送")
    pc = config.get("push") or {}
    mode = pc.get("content_mode", "summary")
    title = (pc.get("title_template") or "🎙 录音豆 · {date} 总结").replace("{date}", date)
    md = j["markdown"] or ""
    summary = j["summary"] or ""
    base = (pc.get("public_base_url") or "").rstrip("/")
    link = f"{base}/admin#/journal/{date}" if base else ""
    if mode == "full":
        body = md[:3800]
    elif mode == "link":
        body = f"{summary}\n\n{link}" if link else summary
    else:
        body = summary
    if link and mode == "summary":
        body = f"{body}\n\n📎 {link}"
    results = push.send_all(title, body, link, target_date=date)
    db.set_push_status(date, {k: ("ok" if v.get("ok") else f"fail: {v.get('message')}") for k, v in results.items()})
    return results


# ============================================================ 身份校验（Web）
def _http_of(msg: str) -> int | None:
    m = re.match(r"HTTP (\d{3})", msg or "")
    return int(m.group(1)) if m else None


def _auth_ok(request: Request) -> bool:
    cfg = config.get("web") or {}
    user = cfg.get("username") or "admin"
    pwd = cfg.get("password") or "recbean"
    auth = request.headers.get("Authorization") or ""
    if auth.startswith("Basic "):
        import base64

        try:
            raw = base64.b64decode(auth[6:]).decode("utf-8")
            u, _, p = raw.partition(":")
            return secrets.compare_digest(u, user) and secrets.compare_digest(p, pwd)
        except Exception:  # noqa: BLE001
            return False
    return False


def require_auth(request: Request) -> None:
    if not _auth_ok(request):
        raise HTTPException(401, "需要登录", headers={"WWW-Authenticate": 'Basic realm="recbean"'})


# ============================================================ 管理 API
@app.get("/api/config", dependencies=[Depends(require_auth)])
async def api_get_config():
    return JSONResponse(config.public_config())


@app.put("/api/config", dependencies=[Depends(require_auth)])
async def api_put_config(request: Request):
    body = await request.json()
    if not isinstance(body, dict):
        raise HTTPException(400, "body 必须是 JSON 对象")
    try:
        config.merge_update(body)
    except Exception as e:  # noqa: BLE001
        raise HTTPException(500, f"保存失败: {e}")
    return {"ok": True, "config": config.public_config()}


@app.post("/api/config/reload", dependencies=[Depends(require_auth)])
async def api_reload_config():
    return {"ok": True, "config": config.public_config() if config.reload() else {}}


@app.get("/api/status", dependencies=[Depends(require_auth)])
async def api_status():
    st = db.stats()
    return {
        "stats": st,
        "transcribe": transcribe.state(),
        "llm": {
            "configured": bool(config.get("llm", "api_key") or config.get("llm", "base_url")),
            "model": config.get("llm", "model"),
            "base_url": config.get("llm", "base_url"),
            "auto_summary": config.get("llm", "auto_summary"),
            "daily_time": config.get("llm", "daily_time"),
        },
        "push": {"enabled": push.enabled_channels()},
        "scheduler": scheduler_state(),
        "time": now_iso(),
    }


@app.post("/api/asr/test", dependencies=[Depends(require_auth)])
async def api_asr_test():
    h = transcribe.asr_health()
    return h


@app.post("/api/asr/transcribe/{uid}", dependencies=[Depends(require_auth)])
async def api_transcribe_one(uid: str, force: bool = True):
    row = db.get_upload(uid)
    if not row:
        raise HTTPException(404, "not found")
    res = transcribe.transcribe_upload(dict(row), force=force)
    t = db.get_transcript(uid)
    return {"result": res, "transcript": dict(t) if t else None}


@app.post("/api/llm/test", dependencies=[Depends(require_auth)])
async def api_llm_test(request: Request):
    body = {}
    try:
        body = await request.json()
    except Exception:  # noqa: BLE001
        pass
    override = body.get("llm") if isinstance(body, dict) else None
    # 允许前端用“未保存的表单值”直接测试；脱敏占位符回退到已保存值
    if isinstance(override, dict):
        cur = config.get("llm") or {}
        merged = {**cur, **{k: (cur.get(k) if v == config.SECRET_MASK else v) for k, v in override.items()}}
    else:
        merged = None
    return llm.test_connection(merged)


@app.post("/api/push/test/{channel}", dependencies=[Depends(require_auth)])
async def api_push_test(channel: str, request: Request):
    override = None
    try:
        body = await request.json()
        if isinstance(body, dict) and isinstance(body.get("channel"), dict):
            cur = (config.get("push", "channels") or {}).get(channel) or {}
            override = {**cur, **{k: (cur.get(k) if v == config.SECRET_MASK else v) for k, v in body["channel"].items()}}
    except Exception:  # noqa: BLE001
        pass
    try:
        if override is not None:
            ok, msg = push.SENDERS[channel](
                override, "🎙 录音豆 · 推送测试", "这是一条来自录音豆后端的测试消息。", ""
            )
            db.add_push_log(channel, ok, msg, http_status=_http_of(msg))
        else:
            ok, msg = push.test_channel(channel)
            db.add_push_log(channel, ok, msg, http_status=_http_of(msg))
        return {"ok": ok, "message": msg}
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "message": f"{type(e).__name__}: {e}"}



@app.get("/api/push/logs", dependencies=[Depends(require_auth)])
async def api_push_logs(limit: int = 50):
    return {"items": db.push_logs(limit)}


@app.get("/api/days", dependencies=[Depends(require_auth)])
async def api_days(limit: int = 90):
    return {"items": db.days_with_uploads(limit)}


@app.get("/api/journals", dependencies=[Depends(require_auth)])
async def api_journals(limit: int = 60):
    return db.journals_list(limit)


@app.get("/api/journal/{date}", dependencies=[Depends(require_auth)])
async def api_journal(date: str):
    j = db.get_journal(date)
    items = db.transcripts_for_day(date)
    if not j and not items:
        raise HTTPException(404, "not found")
    return {
        "date": date,
        "journal": dict(j) if j else None,
        "transcripts": [
            {
                "upload_id": i["upload_id"],
                "filename": i.get("filename"),
                "time": i.get("effective_at") or i.get("received_at"),
                "time_trusted": i.get("time_trusted"),
                "text": i.get("text"),
                "duration_s": i.get("duration_s"),
            }
            for i in items
        ],
    }


@app.post("/api/summarize", dependencies=[Depends(require_auth)])
async def api_summarize(request: Request):
    body = await request.json()
    date = body.get("date")
    if not date:
        raise HTTPException(400, "缺少 date")
    try:
        res = summarize_date(date, push_after=bool(body.get("push")))
        return {"ok": True, "date": date, "markdown": res["markdown"], "push": res.get("push")}
    except Exception as e:  # noqa: BLE001
        return JSONResponse(status_code=500, content={"ok": False, "error": f"{type(e).__name__}: {e}"})


@app.post("/api/push/{date}", dependencies=[Depends(require_auth)])
async def api_push_date(date: str):
    try:
        return {"ok": True, "results": do_push(date, force=True)}
    except Exception as e:  # noqa: BLE001
        return JSONResponse(status_code=500, content={"ok": False, "error": str(e)})


@app.post("/api/cleanup", dependencies=[Depends(require_auth)])
async def api_cleanup(request: Request):
    body = {}
    try:
        body = await request.json()
    except Exception:  # noqa: BLE001
        pass
    days = int((body or {}).get("days") or config.get("retention", "audio_days") or 30)
    return cleanup_audio(days)


# ============================================================ 调度器
SCHED_STATE = {"last_run": {}, "next": {}, "started_at": None}


def scheduler_state() -> dict[str, Any]:
    return dict(SCHED_STATE)


def _hhmm() -> tuple[int, int]:
    t = (config.get("llm", "daily_time") or "22:00").strip()
    try:
        h, m = t.split(":")
        return int(h), int(m)
    except ValueError:
        return 22, 0


def _scheduler_loop() -> None:
    SCHED_STATE["started_at"] = now_iso()
    time.sleep(20)
    last_summary_date = ""
    last_cleanup_date = ""
    while True:
        try:
            nowdt = datetime.now(TZ)
            # ---- 每日总结
            if config.get("llm", "auto_summary", default=True):
                h, m = _hhmm()
                today = nowdt.strftime("%Y-%m-%d")
                hm = nowdt.strftime("%H:%M")
                SCHED_STATE["next"]["summary"] = f"{today} {h:02d}:{m:02d}"
                if hm == f"{h:02d}:{m:02d}" and last_summary_date != today:
                    last_summary_date = today
                    lookback = int(config.get("llm", "summary_lookback_days") or 1)
                    target = (nowdt - timedelta(days=max(1, lookback) - 1)).strftime("%Y-%m-%d")
                    SCHED_STATE["last_run"]["summary"] = now_iso()
                    SCHED_STATE["last_run"]["summary_date"] = target
                    SCHED_STATE["last_run"]["summary_has_data"] = bool(db.transcripts_for_day(target))
                    _run_daily_summary(target)
            # ---- 清理
            ct = (config.get("retention", "cleanup_time") or "03:30")
            days = int(config.get("retention", "audio_days") or 0)
            if days > 0:
                today = nowdt.strftime("%Y-%m-%d")
                SCHED_STATE["next"]["cleanup"] = f"{today} {ct}"
                if nowdt.strftime("%H:%M") == ct and last_cleanup_date != today:
                    last_cleanup_date = today
                    SCHED_STATE["last_run"]["cleanup"] = now_iso()
                    cleanup_audio(days)
        except Exception:  # noqa: BLE001
            traceback.print_exc()
        time.sleep(30)


def _run_daily_summary(date: str) -> None:
    try:
        items = db.transcripts_for_day(date)
        if not items:
            print(f"[sched] {date} 无录音/转写，跳过总结与推送（不做无意义的 LLM 调用）", flush=True)
            return
        # 转写全部失败或全为空文本时同样不调用 LLM
        if not any((i.get("text") or "").strip() for i in items):
            print(f"[sched] {date} 有录音但没有可用的转写文本，跳过总结与推送", flush=True)
            return
        j = db.get_journal(date)
        if j and j["status"] == "done":
            print(f"[sched] {date} 已有总结，仅推送", flush=True)
            do_push(date)
            return
        res = summarize_date(date, push_after=True)
        print(f"[sched] {date} 总结完成，推送: {res.get('push')}", flush=True)
    except Exception as e:  # noqa: BLE001
        print(f"[sched] {date} 总结失败: {e}", flush=True)


# ============================================================ Web 页面
TEMPLATES = Path(__file__).parent / "templates"
STATIC = Path(__file__).parent / "static"
if STATIC.exists():
    app.mount("/static", StaticFiles(directory=str(STATIC)), name="static")


@app.get("/admin", response_class=HTMLResponse, dependencies=[Depends(require_auth)])
@app.get("/admin/", response_class=HTMLResponse, dependencies=[Depends(require_auth)])
async def admin_page():
    p = TEMPLATES / "admin.html"
    return HTMLResponse(p.read_text("utf-8"))


@app.get("/", response_class=HTMLResponse)
async def index(request: Request):
    """未登录跳转到 /admin；已登录也展示配置页。"""
    if not _auth_ok(request):
        return HTMLResponse(
            '<!doctype html><meta charset="utf-8"><title>录音豆</title>'
            '<body style="font-family:system-ui;background:#0f1216;color:#e6edf3;padding:40px">'
            '<h2>🎙 录音豆后端</h2><p>服务运行中。</p>'
            '<p>管理后台：<a style="color:#58a6ff" href="/admin">/admin</a>（需要登录）</p>'
            '<p>设备接口：<code>POST /upload</code> · <code>GET /health</code></p></body>'
        )
    return await admin_page()


# ============================================================ 启动
@app.on_event("startup")
async def _startup() -> None:
    transcribe.start_worker()
    threading.Thread(target=_scheduler_loop, name="scheduler", daemon=True).start()
    print(
        f"[start] data={DATA_DIR} audio={AUDIO_DIR} db={db.CURRENT_DB} "
        f"asr={config.get('asr', 'base_url')} llm={config.get('llm', 'base_url')}/{config.get('llm', 'model')}",
        flush=True,
    )
    if UPLOAD_TOKEN:
        cfg = config.load()
        if UPLOAD_TOKEN not in (cfg["device"].get("tokens") or []):
            cfg["device"].setdefault("tokens", []).append(UPLOAD_TOKEN)
            cfg["device"]["require_token"] = True
            config.save(cfg)
