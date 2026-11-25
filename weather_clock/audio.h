// ============================================================
// audio.h —— U5 音频区（AUDB blob）：上传上去，从 U5 流式播到 I2S
//
// U5（16MB）分区：
//   0x000000 ~ 0x7FFFFF   GIF blob（gif_store.h）
//   0x800000 ~ 0xFFFFFF   音频区（本文件，8MB ≒ 100 秒 @40kHz 单声道 16bit）
//
// blob 格式（小端，tools/wav2aud.py 生成）：
//   0   u32  magic "AUDB"
//   4   u16  version = 1
//   6   u16  channels = 1        只支持单声道（脚本负责混音）
//   8   u32  sampleRate           必须 = SPK_SR（31250），脚本负责重采样
//   12  u32  dataBytes            PCM 字节数
//   16  u32  crc32                仅上传时校验，播放不校验
//   20  u32  reserved
//   24  ...  int16 LE PCM
//
// ★ 采样率为什么锁死 50kHz：见 spk.h 顶部。I2S 只有 PLL_160M，
//   160e6 ÷ (fs × 32) 必须整除且为偶数才不会让 MAX98357A 出沙声。
//   换采样率要重配 I2S 时钟（会有切换爆音），第一版不做，统一由脚本重采样。
//
// 播放路径：spk.h 的任务每 4.8ms 要一块 240 帧，钩子 spkPcmSrc 指向 audioFill()，
//   audioFill 从 8KB 读缓冲里取样本、按 spkVol 调音量、左右声道复制成一份。
//   缓冲 8KB = 4096 样本 = 82ms，足够跨过 GIF 解码任务持有 U5 锁的那几毫秒。
// ============================================================
#pragma once
#include <Arduino.h>
#include "u5flash.h"
#include "spk.h"

#define AUDIO_BASE    0x800000UL        // 8MB
#define AUDB_MAGIC    0x42445541UL      // 'A'|'U'<<8|'D'<<16|'B'<<24
#define AU_HDR        24
#define AU_BLK        8192              // 读缓冲 8KB = 4096 样本 = 82ms

struct AudioInfo {
  bool     ok        = false;
  uint16_t ch        = 0;
  uint32_t sr        = 0;
  uint32_t dataBytes = 0;
  uint32_t crc       = 0;
};

static AudioInfo aud;
static uint32_t  auPos    = 0;          // 已从 flash 读走的字节（相对 PCM 区）
static uint8_t   auBuf[AU_BLK];
static int       auLen    = 0;          // 缓冲内有效字节
static int       auIdx    = 0;
static volatile bool auPlaying = false;
static uint32_t  auFrames = 0;          // 本次已播帧数

static inline uint32_t au_rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t au_rd16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// 读 header。失败时 aud.ok = false。
static bool audioLoad() {
  aud.ok = false;
  aud.dataBytes = 0;
  if (!u5ok) { Serial.println("[aud] U5 不可用"); return false; }

  uint8_t h[AU_HDR];
  u5readRaw(AUDIO_BASE, h, AU_HDR);

  uint32_t magic = au_rd32(h);
  if (magic != AUDB_MAGIC) {
    Serial.printf("[aud] 音频区为空（magic=%08lX）\n", (unsigned long)magic);
    return false;
  }
  uint16_t ver = au_rd16(h + 4);
  aud.ch        = au_rd16(h + 6);
  aud.sr        = au_rd32(h + 8);
  aud.dataBytes = au_rd32(h + 12);
  aud.crc       = au_rd32(h + 16);

  if (ver != 1 || aud.ch != 1 || aud.dataBytes == 0) {
    Serial.printf("[aud] 头异常 ver=%u ch=%u bytes=%lu\n",
                  ver, aud.ch, (unsigned long)aud.dataBytes);
    return false;
  }
  if (aud.sr != SPK_SR) {
    Serial.printf("[aud] 采样率 %lu 与 I2S %d 不符，请用 tools/wav2aud.py 重新打包\n",
                  (unsigned long)aud.sr, SPK_SR);
    return false;
  }
  if (AUDIO_BASE + AU_HDR + aud.dataBytes > u5cap) {
    Serial.printf("[aud] 长度越界 %lu > 容量\n", (unsigned long)aud.dataBytes);
    return false;
  }

  aud.ok = true;
  Serial.printf("[aud] 就绪 %.1f 秒  %lu KB  %luHz 单声道\n",
                aud.dataBytes / 2.0f / aud.sr,
                (unsigned long)((aud.dataBytes + 1023) / 1024),
                (unsigned long)aud.sr);
  return true;
}

// 从 flash 补一块到缓冲。false = 已经到底。
static bool auRefill() {
  if (!aud.ok) return false;
  if (auPos >= aud.dataBytes) return false;
  uint32_t n = aud.dataBytes - auPos;
  if (n > AU_BLK) n = AU_BLK;
  u5readRaw(AUDIO_BASE + AU_HDR + auPos, auBuf, (size_t)n);   // 内部带锁
  auPos += n;
  auLen = (int)n;
  auIdx = 0;
  return true;
}

// spk.h 的 PCM 源钩子：填 frames 帧立体声（左右相同）。false = 播完/出错，自动摘钩。
static bool audioFill(int16_t *dst, int frames) {
  for (int i = 0; i < frames; i++) {
    if (auIdx + 2 > auLen) {
      if (!auRefill()) {
        for (int j = i; j < frames; j++) { dst[j * 2] = 0; dst[j * 2 + 1] = 0; }
        auPlaying = false;
        return false;
      }
    }
    int16_t s = (int16_t)(auBuf[auIdx] | ((int)auBuf[auIdx + 1] << 8));
    auIdx += 2;
    if (spkMute) {
      s = 0;
    } else if (spkVol != 100) {
      s = (int16_t)(((int32_t)s * spkVol) / 100);
    }
    dst[i * 2]     = s;
    dst[i * 2 + 1] = s;
  }
  auFrames += (uint32_t)frames;
  return true;
}

static bool audioPlay() {
  if (!aud.ok && !audioLoad()) return false;
  auPos = 0; auLen = 0; auIdx = 0; auFrames = 0;
  auPlaying = true;
  spkPcmSrc = audioFill;
  Serial.printf("[aud] 播放 %.1f 秒\n", aud.dataBytes / 2.0f / aud.sr);
  return true;
}

static void audioStop() {
  spkPcmSrc = nullptr;
  auPlaying = false;
}

static void audioReport() {
  Serial.printf("[aud] ok=%d playing=%d vol=%u%% mute=%d  %lu KB  %.1f s  (已播 %.1f s)\n",
                (int)aud.ok, (int)auPlaying, (unsigned)spkVol, (int)spkMute,
                (unsigned long)((aud.dataBytes + 1023) / 1024),
                aud.sr ? aud.dataBytes / 2.0f / aud.sr : 0.0f,
                aud.sr ? auFrames / (float)aud.sr : 0.0f);
}
