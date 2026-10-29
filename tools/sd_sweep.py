#!/usr/bin/env python3
"""SD 原始扇区读自检扫描（走设备 /api/sdtest，绕过文件系统）。

用法: python3 tools/sd_sweep.py [设备IP] [起始块] [结束块] [每次覆盖块数]
默认扫描 0..200000 块（约 100MB），nsec=32（每次 16KB 读）。
"""
import json, sys, time, urllib.request, urllib.error

ip = sys.argv[1] if len(sys.argv) > 1 else '192.168.5.32'
blk0 = int(sys.argv[2]) if len(sys.argv) > 2 else 0
blk1 = int(sys.argv[3]) if len(sys.argv) > 3 else 200000
step = int(sys.argv[4]) if len(sys.argv) > 4 else 4096
nsec = 32

total_fail = 0
bad = []
t0 = time.time()
starts = list(range(blk0, blk1, step))
for i, s in enumerate(starts):
    url = f'http://{ip}/api/sdtest?start={s}&count={step}&nsec={nsec}'
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            d = json.load(r)
    except Exception as e:
        print(f'[{i+1}/{len(starts)}] start={s} HTTP错误: {e}', flush=True)
        continue
    nf = d.get('failed', -1)
    total_fail += max(nf, 0)
    if nf:
        bad.extend(d.get('failed_blocks', []))
        print(f'[{i+1}/{len(starts)}] start={s} 失败={nf} {d.get("failed_blocks")}', flush=True)
    elif (i + 1) % 10 == 0:
        print(f'[{i+1}/{len(starts)}] 已扫 {s} 块, 累计失败 {total_fail}, 用时 {time.time()-t0:.0f}s', flush=True)

print(f'完成: 扫描 {blk0}..{blk1} 共 {len(starts)} 段, 失败块数 {total_fail}, 用时 {time.time()-t0:.0f}s')
if bad:
    print('坏块列表:', bad[:100])
