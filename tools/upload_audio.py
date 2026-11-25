#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
upload_audio.py —— 把 wav2aud.py 生成的 .bin 通过串口传进板子 U5 的音频区（8MB 起）

用法:
    python upload_audio.py                      # 自动找板子，用 tools/last_aud.bin
    python upload_audio.py my.bin
    python upload_audio.py my.bin -p COM15

协议（与 weather_clock/u5flash.h 一致，只是换了个基地址）:
    A                    → UOK <sector> <capacity>
    H <total> <crc32>    → HOK          （擦除）
    D <seq> <len>\\n + 原始数据 → A <seq>
    E                    → EOK

板子侧 `A` 会先检查 GIF blob 有没有占到 8MB 以后，占了就拒绝上传（避免毁掉 GIF）。
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
BAUD = 115200          # USB CDC 下波特率无效
WIN = 4
AUDIO_BASE = 0x800000
AUDIO_CAP = 0x1000000 - AUDIO_BASE


class Wire:
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
    ap.add_argument("bin", nargs="?", default="tools/last_aud.bin")
    ap.add_argument("-p", "--port")
    ap.add_argument("--win", type=int, default=WIN)
    a = ap.parse_args()

    data = open(a.bin, "rb").read()
    if len(data) < 24 or data[:4] != b"AUDB":
        sys.exit("不是 AUDB blob: %s（先用 tools/wav2aud.py 生成）" % a.bin)
    sr = struct.unpack_from("<I", data, 8)[0]
    nbytes = struct.unpack_from("<I", data, 12)[0]
    crc = zlib.crc32(data) & 0xFFFFFFFF
    total = len(data)
    print("文件 %s  %d 字节  %dHz  %.1f 秒  crc32 %08X"
          % (a.bin, total, sr, nbytes / 2.0 / max(1, sr), crc))
    if total > AUDIO_CAP:
        sys.exit("超过音频区容量 %.1f MB，先截短或降采样" % (AUDIO_CAP / 1048576.0))

    win = max(1, min(a.win, 7))
    port = pick_port(a.port)
    print("端口 %s" % port)
    # 打开串口前就把 DTR/RTS 压低：pyserial 构造时会拉高，那一瞬间就够触发板子复位
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 3
    ser.write_timeout = 20
    ser.dtr = False
    ser.rts = False
    ser.open()
    try:
        try:
            ser.set_buffer_size(rx_size=65536, tx_size=65536)
        except Exception:
            pass
        time.sleep(0.3)
        ser.reset_input_buffer()
        w = Wire(ser)

        # ★ 板子要是刚复位（或正在联网），u5init() 还没跑完，直接发 A 会回 ERR noflash。
        #   先发个 b 探活；探不到就当它还在启动，等一会儿。
        alive = False
        for _ in range(3):
            ser.write(b"b\n")
            ser.flush()
            try:
                w.wait("[btn] A=", timeout=1.5, quiet=True)
                alive = True
                break
            except TimeoutError:
                pass
        if not alive:
            print("板子没响应动作命令，等它启动完（最多 35 秒）...")
            time.sleep(28)
            ser.reset_input_buffer()
            w.buf = b""
        ser.write(b"A\n")
        r = w.wait("UOK", timeout=20)
        _, sector, cap = r.split()
        print("板子就绪  扇区 %s  容量 %.0f MB" % (sector, int(cap) / 1048576.0))

        kb = (total + 1023) // 1024
        print("擦除 %d KB ..." % kb)
        t0 = time.time()
        ser.write(("H %d %x\n" % (total, crc)).encode())
        w.wait("HOK", timeout=30 + kb * 0.06)
        t_erase = time.time() - t0
        print("擦除完成 %.1fs（%.0f KB/s）" % (t_erase, kb / max(t_erase, 0.001)))

        nblk = (total + BLK - 1) // BLK
        print("传输 %d 块 × %d B，流水窗口 %d 块" % (nblk, BLK, win))
        t0 = time.time()
        sent = 0
        ack = 0
        while ack < nblk:
            while sent < nblk and sent - ack < win:
                off = sent * BLK
                chunk = data[off:off + BLK]
                ser.write(("D %d %d\n" % (sent + 1, len(chunk))).encode() + chunk)
                sent += 1
            w.wait("A %d" % (ack + 1), timeout=20, quiet=True)
            ack += 1
            if ack % 32 == 0 or ack == nblk:
                done = min(total, ack * BLK)
                pct = done * 100 // total
                spd = done / 1024.0 / max(0.001, time.time() - t0)
                bar = "#" * (pct // 4) + "." * (25 - pct // 4)
                print("  [%s] %3d%%  %6.1f KB/s" % (bar, pct, spd), end="\r")
        print()

        ser.write(b"E\n")
        w.wait("EOK", timeout=30)
        dt = time.time() - t0
        print("校验通过  EOK  传输 %.1fs (%.0f KB/s)  总耗时 %.1fs"
              % (dt, total / 1024.0 / dt, t_erase + dt))

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

        ser.write(b"s\n")
        print("\n已在板子上开始播放（串口发 s 重播、S 停止、a 看状态）。")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
