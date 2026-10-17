/*
 * GD32 桥接屏协议探测 v4（ESP32-S3 -> 手表屏驱动板）
 * ----------------------------------------------------
 * v3 结果：10 组合全黑。v4 在此基础上自动补测两个变量：
 *   - DAT/CLK 接反（软件层面换引脚，不用动线）
 *   - 全局降速（半周期翻倍，照顾 GD32 软件轮询从机）
 * 共 4 轮 x 5 协议 x 2 面板 = 40 组合，每组合约 4 秒。
 * 串口 115200 实时打印当前组合，屏幕有任何反应立刻记下组合号。
 *
 * 接线不变：3V3->3.3V, IO4->CLK, IO5->DAT, GND->GND
 */

#include <Wire.h>
#include "soc/gpio_struct.h"

// 结构体必须在所有函数之前（Arduino 预处理器会把函数原型插到最前面）
struct CmdStep { uint8_t cmd; uint8_t data[8]; uint8_t len; uint16_t delayMs; };

static int gClk = 4;              // 当前 CLK 引脚（会被轮换）
static int gDat = 5;              // 当前 DAT 引脚
static uint8_t gHalfNop = 8;      // 半周期（越大越慢）
static uint8_t gGapNop  = 600;    // 帧/事务间隔

static inline void setDat(uint8_t v) {
  if (v) GPIO.out_w1ts = (1UL << gDat); else GPIO.out_w1tc = (1UL << gDat);
}
static inline void clkPulse() {
  GPIO.out_w1ts = (1UL << gClk);
  for (volatile int i = 0; i < gHalfNop; i++) {}
  GPIO.out_w1tc = (1UL << gClk);
  for (volatile int i = 0; i < gHalfNop; i++) {}
}
static void bbGap() { for (volatile int i = 0; i < gGapNop; i++) {} }

static void bbSend8(uint8_t b) { for (int i = 7; i >= 0; i--) { setDat((b >> i) & 1); clkPulse(); } }
static void bbSend9(uint16_t w) { for (int i = 8; i >= 0; i--) { setDat((w >> i) & 1); clkPulse(); } }

// ---------------- 协议模式 ----------------
// 0=9bit(标志位0) 1=9bit(标志位1) 2=8bit分事务 3=8bit合并 4=UART
static uint8_t gProto = 0;

void pushBytes(const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (gProto == 0)      bbSend9(0x100 | d[i]);
    else if (gProto == 1) bbSend9(d[i]);
    else if (gProto == 4) Serial1.write(d[i]);
    else                  bbSend8(d[i]);
  }
}

void writeCmd(uint8_t c, const uint8_t *d, size_t n) {
  if (gProto == 0) {
    bbSend9(c);
    for (size_t i = 0; i < n; i++) bbSend9(0x100 | d[i]);
    bbGap();
  } else if (gProto == 1) {
    bbSend9(0x100 | c);
    for (size_t i = 0; i < n; i++) bbSend9(d[i]);
    bbGap();
  } else if (gProto == 2) {
    bbSend8(c); bbGap();
    if (n) { for (size_t i = 0; i < n; i++) bbSend8(d[i]); bbGap(); }
  } else if (gProto == 3) {
    bbSend8(c);
    for (size_t i = 0; i < n; i++) bbSend8(d[i]);
    bbGap();
  } else {
    Serial1.write(c);
    if (n) Serial1.write(d, n);
  }
}

// ---------------- 面板初始化序列 ----------------
const CmdStep seqSt7789[] = {
  {0x01, {0}, 0, 150}, {0x11, {0}, 0, 120}, {0x3A, {0x05}, 1, 10},
  {0x36, {0x00}, 1, 10}, {0x21, {0}, 0, 10}, {0x13, {0}, 0, 10}, {0x29, {0}, 0, 100},
};
const uint8_t seqSt7789Len = 7;

const CmdStep seqGc9a01[] = {
  {0x01, {0}, 0, 150}, {0x11, {0}, 0, 120}, {0xFE, {0}, 0, 10}, {0xEF, {0}, 0, 10},
  {0x3A, {0x05}, 1, 10}, {0x21, {0}, 0, 10}, {0x2C, {0}, 0, 10}, {0x29, {0}, 0, 100},
};
const uint8_t seqGc9a01Len = 8;

void runSequence(const CmdStep *seq, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    writeCmd(seq[i].cmd, seq[i].data, seq[i].len);
    delay(seq[i].delayMs);
  }
}

void fillScreen(uint16_t color) {
  uint8_t win[] = {0x00, 0x00, 0x00, 0xEF};
  writeCmd(0x2A, win, 4);
  writeCmd(0x2B, win, 4);
  writeCmd(0x2C, nullptr, 0);
  uint8_t line[240];
  for (int i = 0; i < 240; i += 2) { line[i] = color >> 8; line[i + 1] = color & 0xFF; }
  for (int r = 0; r < 240; r++) pushBytes(line, 240);
  bbGap();
}

void applyPins() {
  pinMode(gClk, OUTPUT);
  pinMode(gDat, OUTPUT);
  setDat(1);
}

// ---------------- I2C 扫描 ----------------
void i2cScan(int sda, int scl) {
  Wire.begin(sda, scl, 100000);
  Serial.printf("I2C scan  SDA=IO%d SCL=IO%d : ", sda, scl);
  uint8_t found = 0;
  for (uint8_t a = 1; a < 0x7F; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("ACK 0x%02X  ", a); found++; }
  }
  if (!found) Serial.print("no device");
  Serial.println();
  Wire.end();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== GD32 bridge probe v4 (bit-bang, pin-swap, slower) ===");

  gClk = 4; gDat = 5; applyPins();
  i2cScan(5, 4);
  i2cScan(4, 5);

  applyPins();
  Serial.println("I2C 无设备属正常（接口若是 SPI，扫不到任何东西）\n");

  Serial1.begin(115200, SERIAL_8N1, -1, gDat);
}

struct Combo { uint8_t proto; const char *pname; const CmdStep *seq; uint8_t len; const char *sname; };

void loop() {
  static const Combo protos[] = {
    {0, "9bit-flag0 ", seqSt7789, seqSt7789Len, "ST7789"},
    {1, "9bit-flag1 ", seqSt7789, seqSt7789Len, "ST7789"},
    {2, "8bit-split ", seqSt7789, seqSt7789Len, "ST7789"},
    {3, "8bit-merged", seqSt7789, seqSt7789Len, "ST7789"},
    {4, "uart115200", seqSt7789, seqSt7789Len, "ST7789"},
    {0, "9bit-flag0 ", seqGc9a01, seqGc9a01Len, "GC9A01"},
    {1, "9bit-flag1 ", seqGc9a01, seqGc9a01Len, "GC9A01"},
    {2, "8bit-split ", seqGc9a01, seqGc9a01Len, "GC9A01"},
    {3, "8bit-merged", seqGc9a01, seqGc9a01Len, "GC9A01"},
    {4, "uart115200", seqGc9a01, seqGc9a01Len, "GC9A01"},
  };
  static const uint8_t np = sizeof(protos) / sizeof(Combo);

  static uint32_t round = 0;
  round++;
  Serial.printf("\n########## 第 %lu 轮 ##########\n", (unsigned long)round);

  for (uint8_t order = 0; order < 2; order++) {
    // order 0: CLK=IO4 DAT=IO5（正常）；order 1: 对调
    gClk = (order == 0) ? 4 : 5;
    gDat = (order == 0) ? 5 : 4;
    Serial.printf("== 引脚顺序 %s (CLK=IO%d DAT=IO%d) ==\n",
                  order == 0 ? "正常" : "对调", gClk, gDat);

    for (uint8_t i = 0; i < np; i++) {
      Combo c = protos[i];
      gProto = c.proto;
      uint32_t t0 = millis();
      Serial.printf("[%2u/%u] %s + %s (t=%lus)\n",
                    i + 1, np, c.pname, c.sname, (unsigned long)(t0 / 1000));
      applyPins();
      runSequence(c.seq, c.len);
      fillScreen(0xF800);
      Serial.print("       red  done\n");
      delay(800);
      fillScreen(0x001F);
      Serial.print("       blue done\n");
      delay(800);
      if (c.proto == 4) {          // UART 跑完把引脚抢回来
        Serial1.end();
        applyPins();
        Serial1.begin(115200, SERIAL_8N1, -1, gDat);
      }
    }
  }
}
