# 桌面电子屏 · ESP32-S3 + 手表拆机 ST7789

> 一块 3.5 元的智能手表拆机屏，一颗 ESP32-S3，四层自制 PCB（34×50mm），
> 手写驱动、无第三方库：七页可交互界面、GIF 动画专辑、WiFi 天气时钟、手机推流放音乐。
>
> *A pocket-sized ESP32-S3 info panel driving a salvaged smartwatch ST7789 (240×240):
> hand-written driver, GIF album, weather clock and phone-streamed music. It also has
> a battery, a speaker and a custom 4-layer PCB.*
>
> **状态**：V5 板已打样、焊接、在用（2026-11）。代码、PCB、Gerber、BOM 都在本仓库。

---

## 一、能干什么

七页界面，短按 A 键翻页：

| 页 | 内容 |
|---|---|
| **GIF** | 240×240 动画，从外挂 flash 读，**支持多条专辑**（长按 A/B 切换，右上角显示 `2/5`） |
| **CLOCK** | 大号数字时间 + 日期 + 顶部天气（长按 B 立即刷新天气） |
| **FORECAST** | 三天卡片：日期、天气图标、最高/最低温、天气文字（长按重试） |
| **SUN** | 日出/日落时间 + 太阳轨迹弧与当前位置 + **月相**（名称、月龄、亮度%） |
| **MUSIC** | 音乐播放页：曲名（自动 1 行/2 行/跑马灯）、进度、**长按 A/B 调音量**（存 NVS） |
| **BAT** | 电池图标 + 百分比大字、电压、温度、充电状态（ADC + CHRG 脚） |
| **SYS** | WiFi 信号强度/IP、堆与 PSRAM、芯片温度、SPI 频率、运行时长 —— 排障用 |

**音乐**：手机浏览器打开 `http://<板子IP>/`，选本地音频文件，**由手机 JS 解码 + 重采样**
成 31250Hz/16bit/立体声，POST 到板子，写进外挂 flash 的 4MB 环形缓冲，再喂 I2S。

**开机**：Win98 风格四色旗 + 跑马灯 + 多声部开机音（可从按键跳过）。

**按键**：只有两个键。A = 翻页，B = 第二功能；**A+B 同按 700ms 回时钟**（随时逃生）。
长按按页面分派：MUSIC 音量、CLOCK 刷天气、GIF 切专辑、其余回时钟。

---

## 二、硬件

### 器件

| 件 | 型号 | 说明 |
|---|---|---|
| 屏 | ST7789 240×240 IPS | 手表拆机模块，`FPC-T1D3BP-129`，24P / 0.5mm |
| 主控 | **ESP32-S3-MINI-1-N8**（8MB，立创 C2913206） | LGA 底焊，板载天线（不是 1U） |
| 外挂 flash | **GD25Q128 16MB（U5）** | 存 GIF 专辑 + 音乐环形缓冲 + 音频 |
| 功放 | **MAX98357A（U4，QFN）** | I2S 直入 D 类，配 1609 8Ω 0.5W 喇叭 |
| LDO | XC6220B331MR | 500mA，WiFi 峰值不塌 |
| 防倒灌 | AO3401（Q2） | D=BAT S=VSYS G=VBUS |
| 电池 | 602535 软包 500mAh（带保护板） | TP4056-42 **ESOP-8** 充电 |
| PCB | **34 × 50 mm，4 层 1.6mm** | 嘉立创 ¥50/5 片 |

### 引脚（V5 一体板，和外接面包板的编号不同！）

| 功能 | GPIO |
|---|---|
| LCD SCK / MOSI / CS / DC / RST / BL | **12 / 11 / 10 / 9 / 14 / 13** |
| I2S BCLK / LRC / DIN | **15 / 16 / 17** |
| 电池 ADC | 1 |
| 充电状态 CHRG | 2 |
| 按键 A / B | **0（飞线 J6.1）/ 5** |
| U5 flash（16MB，HSPI） | CS 21 / SCK 38 / MOSI 39 / MISO 40 |

> ⚠️ A 键在 V5 上是**飞线**到 J6 的 IO0（原设计 IO4 虚焊，绕开了）。
> 焊 SW1 时务必接**信号侧**，接到 GND 一侧会导致上电直接进 download 模式。

### 最小验证接线（面包板 / FPC 转接板）

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

> **这张表是「镜像」的。** 转接板座子的 pin1 方向与屏排线丝印相反，硬插会把
> 3V3 撞到屏的 GND 触点上（会发烫）。本项目没翻排线，而是把杜邦线按 `排针 n ↔ 屏触点 25-n`
> 映射改插到镜像位置。你若能自由选择方向，优先按物理正序插，别用这张镜像表。

---

## 三、踩过的坑（这个项目 90% 的时间都在这）

### 1. IM 脚必须为高电平 — 否则背光亮、屏全黑

ST7789 的 `IM0/IM1/IM2` 决定接口模式。**IM 为低时不是 4 线 SPI 模式，初始化指令完全不生效**，
症状是：背光正常亮、屏幕纯黑。

这条坑了整整一天，期间误判成「WiFi 库干扰」「固件太大」「杜邦线撑不住 20 MHz」，全是假象。
结论来自原厂开源工程 [`BA4IHS/GD32E230-LaoWang`](https://github.com/BA4IHS/GD32E230-LaoWang)，
其 `TFT_LCD_Init()` 第一行就是 `TFT_LCD_IM12(1);`。

```cpp
digitalWrite(PIN_IM, HIGH);   // 不能省，也不能拉低
```

### 2. 逐字节 `SPI.transfer()` 是性能灾难

`SPI.transfer(uint8_t)` 每次都要等上一字节发完，实测 **1.5–2 µs/字节**。
一帧 115200 字节 → **约 200ms，只有 5fps**。而且**提高 SPI 时钟完全没用**，
瓶颈在 CPU 调用开销，不在时钟。

换成 `SPI.writeBytes()` 批量（每批 16 行 = 7680 字节），整帧期间 CS 保持低、
`0x2C` 只发一次（窗口在全屏初始化时设好），立刻变成 **约 35ms/帧**。

| 方案 | 单帧 | 帧率 |
|---|---|---|
| 10 MHz + 逐字节 `transfer()` | ~200 ms | ~5 fps |
| 26 MHz + `writeBytes()` 批量 | ~35 ms | 跑满 GIF 的 16.7 fps |
| 80 MHz + 批量 + 双核流水线 | **15.1 ms** | 60+ fps（推屏上限） |

### 3. 逻辑地必须单独接

只接 `LED-` 时背光能亮，但屏的逻辑地悬空 → 全黑。转接板的 1/2/22 都是 GND，
至少要保证逻辑地有回路。

### 4. FPC 座插反 = 3V3 被拉塌，症状伪装成「屏黑 + USB 反复枚举」

反过来插会把 3V3 短到 GND。看上去像固件坏了、像 USB 线不好、像模组烧了，
**实际是座子方向错了**。先摸屏排线附近烫不烫，再查软件。

### 5. QFN 的 EP 锡厚会把芯片顶起来，十几脚一起虚焊

MAX98357A（U4）底部的散热焊盘上锡太多，芯片被顶高 0.1mm，**16 个脚全部没接触到焊盘**。
现象：一切正常，就是没声音。

修法：涂助焊剂，风枪 280~300°C 从底部吹 40~60s 让它自己回流坐下。
**没声先查 EP，不要先怀疑 I2S 时序。**

### 6. 采样率不是随便填的：MCLK/fs 必须是 128 / 256 / 384 / 512

一开始设 40000Hz，听到的是**沙沙声**——因为 PLL 拆不出整数分频。
31250Hz 能整除（MCLK 8MHz），于是本项目所有音频都锁在 31250Hz。
★ 别为了「好听」改成 44100，除非整条时钟链重新算。

### 7. flash 擦除会拖死播放，以及「对称去抖」是个陷阱

- 64KB 块擦是**原子操作 150ms**，而 8KB 读缓冲只够 131ms → 必须用 32KB（94ms），
  并且**只在刚 refill 完那一刻擦**，交给独立任务做。实测播放速率才从 51200 爬到 62958 B/s。
- 按键用对称去抖（`millis()-tRaw > 20`）**永远等不到**：氧化触点一次按动是 1~3ms 的脉冲串，
  每个断点都在刷新计时器。改成**计数式**：1ms 采样、连续 2 次 LOW 判按下、松手等 16ms。

---

## 四、目录

| 路径 | 说明 |
|---|---|
| **`weather_clock/`** | ★ **主程序**。多文件拆分：`weather_clock.ino`（UI/页面/按键）+ `tft.h`（驱动）+ `gif_store.h`（GIF 专辑）+ `u5flash.h`（16MB flash）+ `spk.h`（音效合成）+ `player.h`/`webstream.h`（推流播放）+ `www.h`（手机端网页）+ `audio.h`（WAV 播放） |
| **`hardware/`** | ★ **V5 四层 PCB**：KiCad 工程、Gerber、BOM、采购清单、`EDA-画板指南.md`（314KB 实战记录，§1~§39） |
| `tools/album.py` | **GIF 专辑管理**：`list / add / rm / mv / order / migrate`，走串口 |
| `tools/screenshot.py` | **串口截图**：发 `Z` 导当前帧、`z` 重画 logo 后导出，存 PNG。调 UI 不用靠眼睛 |
| `tools/转换GIF.bat` | 把 GIF 拖上去 = 转码 + 上传 + 写索引（一条龙） |
| `tools/gif2bin.py` | GIF → 板端格式（缩放/抽帧/降色/RLE），单帧超限自动降色 |
| `tools/btn_diag.py` `vol_test.py` | 按键计数、音量台阶的自动化验证 |
| `lcd_fast/` `lcd_player/` | 早期驱动版本（可直接对比 `transfer()` 与 `writeBytes()` 的差异） |
| `bare_lcd_test/` | 最小验证：初始化 + 纯色 + 彩条，用来确认接线 |
| `gif_player*/` `*_probe/` `*_test/` | 排查过程留下的探针程序，保留作记录 |

---

## 五、编译与烧录

1. Arduino IDE + **esp32 core 4.0.x**
2. **必须有 `weather_clock/secrets.h`**（已 gitignore）：
   ```cpp
   #define WIFI_SSID "your-ssid"
   #define WIFI_PASS "your-pass"
   ```
   从 `secrets.h.example` 复制改名。
3. 板子选 **ESP32-S3 Dev Module**，下面两项**必须改**，否则屏黑或刷不进去：
   - **USB CDC On Boot: `Enabled`** ← 不开就看不到串口输出
   - **Partition Scheme: `Huge APP (3MB No OTA/1MB SPIFFS)`** ← 否则固件装不下
4. 串口 115200。正常启动会依次打印 WiFi、NTP、天气、U5、专辑索引、缓冲分配。

### 串口命令

| 键 | 作用 |
|---|---|
| `n` / `p` | 下一页 / 上一页 |
| `l` / `L` | 注入「长按 A / 长按 B」 |
| `H` | 注入「A+B 同按」 |
| `P` | 打印当前页 / 音量 / 推流状态 |
| `h` | 推流统计（`rd` 必须 ≈ **62500 B/s**） |
| `y` | 只灌 I2S 静音块，测纯时钟速率（应 ≈ 31499Hz） |
| `Z` / `z` | 导出当前帧 / 重画 logo 后导出（配合 `screenshot.py`） |
| `V` / `I` | 读 / 写专辑索引 |
| `e` / `E` | 按键计数 / 清零 |
| `U` / `A` | 上传 GIF / 音频 |

---

## 六、换 GIF、加音乐

**GIF**：把 `.gif` 拖到 `tools\转换GIF.bat` 上 → 自动转码 + 上传 + 写索引，屏上长按 A/B 切换。
或用命令行：

```bat
python tools\album.py add my.gif        :: 追加一条
python tools\album.py list              :: 看现有条目与占用
python tools\album.py rm 1              :: 删第 2 条（只改索引，数据等覆盖）
python tools\album.py mv 1 0            :: 调顺序
```

**音乐**：手机连同一 WiFi，浏览器开 `http://<屏上 SYS 页显示的 IP>/`，选文件即可播放。
网页里是 JS 解码 + 本地重采样到 31250Hz，**浏览器不给用的采样率会自动兜底**。

---

## 七、已知问题

- **天气接口只能走明文 HTTP**：`api.open-meteo.com` 的 ECDSA 证书会让 mbedTLS 握手失败，
  所以用了 HTTP/80 + 自计时 DNS。
- GIF 播放推到 8~17fps 取决于帧体积；单帧 >41KB 时转码脚本会自动降色。
- 外挂 flash 的 16MB 分了区：`0~2MB` 专辑、`2~6MB` 音乐环形、`8~16MB` 音频，
  其余留给将来。
- USB 供电不足时（满音量 + 满亮 + 推流），个别机器会欠压复位。

---

## 八、还有第二版

第二版是**重新设计的便携播放器**（TF 卡本地解码、侧装双喇叭、3.5mm 耳机、50×40 板），
不在本仓库，也不建议从本仓库改——音频架构完全不同。设计稿在另一个项目里，尚未开工。

---

## 致谢

- [`BA4IHS/GD32E230-LaoWang`](https://github.com/BA4IHS/GD32E230-LaoWang) — 原厂模块开源固件，
  IM 电平与完整初始化序列的结论出自这里
- ST7789 datasheet（写周期 16ns，SPI 上限约 62.5MHz）
- 立创 EDA / 嘉立创（打样）

## License

MIT，见 [LICENSE](LICENSE)。
