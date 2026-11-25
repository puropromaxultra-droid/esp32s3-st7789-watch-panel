#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""album.py —— 管理板子 U5 里的「专辑」：静态图和 GIF 混放，屏上长按 A/B 切换。

板子端布局（16MB GD25Q128）：
    0x000000 ~ 0x1FFFFF   内容区（前 2MB，每条 64KB 对齐，最多 12 条）
    0x1FF000              索引扇区（这 2MB 的最后一个 4KB，独立擦写，不碰内容）
    0x200000 ~ 0x5FFFFF   音乐环形缓冲（固件占，别碰）
    0x800000              整段音频 AUDB

用法：
    python tools/album.py list                  # 看板子里有什么
    python tools/album.py add cat.gif           # 追加一条（GIF）
    python tools/album.py add a.png b.jpg       # 图片也行（单帧）
    python tools/album.py add x.gif --name 猫咪  # 名字只支持 ASCII，中文会被丢掉
    python tools/album.py migrate               # 索引搬到 0x1FF000（不动内容，一次就够）
    python tools/album.py rm 2                  # 删掉第 2 条（只改索引，数据留着等覆盖）
    python tools/album.py mv 2 0                # 把第 2 条挪到最前面（其余顺延）
    python tools/album.py order 1 0 2           # 一次排好全部顺序
    python tools/album.py clear                 # 清空索引（数据不擦，之后还会被覆盖）

序号口径：一律 **0 开始**（和 list 的 # 列一致）。屏上 HUD 显示的 `2/5` 是 1 开始，
所以「屏上第 2 条」在命令里要写 1。

索引协议（固件 gif_store.h 的 ALB_ENT）：
    I <count> <hex>
    每条 24 字节按字段展开：off8 bytes8 frames4 w4 h4 kind2 pad2 name16
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("需要 pyserial: pip install pyserial")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from upload_gif import Wire, BLK            # noqa: E402  复用上传协议的实现

DATA_MIN = 0x010000          # 内容区最早地址（0~64KB 留空）
DATA_MAX = 0x1FF000          # 上限 = 索引起点（前 2MB 的最后一个扇区），再往上就撞索引了
ALB_MAX = 12
NAME_LEN = 8


def pick_port(want=None):
    if want:
        return want
    cands = [p.device for p in list_ports.comports() if p.vid == 0x303A]
    if not cands:
        sys.exit("没找到 ESP32（VID 303A）。用 -p 指定端口。")
    return cands[0]


def align64k(v):
    return (v + 0xFFFF) & ~0xFFFF & 0xFFFFFFFF


# ---------------- 与板子对话 ----------------

class Board:
    def __init__(self, port):
        # ★ 必须"先设 DTR/RTS 再 open"：直接 Serial(port, ...) 打开时 DTR 会拉高，
        #   ESP32-S3 的 USB-CDC 把它当复位信号 —— 板子重启，第一发命令全丢。
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.timeout = 3
        ser.write_timeout = 20
        ser.dtr = False
        ser.rts = False
        ser.open()
        self.ser = ser
        try:
            self.ser.set_buffer_size(rx_size=65536, tx_size=65536)
        except Exception:
            pass
        self.w = Wire(self.ser)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    # --- 读列表 ---
    def list(self):
        self.ser.reset_input_buffer()
        self.ser.write(b"V\n")
        items, end, bufcap = [], 0, 0
        while True:
            line = self.w.line(5.0)
            if not line:
                continue
            if line.startswith("ALB "):
                p = line.split()
                # ALB i kind frames w h bytes off name
                items.append(dict(i=int(p[1]), kind=int(p[2]), frames=int(p[3]),
                                  w=int(p[4]), h=int(p[5]), bytes=int(p[6]),
                                  off=int(p[7]), name=p[8] if len(p) > 8 else ""))
            elif line.startswith("ALBEND"):
                p = line.split()
                end = int(p[2])
                bufcap = int(p[3]) if len(p) > 3 else 0   # 固件当前 GIF 帧缓冲大小
                break
            elif line.startswith("ERR"):
                sys.exit("板子报错: %r" % line)
        return items, end, bufcap

    # --- 传一条 ---
    def upload(self, data, addr):
        crc = zlib.crc32(data) & 0xFFFFFFFF
        total = len(data)
        self.ser.write(b"U\n")
        self.w.wait("UOK", timeout=15)
        kb = (total + 1023) // 1024
        self.ser.write(("H %d %x %x\n" % (total, crc, addr)).encode())
        self.w.wait("HOK", timeout=60 + kb * 0.08)

        nblk = (total + BLK - 1) // BLK
        sent = ack = 0
        win = 4
        while ack < nblk:
            while sent < nblk and sent - ack < win:
                off = sent * BLK
                chunk = data[off:off + BLK]
                self.ser.write(("D %d %d\n" % (sent + 1, len(chunk))).encode() + chunk)
                sent += 1
            self.w.wait("A %d" % (ack + 1), timeout=20, quiet=True)
            ack += 1
            if ack % 64 == 0 or ack == nblk:
                print("    %3d%%" % (min(total, ack * BLK) * 100 // total), end="\r")
        print()
        self.ser.write(b"E\n")
        self.w.wait("EOK", timeout=30)

    # --- 写索引 ---
    def write_index(self, items, end):
        if not items:
            self.ser.reset_input_buffer()
            self.ser.write(b"I 0\n")
        else:
            parts = []
            for e in items:
                nm = e["name"].encode("ascii", "ignore")[:NAME_LEN].ljust(NAME_LEN, b"\0")
                parts.append("%08X%08X%04X%04X%04X%02X%02X%s" % (
                    e["off"], e["bytes"], e["frames"], e["w"], e["h"], e["kind"], 0,
                    "".join("%02X" % b for b in nm)))
            self.ser.reset_input_buffer()
            self.ser.write(("I %d %s\n" % (len(items), "".join(parts))).encode())
        self.w.wait("IOK", timeout=15)
        # 板子后面可能还跟一行 [alb] 日志：读到就丢掉，没有也不算错
        try:
            self.w.line(1.0)
        except TimeoutError:
            pass


# ---------------- 转码 ----------------

def make_blob(path):
    """调 gif2bin.py 转成 GIFB blob，返回 (blob bytes, 是否动图)。"""
    tmp = tempfile.mktemp(suffix=".bin")
    r = subprocess.run([sys.executable, os.path.join(HERE, "gif2bin.py"), path,
                        "-o", tmp, "--size", "240", "--max-kb", "900"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("转码失败: %s\n%s" % (path, r.stdout + r.stderr))
    blob = open(tmp, "rb").read()
    os.remove(tmp)
    frames = struct.unpack_from("<H", blob, 10)[0]
    return blob, frames


def max_frame(blob):
    """blob 里最大的单帧字节数 —— 固件就是按这个值预留帧缓冲的。"""
    frames = struct.unpack_from("<H", blob, 10)[0]
    oaddr = (24 + frames * 2 + 3) & ~3
    offs = struct.unpack_from("<%dI" % (frames + 1), blob, oaddr)
    return max(offs[i + 1] - offs[i] for i in range(frames))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list", help="列出板子里的内容")
    p_add = sub.add_parser("add", help="追加一条或多条")
    p_add.add_argument("files", nargs="+")
    p_add.add_argument("--name", default=None, help="名字（ASCII，最多 8 字符）")
    p_clr = sub.add_parser("clear", help="清空索引（数据不擦）")
    p_mig = sub.add_parser("migrate", help="索引原样搬到 0x1FF000（不动内容）")
    p_rm  = sub.add_parser("rm", help="删掉指定序号（只改索引，数据留着等覆盖）")
    p_rm.add_argument("idx", nargs="+", type=int)
    p_mv  = sub.add_parser("mv", help="把第 A 条挪到第 B 位（其余顺延），如 mv 2 0")
    p_mv.add_argument("frm", type=int)
    p_mv.add_argument("to", type=int)
    p_ord = sub.add_parser("order", help="一次排全部，如 order 2 0 1")
    p_ord.add_argument("seq", nargs="+", type=int)

    for p in (p_list, p_add, p_clr, p_mig, p_rm, p_mv, p_ord):
        p.add_argument("-p", "--port")

    a = ap.parse_args()
    b = Board(pick_port(a.port))
    try:
        items, end, bufcap = b.list()

        if a.cmd == "list":
            if not items:
                print("板子里没有内容")
            else:
                print("%-3s %-5s %-5s %-9s %-10s %-10s %s"
                      % ("#", "类型", "帧数", "尺寸", "大小", "地址", "名称"))
                for e in items:
                    print("%-3d %-5s %-5d %-9s %-10s 0x%06X   %s"
                          % (e["i"], "动图" if e["kind"] else "图片", e["frames"],
                             "%dx%d" % (e["w"], e["h"]),
                             "%.0f KB" % (e["bytes"] / 1024.0), e["off"],
                             e["name"] or "-"))
                print("\n共 %d 条，内容区已用到 %.0f KB / %.0f KB"
                      % (len(items), end / 1024.0, DATA_MAX / 1024.0))
            return

        if a.cmd == "clear":
            b.write_index([], 0)
            print("已清空索引（%d 条）。屏上会显示 NO GIF DATA。" % len(items))
            return

        if a.cmd == "migrate":
            b.write_index(items, end)
            print("索引已重写到 0x1FF000（%d 条），内容一个字没动。" % len(items))
            return

        if a.cmd == "rm":
            drop = set(a.idx)
            bad = sorted(i for i in drop if i < 0 or i >= len(items))
            if bad:
                sys.exit("序号越界：%s（现有 0~%d）" % (bad, len(items) - 1))
            keep = [e for e in items if e["i"] not in drop]
            b.write_index(keep, 0)      # 板子收到 I 会自己重算 dataEnd，删掉的洞下次 add 会被覆盖
            print("已删 %d 条，剩 %d 条：" % (len(drop), len(keep)))
            for e in keep:
                print("  %d. %-10s %s %d 帧  %.0f KB"
                      % (e["i"], e["name"] or "-", "动图" if e["kind"] else "图片",
                         e["frames"], e["bytes"] / 1024.0))
            return

        if a.cmd in ("mv", "order"):
            n = len(items)
            if n < 2:
                sys.exit("只有 %d 条，没什么可排的" % n)
            if a.cmd == "mv":
                if not (0 <= a.frm < n and 0 <= a.to < n):
                    sys.exit("序号越界：现有 0~%d" % (n - 1))
                seq = list(range(n))
                seq.insert(a.to, seq.pop(a.frm))
            else:
                seq = list(a.seq)
                if sorted(seq) != list(range(n)):
                    sys.exit("order 要把 0~%d 每个序号写且只写一次，你写的是 %s"
                             % (n - 1, " ".join(str(x) for x in seq)))
            keep = [items[i] for i in seq]
            for k, e in enumerate(keep):
                e["i"] = k                      # 重排后序号重编
            b.write_index(keep, end)            # 只改索引顺序，内容地址一个都不动
            print("新顺序（屏上长按 B 从上往下走）：")
            for e in keep:
                print("  %d. %-10s %s %d 帧  %.0f KB"
                      % (e["i"], e["name"] or "-", "动图" if e["kind"] else "图片",
                         e["frames"], e["bytes"] / 1024.0))
            return

        # ---- add ----
        if len(items) + len(a.files) > ALB_MAX:
            sys.exit("最多 %d 条，现在已有 %d 条" % (ALB_MAX, len(items)))

        cur = max(end, DATA_MIN)
        need_restart = False
        for path in a.files:
            blob, frames = make_blob(path)
            mf = max_frame(blob)
            addr = align64k(cur)
            if addr + len(blob) > DATA_MAX:
                sys.exit("内容区满了：需要 0x%X，上限 0x%X（先 rm 几条腾地方）"
                         % (addr + len(blob), DATA_MAX))
            nm = a.name or os.path.splitext(os.path.basename(path))[0]
            print("→ %s  %d 帧  %.0f KB  最大帧 %.1f KB  →  0x%06X"
                  % (os.path.basename(path), frames, len(blob) / 1024.0,
                     mf / 1024.0, addr))
            if mf > bufcap:
                print("  ⚠ 单帧 %.1f KB 超过当前帧缓冲 %.1f KB —— 加完要复位一次（拔插 USB）才能播"
                      % (mf / 1024.0, bufcap / 1024.0))
                need_restart = True
            if mf > 65536:
                print("  ⚠ 单帧 > 64KB，内部 RAM 大概率拿不到这么大连块：少帧/降色再转一次")
            b.upload(blob, addr)
            items.append(dict(i=len(items), kind=1 if frames > 1 else 0, frames=frames,
                              w=240, h=240, bytes=len(blob), off=addr, name=nm))
            cur = addr + len(blob)

        b.write_index(items, align64k(cur))
        print("\n索引已更新，共 %d 条：屏上回到 GIF 页，长按 A/B 切换。" % len(items))
        for e in items:
            print("  %d. %-10s %s %d 帧  %.0f KB"
                  % (e["i"], e["name"] or "-", "动图" if e["kind"] else "图片",
                     e["frames"], e["bytes"] / 1024.0))
        if need_restart:
            print("\n★ 有新条目单帧比当前缓冲大：复位板子（拔插 USB 或按复位键）后即可播放。")
    finally:
        b.close()


if __name__ == "__main__":
    main()
