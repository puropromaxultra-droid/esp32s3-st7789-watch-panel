# ESP32-S3 直驱手表拆机屏（ST7789 240×240）

> 把一块 3.5 元的智能手表拆机模块里的 ST7789 IPS 屏，用 FPC 转接板接到 ESP32-S3，
> 零第三方库手写驱动，GIF 动画跑满 16.7 fps。
>
> *Direct-driving a salvaged smartwatch ST7789 panel (240×240) with an ESP32-S3 — hand-written driver, no external display library.*

---

## 背景

淘宝「老王电子」卖的手表拆机模块（板号 WD40），屏是 `FPC-T1D3BP-129` 24 pin / 0.5 mm，
主控 GD32E230CBT6 自驱 ST7789，外挂 GD25Q128 存图。

**这块板不接收外部显示数据。** 它的 CN3（DAT/CLK）是 PF7/PF6 的 GPIO，在原厂固件里是软件 I2C
主机口，用来接传感器。所以从外部「喂」显示数据永远点不亮——唯一的路是把 FPC 排线拔下来，
用转接板直接驱动裸屏。

本仓库就是这条路走通之后的全部代码。

---

## 硬件

| 件 | 说明 |
|---|---|
| 屏 | ST7789 240×240 IPS，24 pin 0.5 mm FPC（从手表模块拆出） |
| 转接板 | FPC-24P 0.5 mm 转 2.54 mm 排针（翻盖式座，触点在腔底） |
| 主控 | ESP32-S3（本项目用 YD-ESP32-S3，N16R8） |
| 连线 | 杜邦线；26 MHz 实测稳定，40 MHz 起看线材质量 |

### 接线表

```
转接板排针                  →  ESP32-S3
1  (LED+)                   →  3V3
2  (LED-)                   →  GND
4  (VCC)                    →  3V3
5  (IOVCC)                  →  3V3
11 (RD)                     →  3V3      ← 串口模式选择，SPI 模式下也要拉高
23 (逻辑地)                 →  GND      ← ★ 必须接，只接 LED- 不够
9  (SCK)                    →  IO4
12 (SDA/MOSI)               →  IO5
8  (CS)                     →  IO6
10 (DC)                     →  IO7
7  (RST)                    →  IO8
6  (IM)                     →  IO9      ← ★ 必须为高电平，见下
```

> **注意：这张表是「镜像」的。** 转接板座子的 pin1 方向与屏排线丝印相反，硬插会把
> 3V3 撞到屏的 GND 触点上（会发烫）。本项目没有翻排线，而是把杜邦线按 `排针 n ↔ 屏触点 25-n`
> 映射改插到镜像位置，等效纠正。你若能自由选择排线方向，优先按物理正序插，别用镜像表。

---

## 三个坑（这个项目 90% 的时间都花在这里）

### 1. IM 脚必须为高电平 — 否则背光亮、屏全黑

ST7789 的 `IM0/IM1/IM2` 决定接口模式。**IM 为低时不是 4 线 SPI 模式，初始化指令完全不生效**，
症状是：背光正常亮、屏幕纯黑，连开机色块都不出来。

这条坑了整整一天，期间误判成「WiFi 库干扰」「固件太大」「杜邦线撑不住 20 MHz」，全是假象。
结论来自原厂开源工程 [`BA4IHS/GD32E230-LaoWang`](https://github.com/BA4IHS/GD32E230-LaoWang)，
其 `TFT_LCD_Init()` 第一行就是：

```c
TFT_LCD_IM12(1);   // SPI 模式
```

```cpp
digitalWrite(PIN_IM, HIGH);   // 不能省，也不能拉低
```

### 2. 逐字节 `SPI.transfer()` 是性能灾难

`SPI.transfer(uint8_t)` 每次调用都要等上一字节发送完成，实测 **1.5–2 µs/字节**。
一帧 240×240×2 = 115200 字节 → **约 200 ms，只有 5 fps**。
而且**提高 SPI 时钟完全没用**，因为瓶颈在 CPU 的调用开销，不在时钟。

换成批量推（`SPI.writeBytes()`，每批 16 行 = 7680 字节），并且在整帧期间保持 CS 低、
`0x2C` 只发一次（窗口在全屏初始化时设好），立刻变成 **约 35 ms/帧**。

| 方案 | 单帧耗时 | 帧率 |
|---|---|---|
| 10 MHz + 逐字节 `transfer()` | ~200 ms | ~5 fps |
| 26 MHz + `writeBytes()` 批量 | ~35 ms | **跑满 GIF 的 16.7 fps** |

### 3. 逻辑地必须单独接

只接 `LED-` 时背光能亮，但屏的逻辑地悬空 → 全黑。转接板的 1/2/22 三个触点都是 GND，
至少要保证逻辑地有回路。

---

## 目录

| 目录 | 说明 |
|---|---|
| **`lcd_fast/`** | ★ **主线程序**。ST7789 驱动 + GIF 播放，`writeBytes` 批量推、默认 26 MHz、帧率统计、串口切频 |
| `lcd_player/` | 前一代（逐字节 `transfer()`，可直接对比性能差异） |
| `bare_lcd_test/` | 最小验证程序：只做初始化 + 纯色 + 彩条，用来确认接线 |
| `lcd_demo/` | 绘图库：像素/线/矩形/圆/ASCII 文字，演示页 |
| `tools/gif2h.py` | GIF → RGB565 + RLE 头文件转换（缩放、抽帧、体积预算） |
| `tools/转换GIF.bat` | 把 GIF 拖上去就转换（Windows） |
| `tools/make_demo_gif.py` | 生成仓库自带的测试动画 |
| `demo/gif_data_demo.h` | demo 动画的转换结果，复制到 `lcd_fast/gif_data.h` 即可直接编译 |
| `weather_clock/` | ⚠️ 实验状态：WiFi + NTP + Open-Meteo 天气时钟，**IM 尚未修正，当前会黑屏** |
| `gif_player/` `gif_player2/` | GIF 播放的调试中间版本 |
| `wifi_lcd_probe/` `lcd_retry_probe/` `bare_pad_test/` `gd32_proxy_test/` `watch_lcd_test/` | 排查过程留下的探针程序（保留作记录，不必运行） |

---

## 快速开始

1. Arduino IDE + **esp32 core 4.0.0-alpha1**（其他 3.x 版本理论也行）
2. 按上面的接线表连线
3. 放一份动画数据：
   ```bat
   copy demo\gif_data_demo.h lcd_fast\gif_data.h
   ```
4. 打开 `lcd_fast/lcd_fast.ino`，选 ESP32-S3 烧录
5. 串口 115200，应看到红→绿→蓝→白→彩条自检，然后循环播放动画，每秒打印：
   ```
   fps=16.7  frame=35ms  spi=26MHz  heap=...
   ```

### 运行时切换 SPI 频率

串口直接发单个字符：

| 发送 | 频率 |
|---|---|
| `1` | 10 MHz |
| `2` | 26 MHz（默认） |
| `3` | 40 MHz |
| `4` | 60 MHz |
| `5` | 80 MHz |

出现花屏/错位/颜色异常就是信号质量到顶了，退回上一档即可。杜邦线上 26 MHz 稳，40 MHz 取决于线长。

---

## 换自己的 GIF

```bat
:: 直接拖 GIF 到 tools\转换GIF.bat 上，或：
python tools\gif2h.py your.gif -o lcd_fast\gif_data.h --max-kb 850 --fit cover
```

`gif2h.py` 会：缩放到 240×240（`--fit contain|cover`）→ RGB565 → RLE 压缩 →
超出 `--max-kb` 预算时自动均匀抽帧并倍增帧延时。

数据格式：`gifData[]` 是 `(run_length, color565)` 对，`gifFrameOffset[]` 记录每帧起始下标，
`gifDelayMs[]` 是每帧延时。MCU 端只做 RLE 解压 + 推 SPI，不依赖文件系统、不依赖第三方库。

**分区预算**：默认 1.2 MB app 分区，程序约 365 KB，动画建议 ≤ 850 KB。

---

## 已知问题 / 未完成

- **`weather_clock` 目前是坏的**：它写于 IM 结论之前，`PIN_IM` 仍是拉低 → 黑屏。
  修法照抄 `lcd_fast`（IM 拉高 + 原厂初始化序列即可），硬件不用动。
- 40 MHz 以上未做长时间稳定性验证。
- GIF 的帧率上限由源文件的帧间隔决定（本 demo 是 60 ms/帧）；屏幕和驱动本身还能更快，
  瓶颈在转换端。

---

## 致谢

- [`BA4IHS/GD32E230-LaoWang`](https://github.com/BA4IHS/GD32E230-LaoWang) — 原厂模块的开源固件，
  IM 电平与完整初始化序列的结论出自这里
- ST7789 datasheet（写周期 16 ns，SPI 最高约 62.5 MHz）

## License

MIT，见 [LICENSE](LICENSE)。
