# -*- coding: utf-8 -*-
"""音乐页推流中调音量：确认 page=MUSIC、vol 变化、rd 速率不掉；最后双键回时钟"""
import serial, time, re, math, struct, threading, urllib.request, json, sys

IP = "10.186.80.208"
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM15"
SR = 31250
DUR = 45.0
base = "http://%s" % IP

s = serial.Serial()
s.port = PORT
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()
time.sleep(1.5)
s.reset_input_buffer()


def rd(d=0.6):
    b = bytearray()
    t0 = time.time()
    while time.time() - t0 < d:
        x = s.read(4096)
        if x:
            b.extend(x)
    return bytes(b).decode("utf-8", "replace")


def ask(cmd=b"P", d=0.6):
    s.reset_input_buffer()
    s.write(cmd)
    return rd(d).strip().replace("\r", "")


def page_now():
    t = ask()
    m = re.search(r"page=(\d)/7 (\w+)", t)
    return (int(m.group(1)), m.group(2), t) if m else (None, None, t)


def h():
    s.reset_input_buffer()
    s.write(b"h")
    t = rd(1.0)
    m = re.search(r"rd=(\d+)", t)
    u = re.search(r"under=(\d+) late=(\d+)", t)
    return (int(m.group(1)) if m else None), (u.groups() if u else None), t


def push():
    n = int(SR * DUR)
    out = bytearray(n * 2)
    ph = 0.0
    step = SR // 2
    for k in range(n):
        seq = (k // step) % 3
        f = 440.0 if seq == 0 else (554.0 if seq == 1 else 659.0)
        ph += 2.0 * math.pi * f / SR
        struct.pack_into("<h", out, k * 2, int(11000 * math.sin(ph)))
    raw = bytes(out)
    urllib.request.urlopen(base + "/start?len=%d&name=tone" % len(raw), timeout=20).read()
    CH = 16384
    off = 0
    while off < len(raw):
        req = urllib.request.Request(base + "/pcm", data=raw[off:off + CH], method="POST")
        urllib.request.urlopen(req, timeout=40).read()
        off += CH
    urllib.request.urlopen(base + "/eos", timeout=10).read()


# 1) 走到音乐页（用 P 确认，最多 7 次）
for _ in range(7):
    p, n, _t = page_now()
    if p == 5:
        break
    s.write(b"n")
    rd(0.5)
p, n, t = page_now()
print("当前页:", t)
assert p == 5, "没走到音乐页"
print("音量起点:", json.loads(urllib.request.urlopen(base + "/status", timeout=8).read())["vol"], "%")

threading.Thread(target=push, daemon=True).start()
time.sleep(6.0)

t1 = time.time()
r1, u1, _ = h()
print("推流中 rd=%s under/late=%s" % (r1, u1))

for c in (b"l", b"l", b"L", b"L", b"L"):
    s.write(c)
    print("  ", rd(0.5).strip().replace("\r", "").replace("\n", " | "))

time.sleep(7.0)
t2 = time.time()
r2, u2, _ = h()
p, n, t = page_now()
print("长按后:", t)
print("推流中 rd=%s under/late=%s" % (r2, u2))
if r1 and r2:
    rate = (r2 - r1) / (t2 - t1)
    print("rd 速率 = %.0f B/s（目标 62500，%.1f%%）" % (rate, rate / 625.0))

# 双键回时钟
s.write(b"H")
time.sleep(0.8)
p, n, t = page_now()
print("双键后:", t)
s.close()
