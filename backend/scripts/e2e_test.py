#!/usr/bin/env python3
"""
录音豆 · 全链路验证脚本

按顺序验证：
  1. 后端健康检查
  2. 设备上传协议（file 在前 + 全部字段 + X-Device-Id）
  3. 幂等（重复上传同 id）
  4. 时间不可信链路（time_trusted=false -> 用 received_at 推算日期）
  5. FunASR 转写（含 RTF 统计）
  6. 大模型总结（需已配置 API Key）
  7. 推送（需已配置渠道）

用法：
  python3 scripts/e2e_test.py --server http://127.0.0.1:8010 --user admin --pass recbean \
      --wav data/samples/sensevoice_zh.mp3 --wav2 data/samples/paraformer_example2.wav
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
import urllib.request
from base64 import b64encode
from datetime import datetime, timedelta, timezone

TZ = timezone(timedelta(hours=8))
PASS, FAIL = "✅", "❌"
results: list[tuple[str, bool, str]] = []


def rec(name: str, ok: bool, detail: str = "") -> None:
    results.append((name, ok, detail))
    print(f"{PASS if ok else FAIL} {name}" + (f" — {detail}" if detail else ""))


def http(base: str, path: str, method="GET", body=None, auth=None, timeout=30):
    req = urllib.request.Request(
        base.rstrip("/") + path,
        data=json.dumps(body).encode() if body is not None else None,
        method=method,
        headers={"Content-Type": "application/json", **auth} if body is not None else ({**auth} if auth else {}),
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        raw = e.read().decode()
        try:
            return e.code, json.loads(raw)
        except ValueError:
            return e.code, {"raw": raw}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="http://127.0.0.1:8010")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--pass", dest="pwd", default="recbean")
    ap.add_argument("--wav", required=True)
    ap.add_argument("--wav2", default="")
    ap.add_argument("--skip-llm", action="store_true")
    ap.add_argument("--skip-push", action="store_true")
    a = ap.parse_args()
    auth = {"Authorization": "Basic " + b64encode(f"{a.user}:{a.pwd}".encode()).decode()}

    print("=" * 62)
    print("录音豆 · 全链路验证")
    print("=" * 62)

    # 1 健康检查
    st, d = http(a.server, "/health")
    rec("1. GET /health", st == 200 and d.get("status") == "ok", f"uploads={d.get('uploads')}")

    # 2/3 上传 + 幂等
    sess = f"session-{int(time.time()) % 10000:04d}"
    # 用 --json 时每次上传输出一个 JSON 对象（多行），依次解析
    def _run_sim(wav: str, extra: list[str]) -> list[dict]:
        out = subprocess.run(
            [sys.executable, "scripts/sim_device.py", "--server", a.server, "--wav", wav,
             "--session", sess, "--sequence", "1", "--json", *extra],
            capture_output=True, text=True,
        )
        dec, idx, blocks = json.JSONDecoder(), 0, []
        s = out.stdout
        while idx < len(s):
            while idx < len(s) and s[idx] in " \r\n\t":
                idx += 1
            if idx >= len(s):
                break
            obj, end = dec.raw_decode(s, idx)
            blocks.append(obj)
            idx = end
        if not blocks:
            raise RuntimeError(f"模拟器无输出: {out.stderr[-400:]}")
        return blocks

    try:
        blocks = _run_sim(a.wav, ["--repeat", "2"])
        rec("2. POST /upload（file 最前 + 11 字段）", blocks[0]["ok"],
            f"{blocks[0]['elapsed']}s id={blocks[0]['body'].get('id')}")
        rec("3. 幂等（重复上传同一 id）",
            blocks[0]["body"].get("id") == blocks[1]["body"].get("id") and blocks[1]["body"].get("duplicate") is True)
        uid = blocks[0]["body"].get("id")
    except Exception as e:  # noqa: BLE001
        rec("2. POST /upload", False, str(e)[:300])
        uid = None

    # 4 时间不可信（用陌生 wav 避免与上面的幂等文件去重）
    if a.wav2:
        try:
            b2 = _run_sim(a.wav2, ["--notrust", "--device", "esp32s3-notrust"])[0]
            st_u, du = http(a.server, f"/api/uploads/{b2['body']['id']}", auth=auth)
            # 重复上传时 duplicate=true 是正确行为，两种都接受（幂等语义）
            rec("4. 时间不可信（recorded_at=NULL，用接收时间推算日期）",
                du.get("recorded_at") is None and du.get("time_trusted") == 0 and bool(du.get("day")),
                f"day={du.get('day')} effective={du.get('effective_at')} trusted={du.get('time_trusted')}"
                f" dup={b2['body'].get('duplicate')}")
        except Exception as e:  # noqa: BLE001
            rec("4. 时间不可信", False, str(e)[:300])

    # 5 转写
    print("\n-- 等待 FunASR 转写（2 核 CPU，约 0.3~1x 实时）--")
    ok_tr, t0 = False, time.time()
    trs = []
    while time.time() - t0 < 900:
        st_c, sd = http(a.server, "/api/status", auth=auth)
        tr = sd.get("stats", {}).get("transcribe", {})
        pend = (tr.get("pending", 0) + tr.get("running", 0))
        print(f"   转写状态: done={tr.get('done',0)} pending={pend} failed={tr.get('failed',0)}", flush=True)
        if pend == 0:
            ok_tr = (tr.get("failed", 0) == 0)
            break
        time.sleep(10)
    st_c, items = http(a.server, "/api/uploads?limit=10&transcribe_status=done", auth=auth)
    trs = items.get("items", [])
    if trs and uid:
        st_t, det = http(a.server, f"/api/uploads/{uid}", auth=auth)
        text = (det.get("transcript") or {}).get("text") or ""
        rec("5. FunASR 转写", bool(text.strip()), f"{len(text)} 字: {text[:60]}…" if text else "无文本")

    # 6 总结
    if not a.skip_llm:
        day = (trs[0]["day"] if trs else datetime.now(TZ).strftime("%Y-%m-%d"))
        t0 = time.time()
        st_s, sd = http(a.server, "/api/summarize", "POST", {"date": day}, auth=auth, timeout=600)
        rec("6. 大模型生成每日总结", st_s == 200 and sd.get("ok") is True,
            f"{round(time.time()-t0,1)}s" if st_s == 200 else str(sd)[:200])
        if st_s == 200:
            md = sd.get("markdown", "")
            print("\n----- 生成的日记（前 900 字）-----")
            print(md[:900])
            print("----------------------------------\n")

    # 7 推送
    if not a.skip_push:
        st_p, pd = http(a.server, "/api/status", auth=auth)
        chans = pd.get("push", {}).get("enabled", [])
        if not chans:
            rec("7. 推送", True, "跳过：未启用任何渠道")
        else:
            day = (trs[0]["day"] if trs else datetime.now(TZ).strftime("%Y-%m-%d"))
            st_x, xd = http(a.server, f"/api/push/{day}", "POST", {}, auth=auth, timeout=120)
            res = xd.get("results", {})
            allok = bool(res) and all(v.get("ok") for k, v in res.items() if k != "_none")
            rec("7. 多渠道推送", allok, json.dumps({k: v.get("ok") for k, v in res.items()}, ensure_ascii=False))

    print("\n" + "=" * 62)
    passed = sum(1 for _, ok, _ in results if ok)
    print(f"结果: {passed}/{len(results)} 通过")
    for name, ok, detail in results:
        print(f"  {PASS if ok else FAIL} {name}")
    print("=" * 62)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
