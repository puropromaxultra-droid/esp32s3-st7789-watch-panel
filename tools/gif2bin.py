#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gif2bin.py —— 把 GIF 转成 U5（外挂 16MB flash）用的二进制 blob

和 gif2h.py 的区别：
    gif2h.py  输出 C 头文件，编进固件，受内部 flash 分区限制（~1.8MB）
    gif2bin.py 输出 .bin，走串口传到 U5，预算约 15MB（能放 200+ 帧）

用法:
    python gif2bin.py input.gif                    # 输出 input.bin
    python gif2bin.py input.gif -o my.bin --max-kb 14000
    python gif2bin.py input.gif --fit contain      # contain(留黑边) | cover(裁满)

blob 格式（全小端）:
    0   u32  "GIFB"
    4   u16  version = 1
    6   u16  width
    8   u16  height
    10  u16  frameCount
    12  u32  dataBytes
    16  u32  crc32(data)
    20  u32  dataOff
    24  u16  delayMs[n]
        u32  frameOff[n+1]      相对 dataOff
        ---- dataOff ----
        RLE: count(u16) + color565(u16)
"""

import argparse
import os
import struct
import sys
import zlib

try:
    from PIL import Image, ImageSequence, ImageOps
except ImportError:
    sys.exit("需要 Pillow: pip install pillow")

MAGIC = b"GIFB"
VERSION = 1


def frame_to_rgb(im, size, fit):
    im = im.convert("RGBA")
    bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
    bg.alpha_composite(im)
    im = bg.convert("RGB")
    if fit == "cover":
        out = ImageOps.fit(im, (size, size), Image.LANCZOS)
    else:
        out = ImageOps.contain(im, (size, size), Image.LANCZOS)
        canvas = Image.new("RGB", (size, size), (0, 0, 0))
        canvas.paste(out, ((size - out.width) // 2, (size - out.height) // 2))
        out = canvas
    return out


# 量化级别：每通道保留多少位（掩码）。位数越少，相邻像素越容易撞成同一个值，
# RLE 段数（也是单帧字节数，每段 4 字节）就掉得越快。
# ★ 固件的帧缓冲是固定的（约 37KB，内部 RAM 拿不到更大的连续块），
#   所以"单帧太大"只能靠降色解决 —— 见 --max-frame-kb。
QMASK = {
    "565": (0xF8, 0xFC, 0xF8),   # 原始（5/6/5）
    "555": (0xF8, 0xF8, 0xF8),   # 绿降 1 位
    "444": (0xF0, 0xF0, 0xF0),   # 每通道 16 级
    "333": (0xE0, 0xE0, 0xE0),   # 每通道 8 级
    "332": (0xE0, 0xE0, 0xC0),   # 8/8/4 级
    "222": (0xC0, 0xC0, 0xC0),   # 每通道 4 级（很糊，兜底）
}
QORDER = ["565", "555", "444", "333", "332", "222"]


def rgb_to_rle(im, q="565"):
    mr, mg, mb = QMASK[q]
    runs = []
    cur = None
    cnt = 0
    for (r, g, b) in im.getdata():
        v = ((r & mr) << 8) | ((g & mg) << 3) | ((b & mb) >> 3)
        if v == cur and cnt < 65535:
            cnt += 1
        else:
            if cur is not None:
                runs.append((cnt, cur))
            cur = v
            cnt = 1
    if cur is not None:
        runs.append((cnt, cur))
    return runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("-o", "--out")
    ap.add_argument("--size", type=int, default=240)
    ap.add_argument("--fit", choices=["cover", "contain"], default="cover")
    ap.add_argument("--max-kb", type=int, default=14000)
    ap.add_argument("--max-frames", type=int, default=0)
    ap.add_argument("--max-frame-kb", type=int, default=34,
                    help="单帧上限 KB（固件帧缓冲约 37KB）；超了自动降色，0=不限")
    ap.add_argument("--quant", choices=["auto"] + QORDER, default="auto",
                    help="强制指定量化级别，默认按单帧上限自动降")
    a = ap.parse_args()

    im = Image.open(a.src)
    frames, delays = [], []
    for fr in ImageSequence.Iterator(im):
        d = fr.info.get("duration", 0) or im.info.get("duration", 0) or 100
        frames.append(fr.copy())
        delays.append(int(d))
    total_in = len(frames)
    if total_in == 0:
        sys.exit("GIF 里没有帧")

    if a.max_frames and total_in > a.max_frames:
        step = total_in / float(a.max_frames)
        keep = [int(i * step) for i in range(a.max_frames)]
        frames = [frames[i] for i in keep]
        delays = [delays[i] for i in keep]

    # 每帧 RLE 段数 → 字节数（每段 4 字节）
    print("解析 %d 帧 ..." % len(frames))
    imgs = [frame_to_rgb(f, a.size, a.fit) for f in frames]

    # ★ 单帧上限：降色直到每帧都塞得进固件帧缓冲（约 37KB，内部 RAM 拿不到更大的连续块）
    lim = a.max_frame_kb * 1024
    qs = QORDER if a.quant == "auto" else [a.quant]
    rles, used, mx = None, None, 0
    for i, q in enumerate(qs):
        rr = [rgb_to_rle(im, q) for im in imgs]
        mx = max(len(r) * 4 for r in rr)
        if a.quant != "auto" or lim <= 0 or mx <= lim or i == len(qs) - 1:
            rles, used = rr, q
            break
        print("  单帧 %.1f KB > 上限 %d KB → 降到 %s 重试"
              % (mx / 1024.0, a.max_frame_kb, qs[i + 1]))
    if lim > 0 and mx > lim:
        sys.exit("降到 %s 单帧还有 %.1f KB，仍超上限 %d KB：源图太杂，裁短点再转"
                 % (used, mx / 1024.0, a.max_frame_kb))
    sizes = [len(r) * 4 for r in rles]
    if used != "565":
        print("! 降色到 %s（单帧最大 %.1f KB，上限 %d KB）"
              % (used, mx / 1024.0, a.max_frame_kb))

    budget = a.max_kb * 1024.0
    if sum(sizes) > budget and len(frames) > 1:
        n0 = len(frames)
        lo, hi = 1, n0
        best = [0]
        while lo <= hi:
            k = (lo + hi) // 2
            keep = sorted(set(int(i * n0 / k) for i in range(k)))
            if sum(sizes[i] for i in keep) <= budget:
                best = keep
                lo = k + 1
            else:
                hi = k - 1
        factor = n0 / float(len(best))
        rles = [rles[i] for i in best]
        delays = [int(round(delays[i] * factor)) for i in best]
        print("! 超预算 %.0fKB，均匀抽帧 %d → %d（延时 ×%.2f）"
              % (budget / 1024, n0, len(best), factor))
        sizes = [len(r) * 4 for r in rles]
    elif sum(sizes) > budget:
        sys.exit("单帧就超预算 %dKB，把 --max-kb 调大或缩短源图" % a.max_kb)

    n = len(rles)
    data = b"".join(struct.pack("<%dH" % (len(r) * 2),
                                *[v for run in r for v in run]) for r in rles)
    # 逐帧偏移
    offs = [0]
    for s in sizes:
        offs.append(offs[-1] + s)

    delay_blob = struct.pack("<%dH" % n, *[min(65535, max(0, d)) for d in delays])
    off_addr = 24 + len(delay_blob)
    off_addr = (off_addr + 3) & ~3
    pad1 = b"\x00" * (off_addr - (24 + len(delay_blob)))
    off_blob = struct.pack("<%dI" % (n + 1), *offs)
    data_off = off_addr + len(off_blob)

    head = struct.pack("<4sHHHHIII",
                       MAGIC, VERSION, a.size, a.size, n,
                       len(data), zlib.crc32(data) & 0xFFFFFFFF, data_off)
    blob = head + delay_blob + pad1 + off_blob + data

    out = a.out or (os.path.splitext(a.src)[0] + ".bin")
    with open(out, "wb") as f:
        f.write(blob)

    print("帧数 %d   尺寸 %dx%d   RLE %.1f KB   文件 %.1f KB"
          % (n, a.size, a.size, len(data) / 1024.0, len(blob) / 1024.0))
    print("crc32 %08X   dataOff %d" % (zlib.crc32(data) & 0xFFFFFFFF, data_off))
    fps = 1000.0 / (sum(delays) / float(n))
    print("平均延时 %.0fms  →  原始 %.1f fps（屏上限约 9~10fps）"
          % (sum(delays) / float(n), fps))
    print("输出 " + out)
    return out


if __name__ == "__main__":
    main()
