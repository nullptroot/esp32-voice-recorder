"""录音豆后端 · 多渠道推送

支持（Web 上可多选，可分别测试）：
  bark         Bark           —— iOS 首选，免费开源（自建或 api.day.app）
  serverchan   Server 酱      —— 微信服务号接收（sctp 新版接口 + 旧版 sc.ftqq.com 兼容）
  pushplus     PushPlus       —— 微信接收，免费额度
  ntfy         ntfy.sh        —— 跨平台，最省事（可自建）
  dingtalk     钉钉机器人      —— 支持加签
  feishu       飞书机器人      —— 支持加签
  wecom        企业微信群机器人
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import re
import time
import urllib.parse
from typing import Any, Callable

import httpx

from . import config

TIMEOUT = 20.0


class PushError(RuntimeError):
    pass


def _post(url: str, *, json_body: Any = None, headers: dict | None = None, data: Any = None) -> httpx.Response:
    with httpx.Client(timeout=TIMEOUT, follow_redirects=True) as cli:
        return cli.post(url, json=json_body, headers=headers or {}, data=data)


def _get(url: str, *, headers: dict | None = None, params: dict | None = None) -> httpx.Response:
    with httpx.Client(timeout=TIMEOUT, follow_redirects=True) as cli:
        return cli.get(url, headers=headers or {}, params=params or {})


def _check(r: httpx.Response, ok_pred: Callable[[Any], bool] | None = None) -> tuple[bool, str]:
    try:
        body = r.json()
    except Exception:  # noqa: BLE001
        body = r.text[:300]
    ok = r.status_code < 300
    if ok and ok_pred is not None:
        try:
            ok = bool(ok_pred(body))
        except Exception:  # noqa: BLE001
            ok = False
    return ok, f"HTTP {r.status_code} {str(body)[:280]}"


# ------------------------------------------------------------------ Bark
def send_bark(c: dict, title: str, body: str, url: str = "") -> tuple[bool, str]:
    base = (c.get("url") or "https://api.day.app").strip().rstrip("/")
    key = (c.get("key") or "").strip()
    if not key:
        raise PushError("Bark 未配置 key")
    # 兼容两种填法：只填 key，或直接填完整 https://api.day.app/<key>
    if base.endswith("/"):
        base = base[:-1]
    if not base.startswith("http"):
        raise PushError("Bark url 需以 http(s):// 开头")
    payload: dict[str, Any] = {
        "title": title,
        "body": body[:3500],
        "group": c.get("group") or "录音豆",
        "level": "active",
    }
    if c.get("sound"):
        payload["sound"] = c["sound"]
    if c.get("icon"):
        payload["icon"] = c["icon"]
    if url:
        payload["url"] = url
    r = _post(f"{base}/{key}", json_body=payload)
    ok, msg = _check(r, lambda b: b.get("code") in (200, 0) if isinstance(b, dict) else False)
    if not ok:
        # 老版本 Bark 只支持 GET/路径拼接，再试一次
        q = {"title": title, "body": body[:3500], "group": payload["group"]}
        if url:
            q["url"] = url
        r2 = _get(f"{base}/{key}/{urllib.parse.quote(title, safe='')}/{urllib.parse.quote(body[:1500], safe='')}", params=q)
        ok2, msg2 = _check(r2, lambda b: b.get("code") == 200 if isinstance(b, dict) else False)
        if ok2:
            return True, "ok(get fallback)"
        return False, f"{msg} | {msg2}"
    return True, msg


# ------------------------------------------------------------ Server 酱
def send_serverchan(c: dict, title: str, body: str, url: str = "") -> tuple[bool, str]:
    key = (c.get("sendkey") or "").strip()
    if not key:
        raise PushError("Server 酱未配置 sendkey")
    desp = body + (f"\n\n[查看完整日记]({url})" if url else "")
    if key.startswith("sctp"):
        api = f"https://{key}.push.ft07.com/send"
        r = _post(api, data={"title": title[:100], "desp": desp, "channel": c.get("channel") or ""})
        return _check(r, lambda b: b.get("code") == 0 if isinstance(b, dict) else False)
    r = _post(f"https://sctapi.ftqq.com/{key}.send", data={"title": title[:100], "desp": desp})
    return _check(r, lambda b: b.get("code") == 0 if isinstance(b, dict) else False)


# ------------------------------------------------------------- PushPlus
def send_pushplus(c: dict, title: str, body: str, url: str = "") -> tuple[bool, str]:
    token = (c.get("token") or "").strip()
    if not token:
        raise PushError("PushPlus 未配置 token")
    payload: dict[str, Any] = {
        "token": token,
        "title": title[:100],
        "content": body,
        "template": c.get("template") or "markdown",
    }
    if c.get("topic"):
        payload["topic"] = c["topic"]
    if url:
        payload["content"] = f"{body}\n\n[查看完整日记]({url})"
    r = _post("https://www.pushplus.plus/send", json_body=payload)
    return _check(r, lambda b: b.get("code") == 200 if isinstance(b, dict) else False)


# ---------------------------------------------------------------- ntfy
def send_ntfy(c: dict, title: str, body: str, url: str = "") -> tuple[bool, str]:
    base = (c.get("url") or "https://ntfy.sh").strip().rstrip("/")
    topic = (c.get("topic") or "").strip()
    if not topic:
        raise PushError("ntfy 未配置 topic")
    headers = {
        "Title": _b64_header(title),
        "Priority": str(c.get("priority") or "default"),
        "Content-Type": "text/plain; charset=utf-8",
    }
    if c.get("tags"):
        headers["Tags"] = str(c["tags"])
    if url:
        headers["Click"] = url
    if c.get("token"):
        tok = c["token"].strip()
        headers["Authorization"] = tok if tok.lower().startswith("bearer ") else f"Bearer {tok}"
    text = body + (f"\n\n{url}" if url else "")
    r = _post(f"{base}/{topic}", data=text.encode("utf-8"), headers=headers)
    return _check(r)


def _b64_header(s: str) -> str:
    """非 ASCII 头部按 RFC 2047 编码，兼容 ntfy 服务端。"""
    try:
        s.encode("ascii")
        return s
    except UnicodeEncodeError:
        return "=?UTF-8?B?" + base64.b64encode(s.encode("utf-8")).decode() + "?="


# ------------------------------------------------------------- 钉钉
def send_dingtalk(c: dict, title: str, body: str, url: str = "") -> tuple[bool, str]:
    hook = (c.get("webhook") or "").strip()
    if not hook:
        raise PushError("钉钉未配置 webhook")
    if c.get("secret"):
        ts = str(round(time.time() * 1000))
        string_to_sign = f"{ts}\n{c['secret']}"
        sign = base64.b64encode(
            hmac.new(c["secret"].encode(), string_to_sign.encode(), digestmod=hashlib.sha256).digest()
        ).decode()
        sep = "&" if "?" in hook else "?"
        hook = f"{hook}{sep}timestamp={ts}&sign={urllib.parse.quote_plus(sign)}"
    text = body + (f"\n\n[查看完整日记]({url})" if url else "")
    if (c.get("msgtype") or "markdown") == "text":
        payload = {"msgtype": "text", "text": {"content": f"{title}\n\n{text}"}}
        if c.get("at_mobiles"):
            payload["at"] = {"atMobiles": [m.strip() for m in c["at_mobiles"].split(",") if m.strip()]}
    else:
        payload = {
            "msgtype": "markdown",
            "markdown": {"title": title, "text": f"### {title}\n\n{text}"},
        }
        if c.get("at_mobiles"):
            mobiles = [m.strip() for m in c["at_mobiles"].split(",") if m.strip()]
            payload["at"] = {"atMobiles": mobiles, "isAtAll": False}
            payload["markdown"]["text"] += "\n\n" + " ".join(f"@{m}" for m in mobiles)
    r = _post(hook, json_body=payload)
    return _check(r, lambda b: b.get("errcode") == 0 if isinstance(b, dict) else False)


# ------------------------------------------------------------- 飞书
def _feishu_sign(secret: str, ts: str) -> str:
    string_to_sign = f"{ts}\n{secret}"
    return base64.b64encode(
        hmac.new(string_to_sign.encode("utf-8"), digestmod=hashlib.sha256).digest()
    ).decode()


def send_feishu(c: dict, title: str, body: str, url: str = "") -> tuple[bool, str]:
    hook = (c.get("webhook") or "").strip()
    if not hook:
        raise PushError("飞书未配置 webhook")
    text = f"**{title}**\n\n{body}" + (f"\n\n[查看完整日记]({url})" if url else "")
    payload: dict[str, Any] = {
        "msg_type": "interactive",
        "card": {
            "config": {"wide_screen_mode": True},
            "header": {"title": {"tag": "plain_text", "content": title}, "template": "blue"},
            "elements": [{"tag": "div", "text": {"tag": "lark_md", "content": text[:4000]}}],
        },
    }
    if c.get("secret"):
        ts = str(int(time.time()))
        payload["timestamp"] = ts
        payload["sign"] = _feishu_sign(c["secret"], ts)
    r = _post(hook, json_body=payload)
    ok, msg = _check(r, lambda b: (b.get("code") == 0 or b.get("StatusCode") == 0 or b.get("status_code") == 0)
                     if isinstance(b, dict) else False)
    if not ok:
        # 卡片格式不被接受时退化为纯文本
        plain = {"msg_type": "text", "content": {"text": f"{title}\n\n{body[:3000]}" + (f"\n{url}" if url else "")}}
        if c.get("secret"):
            ts = str(int(time.time()))
            plain["timestamp"] = ts
            plain["sign"] = _feishu_sign(c["secret"], ts)
        r2 = _post(hook, json_body=plain)
        ok2, msg2 = _check(r2, lambda b: b.get("code") == 0 if isinstance(b, dict) else False)
        if ok2:
            return True, "ok(text fallback)"
        return False, f"{msg} | {msg2}"
    return True, msg


# ------------------------------------------------------------ 企业微信
def send_wecom(c: dict, title: str, body: str, url: str = "") -> tuple[bool, str]:
    hook = (c.get("webhook") or "").strip()
    if not hook:
        raise PushError("企业微信未配置 webhook")
    text = f"# {title}\n\n{body}" + (f"\n\n[查看完整日记]({url})" if url else "")
    payload = {"msgtype": "markdown", "markdown": {"content": text[:4000]}}
    r = _post(hook, json_body=payload)
    return _check(r, lambda b: b.get("errcode") == 0 if isinstance(b, dict) else False)


SENDERS: dict[str, Callable[[dict, str, str, str], tuple[bool, str]]] = {
    "bark": send_bark,
    "serverchan": send_serverchan,
    "pushplus": send_pushplus,
    "ntfy": send_ntfy,
    "dingtalk": send_dingtalk,
    "feishu": send_feishu,
    "wecom": send_wecom,
}

CHANNEL_LABELS = {
    "bark": "Bark (iOS)",
    "serverchan": "Server 酱",
    "pushplus": "PushPlus",
    "ntfy": "ntfy",
    "dingtalk": "钉钉机器人",
    "feishu": "飞书机器人",
    "wecom": "企业微信机器人",
}


def send_all(title: str, body: str, url: str = "", target_date: str | None = None,
             only: list[str] | None = None) -> dict[str, Any]:
    """向所有已启用渠道推送。返回 {channel: {"ok": bool, "message": str}}。"""
    from . import db

    ch_cfg = config.get("push", "channels") or {}
    results: dict[str, Any] = {}
    for name, fn in SENDERS.items():
        c = ch_cfg.get(name) or {}
        if only is not None and name not in only:
            continue
        if only is None and not c.get("enabled"):
            continue
        t0 = time.time()
        try:
            ok, msg = fn(c, title, body, url)
        except Exception as e:  # noqa: BLE001
            ok, msg = False, f"{type(e).__name__}: {e}"
        results[name] = {"ok": ok, "message": msg, "elapsed": round(time.time() - t0, 2)}
        m = re.match(r"HTTP (\d{3})", msg or "")
        db.add_push_log(name, ok, msg, target_date=target_date,
                        http_status=int(m.group(1)) if m else None)
    if not results:
        results["_none"] = {"ok": False, "message": "没有启用任何推送渠道"}
    return results


def enabled_channels() -> list[str]:
    ch = config.get("push", "channels") or {}
    return [n for n in SENDERS if (ch.get(n) or {}).get("enabled")]


def test_channel(name: str) -> tuple[bool, str]:
    ch_cfg = config.get("push", "channels") or {}
    c = ch_cfg.get(name)
    if c is None or name not in SENDERS:
        raise PushError(f"未知渠道: {name}")
    return SENDERS[name](
        c,
        "🎙 录音豆 · 推送测试",
        "这是一条来自录音豆后端的测试消息。\n如果收到，说明该渠道配置正确。",
        "",
    )
