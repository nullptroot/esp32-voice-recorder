#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
校验配音时长：读 narration/vo-*.wav，和目标时长对比，给出建议 playbackRate。

用法:
    python3 marketing/tools/check_voice_durations.py
    python3 marketing/tools/check_voice_durations.py --dir path/to/audio

只依赖标准库（wave），不需要 ffmpeg。
mp3 需要额外解码，脚本会跳过并提示你转 wav。
"""
import argparse
import glob
import json
import os
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEGMENTS = os.path.join(ROOT, "narration", "segments.json")
TOLERANCE = 0.15  # ±15%


def wav_duration(path):
    with wave.open(path, "rb") as w:
        frames = w.getnframes()
        rate = w.getframerate()
        return frames / float(rate)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(ROOT, "narration"),
                    help="配音文件所在目录（默认 marketing/narration/）")
    ap.add_argument("--tolerance", type=float, default=TOLERANCE)
    args = ap.parse_args()

    if not os.path.exists(SEGMENTS):
        print(f"❌ 找不到 {SEGMENTS}")
        return 1

    with open(SEGMENTS, encoding="utf-8") as f:
        segs = json.load(f)

    print(f"{'段':<4}{'场景':<16}{'实际':>8}{'目标':>8}{'偏差':>9}  判定")
    print("-" * 62)

    missing, over, under, ok = [], [], [], 0
    for s in segs:
        sid = s["id"]
        target = s["target_s"]
        cand = os.path.join(args.dir, f"vo-{sid}.wav")
        if not os.path.exists(cand):
            mp3 = os.path.join(args.dir, f"vo-{sid}.mp3")
            if os.path.exists(mp3):
                print(f"{sid:<4}{s['title']:<16}{'mp3 跳过':>8}{target:>7.1f}s"
                      f"{'':>9}  转成 wav 再测")
            else:
                print(f"{sid:<4}{s['title']:<16}{'缺失':>8}{target:>7.1f}s")
                missing.append(sid)
            continue

        actual = wav_duration(cand)
        diff = (actual - target) / target
        rate = actual / target  # >1 表示实际更长，需要加速

        if diff > args.tolerance:
            verdict = f"超长 {diff:+.0%} → playbackRate≈{rate:.2f}"
            over.append((sid, actual, target, rate))
        elif diff < -args.tolerance:
            verdict = f"偏短 {diff:+.0%} → 可留白或放慢"
            under.append((sid, actual, target))
        else:
            verdict = "OK"
            ok += 1

        print(f"{sid:<4}{s['title']:<16}{actual:>7.2f}s{target:>7.1f}s"
              f"{diff:>+8.0%}  {verdict}")

    print("-" * 62)
    print(f"通过 {ok} / 共 {len(segs)}；超长 {len(over)}，偏短 {len(under)}，缺失 {len(missing)}")

    if over:
        print("\n建议：优先把对应 Scene 的 durationInFrames 加长（±20 帧内无感），")
        print("其次才在 Remotion 里给 <Audio> 设 playbackRate：")
        for sid, a, t, r in over:
            print(f"  vo-{sid}: playbackRate={r:.2f}  (+{(a-t):.2f}s)")

    if missing:
        print(f"\n还没生成：{', '.join('vo-'+m for m in missing)}")

    return 0 if not (over or missing) else 2


if __name__ == "__main__":
    sys.exit(main())
