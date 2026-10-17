// ============================================================
// 探针2：验证"屏幕初始化没吃进去"——同一固件里初始化两次
// 关键条件：include WiFi.h（复现黑屏条件），但不调用 WiFi
//   1) 开机立即 lcdInit + 红块 3 秒
//   2) 再 lcdInit 一次 + 绿块 3 秒
//   3) 第三次 lcdInit + 白块 3 秒（最亮，肉眼最好判断）
// 哪一次开始出颜色，就说明初始化需要重试/延时。
// ============================================================

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>

#define PIN_SCK  4
#define PIN_MOSI 5
#define PIN_CS   6
#define PIN_DC   7
#define PIN_RST  8
#define PIN_IM   9
#define W 240
#define H 240

SPISettings spiCfg(10000000UL, MSBFIRST, SPI_MODE0);

static void tCmd(uint8_t c) {
  digitalWrite(PIN_DC, LOW); digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg); SPI.transfer(c); SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}
static void tData(const uint8_t *d, uint32_t n) {
  digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  while (n--) SPI.transfer(*d++);
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}
static void tCmd1(uint8_t c, uint8_t d) { tCmd(c); tData(&d, 1); }
static void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  uint8_t ca[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
  uint8_t ra[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
  tCmd(0x2A); tData(ca, 4); tCmd(0x2B); tData(ra, 4); tCmd(0x2C);
}
void lcdInit() {
  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_IM, OUTPUT);  digitalWrite(PIN_IM, LOW);
  SPI.begin(PIN_SCK, -1, PIN_MOSI);
  digitalWrite(PIN_RST, LOW);  delay(60);
  digitalWrite(PIN_RST, HIGH); delay(250);
  tCmd(0x01); delay(200);
  tCmd(0x11); delay(200);
  tCmd1(0x3A, 0x55); delay(20);
  tCmd1(0x36, 0x00);
  tCmd(0x21); delay(20);
  tCmd(0x13); delay(20);
  tCmd(0x29); delay(80);
}
void fillScreen(uint16_t c) {
  setWindow(0, 0, W - 1, H - 1);
  uint8_t hi = c >> 8, lo = c & 0xFF;
  digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  for (uint32_t i = 0; i < (uint32_t)W * H; i++) { SPI.transfer(hi); SPI.transfer(lo); }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== 探针2：双重初始化 ===");

  lcdInit();
  fillScreen(0xF800);
  Serial.println("1st init + RED");
  delay(3000);

  lcdInit();
  fillScreen(0x07E0);
  Serial.println("2nd init + GREEN");
  delay(3000);

  lcdInit();
  fillScreen(0xFFFF);
  Serial.println("3rd init + WHITE (hold 8s)");
  delay(8000);

  fillScreen(0x001F);
  Serial.println("end: BLUE held");
}

void loop() {
  delay(1000);
}
