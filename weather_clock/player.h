// ============================================================
// player.h —— 音乐播放器：U5 环形缓冲 + 状态机
//
// 为什么不用内存做缓冲：这块模组**没有 PSRAM**（ESP32-S3-MINI-1 N8，
//   实测 PSRAM=enabled / =opi 两种配置读回来都是 0），512KB SRAM 里
//   fb 占 115KB、GIF 帧缓冲 37KB，剩给音频的只有 20KB 出头。
//   1 秒 31250Hz 单声道要 62.5KB ⇒ 缓冲只能放 U5。
//
// 环形区：0x200000 ~ 0x600000（4MB = 67 秒 @31250）
//   GIF 只占 ~1MB，所以从 2MB 起是安全的；8MB 处留给 audio.h 的整段试听区。
//
// ★★ 卡顿的真正原因（2026-11-23 用 rd 速率量出来的，别再猜）：
//   正常播放每秒必须从环形区读走 62500 字节（31250 帧 × 2B）。实测只有
//   **51200 B/s ≈ 82%**，也就是说每 5 秒里有近 1 秒在输出静音（auto_clear 补 0），
//   听起来就是"慢半拍 + 断断续续 + 沙沙"。
//   凶手：擦除时 u5Mtx 被占，rnRefill 的 u5readRaw 最多要等整个擦除周期；
//   内存读缓冲只有 8KB = 131ms，而 64KB 块擦 150ms ⇒ **每次擦除必然把缓冲抽干**。
//   三个改动一起上：
//     ① 擦除粒度降到 32KB（0x52，typ 120ms，实测见 u5eraseMs）；
//     ② 擦除只在「刚 refill 完」那一瞬间做（rnFresh 信号量），
//        此时手里是满仓 131ms + DMA 30.7ms = 162ms，远大于 120ms；
//     ③ 擦除挪到独立低优先级任务，手机推流/HTTP 不再被它拖住。
//   ★ 后来者注意：`under` 计数看不出这个问题 —— refill 最终是成功的，
//     只是晚了。要看 rnLate（refill 被挡 >30ms 的次数）和 rd 的字节速率。
// ============================================================
#pragma once
#include <Arduino.h>
#include "u5flash.h"
#include "spk.h"

#define RN_BASE   0x200000UL        // 环形区起点（2MB）
#define RN_SIZE   0x400000UL        // 4MB
#define RN_ERASE  0x8000UL          // 32KB 块擦（0x52）：typ 120ms，短于 8KB 读缓冲的 131ms
#define RN_BLK    RN_ERASE          // 环形安全边界（留一块的余量）
//   ★ 走过弯路：一开始为了缩短阻塞（150→45ms）改用 4KB 扇区擦除，**实测是错的**——
//   扇区擦除的单次开销远不止 45ms（命令+忙等状态轮询），有效吞吐掉到 30KB/s 以下，
//   追不上 62.5KB/s 的播放速度，写指针卡死、缓冲满、手机端 TCP 超时。
//   32KB / 64KB 块擦才有 437KB/s 级别的有效吞吐。
#define RN_LOOK   0x40000UL         // 预擦水位：领先写指针 256KB（4.1 秒 @31250）。
//   别调太大：擦除总量 = 数据量 + 这个水位，放 12 秒的歌却预擦 1MB 就是纯浪费 flash 寿命。
#define RN_RD_BUF 8192              // 读缓冲 8KB = 131ms @31250。
//   ★ 为什么不能再大：这块板子没有 PSRAM，GIF 解码必须在启动时拿到一块**连续的**
//     37KB。静态数组每多 8KB，那个连续块就少 8KB，实测 16KB 时 gif 直接分配失败。
//   ★ 为什么 131ms 够：擦 32KB 是 120ms，而 spkTask 手里还有
//     I2S DMA 的 4×7.7ms = 30.7ms —— 131 + 30.7 = 162ms > 120ms，余量 42ms。
//     前提是「从满仓开始擦」，这正是 rnFresh 保证的。

struct MusicInfo {
  char     title[64];
  char     artist[28];
  bool     stream   = false;   // 有声源（手机在推流）
  bool     paused   = false;
  bool     eos      = false;   // 数据发完了
  bool     started  = false;   // 缓冲攒够才开闸（避免开头一路欠载）
  uint32_t sr       = SPK_SR;
  uint32_t wr       = 0;       // 累计写入字节
  uint32_t rd       = 0;       // 累计读出字节
  uint32_t er       = 0;       // 累计已擦除字节
  uint32_t total    = 0;       // 总字节（0 = 未知，纯流式）
  uint32_t lastRxMs = 0;       // 最后一次收到数据的时刻
};

static MusicInfo mus;

static inline uint32_t rnPhys(uint32_t n) { return RN_BASE + (n % RN_SIZE); }
static inline uint32_t rnAvail() { return mus.wr - mus.rd; }                  // 可播字节
static inline uint32_t rnRoom()  { return (RN_SIZE - RN_BLK) - rnAvail(); }   // 还能收多少

static volatile uint16_t rnLeftFr = 0;      // rnBuf 里还剩多少帧没播
static inline uint32_t   rnBufMs() { return (uint32_t)rnLeftFr * 1000UL / mus.sr; }

static SemaphoreHandle_t rnFresh = nullptr;     // 「刚 refill 完」通知（擦除任务等它）
static TaskHandle_t      rnEraseH = nullptr;

// 擦一块 32KB。force=true 给 rnStart 预擦用（那时候还没开播，不需要保护）。
static bool rnEraseOne(bool force = false) {
  if (mus.er - mus.wr >= RN_LOOK) return false;           // 预擦水位够了，先不擦
  if (mus.er + RN_ERASE > mus.rd + RN_SIZE) return false; // 再擦就啃到还没播的数据
  if (!force && rnBufMs() < 110) return false;            // 余粮不到 110ms，等下一个 refill
  u5eraseBlock32(rnPhys(mus.er));                         // 32KB，typ 120ms
  mus.er += RN_ERASE;
  return true;
}

// 写一段（不会被环形末尾截断的语义：返回实际写入的字节数，调用者自己重试剩余）
static size_t rnWrite(const uint8_t *buf, size_t len) {
  if (!u5ok) return 0;
  if (mus.wr >= mus.er) return 0;                        // 这块还没擦
  uint32_t phys = mus.wr % RN_SIZE;
  size_t   room = RN_SIZE - phys;                        // 到环形末尾
  if (len > room) len = room;
  if (len > rnRoom()) len = rnRoom();
  if (!len) return 0;
  u5write(RN_BASE + phys, buf, len);
  mus.wr += len;
  mus.lastRxMs = millis();
  return len;
}

// 读一块到 rnBuf。false = 缓冲里没数据了。
static uint8_t *rnBufPtr() { static uint8_t b[RN_RD_BUF]; return b; }
static int      rnBLen = 0, rnBIdx = 0;

static volatile uint32_t rnUnder = 0;   // 真的等不到数据（环形区空）
static volatile uint32_t rnLate  = 0;   // refill 被擦除挡住 >30ms 的次数 ← 卡顿的真指标
static volatile uint32_t rnPeak  = 0;

static bool rnRefill() {
  if (mus.rd >= mus.wr) return false;
  size_t   n    = mus.wr - mus.rd;
  if (n > RN_RD_BUF) n = RN_RD_BUF;
  uint32_t phys = mus.rd % RN_SIZE;
  size_t   room = RN_SIZE - phys;
  if (n > room) n = room;
  uint32_t t0 = micros();
  u5readRaw(RN_BASE + phys, rnBufPtr(), n);
  // 超过 30ms 说明被擦除的 u5Mtx 挡住了 —— DMA 只有 30.7ms 余量，这时已经在补 0 了
  if (micros() - t0 > 30000UL) rnLate = rnLate + 1;
  mus.rd += n;
  rnBLen = (int)n;
  rnBIdx = 0;
  rnLeftFr = (uint16_t)(n / 2);      // 门控要看这个：里面还有多少帧
  if (rnFresh) xSemaphoreGive(rnFresh);   // 通知擦除任务：现在手里是满仓，可以擦
  return true;
}

// spk.h 的 PCM 钩子：从环形缓冲取样本。返回 false 才摘钩。
static bool rnFill(int16_t *dst, int frames) {
  if (mus.paused) {
    memset(dst, 0, (size_t)frames * 4);
    return true;
  }
  uint32_t av = rnAvail();
  if (av > rnPeak) rnPeak = av;

  // ★ 预缓冲：手机刚点播放时环形区是空的，这时候开闸只会一路欠载（实测 41 次咔哒声）。
  //   等攒够 1 秒音频再出声，代价是按下播放后晚 1 秒开始响。
  if (!mus.started) {
    if (av < mus.sr * 2) {
      memset(dst, 0, (size_t)frames * 4);
      return true;
    }
    mus.started = true;
  }

  uint8_t *b = rnBufPtr();
  for (int i = 0; i < frames; i++) {
    if (rnBIdx + 2 > rnBLen) {
      if (!rnRefill()) {
        // 缓冲空了：填静音等数据，别摘钩（手机可能只是网络抖了一下）
        for (int j = i; j < frames; j++) { dst[j * 2] = 0; dst[j * 2 + 1] = 0; }
        rnUnder = rnUnder + 1;
        // 断流超时：数据发完 eos，或者 4 秒没收到任何东西
        bool dead = mus.eos || (mus.lastRxMs && millis() - mus.lastRxMs > 4000UL);
        // ★ 必须在这里收尾：spkTask 拿到 false 就摘钩、再也不进来，
        //   如果 stream 还挂着 true，GIF 会一直按 8fps 跑（实测播完后掉到 5.6fps）。
        if (dead) mus.stream = false;
        return !dead;
      }
    }
    int16_t s = (int16_t)(b[rnBIdx] | ((int)b[rnBIdx + 1] << 8));
    rnBIdx += 2;
    if (rnLeftFr) rnLeftFr = (uint16_t)(rnLeftFr - 1);
    if (spkMute) s = 0;
    else if (spkVol != 100) s = (int16_t)(((int32_t)s * spkVol) / 100);
    dst[i * 2]     = s;
    dst[i * 2 + 1] = s;
  }
  return true;
}

// 擦除任务：**只在刚 refill 完的那一刻**擦，保证擦的 120ms 有 162ms 的余粮兜着。
//   core1 / 优先级 1：和 loop() 同级，不抢 GIF（core0 优先级 2）也不抢按键（core1 优先级 3）。
static void rnEraseTask(void *) {
  for (;;) {
    if (!mus.stream) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    if (!rnFresh) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    if (xSemaphoreTake(rnFresh, pdMS_TO_TICKS(50)) != pdTRUE) continue;
    // ★ 只认「刚 refill 完」那一次通知。擦除要花 97ms，期间还会有 refill 发生，
    //   信号量是二值的 ⇒ 下一次 take 拿到的可能是**过期的**通知（缓冲只剩几十毫秒），
    //   这时候擦就会把音频抽干（实测还剩 4% 的缺口就是这么来的）。
    //   所以用状态判据再确认一次：手里不足 120ms 就丢掉这次通知，等下一次。
    if (rnBufMs() < 120) { while (xSemaphoreTake(rnFresh, 0) == pdTRUE) {} continue; }
    rnEraseOne();     // 每擦一块 ≈90ms（32KB），h 里的 erase= 能看到最近一次的耗时
  }
}

// 开始一次新的播放流。会丢掉旧内容（预擦 8 块 ≈ 1s）。
static bool rnStart(const char *title, const char *artist, uint32_t total) {
  if (!u5ok) return false;
  if (RN_BASE + RN_SIZE > u5cap) return false;

  audioStop();                       // 别和 audio.h 的整段试听抢钩子
  mus.wr = mus.rd = mus.er = 0;
  mus.eos     = false;
  mus.paused  = false;
  mus.started = false;
  mus.stream  = true;
  mus.total  = total;
  mus.lastRxMs = millis();
  rnBIdx = rnBLen = 0;
  rnUnder = 0;
  rnLate  = 0;
  rnPeak  = 0;
  snprintf(mus.title,  sizeof(mus.title),  "%s", title  ? title  : "Untitled");
  snprintf(mus.artist, sizeof(mus.artist), "%s", artist ? artist : "");

  rnLeftFr = 0;
  for (int i = 0; i < 8; i++) rnEraseOne(true);   // 先备 256KB（还没开播，不用门控）

  if (!rnFresh) rnFresh = xSemaphoreCreateBinary();
  if (rnFresh && !rnEraseH) {
    xTaskCreatePinnedToCore(rnEraseTask, "rnErase", 3072, nullptr, 1, &rnEraseH, 1);
  }
  spkPcmSrc = rnFill;

  Serial.printf("[mus] start \"%s\" / \"%s\" total=%lu  erase1=%lums\n",
                mus.title, mus.artist, (unsigned long)total,
                (unsigned long)u5eraseMs);
  return true;
}

static void rnStop() {
  spkPcmSrc = nullptr;
  mus.stream = false;
  mus.eos    = false;      // 主动停止 ≠ 播完：放完显示 END，停掉显示 READY
  mus.paused = false;
  rnBIdx = rnBLen = 0;
  rnLeftFr = 0;
}

// 保留调用点（主循环每圈都叫）：擦除已经交给 rnEraseTask，这里不再做任何事。
//   ★ 别再往这里塞擦除：loop() 被一条 120ms 的擦除卡住会直接影响按键/翻页手感。
static void rnPump() {}

static void rnReport() {
  Serial.printf("[mus] stream=%d paused=%d eos=%d  avail=%luKB  buf=%lums  wr=%lu rd=%lu er=%lu  under=%lu late=%lu peak=%luKB  erase=%lums\n",
                (int)mus.stream, (int)mus.paused, (int)mus.eos,
                (unsigned long)(rnAvail() / 1024), (unsigned long)rnBufMs(),
                (unsigned long)mus.wr, (unsigned long)mus.rd, (unsigned long)mus.er,
                (unsigned long)rnUnder, (unsigned long)rnLate,
                (unsigned long)(rnPeak / 1024), (unsigned long)u5eraseMs);
}
