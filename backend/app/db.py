"""录音豆后端 · SQLite 存储层

表结构在 docs/backend-api.md §6 的基础上做了向后兼容扩展
（原有列名/语义不变，仅新增列，便于与验收服务对齐）。
"""
from __future__ import annotations

import json
import sqlite3
import threading
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Iterable

DB_LOCK = threading.RLock()
_local = threading.local()

TZ = timezone(timedelta(hours=8))

SCHEMA = """
CREATE TABLE IF NOT EXISTS uploads (
    id            TEXT PRIMARY KEY,      -- uuid
    device_id     TEXT NOT NULL,
    filename      TEXT NOT NULL,
    stored_path   TEXT NOT NULL,         -- 相对 storage/audio
    size_bytes    INTEGER NOT NULL,
    sha256        TEXT NOT NULL,
    recorded_at   TEXT,                  -- 时间不可信时为 NULL
    time_trusted  INTEGER NOT NULL DEFAULT 1,
    session_id    TEXT,
    sequence      INTEGER,
    monotonic_ms  INTEGER,
    received_at   TEXT NOT NULL,
    client_ip     TEXT,
    status        TEXT NOT NULL DEFAULT 'stored',
    wav_valid     INTEGER NOT NULL DEFAULT 1,
    -- 扩展列
    time_source   TEXT,
    effective_at  TEXT,                  -- 归一化后的“业务时间”（可信=recorded_at，否则=received_at）
    day           TEXT,                  -- YYYY-MM-DD（业务时间所在日，本地时区）
    duration_s    REAL,
    transcribe_status TEXT NOT NULL DEFAULT 'pending',  -- pending|running|done|failed|skipped
    transcribe_error  TEXT,
    transcribe_attempts INTEGER NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_dedup ON uploads(device_id, sha256);
CREATE INDEX IF NOT EXISTS idx_recv ON uploads(received_at);
CREATE INDEX IF NOT EXISTS idx_day ON uploads(day);
CREATE INDEX IF NOT EXISTS idx_trs ON uploads(transcribe_status);

CREATE TABLE IF NOT EXISTS transcripts (
    upload_id     TEXT PRIMARY KEY,
    device_id     TEXT,
    day           TEXT,
    engine        TEXT,
    text          TEXT,
    segments_json TEXT,
    duration_s    REAL,
    elapsed_s     REAL,
    warnings      TEXT,
    created_at    TEXT NOT NULL,
    FOREIGN KEY(upload_id) REFERENCES uploads(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_transcripts_day ON transcripts(day);

CREATE TABLE IF NOT EXISTS journals (
    date          TEXT PRIMARY KEY,      -- YYYY-MM-DD
    status        TEXT NOT NULL,         -- pending|running|done|failed
    markdown      TEXT,
    summary       TEXT,
    model         TEXT,
    llm_base_url  TEXT,
    source_chars  INTEGER,
    source_files  INTEGER,
    error         TEXT,
    attempts      INTEGER NOT NULL DEFAULT 0,
    push_status   TEXT,                  -- json: {channel: ok/fail}
    created_at    TEXT,
    updated_at    TEXT
);

CREATE TABLE IF NOT EXISTS transcribe_jobs (
    id TEXT PRIMARY KEY, upload_id TEXT, engine TEXT, status TEXT,
    transcript_path TEXT, error TEXT, retry_count INTEGER,
    created_at TEXT, updated_at TEXT
);

CREATE TABLE IF NOT EXISTS journal_jobs (
    id TEXT PRIMARY KEY, date TEXT, status TEXT, journal_path TEXT,
    error TEXT, retry_count INTEGER, created_at TEXT, updated_at TEXT
);

CREATE TABLE IF NOT EXISTS push_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts TEXT NOT NULL,
    channel TEXT NOT NULL,
    target_date TEXT,
    ok INTEGER NOT NULL,
    http_status INTEGER,
    message TEXT,
    detail TEXT
);
CREATE INDEX IF NOT EXISTS idx_push_ts ON push_logs(ts);
"""


def now() -> datetime:
    return datetime.now(TZ)


def now_iso() -> str:
    return now().isoformat(timespec="seconds")


def conn(db_path: Path) -> sqlite3.Connection:
    c = getattr(_local, "conn", None)
    if c is None:
        db_path.parent.mkdir(parents=True, exist_ok=True)
        c = sqlite3.connect(str(db_path), timeout=30, check_same_thread=False)
        c.row_factory = sqlite3.Row
        c.execute("PRAGMA journal_mode=WAL")
        c.execute("PRAGMA synchronous=NORMAL")
        c.execute("PRAGMA foreign_keys=ON")
        _local.conn = c
    return c


def init(db_path: Path) -> None:
    with DB_LOCK:
        c = conn(db_path)
        c.executescript(SCHEMA)
        c.commit()
        _migrate(c)


def _migrate(c: sqlite3.Connection) -> None:
    """兼容旧库：补齐扩展列。"""
    have = {r["name"] for r in c.execute("PRAGMA table_info(uploads)")}
    extra = {
        "time_source": "TEXT",
        "effective_at": "TEXT",
        "day": "TEXT",
        "duration_s": "REAL",
        "transcribe_status": "TEXT NOT NULL DEFAULT 'pending'",
        "transcribe_error": "TEXT",
        "transcribe_attempts": "INTEGER NOT NULL DEFAULT 0",
    }
    for col, typ in extra.items():
        if col not in have:
            c.execute(f"ALTER TABLE uploads ADD COLUMN {col} {typ}")
    # 旧数据回填 effective_at / day
    rows = c.execute(
        "SELECT id, recorded_at, time_trusted, received_at FROM uploads WHERE effective_at IS NULL OR day IS NULL"
    ).fetchall()
    for r in rows:
        eff = r["recorded_at"] if (r["time_trusted"] and r["recorded_at"]) else r["received_at"]
        day = _to_day(eff)
        c.execute("UPDATE uploads SET effective_at=?, day=? WHERE id=?", (eff, day, r["id"]))
    c.commit()


def _to_day(iso: str | None) -> str | None:
    if not iso:
        return None
    try:
        dt = datetime.fromisoformat(iso.replace("Z", "+00:00"))
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=TZ)
        return dt.astimezone(TZ).strftime("%Y-%m-%d")
    except ValueError:
        return iso[:10]


def new_id() -> str:
    return str(uuid.uuid4())


# ---------------------------------------------------------------- uploads
def insert_upload(d: dict[str, Any]) -> None:
    cols = [
        "id", "device_id", "filename", "stored_path", "size_bytes", "sha256",
        "recorded_at", "time_trusted", "session_id", "sequence", "monotonic_ms",
        "received_at", "client_ip", "status", "wav_valid", "time_source",
        "effective_at", "day", "duration_s", "transcribe_status",
    ]
    with DB_LOCK:
        c = conn(CURRENT_DB)
        c.execute(
            f"INSERT INTO uploads({','.join(cols)}) VALUES ({','.join('?' * len(cols))})",
            [d.get(k) for k in cols],
        )
        c.commit()


def find_duplicate(device_id: str, sha256: str) -> sqlite3.Row | None:
    with DB_LOCK:
        return conn(CURRENT_DB).execute(
            "SELECT * FROM uploads WHERE device_id=? AND sha256=?", (device_id, sha256)
        ).fetchone()


def get_upload(uid: str) -> sqlite3.Row | None:
    with DB_LOCK:
        return conn(CURRENT_DB).execute("SELECT * FROM uploads WHERE id=?", (uid,)).fetchone()


def list_uploads(limit: int = 100, offset: int = 0, day: str | None = None,
                 transcribe_status: str | None = None, device_id: str | None = None) -> list[dict]:
    q = "SELECT * FROM uploads WHERE 1=1"
    args: list[Any] = []
    if day:
        q += " AND day=?"
        args.append(day)
    if transcribe_status:
        q += " AND transcribe_status=?"
        args.append(transcribe_status)
    if device_id:
        q += " AND device_id=?"
        args.append(device_id)
    q += " ORDER BY ts_sort DESC LIMIT ? OFFSET ?"
    args += [limit, offset]
    with DB_LOCK:
        rows = conn(CURRENT_DB).execute(_with_sort(q), args).fetchall()
    return [dict(r) for r in rows]


def _with_sort(q: str) -> str:
    # 业务时间排序：用 datetime() 归一化带时区的 ISO8601，避免字符串直接比较导致乱序
    return q.replace(
        "ORDER BY ts_sort DESC",
        "ORDER BY datetime(IFNULL(effective_at, received_at)) DESC, received_at DESC",
    )


def count_uploads() -> int:
    with DB_LOCK:
        return conn(CURRENT_DB).execute("SELECT COUNT(*) AS n FROM uploads").fetchone()["n"]


def stats() -> dict[str, Any]:
    with DB_LOCK:
        c = conn(CURRENT_DB)
        n = c.execute("SELECT COUNT(*) AS n FROM uploads").fetchone()["n"]
        total = c.execute("SELECT COALESCE(SUM(size_bytes),0) AS s FROM uploads").fetchone()["s"]
        trs = dict_fetchall(c.execute(
            "SELECT transcribe_status, COUNT(*) AS n FROM uploads GROUP BY transcribe_status"
        ))
        days = [
            dict(r)
            for r in c.execute(
                "SELECT day, COUNT(*) AS files, COALESCE(SUM(size_bytes),0) AS bytes "
                "FROM uploads GROUP BY day ORDER BY day DESC LIMIT 60"
            ).fetchall()
        ]
        jn = c.execute("SELECT COUNT(*) AS n FROM journals WHERE status='done'").fetchone()["n"]
    return {
        "uploads": n,
        "bytes": total,
        "transcribe": trs,
        "days": days,
        "journals": jn,
    }


def dict_fetchall(cur: Iterable[sqlite3.Row]) -> dict[str, int]:
    return {r[0]: r[1] for r in cur}


def set_transcribe(uid: str, status: str, error: str | None = None, bump: bool = False) -> None:
    with DB_LOCK:
        c = conn(CURRENT_DB)
        if bump:
            c.execute(
                "UPDATE uploads SET transcribe_status=?, transcribe_error=?, "
                "transcribe_attempts=transcribe_attempts+1 WHERE id=?",
                (status, error, uid),
            )
        else:
            c.execute(
                "UPDATE uploads SET transcribe_status=?, transcribe_error=? WHERE id=?",
                (status, error, uid),
            )
        c.commit()


def set_duration(uid: str, dur: float | None) -> None:
    with DB_LOCK:
        c = conn(CURRENT_DB)
        c.execute("UPDATE uploads SET duration_s=? WHERE id=?", (dur, uid))
        c.commit()


# ------------------------------------------------------------ transcripts
def upsert_transcript(d: dict[str, Any]) -> None:
    with DB_LOCK:
        c = conn(CURRENT_DB)
        c.execute(
            """INSERT INTO transcripts(upload_id, device_id, day, engine, text, segments_json,
                                       duration_s, elapsed_s, warnings, created_at)
               VALUES (?,?,?,?,?,?,?,?,?,?)
               ON CONFLICT(upload_id) DO UPDATE SET
                 engine=excluded.engine, text=excluded.text, segments_json=excluded.segments_json,
                 duration_s=excluded.duration_s, elapsed_s=excluded.elapsed_s,
                 warnings=excluded.warnings, created_at=excluded.created_at""",
            (
                d["upload_id"], d.get("device_id"), d.get("day"), d.get("engine"),
                d.get("text"), json.dumps(d.get("segments") or [], ensure_ascii=False),
                d.get("duration_s"), d.get("elapsed_s"),
                json.dumps(d.get("warnings") or [], ensure_ascii=False),
                now_iso(),
            ),
        )
        c.commit()


def get_transcript(upload_id: str) -> sqlite3.Row | None:
    with DB_LOCK:
        return conn(CURRENT_DB).execute(
            "SELECT * FROM transcripts WHERE upload_id=?", (upload_id,)
        ).fetchone()


def transcripts_for_day(day: str) -> list[dict]:
    with DB_LOCK:
        rows = conn(CURRENT_DB).execute(
            """SELECT t.*, u.filename, u.recorded_at, u.received_at, u.time_trusted,
                      u.session_id, u.sequence, u.effective_at
               FROM transcripts t JOIN uploads u ON u.id = t.upload_id
               WHERE u.day = ?
               ORDER BY datetime(IFNULL(u.effective_at, u.received_at)) ASC,
                        u.session_id ASC, u.sequence ASC""",
            (day,),
        ).fetchall()
    return [dict(r) for r in rows]


def days_with_uploads(limit: int = 90) -> list[dict]:
    with DB_LOCK:
        rows = conn(CURRENT_DB).execute(
            """SELECT day, COUNT(*) AS files, SUM(CASE WHEN transcribe_status='done' THEN 1 ELSE 0 END) AS done,
                      SUM(CASE WHEN transcribe_status='pending' OR transcribe_status='running' THEN 1 ELSE 0 END) AS pending,
                      SUM(CASE WHEN transcribe_status='failed' THEN 1 ELSE 0 END) AS failed,
                      COALESCE(SUM(size_bytes),0) AS bytes
               FROM uploads WHERE day IS NOT NULL GROUP BY day ORDER BY day DESC LIMIT ?""",
            (limit,),
        ).fetchall()
    journals = {r["date"]: r["status"] for r in journals_list(limit)["items"]}
    out = []
    for r in rows:
        d = dict(r)
        d["journal_status"] = journals.get(d["day"])
        out.append(d)
    return out


# --------------------------------------------------------------- journals
def upsert_journal(d: dict[str, Any]) -> None:
    with DB_LOCK:
        c = conn(CURRENT_DB)
        cur = c.execute("SELECT date, attempts, created_at FROM journals WHERE date=?", (d["date"],)).fetchone()
        ts = now_iso()
        if cur is None:
            c.execute(
                """INSERT INTO journals(date, status, markdown, summary, model, llm_base_url,
                                        source_chars, source_files, error, attempts, push_status,
                                        created_at, updated_at)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                (
                    d["date"], d.get("status", "pending"), d.get("markdown"), d.get("summary"),
                    d.get("model"), d.get("llm_base_url"), d.get("source_chars"),
                    d.get("source_files"), d.get("error"), d.get("attempts", 0),
                    json.dumps(d.get("push_status") or {}, ensure_ascii=False), ts, ts,
                ),
            )
        else:
            c.execute(
                """UPDATE journals SET status=?, markdown=COALESCE(?, markdown),
                          summary=COALESCE(?, summary), model=?, llm_base_url=?, source_chars=?,
                          source_files=?, error=?, attempts=?, push_status=?, updated_at=?
                   WHERE date=?""",
                (
                    d.get("status", "done"), d.get("markdown"), d.get("summary"),
                    d.get("model"), d.get("llm_base_url"), d.get("source_chars"),
                    d.get("source_files"), d.get("error"), d.get("attempts", 0),
                    json.dumps(d.get("push_status") or {}, ensure_ascii=False),
                    ts, d["date"],
                ),
            )
        c.commit()


def get_journal(date: str) -> sqlite3.Row | None:
    with DB_LOCK:
        return conn(CURRENT_DB).execute("SELECT * FROM journals WHERE date=?", (date,)).fetchone()


def journals_list(limit: int = 60, offset: int = 0) -> dict[str, Any]:
    with DB_LOCK:
        c = conn(CURRENT_DB)
        n = c.execute("SELECT COUNT(*) AS n FROM journals").fetchone()["n"]
        rows = c.execute(
            "SELECT date, status, summary, model, source_chars, source_files, error, updated_at "
            "FROM journals ORDER BY date DESC LIMIT ? OFFSET ?",
            (limit, offset),
        ).fetchall()
    return {"count": n, "items": [dict(r) for r in rows]}


def set_push_status(date: str, status: dict[str, Any]) -> None:
    with DB_LOCK:
        c = conn(CURRENT_DB)
        c.execute(
            "UPDATE journals SET push_status=?, updated_at=? WHERE date=?",
            (json.dumps(status, ensure_ascii=False), now_iso(), date),
        )
        c.commit()


# -------------------------------------------------------------- push logs
def add_push_log(channel: str, ok: bool, message: str, detail: str = "",
                 target_date: str | None = None, http_status: int | None = None) -> None:
    with DB_LOCK:
        c = conn(CURRENT_DB)
        c.execute(
            "INSERT INTO push_logs(ts, channel, target_date, ok, http_status, message, detail) "
            "VALUES (?,?,?,?,?,?,?)",
            (now_iso(), channel, target_date, 1 if ok else 0, http_status, message, detail[:4000]),
        )
        c.commit()


def push_logs(limit: int = 50) -> list[dict]:
    with DB_LOCK:
        rows = conn(CURRENT_DB).execute(
            "SELECT * FROM push_logs ORDER BY id DESC LIMIT ?", (limit,)
        ).fetchall()
    return [dict(r) for r in rows]


def uploads_pending_transcribe(limit: int = 50, max_retry: int = 3) -> list[dict]:
    with DB_LOCK:
        rows = conn(CURRENT_DB).execute(
            "SELECT * FROM uploads WHERE transcribe_status IN ('pending','failed') "
            "AND transcribe_attempts < ? ORDER BY IFNULL(effective_at, received_at) ASC LIMIT ?",
            (max_retry, limit),
        ).fetchall()
    return [dict(r) for r in rows]


# 当前库路径（进程级，由 main 启动时设置）
CURRENT_DB: Path = Path("/data/storage/recbean.db")
