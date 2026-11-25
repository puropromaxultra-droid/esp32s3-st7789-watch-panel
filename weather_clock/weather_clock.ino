// ============================================================
// 天气时钟 v3 —— ST7789 240x240 + ESP32-S3
// WiFi + NTP + Open-Meteo 天气 + GIF 动图 + 音乐播放 + 双按钮翻页
//
// 七页（A 上一页 / B 下一页）：
//   [0] GIF      全屏动图（数据存在 U5 外挂 16MB flash 里，用 tools/upload_gif.py 传）
//   [1] CLOCK    大字时间 + 天气卡
//   [2] FORECAST 未来三天
//   [3] SUN      日出日落 + 太阳位置 + 月相
//   [4] MUSIC    音乐播放器（手机 WiFi 推流，U5 环形缓冲）★长按 A/B 调音量
//   [5] BATTERY  电量 / 充电状态 / 充满·用尽倒计时
//   [6] SYSTEM   运行时状态
//
// 按键（长按按页面分派，见 handleButtons）：
//   短按   A/B = 上一页 / 下一页
//   长按   MUSIC：A 音量− / B 音量+（按住不放连发，一档 5%）
//          CLOCK：立即刷天气（原"确认"语义）
//          其它页：回时钟页
//   双键同按 700ms = 任何页回时钟（音乐页的长按被调音量占用了，回家走这条）
//
// ★ 铁律（踩过的坑，别动）：
//   1) IM 必须拉高 —— 拉低 = 非 SPI 模式，初始化不生效，屏全黑
//   2) 推像素必须 writeBytes 批量 —— 逐字节 transfer 慢十倍
//   3) Open-Meteo 的 current_units 段也含 "temperature_2m":"°C"，
//      解析必须先定位 "current":{ / "daily":{ 子段
//
// 接线：SCK=4 MOSI=5 CS=6 DC=7 RST=8 IM=9
//       按钮 A=IO10  B=IO11（另一端接 GND，内部上拉）
//
// 串口：n 下一页 / p 上一页 / w 刷天气 / t LCD 自检 / 1·2·3 切 SPI 10·26·40MHz
//       U 进入 U5 上传模式（配 tools/upload_gif.py）
//
// ★ 换 GIF：把 .gif 拖到 tools\转换GIF.bat 上就行 —— 自动转换 + 编译 + 烧录。
//   本工程自包含：驱动 / GIF 播放 / 自检全在这一个 .ino 里，不依赖 lcd_fast。
// ============================================================

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <lwip/dns.h>
#include <freertos/semphr.h>
#include <Preferences.h>       // 音量存 NVS（重启后不回到 55%）

#include "secrets.h"   // WiFi 凭据：复制 secrets.h.example 为 secrets.h 后填写
#include "u5flash.h"   // U5 外挂 GD25Q128 16MB：驱动 + 串口上传
#include "gif_store.h" // 从 U5 读 GIF blob（替代编进固件的 gif_data.h）
#include "spk.h"       // U4 MAX98357A 音频引擎（50kHz，独立任务，不干扰帧管线）
#include "audio.h"     // U5 音频区（AUDB）：上传 wav → 从 flash 流式播到 I2S
#include "player.h"    // 音乐播放器：手机推流 → U5 4MB 环形缓冲 → I2S
#include "webstream.h" // 手机浏览器点播的 HTTP 服务端（手写，省 RAM）

// 武汉坐标
#define LAT 30.5928f
#define LON 114.3055f

// ---------------- 屏幕 ----------------
#define W 240
#define H 240

// ★ V5 一体板接线（2026-11-14 改）：U1 pad13~18 = IO9~IO14
#define PIN_SCK  12      // U1.16
#define PIN_MOSI 11      // U1.15
#define PIN_CS   10      // U1.14
#define PIN_DC   9       // U1.13
#define PIN_RST  14      // U1.18
#define PIN_BL   13      // U1.17 → Q1 栅极（低边开关，HIGH = 背光亮）
// IM：V5 板上直接接 3V3，不再占 GPIO（面包板时代的 IO9 已释放）

// 两个轻触开关：一脚接 GND，另一脚接这里；内部上拉，按下读到 LOW
// ★ A 键不走 U1 的脚了 —— MINI-1 是 LGA，46 个 pad 全压在底面，探针和烙铁都够不到，
//   pad7(IO3)/pad8(IO4) 都焊不了。全板唯一"引到能碰的焊盘上"的空闲 IO 是 J6。
#define PIN_BTN_A 0      // ★ 飞线后：SW1 3 脚 → J6 的 1 脚（方形焊盘，丝印 IO0）
#define PIN_BTN_B 5      // U1.9  → SW2 右键（下一页）

// ★ 2026-11-22 二次实测确认（串口 'b' 连读）：按住 A 读 A=H 共 31/31 次，
//   同时按住 B 读 B=L 共 23/23 次 ⇒ 读数链路、去抖、引脚宏都正常，A 是**硬件断**，
//   和 11-18 判断一致（U1 pad8 = IO4 底焊盘虚焊）。软件改不回来。
//   ⇒ 飞线方案：SW1 的 3 脚（键侧）→ **J6 的 1 脚**（1x02 排针的方形焊盘 = IO0、2 脚是 GND）。
//     为什么不飞 U1：MINI-1 是 LGA 全底焊盘，pad7(IO3) 根本没有走线也没有过孔，
//     pad8/pad9 又压在模组底下 ⇒ 唯一能下手的备用 IO 就是已经引到 J6 的 IO0。
//   代价：IO0 是**启动模式 strapping 脚** —— 按住 A 上电会停在 download（程序不跑），
//     松手重新上电即恢复，不损坏。所以别按住 A 冷启动，J6 也别再插跳线帽。
//   ⇒ 没飞线时 IO0 靠内部上拉恒为 H，扫描任务读不到按下，B 单键照旧可用；
//     'b' 命令可随时看 A/B 实时电平，按住变 L 就是焊通了。
#define BTN_A_OK 1

// ---- 去抖参数（2026-11-23 重做：非对称 + 提高采样率）----
// ★ 症状：B 键"有时候按几下才动一次"。轻触开关用了几个月，触点会氧化，
//   一次人手按动在电信号上其实是一串 1~3ms 的短脉冲（示波器上看得清楚）。
//   原来对称的"20ms 内电平不许变"去抖 + 4ms 采样，会把整串脉冲全判成抖动
//   ⇒ 20ms 窗口永远赢不了 ⇒ 事件根本不产生，看起来就是"按了没反应"。
// 现在：按下只要连续 3 次采样(≈6ms)都是 LOW 就认（宁可灵敏）；
//       松手要稳定 16ms 才认（松手抖动很多，而且此时短按已记下，
//       认早了会把一次按动拆成多次 → 一次按动翻两页）。
#define BTN_SAMPLE_MS      1     // 扫描周期（原 4ms）—— 1kHz 采样才打得中 1~3ms 的脉冲
#define BTN_DN_CNT         2     // 连续几次 LOW 采样才算按下（≈2ms）
#define BTN_UP_MS         16     // 松手要稳定多久才算松开
#define BTN_MIN_MS        60     // 两次"按下"之间至少隔这么久（防抖动把一次按动拆成多次翻页）
#define BTN_LONG_MS     1200     // 单键长按（页面动作，见 handleButtons）
#define BTN_REP_MS       260     // 长按按住不放时的连发间隔（调音量用）
#define BTN_BOTH_MS      700     // 双键同按 = 回时钟（给"长按被页面占用"的页留逃生口）
#define BTN_PAIR_MS      400     // 只有两键按下时刻相差 <400ms 才算"同按"
                                 // （A 是飞线，IO0 偶尔被 WiFi 干扰出假低电平，
                                 //   若不配对就会把 B 的单键短按当双键吞掉）

// ★ GIF 移到了 U5（外挂 16MB）。尺寸在 gifLoad() 里按屏宽高校验，
//   生成脚本 tools/gif2bin.py 固定 --size 240，尺寸不符会拒绝加载。

// 颜色（RGB565）
#define C_BG    0x0000
#define C_CARD  0x10A2   // 深灰蓝卡片
#define C_LINE  0x2124
#define C_TRACK 0x18E3
#define C_WHITE 0xFFFF
#define C_YEL   0xFFE0
#define C_CYAN  0x07FF
#define C_GREEN 0x07E0
#define C_RED   0xF800
#define C_ORG   0xFD20
#define C_GRAY  0x8410
#define C_GRAY2 0x630C
#define C_PURP  0xA81F
#define C_BROWN 0x8200

// ★ 2026-11-19 提帧率：26MHz 推满 115200B 要 35.4ms，把上限锁在 ~28fps。
//   实测这块屏（PCB 走线 + 短 FPC）40 / 80MHz 都稳跑：40MHz 推满 26.6ms，80MHz 15.1ms
//   （有效约 61Mbps —— S3 的 SPI FIFO 只有 64B，是 CPU 搬 FIFO 而不是真 DMA，再高也吃不满）。
//   花屏就往回退：串口 1/2/3/4 = 10 / 26 / 40 / 80MHz。
static SPISettings spiCfg(80000000UL, MSBFIRST, SPI_MODE0);
static uint32_t    spiHzNow = 80000000UL;   // 实际在跑的 SPI 频率（SYS 页要显示真值）

// ---------------- 时延统计（查"按一下半天才翻页"用） ----------------
// 声明放在最前面：drawSysPage / gifTick / loop 都要用，.ino 里只能靠前的定义。
// quietLog 也在这一批：一条 printf 在 115200 下要 8~10ms，正好把那一帧顶晚；
// USB CDC 主机不读时更狠，旧版要忙等 250ms（tx_timeout）。
static bool quietLog = true;          // 默认静音（'q' 切换回来）
static int  bootReset = 0;            // 本次启动的复位原因，'h' 里报一次（崩溃后能查）
static uint32_t pendingFlipMs = 0;    // 已按下、还没画完的那次翻页：按下的时刻
static uint32_t flipLastMs    = 0;    // 最近一次：按下 → 新页第一帧上屏
static uint32_t flipMaxMs     = 0;    // 历史最差
static uint16_t flipCnt       = 0;
static uint32_t loopGapMaxUs  = 0;    // 本窗口内主循环两次迭代的最大间隔（>100ms = 被谁堵住了）
static uint32_t loopPrevUs    = 0;
static uint32_t lastFlipAtMs  = 0;    // 上次翻页完成的时刻：2 秒内不主动发起网络请求
static volatile bool inNet    = false;  // 正在网络阻塞里（此时只放行轻量串口命令）
static uint16_t   *fb = nullptr;          // 整屏帧缓冲 W*H*2 = 115200 B

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

// ---------------- 背光 PWM（省电大头） ----------------
// Q1 只是低边开关，背光 LED 串在 3V3 上无限流 —— 全亮时电流很大。
// 用 LEDC 调占空比，35% 亮度已经够看，电流直接降到三分之一。
#define BL_DEFAULT 35
static uint8_t blPct = BL_DEFAULT;
static bool    blPwm = false;

static void blSet(uint8_t pct) {
  if (pct > 100) pct = 100;
  blPct = pct;
  if (blPwm) ledcWrite(PIN_BL, (uint32_t)255 * pct / 100);
  else digitalWrite(PIN_BL, pct > 0 ? HIGH : LOW);
}

// ---------------- SPI 底层 ----------------
static void tCmd(uint8_t c) {
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  SPI.transfer(c);
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}
static void tData(const uint8_t *d, uint32_t n) {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  SPI.writeBytes(d, n);
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}
static void tCmdN(uint8_t c, const uint8_t *d, uint32_t n) { tCmd(c); tData(d, n); }
static void tCmd1(uint8_t c, uint8_t d) { tCmd(c); tData(&d, 1); }

static void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  uint8_t ca[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
  uint8_t ra[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
  tCmd(0x2A); tData(ca, 4);
  tCmd(0x2B); tData(ra, 4);
}

// 原厂完整初始化序列（lcd_fast 已验证）
static void lcdInit() {
  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  // ★ 背光：Q1 低边开关，PWM 调光（R4 下拉，默认灭）
  blPwm = ledcAttach(PIN_BL, 5000, 8);
  if (!blPwm) { pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH); }
  else blSet(BL_DEFAULT);
  delay(20);

  SPI.begin(PIN_SCK, -1, PIN_MOSI);

  digitalWrite(PIN_RST, LOW);  delay(100);
  digitalWrite(PIN_RST, HIGH); delay(200);

  tCmd1(0x36, 0x00);                 // MADCTL
  tCmd1(0x3A, 0x55);                 // COLMOD 16bit

  const uint8_t b2[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
  tCmdN(0xB2, b2, 5);                // PORCTRL
  tCmd1(0xB7, 0x35);                 // GCTRL
  tCmd1(0xBB, 0x19);                 // VCOMS
  tCmd1(0xC0, 0x2C);                 // LCMCTRL
  tCmd1(0xC2, 0x01);                 // VDVVRHEN
  tCmd1(0xC3, 0x12);                 // VRHS
  tCmd1(0xC4, 0x20);                 // VDVS
  tCmd1(0xC6, 0x0F);                 // FRCTRL2
  const uint8_t d0[] = { 0xA4, 0xA1 };
  tCmdN(0xD0, d0, 2);                // PWCTRL1

  const uint8_t ep[] = {
    0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
    0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23
  };
  tCmdN(0xE0, ep, 14);
  const uint8_t en[] = {
    0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
    0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23
  };
  tCmdN(0xE1, en, 14);

  tCmd(0x21); delay(10);             // INVON，IPS 必须开
  tCmd(0x11); delay(120);            // SLPOUT
  tCmd(0x29); delay(50);             // DISPON
}

// ---------------- 帧缓冲 + 脏矩形 ----------------
static int dX0, dY0, dX1, dY1;

static inline void dirtyClear() { dX0 = W; dY0 = H; dX1 = -1; dY1 = -1; }
static inline void markDirty(int x0, int y0, int x1, int y1) {
  x0 = imax(0, x0); y0 = imax(0, y0);
  x1 = imin(W - 1, x1); y1 = imin(H - 1, y1);
  if (x0 > x1 || y0 > y1) return;
  if (x0 < dX0) dX0 = x0;
  if (y0 < dY0) dY0 = y0;
  if (x1 > dX1) dX1 = x1;
  if (y1 > dY1) dY1 = y1;
}

static inline void px(int x, int y, uint16_t c) {
  if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
  fb[(size_t)y * W + x] = c;
  markDirty(x, y, x, y);
}

static void fillRect(int x, int y, int w, int h, uint16_t c) {
  int x0 = imax(0, x), y0 = imax(0, y);
  int x1 = imin(W, x + w), y1 = imin(H, y + h);
  if (x0 >= x1 || y0 >= y1) return;
  for (int yy = y0; yy < y1; yy++) {
    uint16_t *p = fb + (size_t)yy * W;
    for (int xx = x0; xx < x1; xx++) p[xx] = c;
  }
  markDirty(x0, y0, x1 - 1, y1 - 1);
}

static void hline(int x0, int x1, int y, uint16_t c) {
  if (y < 0 || y >= H) return;
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  x0 = imax(0, x0); x1 = imin(W - 1, x1);
  if (x0 > x1) return;
  uint16_t *p = fb + (size_t)y * W;
  for (int x = x0; x <= x1; x++) p[x] = c;
  markDirty(x0, y, x1, y);
}

static void line(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    px(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void disc(int cx, int cy, int r, uint16_t c) {
  if (r <= 0) return;
  for (int y = -r; y <= r; y++) {
    int dx = (int)sqrtf((float)(r * r - y * y));
    hline(cx - dx, cx + dx, cy + y, c);
  }
}

static void ring(int cx, int cy, int r, uint16_t c) {
  int x = r, y = 0, e = 1 - r;
  while (x >= y) {
    px(cx + x, cy + y, c); px(cx - x, cy + y, c);
    px(cx + y, cy + x, c); px(cx - y, cy + x, c);
    px(cx - x, cy - y, c); px(cx + x, cy - y, c);
    px(cx - y, cy - x, c); px(cx + y, cy - x, c);
    y++;
    if (e < 0) e += 2 * y + 1;
    else { x--; e += 2 * (y - x) + 1; }
  }
}

static void rrect(int x, int y, int w, int h, int r, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  if (r <= 0) { fillRect(x, y, w, h, c); return; }
  fillRect(x + r, y, w - 2 * r, h, c);
  fillRect(x, y + r, r, h - 2 * r, c);
  fillRect(x + w - r, y + r, r, h - 2 * r, c);
  disc(x + r, y + r, r, c);
  disc(x + w - r - 1, y + r, r, c);
  disc(x + r, y + h - r - 1, r, c);
  disc(x + w - r - 1, y + h - r - 1, r, c);
}

// ---------------- 5x7 字库（ASCII 32~126） ----------------
static const uint8_t font5x7[][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
  {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
  {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
  {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
  {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
  {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
  {0x00,0x56,0x36,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
  {0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
  {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},
  {0x3E,0x41,0x41,0x51,0x32},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
  {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
  {0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
  {0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},{0x63,0x14,0x08,0x14,0x63},
  {0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
  {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},
  {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
  {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
  {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
  {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
  {0x00,0x7F,0x10,0x28,0x44},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
  {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
  {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
  {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
  {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
  {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
  {0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08},
};

static void glyph(int x, int y, char ch, uint16_t c, uint8_t sc) {
  if (ch < 32 || ch > 126) ch = '?';
  const uint8_t *g = font5x7[ch - 32];
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t bits = pgm_read_byte(&g[col]);
    for (uint8_t row = 0; row < 7; row++)
      if (bits & (1 << row))
        fillRect(x + col * sc, y + row * sc, sc, sc, c);
  }
}
static int textW(const char *s, uint8_t sc) {
  int n = (int)strlen(s);
  return n > 0 ? n * 6 * sc - sc : 0;
}
static void text(const char *s, int x, int y, uint16_t c, uint8_t sc) {
  while (*s) { glyph(x, y, *s++, c, sc); x += 6 * sc; }
}
static void textC(const char *s, int cy, uint16_t c, uint8_t sc) {   // 水平居中
  text(s, (W - textW(s, sc)) / 2, cy, c, sc);
}

// ---------------- 推屏 ----------------
#define FLUSH_ROWS 8
static uint8_t outBuf[W * 2 * FLUSH_ROWS];

// 脏矩形推屏：fb 存小端 uint16，这里顺手转成大端字节流
static void flushScreen() {
  if (dX1 < dX0) return;
  int x0 = dX0, y0 = dY0, x1 = dX1, y1 = dY1;
  dirtyClear();
  int w = x1 - x0 + 1;

  setWindow(x0, y0, x1, y1);
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  SPI.transfer(0x2C);                 // RAMWR 只发一次，之后连着灌
  digitalWrite(PIN_DC, HIGH);
  for (int y = y0; y <= y1; y += FLUSH_ROWS) {
    int n = imin(FLUSH_ROWS, y1 - y + 1);
    uint8_t *p = outBuf;
    for (int r = 0; r < n; r++) {
      uint16_t *s = fb + (size_t)(y + r) * W + x0;
      for (int i = 0; i < w; i++) { uint16_t c = *s++; *p++ = (uint8_t)(c >> 8); *p++ = (uint8_t)(c & 0xFF); }
    }
    SPI.writeBytes(outBuf, (size_t)n * w * 2);
  }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}

static void fillScreen(uint16_t c) { fillRect(0, 0, W, H, c); }

// ---------------- 页面 ----------------
// 顺序：GIF → 时钟 → 预报 → 太阳月亮 → 空气 → 系统
enum { P_GIF = 0, P_CLOCK, P_FORECAST, P_SUN, P_MUSIC, P_BAT, P_SYS, P_MAX };
static uint8_t page = P_GIF;
#define PAGE_COUNT P_MAX

// ---------------- 天气数据 ----------------
#define NDAYS 3
struct Weather {
  float temp = 0, hum = 0, wind = 0, tmax = 0, tmin = 0;
  int   code = -1;
  bool  ok = false;
  // 未来 3 天预报
  int   nDays = 0;
  float fmax[NDAYS] = {0, 0, 0};
  float fmin[NDAYS] = {0, 0, 0};
  int   fcode[NDAYS] = {-1, -1, -1};
  int   fday[NDAYS] = {0, 0, 0};
  int   fmon[NDAYS] = {0, 0, 0};
  // 日出日落（今天，分钟数；-1 = 没拿到）
  int   sunriseMin = -1;
  int   sunsetMin  = -1;
} wx;

// ★ 空气质量（AQI/PM2.5/PM10）整块移除：2026-11-23 用户要求把这一页换成音乐播放器，
//   连 air-quality-api 的 HTTP 请求一起去掉（每次刷天气少一轮 DNS+TCP+HTTP）。

// 读数组 "key":[a,b,c] 里的数值
static int jarrayF(const String &j, const char *key, int from, float *out, int maxn) {
  char pat[48];
  snprintf(pat, sizeof(pat), "\"%s\":[", key);
  int i = j.indexOf(pat, from);
  if (i < 0) return 0;
  const char *p = j.c_str() + i + strlen(pat);
  int n = 0;
  while (n < maxn) {
    while (*p == ' ' || *p == ',' || *p == '\t') p++;
    if (*p == 0 || *p == ']') break;
    char *end = nullptr;
    float v = strtof(p, &end);
    if (end == p) break;
    out[n++] = v;
    p = end;
  }
  return n;
}

// 读日期数组 "time":["2026-10-18",...] 的月/日
static int jarrayMD(const String &j, const char *key, int from, int *mon, int *day, int maxn) {
  char pat[48];
  snprintf(pat, sizeof(pat), "\"%s\":[", key);
  int i = j.indexOf(pat, from);
  if (i < 0) return 0;
  const char *p = j.c_str() + i + strlen(pat);
  int n = 0;
  while (n < maxn) {
    while (*p && *p != '"' && *p != ']') p++;
    if (*p != '"') break;
    p++;
    int y = 0, m = 0, d = 0;
    if (sscanf(p, "%4d-%2d-%2d", &y, &m, &d) == 3) { mon[n] = m; day[n] = d; n++; }
    while (*p && *p != '"') p++;
    if (*p == '"') p++;
  }
  return n;
}

// 从 from 开始找 "key": 后面的数值（必须真的读到数字，否则算失败）
static bool jnum(const String &j, const char *key, int from, float &out) {
  char pat[48];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  int i = j.indexOf(pat, from);
  if (i < 0) return false;
  i += (int)strlen(pat);
  while (i < (int)j.length() &&
         (j[i] == ' ' || j[i] == '\t' || j[i] == '[' || j[i] == '"')) i++;
  if (i >= (int)j.length()) return false;
  const char *s = j.c_str() + i;
  char *end = nullptr;
  out = strtof(s, &end);
  return end != s;                       // 没读到数字就是撞上了单位串
}

// 读 "key":["2026-10-18T06:24",...] 第一个值里的 HH:MM，存成当天分钟数
static bool jtimeHM(const String &j, const char *key, int from, int &mins) {
  char pat[48];
  snprintf(pat, sizeof(pat), "\"%s\":[", key);
  int i = j.indexOf(pat, from);
  if (i < 0) return false;
  i += (int)strlen(pat);
  int q = j.indexOf('"', i);
  if (q < 0 || q + 17 >= (int)j.length()) return false;
  const char *s = j.c_str() + q + 1;
  if (s[10] != 'T') return false;
  if (s[11] < '0' || s[11] > '9' || s[12] < '0' || s[12] > '9') return false;
  if (s[14] < '0' || s[14] > '9' || s[15] < '0' || s[15] > '9') return false;
  int hh = (s[11] - '0') * 10 + (s[12] - '0');
  int mm = (s[14] - '0') * 10 + (s[15] - '0');
  if (hh > 23 || mm > 59) return false;
  mins = hh * 60 + mm;
  return true;
}

// JSON 花括号闭合 = 响应收完了（省得死等 / 提前断）
static bool jsonBalanced(const String &s) {
  if (s.length() < 20) return false;
  int d = 0;
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '{') d++;
    else if (s[i] == '}') { d--; if (d == 0) return true; }
  }
  return false;
}

// ---- 带超时的 DNS 解析 ----
// WiFi.hostByName 走 getaddrinfo，lwIP 内部重试没有任何可控上限，
// DNS 不回包时能挂几十秒 —— 这就是开机卡死在 FETCHING WEATHER 的根因。
// 这里直接调 dns_gethostbyname（异步 + 回调），自己掐表。
// 解析成功后结果已进 lwIP 缓存，后面 connect(host,...) 立即命中；
// 而且 SNI 仍然是域名 —— 不能改成 IP 直连，Cloudflare 会拒。
static volatile bool sDnsDone = false;
static volatile bool sDnsOk   = false;

static void dnsFound(const char *name, const ip_addr_t *ip, void *arg) {
  (void)name; (void)arg;
  if (ip) sDnsOk = true;
  sDnsDone = true;
}

static bool dnsWarm(const char *host, uint32_t ms) {
  ip_addr_t a;
  sDnsDone = false;
  sDnsOk   = false;
  err_t e = dns_gethostbyname(host, &a, dnsFound, NULL);
  if (e == ERR_OK) return true;             // 缓存命中
  if (e != ERR_INPROGRESS) return false;    // 名字非法之类
  uint32_t t0 = millis();
  while (!sDnsDone && millis() - t0 < ms) {
    if (!uiPump(5)) return false;          // 按键优先：DNS 慢也别让画面等
  }
  return sDnsDone && sDnsOk;
}

// 通用 HTTP GET（HTTP/1.0 + 手工处理 chunked / Content-Length 缺失）
// budgetMs = 整个请求（DNS + TCP + 收包）的总预算，到点必返回，绝不无限期挂死。
//
// ★★ 2026-11-18 实测：为什么走明文而不是 HTTPS ——
//   api.open-meteo.com 的证书是 ECDSA（ECDHE-ECDSA-AES256-GCM-SHA384），
//   ESP32 的 mbedTLS 跟它握手必失败：connect fail 5.2 秒，每次开机白等。
//   （air-quality-api 子域是另一套配置，反而能过，所以只坏一个域名。）
//   但它的 80 端口明文直接返回 200 和完整 JSON —— 天气是公开数据，没有隐私，
//   走 HTTP 既绕开这个坑，又省掉 TLS 的 3~5 秒握手和约 30KB 堆。
//   如果哪天 open-meteo 关了 80，把 WiFiClient 换回 WiFiClientSecure、
//   端口改 443、加 cli.setInsecure() + cli.setHandshakeTimeout(5) 即可。
// 网络等待期间也要让按键活过来。
// ★ fetchWeather + fetchAir 串起来最坏要 16 秒（两个 8 秒预算），全在 loop 里同步跑 ——
//   这 16 秒里按翻页，旧版就是彻底没反应，等它跑完才动，正好是"按一下半天才翻页"。
//   这里每 1ms 喂一次按键；只要有人按了，立刻返回 false 让上层放弃这次请求（数据下次再刷）。
static bool uiPump(uint32_t ms) {
  uint32_t t = millis();
  do {
    handleSerial();      // 串口命令也照进（'U'/'x'/'t' 这类重活由 inNet 挡掉）
    handleButtons();
    if (pendingFlipMs) return false;
    delay(1);
  } while (millis() - t < ms);
  return true;
}

// 出作用域自动清 inNet（httpGet 里多处 return，用析构最省事）
struct NetHold {
  NetHold()  { inNet = true; }
  ~NetHold() { inNet = false; }
};

static bool httpGet(const char *host, const char *path, String &body, uint32_t budgetMs) {
  WiFiClient cli;
  cli.setTimeout(1500);

  uint32_t t0 = millis();
  uint32_t dl = t0 + budgetMs;
  auto left = [&]() -> int32_t { return (int32_t)(dl - millis()); };

  // 1) DNS：最多 1.5 秒
  uint32_t dnsMs = budgetMs / 3;
  if (dnsMs > 1500) dnsMs = 1500;
  if (!dnsWarm(host, dnsMs)) {
    Serial.printf("[http] %s dns fail %lums\n", host, millis() - t0);
    return false;
  }
  Serial.printf("[http] %s dns ok %lums\n", host, millis() - t0);

  // 2) TCP 连接。
  // ★ connect() 是**不可中断**的阻塞调用：里面既收不到按键、也没法 abort。
  //   原来给 3 秒，网络一抖就是整整 3 秒画面不动（实测过 connect fail 3004ms），
  //   正常情况 200ms 就建好了，超时给 1.2 秒已经足够，宁可失败下次再试。
  if (left() <= 0) {
    Serial.printf("[http] %s budget out\n", host);
    return false;
  }
  uint32_t connMs = (left() > 1200) ? 1200 : (uint32_t)left();
  if (!cli.connect(host, 80, (int32_t)connMs)) {
    Serial.printf("[http] %s connect fail %lums\n", host, millis() - t0);
    return false;
  }
  cli.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: esp32-clock/3.0\r\n"
             "Accept-Encoding: identity\r\n\r\n", path, host);
  cli.flush();
  NetHold nh;      // 从这里到函数返回都算"在网络里"

  // 3) 响应头
  bool chunked = false;
  long clen = -1;
  while (left() > 0) {
    // readStringUntil 本身会阻塞到超时，所以每读一行之前先让一次（实测这一段能占近 1 秒）
    if (!uiPump(1)) {
      cli.stop();
      if (!quietLog) Serial.printf("[http] %s abort by key %lums\n", host, millis() - t0);
      return false;
    }
    String l = cli.readStringUntil('\n');
    l.trim();
    if (!l.length()) break;                        // 空行 = 头结束
    if (l.startsWith("Transfer-Encoding") && l.indexOf("chunked") >= 0) chunked = true;
    else if (l.startsWith("Content-Length")) clen = l.substring(15).toInt();
  }

  // 4) 响应体（共用同一个总预算，不再续命 —— 续命就等于没有上限）
  body = "";
  body.reserve(1800);
  if (chunked) {
    while (left() > 0 && body.length() < 6000) {
      String sz = cli.readStringUntil('\n');
      sz.trim();
      if (!sz.length()) { if (!cli.connected()) break; else continue; }
      int sc = sz.indexOf(';');
      if (sc >= 0) sz = sz.substring(0, sc);
      long n = strtol(sz.c_str(), nullptr, 16);
      if (n <= 0) break;
      while (n-- > 0) {
        int ch = cli.read();
        if (ch < 0) break;
        body += (char)ch;
      }
      cli.readStringUntil('\n');
    }
  } else {
    uint32_t lastPump = 0;
    while (left() > 0) {
      // ★ 数据是成串到的，available()>0 的时候根本走不到下面的 else，所以让步必须放循环顶部。
      //   每 5ms 喂一次按键：一次本地请求 0.8~1.3s，落在这段时间里的按键才不会白等。
      if ((uint32_t)(millis() - lastPump) >= 5) {
        lastPump = millis();
        if (!uiPump(1)) {                             // 有按键 → 立刻收手，别让画面等网络
          cli.stop();
          if (!quietLog) Serial.printf("[http] %s abort by key %lums\n", host, millis() - t0);
          return false;
        }
      }
      int av = cli.available();
      if (av > 0) {
        while (av-- > 0) {
          int ch = cli.read();
          if (ch < 0) break;
          body += (char)ch;
        }
        if (jsonBalanced(body)) break;                // 整段收完
        if (clen > 0 && (long)body.length() >= clen) break;
      } else if (!cli.connected()) {
        if (!cli.available()) break;
      } else {
        if (!uiPump(5)) {                             // 有按键 → 立刻收手，别让画面等网络
          cli.stop();
          if (!quietLog) Serial.printf("[http] %s abort by key %lums\n", host, millis() - t0);
          return false;
        }
      }
    }
  }
  cli.stop();

  Serial.printf("[http] %s %lums chunked=%d clen=%ld len=%u\n",
                host, millis() - t0, (int)chunked, clen, (unsigned)body.length());
  if (body.length() < 60) { Serial.printf("[http] short body %u\n", (unsigned)body.length()); return false; }
  return true;
}

static bool fetchWeather() {
  char path[420];
  snprintf(path, sizeof(path),
    "/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
    "&daily=temperature_2m_max,temperature_2m_min,weather_code,sunrise,sunset"
    "&timezone=auto&forecast_days=3",
    LAT, LON);

  String body;
  if (!httpGet("api.open-meteo.com", path, body, 5000)) return false;

  // ★ 必须分开找：current_units 里也有 "temperature_2m":"°C"，先命中就全是 0
  int cur = body.indexOf("\"current\":{");
  if (cur < 0) cur = body.indexOf("\"current\":");
  int dai = body.indexOf("\"daily\":{");
  if (dai < 0) dai = body.indexOf("\"daily\":");
  if (cur < 0 || dai < 0) {
    Serial.print("[wx] no current/daily: ");
    Serial.println(body.substring(0, 200));
    return false;
  }

  Weather w;
  bool ok = true;
  ok &= jnum(body, "temperature_2m", cur, w.temp);
  ok &= jnum(body, "relative_humidity_2m", cur, w.hum);
  float fc = 0;
  ok &= jnum(body, "weather_code", cur, fc);
  w.code = (int)fc;
  ok &= jnum(body, "wind_speed_10m", cur, w.wind);
  ok &= jnum(body, "temperature_2m_max", dai, w.tmax);
  ok &= jnum(body, "temperature_2m_min", dai, w.tmin);
  if (!ok) {
    Serial.print("[wx] parse fail: ");
    Serial.println(body.substring(0, 300));
    return false;
  }
  wx = w;
  wx.ok = true;

  // 3 天预报：拿不到就保持 nDays=0，主界面照常可用
  float fc3[NDAYS] = {0, 0, 0};
  int ncod = jarrayF(body, "weather_code", dai, fc3, NDAYS);
  int nmax = jarrayF(body, "temperature_2m_max", dai, wx.fmax, NDAYS);
  int nmin = jarrayF(body, "temperature_2m_min", dai, wx.fmin, NDAYS);
  wx.nDays = jarrayMD(body, "time", dai, wx.fmon, wx.fday, NDAYS);
  if (wx.nDays > nmax) wx.nDays = nmax;
  if (wx.nDays > nmin) wx.nDays = nmin;
  for (int i = 0; i < NDAYS; i++) wx.fcode[i] = (i < ncod) ? (int)fc3[i] : -1;

  // 日出日落（daily 段内，第一个就是今天）
  if (!jtimeHM(body, "sunrise", dai, wx.sunriseMin)) wx.sunriseMin = -1;
  if (!jtimeHM(body, "sunset", dai, wx.sunsetMin)) wx.sunsetMin = -1;

  Serial.printf("[wx] %.1fC hum %.0f%% wind %.0f code %d  %.1f/%.1f  days=%d  sun %d-%d  heap %u\n",
                wx.temp, wx.hum, wx.wind, wx.code, wx.tmax, wx.tmin, wx.nDays,
                wx.sunriseMin, wx.sunsetMin, (unsigned)ESP.getFreeHeap());
  for (int i = 0; i < wx.nDays; i++)
    Serial.printf("     d%d %02d-%02d  %.0f/%.0f code %d\n",
                  i, wx.fmon[i], wx.fday[i], wx.fmax[i], wx.fmin[i], wx.fcode[i]);
  return true;
}

// ---------------- 天气图标 ----------------
// 0晴 1多云 2阴/雾 3雨 4雪 5雷雨
static int iconType(int code) {
  if (code == 0) return 0;
  if (code <= 2) return 1;
  if (code == 45 || code == 48) return 2;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 4;
  if (code >= 95) return 5;
  return 3;   // 51-67 毛毛雨/雨, 80-82 阵雨
}
static const char *wxText(int t) {
  switch (t) {
    case 0: return "CLEAR";
    case 1: return "CLOUDY";
    case 2: return "FOG";
    case 3: return "RAIN";
    case 4: return "SNOW";
    default: return "STORM";
  }
}
static void drawSun(int cx, int cy) {
  disc(cx, cy, 15, C_YEL);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.785398f;
    line(cx + cosf(a) * 20, cy + sinf(a) * 20, cx + cosf(a) * 27, cy + sinf(a) * 27, C_YEL);
  }
}
static void drawCloud(int cx, int cy, uint16_t col) {
  disc(cx - 13, cy + 3, 10, col);
  disc(cx + 1, cy - 4, 13, col);
  disc(cx + 15, cy + 4, 10, col);
  fillRect(cx - 21, cy + 4, 46, 11, col);
}
static void drawIcon(int type, int cx, int cy) {
  switch (type) {
    case 0:
      drawSun(cx, cy);
      break;
    case 1:
      drawSun(cx - 9, cy - 11);
      drawCloud(cx + 4, cy + 8, C_GRAY);
      break;
    case 2:
      drawCloud(cx, cy - 2, C_GRAY);
      drawCloud(cx + 8, cy + 8, C_GRAY2);
      break;
    case 3:
      drawCloud(cx, cy - 8, C_GRAY);
      for (int i = 0; i < 3; i++) line(cx - 12 + i * 12, cy + 10, cx - 16 + i * 12, cy + 22, C_CYAN);
      break;
    case 4:
      drawCloud(cx, cy - 8, C_WHITE);
      for (int i = 0; i < 3; i++) {
        int bx = cx - 12 + i * 12;
        line(bx - 3, cy + 14, bx + 3, cy + 14, C_CYAN);
        line(bx, cy + 11, bx, cy + 17, C_CYAN);
        line(bx - 2, cy + 12, bx + 2, cy + 16, C_CYAN);
        line(bx + 2, cy + 12, bx - 2, cy + 16, C_CYAN);
      }
      break;
    default:
      drawCloud(cx, cy - 8, C_GRAY);
      line(cx - 2, cy + 8, cx - 8, cy + 19, C_YEL);
      line(cx - 8, cy + 19, cx - 2, cy + 19, C_YEL);
      line(cx - 2, cy + 19, cx - 8, cy + 30, C_YEL);
      line(cx - 14, cy + 14, cx - 17, cy + 24, C_CYAN);
      line(cx + 12, cy + 14, cx + 9, cy + 24, C_CYAN);
      break;
  }
}

// 小号天气图标（预报列表用，半径约 11）
static void drawIconMini(int type, int cx, int cy) {
  switch (type) {
    case 0:
      disc(cx, cy, 7, C_YEL);
      for (int i = 0; i < 4; i++) {
        float a = i * 1.5707963f + 0.7853982f;
        line(cx + cosf(a) * 10, cy + sinf(a) * 10, cx + cosf(a) * 14, cy + sinf(a) * 14, C_YEL);
      }
      break;
    case 1:
      disc(cx - 6, cy + 4, 7, C_GRAY);
      disc(cx + 6, cy + 1, 8, C_GRAY);
      fillRect(cx - 13, cy + 4, 26, 6, C_GRAY);
      break;
    case 2:
      disc(cx - 5, cy + 3, 7, C_GRAY);
      disc(cx + 6, cy + 2, 7, C_GRAY2);
      fillRect(cx - 12, cy + 3, 24, 6, C_GRAY);
      break;
    case 4:
      disc(cx - 5, cy - 2, 7, C_WHITE);
      disc(cx + 6, cy - 4, 8, C_WHITE);
      fillRect(cx - 12, cy - 2, 24, 6, C_WHITE);
      for (int i = 0; i < 3; i++) {
        int bx = cx - 8 + i * 8;
        line(bx - 2, cy + 9, bx + 2, cy + 9, C_CYAN);
        line(bx, cy + 7, bx, cy + 11, C_CYAN);
      }
      break;
    case 5:
      disc(cx - 5, cy - 3, 7, C_GRAY);
      disc(cx + 6, cy - 5, 8, C_GRAY);
      fillRect(cx - 12, cy - 3, 24, 6, C_GRAY);
      line(cx + 1, cy + 6, cx - 3, cy + 13, C_YEL);
      line(cx - 3, cy + 13, cx + 1, cy + 13, C_YEL);
      line(cx + 1, cy + 13, cx - 3, cy + 20, C_YEL);
      break;
    default:   // 雨
      disc(cx - 5, cy - 3, 7, C_GRAY);
      disc(cx + 6, cy - 5, 8, C_GRAY);
      fillRect(cx - 12, cy - 3, 24, 6, C_GRAY);
      for (int i = 0; i < 3; i++) line(cx - 6 + i * 7, cy + 6, cx - 9 + i * 7, cy + 14, C_CYAN);
      break;
  }
}

// ---------------- 界面 ----------------
static const char *WD[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };

static bool nowTM(struct tm *t) {
  return getLocalTime(t, 0) && t->tm_year > 120;
}

static void drawHeader(const struct tm *t) {
  fillRect(0, 6, W, 38, C_BG);          // 清到 y=43，连 OFFLINE 一起擦掉
  text("WUHAN", 14, 10, C_CYAN, 2);
  if (t) {
    char d[20];
    snprintf(d, sizeof(d), "%02d-%02d %s", t->tm_mon + 1, t->tm_mday, WD[t->tm_wday]);
    text(d, W - 14 - textW(d, 2), 10, C_WHITE, 2);
  } else {
    const char *d = "--  NTP";
    text(d, W - 14 - textW(d, 2), 10, C_GRAY, 2);
  }
  hline(14, W - 14, 32, C_LINE);
  if (WiFi.status() != WL_CONNECTED) {
    text("OFFLINE", 14, 36, C_RED, 1);
  }
}

static void drawClock(const struct tm *t) {
  fillRect(0, 44, W, 62, C_BG);              // 44..105
  char hm[8];
  if (t) snprintf(hm, sizeof(hm), "%02d:%02d", t->tm_hour, t->tm_min);
  else   strcpy(hm, "--:--");
  text(hm, (W - textW(hm, 7)) / 2, 48, t ? C_WHITE : C_GRAY2, 7);

  // 秒进度条
  fillRect(20, 100, 200, 6, C_TRACK);
  int sw = t ? (t->tm_sec * 200) / 59 : 0;
  if (sw < 8) sw = 8;
  rrect(20, 100, sw, 6, 3, C_CYAN);
}

static void drawWeatherCard() {
  rrect(10, 112, 220, 124, 14, C_CARD);
  if (!wx.ok) {
    textC("NO WEATHER", 150, C_GRAY, 2);
    textC(WiFi.status() == WL_CONNECTED ? "retrying..." : "no wifi", 180, C_GRAY2, 1);
    return;
  }
  int type = iconType(wx.code);
  drawIcon(type, 54, 160);
  text(wxText(type), 54 - textW(wxText(type), 2) / 2, 202, C_GRAY, 2);

  // 大温度
  char tb[8];
  int ti = (int)lroundf(wx.temp);
  snprintf(tb, sizeof(tb), "%d", ti);
  text(tb, 100, 126, C_WHITE, 6);
  int tw = textW(tb, 6);
  disc(100 + tw + 8, 134, 4, C_WHITE);        // 度数点
  text("C", 100 + tw + 16, 134, C_WHITE, 3);

  char buf[24];
  snprintf(buf, sizeof(buf), "HUM %d%%", (int)lroundf(wx.hum));
  text(buf, 100, 174, C_CYAN, 2);
  snprintf(buf, sizeof(buf), "WIND %dkm/h", (int)lroundf(wx.wind));
  text(buf, 100, 196, C_GREEN, 2);
  snprintf(buf, sizeof(buf), "H%d  L%d", (int)lroundf(wx.tmax), (int)lroundf(wx.tmin));
  text(buf, 100, 218, C_ORG, 1);
}

static void drawPageTag(const char *title) {
  text(title, 14, 10, C_CYAN, 2);
  char b[8];
  snprintf(b, sizeof(b), "%d/%d", page + 1, PAGE_COUNT);
  text(b, W - 14 - textW(b, 2), 10, C_GRAY, 2);
  hline(14, W - 14, 32, C_LINE);
}

// 页 3：未来 3 天
static void drawForecastPage() {
  drawPageTag("FORECAST");
  if (!wx.ok || wx.nDays <= 0) {
    textC("NO FORECAST DATA", 110, C_GRAY, 2);
    textC("long press to retry", 134, C_GRAY2, 1);
    return;
  }
  for (int i = 0; i < wx.nDays && i < NDAYS; i++) {
    int y = 42 + i * 64;
    rrect(10, y, 220, 58, 12, C_CARD);
    char b[16];
    if (i == 0) strcpy(b, "TODAY");
    else snprintf(b, sizeof(b), "%02d-%02d", wx.fmon[i], wx.fday[i]);
    text(b, 22, y + 10, i == 0 ? C_CYAN : C_WHITE, 2);

    int t = iconType(wx.fcode[i]);
    drawIconMini(t, 104, y + 30);

    snprintf(b, sizeof(b), "%d/%d", (int)lroundf(wx.fmax[i]), (int)lroundf(wx.fmin[i]));
    text(b, 142, y + 14, C_ORG, 3);
    text(wxText(t), 142, y + 44, C_GRAY, 1);
  }
}

// ---- 页 4：日出日落 + 太阳位置 + 月相 ----

// 月相 0..1（0/1=新月 0.5=满月）
static float moonPhase() {
  time_t now = time(nullptr);
  if (now < 1600000000) return 0;
  const double syn = 29.530588853;
  double days = ((double)now - 947182440.0) / 86400.0;   // 2000-01-06 18:14 UTC 新月
  double p = fmod(days / syn, 1.0);
  if (p < 0) p += 1.0;
  return (float)p;
}

static const char *moonName(float p) {
  if (p < 0.03f || p > 0.97f) return "NEW MOON";
  if (p < 0.22f) return "WAXING CRESCENT";
  if (p < 0.28f) return "FIRST QUARTER";
  if (p < 0.47f) return "WAXING GIBBOUS";
  if (p < 0.53f) return "FULL MOON";
  if (p < 0.72f) return "WANING GIBBOUS";
  if (p < 0.78f) return "LAST QUARTER";
  return "WANING CRESCENT";
}

// 逐像素画月相：明暗分界是一个椭圆，半宽 = |cos(2πp)|
static void drawMoon(int cx, int cy, int r, float p) {
  float ca = cosf(2.0f * 3.14159265f * p);
  for (int y = -r; y <= r; y++) {
    for (int x = -r; x <= r; x++) {
      if (x * x + y * y > r * r) continue;
      float ny = (float)y / r;
      float span = sqrtf(fmaxf(0.0f, 1.0f - ny * ny));
      float nx = (float)x / r;
      bool lit = (p < 0.5f) ? (nx >= ca * span) : (nx <= -ca * span);
      px(cx + x, cy + y, lit ? C_YEL : 0x18C3);
    }
  }
}

static void drawSunPage() {
  drawPageTag("SUN & MOON");

  char b[16];
  text("SUNRISE", 22, 42, C_GRAY, 1);
  if (wx.sunriseMin >= 0) snprintf(b, sizeof(b), "%02d:%02d", wx.sunriseMin / 60, wx.sunriseMin % 60);
  else strcpy(b, "--:--");
  text(b, 19, 54, C_ORG, 3);

  text("SUNSET", 142, 42, C_GRAY, 1);
  if (wx.sunsetMin >= 0) snprintf(b, sizeof(b), "%02d:%02d", wx.sunsetMin / 60, wx.sunsetMin % 60);
  else strcpy(b, "--:--");
  text(b, 139, 54, C_PURP, 3);

  // 太阳轨迹（半椭圆弧）
  const int cx = 120, hy = 174, rr = 84;
  for (int a = 0; a <= 180; a += 2) {
    float rad = a * 3.14159265f / 180.0f;
    int x0 = cx - (int)(rr * cosf(rad));
    int y0 = hy - (int)(rr * 0.60f * sinf(rad));
    px(x0, y0, C_LINE);
    px(x0, y0 + 1, C_LINE);
  }
  hline(24, 216, hy + 1, C_LINE);

  // 当前太阳位置
  struct tm t;
  if (nowTM(&t) && wx.sunriseMin >= 0 && wx.sunsetMin > wx.sunriseMin) {
    int nowMin = t.tm_hour * 60 + t.tm_min;
    int span = wx.sunsetMin - wx.sunriseMin;
    int el = nowMin - wx.sunriseMin;
    if (el >= 0 && el <= span) {
      float f = (float)el / (float)span;
      float rad = f * 3.14159265f;
      int sx = cx - (int)(rr * cosf(rad));
      int sy = hy - (int)(rr * 0.60f * sinf(rad));
      disc(sx, sy, 6, C_YEL);
      ring(sx, sy, 9, 0x7BE0);
    }
  }

  // 月相
  float p = moonPhase();
  drawMoon(44, 206, 20, p);
  text(moonName(p), 72, 192, C_WHITE, 1);
  snprintf(b, sizeof(b), "AGE %.0fD", p * 29.53f);
  text(b, 72, 206, C_GRAY, 1);
  snprintf(b, sizeof(b), "LIT %.0f%%", (1.0f - cosf(2.0f * 3.14159265f * p)) * 50.0f);
  text(b, 72, 220, C_GRAY2, 1);
}

// 一条横向强度条：v/max 映射到 0..w
static void bar(int x, int y, int w, int h, float v, float vmax, uint16_t c) {
  fillRect(x, y, w, h, C_TRACK);
  float f = v / vmax;
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  int fw = (int)(w * f);
  if (fw < 2) fw = 2;
  rrect(x, y, fw, h, h / 2, c);
}

// ---- 页 5：音乐播放器 ----
//
// 数据来自 player.h：手机浏览器把歌解码成 50kHz 单声道 16bit PCM 推上来，
// 落进 U5 的 4MB 环形区，I2S 任务边取边放。屏幕只负责显示和遥控。
//
// ★ 只有 ASCII 字库：中文歌名显示不出来。手机端会把非 ASCII 的曲名
//   退化成 "TRACK"，艺术家同理 —— 想显示中文得另做点阵字库（见 §34）。

static void fmtMMSS(char *b, size_t n, uint32_t ms) {
  uint32_t sec = ms / 1000;
  if (sec > 359999UL) sec = 359999UL;
  snprintf(b, n, "%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

// 播放三角 / 暂停双竖条，16px 高
static void drawPlayIcon(int x, int y, bool playing, uint16_t c) {
  if (playing) {
    fillRect(x, y, 5, 16, c);
    fillRect(x + 9, y, 5, 16, c);
  } else {
    for (int i = 0; i < 16; i++) {
      int w = (i < 8) ? (i + 1) : (16 - i);   // 中间最宽的三角形
      fillRect(x, y + i, w, 1, c);
    }
  }
}

// 曲名里出现非 ASCII 就退化成 TRACK（点阵字库只到 0x7F）
static bool asciiOnly(const char *s) {
  if (!s || !*s) return false;
  for (const char *p = s; *p; p++) if ((uint8_t)*p >= 0x80) return false;
  return true;
}

static bool     volDirty  = false;      // 音量改过，还没落 NVS
static uint32_t volSaveMs = 0;          // 最后一次改音量的时刻（攒 3 秒再写，省 flash）

// 音量行（音乐页 y=176..193）。单独抽出来是为了**只推这一条脏矩形**：
// 长按调节时整页重绘要多推 ~110KB，播放中看得出来抖一下。
static void drawVolRow() {
  fillRect(14, 176, W - 28, 18, C_BG);
  char b[32];
  snprintf(b, sizeof(b), "VOL %u%%", (unsigned)spkVol);
  text(b, 16, 180, C_GRAY2, 1);
  bar(66, 182, 100, 5, (float)spkVol, 100.0f, spkMute ? C_GRAY : C_GREEN);
  const char *h = "A- B+";                    // 这页长按是调音量，写在屏上省得猜
  text(h, W - 14 - textW(h, 1), 180, C_GRAY2, 1);
}

// 长按调音量：一档 5%，按住不放会连发（BTN_REP_MS）。静音状态下调音量顺手解除静音。
static void musicVol(int dir) {
  int v = (int)spkVol + (dir > 0 ? 5 : -5);
  if (v > 100) v = 100;
  if (v < 0)   v = 0;
  if (v == (int)spkVol && !spkMute) return;   // 已经到头，别刷屏
  spkMute = false;
  spkSetVol(v);
  // 播放中不要塞提示音：spkTask 被 PCM 钩子占着，合成的"嘀"根本出不来，只会白占队列
  if (!mus.stream) spkPlay(SND_KEY);
  if (page == P_MUSIC) { drawVolRow(); flushScreen(); }
  volDirty  = true;                           // 让主循环攒 3 秒后写 NVS
  volSaveMs = millis();
  if (!quietLog) Serial.printf("[vol] %u%%\n", (unsigned)spkVol);
}

// ---------------- 曲名自适应：一行 / 两行 / 跑马灯 ----------------
// ★ 原来是"超宽就截断加 .."：scale2 一行只放 17 个字符，43 字节的曲名后面全扔了。
//   现在三档：① 一行放得下就一行 ② 折两行（两段都能放下）③ 还不行就跑马灯。
//   跑马灯只重画 240×18 那一条（~8.6KB/帧），不碰其它元素。
#define MUS_T_W     (W - 32)      // 可用宽度
#define MUS_T_Y1    65            // 单行 / 跑马灯基线
#define MUS_T_Y2A   62            // 两行：上
#define MUS_T_Y2B   79            // 两行：下
#define MUS_BAND_Y  62
#define MQ_STEP_PX  2             // 每步 2px（= 1 个字体像素）
#define MQ_STEP_MS  40            // 40ms 一步 → 50px/s
#define MQ_REST_MS  1200          // 一轮走完回到起点后停一下

// 带裁剪的文字（跑马灯要用）：超出的列直接丢掉，绝不写到区域外
static void glyphClip(int x, int y, char ch, uint16_t c, uint8_t sc, int cx0, int cx1) {
  if (ch < 32 || ch > 126) ch = '?';
  const uint8_t *g = font5x7[ch - 32];
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t bits = pgm_read_byte(&g[col]);
    if (!bits) continue;
    int px = x + col * sc;
    if (px + sc <= cx0 || px >= cx1) continue;
    int rx = px, rw = sc;
    if (rx < cx0)       { rw -= (cx0 - rx); rx = cx0; }
    if (rx + rw > cx1)  rw = cx1 - rx;
    if (rw <= 0) continue;
    for (uint8_t row = 0; row < 7; row++)
      if (bits & (1 << row)) fillRect(rx, y + row * sc, rw, sc, c);
  }
}
static void textClip(const char *s, int x, int y, uint16_t c, uint8_t sc, int cx0, int cx1) {
  while (*s) { glyphClip(x, y, *s++, c, sc, cx0, cx1); x += 6 * sc; }
}

static char     musName[64];              // 屏上用的名字（已清洗）
static char     musL1[64], musL2[64];
static bool     musTwo     = false;       // 折两行
static bool     musScroll  = false;       // 跑马灯
static bool     musNameIni = false;
static int      musOff     = 0;           // 跑马灯左移像素
static uint32_t musRestMs  = 0;           // 回到起点后停到什么时候

// 折行点：在**所有空格**里挑一个"两段都放得下、且较长那段最短"的位置；
//   没有可用空格时才硬切一半（硬切只在总长 ≤34 字符时做，否则交给跑马灯）。
//   ★ 只从中间往前找第一个空格是不行的：`Moonlight Sonata 1st Movement` 会在
//     "Moonlight" 后面断，右半段 20 字符仍然超宽 → 白白退化成跑马灯。
static bool musSplit(const char *s) {
  int n = (int)strlen(s), best = -1, bestMax = 1 << 20;
  for (int i = 1; i < n - 1; i++) {
    if (s[i] != ' ') continue;
    int e = i;  while (e > 0 && s[e - 1] == ' ') e--;     // 左段去掉尾部空格
    int st = i; while (st < n && s[st] == ' ') st++;      // 右段去掉头部空格
    int n1 = e, n2 = n - st;
    if (n1 <= 0 || n2 <= 0) continue;
    int w1 = n1 * 12 - 2, w2 = n2 * 12 - 2;               // scale2 宽度
    if (w1 > MUS_T_W || w2 > MUS_T_W) continue;
    int m = w1 > w2 ? w1 : w2;
    if (m < bestMax) { bestMax = m; best = i; }
  }
  if (best < 0) {
    if (n > 34) return false;                            // 两行也塞不下
    best = n / 2;
  }
  int e = best;  while (e > 0 && s[e - 1] == ' ') e--;
  int st = best; while (st < n && s[st] == ' ') st++;
  if (e > 62) e = 62;
  memcpy(musL1, s, e); musL1[e] = 0;
  snprintf(musL2, sizeof(musL2), "%s", s + st);
  return musL1[0] && musL2[0];
}

static void musTitlePrep(const char *raw) {
  const char *src = (raw && raw[0]) ? (asciiOnly(raw) ? raw : "TRACK") : "--";
  if (musNameIni && !strcmp(src, musName)) return;    // 名字没变：保留滚动位置
  musNameIni = true;
  snprintf(musName, sizeof(musName), "%s", src);
  musOff = 0; musRestMs = millis() + MQ_REST_MS;
  musTwo = false; musScroll = false;

  if (textW(musName, 2) <= MUS_T_W) return;           // ① 一行够
  if (musSplit(musName)) { musTwo = true; return; }   // ② 两行
  musScroll = true;                                   // ③ 跑马灯
}

static void drawTitleBand() {
  fillRect(0, MUS_BAND_Y, W, musTwo ? 30 : 18, C_BG); // 62..91 / 62..79
  if (musScroll)      textClip(musName, 16 - musOff, MUS_T_Y1, C_WHITE, 2, 0, W);
  else if (musTwo)  { text(musL1, 16, MUS_T_Y2A, C_WHITE, 2);
                      text(musL2, 16, MUS_T_Y2B, C_WHITE, 2); }
  else                text(musName, 16, MUS_T_Y1, C_WHITE, 2);
}

// 由 loop 按 MQ_STEP_MS 调用：挪一步 + 只推那一条
static void musicMarquee() {
  if (millis() < musRestMs) return;
  musOff += MQ_STEP_PX;
  if (16 - musOff + textW(musName, 2) <= 0) {         // 整串走出左边了
    musOff = 0;
    musRestMs = millis() + MQ_REST_MS;
  }
  drawTitleBand();
  flushScreen();
}

static void drawMusicPage() {
  drawPageTag("MUSIC");

  bool live = mus.stream;
  bool play = live && !mus.paused && !mus.eos && mus.started;
  uint16_t stc = !live ? (mus.eos ? C_GRAY : C_GRAY) : (play ? C_GREEN : C_YEL);
  const char *st = !live ? (mus.eos ? "END" : "READY")
                 : mus.eos ? "END"
                 : mus.paused ? "PAUSED"
                 : mus.started ? "PLAYING" : "BUFFER";

  drawPlayIcon(16, 40, play, stc);
  text(st, 42, 42, stc, 1);

  char b[48];
  snprintf(b, sizeof(b), "%luk", (unsigned long)(rnAvail() / 1024));
  text(b, W - 14 - textW(b, 1), 42, C_GRAY2, 1);

  // 曲名：自适应排法（一行 / 两行 / 跑马灯），逻辑见 musTitlePrep
  musTitlePrep(mus.title);
  drawTitleBand();
  if (mus.artist[0] && asciiOnly(mus.artist)) {
    char ab[40];
    snprintf(ab, sizeof(ab), "%s", mus.artist);            // scale1 一行放 34 字符
    while (strlen(ab) > 3 && textW(ab, 1) > MUS_T_W) ab[strlen(ab) - 1] = 0;
    text(ab, 16, musTwo ? 95 : 88, C_GRAY, 1);
  }

  hline(14, W - 14, 104, C_LINE);

  // 缓冲水位条：绿=健康（>2 秒） 黄=偏低  红=快断
  uint32_t av = rnAvail();
  uint32_t need = mus.sr * 2 * 2;                 // 2 秒音频的字节数
  uint16_t bc = av > need ? C_GREEN : (av > need / 3 ? C_YEL : C_RED);
  text("BUFFER", 16, 112, C_GRAY2, 1);
  bar(70, 114, 154, 6, (float)av, (float)(mus.sr * 2 * 8), bc);   // 满格 = 8 秒

  // 已播 / 总长
  char t1[8], t2[8];
  fmtMMSS(t1, sizeof(t1), mus.sr ? (uint32_t)((uint64_t)mus.rd * 1000ULL / (mus.sr * 2ULL)) : 0);
  if (mus.total) fmtMMSS(t2, sizeof(t2), (uint32_t)((uint64_t)mus.total * 1000ULL / (mus.sr * 2ULL)));
  else           snprintf(t2, sizeof(t2), "--:--");
  text(t1, 16, 132, C_WHITE, 2);
  text(t2, W - 14 - textW(t2, 2), 132, C_GRAY, 2);

  float f = mus.total ? (float)mus.rd / (float)mus.total : 0.0f;
  if (f > 1.0f) f = 1.0f;
  bar(16, 160, W - 32, 8, f, 1.0f, C_CYAN);

  // 音量（长按 A/B 可调，见 musicVol）
  drawVolRow();

  // 手机该打开的地址（这是全屏最有用的信息）
  hline(14, W - 14, 198, C_LINE);
  snprintf(b, sizeof(b), "http://%s", wifiUp() ? WiFi.localIP().toString().c_str() : "no wifi");
  text(b, 16, 204, wifiUp() ? C_CYAN : C_GRAY, 1);
  text("open it on your phone", 16, 216, C_GRAY2, 1);
}

// ---------------- 电池 / 充电 ----------------
// ADC_BAT = U1.5(IO1) ← R6/R7 各 100k 分压（÷2）
// CHRG    = U1.6(IO2) ← U3.7 开漏输出，充电中拉低
#define PIN_ADC_BAT 1
#define PIN_CHRG    2

#define BAT_MAH   600.0f    // 602535 实测 600mAh
#define CHG_MA    255.0f    // R5 = 4.7k → Ibat ≈ 1200/4.7
#define LOAD_MA   210.0f    // 整机平均放电电流（屏+背光+WiFi，估）
#define VBAT_K    1.022f    // 校准：万用表 4.21V vs 读数 4.12V（ADC 高阻源偏低约 2%）

enum { BAT_DSG = 0, BAT_CHG, BAT_FULL };

struct BatInfo {
  float v;        // 电池电压 V
  int   pct;      // 0~100
  int   state;
  int   mins;     // 距充满 / 距用尽，分钟
  bool  chrgLow;
  bool  valid;
};
static BatInfo bat = { 0, 0, BAT_DSG, -1, false, false };

// ★ 实测 U3.7(CHRG) 到 IO2 之间不通：充电中读到的也是 HIGH。
//   兜底：用 10 分钟窗口的电压趋势判断在充还是在放（充电必升、放电必降）。
static float    vTrendRef = 0;
static uint32_t vTrendT   = 0;
static int8_t   vDir      = 0;      // 1 上升（在充）/ -1 下降（在放）/ 0 不明显
static uint8_t  vTrendN   = 0;      // 已完成的窗口数

static void batTrend() {
  if (vTrendT == 0) { vTrendRef = bat.v; vTrendT = millis(); return; }
  // 开机头 3 个窗口用 2 分钟（快速定性），之后放宽到 10 分钟（抗噪）
  uint32_t win = (vTrendN < 3) ? 120000UL : 600000UL;
  float    thr = (vTrendN < 3) ? 0.008f  : 0.005f;
  if (millis() - vTrendT >= win) {
    float d = bat.v - vTrendRef;
    vDir      = (d > thr) ? 1 : (d < -thr ? -1 : 0);
    vTrendRef = bat.v;
    vTrendT   = millis();
    if (vTrendN < 3) vTrendN++;
  }
}

// 单体锂电静置放电曲线：3.30V=0% … 4.20V=100%，中间线性插值
static const float   SOC_V[12] = { 3.30f, 3.40f, 3.50f, 3.60f, 3.65f, 3.70f,
                                   3.75f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f };
static const uint8_t SOC_P[12] = { 0, 3, 9, 18, 28, 38, 48, 58, 72, 82, 92, 100 };

static int vbatPct(float v) {
  if (v <= SOC_V[0]) return 0;
  for (int i = 1; i < 12; i++) {
    if (v <= SOC_V[i]) {
      float f = (v - SOC_V[i - 1]) / (SOC_V[i] - SOC_V[i - 1]);
      return (int)lroundf(SOC_P[i - 1] + f * (SOC_P[i] - SOC_P[i - 1]));
    }
  }
  return 100;
}

// 32 次采样排序后取中间 16 个平均（去极值）。
// ★ 100k/100k 分压源阻抗 50k，WiFi 一发射 ADC 就抖 ±50mV —— 不滤的话电量会乱跳。
static float vbatMilliVolts() {
  // ★ 原来是 32 次 × 250µs = 8ms 纯等待，加上 ADC 约 8.5ms，正好在 40ms 的 GIF 帧里卡出
  //   一次"顿"（gapmax 50ms 就是它）。降到 10 次 × 250µs ≈ 2.5ms，配一阶低通 + 状态消抖已经够稳。
  uint16_t sv[10];
  for (int i = 0; i < 10; i++) {
    sv[i] = (uint16_t)analogReadMilliVolts(PIN_ADC_BAT);
    delayMicroseconds(250);      // 拉开采样时刻：连读会全部落在同一个 WiFi 相位上
  }
  for (int i = 1; i < 10; i++) {                 // 插入排序，10 个元素几乎不耗时
    uint16_t k = sv[i];
    int j = i - 1;
    while (j >= 0 && sv[j] > k) { sv[j + 1] = sv[j]; j--; }
    sv[j + 1] = k;
  }
  uint32_t acc = 0;
  for (int i = 2; i < 8; i++) acc += sv[i];      // 去掉最大最小各 2 个
  return acc / 6.0f;
}

static float vbatRead() {
  return vbatMilliVolts() * 2.0f * VBAT_K / 1000.0f;   // ×2 还原分压比
}

// 距充满：剩余容量 ÷ 有效充电电流。进 CV 段（>80%）电流衰减，另加尾巴
static int batTimeToFull(int pct) {
  if (pct >= 100) return 0;
  float need = (100 - pct) / 100.0f * BAT_MAH;
  float ia   = CHG_MA * (pct >= 80 ? 0.45f : 0.90f);
  return (int)lroundf(need / ia * 60.0f) + (pct >= 80 ? 8 : 0);
}

// 距用尽：剩余容量 ÷ 平均放电电流
static int batTimeToEmpty(int pct) {
  if (pct <= 0) return 0;
  return (int)lroundf(pct / 100.0f * BAT_MAH / LOAD_MA * 60.0f);
}

static void batSample() {
  static float vf = 0;
  // ★ 消抖：U3 的 CHRG 到 IO2 是虚焊（已知硬件问题），引脚浮空 → digitalRead 随机跳。
  //   不消抖的话 bat.state 每 5 秒在 CHG / FULL / DSG 之间乱翻，每次翻都要 printf，
  //   一帧被顶晚 10ms —— 这就是"隔一会卡一下"的来源。
  static bool    chrgLast = false;
  static uint8_t chrgSame = 0;
  static uint8_t stTry    = 0, stSame = 0;

  float raw = vbatRead();
  vf = (vf < 1.0f) ? raw : vf + (raw - vf) * 0.25f;    // 一阶低通，约 4 次采样收敛
  float v  = vf;

  bool loRaw = (digitalRead(PIN_CHRG) == LOW);
  if (loRaw == chrgLast) { if (chrgSame < 8) chrgSame++; }
  else { chrgLast = loRaw; chrgSame = 0; }
  bool lo = (chrgSame >= 2) ? chrgLast : bat.chrgLow;   // 只见过 1 次就当噪声，沿用上次

  bat.v       = v;
  bat.chrgLow = lo;
  bat.valid   = (v > 2.50f);
  bat.pct     = bat.valid ? vbatPct(v) : 0;
  batTrend();

  // ★ 状态必须带迟滞。电池现在就在 4.13~4.16 之间，用单一阈值 4.14 会每几分钟翻一次，
  //   翻一次就 printf + 重画，就是"隔一会卡一下"的另一半来源。
  uint8_t st;
  if (lo) {
    st = BAT_CHG;                                     // CHRG 拉低（消抖后）= 真在充
  } else if (bat.state == BAT_FULL) {
    st = (v < 4.06f) ? BAT_DSG : BAT_FULL;            // 满了要掉到 4.06 以下才算开始放电
  } else if (bat.state == BAT_CHG) {
    st = (vDir > 0) ? BAT_CHG : (v > 4.15f ? BAT_FULL : BAT_DSG);
  } else {                                            // DSG
    if (v > 4.15f && vDir >= 0) st = BAT_FULL;
    else if (vDir > 0)          st = BAT_CHG;
    else                        st = BAT_DSG;
  }
  if (st == BAT_CHG)      bat.mins = batTimeToFull(bat.pct);
  else if (st == BAT_FULL) bat.mins = 0;
  else                     bat.mins = batTimeToEmpty(bat.pct);

  // 状态也要连续 3 次一致才认，避免 4.13/4.16 在阈值上反复横跳
  if (st == bat.state || st == stTry) { if (stSame < 8) stSame++; }
  else { stTry = st; stSame = 0; }
  if (stSame >= 2 && st != bat.state) { bat.state = st; stSame = 0; }
}

// 页 6：电池 / 充电
static void drawBatteryIcon(int x, int y, int w, int h, int pct, int state, bool pulse) {
  uint16_t col = (pct <= 15) ? C_RED : (pct <= 35) ? C_ORG : C_GREEN;
  if (state == BAT_CHG) col = C_CYAN;
  rrect(x, y, w, h, 8, C_GRAY2);                    // 外壳
  fillRect(x + 3, y + 3, w - 6, h - 6, C_BG);
  rrect(x + w, y + h / 3, 7, h / 3, 3, C_GRAY2);    // 正极凸起
  int iw = (w - 6) * pct / 100;
  if (iw < 0) iw = 0;
  if (iw > 0) rrect(x + 3, y + 3, iw, h - 6, 4, col);
  if (state == BAT_CHG && pulse && iw > 16) {       // 充电：右端白色脉冲
    fillRect(x + 3 + iw - 8, y + 3, 8, h - 6, C_WHITE);
  }
}

static void drawBatPage() {
  drawPageTag("BATTERY");
  char b[40];

  drawBatteryIcon(36, 46, 160, 54, bat.pct, bat.state, ((millis() / 1000) & 1));

  if (bat.valid) snprintf(b, sizeof(b), "%d", bat.pct);
  else           snprintf(b, sizeof(b), "--");
  int pw = textW(b, 5);
  text(b, (W - pw) / 2 - 8, 106, bat.valid ? C_WHITE : C_GRAY, 5);
  if (bat.valid) text("%", (W + pw) / 2 - 4, 118, C_GRAY, 2);

  const char *st = "NO BATTERY";
  uint16_t stc   = C_RED;
  if (bat.valid) {
    if (bat.state == BAT_CHG)       { st = "CHARGING";    stc = C_CYAN; }
    else if (bat.state == BAT_FULL) { st = "FULL";        stc = C_GREEN; }
    else if (bat.pct <= 15)         { st = "LOW BATTERY"; stc = C_RED; }
    else                            { st = "DISCHARGING"; stc = C_ORG; }
  }
  textC(st, 148, stc, 2);

  snprintf(b, sizeof(b), "%.2fV", bat.valid ? bat.v : 0.0f);
  textC(b, 168, C_GRAY, 1);

  uint16_t tc = C_GRAY;
  if (!bat.valid)                       snprintf(b, sizeof(b), "--:--");
  else if (bat.state == BAT_CHG) {
    if (bat.pct >= 99)                  snprintf(b, sizeof(b), "FULL SOON");
    else                                snprintf(b, sizeof(b), "FULL IN %d:%02d", bat.mins / 60, bat.mins % 60);
  } else if (bat.state == BAT_FULL)     snprintf(b, sizeof(b), "ALL CHARGED");
  else                                  snprintf(b, sizeof(b), "LEFT %d:%02d", bat.mins / 60, bat.mins % 60);
  if (bat.state == BAT_DSG) tc = C_WHITE;
  else if (bat.state == BAT_FULL) tc = C_GREEN;
  else tc = C_YEL;
  textC(b, 182, tc, 2);

  snprintf(b, sizeof(b), "%dmAh  CHG %dmA  LOAD %dmA", (int)BAT_MAH, (int)CHG_MA, (int)LOAD_MA);
  textC(b, 210, C_GRAY2, 1);
  textC("B:next   long:home", 224, C_GRAY2, 1);
}

// 页 7：运行状态
static void drawSysPage() {
  drawPageTag("SYSTEM");
  char b[40];
  int y = 44;
  bool up = (WiFi.status() == WL_CONNECTED);
  snprintf(b, sizeof(b), "WIFI  %d dBm", up ? (int)WiFi.RSSI() : 0);
  text(b, 14, y, up ? C_GREEN : C_RED, 2); y += 22;
  snprintf(b, sizeof(b), "IP    %s", up ? WiFi.localIP().toString().c_str() : "offline");
  text(b, 14, y, C_WHITE, 1); y += 20;
  snprintf(b, sizeof(b), "HEAP  %uK  PSRAM %uK", (unsigned)(ESP.getFreeHeap() / 1024),
           (unsigned)(ESP.getFreePsram() / 1024));
  text(b, 14, y, C_WHITE, 1); y += 20;
  snprintf(b, sizeof(b), "CHIP  %d C   SPI %uMHz", (int)temperatureRead(),
           (unsigned)(spiHzNow / 1000000));
  text(b, 14, y, C_WHITE, 1); y += 20;
  uint32_t s = millis() / 1000;
  snprintf(b, sizeof(b), "UP    %02u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
  text(b, 14, y, C_WHITE, 2); y += 22;
  snprintf(b, sizeof(b), "WX %s  GIF %dF  MUS %s", wx.ok ? "ok" : "--", gif.ok ? gif.frames : 0,
           mus.stream ? (mus.paused ? "PAUSE" : "PLAY") : "IDLE");
  text(b, 14, y, C_CYAN, 1);
  y += 16;
  // 翻页时延：上一次 / 历史最差，单位 ms。>80ms 就是"按一下要等一下"了。
  snprintf(b, sizeof(b), "FLIP  %lums  max %lums", (unsigned long)flipLastMs,
           (unsigned long)flipMaxMs);
  text(b, 14, y, flipMaxMs > 80 ? C_ORG : C_GREEN, 1);
  y += 16;
  if (bat.valid) {
    snprintf(b, sizeof(b), "BAT   %d%%  %.2fV  %s", bat.pct, bat.v,
             bat.state == BAT_CHG ? "CHG" : bat.state == BAT_FULL ? "FULL" : "DSG");
  } else {
    snprintf(b, sizeof(b), "BAT   --  adc no signal");
  }
  text(b, 14, y, bat.state == BAT_DSG ? C_ORG : C_GREEN, 1);

  hline(14, W - 14, 196, C_LINE);
  text("A:PREV  B:NEXT", 14, 204, C_GRAY2, 1);
  text("long press = HOME", 14, 218, C_GRAY2, 1);
}

// ---------------- GIF 播放 ----------------
static uint16_t gifFrame = 0;
static uint32_t gifNextUs = 0;        // 下一帧的时刻（micros）
static uint32_t gifFpsT0 = 0;
static uint16_t gifFpsCnt = 0;
static uint32_t gifLastUs = 0;        // 一帧总耗时
static uint32_t gifDecUs = 0;         // 其中：U5 读 + RLE 解压
static uint32_t gifPushUs = 0;        // 其中：SPI 推屏
static uint32_t gifSkipCnt = 0;       // 渲染追不上目标帧率、被迫丢掉的延时次数
static uint32_t gifGapPrevUs = 0;     // 上一帧的起点
static uint16_t gifLateCnt = 0;       // 间隔超过目标 +5ms 的帧数（肉眼可见的顿）
static uint32_t gifGapMaxUs = 0;      // 本统计窗口内最大帧间隔（查"跳动"用）

// ★ 帧率上限。GIF 文件自带的 delay 通常是 60~100ms（10~16fps），
//   不改它，SPI 提多快都白搭 —— 因为大部分时间在干等。
//   0 = 完全照 GIF 原始延时；其它值 = 每帧最多 1000/cap 毫秒（原本更短的照旧）。
//   串口 'f' 循环切换：原始 / 24 / 30 / 45 / 60。
static const uint8_t kFpsMenu[] = { 0, 24, 30, 45, 60 };
static const uint8_t kFpsMenuN  = sizeof(kFpsMenu) / sizeof(kFpsMenu[0]);
static uint8_t gifFpsIdx = 0;         // 默认 0 = 按 GIF 原始延时（这份 40ms/帧 = 25fps，原速不变调）

static inline uint8_t gifCap() { return kFpsMenu[gifFpsIdx]; }

// 这一帧该显示多久（ms）
static uint32_t gifFrameMs(uint16_t f) {
  uint32_t d = gif.delayMs[f];
  uint8_t  cap = gifCap();
  if (cap) {
    uint32_t lim = 1000UL / cap;
    if (d > lim) d = lim;
  }
  // ★ 手机在推流时：U5 每 0.65 秒就要擦一块 64KB，那 150ms 是**持锁不可打断**的。
  //   GIF 还照 40ms 去抢，结果就是连续等锁再猛跳（实测 22.5fps / gapmax 311ms）。
  //   主动降到 8fps：擦除停顿正好落在两帧之间，观感是"慢一点"而不是"卡一下"。
  if (mus.stream && d < 125) d = 125;
  if (d < 1) d = 1;
  return d;
}

static void gifReset() {
  gifFrame = 0;
  gifNextUs = micros();
  dirtyClear();
  gifFpsT0 = millis();
  gifFpsCnt = 0;
  gifSkipCnt = 0;
}

// ---------------- 专辑：切换时右上角显示 "2/5" ----------------
// 只显示 1.8 秒。★ 必须每帧重画：GIF 是整屏推的，上一帧的像素会被下一帧覆盖掉。
static uint32_t gifHudMs = 0;
static inline bool gifHudOn() { return gifHudMs && millis() - gifHudMs < 1800; }

// ★ GIF 页的 fb 是"字节预交换"格式（drawGifFrame 为了整块 sendBytes 直接这么存的），
//   在这儿画东西必须同样交换，否则屏上是粉红底 + 蓝字。
static inline uint16_t gifSw(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static void gifHudDraw() {
  if (!gifHudOn() || albumN < 2) return;
  char b[16];
  snprintf(b, sizeof(b), "%u/%u", (unsigned)(albumIdx + 1), (unsigned)albumN);
  int w = textW(b, 1) + 10;
  int x = W - w - 4;
  fillRect(x, 4, w, 15, gifSw(C_TRACK));   // 深底，压在任意画面上都看得清
  text(b, x + 5, 8, gifSw(C_WHITE), 1);
}

// 16 行 = 7680 字节一批：writeBytes 调用次数少一点，也不会卡住其它任务太久
#define GIF_ROWS 16

static void drawGifFrame(uint16_t f) {
  uint32_t t0 = micros();

  // ---- 1) U5 读整帧 RLE（HSPI 40MHz，约 9ms）→ 解压到 fb ----
  // 解压内层改成 16 位整字写（bswap 成大端），比原来逐字节 *p++ 两次快约一倍。
  uint32_t flen = gifLoadFrame(f);
  const uint8_t *q  = gif.buf;
  const uint8_t *qe = gif.buf + flen;
  uint16_t *p    = fb;
  uint16_t *pend = fb + (size_t)W * H;

  // ★ 按 RLE 段整段填，段内不再逐像素判断 remain —— 内层退化成纯 store 循环，
  //   115200 像素从 3.9ms 降到 1.2ms 左右。
  while (p < pend) {
    uint32_t cnt, col;
    if (q + 4 > qe) {                       // 数据提前结束：余下补黑，绝不越界
      cnt = (uint32_t)(pend - p);
      col = 0;
    } else {
      cnt = (uint32_t)(q[0] | (q[1] << 8)); // blob 是小端
      col = (uint16_t)(q[2] | (q[3] << 8));
      q += 4;
      if (cnt == 0) continue;               // 防御：0 长度会死循环
    }
    if (cnt > (uint32_t)(pend - p)) cnt = (uint32_t)(pend - p);
    uint16_t be = (uint16_t)((col >> 8) | (col << 8));   // fb 按大端字节序存
    uint16_t *pe = p + cnt;
    while (p < pe) *p++ = be;
  }
  uint32_t t1 = micros();
  gifHudDraw();                     // 序号覆盖层（切换后 1.8 秒内），要赶在推屏之前

  // ---- 2) 整屏推（RAMWR 只发一次，CS 全程拉低）----
  setWindow(0, 0, W - 1, H - 1);
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  SPI.transfer(0x2C);
  digitalWrite(PIN_DC, HIGH);

  const uint8_t *raw = (const uint8_t *)fb;
  SPI.writeBytes(raw, (size_t)W * H * 2);      // 整帧一次灌：hal 内部按 64B FIFO 连续搬运，比分段调用少 15 次开销

  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);

  uint32_t t2 = micros();
  gifDecUs  = t1 - t0;
  gifPushUs = t2 - t1;
  gifLastUs = t2 - t0;
}

// ============ GIF 块级流水线（双核） ============
// 串行做的时候「U5读+解压 8ms」+「SPI 推屏 15ms」= 23ms/帧，卡在 43fps；
// 45fps 要 22.2ms，差一点点，60fps 更是够不到。
// 拆成生产者/消费者：core0 解一块（16 行），core1 立刻把这块推出去，两者重叠后
// 帧时间 = 慢的那一边（推屏 15ms），上限 66fps。
// 缓冲只要 4 块 × 7680B = 30KB 内部 RAM，不需要第二个整帧缓冲。
#define GP_ROWS   16
#define GP_BLKS   4                          // 必须是 2 的幂（用 & 取模）
// 试过 8 块（想让被复用的缓冲离得更远，避开 writeBytes 返回后的 FIFO 残留）：
// 60KB 内部 RAM 直接分配失败 → 退回串行。4 块 30KB 是这块板能给的上限。
#define GP_NB     (H / GP_ROWS)              // 每帧 15 块
#define GP_BYTES  (GP_ROWS * W * 2)          // 7680

static uint16_t         *gpBuf[GP_BLKS] = { nullptr };
static SemaphoreHandle_t gpFree = nullptr, gpFull = nullptr;
static SemaphoreHandle_t gpIdle = nullptr;   // 生产者"我已收工"回执（gpEnd 靠它握手，不再猜时间）
static TaskHandle_t      gpTask = nullptr;
static bool              gpOk   = false;
static volatile bool     gpLive = true;
static volatile uint16_t gpWant = 0;
static uint32_t          gpProdUs = 0;       // 生产者解一整帧的耗时

// 生产者：core0。等主循环喊一声，就把这一整帧读回来、按块解压、交出去。
static void gpProducer(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!gpLive || !gif.ok) {
      xSemaphoreGive(gpIdle);                       // 被喊停：回执一下，gpEnd 才好确定能抹账
      continue;
    }

    uint32_t t0   = micros();
    uint32_t flen = gifLoadFrame(gpWant);    // U5 读整帧 RLE（80MHz Fast Read，约 4ms）
    const uint8_t *q  = gif.buf;
    const uint8_t *qe = gif.buf + flen;

    for (int k = 0; k < GP_NB; k++) {
      if (!gpLive) break;
      if (xSemaphoreTake(gpFree, pdMS_TO_TICKS(300)) != pdTRUE) break;
      if (!gpLive) { xSemaphoreGive(gpFree); break; }   // 拿到空位后才发现要收工，还回去

      uint16_t *p    = gpBuf[k & (GP_BLKS - 1)];
      uint16_t *pend = p + GP_ROWS * W;
      while (p < pend) {
        uint32_t cnt, col;
        if (q + 4 > qe) {                    // 数据提前结束：余下补黑，绝不越界
          cnt = (uint32_t)(pend - p);
          col = 0;
        } else {
          cnt = (uint32_t)(q[0] | (q[1] << 8));
          col = (uint16_t)(q[2] | (q[3] << 8));
          q += 4;
          if (cnt == 0) continue;            // 防御：0 长度会死循环
        }
        if (cnt > (uint32_t)(pend - p)) cnt = (uint32_t)(pend - p);
        uint16_t be = (uint16_t)((col >> 8) | (col << 8));   // 大端字节序
        uint16_t *pe = p + cnt;
        while (p < pe) *p++ = be;
      }
      xSemaphoreGive(gpFull);
    }
    gpProdUs = micros() - t0;
    if (!gpLive) xSemaphoreGive(gpIdle);     // 中途被喊停，同样回执
  }
}

static bool gpBegin() {
  if (gpOk) return true;
  size_t need = (size_t)GP_BLKS * GP_BYTES;
  uint8_t *m = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!m) m = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!m) { Serial.println("[gif] 流水线缓冲分配失败，退回单核串行"); return false; }
  for (int i = 0; i < GP_BLKS; i++) gpBuf[i] = (uint16_t *)(m + (size_t)i * GP_BYTES);

  gpFree = xSemaphoreCreateCounting(GP_BLKS, GP_BLKS);
  gpFull = xSemaphoreCreateCounting(GP_BLKS, 0);
  gpIdle = xSemaphoreCreateBinary();
  if (!gpFree || !gpFull || !gpIdle) { Serial.println("[gif] 信号量创建失败，退回串行"); return false; }

  if (xTaskCreatePinnedToCore(gpProducer, "gifdec", 4096, nullptr, 2, &gpTask, 0) != pdPASS) {
    Serial.println("[gif] 解码任务创建失败，退回串行");
    return false;
  }
  gpOk = true;
  Serial.printf("[gif] 双核流水线就绪：%u 块 x %u B = %u KB\n",
                (unsigned)GP_BLKS, (unsigned)GP_BYTES, (unsigned)(need / 1024));
  return true;
}

// 上一帧没收干净（超时/切页）时把信号量账面抹平，否则下一帧会错块。
// ★ 旧版「把 gpFree 取空、再补回 GP_BLKS-c 个」在空闲时 c 就等于 GP_BLKS → 补 0 个 →
//   账面直接被清成 0，下一次又得再抹一次才恢复；40/45fps 下表现为翻几次页就多等 300ms。
//   这里直接补到 GP_BLKS，一步到位。
static void gpSettle() {
  while (xSemaphoreTake(gpFull, 0) == pdTRUE) {}
  while (uxSemaphoreGetCount(gpFree) < GP_BLKS) {
    if (xSemaphoreGive(gpFree) != pdTRUE) break;
  }
}

// 消费者：主循环所在 core1。整帧一个 RAMWR，然后逐块灌
static void gpPushFrame(uint16_t f) {
  if (uxSemaphoreGetCount(gpFree) != GP_BLKS) gpSettle();

  gpWant = f;
  xTaskNotifyGive(gpTask);

  setWindow(0, 0, W - 1, H - 1);
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  SPI.transfer(0x2C);
  digitalWrite(PIN_DC, HIGH);

  uint32_t t0 = micros();
  int got = 0;
  for (int k = 0; k < GP_NB; k++) {
    // 正常一帧 33ms，等一块最多几毫秒。超时给 100ms：真出问题宁可少画一块（当场抹账重来），
    // 也别让一次翻页卡上 300ms —— 等待越久，用户按的那一下就越是"没反应"。
    if (xSemaphoreTake(gpFull, pdMS_TO_TICKS(100)) != pdTRUE) break;
    SPI.writeBytes((const uint8_t *)gpBuf[k & (GP_BLKS - 1)], GP_BYTES);
    xSemaphoreGive(gpFree);
    got++;
  }
  uint32_t t1 = micros();

  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);

  gifDecUs  = gpProdUs;
  gifPushUs = t1 - t0;
  gifLastUs = t1 - t0;
  if (got < GP_NB) {
    if (!quietLog) Serial.printf("[gif] push short %d/%d\n", got, (int)GP_NB);
    gpSettle();
  }
}

static void gpEnd() {
  if (!gpOk) return;
  while (xSemaphoreTake(gpIdle, 0) == pdTRUE) {}   // 先清掉历史回执，免得下面"秒过"
  gpLive = false;                 // 让生产者收起当前块循环
  xTaskNotifyGive(gpTask);        // 万一它正阻塞在通知上，叫醒它好让它看见 gpLive=false
  // ★ 等生产者**确认**收工，不再猜时间（旧版 delay(30)：生产者慢一拍返回，会把块
  //   塞进已经抹平的账里 → 计数永久错乱 → 之后每次翻页都要等满 300ms 超时 = "按一下半天"）。
  if (xSemaphoreTake(gpIdle, pdMS_TO_TICKS(60)) != pdTRUE) {
    if (!quietLog) Serial.println("[gif] gpEnd 等生产者超时（账已抹平，继续）");
  }
  gpSettle();
  xTaskNotifyStateClear(gpTask);  // 丢掉残留通知，否则下次推送会多解一帧旧数据
  gpLive = true;
}


// 把 fb 当大端原始像素整屏推出去（自检/非 GIF 页用）
static void pushFullRaw() {
  setWindow(0, 0, W - 1, H - 1);
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  SPI.transfer(0x2C);
  digitalWrite(PIN_DC, HIGH);
  for (int y = 0; y < H; y += GIF_ROWS) {
    int rows = imin(GIF_ROWS, H - y);
    SPI.writeBytes((const uint8_t *)fb + (size_t)y * W * 2, (size_t)rows * W * 2);
  }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}

// LCD 自检（原 lcd_fast 的开机自检搬进来）：纯色 → 彩条。
// 屏花、颜色反、半边黑的时候先跑这个，比看界面更容易判断是驱动还是数据问题。
static void lcdSelfTest() {
  static const uint16_t cols[8] = {
    0xF800, 0xFC00, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF
  };
  static const uint16_t solids[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
  static const char *names[4] = { "RED", "GREEN", "BLUE", "WHITE" };
  uint8_t *raw = (uint8_t *)fb;

  for (int s = 0; s < 4; s++) {
    for (int i = 0; i < W * H; i++) {
      raw[i * 2]     = (uint8_t)(solids[s] >> 8);
      raw[i * 2 + 1] = (uint8_t)(solids[s] & 0xFF);
    }
    Serial.printf("[selftest] %s\n", names[s]);
    pushFullRaw();
    delay(350);
  }

  Serial.println("[selftest] BARS");
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      uint16_t c = cols[(x * 8) / W];
      size_t o = ((size_t)y * W + x) * 2;
      raw[o]     = (uint8_t)(c >> 8);
      raw[o + 1] = (uint8_t)(c & 0xFF);
    }
  }
  pushFullRaw();
  delay(900);
  Serial.println("[selftest] done");
}

// U5 里没有 GIF 时占位：不然屏就停在上一帧不动，看不出是没数据还是卡住
static void gifTickNoData() {
  static uint32_t lastMs = 0;
  if (lastMs && millis() - lastMs < 5000) { delay(20); return; }
  lastMs = millis();
  fillScreen(C_BG);
  textC("NO GIF DATA", 86, C_ORG, 2);
  textC(u5ok ? "U5 empty" : "U5 not found", 120, C_GRAY, 1);
  textC("send U to upload", 140, C_GRAY2, 1);
  textC("tools/upload_gif.py", 156, C_GRAY2, 1);
  flushScreen();
  delay(20);
}

// 切到上一条 / 下一条（dir = -1 / +1）。只有一条时什么都不做。
static void gifSwitch(int dir) {
  if (albumN < 2) return;
  gpEnd();                                  // 解码任务/流水线先收工，别抢 U5
  int n = (int)albumIdx + dir;
  if (n < 0) n = (int)albumN - 1;
  if (n >= (int)albumN) n = 0;
  if (!gifSelect((uint8_t)n)) {
    if (!quietLog) Serial.println("[alb] 切换失败（保留原内容）");
    return;
  }
  gifReset();                               // 帧号/节拍归零，从头放
  gifHudMs = millis();
  if (!quietLog) Serial.printf("[alb] -> %u/%u %s  %u 帧  %lu KB\n",
                               (unsigned)(albumIdx + 1), (unsigned)albumN,
                               album[albumIdx].name, (unsigned)gif.frames,
                               (unsigned long)(album[albumIdx].bytes / 1024));
}

static void gifTick() {
  if (!gif.ok) { gifTickNoData(); return; }

  // 默认 30fps（cap=30）。45 档帧间隔只有 22ms，贴着扫屏周期，肉眼更易看到撕裂；
  // 曾经试过「插 USB 45 / 电池 30」自动切换，但充满后 bat.state 在 FULL/DSG 之间
  // 来回抖（CHRG 引脚满了就是高阻，软件分不出"插着不充"和"没插"），帧率忽快忽慢更难看。
  // ★ 用 micros 定节拍：millis 只有 1ms 粒度，30fps（33.33ms）会被拖成 28.8fps。
  //   而且下一帧目标是「累加」而不是「本帧开始 + 周期」，这样延时误差不会攒成漂移。
  uint32_t nowUs = micros();
  int32_t  ahead = (int32_t)(gifNextUs - nowUs);
  if (ahead > 0) {
    if (ahead > 1500) delayMicroseconds((uint32_t)ahead - 700);  // 粗等，留余量给主循环
    return;
  }

  uint32_t t0us = micros();
  // ★ 只有 cap ≥ 45 才用双核流水线。30fps 单核串行「读+解 9.5ms + 推 15.1ms」= 24.6ms，
  //   离 33.3ms 还有 8.7ms 余量，完全够；而流水线要把块交给另一个核、块归还后还有覆盖窗口，
  //   复杂度换不来收益（还可能带来看不见的错块）。45fps 才需要两者重叠。
  // ★ 30KB 的流水线缓冲改成"要用才申请"：默认 25fps 走单核串行根本不需要它，
  //   而这块模组没有 PSRAM，全部内存只有 512KB SRAM —— 这 30KB 直接决定
  //   手机推流（HTTP + TCP 缓冲）能不能开起来。
  static uint8_t gpTry = 0;
  if (!gpOk && gifCap() >= 45 && gpTry < 3) { gpTry++; gpBegin(); }
  // HUD 期间一律走单核：流水线是按块直接推屏的，插不进覆盖层（45→43fps 看不出来）
  if (gpOk && gifCap() >= 45 && !gifHudOn()) gpPushFrame(gifFrame);
  else                                       drawGifFrame(gifFrame);
  uint32_t spent = micros() - t0us;
  uint32_t want  = gifFrameMs(gifFrame) * 1000UL;
  if (spent > want) gifSkipCnt++;                    // 绘制比目标还慢，没得等

  // ★ 帧间隔抖动统计：想要"不跳"，帧间隔必须稳定贴着 want。
  //   gapMax 明显大于 want（比如 60ms）＝ 节拍被别的事打断，不是内容问题。
  if (gifGapPrevUs) {
    uint32_t gap = t0us - gifGapPrevUs;
    if (gap < 2000000UL && gap > gifGapMaxUs) gifGapMaxUs = gap;   // 2 秒以内都记，抓 WiFi/网络卡顿
    if (gap > want + 5000UL && gap < 2000000UL) gifLateCnt++;
  }
  gifGapPrevUs = t0us;

  gifNextUs += want;
  if ((int32_t)(nowUs - gifNextUs) > (int32_t)want) gifNextUs = nowUs + want;  // 落后一帧以上就重新对齐
  gifFrame = (uint16_t)((gifFrame + 1) % gif.frames);

  gifFpsCnt++;
  if (millis() - gifFpsT0 >= 30000) {
    uint32_t dt = millis() - gifFpsT0;
    // ★ 这条 printf 要 8~10ms（115200 下几十字节），正好把那一帧顶晚，所以默认 30 秒一次；
    //   串口 'q' 可以把这类周期性统计全部关掉，画面彻底不受打扰。
    if (!quietLog) Serial.printf("[gif] %.1f fps  %lu us/frame (dec %lu + push %lu)  cap %u%s  gapmax %lu late %u  frame %u/%u  flip %lu/%lu ms(%u)  loopmax %lu ms\n",
                  gifFpsCnt * 1000.0f / dt,
                  (unsigned long)gifLastUs,
                  (unsigned long)gifDecUs, (unsigned long)gifPushUs,
                  gifCap(), gifCap() ? "" : "(orig)",
                  (unsigned long)gifGapMaxUs, (unsigned)gifLateCnt,
                  (unsigned)gifFrame, (unsigned)gif.frames,
                  (unsigned long)flipLastMs, (unsigned long)flipMaxMs, (unsigned)flipCnt,
                  (unsigned long)(loopGapMaxUs / 1000));
    gifFpsT0 = millis();
    gifFpsCnt = 0;
    gifGapMaxUs = 0;
    gifLateCnt  = 0;
    gifSkipCnt = 0;
    flipMaxMs = flipLastMs;
    flipCnt = 0;
    loopGapMaxUs = 0;
  }
}

static void drawAll(const struct tm *t) {
  fillScreen(C_BG);
  switch (page) {
    case P_FORECAST: drawForecastPage(); break;
    case P_SUN:      drawSunPage(); break;
    case P_MUSIC:    drawMusicPage(); break;
    case P_BAT:      drawBatPage(); break;
    case P_SYS:      drawSysPage(); break;
    default:
      drawHeader(t);
      drawClock(t);
      drawWeatherCard();
      break;
  }
}

// ---------------- 开机画面：Windows 98 风格四色旗 ----------------
// 几何参数由下面的宏决定。想调形状不必烧板盲猜：串口发 'z' 重画后由
// tools/screenshot.py 直接把帧缓冲导成 PNG（原理见那个脚本头部）。
// 形状来源：Win95/98 的旗是"飘带"——四条边全是正弦波浪、向右上抬、
// 中线分成 4 块（上左红/上右绿/下左蓝/下右黄），中线本身是斜的（上窄下宽）。
#define LOGO_X      45
#define LOGO_W     150
#define LOGO_Y      56
#define LOGO_H      86
#define LOGO_AMP    7.0f     // 上下边缘的波浪幅度（像素）
#define LOGO_TILT -14.0f     // 整面旗向右上抬（负数 = 右端更高）
#define LOGO_SLOPE  0.32f    // 中线倾斜：v=0 处 u≈0.34，v=1 处 u≈0.66
#define LOGO_RED   0xF800
#define LOGO_GRN   0x0540    // 比 C_GREEN 暗，接近原版旗的绿
#define LOGO_BLU   0x001F
#define LOGO_YEL   0xFFE0

// 画一列。按列扫是为了让波浪边缘天然连续：每列只算一次波高，
// 竖直方向最多切 4 段（上块左右 + 下块左右），一共 150 列 ≈ 600 次 fillRect。
static void logoCol(int x) {
  float u    = (float)(x - LOGO_X) / LOGO_W;                     // 0..1
  float wave = LOGO_AMP * sinf(u * 6.2831853f) + LOGO_TILT * u;
  float fh   = LOGO_H * (0.86f + 0.26f * u);                     // 右端略高，有透视感
  float ytf  = (float)LOGO_Y + wave;
  int   yt   = (int)lroundf(ytf);
  int   ym   = (int)lroundf(ytf + fh * 0.5f);
  int   yb   = (int)lroundf(ytf + fh);
  float vth  = 0.5f + (u - 0.5f) / LOGO_SLOPE;                   // 本列的分界位置（归一化）
  // ★ 判据：v > vth ⇒ 左（红/蓝），否则右（绿/黄）。vth 可能落在 [-1.06, 2.06]，
  //   所以下块里 vth<0.5 是"整列全左"（蓝），别写反了。
  if (vth <= 0.0f)       fillRect(x, yt, 1, ym - yt, LOGO_RED);
  else if (vth >= 0.5f)  fillRect(x, yt, 1, ym - yt, LOGO_GRN);
  else {
    int ys = (int)lroundf(ytf + vth * fh);
    fillRect(x, yt, 1, imax(0, ys - yt), LOGO_GRN);
    fillRect(x, ys, 1, imax(0, ym - ys), LOGO_RED);
  }
  if (vth < 0.5f)        fillRect(x, ym, 1, yb - ym, LOGO_BLU);
  else if (vth >= 1.0f)  fillRect(x, ym, 1, yb - ym, LOGO_YEL);
  else {
    int ys = (int)lroundf(ytf + vth * fh);
    fillRect(x, ym, 1, imax(0, ys - ym), LOGO_YEL);
    fillRect(x, ys, 1, imax(0, yb - ys), LOGO_BLU);
  }
}

// 底部跑马灯：Win98 启动时那个来回滚的进度块
#define MARQ_X   40
#define MARQ_Y  220
#define MARQ_W  160
#define MARQ_H    8
#define MARQ_SEG  8
static void logoMarquee(int seg) {
  fillRect(MARQ_X, MARQ_Y, MARQ_W, MARQ_H, 0x2104);        // 槽
  fillRect(MARQ_X, MARQ_Y, MARQ_W, 1, 0x39E7);             // 上沿高光（Win98 的立体感）
  int sw = MARQ_W / MARQ_SEG;
  int x  = MARQ_X + seg * sw;
  fillRect(x + 1, MARQ_Y + 1, sw - 2, MARQ_H - 2, 0x025F); // 蓝块
}

// 整幅：黑底 + 旗 + Microsoft / Windows 98 + 跑马灯
static void bootLogo() {
  fillScreen(C_BG);
  for (int x = LOGO_X; x < LOGO_X + LOGO_W; x++) logoCol(x);
  textC("Microsoft",   152, 0xD69A, 1);
  textC("Windows 98",  166, C_WHITE, 3);
  logoMarquee(0);
  flushScreen();
  Serial.println("[ui] boot logo");
}

// 跑马灯动 ms 毫秒。按任意键或串口来命令就提前结束 —— 别让 1.7 秒的等待挡路。
static void bootLogoAnim(uint32_t ms) {
  uint32_t t0 = millis();
  int seg = 0, dir = 1;
  while (millis() - t0 < ms) {
    logoMarquee(seg);
    flushScreen();                     // 脏的只有那一条 160×8，推 2.5KB
    seg += dir;
    if (seg >= MARQ_SEG - 1) { seg = MARQ_SEG - 1; dir = -1; }
    if (seg <= 0)            { seg = 0;            dir =  1; }
    uint32_t t1 = millis();
    while (millis() - t1 < 130) {
      if (digitalRead(PIN_BTN_A) == LOW || digitalRead(PIN_BTN_B) == LOW) return;
      // ★ 不看串口：烧录刚结束时输入缓冲里常留着残留字节，一 peek 就立刻结束，
      //   屏上就只剩静态 logo、看不到跑马灯。按键跳过才是用户真正会用的。
      delay(4);
    }
  }
}

// 帧缓冲导出（串口 'Z'）：每像素 4 位 hex + 行尾换行，PC 端 tools/screenshot.py 还原成 PNG。
// 960 字节/行 × 240 行 ≈ 230KB，USB CDC 上 2~4 秒。
static void fbDump() {
  static const char hx[] = "0123456789ABCDEF";
  static char ln[W * 4 + 2];
  // ★ GIF 页的 fb 存的是**字节预交换**过的值（drawGifFrame 为了整块 sendBytes 直接这么存的），
  //   其它页面存的是原生 RGB565。导出时统一成 RGB565 —— 否则 GIF 页截出来颜色全错
  //   （绿变暗红、蓝变暗绿，看着像"画面坏了"，其实只是字节序）。
  const bool sw = (page == P_GIF);
  Serial.printf("[fb] %d %d\n", W, H);
  for (int y = 0; y < H; y++) {
    const uint16_t *row = fb + (size_t)y * W;
    char *p = ln;
    for (int x = 0; x < W; x++) {
      uint16_t v = row[x];
      if (sw) v = (uint16_t)((v >> 8) | (v << 8));
      *p++ = hx[(v >> 12) & 0xF];
      *p++ = hx[(v >>  8) & 0xF];
      *p++ = hx[(v >>  4) & 0xF];
      *p++ = hx[ v        & 0xF];
    }
    *p++ = '\n';
    Serial.write((const uint8_t *)ln, (size_t)(p - ln));
  }
  Serial.println("[fb] end");
}

static void splash(const char *l1, const char *l2, uint16_t c2) {
  fillScreen(C_BG);
  if (l1) textC(l1, 92, C_WHITE, 2);
  if (l2) textC(l2, 124, c2, 1);
  flushScreen();
  Serial.printf("[ui] %s %s\n", l1 ? l1 : "", l2 ? l2 : "");
}

// ---------------- WiFi / NTP ----------------
static bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

static void connectWiFi(uint32_t ms) {
  Serial.printf("[wifi] connecting %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);                 // ★ 必须开 modem sleep：false 时 WiFi 常驻 ~100mA
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (!wifiUp() && millis() - t0 < ms) { delay(250); Serial.print("."); }
  Serial.println(wifiUp() ? " OK " + WiFi.localIP().toString() : " FAIL");
}

static bool ntpSync(uint32_t ms) {
  if (!wifiUp()) return false;
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org");
  struct tm t;
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (getLocalTime(&t, 50) && t.tm_year > 120) return true;
    if (!uiPump(1)) return false;        // 按键优先
  }
  return false;
}

// ---------------- 主流程状态 ----------------
// 因低压进过深睡：醒来第一件事是复查电压，仍低就直接再睡（不点屏、不连 WiFi）
RTC_DATA_ATTR static uint8_t lowBatSleep = 0;

static uint32_t lastWxMs   = 0;
static uint32_t wxRetryMs  = 0;
static int      lastSec    = -1;
static int      lastDay    = -1;
static int      lastNet    = -1;     // 0 断 1 通
static bool     fullRedraw = true;

// ---------------- 按钮（轻触开关，内部上拉） ----------------
struct Btn {
  uint8_t  pin;
  bool     raw;       // 上一次采样电平（true = LOW = 按下）
  bool     stable;    // 去抖后的状态
  bool     longFired;
  uint8_t  cnt;       // 与 raw 相同且连续的采样次数（含本次），饱和到 200
  uint32_t tRaw;      // raw 最近一次变化的时刻
  uint32_t tDown;
  uint32_t tRep;      // 上一次"长按连发"的时刻
  uint32_t tLastDown; // 上一次判定"按下"的时刻（用于 BTN_MIN_MS 限速）
};
static Btn btnA = { PIN_BTN_A, false, false, false, 0, 0, 0, 0, 0 };
static Btn btnB = { PIN_BTN_B, false, false, false, 0, 0, 0, 0, 0 };

// 诊断计数（'e' 命令读，用来把"按键不灵"分成 硬件/去抖/双键误吞 三种）。
// 只有扫描任务写，串口读，32 位读写本身原子，不加锁。
static volatile uint32_t btnSample[2] = { 0, 0 };   // 采样次数
static volatile uint32_t btnLow[2]    = { 0, 0 };   // 采到 LOW 的次数
static volatile uint32_t btnDown[2]   = { 0, 0 };   // 判定"按下"的次数
static volatile uint32_t btnShort[2]  = { 0, 0 };   // 产生的短按事件
static volatile uint32_t btnLong[2]   = { 0, 0 };   // 产生的长按事件
static volatile uint32_t btnSwallow   = 0;          // 双键把原本该发的短按吞掉的次数

// 返回 0 无事件 / 1 短按 / 2 长按（首次）/ 3 长按连发。idx: 0=A 1=B
// （参数用 int 而不是 Btn&：.ino 的自动原型会插到 struct 定义之前，用自定义类型必炸）
static int btnEvent(int idx) {
  Btn &b = (idx == 0) ? btnA : btnB;
  bool     raw = (digitalRead(b.pin) == LOW);
  uint32_t now = millis();

  btnSample[idx]++;
  if (raw) btnLow[idx]++;

  if (raw != b.raw) { b.raw = raw; b.cnt = 0; b.tRaw = now; }
  else if (b.cnt < 200) b.cnt++;

  int ev = 0;
  if (!b.stable) {
    // ★ 按下：连续 BTN_DN_CNT 次都是 LOW 就认，不等 20ms。
    //   再加 BTN_MIN_MS 限速：触点半接触时电平可能在几十毫秒内反复通断，
    //   不限速就会把"一次按动"判成好几次短按（观众看到的是"按一下翻三页"）。
    if (raw && (uint8_t)(b.cnt + 1) >= BTN_DN_CNT && now - b.tLastDown >= BTN_MIN_MS) {
      b.stable    = true;
      b.longFired = false;
      b.tDown     = now;
      b.tRep      = now;
      b.tLastDown = now;
      btnDown[idx]++;
    }
  } else {
    // ★ 松手：要稳定 BTN_UP_MS 才认（松手抖动多，认早了会把一次按动拆成多次）
    if (!raw && now - b.tRaw >= BTN_UP_MS) {
      b.stable = false;
      if (!b.longFired) { ev = 1; btnShort[idx]++; }
    }
  }

  if (b.stable && !b.longFired && now - b.tDown > BTN_LONG_MS) {
    b.longFired = true;
    b.tRep = now;
    ev = 2;
    btnLong[idx]++;
  } else if (b.stable && b.longFired && now - b.tRep > BTN_REP_MS) {
    // ★ 按住不放继续连发：调音量时不用"长按一次一档"，可以一路扫上去
    b.tRep = now;
    ev = 3;
  }
  return ev;
}

static bool syncRequest = false;

// ============ 按键事件环（扫描任务 → 主循环） ============
// ★ 为什么按键要独立任务：主循环里随时可能被别的事堵住 —— 一条 printf 在 USB CDC 上
//   最多阻塞 250ms（tx_timeout）、网络重试 8s、整屏重绘 15ms……
//   按键如果只在 loop 里轮询，按下那一瞬正好落在阻塞里就被整个吞掉：
//   当时的症状就是"按一下没反应，得再按一下 / 按了半天才翻页"。
//   改成 core1 上 4ms 一次的独立扫描任务，事件带按下时刻排队，主循环晚多久都能补上。
#define BTN_EVQ 8
static portMUX_TYPE     bqMux  = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t bqHead = 0, bqTail = 0;
static volatile uint8_t bqEv[BTN_EVQ];       // bit0-1 = 键号(0=A 1=B 2=双键), bit2 = 长按, bit3 = 连发
static volatile uint32_t bqTDown[BTN_EVQ];   // 该键按下的时刻（millis）

static void btnPush(int idx, int ev, uint32_t tDownMs) {
  portENTER_CRITICAL(&bqMux);
  uint8_t n = (uint8_t)((bqHead + 1) % BTN_EVQ);
  if (n == bqTail) bqTail = (uint8_t)((bqTail + 1) % BTN_EVQ);  // 满了丢最旧的
  bqEv[bqHead]    = (uint8_t)((uint8_t)(idx & 3) |
                              (ev >= 2 ? 4 : 0) |          // 长按
                              (ev == 3 ? 8 : 0));          // 长按连发
  bqTDown[bqHead] = tDownMs;
  bqHead = n;
  portEXIT_CRITICAL(&bqMux);
}

static void btnScanTask(void *) {
  static bool     bothLatch = false, bothSent = false;
  static uint32_t bothT     = 0;
  for (;;) {
    for (int i = 0; i < 2; i++) {
      if (i == 0 && !BTN_A_OK) continue;
      int ev = btnEvent(i);
      if (!ev) continue;
      const Btn &b = (i == 0) ? btnA : btnB;
      btnPush(i, ev, b.tDown);
    }

    // ★ 双键同按（保持 BTN_BOTH_MS）= 全局回时钟。
    //   为什么需要它：音乐页把单键长按"吃掉"去调音量了，没有这条路就回不到时钟页。
    //   同时把两个键的 longFired 置位 —— 既压掉随后 1.2s 会各自发出的单键长按，
    //   也保证松手时不补发短按（否则一次双键会连带翻两页）。
    // ★★ 必须加"配对时间窗"：A 键是飞线到 IO0 的，WiFi 一发射就可能被干扰出
    //   假的稳定低电平。若不加窗，A 的假低 + B 的真按 会被判成双键，
    //   B 的短按就被 longFired 吞掉 —— 表现正是"按几下不动"。
    bool aD = BTN_A_OK && btnA.stable;
    bool bD = btnB.stable;
    int32_t dt = (int32_t)btnA.tDown - (int32_t)btnB.tDown;
    if (dt < 0) dt = -dt;
    if (aD && bD && dt <= BTN_PAIR_MS) {
      if (!bothLatch) {
        bothLatch = true;
        bothSent  = false;
        bothT     = millis();
        if (!btnA.longFired) btnSwallow++;   // 诊断：这次按压本来是短按，被双键吞了
        if (!btnB.longFired) btnSwallow++;
      }
      btnA.longFired = true;
      btnB.longFired = true;
      if (!bothSent && millis() - bothT > BTN_BOTH_MS) {
        bothSent = true;
        btnPush(2, 2, bothT);
      }
    } else {
      bothLatch = false;
      bothSent  = false;
    }
    vTaskDelay(pdMS_TO_TICKS(BTN_SAMPLE_MS));
  }
}

// 翻页。GIF 页进出都要重置播放状态（fb 的内容格式也不一样）
static void gotoPage(uint8_t p) {
  if (p >= PAGE_COUNT || p == page) return;
  if (page == P_GIF) gpEnd();          // 离开 GIF 页先让解码任务收工、把信号量账面抹平
  page = p;
  fullRedraw = true;
  if (p == P_GIF) gifReset();
  if (!quietLog) Serial.printf("[btn] page %d/%d\n", page + 1, PAGE_COUNT);
}

// 短按：A 上一页 / B 下一页
// 长按：**按页面分派**（原来是"任一长按都回时钟"，音乐页要拿它调音量）
//   P_MUSIC  A = 音量−  B = 音量+   （按住不放连发，BTN_REP_MS 一档）
//   P_CLOCK  立即刷天气（原来的"确认"语义）
//   其它页   回时钟（保持原行为，当逃生口用）
// 双键同按：任何页都回时钟 —— 音乐页的长按被占用了，回家只能靠这条
static void handleButtons() {
  // 事件由 btnScanTask 采好排队，这里只管消费 —— 主循环被堵多久都不会漏按
  for (;;) {
    uint8_t  e  = 0;
    uint32_t td = 0;
    portENTER_CRITICAL(&bqMux);
    bool has = (bqTail != bqHead);
    if (has) {
      e  = bqEv[bqTail];
      td = bqTDown[bqTail];
      bqTail = (uint8_t)((bqTail + 1) % BTN_EVQ);
    }
    portEXIT_CRITICAL(&bqMux);
    if (!has) break;

    int idx  = e & 3;
    int kind = (e & 8) ? 3 : ((e & 4) ? 2 : 1);

    if (idx == 2) {                            // 双键同按 = 任何页都回时钟
      if (page != P_CLOCK) {
        pendingFlipMs = td ? td : millis();
        gotoPage(P_CLOCK);
        if (!quietLog) Serial.println("[btn] both -> home");
      }
      continue;
    }

    if (kind == 1) {
      pendingFlipMs = td ? td : millis();     // 计时从"手指按下"算，不是从"程序看到"算
      if (idx) gotoPage((uint8_t)((page + 1) % PAGE_COUNT));                    // B 下一页
      else     gotoPage((uint8_t)((page + PAGE_COUNT - 1) % PAGE_COUNT));       // A 上一页
      // 提示音去重：连按时不排队，否则一次快翻会攒下好几声"嘀"
      if (!spkBusy && spkQTail == spkQHead) spkPlay(SND_KEY);
      continue;
    }

    // ---- 长按：2 = 首次触发，3 = 按住不放的连发 ----
    if (page == P_MUSIC) {
      musicVol(idx ? +1 : -1);                // 音乐页：A 音量− / B 音量+（可按住扫）
    } else if (page == P_GIF && albumN > 1) {
      if (kind == 2) {                        // 只认首次：连发会把 300KB 一条条读爆
        if (!spkBusy && spkQTail == spkQHead) spkPlay(SND_KEY);
        gifSwitch(idx ? +1 : -1);             // GIF 页：A 上一条 / B 下一条
      }
    } else if (kind == 2 && page == P_CLOCK) {
      if (!spkBusy && spkQTail == spkQHead) spkPlay(SND_KEY);
      syncRequest = true;
      if (!quietLog) Serial.println("[btn] long -> sync weather");
    } else if (kind == 2) {
      if (!spkBusy && spkQTail == spkQHead) spkPlay(SND_KEY);
      pendingFlipMs = td ? td : millis();
      gotoPage(P_CLOCK);
      if (!quietLog) Serial.println("[btn] long -> home");
    }
    // kind==3 且不在音乐页：连发什么也不做（否则会重复翻页 / 重复刷天气）
  }
}

// 串口十六进制解析（专辑索引用）：0-9 / A-F / a-f
static inline int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}
static uint32_t hexN(const char *s, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) v = (v << 4) | (uint32_t)hexVal(s[i]);
  return v;
}

static void handleSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    uint32_t hz = 0;
    // 网络阻塞期间的让步调用里：重活（上传/自检/U5 读）一律不做，否则等于在请求里套请求
    if (inNet && (c == 'U' || c == 'A' || c == 'x' || c == 't')) continue;
    if (c == 'B') { btnPush(1, 1, millis()); continue; }   // 注入一次"B 键短按"（等价于按压，测试用）
    if (c == 'l') { btnPush(0, 2, millis()); continue; }   // 注入"长按 A"（页面动作，测试用）
    if (c == 'L') { btnPush(1, 2, millis()); continue; }   // 注入"长按 B"
    if (c == 'H') { btnPush(2, 2, millis()); continue; }   // 注入"双键同按"（回时钟）
    if (c == 'P') {                                        // 当前页面 / 音量 / 播放状态
      static const char *pn[] = { "GIF", "CLOCK", "FORECAST", "SUN", "MUSIC", "BAT", "SYS" };
      Serial.printf("[ui] page=%u/%u %s  vol=%u%% mute=%d stream=%d\n",
                    (unsigned)(page + 1), (unsigned)PAGE_COUNT,
                    page < P_MAX ? pn[page] : "?", (unsigned)spkVol, (int)spkMute,
                    (int)mus.stream);
      continue;
    }
    if (c == 'V') {                       // 专辑列表（tools/album.py 解析这个）
      for (uint8_t i = 0; i < albumN; i++)
        Serial.printf("ALB %u %u %u %u %u %lu %lu %s\n",
                      (unsigned)i, (unsigned)album[i].kind, (unsigned)album[i].frames,
                      (unsigned)album[i].w, (unsigned)album[i].h,
                      (unsigned long)album[i].bytes, (unsigned long)album[i].off,
                      album[i].name);
      Serial.printf("ALBEND %u %lu %lu %lu\n", (unsigned)albumN, (unsigned long)albumEnd,
                    (unsigned long)gif.bufCap, (unsigned long)gif.wantCap);
      continue;
    }
    if (c == 'I') {                       // 写索引：I <count> <hex × count*48>
      // 每条 24 字节按字段展开成 hex（数值不是字节流）：off8 bytes8 frames4 w4 h4 kind2 pad2 name16
      static char ln[8 + ALB_MAX * ALB_ENT * 2 + 8];
      int n = 0;
      uint32_t t0 = millis();
      while (millis() - t0 < 4000) {
        if (!Serial.available()) { delay(1); continue; }
        int ch = Serial.read();
        if (ch == '\n') break;
        if (ch != '\r' && n < (int)sizeof(ln) - 1) ln[n++] = (char)ch;
        t0 = millis();
      }
      ln[n] = 0;
      unsigned cnt = 0;
      int off = 0;
      if (sscanf(ln, "%u %n", &cnt, &off) != 1 || cnt > ALB_MAX) {
        Serial.println("ERR idx");
        continue;
      }
      const char *hx = ln + off;
      if ((int)strlen(hx) < (int)(cnt * ALB_ENT * 2)) { Serial.println("ERR idx len"); continue; }
      gpEnd();                            // 解码任务别在这时候读 U5
      albumN   = (uint8_t)cnt;
      albumEnd = 0;
      for (unsigned i = 0; i < cnt; i++) {
        const char *q = hx + (size_t)i * ALB_ENT * 2;
        AlbumEntry &e = album[i];
        e.off    = hexN(q,       8);
        e.bytes  = hexN(q +  8,  8);
        e.frames = (uint16_t)hexN(q + 16, 4);
        e.w      = (uint16_t)hexN(q + 20, 4);
        e.h      = (uint16_t)hexN(q + 24, 4);
        e.kind   = (uint8_t)hexN(q + 28, 2);
        for (int k = 0; k < ALB_NAME; k++) e.name[k] = (char)hexN(q + 32 + k * 2, 2);
        e.name[ALB_NAME] = 0;
        uint32_t end = albAlign64k(e.off + e.bytes);
        if (end > albumEnd) albumEnd = end;
      }
      if (albumIdx >= albumN) albumIdx = 0;
      bool okIdx = albumSave();
      if (okIdx) {
        // 新条目可能帧更大：先按全专辑最大帧重算预留（运行中扩容多半拿不到，
        // 拿不到就退回只保证当前这条，屏上照样有画面；真要全播放需复位一次）
        gif.wantCap = gifMaxFrameAll();
        gifLoad();
        gifReset();
        if (page == P_GIF) fullRedraw = true; else gotoPage(P_GIF);
      }
      Serial.printf("IOK %d\n", okIdx ? 1 : 0);
      continue;
    }
    if (c == '+' || c == '=') { spkSetVol(spkVol + 10); spkPlay(SND_KEY); spkReport(); continue; }
    if (c == '-' || c == '_') { spkSetVol(spkVol - 10); spkPlay(SND_KEY); spkReport(); continue; }
    if (c == 'm') { spkToggleMute(); spkReport(); continue; }
    if (c == 'k') { spkPlay(SND_BOOT); Serial.println("[spk] test chime"); continue; }
    if (c == 'a') { audioReport(); continue; }                 // 音频状态
    if (c == 'h') { wsReport(); rnReport();                       // HTTP / 环形缓冲状态
                  Serial.printf("[boot] reset=%d up=%lus heap=%uK maxblk=%u\n",
                                bootReset, (unsigned long)(millis() / 1000),
                                (unsigned)(ESP.getFreeHeap() / 1024),
                                (unsigned)ESP.getMaxAllocHeap());
                  continue; }
    if (c == 's') {                                            // 播放 U5 里的音频
      if (!audioLoad()) { Serial.println("[aud] 没有音频，先用 tools/upload_audio.py 上传"); continue; }
      audioPlay();
      continue;
    }
    if (c == 'S') { audioStop(); Serial.println("[aud] 停止"); continue; }
    if (c == 'A') {                                            // 上传音频（tools/upload_audio.py）
      gpEnd();                                                  // 解码任务在抢 U5，先让它收工
      uint32_t gifEnd = gif.ok ? (gif.dataOff + gif.dataBytes) : 0;
      if (gifEnd > AUDIO_BASE) {
        Serial.printf("[aud] GIF 占到 %lu 字节，已越过音频区起点 %lu，拒绝上传（会毁掉 GIF）\n",
                      (unsigned long)gifEnd, (unsigned long)AUDIO_BASE);
        continue;
      }
      Serial.printf("[aud] 接收音频区（GIF 占 %lu 字节）\n", (unsigned long)gifEnd);
      if (u5upload(Serial, AUDIO_BASE)) audioLoad();
      continue;
    }
    if (c == 'w') { syncRequest = true; continue; }
    if (c == 'v') {                       // 电池诊断：ADC 原始值与换算结果
      batSample();
      Serial.printf("[bat] adc %lumV -> %.2fV  %d%%  %s  %s\n",
                    (unsigned long)analogReadMilliVolts(PIN_ADC_BAT), bat.v, bat.pct,
                    bat.chrgLow ? "CHRG=LOW" : "CHRG=HIGH",
                    bat.state == BAT_CHG ? "CHG" : bat.state == BAT_FULL ? "FULL" : "DSG");
      Serial.print("[bat] raw mV:");
      for (int i = 0; i < 8; i++) Serial.printf(" %u", (unsigned)analogReadMilliVolts(PIN_ADC_BAT));
      Serial.println();
      if (page != P_GIF) fullRedraw = true;
      continue;
    }
    if (c == 'q') {                       // 静音：关掉周期性统计（printf 会顶晚一帧）
      quietLog = !quietLog;
      Serial.printf("[log] quiet %s\n", quietLog ? "ON" : "OFF");
      if (!quietLog) {                    // 刚打开时把静音这段时间的账报一次
        uint32_t dt = millis() - gifFpsT0;
        if (dt < 1) dt = 1;
        Serial.printf("[gif] 近 %.1fs：%.1f fps  gapmax %lu us  late %u  (静音期是否真有卡)\n",
                      dt / 1000.0f, gifFpsCnt * 1000.0f / dt,
                      (unsigned long)gifGapMaxUs, (unsigned)gifLateCnt);
        Serial.printf("[lat] 翻页 %u 次：最近 %lu ms  最差 %lu ms   主循环最长空转 %lu ms  堆 %uK\n",
                      (unsigned)flipCnt, (unsigned long)flipLastMs, (unsigned long)flipMaxMs,
                      (unsigned long)(loopGapMaxUs / 1000), (unsigned)(ESP.getFreeHeap() / 1024));
        loopGapMaxUs = 0;
      }
      continue;
    }
    // ---- 截图：把帧缓冲导出成 hex，PC 端 tools/screenshot.py 还原成 PNG ----
    // 调 UI 时不用再靠肉眼判断"有没有重叠/偏位"，直接看图。960B/行 × 240 行 ≈ 230KB。
    if (c == 'Z') {                       // 截当前画面
      fbDump();
      continue;
    }
    if (c == 'z') {                       // 重画开机 logo 再截（验证开机画面用）
      bootLogo();
      fbDump();
      continue;
    }
    if (c == 'y') {                       // I2S 实际消耗速率：判断"慢"是时钟问题还是供数问题
      rnStop();                           // 先摘钩，DMA 只由这里喂
      audioStop();
      static int16_t yz[SPK_FRAMES * 2];
      memset(yz, 0, sizeof(yz));
      uint32_t t0 = millis();
      size_t   tot = 0;
      while (millis() - t0 < 3000) tot += spkWrite(yz, sizeof(yz));
      float sec = (millis() - t0) / 1000.0f;
      Serial.printf("[i2s] %u B / %.2fs = %.0f B/s = %.0f Hz  (期望 125000 / 31250)\n",
                    (unsigned)tot, sec, tot / sec, tot / sec / 4);
      continue;
    }
    if (c == 'x') {                       // U5 双读自检：查 Fast Read 有没有误码
      gpEnd();                            // 先让解码任务收工，否则它和这里抢 U5
      gifReadTest(8);
      continue;
    }
    if (c == 'b') {                       // 报两个按钮的实时电平，边按边看
      Serial.printf("[btn] A=%c B=%c   (H=松开 L=按下)\n",
                    digitalRead(PIN_BTN_A) ? 'H' : 'L',
                    digitalRead(PIN_BTN_B) ? 'H' : 'L');
      continue;
    }
    if (c == 'e') {                       // ★ 按键诊断：先发 E 清零，按几下，再发 e 核对
      // 读法：按 10 下 B → down B 应该是 10、short B 应该是 10。
      //   down 偏少 ⇒ 电信号没进来（触点氧化/虚焊/飞线），硬件问题；
      //   down 对但 short 偏少 ⇒ 被双键误吞（看 bothSwallow）或按太久算成长按（看 long）。
      Serial.printf("[key] sample A=%lu B=%lu | low A=%lu B=%lu\n",
                    (unsigned long)btnSample[0], (unsigned long)btnSample[1],
                    (unsigned long)btnLow[0],    (unsigned long)btnLow[1]);
      Serial.printf("[key] down   A=%lu B=%lu | short A=%lu B=%lu | long A=%lu B=%lu\n",
                    (unsigned long)btnDown[0],  (unsigned long)btnDown[1],
                    (unsigned long)btnShort[0], (unsigned long)btnShort[1],
                    (unsigned long)btnLong[0],  (unsigned long)btnLong[1]);
      Serial.printf("[key] bothSwallow=%lu | lvl A=%c B=%c stable=%d/%d longFired=%d/%d\n",
                    (unsigned long)btnSwallow,
                    digitalRead(PIN_BTN_A) ? 'H' : 'L',
                    digitalRead(PIN_BTN_B) ? 'H' : 'L',
                    (int)btnA.stable, (int)btnB.stable,
                    (int)btnA.longFired, (int)btnB.longFired);
      continue;
    }
    if (c == 'E') {                       // 按键计数器清零（诊断用，不打印）
      btnSample[0] = btnSample[1] = 0;
      btnLow[0]    = btnLow[1]    = 0;
      btnDown[0]   = btnDown[1]   = 0;
      btnShort[0]  = btnShort[1]  = 0;
      btnLong[0]   = btnLong[1]   = 0;
      btnSwallow   = 0;
      Serial.println("[key] counters cleared");
      continue;
    }
    if (c == 'n') {                       // 串口模拟按 B（下一页）
      pendingFlipMs = millis();
      gotoPage((uint8_t)((page + 1) % PAGE_COUNT));
      continue;
    }
    if (c == 'p') {                       // 串口模拟按 A（上一页）
      pendingFlipMs = millis();
      gotoPage((uint8_t)((page + PAGE_COUNT - 1) % PAGE_COUNT));
      continue;
    }
    if (c == '[' || c == ']') {           // 调背光亮度
      blSet((uint8_t)(c == ']' ? imin(100, blPct + 10) : imax(0, blPct - 10)));
      Serial.printf("[bl] %u%%\n", blPct);
      continue;
    }
    if (c == 'g') {                       // 回到 GIF 页
      gotoPage(P_GIF);
      continue;
    }
    if (c == 'f') {                       // 循环切换 GIF 帧率上限
      gifFpsIdx = (uint8_t)((gifFpsIdx + 1) % kFpsMenuN);
      gifNextUs = micros();
      gifFpsT0  = millis();
      gifFpsCnt = 0;
      Serial.printf("[gif] fps cap = %u%s\n", gifCap(), gifCap() ? "" : " (original)");
      continue;
    }
    if (c == 'U') {                       // 进入 U5 上传模式（tools/upload_gif.py）
      gpEnd();                            // 解码任务也在用 U5，必须先让它收工
      Serial.println("[u5] upload mode, waiting for H/D/E");
      Serial.flush();
      uint32_t t0 = millis();
      bool up = u5upload(Serial);
      Serial.printf("[u5] upload %s  %.1fs\n", up ? "OK" : "FAIL",
                    (millis() - t0) / 1000.0);
      if (up) {
        gifLoad();
        if (page == P_GIF) { gifReset(); fullRedraw = true; }
        else               gotoPage(P_GIF);
      }
      continue;
    }
    if (c == 't') {                       // LCD 自检：纯色 + 彩条
      lcdSelfTest();
      fullRedraw = true;
      continue;
    }
    switch (c) {
      case '1': hz = 10000000UL; break;
      case '2': hz = 26000000UL; break;
      case '3': hz = 40000000UL; break;
      case '4': hz = 80000000UL; break;   // 超规格，花屏就按 3 回退
      default: continue;
    }
    spiCfg = SPISettings(hz, MSBFIRST, SPI_MODE0);
    spiHzNow = hz;
    Serial.printf(">> SPI = %lu Hz\n", (unsigned long)hz);
  }
}

// 电池过低：关背光、黑屏、深睡 10 分钟后再看一次（醒来电压回升就正常启动）
static void enterLowBatSleep(float v) {
  lowBatSleep = 1;
  Serial.printf("[bat] %.2fV too low -> deep sleep 10min\n", v);
  spkPlay(SND_LOWBAT);                // 下行三音告警，等它播完再睡
  Serial.flush();
  delay(1100);
  blSet(0);
  fillScreen(C_BG);
  flushScreen();
  esp_sleep_enable_timer_wakeup(10ULL * 60 * 1000000ULL);
  delay(30);
  esp_deep_sleep_start();
}

void setup() {
  // ★ USB CDC 的接收环形缓冲默认只有约 1KB，上传 GIF 时一次灌 2048 字节块会溢出丢数据
  //   （症状：板子回 ERR short 951/2048）。必须在 Serial.begin() 之前放大。
  Serial.setRxBufferSize(16384);
  Serial.begin(115200);
  // ★ USB CDC 的写是有超时的：主机开着串口但不读（monitor 暂停/滚屏/别的程序占着）时，
  //   USBCDC::write 会在 while(to_send) 里**忙等最多 250ms**（tx_timeout_ms 默认值），
  //   而且是不带 delay 的死转 —— 正好落在翻页那一帧上，就是"按一下半天才翻页"。
  //   降到 20ms：正常日志照打，真堵上了也只丢几个字节，绝不拖住画面。
  Serial.setTxTimeoutMs(20);
  delay(400);

  if (lowBatSleep) {                   // 上次因低压睡过去，先看电压恢复没
    pinMode(PIN_ADC_BAT, INPUT);
    analogSetPinAttenuation(PIN_ADC_BAT, ADC_11db);
    pinMode(PIN_CHRG, INPUT_PULLUP);
    delay(30);
    float v = vbatRead();
    bool  chrg = (digitalRead(PIN_CHRG) == LOW);
    Serial.printf("[bat] wake %.2fV %s\n", v, chrg ? "CHG" : "no-chg");
    if (v < 3.35f && !chrg) {
      Serial.flush();
      esp_sleep_enable_timer_wakeup(10ULL * 60 * 1000000ULL);
      esp_deep_sleep_start();
    }
    lowBatSleep = 0;
  }

  Serial.println("\n=== Weather Clock v3  " __DATE__ " " __TIME__ " ===");
  // 复位原因：上次是正常上电(1)/软复位(3)/panic(4)/任务WDT(6)/中断WDT(7)/掉电(9)
  bootReset = (int)esp_reset_reason();
  Serial.printf("[boot] reset_reason=%d  heap=%uK\n",
                bootReset, (unsigned)(ESP.getFreeHeap() / 1024));

  lcdInit();
  spkInit();                          // I2S 独立任务（core0/优先级1），不占帧管线
  {                                   // 音量从 NVS 恢复（首次 55%）：音乐页长按调过就记住
    Preferences pf;
    if (pf.begin("ui", false)) {
      uint8_t v = pf.getUChar("vol", 55);
      pf.end();
      if (v <= 100) spkSetVol(v);
      Serial.printf("[vol] 恢复 %u%%\n", (unsigned)v);
    }
  }
  pinMode(PIN_BTN_A, INPUT_PULLUP);   // 两个轻触开关：另一端接 GND
  pinMode(PIN_BTN_B, INPUT_PULLUP);
  delay(5);
  // 按键扫描独立任务：core1、优先级 3、4ms 一次。放 core1 是为了跟总线任务（core0）分开，
  // 优先级高于 loop 所在的任务，主循环再忙也照样能采到按下沿。
  xTaskCreatePinnedToCore(btnScanTask, "btn", 2048, nullptr, 3, nullptr, 1);
  // 开机报一次电平：H=松开（正常） L=按下/短路（接错或焊在常通组上）
  Serial.printf("[btn] A=%c B=%c   (H=松开 L=按下)\n",
                digitalRead(PIN_BTN_A) ? 'H' : 'L',
                digitalRead(PIN_BTN_B) ? 'H' : 'L');

  // 电池：IO1 = ADC 分压（÷2），IO2 = TP4056 CHRG 开漏（充电中拉低）
  pinMode(PIN_ADC_BAT, INPUT);
  analogSetPinAttenuation(PIN_ADC_BAT, ADC_11db);
  pinMode(PIN_CHRG, INPUT_PULLUP);
  delay(2);
  batSample();
  Serial.printf("[bat] %.2fV %d%% adc %lumV %s\n", bat.v, bat.pct,
                (unsigned long)analogReadMilliVolts(PIN_ADC_BAT),
                bat.state == BAT_CHG ? "CHG" : bat.state == BAT_FULL ? "FULL" : "DSG");

  fb = (uint16_t *)heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!fb) {
    fb = (uint16_t *)heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fb) Serial.println("[fb] using PSRAM");
  }
  if (!fb) {
    Serial.printf("[fb] alloc FAIL, free=%u\n", (unsigned)ESP.getFreeHeap());
    return;                                  // 没缓冲就什么也画不了
  }
  Serial.printf("[fb] ok, free heap %u\n", (unsigned)ESP.getFreeHeap());

  bootLogo();                         // Windows 98 风格开机画面（黑底 + 四色旗）
  spkPlay(SND_BOOT);                  // 开机和弦（播放不阻塞，WiFi 连接照常）
  bootLogoAnim(1700);                 // 底部跑马灯；按键或串口命令可提前结束

  // ★★ U5 专辑 + GIF 帧缓冲必须在**连 WiFi / 拉天气之前**做好：
  //   帧缓冲要的是 37~41KB 的**连续**内部 RAM。等 WiFi 栈、DNS、HTTP 客户端的堆分配
  //   铺开之后再要，最大连续块只剩 ~32KB → malloc 失败 → 这条 GIF 屏上根本不显示
  //   （日志 "[gif] 分配 40804 字节失败"）。提前到最前面，堆还完整，能拿到。
  u5init();
  albumScan();          // 先读专辑索引（没有索引就把老的单条 GIF 当成第 1 条）
  // 按**全专辑最大帧**预留缓冲（不是只跟着第 1 条走）
  gif.wantCap = gifMaxFrameAll();
  Serial.printf("[gif] 全专辑最大帧 %lu B（启动预留）\n", (unsigned long)gif.wantCap);
  gifLoad();

  connectWiFi(15000);
  if (wifiUp()) {
    splash("WIFI OK", WiFi.localIP().toString().c_str(), C_GREEN);
    bool ntp = ntpSync(8000);
    splash(ntp ? "NTP OK" : "NTP FAIL", ntp ? "syncing clock" : "retry in loop", ntp ? C_GREEN : C_ORG);
  } else {
    splash("WIFI FAIL", "check SSID / 2.4G", C_RED);
  }

  splash("FETCHING WEATHER", "open-meteo.com", C_YEL);
  fetchWeather();
  lastWxMs = millis();

  audioLoad();          // 音频区：没有就只在日志里说一句，不影响启动
  // 双核流水线不在这里申请：见 gifTick()，只有切到 45/60fps 才建。
  gifReset();
  fullRedraw = true;
}

void loop() {
  // 主循环空转间隔：>150ms 说明 loop 被某一步堵住了（printf / 网络 / 整屏重绘），
  // 这是"时不时卡一下"最直接的证据，窗口内取最大值随统计行一起报。
  {
    uint32_t nu = micros();
    if (loopPrevUs) {
      uint32_t g = nu - loopPrevUs;
      if (g < 5000000UL && g > loopGapMaxUs) loopGapMaxUs = g;
    }
    loopPrevUs = nu;
  }
  handleSerial();
  handleButtons();

  // 手机点播服务端：WiFi 一通就拉起来（只建一次）
  if (!wsUp && wifiUp()) wsBegin();
  rnPump();                  // 推进 U5 环形区的预擦水位

  struct tm t;
  bool have = nowTM(&t);

  // 整点报时：只在 08:00~21:59 响，夜里不吵；每小时只触发一次
  static int8_t lastChimeHour = -1;
  if (have && t.tm_min == 0 && t.tm_sec < 3 && (int8_t)t.tm_hour != lastChimeHour) {
    lastChimeHour = (int8_t)t.tm_hour;
    if (t.tm_hour >= 8 && t.tm_hour < 22) spkPlay(SND_HOUR);
  }

  // 长按/串口请求刷天气：先拉数据，随后整屏重绘
  if (syncRequest) {
    syncRequest = false;
    if (wifiUp()) {
      Serial.println("[wx] manual sync");
      fetchWeather();
      lastWxMs = millis();
      wxRetryMs = 0;
    }
    if (page != P_GIF) fullRedraw = true;
  }

  // 网络状态变化：时钟页只动顶部，其它页整页重画
  int net = wifiUp() ? 1 : 0;
  if (net != lastNet) {
    lastNet = net;
    if (page == P_CLOCK && !fullRedraw) drawHeader(have ? &t : nullptr);
    else if (page != P_GIF) fullRedraw = true;
  }

  if (page == P_GIF) {
    if (fullRedraw) { fullRedraw = false; gifReset(); }
    gifTick();                            // 自己整屏推，不走脏矩形
  } else {
    if (fullRedraw) {
      fullRedraw = false;
      drawAll(have ? &t : nullptr);
      lastSec = have ? t.tm_sec : -1;
      lastDay = have ? t.tm_mday : -1;
    } else if (page == P_CLOCK && have && (t.tm_sec != lastSec || t.tm_mday != lastDay)) {
      if (t.tm_mday != lastDay) { lastDay = t.tm_mday; drawHeader(&t); }
      drawClock(&t);
      lastSec = t.tm_sec;
    } else if (page == P_CLOCK && !have && lastSec != -1) {
      drawClock(nullptr);
      lastSec = -1;
    }
    flushScreen();
  }

  // 翻页时延：从"手指按下"到"新页第一帧真的推上屏"。这一条才是用户能感觉到的东西，
  // 单看帧率看不出来（帧率正常但按键被吞，一样是"按了没反应"）。
  if (pendingFlipMs) {
    uint32_t d = (uint32_t)(millis() - pendingFlipMs);
    pendingFlipMs = 0;
    flipLastMs = d;
    lastFlipAtMs = millis();
    if (d > flipMaxMs) flipMaxMs = d;
    flipCnt++;
  }

  // 电量：10 秒采一次（16 次 ADC 平均，约 1ms 不会卡显示）
  static uint32_t lastBatMs = 0;
  static int lastBatState = -1, lastBatPct = -1;
  if (millis() - lastBatMs > 10000UL) {
    lastBatMs = millis();
    batSample();
    if (bat.state != lastBatState) {
      lastBatState = bat.state;
      lastBatPct   = bat.pct;
      if (!quietLog) Serial.printf("[bat] %.2fV %d%% %s  %dmin\n", bat.v, bat.pct,
                    bat.state == BAT_CHG ? "CHG" : bat.state == BAT_FULL ? "FULL" : "DSG",
                    bat.mins);
      if (page != P_GIF) fullRedraw = true;
    } else if (bat.pct != lastBatPct) {
      lastBatPct = bat.pct;
      if (page == P_BAT) fullRedraw = true;
    }
  }
  // ---- 低压保护 ----
  // 3.30V 以下 LDO 只能出 3.24V，ESP32 开始不稳；3.15V 直接睡，别把电池放到 2.6V
  static uint8_t lowCnt = 0;
  if (bat.valid && bat.state != BAT_CHG) {
    if (bat.v < 3.15f) {
      // 连续 3 次（约 15 秒）都低、且趋势不在上升，才睡
      if (++lowCnt >= 3 && vDir <= 0) enterLowBatSleep(bat.v);
    } else if (bat.v < 3.35f) {
      lowCnt = 0;
      if (blPct > 12) { blSet(12); Serial.println("[bat] low -> dim 12%"); }
    } else if (blPct < BL_DEFAULT && bat.v > 3.50f) {
      lowCnt = 0;
      blSet(BL_DEFAULT);
    } else {
      lowCnt = 0;
    }
  } else if (bat.state == BAT_CHG && blPct < BL_DEFAULT) {
    blSet(BL_DEFAULT);                 // 插上充电就恢复亮度
  }

  // 电池页重画：充电时 1 秒（脉冲闪），放电时 30 秒（倒计时走字，省电）
  static uint32_t lastBatDraw = 0;
  if (page == P_BAT) {
    uint32_t iv = (bat.state == BAT_CHG) ? 1000UL : 30000UL;
    if (millis() - lastBatDraw > iv) { lastBatDraw = millis(); fullRedraw = true; }
  }

  // 音乐页重画：状态字 / 缓冲条 / 进度都是活的，不刷就停在"进入那一刻"。
  //   整页推一次 ≈ 11.5ms（115KB @80MHz），一秒一次对播放零影响（音频任务优先级更高）。
  static uint32_t lastMusDraw = 0;
  if (page == P_MUSIC && millis() - lastMusDraw > 1000UL) {
    lastMusDraw = millis();
    fullRedraw = true;
  }

  // 曲名跑马灯：40ms 一步，只推 240×18 那一条（约 8.6KB），不整屏重绘。
  //   整屏推一次 11.5ms，25fps 跑不起；一条 8.6KB 只要 ~0.9ms，随便跑。
  static uint32_t lastMq = 0;
  if (page == P_MUSIC && musScroll && millis() - lastMq >= MQ_STEP_MS) {
    lastMq = millis();
    musicMarquee();
  }

  // 音量落 NVS：改完静置 3 秒才写（连发调音会一秒改好几档，别把 flash 写穿）
  if (volDirty && millis() - volSaveMs > 3000UL) {
    volDirty = false;
    Preferences pf;
    if (pf.begin("ui", false)) { pf.putUChar("vol", spkVol); pf.end(); }
  }

  // 天气：成功则 10 分钟一次，失败 60 秒后重试
  // ★ 播 GIF 时一律不刷（page != P_GIF）：fetchWeather / fetchAir 是阻塞的，
  //   成功要 0.5~1.5 秒，失败要等满超时 —— 那就是"隔一会卡一下"最狠的一刀。
  //   攒着不刷，一离开 GIF 页立刻补上（lastWxMs 没更新，条件自然还是 true）。
  // ★★ 另外：最近 2 秒翻过页就先不刷。人在按键的时候，任何一次联网等待都只会让人
  //   觉得"按键又没反应"；等他不按了再刷，10 分钟的数据晚几秒完全无所谓。
  uint32_t now = millis();
  bool userActive = (lastFlipAtMs && (now - lastFlipAtMs) < 2000UL);
  if (wifiUp() && !userActive) {
    if (page != P_GIF && (wx.ok ? (now - lastWxMs > 600000UL) : (wxRetryMs && now > wxRetryMs))) {
      lastWxMs = now;
      wxRetryMs = wx.ok ? 0 : now + 60000UL;
      fetchWeather();
      if (page != P_GIF) fullRedraw = true;    // 数据变了，当前页整页重画
    }
  }

  // WiFi 掉线：20 秒重连一次，不阻塞显示
  static uint32_t lastRetry = 0;
  if (!wifiUp() && millis() - lastRetry > 20000UL) {
    lastRetry = millis();
    Serial.println("[wifi] reconnect...");
    WiFi.reconnect();
  }
  // 联网恢复但时钟还没对 → 补一次 NTP
  static uint32_t lastNtp = 0;
  if (wifiUp() && !have && page != P_GIF && millis() - lastNtp > 15000UL) {
    lastNtp = millis();
    ntpSync(3000);
    if (page != P_GIF) fullRedraw = true;
  }

  delay(page == P_GIF ? 1 : 20);
}
