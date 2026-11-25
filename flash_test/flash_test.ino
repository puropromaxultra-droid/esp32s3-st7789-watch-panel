// ============================================================
// flash_test —— 探测外挂 U5 (GD25Q128, 16MB) 是否活着 + 测真实吞吐
// V5 一体板 · ESP32-S3-MINI-1-N8
//
// U5 接在 SPI2（普通外设总线，不是内部 flash 的那条）：
//   CLK  = IO38 (U1.34)   MOSI = IO39 (U1.35, U5.DI)
//   MISO = IO40 (U1.36, U5.DO)   CS = IO21 (U1.25)
//
// ① JEDEC ID  ② 状态寄存器  ③ 末尾扇区 擦→写→读回
// ④ 吞吐：逐字节 vs 批量，10 / 26 / 40 MHz
//    —— 这个数字决定 GIF 能不能放 U5 上
// ============================================================

#include <Arduino.h>
#include <SPI.h>

#define PIN_CS    21
#define PIN_SCK   38
#define PIN_MOSI  39
#define PIN_MISO  40

#define CMD_WREN  0x06
#define CMD_RDSR1 0x05
#define CMD_RDSR2 0x35
#define CMD_READ  0x03
#define CMD_PP    0x02
#define CMD_SE    0x20
#define CMD_JEDEC 0x9F

SPIClass spi(FSPI);
static uint32_t gHz = 10000000;          // 当前 SPI 时钟，beginCmd 用它

static inline void csLow()  { digitalWrite(PIN_CS, LOW); }
static inline void csHigh() { digitalWrite(PIN_CS, HIGH); }

static void beginCmd(uint8_t c)
{
  spi.beginTransaction(SPISettings(gHz, MSBFIRST, SPI_MODE0));
  csLow();
  spi.transfer(c);
}
static void endCmd()
{
  csHigh();
  spi.endTransaction();
}
static void waitBusy()
{
  uint32_t t0 = millis();
  for (;;) {
    beginCmd(CMD_RDSR1);
    uint8_t sr = spi.transfer(0);
    endCmd();
    if (!(sr & 0x01)) return;
    if (millis() - t0 > 5000) { Serial.println("[u5] !! busy timeout"); return; }
    delay(1);
  }
}
static void sendAddr(uint32_t a)
{
  spi.transfer((a >> 16) & 0xFF);
  spi.transfer((a >> 8) & 0xFF);
  spi.transfer(a & 0xFF);
}
static void jedec(uint8_t *id3)
{
  beginCmd(CMD_JEDEC);
  id3[0] = spi.transfer(0);
  id3[1] = spi.transfer(0);
  id3[2] = spi.transfer(0);
  endCmd();
}
static void readData(uint32_t addr, uint8_t *buf, size_t len)
{
  beginCmd(CMD_READ);
  sendAddr(addr);
  for (size_t i = 0; i < len; i++) buf[i] = spi.transfer(0);
  endCmd();
}
static void pageProgram(uint32_t addr, const uint8_t *buf, size_t len)
{
  beginCmd(CMD_WREN); endCmd();
  beginCmd(CMD_PP); sendAddr(addr);
  for (size_t i = 0; i < len; i++) spi.transfer(buf[i]);
  endCmd();
  waitBusy();
}
static void sectorErase(uint32_t addr)
{
  beginCmd(CMD_WREN); endCmd();
  beginCmd(CMD_SE); sendAddr(addr); endCmd();
  waitBusy();
}

// ---- 批量读：一次 4KB，用 transferBytes（内部走 spiTransferBytesNL） ----
static uint8_t zeros[4096];
static uint8_t big[4096];

static double benchBulk(uint32_t addr, uint32_t totalKB)
{
  uint32_t blocks = totalKB / 4;
  beginCmd(CMD_READ);
  sendAddr(addr);
  uint32_t t0 = micros();
  for (uint32_t i = 0; i < blocks; i++) spi.transferBytes(zeros, big, 4096);
  uint32_t us = micros() - t0;
  endCmd();
  return (double)us / 1000.0;
}

static double benchByte(uint32_t addr, uint32_t bytes)
{
  beginCmd(CMD_READ);
  sendAddr(addr);
  uint32_t t0 = micros();
  for (uint32_t i = 0; i < bytes; i++) big[i & 4095] = spi.transfer(0);
  uint32_t us = micros() - t0;
  endCmd();
  return (double)us / 1000.0;
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== flash_test · U5 GD25Q128 ===");

  pinMode(PIN_CS, OUTPUT);
  csHigh();
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  Serial.printf("[u5] SPI2  CLK=%d MOSI=%d MISO=%d CS=%d\n",
                PIN_SCK, PIN_MOSI, PIN_MISO, PIN_CS);

  // ---- ① JEDEC ----
  uint8_t id[3] = {0, 0, 0};
  jedec(id);
  Serial.printf("[u5] JEDEC  %02X %02X %02X\n", id[0], id[1], id[2]);
  if (id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00) {
    Serial.println("[u5] x 全 0 —— MISO 没信号：U5 虚焊 / CS 不通 / 3V3 没到");
    return;
  }
  if (id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF) {
    Serial.println("[u5] x 全 FF —— 总线空闲没人应答：CS 那条线可能断了");
    return;
  }
  uint32_t cap = 1UL << id[2];
  Serial.printf("[u5] 厂商 %02X  容量 %lu 字节 (%.0f MB)%s\n",
                id[0], (unsigned long)cap, cap / 1048576.0,
                id[0] == 0xC8 ? "  GigaDevice ✓" : "");

  // ---- ② 状态 ----
  beginCmd(CMD_RDSR1); uint8_t sr1 = spi.transfer(0); endCmd();
  beginCmd(CMD_RDSR2); uint8_t sr2 = spi.transfer(0); endCmd();
  Serial.printf("[u5] SR1=%02X  SR2=%02X  %s\n", sr1, sr2,
                (sr1 & 0x1C) ? "★ 写保护中（block protect 非 0）" : "未写保护 ✓");

  // ---- ③ 擦→写→读回 ----
  uint32_t test = cap - 4096;
  uint8_t pat[32], rd[32];
  for (int i = 0; i < 32; i++) pat[i] = (uint8_t)(0xA0 + i);
  sectorErase(test);
  readData(test, rd, 32);
  bool blank = true;
  for (int i = 0; i < 32; i++) if (rd[i] != 0xFF) blank = false;
  pageProgram(test, pat, 32);
  memset(rd, 0, sizeof(rd));
  readData(test, rd, 32);
  bool same = (memcmp(rd, pat, 32) == 0);
  Serial.printf("[u5] 末尾扇区 @0x%06lX  擦除=%s  写读=%s\n",
                (unsigned long)test,
                blank ? "全FF ✓" : "失败 x",
                same ? "一致 ✓" : "不一致 x");

  // ---- ④ 吞吐 ----
  Serial.println("[u5] ---- 吞吐 ----");
  const uint32_t freqs[3] = {10000000, 26000000, 40000000};
  uint8_t idAt = 0;
  for (int fi = 0; fi < 3; fi++) {
    gHz = freqs[fi];
    // 每个频率下先验一次 ID，确认这个时钟能跑
    jedec(id);
    bool ok = (id[0] == 0xC8);
    if (ok) idAt = fi;
    double ms1 = benchByte(test, 4096);
    double ms2 = benchBulk(test, 64);
    double kb1 = 4096.0 / 1024.0 / (ms1 / 1000.0);
    double kb2 = 65536.0 / 1024.0 / (ms2 / 1000.0);
    Serial.printf("[u5] %2luMHz ID=%s  逐字节 4KB %.2fms (%.0f KB/s)  批量 64KB %.2fms (%.0f KB/s)\n",
                  (unsigned long)(freqs[fi] / 1000000), ok ? "ok" : "x", ms1, kb1, ms2, kb2);
  }

  gHz = freqs[idAt];
  double ms = benchBulk(test, 448);      // 448KB 让计时更稳
  double kbs = 448.0 * 1024.0 / (ms / 1000.0) / 1024.0;
  Serial.printf("[u5] 最高可用 %luMHz：一帧 115KB 需 %.1f ms\n",
                (unsigned long)(gHz / 1000000), 115200.0 / (kbs * 1024.0) * 1000.0);
  Serial.println("[u5] 探测结束。");
}

void loop()
{
  delay(1000);
}
