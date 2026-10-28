#!/usr/bin/env python3
"""
录音豆 —— 极简验收接收服务 (FastAPI + SQLite)

只做验收需要的三件事:
  1. POST /upload  接收设备上传的 WAV + 元数据, 落盘 + 写 SQLite, 幂等
  2. GET  /health  健康检查
  3. GET  /        一个简单的浏览页面 (验收时肉眼确认文件已到)

完整后端契约见 docs/backend-api.md
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import sqlite3
import time
import uuid
from datetime import datetime, timezone, timedelta
from pathlib import Path

from fastapi import FastAPI, File, Form, HTTPException, Request, UploadFile
from fastapi.responses import HTMLResponse, JSONResponse

BASE = Path(__file__).resolve().parent
STORAGE = BASE / "storage"
AUDIO = STORAGE / "audio"
DB_PATH = STORAGE / "uploads.db"
TZ = timezone(timedelta(hours=8))

STORAGE.mkdir(parents=True, exist_ok=True)
AUDIO.mkdir(parents=True, exist_ok=True)

app = FastAPI(title="recbean-ingest", version="1.0.0")


def db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db() -> None:
    with db() as conn:
        conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS uploads (
                id            TEXT PRIMARY KEY,
                device_id     TEXT NOT NULL,
                filename      TEXT NOT NULL,
                stored_path   TEXT NOT NULL,
                size_bytes    INTEGER NOT NULL,
                sha256        TEXT NOT NULL,
                recorded_at   TEXT,
                time_trusted  INTEGER NOT NULL DEFAULT 1,
                session_id    TEXT,
                sequence      INTEGER,
                monotonic_ms  INTEGER,
                received_at   TEXT NOT NULL,
                client_ip     TEXT,
                status        TEXT NOT NULL DEFAULT 'stored',
                wav_valid     INTEGER NOT NULL DEFAULT 1
            );
            CREATE UNIQUE INDEX IF NOT EXISTS idx_dedup
                ON uploads(device_id, sha256);
            CREATE INDEX IF NOT EXISTS idx_recv ON uploads(received_at);
            """
        )


init_db()


def now_iso() -> str:
    return datetime.now(TZ).isoformat(timespec="seconds")


def safe_name(name: str) -> str:
    name = os.path.basename(name or "upload.wav")
    name = re.sub(r"[^A-Za-z0-9._-]", "_", name)
    return name[:120] or "upload.wav"


def parse_bool(v) -> bool:
    if v is None:
        return True
    return str(v).strip().lower() in ("1", "true", "yes", "on")


def parse_optional(v):
    if v is None:
        return None
    s = str(v).strip()
    if s == "" or s.lower() in ("null", "none", "undefined"):
        return None
    return s


def wav_check(head: bytes) -> bool:
    return len(head) >= 12 and head[0:4] == b"RIFF" and head[8:12] == b"WAVE"


@app.get("/health")
async def health() -> JSONResponse:
    with db() as conn:
        n = conn.execute("SELECT COUNT(*) FROM uploads").fetchone()[0]
    return JSONResponse({"status": "ok", "time": now_iso(), "uploads": n})


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
):
    """
    设备端 multipart/form-data 上传。
    幂等: 同一 device_id + sha256 的重复上传返回同一 id 与 status=stored。
    """
    filename = safe_name(file.filename or "upload.wav")
    tmp = AUDIO / f".{uuid.uuid4().hex}.part"
    sha = hashlib.sha256()
    size = 0
    head = b""

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

    with db() as conn:
        row = conn.execute(
            "SELECT id, stored_path, filename FROM uploads WHERE device_id=? AND sha256=?",
            (device_id, digest),
        ).fetchone()
        if row:
            tmp.unlink(missing_ok=True)
            return JSONResponse(
                {
                    "id": row["id"],
                    "status": "stored",
                    "duplicate": True,
                    "stored_path": row["stored_path"],
                }
            )

        rid = str(uuid.uuid4())
        day = datetime.now(TZ).strftime("%Y/%m/%d")
        rel = Path(day) / f"{rid}_{filename}"
        dest = AUDIO / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp.replace(dest)

        recorded_at_v = parse_optional(recorded_at)
        trusted = parse_bool(time_trusted) and recorded_at_v is not None
        if not trusted:
            recorded_at_v = None
        seq = None
        try:
            seq = int(sequence) if sequence not in (None, "", "null") else None
        except ValueError:
            seq = None
        mono = None
        try:
            mono = int(monotonic_ms) if monotonic_ms not in (None, "", "null") else None
        except ValueError:
            mono = None

        conn.execute(
            """INSERT INTO uploads(id, device_id, filename, stored_path, size_bytes, sha256,
                                   recorded_at, time_trusted, session_id, sequence,
                                   monotonic_ms, received_at, client_ip, status, wav_valid)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (
                rid, device_id, filename, str(rel), size, digest,
                recorded_at_v, 1 if trusted else 0, parse_optional(session_id), seq,
                mono, now_iso(), request.client.host if request.client else None,
                "stored", 1 if wav_check(head) else 0,
            ),
        )
    if not wav_check(head):
        print(f"[warn] 收到非标准 WAV: {filename} ({size} bytes)")

    return JSONResponse(
        {
            "id": rid,
            "status": "stored",
            "duplicate": False,
            "stored_path": str(rel),
            "sha256": digest,
            "time_trusted": trusted,
        }
    )


@app.get("/api/uploads")
async def list_uploads(limit: int = 50) -> JSONResponse:
    limit = max(1, min(limit, 500))
    with db() as conn:
        rows = conn.execute(
            "SELECT * FROM uploads ORDER BY received_at DESC LIMIT ?", (limit,)
        ).fetchall()
    return JSONResponse({"count": len(rows), "items": [dict(r) for r in rows]})


@app.get("/api/uploads/{uid}")
async def get_upload(uid: str) -> JSONResponse:
    with db() as conn:
        row = conn.execute("SELECT * FROM uploads WHERE id=?", (uid,)).fetchone()
    if not row:
        raise HTTPException(404, "not found")
    return JSONResponse(dict(row))


@app.get("/", response_class=HTMLResponse)
async def index() -> str:
    with db() as conn:
        rows = conn.execute(
            "SELECT * FROM uploads ORDER BY received_at DESC LIMIT 100"
        ).fetchall()
        total = conn.execute("SELECT COUNT(*) FROM uploads").fetchone()[0]
        total_bytes = conn.execute("SELECT COALESCE(SUM(size_bytes),0) FROM uploads").fetchone()[0]
    trs = []
    for r in rows:
        flag = "" if r["wav_valid"] else ' <span style="color:#d29922">(非标准WAV)</span>'
        tt = "可信" if r["time_trusted"] else '<span style="color:#d29922">时间不可信</span>'
        trs.append(
            f"<tr><td>{r['received_at']}</td><td>{r['device_id']}</td>"
            f"<td>{r['filename']}{flag}</td><td>{r['size_bytes']/1024:.0f} KB</td>"
            f"<td>{r['recorded_at'] or '-'}</td><td>{tt}</td>"
            f"<td>{r['session_id'] or '-'}</td><td>{r['sequence'] if r['sequence'] is not None else '-'}</td></tr>"
        )
    body = "".join(trs) or '<tr><td colspan="8" style="color:#888">暂无上传</td></tr>'
    return f"""<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<title>录音豆 · 接收服务</title>
<style>
body{{background:#0f1216;color:#e6edf3;font-family:system-ui,sans-serif;margin:0;padding:24px}}
h1{{font-size:18px;font-weight:600}} .meta{{color:#8b98a5;font-size:13px;margin-bottom:16px}}
table{{width:100%;border-collapse:collapse;font-size:13px}}
th,td{{text-align:left;padding:7px 8px;border-bottom:1px solid #252d36}}
th{{color:#8b98a5;font-weight:500}} code{{color:#58a6ff}}
</style></head><body>
<h1>🎙 录音豆 · 接收服务</h1>
<div class="meta">共 {total} 个文件 · {total_bytes/1048576:.1f} MB · 存储目录 <code>{AUDIO}</code></div>
<table><thead><tr><th>接收时间</th><th>设备</th><th>文件</th><th>大小</th>
<th>录制时间</th><th>时间可信</th><th>会话</th><th>序号</th></tr></thead>
<tbody>{body}</tbody></table>
</body></html>"""
