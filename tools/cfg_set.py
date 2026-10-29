#!/usr/bin/env python3
"""改设备单个配置项：python3 tools/cfg_set.py <ip> <key> <value>"""
import json, sys, urllib.request

ip, key, val = sys.argv[1], sys.argv[2], sys.argv[3]
base = f'http://{ip}'

def get(p):
    with urllib.request.urlopen(base + p, timeout=15) as r:
        return json.load(r)

cfg = get('/api/config')['config']
old = cfg.get(key)
if key in ('vad_enable', 'upload_on_record', 'upload_on_boot', 'delete_after_upload',
           'sleep_enable', 'bench_mode', 'wifi_off_while_recording',
           'format_sd_if_mount_failed', 'sound_active_low'):
    cfg[key] = val.lower() in ('1', 'true', 'yes', 'on')
elif isinstance(old, str):
    cfg[key] = val
else:
    cfg[key] = int(val)

req = urllib.request.Request(base + '/api/config', data=json.dumps(cfg).encode(), method='POST',
                             headers={'Content-Type': 'application/json'})
with urllib.request.urlopen(req, timeout=20) as r:
    resp = r.read().decode()
print(f'{key}: {old} -> {cfg[key]}  resp={resp[:200]}')
