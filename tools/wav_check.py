#!/usr/bin/env python3
"""WAV 体检：头信息、时长、RMS/峰值/削顶比例、分段静音统计。"""
import sys, wave, struct, math

def db(x):
    return 20 * math.log10(x) if x > 0 else -999.0

path = sys.argv[1]
w = wave.open(path, 'rb')
n, rate, ch, sw = w.getnframes(), w.getframerate(), w.getnchannels(), w.getsampwidth()
frames = w.readframes(n)
w.close()
samples = struct.unpack('<%dh' % (len(frames) // 2), frames)
peak = max(abs(s) for s in samples) if samples else 0
rms = math.sqrt(sum(s * s for s in samples) / len(samples)) if samples else 0
clip = sum(1 for s in samples if abs(s) >= 32000)
# 每 0.5 秒一段的 RMS，用于看开头是否有数字静音
win = rate // 2
segs = []
for i in range(0, len(samples), win):
    chunk = samples[i:i + win]
    if not chunk:
        break
    r = math.sqrt(sum(s * s for s in chunk) / len(chunk))
    segs.append(db(r / 32768.0))
print(f'文件: {path}')
print(f'采样率 {rate} Hz, 声道 {ch}, 位宽 {sw*8} bit, 帧数 {n}, 时长 {n/rate:.2f} s')
print(f'峰值 {peak} ({db(peak/32768.0):.1f} dBFS), RMS {rms:.0f} ({db(rms/32768.0):.1f} dBFS), 削顶样本 {clip}')
print('前 3 秒每 0.5s 的 dBFS:', ' '.join(f'{x:.0f}' for x in segs[:6]))
print('全片每 0.5s 最小/最大 dBFS:', f'{min(segs):.0f} / {max(segs):.0f}')
print('有声段落(>-35dBFS)数量:', sum(1 for x in segs if x > -35), '/', len(segs))
