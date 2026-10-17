// ============================================================
// 天气时钟 —— 手表屏 ST7789 240x240 + ESP32-S3
// WiFi + NTP 对时 + Open-Meteo 天气（免 API key，武汉坐标）
// 显示：日期/星期、大字时钟、天气图标、温度/湿度/风速、今日高低温
//
// ★★★ 上电前把下面两行改成你家 WiFi 的账号密码 ★★★
//
// 复用 lcd_demo 的绘图 API（ST7789，INVON，SPI 20MHz，接线不变）。
// ============================================================

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "secrets.h"   // WiFi 凭据：复制 secrets.h.example 为 secrets.h 后填写

// 武汉坐标
#define LAT 30.5928f
#define LON 114.3055f

// ---------------- 屏幕驱动（同 lcd_demo，已验证） ----------------
#define PIN_SCK  4
#define PIN_MOSI 5
#define PIN_CS   6
#define PIN_DC   7
#define PIN_RST  8
#define PIN_IM   9
#define W 240
#define H 240

SPISettings spiCfg(10000000UL, MSBFIRST, SPI_MODE0);   // 10MHz：点亮验证值，20MHz 飞线跑不动

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
  tCmd(0x21); delay(10);   // INVON，IPS 必须
  tCmd(0x13); delay(10);
  tCmd(0x29); delay(50);
}

void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  setWindow(x, y, x + w - 1, y + h - 1);
  uint8_t hi = c >> 8, lo = c & 0xFF;
  digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(spiCfg);
  for (uint32_t i = 0; i < (uint32_t)w * h; i++) { SPI.transfer(hi); SPI.transfer(lo); }
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
}
void fillScreen(uint16_t c) { fillRect(0, 0, W, H, c); }
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
void drawCircle(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  int16_t x = r, y = 0, e = 1 - r;
  while (x >= y) {
    drawPixel(cx + x, cy + y, c); drawPixel(cx - x, cy + y, c);
    drawPixel(cx + y, cy + x, c); drawPixel(cx - y, cy + x, c);
    drawPixel(cx - x, cy - y, c); drawPixel(cx + x, cy - y, c);
    drawPixel(cx - y, cy - x, c); drawPixel(cx + y, cy - x, c);
    y++;
    if (e < 0) e += 2 * y + 1; else { x--; e += 2 * (y - x) + 1; }
  }
}
void fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  for (int16_t y = -r; y <= r; y++) {
    int16_t dx = (int16_t)sqrtf((float)(r * r - y * y));
    drawLine(cx - dx, cy + y, cx + dx, cy + y, c);
  }
}
void drawRoundRectFill(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t c) {
  fillRect(x + r, y, w - 2 * r, h, c);
  fillRect(x, y + r, r, h - 2 * r, c);
  fillRect(x + w - r, y + r, r, h - 2 * r, c);
  fillCircle(x + r, y + r, r, c);
  fillCircle(x + w - r - 1, y + r, r, c);
  fillCircle(x + r, y + h - r - 1, r, c);
  fillCircle(x + w - r - 1, y + h - r - 1, r, c);
}

// ---------------- 5x7 字库（ASCII 32~126） ----------------
static const uint8_t font5x7[][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
  {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
  {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
  {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
  {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
  {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
  {0x00,0x56,0x36,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
  {0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
  {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},
  {0x3E,0x41,0x41,0x51,0x32},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
  {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
  {0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
  {0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},{0x63,0x14,0x08,0x14,0x63},
  {0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
  {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},
  {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
  {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
  {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
  {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
  {0x00,0x7F,0x10,0x28,0x44},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
  {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
  {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
  {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
  {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
  {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
  {0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08},
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
  while (*s) { drawChar(x, y, *s++, c, scale); x += 6 * scale; }
}
void drawInt(int32_t n, int16_t x, int16_t y, uint16_t c, uint8_t scale) {
  char buf[12]; itoa(n, buf, 10); drawString(buf, x, y, c, scale);
}
// 温度带小数
void drawFloat1(float v, int16_t x, int16_t y, uint16_t c, uint8_t scale) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%d.%d", (int)v, (int)(v * 10) % 10 < 0 ? -(int)(v * 10) % 10 : (int)(v * 10) % 10);
  drawString(buf, x, y, c, scale);
}

// 颜色
#define C_BG    0x0000  // 黑
#define C_CARD  0x1010  // 深灰蓝
#define C_WHITE 0xFFFF
#define C_YEL   0xFFE0
#define C_CYAN  0x07FF
#define C_GREEN 0x07E0
#define C_RED   0xF800
#define C_ORG   0xFD20
#define C_GRAY  0x8410
#define C_BLUE  0x425B

// ---------------- 天气 ----------------
struct Weather {
  float temp = 0, hum = 0, wind = 0, tmax = 0, tmin = 0;
  int   code = -1;
  bool  ok = false;
} wx;

static bool findNum(const char *j, const char *key, float &out) {
  const char *p = strstr(j, key);
  if (!p) return false;
  p += strlen(key);
  while (*p == ' ' || *p == ':' || *p == '"' || *p == '[') p++;
  out = strtof(p, nullptr);
  return true;
}

bool fetchWeather() {
  WiFiClientSecure cli;
  cli.setInsecure();
  cli.setTimeout(8000);
  if (!cli.connect("api.open-meteo.com", 443)) { Serial.println("connect fail"); return false; }
  char req[200];
  snprintf(req, sizeof(req),
    "GET /v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=1 HTTP/1.1\r\n"
    "Host: api.open-meteo.com\r\nUser-Agent: esp32\r\nConnection: close\r\n\r\n",
    LAT, LON);
  cli.print(req);
  String body; bool headDone = false;
  uint32_t t0 = millis();
  while (cli.connected() && millis() - t0 < 10000) {
    while (cli.available()) {
      String line = cli.readStringUntil('\n');
      if (!headDone) { if (line.length() <= 1) headDone = true; }
      else body += line;
      t0 = millis();
    }
  }
  cli.stop();
  if (body.length() < 50) return false;
  Weather w;
  bool ok = true;
  ok &= findNum(body.c_str(), "\"temperature_2m\"", w.temp);
  ok &= findNum(body.c_str(), "\"relative_humidity_2m\"", w.hum);
  float fcode = 0;
  ok &= findNum(body.c_str(), "\"weather_code\"", fcode);
  w.code = (int)fcode;
  ok &= findNum(body.c_str(), "\"wind_speed_10m\"", w.wind);
  ok &= findNum(body.c_str(), "\"temperature_2m_max\"", w.tmax);
  ok &= findNum(body.c_str(), "\"temperature_2m_min\"", w.tmin);
  if (!ok) return false;
  wx = w; wx.ok = true;
  Serial.printf("weather: %.1fC hum %.0f%% wind %.0fkm/h code %d  %.1f/%.1f\n",
                wx.temp, wx.hum, wx.wind, wx.code, wx.tmax, wx.tmin);
  return true;
}

// ---------------- 天气图标（WMO code -> 图形） ----------------
// type: 0晴 1多云 2阴/雾 3雨 4雪 5雷雨
static int iconType(int code) {
  if (code == 0) return 0;
  if (code <= 2) return 1;
  if (code == 45 || code == 48) return 2;
  if (code <= 67 || (code >= 80 && code <= 82)) return 3;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 4;
  return 5; // 雷雨/其他
}

static void drawSun(int16_t cx, int16_t cy) {
  fillCircle(cx, cy, 16, C_YEL);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.785398f;
    drawLine(cx + cosf(a) * 21, cy + sinf(a) * 21, cx + cosf(a) * 29, cy + sinf(a) * 29, C_YEL);
  }
}
static void drawCloud(int16_t cx, int16_t cy, uint16_t col) {
  fillCircle(cx - 14, cy + 4, 12, col);
  fillCircle(cx + 4, cy - 4, 15, col);
  fillCircle(cx + 18, cy + 5, 11, col);
  fillRect(cx - 24, cy + 5, 50, 12, col);
}
static void drawIcon(int type, int16_t cx, int16_t cy) {
  switch (type) {
    case 0: drawSun(cx, cy); break;
    case 1: drawSun(cx - 10, cy - 12); drawCloud(cx + 6, cy + 8, C_GRAY); break;
    case 2: drawCloud(cx, cy, C_GRAY); drawCloud(cx + 10, cy + 8, 0x630C); break;
    case 3:
      drawCloud(cx, cy - 8, C_GRAY);
      for (int i = 0; i < 3; i++) drawLine(cx - 14 + i * 14, cy + 12, cx - 18 + i * 14, cy + 26, C_CYAN);
      break;
    case 4:
      drawCloud(cx, cy - 8, C_GRAY);
      for (int i = 0; i < 3; i++) { drawPixel(cx - 14 + i * 14, cy + 14, C_WHITE); drawPixel(cx - 14 + i * 14, cy + 17, C_WHITE); drawPixel(cx - 17 + i * 14, cy + 15, C_WHITE); drawPixel(cx - 11 + i * 14, cy + 15, C_WHITE); }
      break;
    default:
      drawCloud(cx, cy - 10, C_GRAY);
      drawLine(cx - 2, cy + 6, cx - 6, cy + 18, C_YEL);
      drawLine(cx + 8, cy + 6, cx + 4, cy + 18, C_YEL);
      drawLine(cx - 12, cy + 14, cx - 16, cy + 26, C_CYAN);
      drawLine(cx + 16, cy + 14, cx + 12, cy + 26, C_CYAN);
      break;
  }
}
static const char *wxText(int type) {
  switch (type) {
    case 0: return "SUNNY";
    case 1: return "CLOUDY";
    case 2: return "OVERCAST";
    case 3: return "RAIN";
    case 4: return "SNOW";
    default: return "STORM";
  }
}

// ---------------- UI ----------------
static const char *WD[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static char lastHM[6] = "";

void drawStaticFrame() {
  fillScreen(C_BG);
  // 顶部条：城市 + 日期
  drawRoundRectFill(6, 6, 228, 26, 8, C_CARD);
  drawString("WUHAN", 18, 13, C_CYAN, 2);
  // 中间时钟卡片
  drawRoundRectFill(6, 38, 228, 92, 10, C_CARD);
  // 天气卡片（左：图标 右：数据）
  drawRoundRectFill(6, 136, 228, 98, 10, C_CARD);
  drawLine(6, 132, 234, 132, 0x2124);
}

void drawDate(const struct tm &t) {
  char buf[24];
  fillRect(80, 8, 150, 22, C_CARD);
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, WD[t.tm_wday]);
  drawString(buf, 84, 14, C_WHITE, 1);
  drawString(buf + 11, 152, 13, C_YEL, 2);   // 星期放大
}

void drawClock(const struct tm &t) {
  char hm[6];
  snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
  fillRect(24, 48, 192, 62, C_CARD);
  drawString(hm, 30, 50, C_WHITE, 6);           // 6 倍大字 ≈ 30x42
  fillCircle(158, 78, 5, C_YEL);                 // 秒点
  drawInt(t.tm_sec, 172, 72, C_YEL, 2);
  strcpy(lastHM, hm);
}

void drawWeather() {
  fillRect(6, 136, 228, 98, C_CARD);
  if (!wx.ok) {
    drawString("NO WEATHER DATA", 48, 175, C_GRAY, 2);
    return;
  }
  int type = iconType(wx.code);
  drawIcon(type, 48, 182);
  char buf[16];
  drawFloat1(wx.temp, 84, 150, C_WHITE, 5);      // 大温度
  drawString("C", 152, 152, C_WHITE, 3);
  drawCircle(148, 158, 2, C_WHITE);              // 度数点
  // 湿度/风速
  snprintf(buf, sizeof(buf), "HUM %d%%", (int)wx.hum);
  drawString(buf, 84, 200, C_CYAN, 2);
  snprintf(buf, sizeof(buf), "WIND %dkm/h", (int)wx.wind);
  drawString(buf, 84, 218, C_GREEN, 1);
  // 今日高低
  snprintf(buf, sizeof(buf), "%d/%d", (int)(wx.tmax + 0.5f), (int)(wx.tmin + 0.5f));
  drawString(buf, 176, 196, C_ORG, 2);
  drawString(wxText(type), 168, 214, C_GRAY, 1);
}

// ---------------- WiFi / NTP ----------------
bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

void connectWiFi() {
  Serial.printf("WiFi connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (!wifiUp() && millis() - t0 < 15000) { delay(300); Serial.print("."); }
  Serial.println(wifiUp() ? " OK, IP: " + WiFi.localIP().toString() : " FAIL");
  if (wifiUp()) {
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
  }
}

// ---------------- 主流程 ----------------
uint32_t lastWeatherMs = 0;
bool frameDrawn = false;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n天气时钟启动");
  lcdInit();
  // 开机画面（STEP1 = LCD 初始化完成；若此字都不出 = 画屏本身死了）
  fillScreen(C_BG);
  drawString("WEATHER CLOCK", 42, 60, C_CYAN, 2);
  drawString("STEP1 LCD OK", 60, 95, C_GREEN, 1);
  drawString("connecting WiFi...", 60, 130, C_GRAY, 1);
  Serial.printf("heap after lcdInit=%d\n", ESP.getFreeHeap());
  connectWiFi();
  // 开机结果直接画在屏上，网络卡点一眼可见
  Serial.printf("heap after wifi=%d\n", ESP.getFreeHeap());
  if (wifiUp()) {
    drawString("WiFi OK              ", 42, 128, C_GREEN, 1);
    drawString(WiFi.localIP().toString().c_str(), 42, 142, C_GRAY, 1);
    drawString("NTP+WEATHER FETCHING ", 42, 156, C_YEL, 1);
  } else {
    drawString("WiFi FAIL            ", 42, 128, C_RED, 1);
    drawString("check SSID/PASS,2.4G ", 42, 142, C_GRAY, 1);
  }
  fetchWeather();          // 第一次拉天气
  Serial.printf("heap after weather=%d wx.ok=%d\n", ESP.getFreeHeap(), wx.ok);
  drawString("STEP2 NET OK, DRAW UI", 42, 170, C_GREEN, 1);
  lastWeatherMs = millis();
  frameDrawn = false;
}

void loop() {
  struct tm t;
  bool haveTime = getLocalTime(&t, 200);

  if (!frameDrawn) { drawStaticFrame(); frameDrawn = true; }

  if (haveTime) {
    drawDate(t);
    drawClock(t);
  } else {
    // NTP 没同步也照样显示界面，时钟区显示占位
    fillRect(24, 48, 192, 62, C_CARD);
    drawString("--:--", 30, 50, C_GRAY, 6);
    drawInt((millis() / 1000) % 60, 172, 72, C_GRAY, 2);
    fillRect(80, 8, 150, 22, C_CARD);
    drawString("NTP SYNCING...", 84, 14, C_GRAY, 1);
  }
  drawWeather();

  // 每 10 分钟刷新天气
  if (wifiUp() && millis() - lastWeatherMs > 600000UL) {
    lastWeatherMs = millis();
    if (fetchWeather()) frameDrawn = false;   // 触发重绘
  }
  // WiFi 断了：每 30 秒重试一次，不阻塞正常显示
  static uint32_t lastRetry = 0;
  if (!wifiUp() && millis() - lastRetry > 30000UL) {
    lastRetry = millis();
    WiFi.disconnect();
    connectWiFi();
  }

  delay(900);   // 约 1 秒刷一次
}
