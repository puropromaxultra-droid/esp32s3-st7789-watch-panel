// ============================================================
// 手表屏正式显示库 + 演示 —— ST7789 240x240 SPI
// 硬件已验证（2026-10-17 点亮）：镜像接线，IM 电平无关，IPS 需 INVON。
//
// 接线（转接板排针 -> ESP32-S3 丝印）：
//   3V3 -> 1(LED+) / 4(VCC) / 5(IOVCC) / 11(RD)
//   GND -> 2(LED-) / 23(逻辑地)
//   IO4 -> 9(SCK)   IO5 -> 12(SDA)   IO6 -> 8(CS)
//   IO7 -> 10(DC)   IO8 -> 7(RST)    IO9 -> 6(IM, 电平随意)
//
// 提供 API：
//   lcdInit()                     初始化
//   fillScreen(color)             整屏填色
//   drawPixel(x,y,c)              画点
//   drawLine(x0,y0,x1,y1,c)       画线
//   drawRect / fillRect           矩形
//   drawCircle / fillCircle       圆
//   drawString(str,x,y,c,scale)   文字（scale=1 约 6x8 像素/字符）
//   drawInt(n,x,y,c,scale)        整数
// 颜色：RGB565，如 0xF800红 0x07E0绿 0x001F蓝 0xFFFF白 0x0000黑
// ============================================================

#include <Arduino.h>
#include <SPI.h>

#define PIN_SCK  4
#define PIN_MOSI 5
#define PIN_CS   6
#define PIN_DC   7
#define PIN_RST  8
#define PIN_IM   9

#define W 240
#define H 240

SPISettings spiCfg(20000000UL, MSBFIRST, SPI_MODE0);

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

void lcdInit() {
  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_IM, OUTPUT);  digitalWrite(PIN_IM, LOW);   // 电平已验证无关
  SPI.begin(PIN_SCK, -1, PIN_MOSI);
  digitalWrite(PIN_RST, LOW);  delay(30);
  digitalWrite(PIN_RST, HIGH); delay(150);
  tCmd(0x01); delay(150);            // SWRESET
  tCmd(0x11); delay(120);            // SLPOUT
  tCmd1(0x3A, 0x55); delay(10);      // COLMOD 16bit
  tCmd1(0x36, 0x00);                 // MADCTL 正常方向
  tCmd(0x21); delay(10);             // INVON（IPS 屏，已验证必须开）
  tCmd(0x13); delay(10);             // NORON
  tCmd(0x29); delay(50);             // DISPON
}

// ---------------- 绘图 API ----------------
void fillScreen(uint16_t c) {
  setWindow(0, 0, W - 1, H - 1);
  uint8_t hi = c >> 8, lo = c & 0xFF;
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  for (uint32_t i = 0; i < (uint32_t)W * H; i++) { SPI.transfer(hi); SPI.transfer(lo); }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}

void drawPixel(int16_t x, int16_t y, uint16_t c) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  setWindow(x, y, x, y);
  uint8_t b[2] = { (uint8_t)(c >> 8), (uint8_t)c };
  tData(b, 2);
}

void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c) {
  int16_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int16_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int16_t err = dx + dy;
  for (;;) {
    drawPixel(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int16_t e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  drawLine(x, y, x + w - 1, y, c);
  drawLine(x, y + h - 1, x + w - 1, y + h - 1, c);
  drawLine(x, y, x, y + h - 1, c);
  drawLine(x + w - 1, y, x + w - 1, y + h - 1, c);
}

void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  setWindow(x, y, x + w - 1, y + h - 1);
  uint8_t hi = c >> 8, lo = c & 0xFF;
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  for (uint32_t i = 0; i < (uint32_t)w * h; i++) { SPI.transfer(hi); SPI.transfer(lo); }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}

void drawCircle(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  int16_t x = r, y = 0, e = 1 - r;
  while (x >= y) {
    drawPixel(cx + x, cy + y, c); drawPixel(cx - x, cy + y, c);
    drawPixel(cx + y, cy + x, c); drawPixel(cx - y, cy + x, c);
    drawPixel(cx - x, cy - y, c); drawPixel(cx + x, cy - y, c);
    drawPixel(cx - y, cy - x, c); drawPixel(cx + y, cy - x, c);
    y++;
    if (e < 0) e += 2 * y + 1;
    else { x--; e += 2 * (y - x) + 1; }
  }
}

void fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  for (int16_t y = -r; y <= r; y++) {
    int16_t dx = (int16_t)sqrtf((float)(r * r - y * y));
    drawLine(cx - dx, cy + y, cx + dx, cy + y, c);
  }
}

// ---------------- 5x7 字库（ASCII 32~126）----------------
static const uint8_t font5x7[][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00}, // space
  {0x00,0x00,0x5F,0x00,0x00}, // !
  {0x00,0x07,0x00,0x07,0x00}, // "
  {0x14,0x7F,0x14,0x7F,0x14}, // #
  {0x24,0x2A,0x7F,0x2A,0x12}, // $
  {0x23,0x13,0x08,0x64,0x62}, // %
  {0x36,0x49,0x55,0x22,0x50}, // &
  {0x00,0x05,0x03,0x00,0x00}, // '
  {0x00,0x1C,0x22,0x41,0x00}, // (
  {0x00,0x41,0x22,0x1C,0x00}, // )
  {0x14,0x08,0x3E,0x08,0x14}, // *
  {0x08,0x08,0x3E,0x08,0x08}, // +
  {0x00,0x50,0x30,0x00,0x00}, // ,
  {0x08,0x08,0x08,0x08,0x08}, // -
  {0x00,0x60,0x60,0x00,0x00}, // .
  {0x20,0x10,0x08,0x04,0x02}, // /
  {0x3E,0x51,0x49,0x45,0x3E}, // 0
  {0x00,0x42,0x7F,0x40,0x00}, // 1
  {0x42,0x61,0x51,0x49,0x46}, // 2
  {0x21,0x41,0x45,0x4B,0x31}, // 3
  {0x18,0x14,0x12,0x7F,0x10}, // 4
  {0x27,0x45,0x45,0x45,0x39}, // 5
  {0x3C,0x4A,0x49,0x49,0x30}, // 6
  {0x01,0x71,0x09,0x05,0x03}, // 7
  {0x36,0x49,0x49,0x49,0x36}, // 8
  {0x06,0x49,0x49,0x29,0x1E}, // 9
  {0x00,0x36,0x36,0x00,0x00}, // :
  {0x00,0x56,0x36,0x00,0x00}, // ;
  {0x00,0x08,0x14,0x22,0x41}, // <
  {0x14,0x14,0x14,0x14,0x14}, // =
  {0x41,0x22,0x14,0x08,0x00}, // >
  {0x02,0x01,0x51,0x09,0x06}, // ?
  {0x32,0x49,0x79,0x41,0x3E}, // @
  {0x7E,0x11,0x11,0x11,0x7E}, // A
  {0x7F,0x49,0x49,0x49,0x36}, // B
  {0x3E,0x41,0x41,0x41,0x22}, // C
  {0x7F,0x41,0x41,0x22,0x1C}, // D
  {0x7F,0x49,0x49,0x49,0x41}, // E
  {0x7F,0x09,0x09,0x01,0x01}, // F
  {0x3E,0x41,0x41,0x51,0x32}, // G
  {0x7F,0x08,0x08,0x08,0x7F}, // H
  {0x00,0x41,0x7F,0x41,0x00}, // I
  {0x20,0x40,0x41,0x3F,0x01}, // J
  {0x7F,0x08,0x14,0x22,0x41}, // K
  {0x7F,0x40,0x40,0x40,0x40}, // L
  {0x7F,0x02,0x04,0x02,0x7F}, // M
  {0x7F,0x04,0x08,0x10,0x7F}, // N
  {0x3E,0x41,0x41,0x41,0x3E}, // O
  {0x7F,0x09,0x09,0x09,0x06}, // P
  {0x3E,0x41,0x51,0x21,0x5E}, // Q
  {0x7F,0x09,0x19,0x29,0x46}, // R
  {0x46,0x49,0x49,0x49,0x31}, // S
  {0x01,0x01,0x7F,0x01,0x01}, // T
  {0x3F,0x40,0x40,0x40,0x3F}, // U
  {0x1F,0x20,0x40,0x20,0x1F}, // V
  {0x7F,0x20,0x18,0x20,0x7F}, // W
  {0x63,0x14,0x08,0x14,0x63}, // X
  {0x03,0x04,0x78,0x04,0x03}, // Y
  {0x61,0x51,0x49,0x45,0x43}, // Z
  {0x00,0x00,0x7F,0x41,0x41}, // [
  {0x02,0x04,0x08,0x10,0x20}, // backslash
  {0x41,0x41,0x7F,0x00,0x00}, // ]
  {0x04,0x02,0x01,0x02,0x04}, // ^
  {0x40,0x40,0x40,0x40,0x40}, // _
  {0x00,0x01,0x02,0x04,0x00}, // `
  {0x20,0x54,0x54,0x54,0x78}, // a
  {0x7F,0x48,0x44,0x44,0x38}, // b
  {0x38,0x44,0x44,0x44,0x20}, // c
  {0x38,0x44,0x44,0x48,0x7F}, // d
  {0x38,0x54,0x54,0x54,0x18}, // e
  {0x08,0x7E,0x09,0x01,0x02}, // f
  {0x08,0x14,0x54,0x54,0x3C}, // g
  {0x7F,0x08,0x04,0x04,0x78}, // h
  {0x00,0x44,0x7D,0x40,0x00}, // i
  {0x20,0x40,0x44,0x3D,0x00}, // j
  {0x00,0x7F,0x10,0x28,0x44}, // k
  {0x00,0x41,0x7F,0x40,0x00}, // l
  {0x7C,0x04,0x18,0x04,0x78}, // m
  {0x7C,0x08,0x04,0x04,0x78}, // n
  {0x38,0x44,0x44,0x44,0x38}, // o
  {0x7C,0x14,0x14,0x14,0x08}, // p
  {0x08,0x14,0x14,0x18,0x7C}, // q
  {0x7C,0x08,0x04,0x04,0x08}, // r
  {0x48,0x54,0x54,0x54,0x20}, // s
  {0x04,0x3F,0x44,0x40,0x20}, // t
  {0x3C,0x40,0x40,0x20,0x7C}, // u
  {0x1C,0x20,0x40,0x20,0x1C}, // v
  {0x3C,0x40,0x30,0x40,0x3C}, // w
  {0x44,0x28,0x10,0x28,0x44}, // x
  {0x0C,0x50,0x50,0x50,0x3C}, // y
  {0x44,0x64,0x54,0x4C,0x44}, // z
  {0x00,0x08,0x36,0x41,0x00}, // {
  {0x00,0x00,0x7F,0x00,0x00}, // |
  {0x00,0x41,0x36,0x08,0x00}, // }
  {0x08,0x08,0x2A,0x1C,0x08}, // ~
};

void drawChar(int16_t x, int16_t y, char ch, uint16_t c, uint8_t scale) {
  if (ch < 32 || ch > 126) ch = '?';
  const uint8_t *g = font5x7[ch - 32];
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t bits = pgm_read_byte(&g[col]);
    for (uint8_t row = 0; row < 7; row++)
      if (bits & (1 << row))
        fillRect(x + col * scale, y + row * scale, scale, scale, c);
  }
}

void drawString(const char *s, int16_t x, int16_t y, uint16_t c, uint8_t scale) {
  while (*s) {
    drawChar(x, y, *s++, c, scale);
    x += 6 * scale;
  }
}

void drawInt(int32_t n, int16_t x, int16_t y, uint16_t c, uint8_t scale) {
  char buf[12];
  itoa(n, buf, 10);
  drawString(buf, x, y, c, scale);
}

// ---------------- 演示 ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("手表屏正式版启动 ST7789 240x240");
  lcdInit();
  fillScreen(0x0000);
}

uint16_t bg = 0x0000;

void loop() {
  // ---- 页 1：欢迎 ----
  fillScreen(0x0000);
  drawRect(4, 4, 232, 232, 0x001F);
  fillRect(20, 40, 200, 30, 0xF800);
  drawString("HELLO", 62, 48, 0xFFFF, 3);
  drawString("ST7789 240x240", 36, 90, 0x07E0, 1);
  drawString("ESP32-S3 SPI LCD", 30, 104, 0x07FF, 1);
  drawInt(millis() / 1000, 108, 130, 0xFFE0, 2);
  fillCircle(120, 180, 30, 0xF81F);
  fillCircle(110, 172, 8, 0xFFFF);
  fillCircle(132, 172, 8, 0xFFFF);
  drawLine(104, 192, 138, 192, 0x0000);
  delay(3000);

  // ---- 页 2：图形 ----
  fillScreen(0x0000);
  for (int i = 0; i < 8; i++)
    drawCircle(120, 120, 15 + i * 14, 0x07E0);
  drawLine(0, 0, 239, 239, 0xF800);
  drawLine(239, 0, 0, 239, 0xF800);
  drawString("GRAPHICS OK", 72, 8, 0xFFFF, 1);
  delay(2500);

  // ---- 页 3：弹跳球 ----
  fillScreen(0x0000);
  int16_t bx = 40, by = 40, vx = 3, vy = 2;
  uint32_t t0 = millis();
  while (millis() - t0 < 6000) {
    fillRect(bx - 6, by - 6, 13, 13, bg);      // 擦旧
    bx += vx; by += vy;
    if (bx < 6 || bx > 233) { vx = -vx; bx += vx; }
    if (by < 6 || by > 233) { vy = -vy; by += vy; }
    fillCircle(bx, by, 6, 0xFFE0);             // 画新
    delay(16);
  }
  // ---- 页 4：秒表 ----
  fillScreen(0x0000);
  drawString("SEC:", 84, 100, 0x07FF, 2);
  for (int i = 0; i < 10; i++) {
    fillRect(100, 130, 60, 24, 0x0000);
    drawInt(i, 116, 132, 0xFFFF, 3);
    delay(1000);
  }
}
