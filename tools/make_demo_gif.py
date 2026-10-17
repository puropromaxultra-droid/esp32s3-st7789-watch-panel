#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""生成一个用于验证播放链路的测试 GIF（纯几何图形，无字体依赖）"""
import math
import os
from PIL import Image, ImageDraw

W = H = 240
N = 24
OUT = os.path.join(os.path.dirname(__file__), "demo.gif")

BG = (8, 12, 24)
COLORS = [(255, 71, 87), (255, 165, 2), (255, 214, 10),
          (46, 213, 115), (0, 210, 211), (30, 144, 255),
          (155, 89, 182), (255, 105, 180)]

frames = []
for i in range(N):
    t = i / float(N)
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)

    # 外圈 8 个球，整体绕中心旋转
    for k in range(8):
        a = 2 * math.pi * (k / 8.0 + t)
        cx = W / 2 + 88 * math.cos(a)
        cy = H / 2 + 88 * math.sin(a)
        r = 9 + 4 * math.sin(2 * math.pi * t * 2 + k * 0.8)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=COLORS[k])

    # 中心圆呼吸
    cr = 34 + 10 * math.sin(2 * math.pi * t)
    d.ellipse([W / 2 - cr, H / 2 - cr, W / 2 + cr, H / 2 + cr],
              outline=(0, 210, 211), width=4)
    d.ellipse([W / 2 - 12, H / 2 - 12, W / 2 + 12, H / 2 + 12],
              fill=(255, 255, 255))

    # 底部进度条
    d.rectangle([30, 214, 210, 220], fill=(30, 36, 52))
    d.rectangle([30, 214, 30 + int(180 * t), 220], fill=(0, 210, 211))

    frames.append(im)

frames[0].save(OUT, save_all=True, append_images=frames[1:],
               duration=60, loop=0, optimize=False)
print("OK", OUT, os.path.getsize(OUT), "bytes,", N, "frames")
