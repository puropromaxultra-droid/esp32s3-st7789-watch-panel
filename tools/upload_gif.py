#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
upload_gif.py —— 把 gif2bin.py 生成的 .bin 通过 USB 串口传进板子的 U5（16MB flash）

用法:
    python upload_gif.py                       # 自动找板子，用 tools/last_gif.bin
    python upload_gif.py my.bin
    python upload_gif.py my.bin -p COM15
    python upload_gif.py my.bin --win 6        # 调流水窗口（块数）

协议（跟 weather_clock/u5flash.h 里的一致）:
    U                    → UOK <sector> <capacity>
    H <total> <crc32>    → HOK          （擦除，约 17ms/4KB）
    D <seq> <len>\\n + 原始数据 → A <seq>
    E                    → EOK

速度：
    板子写 2048B（8 页）只要约 3.2ms，剩下全是 USB 往返。
    所以发送端不逐块等 ACK，而是维持 N 块「在途」，收到最老的 ACK 再补发——
    把往返延迟摊到 N 块上。窗口上限受板子接收缓冲（16384B）约束。
"""

import argparse
import struct
import sys
import time
import zlib

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("需要 pyserial: pip install pyserial")

BLK = 2048
BAUD = 115200          # USB CDC 下波特率无效，写什么都一样
WIN = 4                # 流水窗口（块），4×2048 = 8KB 在途，留足接收余量


class Wire:
    """带行缓冲的串口读取器 —— 一次 read() 拿一大把，不逐字节系统调用。"""

    def __init__(self, ser):
        self.ser = ser
        self.buf = b""

    def line(self, timeout=30.0):
        t0 = time.time()
        while True:
            i = self.buf.find(b"\n")
            if i >= 0:
                s = self.buf[:i]
                self.buf = self.buf[i + 1:]
                return s.decode("utf-8", "replace").strip()
            left = timeout - (time.time() - t0)
            if left <= 0:
                raise TimeoutError("等应答超时（%.0fs），残留 %r"
                                   % (timeout, self.buf[:80]))
            n = self.ser.in_waiting
            chunk = self.ser.read(n if n else 1)
            if chunk:
                self.buf += chunk

    def wait(self, *prefixes, timeout=30.0, quiet=False):
        """读到以 prefixes 之一开头的行；板子的其它日志行跳过（可选打印）。"""
        t0 = time.time()
        while time.time() - t0 < timeout:
            line = self.line(max(1.0, timeout - (time.time() - t0)))
            if not line:
                continue
            for p in prefixes:
                if line.startswith(p):
                    return line
            if line.startswith("ERR") or line.startswith("EBAD"):
                sys.exit("板子报错: %r" % line)
            if not quiet:
                print("  · " + line)
        raise TimeoutError("没等到 %s" % (prefixes,))


def pick_port(want=None):
    if want:
        return want
    cands = [p.device for p in list_ports.comports() if p.vid == 0x303A]
    if not cands:
        sys.exit("没找到 ESP32（VID 303A）。插 USB 后用 -p 指定端口。")
    return cands[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin", nargs="?", default="tools/last_gif.bin")
    ap.add_argument("-p", "--port")
    ap.add_argument("--win", type=int, default=WIN,
                    help="流水窗口块数（1=逐块等 ACK，默认 %d）" % WIN)
    a = ap.parse_args()

    data = open(a.bin, "rb").read()
    if data[:4] != b"GIFB":
        sys.exit("不是 GIFB blob: %s" % a.bin)
    frames = struct.unpack_from("<H", data, 10)[0]
    crc = zlib.crc32(data) & 0xFFFFFFFF
    total = len(data)
    print("文件 %s  %d 字节  帧数 %d  crc32 %08X" % (a.bin, total, frames, crc))

    win = max(1, min(a.win, 7))     # 7×2048 = 14KB，别把 16KB 接收缓冲撑爆
    port = pick_port(a.port)
    print("端口 %s" % port)
    ser = serial.Serial(port, BAUD, timeout=3, write_timeout=20)
    try:
        try:
            ser.set_buffer_size(rx_size=65536, tx_size=65536)
        except Exception:
            pass
        time.sleep(0.3)
        ser.reset_input_buffer()
        w = Wire(ser)

        # --- 进入上传模式 ---
        ser.write(b"U\n")
        r = w.wait("UOK", timeout=15)
        _, sector, cap = r.split()
        print("板子就绪  扇区 %s  容量 %.0f MB" % (sector, int(cap) / 1048576.0))

        # --- 擦除 ---
        kb = (total + 1023) // 1024
        print("擦除 %d KB ..." % kb)
        t0 = time.time()
        ser.write(("H %d %x\n" % (total, crc)).encode())
        w.wait("HOK", timeout=30 + kb * 0.06)
        t_erase = time.time() - t0
        print("擦除完成 %.1fs（%.0f KB/s）" % (t_erase, kb / max(t_erase, 0.001)))

        # --- 数据块（滑动窗口） ---
        nblk = (total + BLK - 1) // BLK
        print("传输 %d 块 × %d B，流水窗口 %d 块（%d KB 在途）"
              % (nblk, BLK, win, win * BLK // 1024))
        t0 = time.time()
        sent = 0
        ack = 0
        while ack < nblk:
            # 填满窗口
            while sent < nblk and sent - ack < win:
                off = sent * BLK
                chunk = data[off:off + BLK]
                ser.write(("D %d %d\n" % (sent + 1, len(chunk))).encode() + chunk)
                sent += 1
            # 只等最老的那块 —— 后面几块已经在飞了
            w.wait("A %d" % (ack + 1), timeout=20, quiet=True)
            ack += 1
            if ack % 32 == 0 or ack == nblk:
                done = min(total, ack * BLK)
                pct = done * 100 // total
                spd = done / 1024.0 / max(0.001, time.time() - t0)
                bar = "#" * (pct // 4) + "." * (25 - pct // 4)
                print("  [%s] %3d%%  %6.1f KB/s" % (bar, pct, spd), end="\r")
        print()

        # --- 校验 ---
        ser.write(b"E\n")
        r = w.wait("EOK", timeout=30)
        dt = time.time() - t0
        print("校验通过  EOK  传输 %.1fs (%.0f KB/s)  总耗时 %.1fs"
              % (dt, total / 1024.0 / dt, t_erase + dt))
        # EOK 先发、板子的统计日志后发，多读一会儿把它带回来
        t1 = time.time()
        tail = []
        while time.time() - t1 < 1.5:
            try:
                ln = w.line(0.3)
            except TimeoutError:
                break
            if ln:
                tail.append(ln)
        for ln in tail:
            print("  · " + ln)
        print("\n板子会自动重新加载并跳回 GIF 页。")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
