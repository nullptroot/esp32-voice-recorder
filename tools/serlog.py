#!/usr/bin/env python3
"""串口日志抓取（避免 DTR/RTS 误复位）。

用法:
  python3 tools/serlog.py --port /dev/ttyUSB0 --seconds 20 --out /tmp/boot.log
  python3 tools/serlog.py --seconds 0 --out /tmp/run.log &   # 0 = 一直抓
  python3 tools/serlog.py --reset-only                        # 只做一次复位脉冲
"""
import argparse, sys, time, serial

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyUSB0')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--seconds', type=float, default=15)
    ap.add_argument('--out', default='/tmp/ser.log')
    ap.add_argument('--reset-only', action='store_true')
    ap.add_argument('--reset', action='store_true', help='开始前做一次 EN 复位脉冲')
    a = ap.parse_args()

    s = serial.Serial()
    s.port = a.port
    s.baudrate = a.baud
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    # CH340: DTR->EN(reset), RTS->GPIO0(boot). 都拉高=不复位/正常启动
    s.dtr = False
    s.rts = False

    if a.reset_only or a.reset:
        s.dtr = True
        time.sleep(0.15)
        s.dtr = False
        time.sleep(0.05)
    if a.reset_only:
        s.close()
        return

    t0 = time.time()
    buf = b''
    with open(a.out, 'ab') as f:
        while a.seconds <= 0 or time.time() - t0 < a.seconds:
            d = s.read(4096)
            if not d:
                continue
            f.write(d)
            f.flush()
            sys.stdout.write(d.decode('utf-8', 'replace'))
            sys.stdout.flush()
    s.close()

if __name__ == '__main__':
    main()
