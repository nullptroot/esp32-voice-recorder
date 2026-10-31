# 录音豆 RecBean · 宣传素材包

> 这个文件夹是为了**把项目讲清楚，并交给 AI 做成一条能发抖音/小红书的视频**。
> 里面没有成品视频，只有：项目说明 + 技术复盘 + 素材占位 + 可直接粘贴的 Remotion 提示词。

---

## 1. 先看哪份

| 顺序 | 文档 | 你会得到 |
|---|---|---|
| 1 | [`01-项目介绍.md`](01-项目介绍.md) | 项目是干嘛的、为什么做、值多少钱、现在什么进度 |
| 2 | [`02-技术实现与问题修复.md`](02-技术实现与问题修复.md) | 架构 + 踩坑的「现象/根因/修复」，视频硬核素材 |
| 3 | [`03-素材清单与拍摄指南.md`](03-素材清单与拍摄指南.md) | 要拍哪些照片/视频、怎么拍、怎么替换 |
| 4 | [`04-Remotion-视频提示词.md`](04-Remotion-视频提示词.md) | **直接粘给 AI**，生成 90s 竖屏片 |
| 5 | [`05-AI-配图提示词.md`](05-AI-配图提示词.md) | 文生图 / 图生图的 prompt 和红线 |
| 6 | [`06-旁白与字幕脚本.md`](06-旁白与字幕脚本.md) | 逐句口播、屏幕大字、封面文案、发布文案 |

---

## 2. 目录结构

```
marketing/
├── README.md                      ← 你在这里
├── 01-项目介绍.md
├── 02-技术实现与问题修复.md
├── 03-素材清单与拍摄指南.md
├── 04-Remotion-视频提示词.md
├── 05-AI-配图提示词.md
├── 06-旁白与字幕脚本.md
├── assets/
│   ├── manifest.json              机器可读的素材清单（16 张图 + 6 段视频）
│   ├── photos/                    10 张照片占位（真实 PNG，1080 宽）
│   ├── frames/                     6 张视频静帧占位（1080×1920）
│   └── videos/                     6 个 *.mp4.placeholder 规格说明
└── tools/
    ├── gen_placeholders.py        重新生成占位图 / manifest
    └── make-placeholder-videos.sh 用 ffmpeg 把静帧变成可渲染的占位 mp4
```

---

## 3. 三步跑通

### 第 1 步：拿占位图先出片

```bash
# 在装了 ffmpeg 的电脑上（可选，没有就跳过，Remotion 会用静帧兜底）
bash marketing/tools/make-placeholder-videos.sh
```

把 `marketing/assets/` 整个拷到 Remotion 工程的 `public/assets/`，
然后把 `04-Remotion-视频提示词.md` 从「一、任务」到「九、验收」整段粘给 AI。

### 第 2 步：补真实素材

按 `03-素材清单与拍摄指南.md` 拍完，**同名覆盖**：

- 照片 → `assets/photos/pXX-*.png`
- 视频 → `assets/videos/vXX-*.mp4`
- 然后把 `src/config.ts` 里对应 `SHOT_READY.Vxx = true`

### 第 3 步：出片

```bash
npx remotion render RecBeanPromo out/recbean-promo.mp4 --codec=h264 --crf=18
npx remotion still RecBeanPromo out/cover.png --frame=0
```

---

## 4. 素材现状

| 类型 | 数量 | 状态 |
|---|---:|---|
| 照片占位（PNG） | 10 | ✅ 已生成，等实拍/AI 替换 |
| 视频静帧（PNG） | 6 | ✅ 已生成，可直接缓推成片 |
| 视频（mp4） | 6 | ⏳ 待补（规格见 `assets/videos/*.placeholder`） |
| 音频（BGM/口播/SFX） | — | ⏳ 待补，Remotion 里用开关降级 |

机器可读清单：`assets/manifest.json`（含每张图的尺寸、用途、状态）。

---

## 5. 三条内容红线

1. **别把「硬件跑通」说成「产品完成」**。后端转写/日记还没做，画板还没画。
2. **别编数字**。22.3s 唤醒上传、121 条记录、10/10 连录 是真实实测；续航、误触发率**没测**，不要给。
3. **AI 图别装实拍**。概念图标「概念图」；接线图必须实拍，否则观众照着接会烧板子。

---

## 6. 成本口径

- 物料 BOM ≈ **¥65**（不含外壳、PCB 打样、快递）
- 对外统一说「**65 块**」「**百元以内**」，别说「精确 65.4 元」
- 市面 600~700 是调研区间，**不点名品牌**

---

## 7. 重新生成占位素材

改了尺寸或加新素材，改 `tools/gen_placeholders.py` 里的 `ASSETS` / `VIDEOS` 两个列表，然后：

```bash
python3 marketing/tools/gen_placeholders.py
```

会重写 `assets/photos/`、`assets/frames/`、`assets/videos/*.placeholder` 和 `manifest.json`。
**不要手动改 manifest.json**，它由脚本生成。
