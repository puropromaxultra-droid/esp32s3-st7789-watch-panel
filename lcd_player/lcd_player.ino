// ============================================================
// lcd_player —— 手表屏 GIF/彩条播放器  v3
//
// 关键修正（来自原厂开源工程 BA4IHS/GD32E230-LaoWang）：
//   TFT_LCD_IM12(1);   // IM = 高电平 才是 SPI 模式
//   之前的程序把 IM 拉低 -> 初始化不生效 -> 全程黑屏。
//
// 初始化序列也照原厂 LCD_Chip_Init 补齐：
//   0x36 0x00, 0x3A 0x55, 0xB2, 0xB7, 0xBB, 0xC0, 0xC2,
//   0xC3, 0xC4, 0xC6, 0xD0, 0xE0, 0xE1, 0x21, 0x11, 0x29
// 复位时序：RST 低 100ms -> 高 200ms（原厂值）
//
// 上电顺序：红 -> 绿 -> 蓝 -> 白 -> 彩条 -> GIF 循环
// 串口 115200 会打印构建时间，可确认烧的是不是这一版
// ============================================================

#include <Arduino.h>
#include <SPI.h>

#include "gif_data.h"

#define PIN_SCK  4
#define PIN_MOSI 5
#define PIN_CS   6
#define PIN_DC   7
#define PIN_RST  8
#define PIN_IM   9

SPISettings spiCfg(10000000UL, MSBFIRST, SPI_MODE0);

static uint8_t lineBuf[GIF_W * 2];

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
  while (n--) SPI.transfer(*d++);
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
  tCmd(0x2C);
}

static void fillColor(uint16_t color, uint32_t ms) {
  uint32_t start = millis();
  setWindow(0, 0, GIF_W - 1, GIF_H - 1);
  uint8_t hi = color >> 8, lo = color & 0xFF;
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  for (uint32_t i = 0; i < (uint32_t)GIF_W * GIF_H; i++) {
    SPI.transfer(hi);
    SPI.transfer(lo);
  }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
  while (millis() - start < ms) delay(10);
}

// 原厂 LCD_Chip_Init 完整序列
static void lcdInit() {
  digitalWrite(PIN_RST, LOW);  delay(100);
  digitalWrite(PIN_RST, HIGH); delay(200);

  tCmd1(0x36, 0x00);              // MADCTL
  tCmd1(0x3A, 0x55);              // COLMOD 16bit

  const uint8_t b2[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
  tCmdN(0xB2, b2, 5);             // PORCTRL
  tCmd1(0xB7, 0x35);              // GCTRL
  tCmd1(0xBB, 0x19);              // VCOMS
  tCmd1(0xC0, 0x2C);              // LCMCTRL
  tCmd1(0xC2, 0x01);              // VDVVRHEN
  tCmd1(0xC3, 0x12);              // VRHS
  tCmd1(0xC4, 0x20);              // VDVS
  tCmd1(0xC6, 0x0F);              // FRCTRL2
  const uint8_t d0[] = { 0xA4, 0xA1 };
  tCmdN(0xD0, d0, 2);             // PWCTRL1

  const uint8_t ep[] = {          // 正伽马
    0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
    0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23
  };
  tCmdN(0xE0, ep, 14);
  const uint8_t en[] = {          // 负伽马
    0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
    0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23
  };
  tCmdN(0xE1, en, 14);

  tCmd(0x21);                     // INVON（IPS 屏必须）
  delay(10);
  tCmd(0x11);                     // SLPOUT
  delay(120);
  tCmd(0x29);                     // DISPON
  delay(50);
}

static void colorBars(uint32_t ms) {
  uint16_t cols[8] = { 0xF800, 0xFC00, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF };
  uint32_t start = millis();
  setWindow(0, 0, GIF_W - 1, GIF_H - 1);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  for (uint16_t y = 0; y < GIF_H; y++) {
    for (uint16_t x = 0; x < GIF_W; x++) {
      uint16_t c = cols[(x * 8) / GIF_W];
      SPI.transfer(c >> 8);
      SPI.transfer(c & 0xFF);
    }
  }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
  while (millis() - start < ms) delay(10);
}

static void showFrame(uint16_t f) {
  uint32_t i   = gifFrameOffset[f];
  uint32_t end = gifFrameOffset[f + 1];
  uint16_t remain = 0, color = 0;

  setWindow(0, 0, GIF_W - 1, GIF_H - 1);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);

  for (uint16_t y = 0; y < GIF_H; y++) {
    for (uint16_t x = 0; x < GIF_W; x++) {
      while (remain == 0) {
        if (i + 2 > end) { color = 0x0000; remain = GIF_W; break; }
        remain = gifData[i++];
        color  = gifData[i++];
        if (remain == 0) remain = 1;
      }
      remain--;
      lineBuf[x * 2]     = (uint8_t)(color >> 8);
      lineBuf[x * 2 + 1] = (uint8_t)(color & 0xFF);
    }
    for (uint16_t b = 0; b < GIF_W * 2; b++) SPI.transfer(lineBuf[b]);
  }

  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}

void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println("================================");
  Serial.print("LCDPLAYER v3  build ");
  Serial.print(__DATE__);
  Serial.print(" ");
  Serial.println(__TIME__);
  Serial.println("IM = HIGH (SPI mode, per vendor lib)");
  Serial.println("================================");

  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_IM, OUTPUT);

  // ★ 核心修正：IM 必须为高 —— SPI 模式
  digitalWrite(PIN_IM, HIGH);
  delay(10);

  SPI.begin(PIN_SCK, -1, PIN_MOSI);

  lcdInit();

  Serial.println("self-check RED");
  fillColor(0xF800, 800);
  Serial.println("self-check GREEN");
  fillColor(0x07E0, 800);
  Serial.println("self-check BLUE");
  fillColor(0x001F, 800);
  Serial.println("self-check WHITE");
  fillColor(0xFFFF, 800);
  Serial.println("self-check BARS");
  colorBars(1500);
  Serial.println("self-check done -> GIF");
}

void loop() {
  static uint32_t frames = 0;
  for (uint16_t f = 0; f < GIF_FRAME_COUNT; f++) {
    uint32_t t0 = millis();
    showFrame(f);
    while (millis() - t0 < gifDelayMs[f]) { }
    frames++;
  }
  Serial.printf("played %lu frames, heap=%u\n", (unsigned long)frames, (unsigned)ESP.getFreeHeap());
}
