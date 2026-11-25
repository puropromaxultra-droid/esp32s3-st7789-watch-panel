// ============================================================
// gif_store.h —— 从 U5 读 GIF blob（替代编进固件的 gif_data.h）
//
// blob 格式（全部小端，由 tools/gif2bin.py 生成）：
//   0   u32  magic "GIFB"
//   4   u16  version = 1
//   6   u16  width
//   8   u16  height
//   10  u16  frameCount
//   12  u32  dataBytes          RLE 数据区总字节
//   16  u32  crc32              RLE 数据区 CRC32
//   20  u32  dataOff            数据区在 blob 内的字节偏移
//   24  u16  delayMs[frameCount]
//       u32  frameOff[frameCount+1]   相对 dataOff 的字节偏移
//       ---- dataOff ----
//       RLE: 每段 4 字节 = count(u16) + color565(u16)
//
// 帧数据是逐帧预读到 PSRAM 再解码的：一帧最大约 40KB，40MHz 读只要 10ms，
// 而且不跟 LCD 的 SPI 抢总线（U5 在 HSPI，LCD 在 FSPI）。
// ============================================================

#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "u5flash.h"

#define GIFB_MAGIC 0x42464947UL       // 'G'|'I'<<8|'F'<<16|'B'<<24

#define GIF_MAX_FRAMES 256

struct GifStore {
  bool     ok      = false;
  uint16_t w       = 0;
  uint16_t h       = 0;
  uint16_t frames  = 0;
  uint32_t base    = 0;            // 本条 blob 在 U5 的起始地址（专辑里不是 0）
  uint32_t dataOff = 0;
  uint32_t dataBytes = 0;
  uint32_t crc     = 0;
  uint16_t delayMs[GIF_MAX_FRAMES];
  uint32_t off[GIF_MAX_FRAMES + 1];
  uint8_t *buf     = nullptr;      // 当前帧 RLE（内部 RAM）
  uint32_t bufCap  = 0;
  uint32_t needCap = 0;            // 最大帧需要多少字节（load 时算好，懒分配）
  uint32_t wantCap = 0;            // ★ 全专辑最大帧（setup 预分配用）
};

static GifStore gif;

static inline uint32_t gif_rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t gif_rd16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// ================= 专辑：U5 里存多条内容，可切换 =================
// 内容区 = 0x000000 ~ 0x1FFFFF（前 2MB，2MB 之上是音乐环形缓冲，别碰）。
// 索引单独占这 2MB 的**最后一个 4KB 扇区**（0x1FF000），**不碰内容区** —— 所以老固件
// 写的那条 GIF 不用重传：扫不到索引时自动把 0 地址的 blob 当成第 1 条。
// ★ 早期版本把索引放在 0xFF000，白扔了 1MB 空间；现在优先读新地址，读不到再回退旧地址，
//   所以老索引不会丢（album.py add 一次就会写到新地址，完成迁移）。
//
// 索引扇区：  "GIDX" | ver=2 | count | dataEnd | 条目[count] × 24B
// 条目：      off u32 | bytes u32 | frames u16 | w u16 | h u16 | kind u8 | pad u8 | name[8]
//             kind: 0 = 单帧图，1 = 动图（frames > 1）
//
// 每条内容都是一个完整的 GIFB blob（单帧图 = frames=1 的 blob），起始地址 64KB 对齐，
// 这样擦除时不会误伤邻居。
#define ALB_MAX        12
#define ALB_IDX_ADDR      0x1FF000UL   // 2MB 内容区的最后一个扇区
#define ALB_IDX_ADDR_OLD  0x0FF000UL   // 老固件放这儿（只读，兼容用）
#define ALB_MAGIC     0x58444947UL   // 'G','I','D','X'
#define ALB_DATA_MIN  0x010000UL     // 新条目最早从这里开始
#define ALB_NAME      8
#define ALB_ENT       24

struct AlbumEntry {
  uint32_t off   = 0;
  uint32_t bytes = 0;
  uint16_t frames = 0;
  uint16_t w = 0, h = 0;
  uint8_t  kind = 1;
  char     name[ALB_NAME + 1] = {0};
};

static AlbumEntry album[ALB_MAX];
static uint8_t    albumN   = 0;
static uint8_t    albumIdx = 0;
static uint32_t   albumEnd = 0;      // 已用到的末地址（下一条从这里往上追加）

static inline uint32_t albAlign64k(uint32_t v) { return (v + 0xFFFFUL) & ~0xFFFFUL; }

// 读 off 处的 blob 头，填一条。不是 GIFB 返回 false。
static bool albFromBlob(uint32_t off, AlbumEntry &e) {
  uint8_t h[24];
  u5readRaw(off, h, sizeof(h));
  if (gif_rd32(h) != GIFB_MAGIC) return false;
  e.off    = off;
  e.w      = gif_rd16(h + 6);
  e.h      = gif_rd16(h + 8);
  e.frames = gif_rd16(h + 10);
  e.bytes  = gif_rd32(h + 20) + gif_rd32(h + 12);   // dataOff + dataBytes
  e.kind   = (e.frames > 1) ? 1 : 0;
  return e.frames > 0 && e.w > 0;
}

static void albumScan() {
  albumN = 0; albumIdx = 0; albumEnd = 0;
  if (!u5ok) return;

  uint8_t h[16];
  uint32_t idxAddr = ALB_IDX_ADDR;
  u5readRaw(idxAddr, h, sizeof(h));
  if (!(gif_rd32(h) == ALB_MAGIC && gif_rd16(h + 4) == 2)) {
    // 回退：早期固件把索引写在 0xFF000 —— 读得到就继续用它，下次 add 会迁到新地址
    u5readRaw(ALB_IDX_ADDR_OLD, h, sizeof(h));
    if (gif_rd32(h) == ALB_MAGIC && gif_rd16(h + 4) == 2) {
      idxAddr = ALB_IDX_ADDR_OLD;
      Serial.println("[alb] 老索引 @0xFF000，add 一次即迁移到 0x1FF000");
    }
  }
  if (gif_rd32(h) == ALB_MAGIC && gif_rd16(h + 4) == 2) {
    uint16_t n = gif_rd16(h + 6);
    if (n > ALB_MAX) n = ALB_MAX;
    albumEnd = gif_rd32(h + 8);
    if (n) {
      uint8_t raw[ALB_MAX * ALB_ENT];
      u5readRaw(idxAddr + 16, raw, (size_t)n * ALB_ENT);
      for (uint16_t i = 0; i < n; i++) {
        const uint8_t *p = raw + (size_t)i * ALB_ENT;
        AlbumEntry &e = album[i];
        e.off    = gif_rd32(p);
        e.bytes  = gif_rd32(p + 4);
        e.frames = gif_rd16(p + 8);
        e.w      = gif_rd16(p + 10);
        e.h      = gif_rd16(p + 12);
        e.kind   = p[14];
        memcpy(e.name, p + 16, ALB_NAME);
        e.name[ALB_NAME] = 0;
      }
    }
    albumN = (uint8_t)n;
    if (!albumEnd && n) albumEnd = albAlign64k(album[n - 1].off + album[n - 1].bytes);
    Serial.printf("[alb] 索引 %u 条，已用到 %lu KB\n", (unsigned)n, (unsigned long)(albumEnd / 1024));
    return;
  }

  // —— 老格式：0 地址只有一条 ——
  AlbumEntry e;
  if (albFromBlob(U5_BASE, e)) {
    strncpy(e.name, "legacy", ALB_NAME);
    album[0] = e;
    albumN   = 1;
    albumEnd = albAlign64k(e.off + e.bytes);
    Serial.printf("[alb] 无索引，按老单条处理：%u 帧 %lu KB\n",
                  (unsigned)e.frames, (unsigned long)(e.bytes / 1024));
  } else {
    Serial.println("[alb] U5 里没有内容");
  }
}

// 把 RAM 里的 album[] 写回索引扇区（PC 端整表发过来之后调用）
static bool albumSave() {
  if (!u5ok) return false;
  static uint8_t buf[16 + ALB_MAX * ALB_ENT];
  memset(buf, 0, sizeof(buf));
  buf[0] = 'G'; buf[1] = 'I'; buf[2] = 'D'; buf[3] = 'X';
  buf[4] = 2; buf[5] = 0;
  buf[6] = albumN; buf[7] = 0;
  buf[8]  = (uint8_t)(albumEnd);
  buf[9]  = (uint8_t)(albumEnd >> 8);
  buf[10] = (uint8_t)(albumEnd >> 16);
  buf[11] = (uint8_t)(albumEnd >> 24);
  for (uint8_t i = 0; i < albumN; i++) {
    uint8_t *p = buf + 16 + (size_t)i * ALB_ENT;
    const AlbumEntry &e = album[i];
    memcpy(p,      &e.off,    4);
    memcpy(p + 4,  &e.bytes,  4);
    memcpy(p + 8,  &e.frames, 2);
    memcpy(p + 10, &e.w,      2);
    memcpy(p + 12, &e.h,      2);
    p[14] = e.kind;
    p[15] = 0;
    memcpy(p + 16, e.name, ALB_NAME);
  }
  size_t n = 16 + (size_t)albumN * ALB_ENT;
  {
    U5HzGuard hz(U5_HZ_WRITE);
    U5Lock    lk;
    u5eraseSector(ALB_IDX_ADDR);
    u5write(ALB_IDX_ADDR, buf, n);
  }
  uint8_t chk[16];
  u5readRaw(ALB_IDX_ADDR, chk, 16);
  bool ok = (gif_rd32(chk) == ALB_MAGIC && gif_rd16(chk + 6) == albumN);
  Serial.printf("[alb] 写索引 %s（%u 条，%u 字节）\n", ok ? "OK" : "FAIL",
                (unsigned)albumN, (unsigned)n);
  return ok;
}

// 扫全专辑，返回最大的单帧字节数（只读 24B header + 偏移表，不读数据）。
// ★ 用途：setup 里按"最胖的那条"一次性预留缓冲。原来只按当前 albumIdx 那条分配，
//   所以后加一条帧更大的 GIF 时，运行中 maxblk 只剩 ~32KB → malloc 失败 → 屏上不显示
//   （日志 "[gif] 分配 40804 字节失败"），必须重新上电才可能装得下。
static uint32_t gifMaxFrameAll() {
  static uint32_t offs[GIF_MAX_FRAMES + 1];
  uint32_t need = 0;
  for (uint8_t k = 0; k < albumN; k++) {
    const AlbumEntry &e = album[k];
    if (e.frames == 0 || e.frames > GIF_MAX_FRAMES || !e.bytes) continue;
    uint32_t oaddr = e.off + 24 + (uint32_t)e.frames * 2;
    oaddr = (oaddr + 3) & ~3UL;
    u5readRaw(oaddr, (uint8_t *)offs, ((size_t)e.frames + 1) * 4);
    if (offs[0] != 0) continue;                  // 表不自洽，跳过这条
    for (uint16_t i = 0; i < e.frames; i++) {
      if (offs[i + 1] < offs[i]) continue;
      uint32_t sz = offs[i + 1] - offs[i];
      if (sz > need) need = sz;
    }
  }
  return need;
}

static void gifFree() {
  if (gif.buf) { heap_caps_free(gif.buf); gif.buf = nullptr; }
  gif.bufCap = 0;
}

// 读 base 处的 blob（header + 表），按最大帧准备缓冲。失败返回 false（gif.ok = false）。
// ★ base 来自专辑条目：每条内容起始 64KB 对齐，互不覆盖。
static bool gifLoadAt(uint32_t base) {
  gif.ok = false;
  gif.frames = 0;

  if (!u5ok) { Serial.println("[gif] U5 不可用，跳过"); return false; }

  uint8_t hdr[24];
  u5readRaw(base, hdr, sizeof(hdr));

  // 80MHz Fast Read 在个别批次的 flash 上会读歪 —— 读不到魔数就自动降到 40MHz 再试一次，
  // 别让它被误判成"U5 里没有 GIF"。
  if (gif_rd32(hdr) != GIFB_MAGIC && u5hz > U5_HZ_WRITE) {
    u5hz = U5_HZ_WRITE;
    u5readRaw(base, hdr, sizeof(hdr));
    Serial.printf("[gif] 80MHz 读不对，已降到 %luMHz\n", (unsigned long)(U5_HZ_WRITE / 1000000));
  }

  uint32_t magic = gif_rd32(hdr);
  if (magic != GIFB_MAGIC) {
    Serial.printf("[gif] U5 里没有 GIF（magic=%08lX），等待上传\n", (unsigned long)magic);
    return false;
  }

  uint16_t ver    = gif_rd16(hdr + 4);
  gif.w           = gif_rd16(hdr + 6);
  gif.h           = gif_rd16(hdr + 8);
  gif.frames      = gif_rd16(hdr + 10);
  gif.dataBytes   = gif_rd32(hdr + 12);
  gif.crc         = gif_rd32(hdr + 16);
  gif.dataOff     = gif_rd32(hdr + 20);
  gif.base        = base;               // ★ gifLoadFrame 读帧要用它（漏了这行会一直读 0 地址）

  if (ver != 1 || gif.frames == 0 || gif.frames > GIF_MAX_FRAMES) {
    Serial.printf("[gif] blob 头异常 ver=%u frames=%u\n", ver, gif.frames);
    return false;
  }

  // 延时表
  uint8_t dtmp[GIF_MAX_FRAMES * 2];
  u5readRaw(base + 24, dtmp, (size_t)gif.frames * 2);
  for (uint16_t i = 0; i < gif.frames; i++) gif.delayMs[i] = gif_rd16(dtmp + i * 2);

  // 偏移表
  uint8_t otmp[(GIF_MAX_FRAMES + 1) * 4];
  uint32_t oaddr = base + 24 + (uint32_t)gif.frames * 2;
  // 和 Python 端一致：偏移表紧跟延时表，按 4 字节对齐
  oaddr = (oaddr + 3) & ~3UL;
  u5readRaw(oaddr, otmp, (size_t)(gif.frames + 1) * 4);
  for (uint16_t i = 0; i <= gif.frames; i++) gif.off[i] = gif_rd32(otmp + i * 4);

  if (gif.off[0] != 0 || gif.off[gif.frames] != gif.dataBytes) {
    Serial.printf("[gif] 偏移表不自洽 off[0]=%lu off[n]=%lu data=%lu\n",
                  (unsigned long)gif.off[0], (unsigned long)gif.off[gif.frames],
                  (unsigned long)gif.dataBytes);
    return false;
  }

  // 按最大帧分配
  uint32_t need = 0;
  for (uint16_t i = 0; i < gif.frames; i++) {
    uint32_t sz = gif.off[i + 1] - gif.off[i];
    if (sz > need) need = sz;
  }
  if (need == 0) { Serial.println("[gif] 帧长度为 0"); return false; }

  // ★★ 优先**复用**已经拿到的缓冲：堆一旦被 fb(115KB)、HTTP、音频切碎，
  //   再要 37KB 连续块很容易失败（实测这时候 maxblk 只剩 25KB）。
  //   只有新条目的帧更大时才真去申请 —— 申请前先释放旧的，避免同时占两份。
  // ★ 申请尺寸取 max(need, wantCap)：wantCap 是 setup 里按全专辑最大帧算好的预留值，
  //   这样"某一条帧特别大"不会把整条播放卡死（拿不到预留就退回只保证这一条）。
  uint32_t want = (need > gif.wantCap) ? need : gif.wantCap;
  if (!gif.buf || gif.bufCap < want) {
    gifFree();
    gif.buf = (uint8_t *)heap_caps_malloc(want, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!gif.buf && want > need) {
      gif.buf = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (gif.buf) want = need;
    }
    if (!gif.buf) {
      Serial.printf("[gif] 分配 %lu 字节失败（堆不够）\n", (unsigned long)need);
      return false;
    }
    gif.bufCap = want;
  }
  gif.needCap = need;
  gif.ok      = true;

  Serial.printf("[gif] U5@%luK: %u 帧 %dx%d  %lu KB  max %.0f KB/frame  buf %lu B\n",
                (unsigned long)(base / 1024),
                gif.frames, gif.w, gif.h,
                (unsigned long)(gif.dataBytes / 1024),
                need / 1024.0, (unsigned long)gif.bufCap);
  return true;
}

// 载入专辑第 i 条。失败就退回原来那条（不留半截状态）。
static bool gifSelect(uint8_t i) {
  if (i >= albumN) return false;
  uint8_t old = albumIdx;
  albumIdx = i;
  if (gifLoadAt(album[i].off)) return true;
  albumIdx = old;
  gifLoadAt(album[old].off);
  return false;
}

// 载入当前选中的那条（.ino 里原来的 gifLoad() 调用点不用改）
// ★ albumN == 0 时返回 false 而不是退回 U5_BASE：清空专辑后要真的显示 "NO GIF DATA"，
//   不能因为 0 地址还残留着旧数据就把它当内容。
static bool gifLoad() {
  if (!albumN) return false;
  return gifLoadAt(album[albumIdx < albumN ? albumIdx : 0].off);
}

// 真正把帧缓冲拿到手。已经在手上就是 true。失败返回 false（GIF 页会画空）。
static bool gifEnsureBuf() {
  if (gif.buf) return true;
  if (!gif.ok || !gif.needCap) return false;
  // 内部 RAM 优先：双核流水线里 core0 读 RLE、core1 推屏同时进行，
  // RLE 放 PSRAM 会让两边抢总线（这块板子本来也没 PSRAM，纯保险）。
  gif.buf = (uint8_t *)heap_caps_malloc(gif.needCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!gif.buf) {
    Serial.printf("[gif] 分配 %lu 字节失败（堆不够，GIF 页将空白）\n",
                  (unsigned long)gif.needCap);
    return false;
  }
  gif.bufCap = gif.needCap;
  #if !defined(QUIET_GIF_ALLOC)
  Serial.printf("[gif] 分配帧缓冲 %lu 字节\n", (unsigned long)gif.bufCap);
  #endif
  return true;
}

static uint32_t gifLoadFrame(uint16_t i);   // 前置声明：下面的自检要用

// 一帧 RLE 的指纹（FNV-1a，含长度）：同一份数据两次读出来指纹必须一致。
// 不额外占内存，比 memcmp 更适合查"偶发误码"。
static uint32_t gifFrameSig(uint16_t f) {
  uint32_t len = gifLoadFrame(f);
  uint32_t h = 2166136261UL;
  for (uint32_t i = 0; i < len; i++) { h ^= gif.buf[i]; h *= 16777619UL; }
  return h ^ (len << 24) ^ len;
}

// 双读比对：同一帧从 U5 连读两遍，比较指纹。串口 'x' 触发。
// 80MHz Fast Read 在个别批次 flash / 长走线上可能误码，症状是画面偶发错块（看起来像"割裂"）。
// 有差异 = 读不可靠，把 U5_HZ_READ 降一档（40MHz）；无差异 = 数据没问题，问题在屏或节拍。
static void gifReadTest(uint16_t nFrames) {
  if (!gif.ok) { Serial.println("[u5t] 没有 GIF 数据"); return; }
  uint16_t n = (nFrames < gif.frames) ? nFrames : gif.frames;
  int bad = 0;
  uint32_t t0 = micros();
  for (uint16_t f = 0; f < n; f++) {
    uint32_t a = gifFrameSig(f);
    uint32_t b = gifFrameSig(f);
    if (a != b) { Serial.printf("[u5t] f%u 指纹不同 %08lx / %08lx\n", f, (unsigned long)a, (unsigned long)b); bad++; }
  }
  Serial.printf("[u5t] 双读 %u 帧 %lu ms → 异常 %d 帧%s\n",
                n, (unsigned long)((micros() - t0) / 1000), bad,
                bad ? "  ★ 读不可靠，降频" : "  读干净");
}

// 把第 i 帧的 RLE 读进 gif.buf，返回字节数
static uint32_t gifLoadFrame(uint16_t i) {
  if (!gif.ok || i >= gif.frames || !gif.buf) return 0;
  uint32_t len = gif.off[i + 1] - gif.off[i];
  if (len > gif.bufCap) len = gif.bufCap;
  u5readRaw(gif.base + gif.dataOff + gif.off[i], gif.buf, len);
  return len;
}
