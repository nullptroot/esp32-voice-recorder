#!/usr/bin/env bash
# 用静帧生成「真实可渲染」的占位 mp4（本机需要 ffmpeg）。
#
# 远端构建机没有 ffmpeg，所以默认只给 PNG 静帧 + 规格说明；
# 在你的电脑上跑这个脚本，就能得到 6 段 1080x1920/30fps 的占位视频，
# 让 Remotion 在素材没拍之前也能完整渲染出片。
#
# 用法:
#   bash marketing/tools/make-placeholder-videos.sh
#   bash marketing/tools/make-placeholder-videos.sh --force   # 覆盖已有真实视频（慎用）

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/assets/videos"
FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "❌ 没找到 ffmpeg。macOS: brew install ffmpeg / Ubuntu: sudo apt install ffmpeg"
  exit 1
fi

# id | 输出文件 | 静帧 | 时长
SHOTS=(
  "V01|v01-voice-trigger.mp4|f01-voice-trigger.png|12"
  "V02|v02-web-live-level.mp4|f02-web-live-level.png|10"
  "V03|v03-upload-success.mp4|f03-upload-success.png|8"
  "V04|v04-sleep-power.mp4|f04-sleep-power.png|8"
  "V05|v05-config-wake.mp4|f05-config-wake.png|8"
  "V06|v06-serial-log.mp4|f06-serial-log.png|6"
  "V07|v07-soldering-pins.mp4|f07-soldering.png|20"
)

for row in "${SHOTS[@]}"; do
  IFS='|' read -r id name poster dur <<< "$row"
  src="$ROOT/assets/frames/$poster"
  dst="$OUT/$name"

  if [[ -f "$dst" && $FORCE -eq 0 ]]; then
    echo "⏭  $id 已存在真实视频，跳过（--force 可覆盖）"
    continue
  fi
  [[ -f "$src" ]] || { echo "❌ 缺少静帧: $src"; exit 1; }

  ffmpeg -hide_banner -loglevel error -y \
    -loop 1 -i "$src" \
    -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 \
    -t "$dur" -r 30 \
    -vf "scale=1080:1920:force_original_aspect_ratio=decrease,pad=1080:1920:(ow-iw)/2:(oh-ih)/2,format=yuv420p" \
    -c:v libx264 -preset medium -crf 20 -profile:v high -level 4.0 \
    -c:a aac -b:a 96k -shortest \
    "$dst"

  echo "✅ $id -> assets/videos/$name (${dur}s)"
done

echo
echo "完成。回到 Remotion 工程，把 src/config.ts 里的 SHOT_READY.* 置为 true 即可。"
