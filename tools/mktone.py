# -*- coding: utf-8 -*-
# 生成一段测试音频：小星星前两句，44.1kHz 单声道 16bit（故意不用 50k，验证重采样）
import math
import struct
import wave

SR = 44100
NOTE = {'C4': 261.63, 'D4': 293.66, 'E4': 329.63, 'F4': 349.23,
        'G4': 392.00, 'A4': 440.00}

# 小星星：1 1 5 5 6 6 5 -  4 4 3 3 2 2 1 -
melody = [('C4', .5), ('C4', .5), ('G4', .5), ('G4', .5),
          ('A4', .5), ('A4', .5), ('G4', 1.0),
          ('F4', .5), ('F4', .5), ('E4', .5), ('E4', .5),
          ('D4', .5), ('D4', .5), ('C4', 1.0)]

frames = bytearray()
for name, dur in melody:
    f = NOTE[name]
    n = int(SR * dur)
    attack = int(SR * 0.01)
    release = int(SR * 0.12)
    for i in range(n):
        # 简单包络：淡入 + 释音
        if i < attack:
            env = i / attack
        elif i > n - release:
            env = (n - i) / release
        else:
            env = 1.0
        # 基频 + 少量二次谐波，听起来像简单的电子琴
        v = math.sin(2 * math.pi * f * i / SR) * 0.75
        v += math.sin(2 * math.pi * f * 2 * i / SR) * 0.15
        s = int(v * env * 26000)
        frames += struct.pack('<h', max(-32768, min(32767, s)))
    # 音符之间留 20ms 静音
    frames += b'\x00\x00' * int(SR * 0.02)

w = wave.open(r'C:\Users\ZhuanZ\Desktop\电子屏显示\tools\test_tone.wav', 'wb')
w.setnchannels(1)
w.setsampwidth(2)
w.setframerate(SR)
w.writeframes(bytes(frames))
w.close()
print('生成 tools/test_tone.wav  %.2f 秒' % (len(frames) / 2 / SR))
