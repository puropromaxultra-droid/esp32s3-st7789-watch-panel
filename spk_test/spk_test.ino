// ============================================================
// spk_test —— U4 (MAX98357A) + 喇叭 链路验证
// V5 一体板 · ESP32-S3-MINI-1-N8
//
// 板上已经连好，不用飞任何线：
//   BCLK = IO15 (U1.19) → U4.16
//   LRC  = IO16 (U1.20) → U4.14
//   DIN  = IO17 (U1.21) → U4.1
//   SD_MODE# 直接接 VSYS ⇒ 功放常开；GAIN 悬空 = 9dB
//   喇叭跨接 J7.1(SPK_P) / J7.2(SPK_N)，两脚都不接地
//
// 播放内容（不放扫频，全在 523~880Hz，不刺耳）：
//   ① 提示三音：C5 → E5 → G5（叮-咚-叮）
//   ② 静音 0.9s
//   ③ 小星星前一句：C5 C5 G5 G5 A5 A5 G5(长)
//   ④ 静音 1.5s，循环
//
// ★ 每个音 15ms 淡入 + 缓降 + 40ms 释音，避免开关爆音(POP)。
//   改旋律只要动 MEL_F / MEL_MS 两张表（f = 0 表示静音）。
//
// 串口 115200：+ / - 调音量（默认 0.25），m 静音开关，s 重播
//
// 烧录：
//   arduino-cli compile --upload -p COM15 \
//     --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc spk_test
// ============================================================

#include <Arduino.h>

#define PIN_I2S_BCLK 15   // U1.19
#define PIN_I2S_LRC  16   // U1.20
#define PIN_I2S_DIN  17   // U1.21

#define SR     44100
#define FRAMES 256              // 每块 256 帧 = 5.8ms
#define BLOCK  (FRAMES * 2)     // 立体声交错 = 512 个 int16

// IDF5 有 i2s_std.h（新 API），IDF4 只有老的 driver/i2s.h
#if __has_include(<driver/i2s_std.h>)
  #define USE_I2S_STD 1
  #include <driver/i2s_std.h>
  static i2s_chan_handle_t txHandle = NULL;
#else
  #include <driver/i2s.h>
#endif

static int16_t sBuf[BLOCK];

// ★ 启动日志长期被 USB 枚举吞掉（烧录后主机 1~2 秒才连上），
//   于是"I2S 到底初始化成功没有"一直没验证过。存状态，用串口 'i' 随时查。
static volatile bool   i2sOk  = false;
static volatile size_t lastWr = 0;

// ★ 2026-11-19：原来 0.25f（-12dBFS）实测只有 87mW，听得见但很微弱。
//   链路验证阶段直接给 0.90f（约 1.1W），确认能响之后再按需往下调。
static volatile float sVol     = 0.90f;
static volatile bool  sMute    = false;
static volatile bool  sRestart = false;
// 连续 1kHz 满幅正弦：专门给万用表 AC 档准备的，读数不抖
static volatile bool  sCont    = false;
static double         sContPh  = 0.0;

// ★ 诊断模式（串口 'd'）：送恒定满幅 DC 3 秒。
//   BTL 空闲时 OUTP/OUTN 都停在 VSYS/2（≈2.35V）；加满幅 DC 后一路顶到接近 VSYS、
//   另一路顶到接近 0V。万用表 **DC 档** 一眼就能看出"功放到底有没有在放大"，
//   比 AC 档可靠得多（AC 档测 1kHz 大部分表读数只到一半）。
//   3 秒后自动停：8Ω 上 2.4V 直流 = 0.3A / 0.72W，短时间不伤音圈。
static volatile bool     sDC    = false;
static volatile uint32_t sDCT0  = 0;

// ---- 音符表 ----
#define C5 523.25f
#define D5 587.33f
#define E5 659.25f
#define F5 698.46f
#define G5 783.99f
#define A5 880.00f

static const float MEL_F[] = {
  C5,   0,  E5,   0,  G5,   0,
  C5,  C5,  G5,  G5,  A5,  A5,  G5,   0
};
static const uint16_t MEL_MS[] = {
  200,  80, 200,  80, 350, 900,
  280, 280, 280, 280, 280, 280, 620, 1500
};
static const int NOTE_N = sizeof(MEL_F) / sizeof(MEL_F[0]);

static double sPhase = 0.0;     // 正弦相位（弧度），跨音符不清零，避免咔哒
static float  sNoteT = 0.0f;    // 当前音符内已过时间(ms)
static int    sNote  = 0;

static void i2sInit()
{
#ifdef USE_I2S_STD
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear = true;           // 欠载时输出 0，别重复旧数据
  if (i2s_new_channel(&cc, &txHandle, NULL) != ESP_OK) {
    Serial.println("[spk] ERR new_channel");
    return;
  }

  i2s_std_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SR);
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  sc.gpio_cfg.bclk = (gpio_num_t)PIN_I2S_BCLK;
  sc.gpio_cfg.ws   = (gpio_num_t)PIN_I2S_LRC;
  sc.gpio_cfg.dout = (gpio_num_t)PIN_I2S_DIN;
  sc.gpio_cfg.din  = I2S_GPIO_UNUSED;

  if (i2s_channel_init_std_mode(txHandle, &sc) != ESP_OK) {
    Serial.println("[spk] ERR init_std_mode");
    return;
  }
  if (i2s_channel_enable(txHandle) != ESP_OK) {
    Serial.println("[spk] ERR enable");
    return;
  }
#else
  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = SR;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = FRAMES;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;
  cfg.fixed_mclk           = 0;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("[spk] ERR driver_install");
    return;
  }
  i2s_pin_config_t pc;
  memset(&pc, 0, sizeof(pc));
  pc.bck_io_num   = PIN_I2S_BCLK;
  pc.ws_io_num    = PIN_I2S_LRC;
  pc.data_out_num = PIN_I2S_DIN;
  pc.data_in_num  = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pc) != ESP_OK) {
    Serial.println("[spk] ERR set_pin");
    return;
  }
#endif
  i2sOk = true;
  Serial.printf("[spk] I2S ok  %dHz 16bit stereo\n", SR);
  Serial.printf("[spk] BCLK=%d  LRC=%d  DIN=%d\n", PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN);
}

static size_t i2sWrite(const void *p, size_t bytes)
{
  size_t w = 0;
#ifdef USE_I2S_STD
  i2s_channel_write(txHandle, p, bytes, &w, portMAX_DELAY);
  lastWr = w;
#else
  i2s_write(I2S_NUM_0, p, bytes, &w, portMAX_DELAY);
#endif
  return w;
}

// 填满一块：两声道写同样的样本（MAX98357 单声道接法下也吃立体声）
static void refill()
{
  // ---- 'd'：恒定满幅 DC（诊断），3 秒后自动收 ----
  if (sDC) {
    if (millis() - (uint32_t)sDCT0 > 15000UL) {
      sDC = false;
      Serial.println("[spk] dc end");
    } else {
      for (int i = 0; i < FRAMES; i++) {
        sBuf[i * 2]     = 32000;
        sBuf[i * 2 + 1] = 32000;
      }
      return;
    }
  }

  // ---- 连续 1kHz（'c' 切换）：电平恒定，AC 表读数稳定 ----
  if (sCont) {
    const float amp = sMute ? 0.0f : 0.90f;   // ★ 满幅：AC 档读数才够大（原来跟 sVol=0.25，读数只有 1/4）
    for (int i = 0; i < FRAMES; i++) {
      int16_t v = (int16_t)(sinf((float)sContPh) * amp * 32767.0f);
      sBuf[i * 2]     = v;
      sBuf[i * 2 + 1] = v;
      sContPh += 6.2831853 * 1000.0 / (float)SR;
      if (sContPh > 6.2831853) sContPh -= 6.2831853;
    }
    return;
  }

  if (sRestart) {
    sRestart = false;
    sNote = 0;
    sNoteT = 0.0f;
    sPhase = 0.0;
  }

  const float atk = 15.0f;    // 淡入 ms
  const float rel = 40.0f;    // 释音 ms

  for (int i = 0; i < FRAMES; i++) {
    const float dur = (float)MEL_MS[sNote];
    const float f   = MEL_F[sNote];

    float env = 0.0f;
    if (f > 0.0f) {
      // 淡入 → 缓降（降到 65%）→ 释音
      if (sNoteT < atk) {
        env = sNoteT / atk;
      } else {
        env = 1.0f - 0.35f * (sNoteT - atk) / (dur - atk);
      }
      if (sNoteT > dur - rel) {
        env *= (dur - sNoteT) / rel;
      }
      if (env < 0.0f) env = 0.0f;

      sPhase += 6.2831853 * f / (float)SR;
      if (sPhase > 6.2831853) sPhase -= 6.2831853;
    }

    float amp = sMute ? 0.0f : sVol;
    int16_t s = (int16_t)(sinf((float)sPhase) * amp * env * 32767.0f);
    sBuf[i * 2]     = s;
    sBuf[i * 2 + 1] = s;

    sNoteT += 1000.0f / (float)SR;
    if (sNoteT >= dur) {
      sNoteT -= dur;
      sNote = (sNote + 1) % NOTE_N;
      if (sNote == 0) Serial.println("[spk] -- loop --");
    }
  }
}

void setup()
{
  Serial.begin(115200);
  // ★ 等 USB CDC 真正连上主机（arduino-cli 烧完会关端口，抓日志的脚本要 1~2 秒才开）。
  //   原来 delay(400) 太早，setup 里所有日志都被丢掉了。
  { uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 8000) delay(50); }

  Serial.println();
  Serial.println("=== spk_test · MAX98357A ===");
  i2sInit();
  Serial.printf("[spk] vol=%.2f   (+/- 调音量, m 静音, s 重播, c 连续1kHz)\n", sVol);

  refill();
}

void loop()
{
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '+' || c == '=') {
      sVol += 0.1f;
      if (sVol > 1.0f) sVol = 1.0f;
      Serial.printf("[spk] vol=%.2f\n", sVol);
    } else if (c == '-' || c == '_') {
      sVol -= 0.1f;
      if (sVol < 0.05f) sVol = 0.05f;
      Serial.printf("[spk] vol=%.2f\n", sVol);
    } else if (c == 'm') {
      sMute = !sMute;
      Serial.printf("[spk] mute=%d\n", (int)sMute);
    } else if (c == 's') {
      sRestart = true;
      Serial.println("[spk] restart");
    } else if (c == 'c') {
      sCont = !sCont;
      if (!sCont) { sRestart = true; sContPh = 0.0; }
      Serial.printf("[spk] cont1k=%d\n", (int)sCont);
    } else if (c == 'i') {
      Serial.printf("[spk] i2s=%d  lastWr=%u/%u  vol=%.2f mute=%d cont=%d\n",
                    (int)i2sOk, (unsigned)lastWr, (unsigned)sizeof(sBuf),
                    (float)sVol, (int)sMute, (int)sCont);
    } else if (c == 'd') {          // 恒定满幅 DC 3 秒：万用表 DC 档判断功放是否在放大
      sDC   = true;
      sDCT0 = millis();
      Serial.println("[spk] DC 满幅 15s → 量 J7 两脚（应变一高一低，不是都 2.37V）");
    }
  }

  // ★ 顺序是「先填后写」。原来写成先写后填，每轮送出去的都是上一轮的缓冲，
  //   声音整体晚一块（5.8ms）；堵满时听起来还会像卡了半拍。
  refill();
  i2sWrite(sBuf, sizeof(sBuf));
}
