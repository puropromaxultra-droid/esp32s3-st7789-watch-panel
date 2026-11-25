#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gifh2bin.py —— 把 gif2h.py 生成的 gif_data.h 还原成 U5 用的 .bin

用途：以前编进固件的图，想搬进 U5 时不用重新找源 GIF。
      对任何 gif_data.h 都通用（解析 gifDelayMs / gifFrameOffset / gifData）。

用法:
    python gifh2bin.py weather_clock/gif_data.h -o tools/last_gif.bin
"""

import argparse
import os
import re
import struct
import sys
import zlib


def grab_array(src, name):
    m = re.search(r'\b' + name + r'\s*\[[^\]]*\]\s*=\s*\{(.*?)\};', src, re.S)
    if not m:
        sys.exit("找不到数组 " + name)
    body = m.group(1)
    nums = re.findall(r'0[xX][0-9a-fA-F]+|\d+', body)
    return nums


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("h")
    ap.add_argument("-o", "--out", default="tools/last_gif.bin")
    a = ap.parse_args()

    src = open(a.h, encoding='utf-8', errors='replace').read()

    def defin(k):
        m = re.search(r'#define\s+' + k + r'\s+(\d+)', src)
        if not m:
            sys.exit("找不到 #define " + k)
        return int(m.group(1))

    W = defin('GIF_W')
    H = defin('GIF_H')
    N = defin('GIF_FRAME_COUNT')

    delays = [int(x, 0) for x in grab_array(src, 'gifDelayMs')]
    offs = [int(x, 0) for x in grab_array(src, 'gifFrameOffset')]
    data = [int(x, 0) for x in grab_array(src, 'gifData')]

    if len(delays) != N:
        sys.exit("延时表长度 %d != %d" % (len(delays), N))
    if len(offs) != N + 1:
        sys.exit("偏移表长度 %d != %d" % (len(offs), N + 1))
    if len(data) != offs[-1]:
        sys.exit("数据长度 %d != offs[-1] %d" % (len(data), offs[-1]))

    # uint16 数组 → 小端字节流
    rle = struct.pack('<%dH' % len(data), *data)
    total_words = offs[-1]
    off_bytes = [o * 2 for o in offs]          # word 下标 → 字节偏移

    delay_blob = struct.pack('<%dH' % N, *[min(65535, max(0, d)) for d in delays])
    off_addr = 24 + len(delay_blob)
    off_addr = (off_addr + 3) & ~3
    pad1 = b'\x00' * (off_addr - (24 + len(delay_blob)))
    off_blob = struct.pack('<%dI' % (N + 1), *off_bytes)
    data_off = off_addr + len(off_blob)

    head = struct.pack('<4sHHHHIII', b'GIFB', 1, W, H, N,
                       len(rle), zlib.crc32(rle) & 0xFFFFFFFF, data_off)
    blob = head + delay_blob + pad1 + off_blob + rle

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'wb') as f:
        f.write(blob)

    print("%s → %s" % (a.h, a.out))
    print("  %d 帧  %dx%d  数据 %.1f KB  文件 %.1f KB  words=%d"
          % (N, W, H, len(rle) / 1024.0, len(blob) / 1024.0, total_words))
    print("  crc32 %08X  dataOff %d" % (zlib.crc32(rle) & 0xFFFFFFFF, data_off))


if __name__ == '__main__':
    main()
