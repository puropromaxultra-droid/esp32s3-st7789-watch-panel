#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
wav2aud.py —— 把 wav 打包成板子能播的声音 blob（AUDB）

★ 为什么要重采样到 50kHz：
  板子的 ESP32-S3 I2S 只有 PLL_160M 这一个时钟源（没有 APLL），
  160e6 ÷ (fs × 32) 必须整除且为偶数，BCLK 才不带周期抖动。
  MAX98357A 没有 MCLK，只能从 BCLK 内部 PLL 恢复时钟，对抖动极敏感
  → 44.1kHz 会出沙沙底噪/尖啸。50000Hz 是唯一舒服的一档，
  所以音频统一重采样成 50kHz、单声道、16bit。

blob 格式（小端）：
  0   u32 magic "AUDB"
  4   u16 version = 1
  6   u16 channels = 1
  8   u32 sampleRate = 50000
  12  u32 dataBytes
  16  u32 crc32
  20  u32 reserved
  24  ... int16 小端 PCM

用法:
  python wav2aud.py a.wav                      → tools/last_aud.bin
  python wav2aud.py a.wav -o out.bin
  python wav2aud.py a.wav --gain 1.2
  python wav2aud.py a.wav --start 3.5 --dur 20 # 只取 3.5s 起的 20 秒
  python wav2aud.py a.wav && python upload_audio.py

容量：U5 音频区 8MB，50kHz 单声道 16bit = 100KB/s → 最多约 80 秒。
"""

import argparse
import array
import os
import struct
import sys
import wave
import zlib

SR_OUT = 50000
MAGIC = 0x42445541          # 'A'|'U'<<8|'D'<<16|'B'<<24


def read_wav(path):
    try:
        w = wave.open(path, "rb")
    except Exception as e:
        sys.exit("打不开 %s：%s" % (path, e))
    nch = w.getnchannels()
    sw = w.getsampwidth()
    sr = w.getframerate()
    n = w.getnframes()
    raw = w.readframes(n)
    w.close()
    if sw not in (1, 2, 4):
        sys.exit("只支持 8/16/32bit PCM，这个是 %dbit（24bit 请先用 ffmpeg 转 16bit）" % (sw * 8))
    print("源文件  %dHz  %d声道  %dbit  %.2f 秒"
          % (sr, nch, sw * 8, n / float(sr)))
    return nch, sw, sr, raw


def decode_mono(nch, sw, raw):
    """解码成 array('f')，范围 -1.0 ~ 1.0，多声道取平均。"""
    if sw == 2:
        a = array.array("h")
        a.frombytes(raw)
        if sys.byteorder != "little":
            a.byteswap()
        scale = 1.0 / 32768.0
    elif sw == 1:
        a = array.array("b", bytes((v - 128) & 0xFF for v in raw))
        scale = 1.0 / 128.0
    else:  # 32bit
        a = array.array("i")
        a.frombytes(raw)
        if sys.byteorder != "little":
            a.byteswap()
        scale = 1.0 / 2147483648.0

    if nch == 1:
        return array.array("f", [v * scale for v in a])
    out = array.array("f")
    ap = out.append
    inv = 1.0 / nch
    for i in range(0, len(a) - nch + 1, nch):
        s = 0
        for c in range(nch):
            s += a[i + c]
        ap(s * scale * inv)
    return out


def resample_to_i16(src, sr_in, sr_out, gain):
    """线性插值重采样 + 量化成 int16，一次循环做完（400 万样本纯 Python 约几秒）。"""
    n_in = len(src)
    if n_in == 0:
        return array.array("h")
    n_out = int(n_in * sr_out / float(sr_in))
    if n_out < 1:
        n_out = 1
    ratio = sr_in / float(sr_out)
    out = array.array("h", bytes(2 * n_out))
    last = src[n_in - 1]
    for i in range(n_out):
        pos = i * ratio
        i0 = int(pos)
        if i0 >= n_in - 1:
            v = last
        else:
            fr = pos - i0
            s0 = src[i0]
            v = s0 + (src[i0 + 1] - s0) * fr
        x = v * gain * 32767.0
        if x > 32767.0:
            x = 32767.0
        elif x < -32768.0:
            x = -32768.0
        out[i] = int(x)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("-o", "--out", default=os.path.join("tools", "last_aud.bin"))
    ap.add_argument("--gain", type=float, default=1.0, help="音量倍数（削波会截断）")
    ap.add_argument("--start", type=float, default=0.0, help="从第几秒开始")
    ap.add_argument("--dur", type=float, default=0.0, help="取多少秒（0=到结尾）")
    a = ap.parse_args()

    nch, sw, sr, raw = read_wav(a.wav)
    mono = decode_mono(nch, sw, raw)

    if a.start > 0:
        i0 = int(a.start * sr)
        mono = mono[i0:] if i0 < len(mono) else array.array("f")
    if a.dur > 0:
        i1 = int(a.dur * sr)
        if i1 < len(mono):
            mono = mono[:i1]

    sec = len(mono) / float(sr)
    pcm = resample_to_i16(mono, sr, SR_OUT, a.gain)
    data = pcm.tobytes()
    if sys.byteorder != "little":
        swapped = array.array("h")
        swapped.frombytes(data)
        swapped.byteswap()
        data = swapped.tobytes()

    crc = zlib.crc32(data) & 0xFFFFFFFF
    hdr = struct.pack("<IHHIIII", MAGIC, 1, 1, SR_OUT, len(data), crc, 0)
    blob = hdr + data

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(blob)

    print("输出    %s" % a.out)
    print("  %d 秒  %d 字节 (%.1f KB)  crc32 %08X"
          % (len(data) // 2 // SR_OUT, len(blob), len(blob) / 1024.0, crc))
    if len(blob) > 8 * 1024 * 1024:
        print("  ⚠ 超过音频区 8MB，上传会被拒绝。用 --start/--dur 截短，或降采样率。")
    print("\n下一步： python tools/upload_audio.py %s" % a.out)


if __name__ == "__main__":
    main()
