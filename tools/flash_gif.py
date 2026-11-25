# -*- coding: utf-8 -*-
"""
flash_gif.py —— 一条命令换掉屏幕上的 GIF（走 U5 外挂 16MB flash）

    python tools/flash_gif.py 你的.gif [COM口]

流程：GIF -> bin -> 通过串口写进 U5 -> 板子自动重新加载并跳回 GIF 页。

★ 和旧版（gif_data.h 编译进固件）的区别：
   旧版每次换图要改源码、编译、烧录，约 1 分钟，预算只有 1.8MB。
   现在固件不含 GIF，图直接写进 U5，预算约 15MB，**10 秒完成，且不重启**。

选端口：按 VID 找真 USB 串口（ESP32 原生 303A / CH343 1A86），不抓蓝牙口。
完整输出写到 en/flash_gif_last.log。
"""

import json
import os
import queue
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SKETCH = os.path.join(ROOT, "weather_clock")
GIF2BIN = os.path.join(HERE, "gif2bin.py")
UPLOAD = os.path.join(HERE, "upload_gif.py")
BIN = os.path.join(HERE, "last_gif.bin")

CLI_CANDIDATES = [
    r"C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
    r"D:\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
    r"C:\Program Files (x86)\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
]
CLI = next((c for c in CLI_CANDIDATES if os.path.isfile(c)), CLI_CANDIDATES[0])
LASTLOG = os.path.join(ROOT, "en", "flash_gif_last.log")

# U5 16MB，头部/表结构只占几百字节。留 2MB 余量给以后可能加的其它数据。
BUDGET_KB = 14000

LOG_BUF = []


def log(msg=""):
    print(msg, flush=True)
    LOG_BUF.append(msg)


def flush_log():
    try:
        os.makedirs(os.path.dirname(LASTLOG), exist_ok=True)
        with open(LASTLOG, "w", encoding="utf-8") as f:
            f.write("\n".join(LOG_BUF) + "\n")
    except Exception:
        pass


def run(args, timeout=600):
    r = subprocess.run(args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


# ---------------- 端口 ----------------
USB_VIDS = ("1a86", "10c4", "0403", "303a")


def usb_ports():
    rc, out = run([CLI, "board", "list", "--format", "json"], timeout=120)
    found = []
    if rc != 0:
        return found
    try:
        data = json.loads(out[out.find("{"):])
    except Exception:
        return found
    for item in data.get("detected_ports", []):
        p = item.get("port", {})
        addr = (p.get("address") or "").upper()
        if not re.fullmatch(r"COM\d+", addr):
            continue
        props = p.get("properties") or {}
        vid = (props.get("vid") or "").lower().replace("0x", "")
        pid = (props.get("pid") or "").lower().replace("0x", "")
        found.append((addr, vid, pid, p.get("protocol_label") or ""))
    return found


def pick_port(explicit=None):
    ports = usb_ports()
    if explicit:
        return explicit.strip().upper(), ports
    usb = [p for p in ports if p[1] in USB_VIDS]
    if usb:
        usb.sort(key=lambda p: (p[1] != "303a", p[1] != "1a86"))
        return usb[0][0], ports
    return None, ports


def port_report(ports):
    if not ports:
        return "    一个串口都没枚举出来（连蓝牙的都没有）"
    return "\n".join("    %-6s vid=%s pid=%s %s" % (a, v or "-", d or "-", l)
                     for a, v, d, l in ports)


# ---------------- 转换 ----------------

def convert(gif, kb):
    """返回 (ok, 帧数, 数据KB)"""
    rc, out = run([sys.executable, GIF2BIN, gif,
                   "-o", BIN,
                   "--size", "240",
                   "--fit", "cover",
                   "--max-kb", str(kb)], timeout=600)
    log(out.strip())
    if rc != 0:
        return False, 0, 0
    frames = 0
    kb_out = 0.0
    m = re.search(r"帧数\s*(\d+)", out)
    if m:
        frames = int(m.group(1))
    m = re.search(r"RLE\s*([\d.]+)\s*KB", out)
    if m:
        kb_out = float(m.group(1))
    return True, frames, kb_out


# ---------------- 上传 ----------------

def upload(port):
    rc, out = run([sys.executable, UPLOAD, BIN, "-p", port], timeout=900)
    return rc == 0, out


# ---------------- 验证 ----------------

def verify(expect_frames):
    """直接读串口（不用 monitor，省得开子进程），核对帧数与帧率。"""
    log("[验证] 读串口 10 秒 ...")
    try:
        import serial
        from serial.tools import list_ports
        dev = [p.device for p in list_ports.comports() if p.vid == 0x303A]
        if not dev:
            log("    板子没枚举，跳过")
            return
        s = serial.Serial(dev[0], 115200, timeout=0.2)
    except Exception as e:
        log("    （打不开串口，跳过：%s）" % e)
        return

    s.dtr = False
    s.rts = False
    frames = None
    fps = None
    t0 = time.time()
    buf = b""
    try:
        while time.time() - t0 < 10:
            buf += s.read(8192)
    finally:
        s.close()

    txt = buf.decode("utf-8", "replace")
    for line in txt.split("\n"):
        m = re.search(r"\[gif\]\s*U5:\s*(\d+)\s*帧", line)
        if m:
            frames = int(m.group(1))
        if "[gif]" in line and "fps" in line and fps is None:
            fps = line.strip()
    if frames is None:
        log("    没抓到 [gif] U5 行（板子刚上传完可能还没打印），看屏幕确认")
        return
    ok = (expect_frames == 0 or frames == expect_frames)
    log("    屏上在播 %d 帧%s" % (frames, "" if ok else "，bin 里是 %d 帧 —— 对不上！" % expect_frames))
    if fps:
        log("    " + fps)


# ---------------- 主流程 ----------------

def main():
    if len(sys.argv) < 2:
        log("用法: python tools/flash_gif.py 你的.gif [COM口]")
        return 1
    gif = sys.argv[1]
    if not os.path.isfile(gif):
        log("找不到文件: %s" % gif)
        return 1

    log("=== flash_gif(U5) %s ===" % time.strftime("%Y-%m-%d %H:%M:%S"))
    log("GIF: %s  (%.1f KB)" % (os.path.basename(gif), os.path.getsize(gif) / 1024.0))

    explicit = sys.argv[2] if len(sys.argv) > 2 else None
    port, ports = pick_port(explicit)
    if not port:
        log("\n没找到开发板。当前枚举到的串口：")
        log(port_report(ports))
        log("\n板子没插上，或者 USB 线只供电不传数据。")
        return 1
    log("端口: %s" % port)

    log("\n[1/2] 转换（预算 %d KB）..." % BUDGET_KB)
    ok, frames, kb = convert(gif, BUDGET_KB)
    if not ok:
        log("\n转换失败（见上面 Pillow 的报错）")
        return 1
    log("    %d 帧  %.1f KB" % (frames, kb))

    log("[2/2] 写入 U5 ...")
    ok, out = upload(port)
    for line in out.strip().split("\n"):
        if line.strip():
            log("    " + line.strip())
    if not ok:
        log("\n上传失败。常见原因：")
        log("  · 固件还没升级到 U5 版（串口发 U 没反应）→ 先烧一次 weather_clock")
        log("  · 板子正在深睡 / 已断电")
        log("  · USB 线只供电")
        return 1

    verify(frames)
    log("")
    log("换好了，屏幕已经在放新图。再换一个，直接拖到 转换GIF.bat 上。")
    return 0


if __name__ == "__main__":
    code = 1
    try:
        code = main()
    finally:
        flush_log()
    sys.exit(code)
