"""录音豆后端 · 大模型调用（OpenAI 兼容 Chat Completions 协议）

只要服务端兼容 /v1/chat/completions 就能用：
  DeepSeek、通义千问(DashScope 兼容模式)、Kimi、智谱 GLM、OpenAI、Ollama、LM Studio、vLLM、OneAPI…
在 Web 配置页填 base_url / model / api_key 即可切换。
"""
from __future__ import annotations

import json
import time
from typing import Any

import httpx

from . import config

DEFAULT_SYSTEM = (
    "你是一个中文个人日记助手，专门把用户一天的语音记录转写整理成结构化日记。"
    "你只依据给定转写内容写作，绝不编造事实、人名、数字。"
    "语言自然、简洁，使用第一人称。"
)

DEFAULT_USER_TEMPLATE = """下面是我在 {date} 这一天的语音记录转写，按时间顺序排列（部分时段标记为“时间不精确”）。
请据此写一篇日记，用 Markdown 输出，结构如下：

## 今日概述
用 2~4 句话概括今天主要做了什么。

## 时间线
按时间顺序列出关键事件，每条以 `**HH:MM**` 开头（时间未知的写 `**时段不详**`）。

## 想法与决定
我今天产生的值得记住的想法、判断或做出的决定（没有就写“无”）。

## 待办与承诺
需要后续跟进的事项（没有就写“无”）。若原文没给截止时间，不要编造。

## 亮点摘录
1~3 条最值得回看的原话。

要求：
1. 不要编造转写中没有的内容；信息量很低的片段可合并或省略。
2. 带“⚠️时间不精确”的片段，时间以“大约”描述，并在结尾补一句「（部分时间段不精确）」。
3. 相邻片段在边界处可能句子断裂，请理解语义后合并，不要照抄重复内容。
4. 全文控制在 800 字以内。

===== 转写开始 =====
{transcript}
===== 转写结束 =====
"""

CHUNK_SYSTEM = "你是中文文本整理助手。请把下面这段语音转写压缩成要点式纪要，保留所有人名、数字、时间、待办和关键句子，去掉语气词和重复。只输出纪要，不要评论。"


class LLMError(RuntimeError):
    pass


def _cfg() -> dict[str, Any]:
    return config.get("llm") or {}


def _endpoint(base_url: str) -> str:
    """把用户填的 Base URL 归一化成完整的 chat/completions 地址。

    兼容三种常见写法：
      https://api.deepseek.com/v1             -> .../v1/chat/completions
      https://tokenhub.tencentmaas.com/plan/v3 -> .../plan/v3/chat/completions  （已带版本段，不再补 /v1）
      https://xxx/v1/chat/completions         -> 原样使用
    """
    b = (base_url or "").strip().rstrip("/")
    if not b:
        raise LLMError("未配置大模型 base_url（Web 配置 → 大模型）")
    if b.endswith("/chat/completions"):
        return b
    last = b.rsplit("/", 1)[-1].lower()
    if last.startswith("v") and last[1:].isdigit():
        return b + "/chat/completions"
    return b + "/v1/chat/completions"


def chat(messages: list[dict[str, str]], *, temperature: float | None = None,
         max_tokens: int | None = None, timeout: float | None = None,
         cfg: dict[str, Any] | None = None) -> str:
    c = cfg or _cfg()
    api_key = (c.get("api_key") or "").strip()
    model = (c.get("model") or "").strip()
    if not model:
        raise LLMError("未配置大模型 model（Web 配置 → 大模型）")
    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    extra = c.get("extra_headers") or {}
    if isinstance(extra, dict):
        headers.update({str(k): str(v) for k, v in extra.items() if k})
    body = {
        "model": model,
        "messages": messages,
        "temperature": float(c.get("temperature", 0.4) if temperature is None else temperature),
        "max_tokens": int(c.get("max_tokens", 4096) if max_tokens is None else max_tokens),
        "stream": False,
    }
    url = _endpoint(c.get("base_url", ""))
    t = float(c.get("timeout", 180) if timeout is None else timeout)
    last_err: Exception | None = None
    for attempt in range(3):
        try:
            with httpx.Client(timeout=t) as cli:
                r = cli.post(url, headers=headers, json=body)
            if r.status_code >= 400:
                raise LLMError(f"LLM HTTP {r.status_code}: {r.text[:500]}")
            data = r.json()
            choices = data.get("choices") or []
            if not choices:
                raise LLMError(f"LLM 返回无 choices: {str(data)[:500]}")
            msg = choices[0].get("message") or {}
            content = msg.get("content")
            if isinstance(content, list):  # 某些兼容实现返回分段内容
                content = "".join(p.get("text", "") for p in content if isinstance(p, dict))
            # 推理模型（如 DeepSeek-R1 / 腾讯 TokenHub 的 deepseek-v4.1）可能把正文放在
            # reasoning_content，或 content 为空、全部落在思考过程里：做兜底提取。
            if not content or not str(content).strip():
                content = msg.get("reasoning_content") or msg.get("reasoning") or ""
            if not content:
                raise LLMError(f"LLM 返回内容为空: {str(data)[:400]}")
            return str(content).strip()
        except httpx.HTTPError as e:
            last_err = e
            time.sleep(1.5 * (attempt + 1))
        except LLMError:
            raise
    raise LLMError(f"LLM 请求失败: {last_err}")


def test_connection(cfg: dict[str, Any] | None = None) -> dict[str, Any]:
    t0 = time.time()
    try:
        out = chat(
            [{"role": "user", "content": "只回复两个字：正常"}],
            max_tokens=32,
            timeout=40,
            cfg=cfg,
        )
        c = cfg or _cfg()
        return {
            "ok": True,
            "model": c.get("model"),
            "base_url": c.get("base_url"),
            "latency": round(time.time() - t0, 2),
            "reply": out[:200],
        }
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "error": f"{type(e).__name__}: {e}"}


def build_transcript_block(items: list[dict[str, Any]], date: str) -> str:
    """把一天的分段转写拼成带时间标记的文本。"""
    lines: list[str] = []
    for it in items:
        text = (it.get("text") or "").strip()
        if not text:
            continue
        ts = _fmt_ts(it.get("effective_at") or it.get("recorded_at") or it.get("received_at"))
        flag = "" if it.get("time_trusted") else " ⚠️时间不精确"
        dur = it.get("duration_s")
        dur_s = f"（约{int(dur//60)}分{int(dur%60)}秒）" if dur else ""
        lines.append(f"【{ts}{flag}】{dur_s}\n{text}")
    if not lines:
        return ""
    return "\n\n".join(lines)


def _fmt_ts(iso: str | None) -> str:
    if not iso:
        return "时间未知"
    try:
        from datetime import datetime

        dt = datetime.fromisoformat(iso.replace("Z", "+00:00"))
        return dt.strftime("%H:%M")
    except ValueError:
        return iso[:16].replace("T", " ")


def summarize_day(items: list[dict[str, Any]], date: str) -> dict[str, Any]:
    """生成日记。超长文本先分块摘要，再汇总成最终日记。"""
    c = _cfg()
    block = build_transcript_block(items, date)
    if not block.strip():
        raise LLMError("当天没有可用的转写文本")
    chunk_chars = int(c.get("chunk_chars", 12000) or 12000)
    notes: list[str] = []
    if len(block) > chunk_chars:
        for i in range(0, len(block), chunk_chars):
            part = block[i : i + chunk_chars]
            note = chat(
                [
                    {"role": "system", "content": CHUNK_SYSTEM},
                    {"role": "user", "content": f"（第 {i//chunk_chars + 1} 段）\n{part}"},
                ],
                temperature=0.2,
                max_tokens=1500,
            )
            notes.append(note)
        block = "\n\n---\n\n".join(f"【第{i+1}段纪要】\n{n}" for i, n in enumerate(notes))
    system = (c.get("system_prompt") or "").strip() or DEFAULT_SYSTEM
    template = (c.get("user_prompt") or "").strip() or DEFAULT_USER_TEMPLATE
    user = template.replace("{date}", date).replace("{transcript}", block)
    md = chat(
        [{"role": "system", "content": system}, {"role": "user", "content": user}],
        max_tokens=int(c.get("max_tokens", 4096)),
    )
    return {
        "date": date,
        "markdown": md,
        "summary": extract_summary(md),
        "model": c.get("model"),
        "llm_base_url": c.get("base_url"),
        "source_chars": len(block),
        "source_files": len([x for x in items if (x.get("text") or "").strip()]),
        "chunked": bool(notes),
    }


def extract_summary(markdown: str, limit: int = 400) -> str:
    """从日记里抽出「今日概述」段做推送摘要。"""
    lines = markdown.splitlines()
    buf: list[str] = []
    grab = False
    for ln in lines:
        s = ln.strip()
        if s.startswith("##"):
            if grab:
                break
            if "概述" in s or "摘要" in s or "summary" in s.lower():
                grab = True
            continue
        if grab and s:
            buf.append(s)
    text = " ".join(buf).strip() or markdown.strip()
    text = " ".join(text.split())
    return text[:limit] + ("…" if len(text) > limit else "")
