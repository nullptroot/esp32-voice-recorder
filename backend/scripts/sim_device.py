#!/usr/bin/env python3
"""
录音豆 · 设备上传模拟器（按 docs/backend-api.md §1 精确复刻固件行为）

用途：在没有硬件时做全链路验证（上传 -> 转写 -> 总结 -> 推送）。

用法：
  python3 sim_device.py --server http://127.0.0.1:8010 --wav a.wav \
      --device esp32s3-001 --session session-0101 --sequence 1
  python3 sim_device.py --server http://127.0.0.1:8010 --wav a.wav --notrust
  python3 sim_device.py ... --repeat 2      # 幂等验证

特点：
  - multipart 中 **file 字段在最前**（与固件一致）；
  - 显式 Content-Length，不做 chunked；
  - X-Device-Id 头 + 全部 form 字段；
  - 解析响应 JSON 并断言 status/duplicate。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
import uuid
from datetime import datetime, timedelta, timezone

TZ = timezone(timedelta(hours=8))
BOUNDARY = "----RecBeanBoundary7MA4YWxkTrZu0gW"


def build_body(wav_path: str, filename: str, fields: dict[str, str]) -> bytes:
    parts: list[bytes] = []
    with open(wav_path, "rb") as f:
        data = f.read()
    parts.append(
        f"--{BOUNDARY}\r\n"
        f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
        f"Content-Type: audio/wav\r\n\r\n".encode()
    )
    parts.append(data)
    parts.append(b"\r\n")
    for k, v in fields.items():
        parts.append(
            f"--{BOUNDARY}\r\n"
            f'Content-Disposition: form-data; name="{k}"\r\n\r\n'
            f"{v}\r\n".encode()
        )
    parts.append(f"--{BOUNDARY}--\r\n".encode())
    return b"".join(parts)


def upload(server: str, wav: str, device_id: str, session_id: str, seq: int,
           recorded_at: str | None, time_trusted: bool, time_source: str,
           monotonic_ms: int | None = None, token: str = "") -> dict:
    body = build_body(
        wav,
        os.path.basename(wav),
        {
            "device_id": device_id,
            "recorded_at": recorded_at or "",
            "time_trusted": "true" if time_trusted else "false",
            "time_source": time_source,
            "session_id": session_id,
            "sequence": str(seq),
            "monotonic_ms": str(monotonic_ms if monotonic_ms is not None else int(time.time() * 1000) % 2**31),
        },
    )
    import urllib.request

    req = urllib.request.Request(
        server.rstrip("/") + "/upload",
        data=body,
        method="POST",
        headers={
            "Content-Type": f"multipart/form-data; boundary={BOUNDARY}",
            "Content-Length": str(len(body)),
            "X-Device-Id": device_id,
            **({"X-Upload-Token": token} if token else {}),
        },
    )
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            raw = r.read().decode("utf-8", "replace")
            code = r.status
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        code = e.code
    body_json = {}
    try:
        body_json = json.loads(raw)
    except ValueError:
        pass
    # 设备判定：2xx 且响应体可读 → 成功
    ok = 200 <= code < 300 and bool(body_json)
    return {"ok": ok, "http": code, "elapsed": round(time.time() - t0, 3), "body": body_json, "raw": raw[:500]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="http://127.0.0.1:8010")
    ap.add_argument("--wav", required=True)
    ap.add_argument("--device", default="esp32s3-001")
    ap.add_argument("--session", default=None)
    ap.add_argument("--sequence", type=int, default=1)
    ap.add_argument("--notrust", action="store_true", help="模拟时间不可信 (time_trusted=false)")
    ap.add_argument("--restored", action="store_true", help="time_source=restored（近似时间）")
    ap.add_argument("--offset-min", type=float, default=0, help="录制时间偏移（分钟）")
    ap.add_argument("--repeat", type=int, default=1, help="重复上传次数（验证幂等）")
    ap.add_argument("--token", default=os.environ.get("UPLOAD_TOKEN", ""))
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    session_id = a.session or f"session-{uuid.uuid4().int % 10000:04d}"
    rec_dt = datetime.now(TZ) - timedelta(minutes=a.offset_min)
    recorded_at = rec_dt.isoformat(timespec="seconds")
    trusted = not a.notrust
    source = "restored" if a.restored else ("ntp" if trusted else "none")
    if not trusted and a.restored:
        pass
    if not trusted and not a.restored:
        recorded_at = ""

    results = []
    for i in range(a.repeat):
        r = upload(a.server, a.wav, a.device, session_id, a.sequence,
                   recorded_at, trusted, source, token=a.token)
        results.append(r)
        if a.json:
            print(json.dumps(r, ensure_ascii=False, indent=2))
        else:
            print(f"[{i+1}/{a.repeat}] http={r['http']} ok={r['ok']} {r['elapsed']}s "
                  f"id={r['body'].get('id')} dup={r['body'].get('duplicate')} "
                  f"trusted={r['body'].get('time_trusted')}")
            if not r["ok"]:
                print("  raw:", r["raw"])
    # 幂等断言（--json 模式下走 stderr，避免污染 JSON 输出）
    out = sys.stderr if a.json else sys.stdout
    if a.repeat > 1 and len({r["body"].get("id") for r in results}) == 1:
        print("✅ 幂等通过：重复上传返回同一 id", file=out)
    elif a.repeat > 1:
        print("❌ 幂等失败：id 不一致", file=out)
        return 2
    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
