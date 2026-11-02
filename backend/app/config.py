"""录音豆后端 · 配置管理

配置分两层：
  1. 环境变量（DATA_DIR / DB_PATH / SECRET_KEY 等运行时路径，只读）
  2. config.json（业务配置：ASR、大模型、推送渠道、调度、保留策略）—— 由 Web 页面读写

config.json 中的 API Key 等敏感字段在对外（GET /api/config）时做脱敏，
写回时若值等于脱敏占位符则保留原值，避免前端回填把 Key 覆盖成 ****。
"""
from __future__ import annotations

import copy
import json
import os
import threading
from pathlib import Path
from typing import Any

DATA_DIR = Path(os.environ.get("DATA_DIR", "/data"))
STORAGE_DIR = Path(os.environ.get("STORAGE_DIR", str(DATA_DIR / "storage")))
DB_PATH = Path(os.environ.get("DB_PATH", str(STORAGE_DIR / "recbean.db")))
CONFIG_PATH = Path(os.environ.get("CONFIG_PATH", str(DATA_DIR / "config.json")))

SECRET_MASK = "******"

DEFAULT_CONFIG: dict[str, Any] = {
    "asr": {
        "base_url": os.environ.get("ASR_URL", "http://asr:10095"),
        "timeout": 900,
        "engine_name": "funasr-paraformer-zh",
        "hotword": "",
        "batch_size_s": 300,
        "auto_transcribe": True,
        "max_retry": 3,
        # 相邻分段时间重叠合并（秒）：把 partN 末尾与 partN+1 开头重复的文本摘掉
        "dedup_overlap_seconds": 2.0,
    },
    "llm": {
        "base_url": "https://api.deepseek.com/v1",
        "api_key": "",
        "model": "deepseek-chat",
        "temperature": 0.4,
        "max_tokens": 4096,
        "timeout": 180,
        "extra_headers": {},
        # 长文本分段总结：超过该字符数先分块摘要再汇总
        "chunk_chars": 12000,
        "system_prompt": "",
        "user_prompt": "",
        "daily_time": "22:00",
        "auto_summary": True,
        "summary_lookback_days": 1,
    },
    "push": {
        "channels": {
            "bark": {"enabled": False, "url": "", "key": "", "sound": "", "group": "录音豆", "icon": "https://cdn.jsdelivr.net/gh/finb/bark@master/icon.jpg"},
            "serverchan": {"enabled": False, "sendkey": "", "channel": ""},
            "pushplus": {"enabled": False, "token": "", "topic": "", "template": "markdown"},
            "ntfy": {"enabled": False, "url": "https://ntfy.sh", "topic": "", "token": "", "priority": "default", "tags": "memo"},
            "dingtalk": {"enabled": False, "webhook": "", "secret": "", "at_mobiles": "", "msgtype": "markdown"},
            "feishu": {"enabled": False, "webhook": "", "secret": ""},
            "wecom": {"enabled": False, "webhook": ""},
        },
        # 推送内容：summary(摘要) | full(全文) | link(仅链接)
        "content_mode": "summary",
        "title_template": "🎙 录音豆 · {date} 总结",
        "public_base_url": "",
        "on_success_only": True,
    },
    "retention": {
        "audio_days": 30,
        "transcript_days": 0,   # 0 = 永久
        "cleanup_time": "03:30",
    },
    "web": {
        "username": os.environ.get("WEB_USERNAME", "admin"),
        "password": os.environ.get("WEB_PASSWORD", "recbean"),
        "timezone": "Asia/Shanghai",
    },
    "device": {
        "require_token": False,
        "tokens": [],
        "default_device_id": "esp32s3-001",
    },
}

_lock = threading.RLock()
_cache: dict[str, Any] | None = None


def _deep_merge(base: dict, override: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in (override or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _deep_merge(out[k], v)
        else:
            out[k] = v
    return out


def load() -> dict[str, Any]:
    global _cache
    with _lock:
        if _cache is not None:
            return _cache
        cfg = copy.deepcopy(DEFAULT_CONFIG)
        if CONFIG_PATH.exists():
            try:
                disk = json.loads(CONFIG_PATH.read_text("utf-8"))
                cfg = _deep_merge(cfg, disk)
            except Exception as e:  # noqa: BLE001
                print(f"[config] 读取 {CONFIG_PATH} 失败，使用默认配置: {e}", flush=True)
        # 环境变量兜底（首次部署可只靠环境变量）
        for env_key, path in (
            ("LLM_BASE_URL", ("llm", "base_url")),
            ("LLM_API_KEY", ("llm", "api_key")),
            ("LLM_MODEL", ("llm", "model")),
        ):
            val = os.environ.get(env_key)
            if val and not cfg[path[0]].get(path[1]):
                cfg[path[0]][path[1]] = val
        _cache = cfg
        return _cache


def save(cfg: dict[str, Any]) -> dict[str, Any]:
    global _cache
    with _lock:
        cfg = _deep_merge(DEFAULT_CONFIG, cfg)
        CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
        tmp = CONFIG_PATH.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), "utf-8")
        os.replace(tmp, CONFIG_PATH)
        try:
            CONFIG_PATH.chmod(0o600)
        except OSError:
            pass
        _cache = cfg
        return cfg


def reload() -> dict[str, Any]:
    global _cache
    with _lock:
        _cache = None
        return load()


def invalidate() -> None:
    global _cache
    _cache = None


def get(*path: str, default: Any = None) -> Any:
    cur: Any = load()
    for p in path:
        if not isinstance(cur, dict) or p not in cur:
            return default
        cur = cur[p]
    return cur


SECRET_KEYS = {
    "api_key", "key", "sendkey", "token", "secret", "webhook", "url",
    "password", "tokens", "extra_headers",
}


def _mask(node: Any, key_hint: str = "") -> Any:
    if isinstance(node, dict):
        out = {}
        for k, v in node.items():
            if k in SECRET_KEYS and isinstance(v, str) and v:
                out[k] = SECRET_MASK
            elif k in SECRET_KEYS and isinstance(v, list) and v:
                out[k] = [SECRET_MASK]
            else:
                out[k] = _mask(v, k)
        return out
    return node


def _unmask(new: Any, old: Any) -> Any:
    """前端回传的 ****** 视为“未修改”，保留原值。"""
    if isinstance(new, dict):
        base = old if isinstance(old, dict) else {}
        return {k: _unmask(v, base.get(k)) for k, v in new.items()}
    if isinstance(new, list):
        if old and isinstance(old, list) and all(x == SECRET_MASK for x in new):
            return old
        return new
    if new == SECRET_MASK and old is not None:
        return old
    return new


def public_config() -> dict[str, Any]:
    return _mask(load())


def merge_update(update: dict[str, Any]) -> dict[str, Any]:
    with _lock:
        cur = load()
        clean = _unmask(update, cur)
        # 推送渠道：只合并 channels 中提供的键，避免前端只传部分渠道时把其他清空
        if isinstance(clean.get("push"), dict) and isinstance(clean["push"].get("channels"), dict):
            cur_ch = cur.get("push", {}).get("channels", {})
            merged_ch = copy.deepcopy(cur_ch)
            for name, ch in clean["push"]["channels"].items():
                if isinstance(ch, dict):
                    merged_ch[name] = _deep_merge(cur_ch.get(name, {}), ch)
                else:
                    merged_ch[name] = ch
            clean["push"]["channels"] = merged_ch
        return save(clean)
