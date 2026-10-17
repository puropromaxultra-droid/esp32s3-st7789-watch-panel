/*
 * 手表拆机屏点亮测试（ESP32-S3 N16R8 + LovyanGFX）
 * ------------------------------------------------
 * 依赖：Arduino IDE 库管理器搜索 "LovyanGFX" 安装
 * 板型：ESP32S3 Dev Module（Flash 16MB，PSRAM 关闭即可，本例用不到）
 *
 * 接线（屏板 8 焊盘，顺序以板子丝印为准）：
 *   3V3  -> VCC（先试 3.3V；背光不亮再改接 5V）
 *   IO4  -> SCL   IO5  -> SDA
 *   IO6  -> DC    IO7  -> RES
 *   IO15 -> CS    IO16 -> BLK
 *   GND  -> GND
 *
 * 驱动 IC 未知。这种手表屏最常见的是 ST7789 / GC9A01 / GC9106，
 * 改下面 PANEL_XXX 宏逐个试；换宏后如仍黑屏，再把 spi_mode 改成 3、
 * invert 改成 false 再试。
 */

#define PANEL_ST7789
// #define PANEL_GC9A01
// #define PANEL_GC9106
// #define PANEL_ST7735
// #define PANEL_ILI9342

#define LCD_SCLK 4
#define LCD_MOSI 5
#define LCD_DC   6
#define LCD_RST  7
#define LCD_CS   15
#define LCD_BL   16

#include <LovyanGFX.hpp>

class WatchLCD : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
#if defined(PANEL_ST7789)
  lgfx::Panel_ST7789 _panel;
#elif defined(PANEL_GC9A01)
  lgfx::Panel_GC9A01 _panel;
#elif defined(PANEL_GC9106)
  lgfx::Panel_GC9106 _panel;
#elif defined(PANEL_ST7735)
  lgfx::Panel_ST7735S _panel;
#else
  lgfx::Panel_ILI9342 _panel;
#endif

public:
  WatchLCD() {
    auto bc = _bus.config();
    bc.spi_host    = SPI2_HOST;    // FSPI
    bc.spi_mode    = 0;            // 黑屏可试 3
    bc.freq_write  = 40000000;     // 花屏就降到 20000000
    bc.freq_read   = 16000000;
    bc.spi_3wire   = true;
    bc.use_lock    = true;
    bc.dma_channel = 0;
    bc.pin_sclk    = LCD_SCLK;
    bc.pin_mosi    = LCD_MOSI;
    bc.pin_miso    = -1;
    bc.pin_dc      = LCD_DC;
    _bus.config(bc);
    setBus(&_bus);

    auto pc = _panel.config();
    pc.pin_cs          = LCD_CS;
    pc.pin_rst         = LCD_RST;
    pc.pin_busy        = -1;
    pc.panel_width     = 240;
    pc.panel_height    = 240;      // 若画面上下/左右错位，先调 offset_x/offset_y
    pc.memory_width    = 240;
    pc.memory_height   = 240;
    pc.invert          = true;     // IPS 屏常为 true；颜色反了改 false
    pc.rgb_order       = false;    // 红/蓝互换就改 true
    pc.dummy_read_pixel= 8;
    pc.dummy_read_bits = 1;
    pc.readable        = false;
    pc.offset_x        = 0;
    pc.offset_y        = 0;
    _panel.config(pc);
    setPanel(&_panel);
  }
};

WatchLCD lcd;

// 依次铺满几种颜色，方便判断显示是否正常
void colorCycle() {
  const uint16_t colors[] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, TFT_BLACK };
  const char* names[]    = { "RED", "GREEN", "BLUE", "WHITE", "BLACK" };
  for (int i = 0; i < 5; i++) {
    lcd.fillScreen(colors[i]);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(colors[i] == TFT_BLACK ? TFT_WHITE : TFT_BLACK);
    lcd.drawString(names[i], 120, 120, &fonts::FreeSansBold12pt7b);
    delay(1500);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);      // 个别屏背光是低电平有效，黑屏时试 LOW

  lcd.init();
  lcd.setRotation(0);
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(TFT_GREEN, TFT_BLACK);
  lcd.drawString("Hello, watch LCD!", 120, 100, &fonts::FreeSansBold12pt7b);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.drawString("240x240 SPI test", 120, 140, &fonts::FreeSans12pt7b);
  Serial.println("LCD init done");
}

void loop() {
  colorCycle();
}
