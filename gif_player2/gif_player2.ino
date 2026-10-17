// ============================================================
// gif_player2 —— 控制变量版 GIF 播放器
//
// 结构完全照搬 bare_pad_test（已实机验证能亮），只做两处改动：
//   1. 引入 gif_data.h（60KB 常量数据，与 gif_player 相同）
//   2. loop 里播放 GIF
//
// 上电自检：红(700ms) → 绿(700ms) → 蓝(400ms) → 播放动画
// 判读：
//   红绿蓝都出、动画黑  -> 问题在 showFrame/GIF 数据
//   红绿蓝都不出        -> 问题在"引入 gif_data.h 这份数据"本身
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

static void lcdInit() {
  digitalWrite(PIN_RST, LOW);  delay(30);
  digitalWrite(PIN_RST, HIGH); delay(150);
  tCmd(0x01); delay(150);
  tCmd(0x11); delay(120);
  tCmd1(0x3A, 0x55); delay(10);
  tCmd1(0x36, 0x00);
  tCmd(0x21); delay(10);
  tCmd(0x13); delay(10);
  tCmd(0x29); delay(50);
}

static void showFrame(uint16_t f) {
  uint32_t i   = gifFrameOffset[f];
  uint32_t end = gifFrameOffset[f + 1];

  uint16_t remain = 0;
  uint16_t color  = 0;

  setWindow(0, 0, GIF_W - 1, GIF_H - 1);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);

  for (uint16_t y = 0; y < GIF_H; y++) {
    for (uint16_t x = 0; x < GIF_W; x++) {
      while (remain == 0) {
        if (i + 2 > end) {
          color  = 0x0000;
          remain = GIF_W;
          break;
        }
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
  delay(300);

  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_IM, OUTPUT);  digitalWrite(PIN_IM, LOW);
  SPI.begin(PIN_SCK, -1, PIN_MOSI);

  Serial.printf("v2 boot, gifData[0]=%04X frames=%d\n",
                (unsigned)gifData[0], GIF_FRAME_COUNT);

  lcdInit();

  Serial.println("self-check: RED");
  fillColor(0xF800, 700);
  Serial.println("self-check: GREEN");
  fillColor(0x07E0, 700);
  Serial.println("self-check: BLUE");
  fillColor(0x001F, 400);
  Serial.println("self-check done, play");
}

void loop() {
  for (uint16_t f = 0; f < GIF_FRAME_COUNT; f++) {
    uint32_t t0 = millis();
    showFrame(f);
    while (millis() - t0 < gifDelayMs[f]) { }
  }
}
