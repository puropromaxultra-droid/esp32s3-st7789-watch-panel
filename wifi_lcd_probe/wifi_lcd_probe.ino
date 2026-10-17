// ============================================================
// 分步探针：找出"屏 + WiFi"互相干扰的环节
// A 纯屏（WiFi 未启动）→ 红
// B 纯屏第二遍       → 绿
// C 启动 WiFi 后     → 蓝（连上）/ 紫（失败）
// D 最终状态         → 白字 + 内存余量
// 每步串口也打点。哪一步之后屏黑了，凶手就是它。
// ============================================================

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

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
void fillScreen(uint16_t c) {
  setWindow(0, 0, W - 1, H - 1);
  uint8_t hi = c >> 8, lo = c & 0xFF;
  digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  for (uint32_t i = 0; i < (uint32_t)W * H; i++) { SPI.transfer(hi); SPI.transfer(lo); }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}
void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  setWindow(x, y, x + w - 1, y + h - 1);
  uint8_t hi = c >> 8, lo = c & 0xFF;
  digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  for (uint32_t i = 0; i < (uint32_t)w * h; i++) { SPI.transfer(hi); SPI.transfer(lo); }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}
static void bigBlock(uint16_t c) { fillRect(40, 60, 160, 120, c); }

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== LCD+WiFi 探针 ===");
  lcdInit();
  Serial.printf("A: lcdInit done, heap=%d\n", ESP.getFreeHeap());

  fillScreen(0x0000);
  bigBlock(0xF800);                    // 红块，WiFi 未启动
  Serial.println("A: RED block drawn (WiFi off)");
  delay(3000);

  bigBlock(0x07E0);                    // 换成绿块
  Serial.println("B: GREEN block drawn (still WiFi off)");
  delay(3000);

  Serial.println("C: starting WiFi...");
  Serial.printf("C: heap before wifi=%d\n", ESP.getFreeHeap());
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print("."); }
  Serial.printf("\nC: wifi status=%d heap=%d\n", (int)WiFi.status(), ESP.getFreeHeap());

  // WiFi 起来后立刻再画：蓝=连上，紫=没连上
  bigBlock(WiFi.status() == WL_CONNECTED ? 0x001F : 0xF81F);
  Serial.println("C: BLUE(branch) block drawn after WiFi start");
  delay(3000);

  Serial.println("D: end of probe, loop idle");
}

void loop() {
  delay(1000);
}
