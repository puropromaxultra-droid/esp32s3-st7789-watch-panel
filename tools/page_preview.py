# -*- coding: utf-8 -*-
# 离线复刻 weather_clock 的绘图原语，把电池页三种状态渲染成 PNG 检查版式
import re, math
from PIL import Image

SRC = r'C:\Users\ZhuanZ\Desktop\电子屏显示\weather_clock\weather_clock.ino'
s = open(SRC, encoding='utf-8').read()

# ---- 解析字库 ----
m = re.search(r'font5x7\[\]\[5\]\s*PROGMEM\s*=\s*\{(.*?)\n\};', s, re.S)
groups = re.findall(r'\{([^}]*)\}', m.group(1))
FONT = [[int(v, 16) for v in g.split(',')] for g in groups]
assert len(FONT) == 95, len(FONT)

W = H = 240


def c565(v):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return ((r * 255) // 31, (g * 255) // 63, (b * 255) // 31)


C_BG = 0x0000; C_CARD = 0x10A2; C_LINE = 0x2124; C_TRACK = 0x18E3
C_WHITE = 0xFFFF; C_YEL = 0xFFE0; C_CYAN = 0x07FF; C_GREEN = 0x07E0
C_RED = 0xF800; C_ORG = 0xFD20; C_GRAY = 0x8410; C_GRAY2 = 0x630C
BAT_CHG, BAT_FULL, BAT_DSG = 1, 2, 0


class Canvas:
    def __init__(self):
        self.im = Image.new('RGB', (W, H), c565(C_BG))
        self.px = self.im.load()

    def fillRect(self, x, y, w, h, c):
        col = c565(c)
        for yy in range(max(0, y), min(H, y + h)):
            for xx in range(max(0, x), min(W, x + w)):
                self.px[xx, yy] = col

    def hline(self, x0, x1, y, c):
        self.fillRect(min(x0, x1), y, abs(x1 - x0) + 1, 1, c)

    def disc(self, cx, cy, r, c):
        if r <= 0:
            return
        for y in range(-r, r + 1):
            dx = int(math.sqrt(max(0, r * r - y * y)))
            self.hline(cx - dx, cx + dx, cy + y, c)

    def rrect(self, x, y, w, h, r, c):
        if w <= 0 or h <= 0:
            return
        if r * 2 > w:
            r = w // 2
        if r * 2 > h:
            r = h // 2
        if r <= 0:
            self.fillRect(x, y, w, h, c)
            return
        self.fillRect(x + r, y, w - 2 * r, h, c)
        self.fillRect(x, y + r, r, h - 2 * r, c)
        self.fillRect(x + w - r, y + r, r, h - 2 * r, c)
        self.disc(x + r, y + r, r, c)
        self.disc(x + w - r - 1, y + r, r, c)
        self.disc(x + r, y + h - r - 1, r, c)
        self.disc(x + w - r - 1, y + h - r - 1, r, c)

    def glyph(self, x, y, ch, c, sc):
        if not (32 <= ord(ch) <= 126):
            ch = '?'
        g = FONT[ord(ch) - 32]
        for col in range(5):
            bits = g[col]
            for row in range(7):
                if bits & (1 << row):
                    self.fillRect(x + col * sc, y + row * sc, sc, sc, c)

    def textW(self, st, sc):
        n = len(st)
        return n * 6 * sc - sc if n else 0

    def text(self, st, x, y, c, sc):
        for ch in st:
            self.glyph(x, y, ch, c, sc)
            x += 6 * sc

    def textC(self, st, cy, c, sc):
        self.text(st, (W - self.textW(st, sc)) // 2, cy, c, sc)


def drawBatteryIcon(cv, x, y, w, h, pct, state, pulse):
    col = C_RED if pct <= 15 else (C_ORG if pct <= 35 else C_GREEN)
    if state == BAT_CHG:
        col = C_CYAN
    cv.rrect(x, y, w, h, 8, C_GRAY2)
    cv.fillRect(x + 3, y + 3, w - 6, h - 6, C_BG)
    cv.rrect(x + w, y + h // 3, 7, h // 3, 3, C_GRAY2)
    iw = (w - 6) * pct // 100
    if iw < 0:
        iw = 0
    if iw > 0:
        cv.rrect(x + 3, y + 3, iw, h - 6, 4, col)
    if state == BAT_CHG and pulse and iw > 16:
        cv.fillRect(x + 3 + iw - 8, y + 3, 8, h - 6, C_WHITE)


def drawBatPage(cv, pct, volt, state, mins, pulse, page=5, pages=7):
    # drawPageTag("BATTERY")
    cv.text("BATTERY", 14, 10, C_CYAN, 2)
    b = "%d/%d" % (page + 1, pages)
    cv.text(b, W - 14 - cv.textW(b, 2), 10, C_GRAY, 2)
    cv.hline(14, W - 14, 32, C_LINE)

    drawBatteryIcon(cv, 36, 46, 160, 54, pct, state, pulse)

    t = "%d" % pct
    pw = cv.textW(t, 5)
    cv.text(t, (W - pw) // 2 - 8, 106, C_WHITE, 5)
    cv.text("%", (W + pw) // 2 - 4, 118, C_GRAY, 2)

    if state == BAT_CHG:
        st, stc = "CHARGING", C_CYAN
    elif state == BAT_FULL:
        st, stc = "FULL", C_GREEN
    elif pct <= 15:
        st, stc = "LOW BATTERY", C_RED
    else:
        st, stc = "DISCHARGING", C_ORG
    cv.textC(st, 148, stc, 2)

    cv.textC("%.2fV" % volt, 168, C_GRAY, 1)

    if state == BAT_CHG:
        tb = "FULL SOON" if pct >= 99 else "FULL IN %d:%02d" % (mins // 60, mins % 60)
        tc = C_YEL
    elif state == BAT_FULL:
        tb, tc = "ALL CHARGED", C_GREEN
    else:
        tb, tc = "LEFT %d:%02d" % (mins // 60, mins % 60), C_WHITE
    cv.textC(tb, 182, tc, 2)

    cv.textC("600mAh  CHG 255mA  LOAD 210mA", 210, C_GRAY2, 1)
    cv.textC("B:next   long:home", 224, C_GRAY2, 1)


cases = [
    (78, 3.96, BAT_DSG, 133, 0),
    (42, 4.02, BAT_CHG, 91, 1),
    (100, 4.19, BAT_FULL, 0, 0),
]
out = Image.new('RGB', (W * 3 + 20, H), (30, 30, 30))
for i, (p, v, st, mn, pu) in enumerate(cases):
    cv = Canvas()
    drawBatPage(cv, p, v, st, mn, pu)
    out.paste(cv.im, (i * (W + 10), 0))
dst = r'C:\Users\ZhuanZ\Desktop\电子屏显示\_bat_preview.png'
out.save(dst)
print('saved', dst)
