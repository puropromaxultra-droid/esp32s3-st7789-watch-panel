// 最小验证固件：只闪背光
// 目的：分清"USB 掉线 + 屏黑"是固件问题还是硬件问题
//   背光闪  -> IO13、Q1、LED、屏全通
//   背光不闪 -> IO13 / Q1 / 屏 某处断
// 不碰 SPI、不碰 WiFi，排除一切干扰
#define PIN_BL 13

void setup() {
  pinMode(PIN_BL, OUTPUT);
}

void loop() {
  digitalWrite(PIN_BL, HIGH);
  delay(400);
  digitalWrite(PIN_BL, LOW);
  delay(400);
}
