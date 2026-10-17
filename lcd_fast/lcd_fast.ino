// ============================================================
// lcd_fast —— 手表屏 高速版  v4
//
// 相对 lcd_player v3 只改传输层，不改画面：
//   1) 每字节一次 SPI.transfer()  ->  整批 SPI.writeBytes()
//      （逐字节调用每次都要等上一次传输完成，是最大的瓶颈）
//   2) 整帧期间 CS 一直拉低，0x2C 只发一次，窗口在 init 时设好
//   3) 频率可运行时切换：串口发 1/2/3/4/5 = 10/26/40/60/80 MHz
//      默认 40MHz。花屏 = 线撑不住，发 2 降到 26MHz 再试。
//   4) 每秒打印一次 fps 和单帧耗时，用数字判断而不是"感觉"
//
// 接线不变：SCK=4 MOSI=5 CS=6 DC=7 RST=8 IM=9（IM 必须高）
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

// 一批刷多少行；16 行 = 7680 字节，够大能摊平调用开销，又不占多少内存
#define ROWS_PER_BATCH 16

static uint32_t    spiHz = 26000000UL;
static SPISettings spiCfg(26000000UL, MSBFIRST, SPI_MODE0);

static uint8_t batchBuf[GIF_W * 2 * ROWS_PER_BATCH];

// ---------------- 底层 ----------------
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

// ---------------- 初始化（原厂序列，不变） ----------------
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

  tCmd(0x21);                     // INVON
  delay(10);
  tCmd(0x11);                     // SLPOUT
  delay(120);
  tCmd(0x29);                     // DISPON
  delay(50);

  // 窗口设成全屏，之后每帧只发 0x2C，不再重设
  setWindow(0, 0, GIF_W - 1, GIF_H - 1);
}

// ---------------- 满屏纯色 ----------------
static void fillColor(uint16_t color, uint32_t ms) {
  uint8_t hi = color >> 8, lo = color & 0xFF;
  for (uint16_t i = 0; i < GIF_W * ROWS_PER_BATCH; i++) {
    batchBuf[i * 2]     = hi;
    batchBuf[i * 2 + 1] = lo;
  }

  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  SPI.transfer(0x2C);
  digitalWrite(PIN_DC, HIGH);
  for (uint16_t y = 0; y < GIF_H; y += ROWS_PER_BATCH) {
    uint16_t rows = (GIF_H - y) < ROWS_PER_BATCH ? (GIF_H - y) : ROWS_PER_BATCH;
    SPI.writeBytes(batchBuf, (size_t)rows * GIF_W * 2);
  }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);

  uint32_t start = millis();
  while (millis() - start < ms) { delay(10); }
}

// ---------------- 彩条 ----------------
static void colorBars(uint32_t ms) {
  static const uint16_t cols[8] = {
    0xF800, 0xFC00, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF
  };
  // 一行 8 段，先算好一行，后面整批复用
  for (uint16_t x = 0; x < GIF_W; x++) {
    uint16_t c = cols[(x * 8) / GIF_W];
    batchBuf[x * 2]     = c >> 8;
    batchBuf[x * 2 + 1] = c & 0xFF;
  }
  // 把一行复制到整批
  for (uint16_t r = 1; r < ROWS_PER_BATCH; r++) {
    memcpy(batchBuf + (size_t)r * GIF_W * 2, batchBuf, GIF_W * 2);
  }

  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  SPI.transfer(0x2C);
  digitalWrite(PIN_DC, HIGH);
  for (uint16_t y = 0; y < GIF_H; y += ROWS_PER_BATCH) {
    uint16_t rows = (GIF_H - y) < ROWS_PER_BATCH ? (GIF_H - y) : ROWS_PER_BATCH;
    SPI.writeBytes(batchBuf, (size_t)rows * GIF_W * 2);
  }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);

  uint32_t start = millis();
  while (millis() - start < ms) { delay(10); }
}

// ---------------- RLE 解一帧并刷屏 ----------------
static void showFrame(uint16_t f) {
  uint32_t i   = gifFrameOffset[f];
  uint32_t end = gifFrameOffset[f + 1];
  uint16_t remain = 0, color = 0;

  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  SPI.transfer(0x2C);              // RAMWR，一次性把这一帧灌完
  digitalWrite(PIN_DC, HIGH);

  for (uint16_t y0 = 0; y0 < GIF_H; y0 += ROWS_PER_BATCH) {
    uint16_t rows = (GIF_H - y0) < ROWS_PER_BATCH ? (GIF_H - y0) : ROWS_PER_BATCH;
    uint8_t *p = batchBuf;

    for (uint16_t n = 0; n < (uint16_t)rows * GIF_W; n++) {
      if (remain == 0) {
        if (i + 2 > end) { color = 0x0000; remain = 1; }
        else {
          remain = gifData[i++];
          color  = gifData[i++];
          if (remain == 0) remain = 1;
        }
      }
      remain--;
      *p++ = (uint8_t)(color >> 8);
      *p++ = (uint8_t)(color & 0xFF);
    }

    SPI.writeBytes(batchBuf, (size_t)rows * GIF_W * 2);
  }

  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}

// ---------------- 串口切频率 ----------------
static void handleSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    uint32_t hz = 0;
    switch (c) {
      case '1': hz = 10000000UL; break;
      case '2': hz = 26000000UL; break;
      case '3': hz = 40000000UL; break;
      case '4': hz = 60000000UL; break;
      case '5': hz = 80000000UL; break;
      default: continue;
    }
    spiHz  = hz;
    spiCfg = SPISettings(hz, MSBFIRST, SPI_MODE0);
    Serial.printf(">> SPI = %lu Hz\n", (unsigned long)hz);
  }
}

// ---------------- 帧率统计 ----------------
static uint32_t statFrames = 0;
static uint32_t statStart  = 0;
static uint32_t statLastMs = 0;

static void statTick() {
  statFrames++;
  uint32_t now = millis();
  if (now - statStart >= 1000) {
    float sec = (now - statStart) / 1000.0f;
    Serial.printf("fps=%.1f  frame=%lums  spi=%luMHz  heap=%u\n",
                  statFrames / sec,
                  (unsigned long)statLastMs,
                  (unsigned long)(spiHz / 1000000UL),
                  (unsigned)ESP.getFreeHeap());
    statFrames = 0;
    statStart  = now;
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println("================================");
  Serial.print("LCDFAST v4  build ");
  Serial.print(__DATE__);
  Serial.print(" ");
  Serial.println(__TIME__);
  Serial.println("SPI 26MHz default | serial 1/2/3/4/5 = 10/26/40/60/80MHz");
  Serial.println("================================");

  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_IM, OUTPUT);

  digitalWrite(PIN_IM, HIGH);   // ★ SPI 模式，不能省
  delay(10);

  SPI.begin(PIN_SCK, -1, PIN_MOSI);

  lcdInit();

  Serial.println("self-check RED");
  fillColor(0xF800, 500);
  Serial.println("self-check GREEN");
  fillColor(0x07E0, 500);
  Serial.println("self-check BLUE");
  fillColor(0x001F, 500);
  Serial.println("self-check WHITE");
  fillColor(0xFFFF, 500);
  Serial.println("self-check BARS");
  colorBars(1200);
  Serial.println("self-check done -> GIF");

  statStart = millis();
}

void loop() {
  for (uint16_t f = 0; f < GIF_FRAME_COUNT; f++) {
    uint32_t t0 = millis();
    showFrame(f);
    statLastMs = millis() - t0;
    statTick();

    while (millis() - t0 < gifDelayMs[f]) {
      handleSerial();
      delay(1);
    }
  }
}
