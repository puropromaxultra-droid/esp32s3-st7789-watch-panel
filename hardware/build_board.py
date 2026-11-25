# -*- coding: utf-8 -*-
"""ESP32-S3 + ST7789 一体化小板：生成板框、布局、网络、规则。
   所有元件放 F 面，屏在 B 面（丝印轮廓），FPC 从板下边绕到 F 面进座。
"""
import pcbnew, os

ROOT = r"C:\Users\ZhuanZ\Desktop\电子屏显示\hardware"
PCB = os.path.join(ROOT, "esp32s3-lcd.kicad_pcb")
TMP = os.path.join(ROOT, "_build_tmp.kicad_pcb")
LIB_ESP = os.path.join(ROOT, "lib", "Espressif.pretty")
SYS_FP = r"D:\KiCad\share\kicad\footprints"
LOG = os.path.join(os.environ["TEMP"], "build.txt")
out = []

PW, PH = 32.0, 42.0
board = pcbnew.NewBoard(TMP)
board.SetCopperLayerCount(4)

# ---------------- 板框 ----------------
def add_edge(x1, y1, x2, y2):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(x1), pcbnew.FromMM(y1)))
    s.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x2), pcbnew.FromMM(y2)))
    s.SetLayer(pcbnew.Edge_Cuts)
    s.SetWidth(pcbnew.FromMM(0.1))
    board.Add(s)

for a in [(0, 0, PW, 0), (PW, 0, PW, PH), (PW, PH, 0, PH), (0, PH, 0, 0)]:
    add_edge(*a)

def line(x1, y1, x2, y2, layer, w=0.12):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(x1), pcbnew.FromMM(y1)))
    s.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x2), pcbnew.FromMM(y2)))
    s.SetLayer(layer)
    s.SetWidth(pcbnew.FromMM(w))
    board.Add(s)

# ---------------- 网络 ----------------
NET_NAMES = ["GND", "+3V3", "VBUS", "LCD_SCK", "LCD_MOSI", "LCD_CS", "LCD_DC",
             "LCD_RST", "LCD_BL", "LED_K", "USB_DP", "USB_DM", "CC1", "CC2", "EN",
             "BTN_A", "BTN_B"]
nets = {}
for n in NET_NAMES:
    ni = pcbnew.NETINFO_ITEM(board, n)
    board.Add(ni)
    nets[n] = ni

# ---------------- 布局 ----------------
def place(lib, name, ref, cx, cy, rot=0.0, value=None):
    fp = pcbnew.FootprintLoad(lib, name)
    if fp is None:
        out.append("MISS %s : %s" % (lib, name))
        return None
    fp.SetReference(ref)
    if value:
        fp.SetValue(value)
    fp.Reference().SetVisible(False)
    fp.Value().SetVisible(False)
    fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(cx), pcbnew.FromMM(cy)))
    fp.SetOrientationDegrees(rot)
    board.Add(fp)
    return fp

def assign(fp, mapping):
    if fp is None:
        return
    for p in fp.Pads():
        num = p.GetNumber()
        if num in mapping:
            p.SetNet(nets[mapping[num]])

# U1：转 180°，让 LCD 信号全部靠上边，靠近 FPC 座；天线端落到板下沿
U1 = place(LIB_ESP, "ESP32-S3-MINI-1", "U1", 18.2, 25.0, 180, "ESP32-S3-MINI-1U-N8")
m = {}
for n in [1, 2, 42, 43, 61] + list(range(46, 61)):
    m[str(n)] = "GND"
m.update({"3": "+3V3", "13": "LCD_DC", "14": "LCD_CS", "15": "LCD_MOSI",
          "16": "LCD_SCK", "17": "LCD_BL", "18": "LCD_RST",
          "23": "USB_DM", "24": "USB_DP", "45": "EN",
          "8": "BTN_A", "9": "BTN_B"})
assign(U1, m)

# J1：24P 0.5mm 卧式，开口朝板边（y=0），FPC 绕边后进入
J1 = place(os.path.join(SYS_FP, "Connector_FFC-FPC.pretty"),
           "Molex_502244-2430_1x24-1MP_P0.5mm_Horizontal", "J1", 16.0, 11.08, 0,
           "FPC 24P 0.5mm")
jm = {"1": "GND", "2": "GND", "22": "GND", "MP": "GND",
      "13": "LCD_MOSI", "14": "+3V3", "15": "LCD_DC", "16": "LCD_SCK", "17": "LCD_CS",
      "18": "LCD_RST", "19": "+3V3", "20": "+3V3", "21": "+3V3",
      "23": "LED_K", "24": "+3V3"}
assign(J1, jm)

# J2：Type-C 侧出。rot=270 使插口（local +y 端）朝 -x 板外；
# x=2.225：前壳脚焊盘内沿距板边 0.35（>0.3 边距规则），插口面距板边 0.15，
# 信号焊盘列 x=5.895。
J2 = place(os.path.join(SYS_FP, "Connector_USB.pretty"),
           "USB_C_Receptacle_XKB_U262-16XN-4BVC11", "J2", 2.225, 21.0, 270, "USB-C 16P 单排")
tm = {"A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "SH": "GND", "": "GND",
      "A4": "VBUS", "A9": "VBUS", "B4": "VBUS", "B9": "VBUS",
      "A5": "CC1", "B5": "CC2",
      "A6": "USB_DP", "B6": "USB_DP", "A7": "USB_DM", "B7": "USB_DM"}
assign(J2, tm)
# A1/A12/B1/B12（屏蔽地）左侧被 NPTH 禁区、下方被 VBUS 焊盘堵死，热花盘只出 1 根辐条；
# 屏蔽脚无需热隔离 → 设实心铺铜连接（KiCad 10 的 API 是 SetLocalZoneConnection）
for _p in J2.Pads():
    if _p.GetNetname() == "GND" and _p.GetNumber() in ("A1", "A12", "B1", "B12"):
        _p.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)

# 电源 / 背光
# U2 = ME6211C33M5G-N（SOT-23-5，500mA：1=VIN, 2=GND, 3=CE, 4=NC, 5=VOUT）
# 换 500mA 的唯一理由：ESP32-S3 发 WiFi 峰值 355mA，XC6206（200mA / 限流 250mA）撑不住 -> 掉压复位
# 焊盘：pad1(2.063,27.35) pad2(2.063,28.30) pad3(2.063,29.25) pad4(4.338,29.25)NC pad5(4.338,27.35)
U2 = place(os.path.join(SYS_FP, "Package_TO_SOT_SMD.pretty"), "SOT-23-5", "U2",
           3.2, 28.3, 0, "ME6211C33M5G-N")
assign(U2, {"1": "VBUS", "2": "GND", "3": "VBUS", "5": "+3V3"})

# J3/J4：外部按键接口，pin1=GND、pin2=BUTTON（2.54 排针，直接接线或焊轻触开关）
_PH = os.path.join(SYS_FP, "Connector_PinHeader_2.54mm.pretty")
J3 = place(_PH, "PinHeader_1x02_P2.54mm_Vertical", "J3", 5.0, 2.6, 0, "BTN 2P")
assign(J3, {"1": "GND", "2": "BTN_B"})
J4 = place(_PH, "PinHeader_1x02_P2.54mm_Vertical", "J4", 29.6, 2.6, 0, "BTN 2P")
assign(J4, {"1": "GND", "2": "BTN_A"})

Q1 = place(os.path.join(SYS_FP, "Package_TO_SOT_SMD.pretty"), "SOT-23", "Q1",
           28.6, 11.5, 0, "AO3400A")
assign(Q1, {"1": "LCD_BL", "2": "GND", "3": "LED_K"})

R_SMD = os.path.join(SYS_FP, "Resistor_SMD.pretty")
C_SMD = os.path.join(SYS_FP, "Capacitor_SMD.pretty")

R1 = place(R_SMD, "R_0603_1608Metric", "R1", 2.2, 31.5, 0, "5.1k")
assign(R1, {"1": "CC1", "2": "GND"})
R2 = place(R_SMD, "R_0603_1608Metric", "R2", 5.2, 31.5, 0, "5.1k")
assign(R2, {"1": "CC2", "2": "GND"})
R3 = place(R_SMD, "R_0603_1608Metric", "R3", 7.0, 33.5, 0, "10k")
assign(R3, {"1": "EN", "2": "+3V3"})
R4 = place(R_SMD, "R_0603_1608Metric", "R4", 30.0, 9.0, 0, "100k")
assign(R4, {"1": "LCD_BL", "2": "GND"})

C1 = place(C_SMD, "C_0805_2012Metric", "C1", 7.6, 27.5, 0, "10uF")
assign(C1, {"1": "VBUS", "2": "GND"})
C2 = place(C_SMD, "C_0603_1608Metric", "C2", 7.6, 29.9, 0, "1uF")
assign(C2, {"1": "+3V3", "2": "GND"})
C3 = place(C_SMD, "C_0603_1608Metric", "C3", 28.2, 28.5, 0, "100nF")
assign(C3, {"1": "+3V3", "2": "GND"})
C4 = place(C_SMD, "C_0805_2012Metric", "C4", 28.2, 31.8, 0, "10uF")
assign(C4, {"1": "+3V3", "2": "GND"})
C5 = place(C_SMD, "C_0603_1608Metric", "C5", 3.5, 33.5, 0, "100nF")
assign(C5, {"1": "EN", "2": "GND"})

# ---------------- Type-C 扇出 ----------------
# 单排 16P：每个信号焊盘向 +x 引一小段，打 0.5/0.2 过孔进内层，
# 避免在 0.5mm 间距的焊盘列里互相封死。
def track(net, x1, y1, x2, y2, w=0.15, layer=pcbnew.F_Cu):
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(x1), pcbnew.FromMM(y1)))
    t.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x2), pcbnew.FromMM(y2)))
    t.SetWidth(pcbnew.FromMM(w))
    t.SetLayer(layer)
    t.SetNet(nets[net])
    board.Add(t)
    return t

def viadrop(net, x, y, d=0.45, drill=0.3):
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
    v.SetWidth(pcbnew.FromMM(d))
    v.SetDrill(pcbnew.FromMM(drill))
    v.SetViaType(pcbnew.VIATYPE_THROUGH)
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    v.SetNet(nets[net])
    board.Add(v)
    return v

# FPC 14 脚 = RD：SPI 模式下无用，原板由固件拉高，这里照抄接 +3V3。
# 走焊盘上沿外（y=6.5，触点端在 7.225）绕到 19 脚的 +3V3 主干。
track("+3V3", 16.75, 8.00, 16.75, 6.50, 0.15)
track("+3V3", 16.75, 6.50, 19.25, 6.50, 0.15)
track("+3V3", 19.25, 6.50, 19.25, 7.35, 0.15)

# 扇出：信号焊盘列在 x=5.895。DP 两根都到 x=7.30 打孔（In1 同列合并）；
# DM 两根都到 x=7.90，其 F stub 从 DP 的 In1 合并段正上方跨过（层间隔离，不冲突）。
# 槽位（rot270 后 y=21+local_x）：19.75=A5/CC1, 20.25=B7/DM, 20.75=A6/DP,
# 21.25=A7/DM, 21.75=B6/DP, 22.75=B5/CC2；19.25/22.25 是 A8/B8（NC，不引）。
FANOUT = [("CC1", 19.750, 7.30), ("USB_DP", 20.750, 7.30),
          ("USB_DM", 20.250, 7.90), ("USB_DM", 21.250, 7.90),
          ("USB_DP", 21.750, 7.30), ("CC2", 22.750, 7.90)]
for n, py, vx in FANOUT:
    track(n, 5.895, py, vx, py)
    viadrop(n, vx, py)

# DP：In1 合并后沿 y=21.75 右行到 x=17.35，下行到 19.65 打孔回 F 面进焊盘。
# 19.65 的落点给 DM 的 In1 绕行线（y=19.00）留出 0.35 净距。
track("USB_DP", 7.30, 21.75, 7.30, 20.75, 0.15, pcbnew.In1_Cu)
track("USB_DP", 7.30, 21.75, 17.35, 21.75, 0.15, pcbnew.In1_Cu)
track("USB_DP", 17.35, 21.75, 17.35, 19.65, 0.15, pcbnew.In1_Cu)
viadrop("USB_DP", 17.35, 19.65)
track("USB_DP", 17.35, 19.65, 17.35, 18.00, 0.15)

# DM：In1 合并后沿 y=20.25 到 x=16.30（DP 立柱 17.35 之前收住），
# 折到 y=19.00 从 DP 落点下方绕过，x=18.20 上到 19.65 打孔回 F 面。
track("USB_DM", 7.90, 21.25, 7.90, 20.25, 0.15, pcbnew.In1_Cu)
track("USB_DM", 7.90, 20.25, 16.30, 20.25, 0.15, pcbnew.In1_Cu)
track("USB_DM", 16.30, 20.25, 16.30, 19.00, 0.15, pcbnew.In1_Cu)
track("USB_DM", 16.30, 19.00, 18.20, 19.00, 0.15, pcbnew.In1_Cu)
track("USB_DM", 18.20, 19.00, 18.20, 19.65, 0.15, pcbnew.In1_Cu)
viadrop("USB_DM", 18.20, 19.65)
track("USB_DM", 18.20, 19.65, 18.20, 18.00, 0.15)

# CC1：In2 先横回 x=3.30（从连接器体下方穿过）再下行，R1 上方出来
track("CC1", 7.30, 19.75, 3.30, 19.75, 0.15, pcbnew.In2_Cu)
track("CC1", 3.30, 19.75, 3.30, 30.20, 0.15, pcbnew.In2_Cu)
viadrop("CC1", 3.30, 30.20)
track("CC1", 3.30, 30.20, 1.375, 30.20, 0.15)
track("CC1", 1.375, 30.20, 1.375, 31.20, 0.15)

# CC2：In2 走 x=3.85 下行（避开后壳脚焊盘 x>=4.28 与 CC1 立柱），R2 上方斜进
track("CC2", 7.90, 22.75, 3.85, 22.75, 0.15, pcbnew.In2_Cu)
track("CC2", 3.85, 22.75, 3.85, 30.50, 0.15, pcbnew.In2_Cu)
viadrop("CC2", 3.85, 30.50)
track("CC2", 3.85, 30.50, 4.60, 31.20, 0.15)

# Type-C 信号 GND 焊盘不拉短线：J2 移正后焊盘四周铺铜开阔，
# 后壳脚（通孔）直连四层 GND，热花盘自然连上（拉线反而堵死辐条）
# A1/A12 左侧被 NPTH 禁区堵死、下方被 VBUS 焊盘压住 → 右侧各补一颗地孔，
# 把东向铺铜锚进主地网，凑足第二根辐条
viadrop("GND", 7.0, 17.8)
viadrop("GND", 7.0, 24.2)
# ME6211 的 GND 在 pad2(2.063,28.30)，0.6 高的焊盘上压孔会吃锡；
# 往右拉到 4.338（夹在 pad5@27.35 与 pad4@29.25 正中，两侧各余 0.42），另起一个地孔
track("GND", 2.062, 28.30, 4.338, 28.30, 0.2)
viadrop("GND", 4.338, 28.30)
# C5 的地焊盘同理
viadrop("GND", 5.10, 33.50)
track("GND", 4.60, 33.50, 5.10, 33.50, 0.2)

# LDO 输出 -> 输出电容 C2（这一段自动布线器老是漏，手动画上）
track("+3V3", 4.338, 27.35, 5.55, 27.35, 0.2)
track("+3V3", 5.55, 27.35, 5.55, 28.30, 0.2)
track("+3V3", 5.55, 28.30, 5.55, 29.575, 0.2)
track("+3V3", 5.55, 29.575, 6.825, 29.575, 0.2)
track("+3V3", 6.825, 29.575, 6.825, 29.9, 0.2)
# +3V3 右半区桥接：freerouting 总把 +3V3 分成两簇（U2.3/C2.1 与 U1/C3/C4/R3.2），
# 从 C2.1 沿 R3 两焊盘中间的 0.5mm 走廊（净距恰 0.15）北上，落进 R3.2 并网
track("+3V3", 7.0, 29.9, 7.0, 32.5, 0.2)
track("+3V3", 7.0, 32.5, 7.6, 32.5, 0.2)
track("+3V3", 7.6, 32.5, 7.6, 33.3, 0.2)
# 注：In1 层 +3V3 的悬空端若 DRC 报未连接，在其端点补一颗 +3V3 过孔即可

# VBUS：主干走 x=8.60（连接器后壳面之外），上下两个 VBUS pad 从右侧汇入；
# y=26.30 左行（C1 焊盘上沿 26.775 / U2 pad1 之外）分两路：x=6.825 下探进 C1 pad1、
# 左端 x=1.30 下行进 LDO pad2
track("VBUS", 5.895, 18.60, 8.60, 18.60, 0.2)
track("VBUS", 5.895, 23.40, 8.60, 23.40, 0.2)
track("VBUS", 8.60, 18.60, 8.60, 26.30, 0.2)
track("VBUS", 8.60, 26.30, 1.10, 26.30, 0.2)
track("VBUS", 6.825, 26.30, 6.825, 27.50, 0.2)
track("VBUS", 1.10, 26.30, 1.10, 29.25, 0.2)
track("VBUS", 1.10, 27.35, 2.062, 27.35, 0.2)
track("VBUS", 1.10, 29.25, 2.062, 29.25, 0.2)

# 按键：F 面短桩 -> 过孔 -> B.Cu 立柱 -> J3/J4 通孔焊盘。
# 走底层是因为 F 面 y=12.45 有一条 GND 横线（Q1 源极到主地）把右侧颈部封死。
track("BTN_A", 25.20, 25.00, 27.40, 25.00)
viadrop("BTN_A", 27.40, 25.00)
track("BTN_A", 27.40, 25.00, 27.40, 5.14, 0.2, pcbnew.B_Cu)
track("BTN_A", 27.40, 5.14, 29.60, 5.14, 0.2, pcbnew.B_Cu)

track("BTN_B", 25.20, 24.15, 25.90, 24.15)
viadrop("BTN_B", 25.90, 24.15)
track("BTN_B", 25.90, 24.15, 25.90, 4.00, 0.2, pcbnew.B_Cu)
track("BTN_B", 25.90, 4.00, 5.00, 4.00, 0.2, pcbnew.B_Cu)
track("BTN_B", 5.00, 4.00, 5.00, 5.14, 0.2, pcbnew.B_Cu)

# ---------------- 丝印 ----------------
# 屏轮廓（B 面）26.5 x 29.5
sx1, sy1, sx2, sy2 = 2.75, 10.0, 29.25, 39.5
for a in [(sx1, sy1, sx2, sy1), (sx2, sy1, sx2, sy2), (sx2, sy2, sx1, sy2), (sx1, sy2, sx1, sy1)]:
    line(*a, pcbnew.B_SilkS, 0.15)
# FPC 进线区（F 面）
line(9.5, 6.6, 22.5, 6.6, pcbnew.F_SilkS, 0.12)
line(9.5, 6.2, 9.5, 6.6, pcbnew.F_SilkS, 0.12)
line(22.5, 6.2, 22.5, 6.6, pcbnew.F_SilkS, 0.12)

# ---------------- 设计规则 ----------------
ds = board.GetDesignSettings()
ds.SetBoardThickness(pcbnew.FromMM(1.6))

board.BuildListOfNets()
board.Save(TMP)
os.replace(TMP, PCB)

out.append("saved -> %s" % PCB)
out.append("footprints=%d" % len(board.GetFootprints()))
for fp in board.GetFootprints():
    bb = fp.GetBoundingBox(False, False)
    out.append("%-3s %-30s c=(%.2f,%.2f) rot=%.0f body x[%.2f,%.2f] y[%.2f,%.2f]" % (
        fp.GetReference(), fp.GetValue()[:30],
        pcbnew.ToMM(fp.GetPosition().x), pcbnew.ToMM(fp.GetPosition().y),
        fp.GetOrientationDegrees(),
        pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetRight()),
        pcbnew.ToMM(bb.GetTop()), pcbnew.ToMM(bb.GetBottom())))
open(LOG, "w", encoding="utf-8").write("\n".join(out))
