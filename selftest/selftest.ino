// ESP32-S3 一体板 V5 · 屏幕最小自检
// 只做一件事：初始化 ST7789 + 点背光 + 刷四色 + 彩条
// 判据：
//   背光不亮            -> IO13 / Q1 / R4 / 屏供电
//   背光亮但全黑         -> SPI 某根线不通（MOSI/SCK/CS/DC/RST）
//   有颜色               -> 全通
#include <Arduino.h>
#include <SPI.h>

#define W 240
#define H 240

#define PIN_SCK  12   // U1.16
#define PIN_MOSI 11   // U1.15
#define PIN_CS   10   // U1.14
#define PIN_DC    9   // U1.13
#define PIN_RST  14   // U1.18
#define PIN_BL   13   // U1.17 -> Q1 栅极，HIGH = 背光亮

static SPISettings cfg(26000000UL, MSBFIRST, SPI_MODE0);
static uint16_t lineBuf[W];

static void tCmd(uint8_t c) {
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(cfg);
  SPI.transfer(c);
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}
static void tData(const uint8_t *d, uint32_t n) {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(cfg);
  SPI.writeBytes(d, n);
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}
static void tCmd1(uint8_t c, uint8_t v) { tCmd(c); tData(&v, 1); }
static void tCmdN(uint8_t c, const uint8_t *d, uint32_t n) { tCmd(c); tData(d, n); }

static void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  uint8_t ca[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
  uint8_t ra[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
  tCmd(0x2A); tData(ca, 4);
  tCmd(0x2B); tData(ra, 4);
}

static void lcdInit() {
  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BL, OUTPUT);

  // ★ 先把背光点亮：屏此时应是"发灰的空白"，这一步就能单独证明背光通路
  digitalWrite(PIN_BL, HIGH);
  delay(400);

  SPI.begin(PIN_SCK, -1, PIN_MOSI);

  digitalWrite(PIN_RST, LOW);  delay(100);
  digitalWrite(PIN_RST, HIGH); delay(200);

  tCmd1(0x36, 0x00);                 // MADCTL
  tCmd1(0x3A, 0x55);                 // COLMOD 16bit
  const uint8_t b2[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
  tCmdN(0xB2, b2, 5);
  tCmd1(0xB7, 0x35);
  tCmd1(0xBB, 0x19);
  tCmd1(0xC0, 0x2C);
  tCmd1(0xC2, 0x01);
  tCmd1(0xC3, 0x12);
  tCmd1(0xC4, 0x20);
  tCmd1(0xC6, 0x0F);
  const uint8_t d0[] = { 0xA4, 0xA1 };
  tCmdN(0xD0, d0, 2);
  const uint8_t ep[] = { 0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,0x4C,0x18,0x0D,0x0B,0x1F,0x23 };
  tCmdN(0xE0, ep, 14);
  const uint8_t en[] = { 0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,0x51,0x2F,0x1F,0x1F,0x20,0x23 };
  tCmdN(0xE1, en, 14);

  tCmd(0x21); delay(10);             // INVON
  tCmd(0x11); delay(120);            // SLPOUT
  tCmd(0x29); delay(50);             // DISPON
}

static void fillScreen(uint16_t c) {
  for (int i = 0; i < W; i++) lineBuf[i] = c;
  setWindow(0, 0, W - 1, H - 1);
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(cfg);
  SPI.transfer(0x2C);                // RAMWR 只发一次
  digitalWrite(PIN_DC, HIGH);
  for (int y = 0; y < H; y++) SPI.writeBytes((uint8_t *)lineBuf, W * 2);
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}

static void bars() {
  const uint16_t col[8] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xA81F, 0xF800, 0x001F, 0x0000 };
  for (int x = 0; x < W; x++) lineBuf[x] = col[(x * 8) / W];
  setWindow(0, 0, W - 1, H - 1);
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(cfg);
  SPI.transfer(0x2C);
  digitalWrite(PIN_DC, HIGH);
  for (int y = 0; y < H; y++) SPI.writeBytes((uint8_t *)lineBuf, W * 2);
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}

void setup() {
  lcdInit();
}

void loop() {
  fillScreen(0xFFFF); delay(1200);   // 白
  fillScreen(0xF800); delay(1200);   // 红
  fillScreen(0x07E0); delay(1200);   // 绿
  fillScreen(0x001F); delay(1200);   // 蓝
  bars();             delay(3000);   // 彩条
}
