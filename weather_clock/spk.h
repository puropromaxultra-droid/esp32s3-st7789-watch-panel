// ============================================================
// spk.h —— MAX98357A 音频引擎（主固件内置，取代 spk_test）
//
// 硬链路（V5 一体板，不用飞线）：
//   BCLK = IO15 → U4.16、LRC = IO16 → U4.14、DIN = IO17 → U4.1
//   SD_MODE 直接接 VSYS（功放常开）、GAIN 悬空 = 9dB
//   喇叭跨接 J7.1 / J7.2（BTL，两脚都不接地）
//
// ★★ 为什么是 31250Hz —— 这一条踩过坑，看仔细：
//
//  I2S 时钟链：160MHz(PLL) ÷ mclk_div → MCLK ÷ bclk_div → BCLK = fs × 32
//  所以必须满足：  mclk_div × bclk_div = 160e6 ÷ (fs × 32)
//  而 IDF 的 i2s_std 驱动只接受 MCLK = fs × {128, 256, 384, 512}，
//  即 mclk_div 只能是 1250/256/... 这几档，必须恰好落在集合里才算"精确"。
//
//  ✗ 40000：160e6 ÷ 1.28e6 = 125 = 5³，只能拆 (25,5) 或 (5,25)
//           → MCLK/fs = 160 或 800，都不在 {128,256,384,512}
//           ⇒ 驱动只能做近似分频，BCLK 带抖动 → **持续沙沙 + 咔哒**（实测）
//  ✓ 31250：160e6 ÷ 1e6 = 160 = (40,4) → MCLK/fs = 128 ✓ 精确
//  ✓ 50000：160e6 ÷ 1.6e6 = 100 = (25,4) → MCLK/fs = 128 ✓ 精确（但带宽 100KB/s 太重）
//  ✓ 25000：160e6 ÷ 0.8e6 = 200 = (50,4) → MCLK/fs = 128 ✓（12.5kHz 上限，音乐发闷）
//
//  31250 的取舍：音质上限 15.6kHz（MP3 本身 cutoff 就在 16kHz，听不出），
//  数据量 62.5KB/s —— 比 50kHz 省 37.5%，比 40000 省 22%，是"精确时钟 + 够用音质 +
//  最省 flash 带宽"的唯一交点。
//
// ★ 设计约束（跟 GIF 播放共存）：
//   - 独立 FreeRTOS 任务跑在 **core0、优先级 1**，低于 GIF 解码任务的 2，
//     永不抢占帧管线；自己 90% 时间阻塞在 i2s_channel_write 上。
//   - 每块 240 帧（4.8ms），4 个 DMA 描述符 ≈ 19ms 缓冲，够跨过一次 SPI 推屏。
//   - 无动态分配、无浮点（正弦查表 + 整数包络），不碰 PSRAM。
//
// 用法：
//   spkInit();               // setup 里一次
//   spkPlay(SND_BOOT);       // 触发提示音（入队，立即返回）
//   spkSetVol(55);           // 0~100
//   spkToggleMute();
// ============================================================
#pragma once
#include <Arduino.h>

#define PIN_SPK_BCLK 15
#define PIN_SPK_LRC  16
#define PIN_SPK_DIN  17

#define SPK_SR      31250            // 见上：160e6 ÷ (31250×32) = 160 → MCLK/fs = 128 精确
#define SPK_FRAMES  240              // 240 帧 = 7.7ms @31250（够粗，DMA 效率更高）
#define SPK_BLOCK   (SPK_FRAMES * 2) // 立体声交错

// 音效编号（加新音效：加枚举 + 两张表 + 个数表）
enum {
  SND_NONE = 0,
  SND_BOOT,      // 开机三音上行
  SND_KEY,       // 按键轻点
  SND_HOUR,      // 整点报时
  SND_LOWBAT,    // 低压告警（下行三音）
  SND_ALERT,     // 提醒（天气/异常）
  SND_COUNT
};

#if __has_include(<driver/i2s_std.h>)
  #include <driver/i2s_std.h>
  #define SPK_I2S_STD 1
  static i2s_chan_handle_t spkTx = NULL;
#else
  #include <driver/i2s.h>
#endif

// ---------------- 旋律表（f=0 是静音，单位 Hz / ms） ----------------
#define N_C5 523
#define N_E5 659
#define N_G5 784
#define N_A5 880
#define N_C6 1047
#define N_G4 392

static const uint16_t M_BOOT_F[]   = { N_C5, N_E5, N_G5, 0 };
static const uint16_t M_BOOT_D[]   = { 130,  130,  320,  40 };

static const uint16_t M_KEY_F[]    = { 1400, 0 };
static const uint16_t M_KEY_D[]    = { 18,   12 };

static const uint16_t M_HOUR_F[]   = { N_G5, 0, N_C5, 0 };
static const uint16_t M_HOUR_D[]   = { 130, 70, 300, 30 };

static const uint16_t M_LOWBAT_F[] = { N_G5, 0, N_E5, 0, N_C5 };
static const uint16_t M_LOWBAT_D[] = { 170, 50, 170, 50, 420 };

static const uint16_t M_ALERT_F[]  = { N_C6, 0, N_C6, 0 };
static const uint16_t M_ALERT_D[]  = { 90,  60, 90,  40 };

static const uint16_t *const SPK_F_TAB[SND_COUNT] = {
  nullptr, M_BOOT_F, M_KEY_F, M_HOUR_F, M_LOWBAT_F, M_ALERT_F
};
static const uint16_t *const SPK_D_TAB[SND_COUNT] = {
  nullptr, M_BOOT_D, M_KEY_D, M_HOUR_D, M_LOWBAT_D, M_ALERT_D
};
static const uint8_t SPK_N_TAB[SND_COUNT] = { 0, 4, 2, 4, 5, 4 };
// 每种音效的相对音量（%），按键要轻、告警要响
// ★ SND_BOOT：100 → 45 → **22**。开机音有 10 个声部叠加，等效响度本来就大：
//   45 时实测峰值 21.7% 仍偏吵，降到 22 后峰值 ~10.6%（≈ 手机小音量）。
// ★ 这张表**只作用于合成音效**（spkLoadNote / spkBootFill）。音乐与整段音频走
//   player.h / audio.h 里的 `s = s * spkVol / 100`，跟这里无关，改它不会动音乐音量。
static const uint8_t SPK_GAIN_TAB[SND_COUNT] = { 0, 22, 35, 70, 100, 85 };

// ---------------- 状态 ----------------
static bool     spkOk      = false;
static volatile bool spkMute = false;
static volatile uint8_t spkVol = 55;      // 0~100

static int16_t  spkBuf[SPK_BLOCK];         // 960 B，内部 RAM
// PCM 源钩子（audio.h 挂上就播 U5 里的音频）：填 frames 帧立体声，返回 false 自动摘钩
static bool (*spkPcmSrc)(int16_t *dst, int frames) = nullptr;
static int16_t  spkSine[256];              // 满幅正弦表
static volatile uint8_t spkQueue[8];
static volatile uint8_t spkQHead = 0, spkQTail = 0;

// 播放器（只被音频任务读写）
static volatile uint8_t spkCur  = SND_NONE;
static volatile bool    spkBusy = false;
static volatile uint32_t bootT0 = 0;      // 开机音开始时刻
static volatile bool     bootAbort = false; // 请求掐掉开机音（有别的音效要播）
static uint8_t  spkNote = 0;
static uint32_t spkLeft = 0;               // 本音符剩余样本
static uint32_t spkPos  = 0;               // 本音符已播样本
static uint32_t spkLen  = 1;
static uint32_t spkPhase = 0;
static uint32_t spkInc   = 0;
static int32_t  spkAmp   = 0;              // Q15 音量，每音符算一次

static void spkQueuePush(uint8_t id) {
  if (id == SND_NONE || id >= SND_COUNT) return;
  uint8_t n = (uint8_t)((spkQHead + 1) & 7);
  if (n == spkQTail) return;                // 满了就丢最新的，不覆盖正在播的
  spkQueue[spkQHead] = id;
  spkQHead = n;
}

static void spkPlay(uint8_t id) {
  // 开机音有 4.2 秒。如果它还在放，新来的音效只能排队等 —— 用户按了键没声，
  // 会以为按键又坏了。所以播过 1.5s 后，任何新音效都直接把开机音掐掉。
  if (id != SND_BOOT && spkCur == SND_BOOT && millis() - bootT0 > 1500) bootAbort = true;
  spkQueuePush(id);
}

static void spkSetVol(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  spkVol = (uint8_t)pct;
}

static void spkToggleMute() { spkMute = !spkMute; }

// 载入一个音效的第 note 个音符（算相位增量与包络上限）
static void spkLoadNote() {
  const uint16_t *F = SPK_F_TAB[spkCur];
  const uint16_t *D = SPK_D_TAB[spkCur];
  uint32_t ms = D[spkNote];
  spkLen  = (ms * SPK_SR) / 1000U;
  if (spkLen < 1) spkLen = 1;
  spkLeft = spkLen;
  spkPos  = 0;
  uint32_t f = F[spkNote];
  spkInc = (uint32_t)(((uint64_t)f << 32) / SPK_SR);   // 频率 → 相位增量
  // Q15 振幅：全局音量 × 本音效增益
  int32_t a = ((int32_t)spkVol * 32767) / 100;
  a = (a * SPK_GAIN_TAB[spkCur]) / 100;
  spkAmp = a;
}

// 填一块。返回后 spkBuf 里是 240 帧立体声
static void spkFill() {
  if (!spkBusy) {
    for (int i = 0; i < SPK_BLOCK; i++) spkBuf[i] = 0;
    return;
  }
  const uint32_t atk = (SPK_SR * 8U) / 1000U;     // 8ms 淡入
  const uint32_t rel = (SPK_SR * 28U) / 1000U;    // 28ms 释音

  for (int i = 0; i < SPK_FRAMES; i++) {
    int16_t s = 0;
    if (spkCur != SND_NONE && !spkMute) {
      const uint16_t *F = SPK_F_TAB[spkCur];
      const uint16_t f = F[spkNote];

      if (f) {
        // 包络：淡入 → 平台（89%）→ 释音
        int32_t env;
        if (spkPos < atk)                 env = (int32_t)(spkPos * 32767U / atk);
        else if (spkLeft < rel)           env = (int32_t)(spkLeft * 29200U / rel);
        else                              env = 29200;
        int32_t amp = (spkAmp * env) >> 15;
        int32_t v   = spkSine[spkPhase >> 24];
        s = (int16_t)((v * amp) >> 15);
        spkPhase += spkInc;
      }
      spkPos++;
      if (--spkLeft == 0) {
        spkNote++;
        if (spkNote >= SPK_N_TAB[spkCur]) {
          spkBusy = false;
          spkCur  = SND_NONE;
        } else {
          spkLoadNote();
        }
      }
    }
    spkBuf[i * 2]     = s;
    spkBuf[i * 2 + 1] = s;
  }
}

// ============ 开机音：仿 Win98「The Microsoft Sound」（多声部叠加） ============
// 原来的 SND_BOOT 是"单音依次播 C5-E5-G5"，听感就是电子琴按了三下。
// Win98 那个启动音的氛围来自**和弦叠加**：低音铺底 + 逐层上行 + 高音铃尾，
// 同时有 3~6 个声部在响 —— 单声部状态机做不出来，所以给它一条专用路径。
//
// 调性 D 大调：D3-A3-D4-F#4-A4-D5-F#5-A5-D6，末尾 A6 铃音收尾，总长 4.2s。
// 表里存的是**样本数**不是毫秒（31250Hz 下预先算好，省掉每帧的除法）。
struct BootEv { uint32_t t0, dur; uint16_t f; uint8_t amp, bell; };

#define BOOT_LEN_MS 4200
#define BOOT_LEN_S  ((uint32_t)BOOT_LEN_MS * SPK_SR / 1000U)

static const BootEv BOOT_EV[] = {
  //  起(样本) 时长(样本)   频率  幅度%  铃
  {      0, 125000,  147, 26, 0 },   // D3  铺底
  {   3750, 118750,  220, 20, 0 },   // A3
  {  10000, 106250,  294, 17, 0 },   // D4
  {  18750,  93750,  370, 15, 0 },   // F#4
  {  29687,  81250,  440, 13, 0 },   // A4
  {  42187,  68750,  587, 12, 0 },   // D5
  {  56250,  56250,  740, 11, 0 },   // F#5
  {  70312,  46875,  880, 11, 0 },   // A5
  {  84375,  40625, 1175, 12, 0 },   // D6
  { 103125,  28125, 1760, 16, 1 },   // A6  铃尾
};
#define BOOT_N (sizeof(BOOT_EV) / sizeof(BOOT_EV[0]))

static uint32_t bootPos = 0;
static uint32_t bootInc[BOOT_N];
static uint32_t bootPh[BOOT_N];

static void bootStart() {
  bootPos = 0;
  bootT0  = millis();
  bootAbort = false;
  for (uint8_t i = 0; i < BOOT_N; i++) {
    bootPh[i]  = 0;
    bootInc[i] = (uint32_t)(((uint64_t)BOOT_EV[i].f << 32) / SPK_SR);
  }
}

// 填一块 PCM。返回 false = 播完了
static bool spkBootFill(int16_t *dst, int frames) {
  int32_t g = ((int32_t)spkVol * 32767) / 100;
  g = (g * SPK_GAIN_TAB[SND_BOOT]) / 100;

  for (int i = 0; i < frames; i++, bootPos++) {
    int32_t acc = 0;
    if (!spkMute && bootPos < BOOT_LEN_S) {
      for (uint8_t k = 0; k < BOOT_N; k++) {
        const BootEv &e = BOOT_EV[k];
        if (bootPos < e.t0) continue;
        uint32_t t = bootPos - e.t0;
        if (t >= e.dur) continue;

        // 包络：pad = 260ms 淡入 / 保持 / 600ms 淡出；铃 = 4ms 淡入 + 平方衰减
        uint32_t atk = e.bell ? (SPK_SR * 4U / 1000U) : (SPK_SR * 260U / 1000U);
        int32_t env;
        if (t < atk) {
          env = (int32_t)(t * 32767U / atk);
        } else {
          uint32_t rel  = e.bell ? (e.dur / 2) : (SPK_SR * 600U / 1000U);
          uint32_t left = e.dur - t;
          if (rel < 1) rel = 1;
          if (left < rel) {
            int32_t x = (int32_t)(left * 32767U / rel);
            env = e.bell ? (x * x) / 32767 : x;   // 铃用平方，衰减更像真实乐器
          } else {
            env = 32767;
          }
        }
        int32_t a = (g * (int32_t)e.amp) / 100;
        int32_t v = spkSine[bootPh[k] >> 24];
        acc += (v * ((a * env) >> 15)) >> 15;
        bootPh[k] += bootInc[k];
      }
    }
    if (acc >  32000) acc =  32000;
    if (acc < -32000) acc = -32000;
    dst[i * 2]     = (int16_t)acc;
    dst[i * 2 + 1] = (int16_t)acc;
  }
  return bootPos < BOOT_LEN_S;
}

static size_t spkWrite(const void *p, size_t bytes) {
  size_t w = 0;
  if (!spkOk) return 0;
#ifdef SPK_I2S_STD
  i2s_channel_write(spkTx, p, bytes, &w, pdMS_TO_TICKS(200));
#else
  i2s_write(I2S_NUM_0, p, bytes, &w, pdMS_TO_TICKS(200));
#endif
  return w;
}

static void spkTask(void *) {
  for (;;) {
    if (spkPcmSrc) {                  // 有 PCM 源就优先播它（U5 音频）
      if (spkPcmSrc(spkBuf, SPK_FRAMES)) spkWrite(spkBuf, sizeof(spkBuf));
      else spkPcmSrc = nullptr;       // 播完/出错自动摘钩，回到合成音模式
      continue;
    }
    if (!spkBusy) {
      // 空闲：取队列；没有就睡 20ms（不喂静音，DMA 的 auto_clear 会补 0）
      if (spkQTail != spkQHead) {
        uint8_t id = spkQueue[spkQTail];
        spkQTail = (uint8_t)((spkQTail + 1) & 7);
        if (id >= 1 && id < SND_COUNT) {
          spkCur  = id;
          spkNote = 0;
          spkPhase = 0;
          spkLoadNote();
          if (id == SND_BOOT) bootStart();   // ★ 开机音走多声部专用路径
          spkBusy = true;
        }
      } else {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      continue;
    }
    if (spkCur == SND_BOOT) {
      bool ok = spkBootFill(spkBuf, SPK_FRAMES);
      if (!ok || bootAbort) {
        spkBusy    = false;
        spkCur     = SND_NONE;
        bootAbort  = false;
      }
    } else {
      spkFill();
    }
    spkWrite(spkBuf, sizeof(spkBuf));
  }
}

static void spkInit() {
  for (int i = 0; i < 256; i++) {
    spkSine[i] = (int16_t)lroundf(sinf((float)i * 6.2831853f / 256.0f) * 32767.0f);
  }

#ifdef SPK_I2S_STD
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear   = true;             // 欠载输出 0，别重复旧样本
  cc.dma_desc_num = 4;
  cc.dma_frame_num = SPK_FRAMES;
  if (i2s_new_channel(&cc, &spkTx, NULL) != ESP_OK) {
    Serial.println("[spk] ERR new_channel");
    return;
  }
  i2s_std_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SR);
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  sc.gpio_cfg.bclk = (gpio_num_t)PIN_SPK_BCLK;
  sc.gpio_cfg.ws   = (gpio_num_t)PIN_SPK_LRC;
  sc.gpio_cfg.dout = (gpio_num_t)PIN_SPK_DIN;
  sc.gpio_cfg.din  = I2S_GPIO_UNUSED;
  if (i2s_channel_init_std_mode(spkTx, &sc) != ESP_OK) {
    Serial.println("[spk] ERR init_std_mode");
    return;
  }
  if (i2s_channel_enable(spkTx) != ESP_OK) {
    Serial.println("[spk] ERR enable");
    return;
  }
#else
  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = SPK_SR;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 4;
  cfg.dma_buf_len          = SPK_FRAMES;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("[spk] ERR driver_install");
    return;
  }
  i2s_pin_config_t pc;
  memset(&pc, 0, sizeof(pc));
  pc.bck_io_num   = PIN_SPK_BCLK;
  pc.ws_io_num    = PIN_SPK_LRC;
  pc.data_out_num = PIN_SPK_DIN;
  pc.data_in_num  = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pc) != ESP_OK) {
    Serial.println("[spk] ERR set_pin");
    return;
  }
#endif

  spkOk = true;
  // ★ 优先级 3（不是 1）：DMA 只有 4×7.7ms = 30.7ms 余量，而 core0 上跑着 HTTP
  //   （优先级 2）和 GIF（优先级 2）。它们连续干 30ms 以上（比如一次 32KB 推流
  //   要写 32 页 flash）就会把 spkTask 饿到 DMA 排空 —— 每个 7.7ms 空档就是一段静音，
  //   一秒漏 5 个就是 4% "慢" + 断续（实测 rd 只有 60000 B/s 而不是 62500）。
  //   音频任务本身每 7.7ms 只占几微秒（一次 960B 拷贝 + 一次驱动调用），
  //   抬高优先级对画面几乎零影响，却是音频不断的关键。
  if (xTaskCreatePinnedToCore(spkTask, "spk", 3072, nullptr, 3, nullptr, 0) != pdPASS) {
    Serial.println("[spk] ERR task");
    spkOk = false;
    return;
  }
  Serial.printf("[spk] 就绪 %dHz 16bit  BCLK=%d LRC=%d DIN=%d  vol=%u%%\n",
                SPK_SR, PIN_SPK_BCLK, PIN_SPK_LRC, PIN_SPK_DIN, spkVol);
}

// 串口/系统页用：一句话状态
static void spkReport() {
  Serial.printf("[spk] ok=%d busy=%d cur=%u vol=%u%% mute=%d q=%u\n",
                (int)spkOk, (int)spkBusy, (unsigned)spkCur,
                (unsigned)spkVol, (int)spkMute,
                (unsigned)((spkQHead - spkQTail) & 7));
}
