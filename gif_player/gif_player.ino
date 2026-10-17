// ============================================================
// GIF 动画播放器 —— ST7789 240x240 (老王手表屏直驱)
//
// 不需要任何第三方库：GIF 在电脑上已由 tools/gif2h.py 转成
// RGB565 + RLE 数据，写进 Flash，这里只做解压 + 推屏。
//
// 接线（与 bare_lcd_test 完全相同，不要改动）：
//   SCK=IO4  SDA=IO5  CS=IO6  DC=IO7  RST=IO8  IM=IO9
//   3V3 -> 1/4/5/11(转接板)   GND -> 2/23
//
// 换动画：在电脑上跑
//   python tools/gif2h.py 你的.gif -o gif_player/gif_data.h
// 然后重新编译烧录即可。
// ============================================================

#include <Arduino.h>
#include <SPI.h>

#define PIN_SCK  4
#define PIN_MOSI 5
#define PIN_CS   6
#define PIN_DC   7
#define PIN_RST  8
#define PIN_IM   9

// 10MHz 是飞线接法下实测稳定的上限，不要再往上提
SPISettings spiCfg(10000000UL, MSBFIRST, SPI_MODE0);

#include "gif_data.h"

static uint8_t lineBuf[GIF_W * 2];   // 一行像素（大端字节流）

// ---------------- 底层 SPI ----------------
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

static void lcdInit() {
  digitalWrite(PIN_RST, LOW);  delay(30);
  digitalWrite(PIN_RST, HIGH); delay(150);
  tCmd(0x01); delay(150);            // SWRESET
  tCmd(0x11); delay(120);            // SLPOUT
  tCmd1(0x3A, 0x55); delay(10);      // COLMOD 16bit
  tCmd1(0x36, 0x00);                 // MADCTL
  tCmd(0x21); delay(10);             // INVON（IPS 屏必须开）
  tCmd(0x13); delay(10);             // NORON
  tCmd(0x29); delay(50);             // DISPON
}

static void fillScreen(uint16_t color) {
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
}

// ---------------- RLE 解压 + 单帧显示 ----------------
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
        if (i + 2 > end) {        // 数据提前结束：余下补黑，绝不越界
          color  = 0x0000;
          remain = GIF_W;
          break;
        }
        remain = gifData[i++];
        color  = gifData[i++];
        if (remain == 0) remain = 1;   // 防御：0 长度会死循环
      }
      remain--;
      lineBuf[x * 2]     = (uint8_t)(color >> 8);
      lineBuf[x * 2 + 1] = (uint8_t)(color & 0xFF);
    }
    // 用 bare_lcd_test 验证过的 transfer 逐字节路径（writeBytes 嫌疑待排除）
    for (uint16_t b = 0; b < GIF_W * 2; b++) SPI.transfer(lineBuf[b]);
  }

  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}

// ---------------- 播放 ----------------
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_IM, OUTPUT);  digitalWrite(PIN_IM, LOW);
  SPI.begin(PIN_SCK, -1, PIN_MOSI);

  lcdInit();
  fillScreen(0xF800);           // 开机红闪 600ms：验证"初始化+transfer 路径"是否活着
  delay(600);
  fillScreen(0x0000);

  Serial.printf("GIF player: %dx%d, %d frames\n", GIF_W, GIF_H, GIF_FRAME_COUNT);
  uint32_t total = 0;
  for (uint16_t f = 0; f < GIF_FRAME_COUNT; f++) total += gifFrameOffset[f + 1] - gifFrameOffset[f];
  Serial.printf("RLE words: %lu (%.1f KB)\n", (unsigned long)total, total * 2 / 1024.0);
}

void loop() {
  static uint32_t loops = 0;
  uint32_t tAll = millis();

  for (uint16_t f = 0; f < GIF_FRAME_COUNT; f++) {
    uint32_t t0 = millis();
    showFrame(f);

    // 严格等够本帧时长；若绘制已超时则立即进下一帧
    while (millis() - t0 < gifDelayMs[f]) { }

    if (f == 0) {
      Serial.printf("frame0 draw %lu ms\n", (unsigned long)(millis() - t0));
    }
  }

  loops++;
  if (loops % 5 == 0) {
    Serial.printf("loop %lu done in %lu ms\n", (unsigned long)loops,
                  (unsigned long)(millis() - tAll));
  }
}
