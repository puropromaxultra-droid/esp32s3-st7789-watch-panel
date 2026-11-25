// ============================================================
// webstream.h —— 手机浏览器点播的 HTTP 服务端
//
// 为什么手写 HTTP 而不是用 WebServer/ESPAsyncWebServer：
//   这块板子空闲堆只有 ~20KB（无 PSRAM），那两个库光对象和 String
//   拼装就要吃掉十几 KB。这里全部用固定 char 缓冲，RAM 占用几十字节。
//
// 路由：
//   GET  /                    播放页（www.h，存在 flash 里）
//   GET  /start?len=&name=    开一次新的播放流（重置环形缓冲 + 预擦）
//   POST /pcm                 推 PCM 块（40kHz 单声道 16bit LE）
//   GET  /eos                 数据发完了
//   GET  /ctl?c=pause|resume|stop|vol&v=N
//   GET  /status              JSON 状态
//
// ★ 背压：/pcm 在环形缓冲满时不返回，TCP 窗口收缩 → 浏览器 fetch 挂住 →
//   发送速度自动等于播放速度。手机端不用限速，也灌不爆 40 秒缓冲。
// ============================================================
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "u5flash.h"
#include "spk.h"
#include "player.h"
#include "www.h"

static WiFiServer      *wsSrv   = nullptr;
static volatile bool    wsUp    = false;
static volatile uint32_t wsReq  = 0;      // 处理过的请求数
static volatile uint32_t wsByte = 0;      // 累计收到的 PCM 字节
static volatile uint32_t wsMaxBlk = 0;    // 单次 /pcm 最大块

// 读一行（不带 \r\n）。返回长度，0 = 空行/超时/断开。
static int wsLine(WiFiClient &c, char *b, int n, uint32_t toMs) {
  int i = 0;
  uint32_t t0 = millis();
  while (i < n - 1) {
    if (c.available()) {
      int ch = c.read();
      if (ch < 0) break;
      if (ch == '\n') break;
      if (ch == '\r') continue;
      b[i++] = (char)ch;
      t0 = millis();
    } else {
      if (!c.connected()) break;
      if (millis() - t0 > toMs) break;
      vTaskDelay(1);
    }
  }
  b[i] = 0;
  return i;
}

// %XX / + 就地解码（浏览器 encodeURIComponent 出来的）
static void wsDecode(char *s) {
  char *o = s;
  while (*s) {
    if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
      char h[3] = { s[1], s[2], 0 };
      *o++ = (char)strtol(h, nullptr, 16);
      s += 3;
    } else if (*s == '+') {
      *o++ = ' ';
      s++;
    } else {
      *o++ = *s++;
    }
  }
  *o = 0;
}

// 从 query 里取 key=value
static bool wsArg(const char *q, const char *k, char *out, int n) {
  char pat[20];
  snprintf(pat, sizeof(pat), "%s=", k);
  const char *p = strstr(q, pat);
  if (!p) return false;
  p += strlen(pat);
  int i = 0;
  while (*p && *p != '&' && i < n - 1) out[i++] = *p++;
  out[i] = 0;
  return true;
}

static void wsHead(WiFiClient &c, const char *ct, int len) {
  char h[192];
  int n = snprintf(h, sizeof(h),
                   "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                   "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
                   "Connection: close\r\n\r\n", ct, len);
  c.write((const uint8_t *)h, (size_t)n);
}

static void wsText(WiFiClient &c, const char *s) {
  int n = (int)strlen(s);
  wsHead(c, "text/plain; charset=utf-8", n);
  c.write((const uint8_t *)s, (size_t)n);
}

// ---------------- /pcm：核心 ----------
static void wsPcm(WiFiClient &c, long clen) {
  static uint8_t buf[1024];
  long got  = 0;
  uint32_t idle = millis();
  uint32_t t0   = millis();
  wsMaxBlk = 0;
  uint32_t sinceYield = 0;

  while (got < clen) {
    int n = c.read(buf, sizeof(buf));
    if (n <= 0) {
      if (!c.connected() || millis() - idle > 6000) break;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    idle = millis();
    got += n;
    wsByte += (uint32_t)n;
    if ((uint32_t)n > wsMaxBlk) wsMaxBlk = (uint32_t)n;

    size_t o = 0;
    while (o < (size_t)n) {
      size_t w = rnWrite(buf + o, (size_t)n - o);
      if (w) { o += w; continue; }
      // 缓冲满 / 这块还没擦：擦除由 rnEraseTask 负责（它挑"刚 refill 完"的时机擦），
      // 这里只要等水位推进。★ 千万别在这里自己调 rnEraseOne —— 那条 120ms 的擦除
      // 会占住 u5Mtx，把播放的 readRaw 挡住，正是之前卡顿的根源。
      vTaskDelay(pdMS_TO_TICKS(2));
      if (millis() - idle > 12000) { got = clen; break; }   // 卡死保护
    }
    // ★ 每写 8KB 让一次 CPU：本任务优先级 2、高于音频的 DMA 喂数节奏，
    //   不让一下的话一次 32KB 推流会连续占 40~50ms，DMA（30.7ms）就排空了。
    sinceYield += (uint32_t)n;
    if (sinceYield >= 8192) { sinceYield = 0; vTaskDelay(1); }
  }
  wsText(c, "OK");
  // 每 64 块打一次（≈10 秒一次）：频繁 printf 在 115200 下会偷帧，
  // 而这里又是每 0.16 秒就进来一次。
  static uint16_t cnt = 0;
  if (++cnt >= 64) {
    cnt = 0;
    Serial.printf("[ws] pcm %ld B %lums  avail=%luKB  under=%lu\n",
                  (long)got, (unsigned long)(millis() - t0),
                  (unsigned long)(rnAvail() / 1024), (unsigned long)rnUnder);
  }
}

static void wsStatus(WiFiClient &c) {
  char j[256];
  int n = snprintf(j, sizeof(j),
    "{\"stream\":%d,\"paused\":%d,\"eos\":%d,\"sr\":%lu,"
    "\"avail\":%lu,\"rd\":%lu,\"wr\":%lu,\"total\":%lu,\"vol\":%u,\"under\":%lu}",
    (int)mus.stream, (int)mus.paused, (int)mus.eos,
    (unsigned long)mus.sr,
    (unsigned long)rnAvail(), (unsigned long)mus.rd, (unsigned long)mus.wr,
    (unsigned long)mus.total, (unsigned)spkVol, (unsigned long)rnUnder);
  wsHead(c, "application/json", n);
  c.write((const uint8_t *)j, (size_t)n);
}

// ---------------- 总入口 ----------------
static void wsHandle(WiFiClient &c) {
  char line[200];
  if (wsLine(c, line, sizeof(line), 2500) <= 0) return;

  char method[8] = {0}, path[128] = {0};
  if (sscanf(line, "%7s %127s", method, path) != 2) return;

  long clen = 0;
  int guard = 0;
  while (wsLine(c, line, sizeof(line), 1500) > 0 && guard++ < 40) {
    if (!strncasecmp(line, "Content-Length:", 15)) clen = atol(line + 15);
  }

  char *q = strchr(path, '?');
  if (q) *q++ = 0; else q = path + strlen(path);
  wsReq = wsReq + 1;

  // ---- 首页 ----
  if (!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
    const size_t total = sizeof(WS_HTML) - 1;
    wsHead(c, "text/html; charset=utf-8", (int)total);
    size_t off = 0;
    while (off < total) {
      size_t n = total - off;
      if (n > 1460) n = 1460;
      if (c.write((const uint8_t *)WS_HTML + off, n) != n) break;
      off += n;
    }
    return;
  }

  // ---- 开始新流 ----
  if (!strcmp(method, "GET") && !strcmp(path, "/start")) {
    char v[96];
    uint32_t len = 0;
    if (wsArg(q, "len", v, sizeof(v))) len = (uint32_t)strtoul(v, nullptr, 10);
    char name[44] = "TRACK";
    if (wsArg(q, "name", v, sizeof(v))) { wsDecode(v); snprintf(name, sizeof(name), "%s", v); }
    wsByte = 0;
    if (rnStart(name, "", len)) {
      wsText(c, "OK");
    } else {
      wsText(c, "ERR");
    }
    return;
  }

  // ---- 推数据 ----
  if (!strcmp(method, "POST") && !strcmp(path, "/pcm")) {
    wsPcm(c, clen);
    return;
  }

  if (!strcmp(method, "GET") && !strcmp(path, "/eos")) {
    mus.eos = true;
    wsText(c, "OK");
    return;
  }

  // ---- 控制 ----
  if (!strcmp(method, "GET") && !strcmp(path, "/ctl")) {
    char v[24];
    if (wsArg(q, "c", v, sizeof(v))) {
      if (!strcmp(v, "pause"))       mus.paused = true;
      else if (!strcmp(v, "resume")) mus.paused = false;
      else if (!strcmp(v, "stop"))   rnStop();
      else if (!strcmp(v, "vol")) {
        if (wsArg(q, "v", v, sizeof(v))) {
          int x = atoi(v);
          if (x < 0) x = 0;
          if (x > 100) x = 100;
          spkVol = (uint8_t)x;
          spkMute = false;
        }
      }
    }
    wsText(c, "OK");
    return;
  }

  if (!strcmp(method, "GET") && !strcmp(path, "/status")) {
    wsStatus(c);
    return;
  }

  // ---- 404 ----
  wsText(c, "404");
}

static void wsTask(void *) {
  for (;;) {
    if (!wsSrv) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    WiFiClient c = wsSrv->accept();
    if (!c) { vTaskDelay(pdMS_TO_TICKS(12)); continue; }
    c.setTimeout(2);
    c.setNoDelay(true);
    wsHandle(c);
    c.stop();
  }
}

static bool wsBegin() {
  if (wsSrv) return true;
  wsSrv = new WiFiServer(80);
  if (!wsSrv) return false;
  wsSrv->begin();
  wsSrv->setNoDelay(true);
  if (xTaskCreatePinnedToCore(wsTask, "web", 6144, nullptr, 2, nullptr, 0) != pdPASS) {
    delete wsSrv;
    wsSrv = nullptr;
    Serial.println("[ws] 任务创建失败");
    return false;
  }
  wsUp = true;
  Serial.printf("[ws] 播放页 http://%s/  堆 %uK\n",
                WiFi.localIP().toString().c_str(), (unsigned)(ESP.getFreeHeap() / 1024));
  return true;
}

static void wsReport() {
  Serial.printf("[ws] up=%d req=%lu pcm=%luKB blk=%lu heap=%uK maxblk=%u\n",
                (int)wsUp, (unsigned long)wsReq, (unsigned long)(wsByte / 1024),
                (unsigned long)wsMaxBlk, (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)ESP.getMaxAllocHeap());
}
