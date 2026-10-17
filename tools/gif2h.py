#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gif2h.py —— 把 GIF 转成 ESP32 直接能播的 C 头文件

用法:
    python gif2h.py input.gif                 # 输出 input_gif.h，240x240
    python gif2h.py input.gif -o mygif.h      # 指定输出
    python gif2h.py input.gif --size 240 --max-kb 600
    python gif2h.py input.gif --fit contain   # contain(保持比例) | cover(裁切填满)

原理:
    1. 每帧缩放到目标尺寸、居中贴到黑底
    2. RGB565 (16bit)
    3. RLE 压缩: (count, color) 交替，同色连续段合并
    4. 输出 C 数组 + 帧偏移表 + 帧延时表

播放端只要按帧解压推 SPI 即可，零解码库。
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageSequence, ImageOps
except ImportError:
    sys.exit("需要 Pillow: pip install pillow")


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


def rgb_to_rle(im):
    """返回 [(count, rgb565), ...]，count <= 65535"""
    px = list(im.getdata())
    runs = []
    cur = None
    cnt = 0
    for (r, g, b) in px:
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
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


def build(gif_path, out_path, size, fit, max_kb, max_frames=0):
    im = Image.open(gif_path)
    frames = []
    delays = []

    for frame in ImageSequence.Iterator(im):
        d = frame.info.get("duration", 0) or im.info.get("duration", 0) or 100
        frames.append(frame.copy())
        delays.append(int(d))

    total_in = len(frames)
    if total_in == 0:
        sys.exit("GIF 里没有帧")

    # 均匀抽帧，控制在 max_frames 之内
    if max_frames and total_in > max_frames:
        step = total_in / float(max_frames)
        keep = [int(i * step) for i in range(max_frames)]
        frames = [frames[i] for i in keep]
        delays = [delays[i] for i in keep]

    # 先全量编码，超预算就均匀抽帧
    while True:
        encoded = [rgb_to_rle(frame_to_rgb(f, size, fit)) for f in frames]
        words = sum(len(e) * 2 for e in encoded)
        approx_kb = words * 2 / 1024.0
        if approx_kb <= max_kb or len(frames) <= 2:
            break
        # 抽掉一半
        keep = list(range(0, len(frames), 2))
        frames = [frames[i] for i in keep]
        delays = [delays[i] for i in keep]
        min_d = min(delays)
        delays = [max(min_d * 2, d) for d in delays]

    offsets = [0]
    data = []
    for e in encoded:
        for run in e:
            data.append(run[0])
            data.append(run[1])
        offsets.append(len(data))

    n = len(frames)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("// 由 tools/gif2h.py 自动生成，请勿手改\n")
        f.write("// 源文件: %s\n" % os.path.basename(gif_path))
        f.write("// %d 帧, %dx%d, 约 %.1f KB\n" % (n, size, size, approx_kb))
        f.write("#pragma once\n#include <Arduino.h>\n\n")
        f.write("#define GIF_W %d\n" % size)
        f.write("#define GIF_H %d\n" % size)
        f.write("#define GIF_FRAME_COUNT %d\n\n" % n)

        f.write("static const uint16_t gifDelayMs[%d] = {\n    " % n)
        f.write(", ".join(str(max(20, d)) for d in delays))
        f.write("\n};\n\n")

        f.write("// 每帧在 gifData 中的起始下标（word 单位）\n")
        f.write("static const uint32_t gifFrameOffset[%d] = {\n    " % (n + 1))
        for i in range(0, n + 1, 10):
            f.write(", ".join(str(v) for v in offsets[i:i + 10]))
            f.write(",\n    " if i + 10 <= n else "\n")
        f.write("};\n\n")

        f.write("// RLE 数据: (count, color565) 交替\n")
        f.write("static const uint16_t gifData[%d] = {\n    " % len(data))
        for i in range(0, len(data), 12):
            f.write(" ".join("0x%04X," % v for v in data[i:i + 12]))
            f.write("\n    ")
        f.write("\n};\n")

    print("OK  %s" % out_path)
    print("    帧数 %d/%d  尺寸 %dx%d  数据 %.1f KB  数组元素 %d"
          % (n, total_in, size, size, approx_kb, len(data)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", "--output", default=None)
    ap.add_argument("--size", type=int, default=240)
    ap.add_argument("--fit", choices=["contain", "cover"], default="contain")
    ap.add_argument("--max-kb", type=float, default=600.0)
    ap.add_argument("--max-frames", type=int, default=0)
    a = ap.parse_args()

    if not os.path.exists(a.input):
        sys.exit("找不到文件: %s" % a.input)
    out = a.output or os.path.splitext(a.input)[0] + "_gif.h"
    build(a.input, out, a.size, a.fit, a.max_kb, a.max_frames)


if __name__ == "__main__":
    main()
