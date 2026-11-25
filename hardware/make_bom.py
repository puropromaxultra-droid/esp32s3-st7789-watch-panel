import os, sys
sys.path.insert(0, r"D:\KiCad\bin"); sys.path.insert(1, r"D:\KiCad\bin\Lib\site-packages")
import pcbnew

H = r"C:\Users\ZhuanZ\Desktop\电子屏显示\hardware"
G = os.path.join(H, "gerber")
P = os.path.join(H, "esp32s3-lcd.kicad_pcb")

b = pcbnew.LoadBoard(P)
rows = {}
for fp in b.GetFootprints():
    v = fp.GetValue()
    ref = fp.GetReference()
    rows.setdefault(v, []).append(ref)

order = ["ESP32-S3-MINI-1U-N8", "FPC 24P 0.5mm", "USB-C 16P 单排", "ME6211C33M5G-N", "AO3400A",
         "BTN 2P", "5.1k", "10k", "100k", "10uF", "1uF", "100nF"]
desc = {
    "ESP32-S3-MINI-1U-N8": ("U1", "ESP32-S3 模组 8MB Flash 外置天线版", "乐鑫 ESP32-S3-MINI-1U-N8 + IPEX 2.4G 天线"),
    "FPC 24P 0.5mm": ("J1", "24P 0.5mm 卧式 FPC 座 下接", "Molex 502244-2430 或兼容"),
    "USB-C 16P 单排": ("J2", "Type-C 16P 卧贴", "XKB U262-16XN-4BVC11"),
    "ME6211C33M5G-N": ("U2", "LDO 3.3V 500mA SOT-23-5", "南京微盟 ME6211C33M5G-N (1=VIN 2=GND 3=CE 4=NC 5=VOUT)"),
    "AO3400A": ("Q1", "NMOS 背光开关", "AOS AO3400A"),
    "BTN 2P": ("J3,J4", "按键接口 2P 排针", "2.54mm 1x02 直插排针 (pin1=GND pin2=BUTTON)"),
    "5.1k": ("R1,R2", "Type-C CC 下拉 5.1k", "0603 1%"),
    "10k": ("R3", "EN 上拉 10k", "0603 1%"),
    "100k": ("R4", "背光下拉 100k", "0603 1%"),
    "10uF": ("C1,C4", "输入/输出大电容", "0805 X5R 10uF/16V"),
    "1uF": ("C2", "LDO 输出 1uF", "0603 X5R 1uF/25V"),
    "100nF": ("C3,C5", "去耦/EN 滤波", "0603 X7R 100nF/50V"),
}

lines = ["Ref,Qty,Value,Description,Package"]
for v in order:
    refs = rows.get(v)
    if not refs:
        continue
    d = desc.get(v, ("", "", ""))
    lines.append("%s,%d,%s,%s,%s" % (" ".join(sorted(refs)), len(refs), v, d[1], d[2]))

# 校验：板内每个 value 都必须出现在 BOM 里，否则就是 order 表漏了（静默丢行）
missing = [v for v in rows if v not in order]
extra = [(v, rows[v]) for v in rows if v in order and desc.get(v, ("",))[0] and
         set(rows[v]) != set(desc[v][0].split(","))]
open(os.path.join(G, "BOM.csv"), "w", encoding="utf-8-sig").write("\n".join(lines))

report = lines[:]
report.append("")
report.append("板内 value 数 = %d, BOM 行数 = %d" % (len(rows), len(lines) - 1))
report.append("未进 BOM 的 value: %s" % (missing if missing else "无"))
report.append("位号不符: %s" % (extra if extra else "无"))
report.append("全部位号: %s" % " ".join(sorted(r for v in rows for r in rows[v])))
open(os.path.join(H, "_bomlog.txt"), "w", encoding="utf-8").write("\n".join(report))
print("ok missing=%s" % missing)
