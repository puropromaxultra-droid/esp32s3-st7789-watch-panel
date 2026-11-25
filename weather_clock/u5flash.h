// ============================================================
// u5flash.h —— 外挂 U5 (GD25Q128, 16MB) 驱动 + 串口上传协议
//
// 为什么单独占一个 SPI 主机：
//   LCD 走默认 SPI（FSPI = SPI2），U5 如果也挂 FSPI，两个设备会抢同一个
//   SPI 外设寄存器 + 引脚矩阵，推屏和读 flash 必须严格串行，很容易出花屏。
//   ESP32-S3 有第二个可用主机 HSPI（= SPI3），把 U5 挪过去，两条总线真正并行。
//
// 接线（V5 一体板）：
//   CLK = IO38 (U1.34)   MOSI = IO39 (U1.35, U5.DI)
//   MISO= IO40 (U1.36)   CS   = IO21 (U1.25)
//
// 实测（flash_test）：40MHz 批量读 3723 KB/s，一帧 115KB 只要 30ms。
//
// 上传协议（走 USB CDC，文本命令行 + 二进制块）：
//   U\n                 → 应答 "UOK <sector> <capacity>"
//   H <total> <crc32>\n → 擦除所需扇区，应答 "HOK"
//   D <seq> <len>\n     → 紧跟 len 字节原始数据；写成功应答 "A <seq>"
//   E\n                 → 校验 CRC，应答 "EOK" / "EBAD <got> <want>"
// 出错应答 "ERR <原因>"，超时 20 秒没命令就退出上传模式。
// ============================================================

#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <USBCDC.h>

// ---------------- 引脚 / 参数 ----------------
#define U5_PIN_CS    21
#define U5_PIN_SCK   38
#define U5_PIN_MOSI  39
#define U5_PIN_MISO  40

// ★ 读和写分开定频率（2026-11-19 提帧率）：
//   读改用 Fast Read 0x0B（3 字节地址 + 8 dummy），规格支持到 104MHz，实跑 80MHz；
//   30fps→45fps 就差这几毫秒：37KB 一帧从 9ms 降到 4.3ms。
//   擦/写/读状态寄存器仍用 40MHz —— 写坏一帧 GIF 比慢一点严重得多。
#define U5_HZ_READ   80000000UL
#define U5_HZ_WRITE  40000000UL
#define U5_SECTOR    4096
#define U5_PAGE      256
#define U5_BASE      0x000000UL    // GIF blob 从 0 地址开始

// ---------------- SPI 指令 ----------------
#define U5_WREN   0x06
#define U5_RDSR1  0x05
#define U5_READ   0x03
#define U5_READF  0x0B             // Fast Read：3 字节地址 + 8 dummy，80MHz 靠它
#define U5_PP     0x02
#define U5_SE     0x20
#define U5_BE     0xD8             // 64KB block erase：16 个扇区一次擦完
#define U5_BE32   0x52             // 32KB block erase：8 个扇区（音频用这个，阻塞期减半）
#define U5_JEDEC  0x9F

#define U5_BLK    2048             // 上传块大小（= 8 个 flash 页，不跨扇区）

static SPIClass  u5spi(HSPI);
static uint8_t   u5dummy[U5_SECTOR];   // 读操作用的 0xFF/0x00 哑源
static uint8_t   u5blk[U5_BLK];
static bool      u5ok     = false;
static uint32_t  u5cap    = 0;
static uint32_t  u5crcRun = 0;
static uint32_t  u5hz     = U5_HZ_READ;   // 当前总线频率：读 80MHz，上传时降到 40MHz

// ★ U5 是多任务共享的：GIF 解码（core0 优先级 2）、音频播放（core0 优先级 1）、上传（主任务）。
//   没有锁时低优先级任务会在 SPI 事务中途被抢占 → 双方读到垃圾（音频爆音、GIF 花屏）。
//   只锁叶子函数（readRaw / write / eraseSector / eraseBlock）；eraseRange 内部逐个调用它们，
//   自己不加锁，避免嵌套死锁。
static SemaphoreHandle_t u5Mtx = nullptr;
struct U5Lock {
  bool got;
  U5Lock()  { got = (u5Mtx && xSemaphoreTake(u5Mtx, pdMS_TO_TICKS(2000)) == pdTRUE); }
  ~U5Lock() { if (got) xSemaphoreGive(u5Mtx); }
};

// ---------------- 底层 ----------------
// 作用域内临时降频（上传写 flash 时用），离开作用域自动恢复
struct U5HzGuard {
  uint32_t old;
  explicit U5HzGuard(uint32_t h) { old = u5hz; u5hz = h; }
  ~U5HzGuard() { u5hz = old; }
};

static inline void u5sel(bool on) { digitalWrite(U5_PIN_CS, on ? LOW : HIGH); }

static inline void u5cmd(uint8_t c) {
  u5spi.beginTransaction(SPISettings(u5hz, MSBFIRST, SPI_MODE0));
  u5sel(true);
  u5spi.transfer(c);
}
static inline void u5end() { u5sel(false); u5spi.endTransaction(); }

static inline void u5addr(uint32_t a) {
  u5spi.transfer((uint8_t)(a >> 16));
  u5spi.transfer((uint8_t)(a >> 8));
  u5spi.transfer((uint8_t)a);
}

static void u5busy() {
  uint32_t t0 = millis();
  for (;;) {
    u5cmd(U5_RDSR1);
    uint8_t sr = u5spi.transfer(0);
    u5end();
    if (!(sr & 0x01)) return;
    if (millis() - t0 > 500) {
      Serial.printf("[u5] busy timeout sr=%02X\n", sr);
      return;
    }
    // 不再 delayMicroseconds(100)：轮询本身要几 µs，最小等待粒度 100µs 会让
    // 每个编程周期都多等最多 100µs，8 页就是 0.8ms/块。
  }
}

static void u5readRaw(uint32_t addr, uint8_t *buf, size_t len) {
  U5Lock lk;
  u5cmd(U5_READF);
  u5addr(addr);
  u5spi.transfer(0);               // 8 个 dummy clock，这期间 MISO 上的字节作废
  while (len) {
    size_t k = len > U5_SECTOR ? U5_SECTOR : len;
    u5spi.transferBytes(u5dummy, buf, k);
    buf += k;
    len -= k;
  }
  u5end();
}

static void u5eraseSector(uint32_t addr) {
  U5Lock lk;
  u5cmd(U5_WREN); u5end();
  u5cmd(U5_SE);   u5addr(addr); u5end();
  u5busy();
}

static void u5eraseBlock(uint32_t addr) {
  U5Lock lk;
  u5cmd(U5_WREN); u5end();
  u5cmd(U5_BE);   u5addr(addr); u5end();
  u5busy();
}

// ★ 32KB 块擦：GD25Q128 支持（0x52，typ 120ms），比 64KB 块擦（0xD8，typ 150~200ms）
//   短一半左右。音频边播边擦时，这几十毫秒就是「读不到数据 → DMA 补 0」的窗口，
//   直接决定会不会听起来卡顿/发慢（实测 64KB 版本丢了 18% 的播放时间）。
static volatile uint32_t u5eraseMs = 0;      // 上一次擦除耗时，诊断用
static void u5eraseBlock32(uint32_t addr) {
  U5Lock lk;
  uint32_t t0 = micros();
  u5cmd(U5_WREN); u5end();
  u5cmd(U5_BE32); u5addr(addr); u5end();
  u5busy();
  u5eraseMs = (micros() - t0) / 1000U;
}

// 一次最多写一页（256B），且不跨页
static void u5writeChunk(uint32_t addr, const uint8_t *buf, size_t len) {
  u5cmd(U5_WREN); u5end();
  u5cmd(U5_PP);   u5addr(addr);
  u5spi.writeBytes(buf, len);
  u5end();
  u5busy();
}

static void u5write(uint32_t addr, const uint8_t *buf, size_t len) {
  U5Lock lk;
  while (len) {
    uint32_t room = U5_PAGE - (addr & (U5_PAGE - 1));
    size_t   k    = len < room ? len : room;
    u5writeChunk(addr, buf, k);
    addr += k;
    buf  += k;
    len  -= k;
  }
}

static void u5eraseRange(uint32_t addr, uint32_t len, bool verbose) {
  uint32_t end    = addr + len;
  uint32_t totalKB = (len + 1023) / 1024;
  uint32_t lastKB = 0;

  // 优先用 64KB 块擦：GD25Q128 的块擦典型 150ms，而 16 个 4KB 扇区擦要
  // 16×30ms = 480ms，实测 956KB 从 7.1s 降到 1.5s 左右。
  // 只有首尾没对齐到 64KB 的部分才退回扇区擦。
  for (uint32_t a = addr; a < end; ) {
    if (((a & 0xFFFFUL) == 0) && (a + 0x10000UL <= end)) {
      u5eraseBlock(a);
      a += 0x10000UL;
    } else {
      u5eraseSector(a);
      a += U5_SECTOR;
    }
    if (verbose) {
      uint32_t kb = ((a > end ? end : a) - addr) / 1024;
      if (kb >= lastKB + 128 || a >= end) {
        Serial.printf("[u5] erase %lu/%lu KB\n",
                      (unsigned long)kb, (unsigned long)totalKB);
        lastKB = kb;
      }
    }
  }
}

// ---------------- 初始化 ----------------
static bool u5init() {
  if (!u5Mtx) u5Mtx = xSemaphoreCreateMutex();   // 必须在任何读写之前
  U5HzGuard hz(U5_HZ_WRITE);       // 探测走保守频率，返回时自动升到读频率
  pinMode(U5_PIN_CS, OUTPUT);
  u5sel(false);
  u5spi.begin(U5_PIN_SCK, U5_PIN_MISO, U5_PIN_MOSI, U5_PIN_CS);

  uint8_t id[3] = { 0, 0, 0 };
  u5cmd(U5_JEDEC);
  id[0] = u5spi.transfer(0);
  id[1] = u5spi.transfer(0);
  id[2] = u5spi.transfer(0);
  u5end();

  Serial.printf("[u5] JEDEC %02X %02X %02X\n", id[0], id[1], id[2]);
  if (id[0] == 0x00 || id[0] == 0xFF) {
    Serial.println("[u5] 没应答 —— 检查 U5 焊接 / IO21(CS) / 3V3");
    u5ok = false;
    return false;
  }
  u5cap = 1UL << id[2];
  u5ok  = (id[0] == 0xC8);
  Serial.printf("[u5] %s %.0fMB  read %luMHz / write %luMHz HSPI(SPI3)\n",
                u5ok ? "GD25Q128" : "未知厂商", u5cap / 1048576.0,
                (unsigned long)(U5_HZ_READ / 1000000),
                (unsigned long)(U5_HZ_WRITE / 1000000));
  return u5ok;
}

// ---------------- CRC32（IEEE，和 Python zlib.crc32 一致） ----------------
static uint32_t u5crc(uint32_t crc, const uint8_t *p, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (-(int32_t)(crc & 1)));
  }
  return ~crc;
}

// ---------------- 上传 ----------------
// Stream::readBytes 是逐字节 timedRead：每字节一次 millis() + 一次虚函数调用，
// 实测只有 228 KB/s。USBCDC 的 read(uint8_t*, size_t) 是纯队列出队循环，
// 少掉这层开销。
//
// ★★ 两个坑（都踩过，各崩一次）：
//  ① 队列空时 read() 立刻返回 0，无脑连调会全速空转把 TinyUSB 饿死
//     → 先问 available()，没有就 delay(1)。
//  ② 一次 read 太多字节（试过 2048 / 512）会让 CPU 长时间出不了循环，
//     IDLE 任务得不到调度，直接触发 Interrupt WDT
//     （Guru Meditation: Core 1 panic'ed (Interrupt wdt timeout on CPU1)，板子重启）
//     → 单次上限 64 字节，刚好一个 USB 包，循环本身足够短。
//     ★ delay(0) 救不了：taskYIELD 只在同优先级间切换，IDLE 优先级更低。
static inline int u5rxBatch(Stream &s, uint8_t *buf, size_t n) {
  if (s.available() <= 0) { delay(1); return 0; }
  size_t k = n > 64 ? 64 : n;              // 一次一个 USB 包，别久占 CPU
  return (int)Serial.read(buf, k);         // CDCOnBoot=cdc 下 Serial 就是 USBCDC
}

static int u5readLine(Stream &s, char *buf, int cap, uint32_t timeoutMs) {
  int n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    int c = s.read();
    if (c < 0) { delay(1); continue; }
    if (c == '\n') { buf[n] = 0; return n; }
    if (c == '\r') continue;
    if (n < cap - 1) buf[n++] = (char)c;
    t0 = millis();                 // 有字节就续命
  }
  buf[n] = 0;
  return -1;
}

// 调用前 'U' 已经被吃掉。返回 true = 上传成功。
static bool u5upload(Stream &s, uint32_t base = U5_BASE) {
  if (!u5ok) { s.println("ERR noflash"); return false; }
  U5HzGuard hz(U5_HZ_WRITE);      // 擦/写一律 40MHz，退出时自动恢复读频率

  // 进入上传模式时，串口里还留着 "U\n" 的换行符（handleSerial 只吃了 'U'），
  // 不清掉的话 u5readLine 第一行就读到空行，会被当成未知命令回 ERR cmd。
  delay(20);
  while (s.available()) s.read();

  s.printf("UOK %d %lu\n", U5_SECTOR, (unsigned long)u5cap);

  uint32_t total = 0, wantCrc = 0, gotCrc = 0, written = 0, expectSeq = 1;
  uint32_t tStart = millis();
  uint32_t usRecv = 0, usWrite = 0, usCrc = 0;   // 耗时分解（诊断用）
  char line[80];

  for (;;) {
    int n = u5readLine(s, line, sizeof(line), 20000);
    if (n < 0) { s.println("ERR timeout"); Serial.println("[u5] upload timeout"); return false; }
    if (n == 0) continue;                    // 空行（残余 \r\n）直接跳过

    if (line[0] == 'H') {
      // 格式：H <total> <crc> [addr]     —— addr 省略时就写在 base（老脚本照旧可用）
      // ★ 专辑功能靠这个 addr：每条内容写到自己的 64KB 对齐地址，互不覆盖。
      unsigned long t = 0, c = 0, ad = 0;
      int nf = sscanf(line + 1, "%lu %lx %lx", &t, &c, &ad);
      if (nf < 2) { s.println("ERR header"); continue; }
      if (nf == 2) ad = base;
      total   = t;
      wantCrc = c;
      base    = (uint32_t)ad;
      if (total == 0 || base + total > u5cap) { s.println("ERR size"); return false; }

      Serial.printf("[u5] 擦除 %lu KB ...\n", (unsigned long)((total + 1023) / 1024));
      uint32_t t0 = millis();
      u5eraseRange(base, total, true);
      Serial.printf("[u5] 擦除完成 %.1fs\n", (millis() - t0) / 1000.0);

      written = 0; gotCrc = 0; expectSeq = 1;
      s.println("HOK");
    }
    else if (line[0] == 'D') {
      unsigned long seq = 0, len = 0;
      if (sscanf(line + 1, "%lu %lu", &seq, &len) != 2) { s.println("ERR data"); continue; }
      if (len == 0 || len > U5_BLK) { s.printf("ERR len %lu\n", len); continue; }

      size_t got = 0;
      uint32_t t0 = micros();
      while (got < len && micros() - t0 < 5000000UL) {
        int r = u5rxBatch(s, u5blk + got, len - got);   // 内部已按需 delay(1)
        if (r > 0) got += r;
      }
      usRecv += micros() - t0;
      if (got != len) { s.printf("ERR short %lu/%lu\n", (unsigned long)got, len); return false; }

      uint32_t t1 = micros();
      u5write(base + written, u5blk, len);
      usWrite += micros() - t1;
      uint32_t t2 = micros();
      gotCrc  = u5crc(gotCrc, u5blk, len);
      usCrc   += micros() - t2;
      written += len;
      s.printf("A %lu\n", seq);
    }
    else if (line[0] == 'E') {
      if (written != total) {
        s.printf("EBAD %lu %lu\n", (unsigned long)written, (unsigned long)total);
        return false;
      }
      if (gotCrc != wantCrc) {
        s.printf("EBAD crc %08lX %08lX\n", (unsigned long)gotCrc, (unsigned long)wantCrc);
        return false;
      }
      s.println("EOK");
      Serial.printf("[u5] 上传完成 %lu 字节，CRC %08lX，%.1fs\n",
                    (unsigned long)written, (unsigned long)gotCrc,
                    (millis() - tStart) / 1000.0);
      {
        float sec = (millis() - tStart) / 1000.0f;
        uint32_t usSum = usRecv + usWrite + usCrc;
        Serial.printf("[u5] 耗时分解: 收数据 %.1fs  写flash %.1fs  crc %.1fs  其它 %.1fs\n",
                      usRecv / 1e6, usWrite / 1e6, usCrc / 1e6,
                      (usSum < (uint32_t)(sec * 1e6))
                          ? (sec - usSum / 1e6f) : 0.0f);
      }
      return true;
    }
    else {
      s.println("ERR cmd");
    }
  }
}
