// ============================================================
// 膨胀实验 —— 验证"固件变大就黑屏"假说
//
// 代码与 bare_lcd_test 100% 相同（唯一验证过能亮的程序），
// 只额外塞入 PAD_KB 大小的无用常量数据，把固件撑大。
//
// 判读：
//   能亮  -> 与固件大小无关，硬件正常，问题在 gif_player 的具体代码
//   不亮  -> 固件大小/链接布局假说成立，下一步查编译选项与 flash 布局
//
// 改 PAD_KB 可以二分出阈值（先 60，再 120 / 30 / 15）。
// ============================================================

#include <Arduino.h>
#include <SPI.h>

#define PAD_KB 130

// 无害填充数据：放 flash 的 .rodata，只占空间不参与逻辑
// 实体在 pad.c（C 编译单元，可用 GNU 范围初始化器保证真实占位）
extern "C" const uint8_t padData[];

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

static void runPhase(bool imHigh) {
  Serial.print("===== IM1/2 = ");
  Serial.print(imHigh ? "HIGH" : "LOW");
  Serial.println(" =====");

  digitalWrite(PIN_IM, imHigh ? HIGH : LOW);
  delay(5);
  lcdInit();

  fillColor(0xF800, 800);  Serial.println("  red");
  fillColor(0x07E0, 800);  Serial.println("  green");
  fillColor(0x001F, 800);  Serial.println("  blue");
  fillColor(0xFFFF, 800);  Serial.println("  white");
  colorBars(2000);         Serial.println("  color bars");
  fillColor(0x0000, 600);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("PAD test: padData=%u bytes (first=%02X), firmware bigger than bare\n",
                (unsigned)(PAD_KB * 1024), (unsigned)padData[0]);

  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_IM, OUTPUT);  digitalWrite(PIN_IM, LOW);
  SPI.begin(PIN_SCK, -1, PIN_MOSI);
}

void loop() {
  runPhase(true);
  runPhase(false);
}
