/*
 * mp3bench —— ESP32-S3 上 helix MP3 解码开销基准
 * =============================================
 * 为什么测这个：
 *   新板用 MINI-1-N8（8MB flash、**无 PSRAM**），软件架构定的是
 *   「SD 读 → helix 解码 → 128KB PCM 环形 → I2S DMA」。
 *   128KB 到底能装多少秒、解码能不能跟上 44.1kHz 实时，
 *   这两笔账只有实测才算数（V5 的 31250Hz 推流路径不能直接套）。
 *
 * 两个阶段：
 *   [A] 走库的 write() 封装：整段喂进去，测总吞吐（= 真实固件路径）
 *   [B] 直接调 MP3Decode 逐帧：测**每帧**耗时分布，取 max/p99
 *       —— 单帧最大值决定环形缓冲深度的下限（不是平均值）
 *
 * 全部数据 incbin 在 flash 里，不碰文件系统，排除 I/O 干扰。
 * 素材由 tools/_mkmp3.py 用 LAME 生成，8 秒定长。
 *
 * 用法：烧进去，开 115200 串口，复位即跑。约 60 秒出全部结果。
 */

#define HELIX_LOGGING_ACTIVE false  // 必须在 include 之前：彻底编译掉每帧日志

#include <Arduino.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "MP3DecoderHelix.h"
#include "mp3data.h"
#include "bench.h"

// ---------------------------------------------------------------- 素材表

static const BenchCase kCases[] = {
  {"44.1k/128k/st", mp3_a_start, mp3_a_end, 44100, 128, 2},
  {"44.1k/320k/st", mp3_b_start, mp3_b_end, 44100, 320, 2},
  {"48.0k/128k/st", mp3_c_start, mp3_c_end, 48000, 128, 2},
  {"44.1k/128k/mo", mp3_d_start, mp3_d_end, 44100, 128, 1},
};
static const int kCaseCount = (int)(sizeof(kCases) / sizeof(kCases[0]));

// ---------------------------------------------------------------- 全局统计

static volatile uint32_t g_frames;
static volatile uint64_t g_samps;      // 每声道样本数累计
static uint32_t *g_frameUs;            // 逐帧耗时记录
static uint32_t g_frameUsCap;
static uint32_t g_frameUsCnt;

static inline int64_t nowUs() { return esp_timer_get_time(); }

// ---------------------------------------------------------------- 帧头解析

// MPEG-1/2/2.5 Layer III 帧长（含 padding）。返回 -1 表示不是有效帧头。
static int mp3FrameLen(const uint8_t *h) {
  if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return -1;
  int ver   = (h[1] >> 3) & 0x3;   // 3=MPEG1 2=MPEG2 0=MPEG2.5
  int layer = (h[1] >> 1) & 0x3;   // 1=Layer III
  int brx   = (h[2] >> 4) & 0xF;
  int srx   = (h[2] >> 2) & 0x3;
  int pad   = (h[2] >> 1) & 0x1;
  if (layer != 1 || brx == 0 || brx == 15 || srx == 3) return -1;

  static const uint16_t br1[] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
  static const uint16_t br2[] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
  static const uint16_t sr1[] = {44100,48000,32000,0};
  static const uint16_t sr2[] = {22050,24000,16000,0};
  static const uint16_t sr25[] = {11025,12000,8000,0};

  int br = (ver == 3) ? br1[brx] : br2[brx];
  int sr = (ver == 3) ? sr1[srx] : (ver == 2 ? sr2[srx] : sr25[srx]);
  if (ver == 3) return 144 * br * 1000 / sr + pad;
  return 72 * br * 1000 / sr + pad;
}

// ---------------------------------------------------------------- 阶段 A 回调

// MP3FrameInfo 是 mp3dec.h 里的 C 结构体，**不在** libhelix 命名空间里
static void onPCM(MP3FrameInfo &info, short *pcm, size_t len, void *ref) {
  (void)pcm; (void)len; (void)ref;
  g_frames++;
  if (info.nChans > 0) g_samps += (uint64_t)info.outputSamps / (uint64_t)info.nChans;
}

// ---------------------------------------------------------------- 阶段 A：库封装吞吐

static void runA(const BenchCase &c) {
  size_t bytes = (size_t)(c.end - c.start);

  size_t heapBefore  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t psramBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  g_frames = 0; g_samps = 0;
  libhelix::MP3DecoderHelix dec(&onPCM);
  bool ok = dec.begin();

  size_t heapAfterBegin = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  int64_t t0 = nowUs();
  size_t consumed = dec.write(c.start, bytes);
  int64_t t1 = nowUs();

  size_t heapAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t psramAfter = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  double ms = (double)(t1 - t0) / 1000.0;
  double audioS = (c.sr > 0) ? (double)g_samps / (double)c.sr : 0.0;
  double rt = (ms > 0.0) ? (audioS * 1000.0 / ms) : 0.0;
  double pct = (audioS > 0.0) ? (ms / (audioS * 1000.0) * 100.0) : 0.0;

  Serial.printf("\n[A] %s   %u B\n", c.name, (unsigned)bytes);
  Serial.printf("    begin=%s  frames=%u  samples/ch=%llu  audio=%.3f s  consumed=%u B\n",
                ok ? "ok" : "FAIL", (unsigned)g_frames,
                (unsigned long long)g_samps, audioS, (unsigned)consumed);
  Serial.printf("    wall=%8.1f ms   realtime x%.2f   CPU=%5.1f%% of one core @240MHz\n",
                ms, rt, pct);
  Serial.printf("    heap: before=%u  afterBegin=%u  afterDecode=%u\n",
                (unsigned)heapBefore, (unsigned)heapAfterBegin, (unsigned)heapAfter);
  Serial.printf("    alloc total=%u B (decoder@begin=%u B)   psram used=%d B\n",
                (unsigned)(heapBefore - heapAfter),
                (unsigned)(heapBefore - heapAfterBegin),
                (int)(psramBefore - psramAfter));
  Serial.printf("    PCM out = %u B  (%.1f KB/s at realtime)\n",
                (unsigned)(g_samps * (uint64_t)c.ch * 2),
                audioS > 0 ? (double)(g_samps * (uint64_t)c.ch * 2) / audioS / 1024.0 : 0.0);
}

// ---------------------------------------------------------------- 阶段 B：逐帧

static int cmpU32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x < y) ? -1 : (x > y ? 1 : 0);
}

#define ROUNDS 6

static void runB(const BenchCase &c) {
  short *pcm = (short *)malloc(2304 * sizeof(short));
  if (!pcm) { Serial.printf("\n[B] %s  malloc FAIL\n", c.name); return; }

  g_frameUsCnt = 0;
  int64_t totalUs = 0;
  uint64_t totalSamps = 0;
  uint32_t minUs = 0xFFFFFFFFu;

  for (int round = 0; round < ROUNDS; round++) {
    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) { Serial.printf("\n[B] %s  MP3InitDecoder FAIL\n", c.name); break; }

    const uint8_t *p = c.start;
    int left = (int)(c.end - c.start);
    int skip = 4;   // 前 4 帧依赖不存在的 bit reservoir，跳过不计

    while (left > 8) {
      int off = MP3FindSyncWord((unsigned char *)p, left);
      if (off < 0) break;
      p += off; left -= off;

      int flen = mp3FrameLen(p);
      if (flen <= 0 || flen > left) break;

      uint8_t *q = (uint8_t *)p;      // helix 只读 inbuf，不改内容
      int bl = left;
      int64_t t0 = nowUs();
      int rc = MP3Decode(dec, &q, &bl, pcm, 0);
      int64_t dt = nowUs() - t0;

      int used = (int)(q - p);
      if (used <= 0) used = flen;

      if (rc == 0) {
        if (skip <= 0) {
          if (g_frameUsCnt < g_frameUsCap) g_frameUs[g_frameUsCnt++] = (uint32_t)dt;
          totalUs += dt;
          totalSamps += 1152;         // MPEG-1 Layer III 固定 1152 样本/帧
          if ((uint32_t)dt < minUs) minUs = (uint32_t)dt;
        } else {
          skip--;
        }
      }
      p += used; left -= used;
    }
    MP3FreeDecoder(dec);
  }
  free(pcm);

  if (g_frameUsCnt < 8) { Serial.printf("\n[B] %s  too few frames (%u)\n", c.name, (unsigned)g_frameUsCnt); return; }

  qsort(g_frameUs, g_frameUsCnt, sizeof(uint32_t), cmpU32);
  uint32_t p50 = g_frameUs[g_frameUsCnt * 50 / 100];
  uint32_t p99 = g_frameUs[g_frameUsCnt * 99 / 100];
  uint32_t mx  = g_frameUs[g_frameUsCnt - 1];

  double avgUs = (double)totalUs / (double)g_frameUsCnt;
  double frameMs = 1000.0 * 1152.0 / (double)c.sr;   // 该采样率下一帧的播放时长
  double audioS = (double)totalSamps / (double)c.sr;
  double rt = (double)totalUs / 1000.0 > 0.0 ? (audioS * 1000.0 * 1000.0 / (double)totalUs) : 0.0;

  Serial.printf("\n[B] %s  frames=%u  x%d rounds\n", c.name, (unsigned)g_frameUsCnt, ROUNDS);
  Serial.printf("    per-frame us : avg %6.0f  min %5u  p50 %5u  p99 %5u  max %5u\n",
                avgUs, (unsigned)minUs, (unsigned)p50, (unsigned)p99, (unsigned)mx);
  Serial.printf("    frame budget : %.2f ms  ->  avg %4.1f%%  p99 %4.1f%%  max %4.1f%%\n",
                frameMs, avgUs / 1000.0 / frameMs * 100.0,
                p99 / 1000.0 / frameMs * 100.0, mx / 1000.0 / frameMs * 100.0);
  Serial.printf("    realtime x%.2f (pure decode, no output)\n", rt);
}

// ---------------------------------------------------------------- 附加：memcpy 与缓冲账

static void runBufMath() {
  // 逐帧输出 4608B；真实管线要把它 memcpy 进环形缓冲
  static short src[2304], dst[2304];
  const int N = 2000;
  int64_t t0 = nowUs();
  for (int i = 0; i < N; i++) memcpy(dst, src, sizeof(src));
  int64_t dt = nowUs() - t0;
  double perCopyUs = (double)dt / N;

  Serial.printf("\n[C] memcpy 4608B x%d: %.2f us/copy (%.0f MB/s)\n",
                N, perCopyUs, 4608.0 / perCopyUs);

  // 缓冲深度账：44.1k/16bit/stereo
  const double rate441 = 44100.0 * 2 * 2;   // B/s
  const double rate480 = 48000.0 * 2 * 2;
  Serial.printf("    PCM rate: 44.1k/st = %.1f KB/s  48k/st = %.1f KB/s\n",
                rate441 / 1024.0, rate480 / 1024.0);
  const int sizes[] = {64, 128, 192, 256, 320, 384};
  for (int i = 0; i < 6; i++) {
    Serial.printf("      ring %3d KB -> %5.2f s @44.1k  %5.2f s @48k\n",
                  sizes[i],
                  sizes[i] * 1024.0 / rate441,
                  sizes[i] * 1024.0 / rate480);
  }
}

// ---------------------------------------------------------------- setup

void setup() {
  Serial.begin(115200);
  delay(600);
  setCpuFrequencyMhz(240);

  g_frameUsCap = 4096;
  g_frameUs = (uint32_t *)malloc(g_frameUsCap * sizeof(uint32_t));

  esp_chip_info_t ci;
  esp_chip_info(&ci);

  Serial.println();
  Serial.println("======== mp3bench ========");
  Serial.printf("chip=%d cores=%d  cpu=%u MHz  flash=%u MB\n",
                (int)ci.model, ci.cores, (unsigned)getCpuFrequencyMhz(),
                (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  Serial.printf("psram total=%u KB  free=%u KB   (bench intentionally avoids PSRAM)\n",
                (unsigned)(ESP.getPsramSize() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  Serial.printf("heap internal free=%u KB  maxblk=%u KB\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

  for (int i = 0; i < kCaseCount; i++) {
    Serial.printf("\n---- case %d/%d  %s ----", i + 1, kCaseCount, kCases[i].name);
    runA(kCases[i]);
    runB(kCases[i]);
    Serial.printf("    heap now=%u KB\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  }

  runBufMath();
  Serial.println("\n======== done ========");
}

void loop() {
  delay(5000);
}
