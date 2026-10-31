#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成素材占位图（真实尺寸 PNG，Remotion 可直接 staticFile 引用）。

用法:
    python3 marketing/tools/gen_placeholders.py

产物:
    marketing/assets/photos/*.png
    marketing/assets/frames/*.png

替换真实素材时：保持文件名、尺寸一致直接覆盖即可，Remotion 工程无需改代码。
"""
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FONT_CANDIDATES = [
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]

BG = (11, 13, 16)
PANEL = (18, 21, 25)
GRID = (28, 32, 37)
TXT = (232, 236, 240)
DIM = (120, 129, 140)
CYAN = (0, 229, 192)
ORANGE = (255, 122, 26)

# id, 文件名, 宽, 高, 中文标题, 副标题, 分类, 色调
ASSETS = [
    # ---------- 照片 ----------
    ("P01", "photos/p01-hero.png", 1080, 1920,
     "封面主图 · 成品 3/4 视角", "未来 PCB「录音豆」悬浮于深色桌面 / 单 LED 环 / USB-C", "photo", "cyan"),
    ("P02", "photos/p02-bom-flatlay.png", 1080, 1350,
     "BOM 全家福 · 俯拍平铺", "ESP32-S3 板 + INMP441 + LM393 + MicroSD + 杜邦线，等距排列", "photo", "cyan"),
    ("P03", "photos/p03-wiring-overview.png", 1080, 1440,
     "接线全景 · 杜邦线阶段", "面包板俯拍，四模块走线清晰可辨，能看懂谁连谁", "photo", "cyan"),
    ("P04", "photos/p04-module-macro.png", 1080, 1080,
     "模块特写 · 1:1", "INMP441 麦克风正脸 / SD 卡座 / LM393 电位器，浅景深", "photo", "cyan"),
    ("P05", "photos/p05-pcb-concept.png", 1080, 1920,
     "成品概念图 · 自绘 PCB", "圆形磨砂黑外壳、直径 ~40mm、可 AI 出图（标注「概念图」）", "photo", "cyan"),
    ("P06", "photos/p06-webui.png", 1080, 1920,
     "设备配置网页 · 手机截图", "真机 http://<ip>/ 页面：状态 + 设备日志 + 文件列表", "photo", "cyan"),
    ("P07", "photos/p07-backend.png", 1080, 1920,
     "后端上传记录 · 截图", "backend-min 上传列表，能看到文件名带时间戳", "photo", "cyan"),
    ("P08", "photos/p08-sleep-current.png", 1080, 1440,
     "深睡电流 · 万用表读数", "µA 级读数特写，红黑表笔接供电回路", "photo", "cyan"),
    ("P09", "photos/p09-size-compare.png", 1080, 1080,
     "尺寸对比 · 1:1", "与硬币 / 耳机盒 / 打火机并排，体现「豆」的大小", "photo", "cyan"),
    ("P10", "photos/p10-lifestyle.png", 1080, 1920,
     "使用场景 · 生活化", "床头柜 / 车内 / 桌面，设备融入环境，暖光", "photo", "cyan"),
    ("P11", "photos/p11-order-1.png", 1080, 1920,
     "订单截图 1 · 主控 + 麦克风", "立创/淘宝订单页，遮住姓名电话地址，只露商品与价格", "photo", "cyan"),
    ("P12", "photos/p12-order-2.png", 1080, 1920,
     "订单截图 2 · 声音检测 + 存储", "LM393 模块、MicroSD 模块与 TF 卡订单", "photo", "cyan"),
    ("P13", "photos/p13-order-3.png", 1080, 1920,
     "订单截图 3 · 电源 + 电池", "TP4056、1000mAh 电池订单", "photo", "cyan"),
    ("P14", "photos/p14-order-4.png", 1080, 1920,
     "订单截图 4 · 杂项 + 合计", "杜邦线、排针、按键等杂项；最好能截到「合计」金额", "photo", "cyan"),
    ("P15", "photos/p15-solder-setup.png", 1080, 1440,
     "焊接工位 · 开工前", "电烙铁、焊锡、助焊剂、裸板排针，俯拍整齐摆放", "photo", "cyan"),
    ("P16", "photos/p16-solder-done.png", 1080, 1080,
     "焊好的排针 · 特写", "排针焊点饱满、无连锡，侧逆光拍出焊点反光", "photo", "cyan"),

    # ---------- 视频静帧（poster） ----------
    ("F01", "frames/f01-voice-trigger.png", 1080, 1920,
     "静帧 V01 · 声音触发", "手机竖拍：拍手 → 设备开始录音，画面含设备与手", "frame", "orange"),
    ("F02", "frames/f02-web-log.png", 1080, 1920,
     "静帧 V02 · 网页日志", "竖屏录屏：点「刷新日志」出现「VAD 确认人声」，文件列表 +1", "frame", "orange"),
    ("F03", "frames/f03-upload-success.png", 1080, 1920,
     "静帧 V03 · 上传成功", "竖屏录屏：文件列表出现 .up，后端收到文件", "frame", "orange"),
    ("F04", "frames/f04-sleep-power.png", 1080, 1920,
     "静帧 V04 · 深睡功耗", "手机拍：万用表 µA 读数 + 设备静止，可加字幕标注", "frame", "orange"),
    ("F05", "frames/f05-config-wake.png", 1080, 1920,
     "静帧 V05 · 配置键唤醒", "手机拍：杜邦线点 GPIO14 → 手机重新打开网页", "frame", "orange"),
    ("F06", "frames/f06-serial-log.png", 1080, 1920,
     "静帧 V06 · 串口日志", "竖屏录屏：日志滚动到「NTP 对时成功 / 上传成功」", "frame", "orange"),
    ("F07", "frames/f07-soldering.png", 1080, 1920,
     "静帧 V07 · 焊排针", "手机拍：烙铁尖、焊锡、排针入孔，烟雾与焊点反光", "frame", "orange"),
]


# 视频素材：没有 ffmpeg 时只产出静帧 poster + 规格，真实 mp4 由用户补齐
# id, 期望文件名, poster, 时长(s), 说明, 拍摄/录制要点
VIDEOS = [
    ("V01", "v01-voice-trigger.mp4", "frames/f01-voice-trigger.png", 12,
     "声音触发自动录音：拍手/说话 → 设备开始录，无手动操作",
     "手机竖拍，画面同时有设备和手；环境声保留，别配乐；结束后保留 1s 静帧"),
    ("V02", "v02-web-log.mp4", "frames/f02-web-log.png", 10,
     "网页设备日志：点「刷新日志」后出现「VAD 确认人声 (能量 … dB)」，状态 recording，文件列表 +1",
     "手机竖屏录屏：先开始录音、说一句话，再点「刷新日志」，让日志停在那一行 2 秒；别开通知"),
    ("V03", "v03-upload-success.mp4", "frames/f03-upload-success.png", 8,
     "上传成功：SD 文件出现 .up 标记，后端列表新增一条带时间戳记录",
     "竖屏录屏，先录设备文件列表，再切后端页面；两段可后期拼接"),
    ("V04", "v04-sleep-power.mp4", "frames/f04-sleep-power.png", 8,
     "深睡功耗：万用表 µA 读数稳定，设备无动作",
     "手机竖拍，表笔/读数必须清晰可读；可后期放大读数并叠加字幕"),
    ("V05", "v05-config-wake.mp4", "frames/f05-config-wake.png", 8,
     "配置键唤醒：杜邦线点 GPIO14 → 手机刷新网页成功",
     "一镜到底最能打：先拍手按 GPIO14，再切手机屏幕亮起网页"),
    ("V06", "v06-serial-log.mp4", "frames/f06-serial-log.png", 6,
     "串口日志 B-roll：滚动到 NTP 对时成功 / 上传成功 / EXT1 唤醒",
     "竖屏录屏终端，字号调大；只截取有信息量的 6 秒"),
    ("V07", "v07-soldering-pins.mp4", "frames/f07-soldering.png", 20,
     "焊排针：给焊盘上锡 → 排针插入 → 逐针焊接 → 检查焊点",
     "手机俯拍/侧拍 4K 60，固定机位；可 2× 加速到 10s；收一点烙铁环境音即可，别配乐"),
]


def load_font(size, index=2):
    for path in FONT_CANDIDATES:
        if not os.path.exists(path):
            continue
        for idx in (index, 0):
            try:
                return ImageFont.truetype(path, size, index=idx)
            except Exception:
                continue
    return ImageFont.load_default()


def draw_grid(d, w, h, step=60):
    for x in range(0, w, step):
        d.line([(x, 0), (x, h)], fill=GRID, width=1)
    for y in range(0, h, step):
        d.line([(0, y), (w, y)], fill=GRID, width=1)


def dashed_round_rect(d, box, radius=28, color=(70, 78, 90), dash=18, gap=14, width=3):
    x0, y0, x1, y1 = box
    # 四条边分别画虚线（角上留空，简化处理）
    def dashed_line(p0, p1):
        dx, dy = p1[0] - p0[0], p1[1] - p0[1]
        dist = (dx * dx + dy * dy) ** 0.5
        if dist == 0:
            return
        ux, uy = dx / dist, dy / dist
        t = 0.0
        while t < dist:
            a = (p0[0] + ux * t, p0[1] + uy * t)
            b = (p0[0] + ux * min(t + dash, dist), p0[1] + uy * min(t + dash, dist))
            d.line([a, b], fill=color, width=width)
            t += dash + gap

    dashed_line((x0 + radius, y0), (x1 - radius, y0))
    dashed_line((x0 + radius, y1), (x1 - radius, y1))
    dashed_line((x0, y0 + radius), (x0, y1 - radius))
    dashed_line((x1, y0 + radius), (x1, y1 - radius))
    for cx, cy, start in ((x0 + radius, y0 + radius, 180), (x1 - radius, y0 + radius, 270),
                          (x1 - radius, y1 - radius, 0), (x0 + radius, y1 - radius, 90)):
        d.arc([cx - radius, cy - radius, cx + radius, cy + radius], start, start + 90,
              fill=color, width=width)


def make(asset):
    aid, rel, w, h, title, sub, kind, tone = asset
    accent = CYAN if tone == "cyan" else ORANGE
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    draw_grid(d, w, h)

    pad = int(w * 0.055)
    box = [pad, pad, w - pad, h - pad]
    d.rounded_rectangle(box, radius=32, fill=PANEL)
    dashed_round_rect(d, box, color=(64, 72, 84))

    # 顶部色条
    d.rounded_rectangle([pad, pad, w - pad, pad + 14], radius=7, fill=accent)

    # 角标 ID
    f_id = load_font(int(w * 0.075))
    d.text((pad + 34, pad + 56), aid, font=f_id, fill=accent)

    f_tag = load_font(int(w * 0.026))
    tag = "PHOTO PLACEHOLDER" if kind == "photo" else "VIDEO POSTER PLACEHOLDER"
    d.text((pad + 40, pad + 56 + int(w * 0.095)), tag, font=f_tag, fill=DIM)

    # 中间相机 / 播放图标
    cx, cy = w // 2, int(h * 0.47)
    r = int(w * 0.10)
    if kind == "photo":
        d.rounded_rectangle([cx - r, cy - int(r * 0.72), cx + r, cy + int(r * 0.72)],
                            radius=int(r * 0.22), outline=accent, width=5)
        d.ellipse([cx - int(r * 0.36), cy - int(r * 0.36), cx + int(r * 0.36), cy + int(r * 0.36)],
                  outline=accent, width=5)
        d.rounded_rectangle([cx - int(r * 0.62), cy - int(r * 0.86), cx - int(r * 0.18), cy - int(r * 0.66)],
                            radius=6, outline=accent, width=4)
    else:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=accent, width=5)
        d.polygon([(cx - int(r * 0.28), cy - int(r * 0.44)),
                   (cx - int(r * 0.28), cy + int(r * 0.44)),
                   (cx + int(r * 0.50), cy)], fill=accent)

    # 标题区
    f_title = load_font(int(w * 0.052))
    f_sub = load_font(int(w * 0.028))
    ty = int(h * 0.60)

    def wrap(text, font, max_w):
        lines, cur = [], ""
        for ch in text:
            if d.textlength(cur + ch, font=font) > max_w:
                lines.append(cur)
                cur = ch
            else:
                cur += ch
        if cur:
            lines.append(cur)
        return lines

    for line in wrap(title, f_title, w - 2 * pad - 80):
        d.text((pad + 40, ty), line, font=f_title, fill=TXT)
        ty += int(w * 0.068)

    ty += 10
    for line in wrap(sub, f_sub, w - 2 * pad - 80):
        d.text((pad + 40, ty), line, font=f_sub, fill=DIM)
        ty += int(w * 0.042)

    # 底部规格
    f_meta = load_font(int(w * 0.024))
    meta = f"{w} x {h}   |   {rel}"
    d.text((pad + 40, h - pad - 96), meta, font=f_meta, fill=DIM)
    d.text((pad + 40, h - pad - 56), "REPLACE THIS FILE — KEEP NAME & SIZE", font=f_meta, fill=accent)

    out = os.path.join(ROOT, "assets", rel)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.save(out, "PNG", optimize=True)
    return out


def main():
    made = []
    for a in ASSETS:
        p = make(a)
        made.append(p)
        print("ok", p)

    # 视频占位说明（不生成伪 mp4，避免 Remotion 误引用坏文件）
    for vid, fname, poster, dur, desc, tip in VIDEOS:
        note = os.path.join(ROOT, "assets", "videos", fname + ".placeholder")
        with open(note, "w", encoding="utf-8") as f:
            f.write(
                f"{vid} 视频占位\n"
                f"====================\n"
                f"期望文件: assets/videos/{fname}\n"
                f"静帧占位: assets/{poster}\n"
                f"时长: {dur}s   画幅: 1080x1920 (9:16)   帧率: 30fps   编码: H.264 / yuv420p\n"
                f"内容: {desc}\n"
                f"拍摄要点: {tip}\n\n"
                f"补齐方式: 把真实视频命名为 {fname} 放到 assets/videos/ 即可，\n"
                f"Remotion 里把 SHOT_READY.{vid} 置为 true，就会从静帧切换到 <OffthreadVideo>。\n"
            )

    manifest = {
        "project": "录音豆 RecBean — 宣传素材包",
        "generated_by": "marketing/tools/gen_placeholders.py",
        "video": {"width": 1080, "height": 1920, "fps": 30, "ratio": "9:16"},
        "photos": [
            {"id": a[0], "file": "assets/" + a[1], "width": a[2], "height": a[3],
             "title": a[4], "brief": a[5], "status": "placeholder"}
            for a in ASSETS if a[6] == "photo"
        ],
        "posters": [
            {"id": a[0], "file": "assets/" + a[1], "width": a[2], "height": a[3],
             "title": a[4], "brief": a[5], "status": "placeholder"}
            for a in ASSETS if a[6] == "frame"
        ],
        "videos": [
            {"id": v[0], "file": "assets/videos/" + v[1], "poster": "assets/" + v[2],
             "duration_s": v[3], "width": 1080, "height": 1920, "fps": 30,
             "brief": v[4], "tips": v[5], "status": "missing"}
            for v in VIDEOS
        ],
    }
    with open(os.path.join(ROOT, "assets", "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    print(f"\n共生成 {len(made)} 张占位图 + {len(VIDEOS)} 个视频占位说明 + manifest.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
