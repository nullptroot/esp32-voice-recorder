#!/usr/bin/env python3
"""自动录音验证：start -> 等待 -> stop -> 轮询 idle -> 校验文件大小。

用法: python3 tools/rec_test.py <设备IP> <每次秒数> <次数> [等待上传秒数]
记录每个文件的名称/大小/期望大小，最后输出汇总表。
"""
import json, sys, time, urllib.request

ip = sys.argv[1] if len(sys.argv) > 1 else '192.168.5.32'
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10
times = int(sys.argv[3]) if len(sys.argv) > 3 else 10
wait_up = float(sys.argv[4]) if len(sys.argv) > 4 else 8

BASE = f'http://{ip}'

def get(path, timeout=15):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return json.load(r)

def post(path, body=b'{}', timeout=30):
    req = urllib.request.Request(BASE + path, data=body, method='POST')
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)

def files():
    return get('/api/files').get('files', [])

def wait_idle(timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st = get('/api/status')
        if st.get('state') == 'idle':
            return st
        time.sleep(0.5)
    return get('/api/status')

ok = 0
rows = []
for i in range(times):
    before = {f['name'] for f in files()}
    post('/api/record/start')
    time.sleep(secs)
    post('/api/record/stop')
    st = wait_idle(secs + 30)
    time.sleep(1.0)
    after = files()
    new = [f for f in after if f['name'] not in before]
    elapsed = st.get('record_seconds', 0)
    if new:
        for f in new:
            rows.append((i + 1, f['name'], f.get('size', 0), elapsed, st.get('last_error', '')))
        good = all(f.get('size', 0) > 1000 for f in new)
        ok += 1 if good else 0
        print(f'[{i+1}/{times}] OK {[ (f["name"], f.get("size")) for f in new]} 录音时长={elapsed}s', flush=True)
    else:
        rows.append((i + 1, '-', 0, elapsed, st.get('last_error', '')))
        print(f'[{i+1}/{times}] 没有产生文件! state={st.get("state")} last_error={st.get("last_error")}', flush=True)
    time.sleep(1.0)

print('--- 汇总 ---')
for r in rows:
    print(f'#{r[0]:2d} {r[1]:48s} size={r[2]:8d} 时长={r[3]:4d}s err={r[4]}')
print(f'成功 {ok}/{times}')
st = wait_idle(60)
time.sleep(wait_up)
print('上传后状态:', json.dumps({k: st.get(k) for k in ('uploaded_files', 'pending_files', 'failed_uploads', 'last_upload')}, ensure_ascii=False))
