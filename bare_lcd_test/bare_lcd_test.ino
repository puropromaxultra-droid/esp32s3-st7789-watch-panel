// ============================================================
// 裸屏点亮测试 —— 老王手表屏(ST7789 240x240) 从 CN1 拔出后
// 经 FPC 转接板直挂 ESP32-S3。零第三方库，SPI.h 是 Arduino 自带。
//
// 接线（CN1 脚号 -> ESP32-S3，均在其开发板右列）：
//   24 LED+  -> 3V3        22 GND   -> GND
//   23 LED-  -> GND        21 VCC   -> 3V3
//   20 IOVCC -> 3V3        19 IM1/2 -> IO9
//   18 RST   -> IO8        17 CS    -> IO6
//   16 SCK   -> IO4        15 DC    -> IO7
//   14 RD    -> 3V3        13 SDA   -> IO5
//   12~5 DB0..DB7、4 TE、3 NC、25/26 -> 悬空
//   2、1 GND -> GND
//
// 程序自动轮换 IM1/2 = 高/低 两种电平（接口模式选择脚，
// 原板上由固件控制，我们直接用 GPIO 两种都试）。
// 串口 115200 打印当前阶段；屏幕出颜色/条纹即命中。
// ============================================================

#include <Arduino.h>
#include <SPI.h>

#define PIN_SCK  4
#define PIN_MOSI 5
#define PIN_CS   6
#define PIN_DC   7
#define PIN_RST  8
#define PIN_IM   9

SPISettings spiCfg(10000000UL, MSBFIRST, SPI_MODE0);

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

// ---- 基础绘制 ----
static void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  uint8_t ca[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
  uint8_t ra[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
  tCmd(0x2A); tData(ca, 4);
  tCmd(0x2B); tData(ra, 4);
  tCmd(0x2C);
}

static void fillColor(uint16_t color, uint32_t ms) {
  uint32_t start = millis();
  setWindow(0, 0, 239, 239);
  uint8_t hi = color >> 8, lo = color & 0xFF;
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  for (uint32_t i = 0; i < 240UL * 240UL; i++) {
    SPI.transfer(hi);
    SPI.transfer(lo);
  }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
  while (millis() - start < ms) delay(10);
}

static void colorBars(uint32_t ms) {
  uint32_t start = millis();
  uint16_t cols[8] = { 0xF800, 0xFC00, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF };
  setWindow(0, 0, 239, 239);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  for (uint16_t y = 0; y < 240; y++) {
    for (uint16_t x = 0; x < 240; x++) {
      uint16_t c = cols[(x * 8) / 240];
      SPI.transfer(c >> 8);
      SPI.transfer(c & 0xFF);
    }
  }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
  while (millis() - start < ms) delay(10);
}

// ---- 初始化（ST7789 通用序列）----
static void lcdInit() {
  // 硬复位
  digitalWrite(PIN_RST, LOW);  delay(30);
  digitalWrite(PIN_RST, HIGH); delay(150);

  tCmd(0x01); delay(150);            // SWRESET
  tCmd(0x11); delay(120);            // SLPOUT
  tCmd1(0x3A, 0x55); delay(10);      // COLMOD 16bit
  tCmd1(0x36, 0x00);                 // MADCTL
  tCmd(0x21); delay(10);             // INVON (IPS 屏)
  tCmd(0x13); delay(10);             // NORON
  tCmd(0x29); delay(50);             // DISPON
}

static void runPhase(bool imHigh) {
  Serial.println();
  Serial.print("===== 阶段: IM1/2 = ");
  Serial.print(imHigh ? "HIGH" : "LOW");
  Serial.println(" —— 盯屏幕 10 秒 =====");

  digitalWrite(PIN_IM, imHigh ? HIGH : LOW);
  delay(5);
  lcdInit();

  fillColor(0xF800, 800);  Serial.println("  red");
  fillColor(0x07E0, 800);  Serial.println("  green");
  fillColor(0x001F, 800);  Serial.println("  blue");
  fillColor(0xFFFF, 800);  Serial.println("  white");
  colorBars(2000);         Serial.println("  color bars");
  fillColor(0x0000, 600);
  Serial.println("----- 阶段结束 -----");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("裸屏测试启动: ST7789 240x240, SPI 10MHz");
  Serial.println("屏幕有反应 -> 报告串口当前的 IM=HIGH/LOW 阶段");

  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_IM, OUTPUT);  digitalWrite(PIN_IM, LOW);
  SPI.begin(PIN_SCK, -1, PIN_MOSI);
}

void loop() {
  runPhase(true);   // IM 高电平
  runPhase(false);  // IM 低电平
}
