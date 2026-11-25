# -*- coding: utf-8 -*-
"""铺 GND 铜皮（四层）+ 天线净空 + 边缘缝合过孔（避让焊盘/走线）

两个 KiCad 10 的坑：
1) Remove(zone) 之后 GetFootprints()/GetTracks() 会退化成裸 SwigPyObject
   → 所有封装/走线数据必须在删铜皮之前采集完。
2) 重复跑时，上一轮的缝合孔会被当成障碍物，导致一个新孔都放不下
   → 先识别并回收"只连铜皮、不接任何走线"的 GND 过孔。
"""
import pcbnew, os

H = r"C:\Users\ZhuanZ\Desktop\电子屏显示\hardware"
PCB = os.path.join(H, "esp32s3-lcd.kicad_pcb")
LOG = os.path.join(H, "..", "_pour.log")
steps = []
POLYS = []

def log(s):
    steps.append(s)
    open(LOG, "w", encoding="utf-8").write("\n".join(steps))

log("start")
b = pcbnew.LoadBoard(PCB)
log("loaded zones=%d fp=%d trk=%d" % (
    len(b.Zones()), len(b.GetFootprints()), len(b.GetTracks())))
if len(b.GetFootprints()) < 10 or b.FindNet("GND") is None:
    log("ABORT: board looks broken")
    raise SystemExit(1)

GND = b.FindNet("GND")
KX0, KY0, KX1, KY1 = 9.9, 32.7, 26.5, 38.5   # 天线净空

# ---------- 1. 采集（必须在删 zone 之前） ----------
obst = []
for fp in list(b.GetFootprints()):
    for p in list(fp.Pads()):
        bb = p.GetBoundingBox()
        obst.append((pcbnew.ToMM(bb.GetLeft()) - 0.35, pcbnew.ToMM(bb.GetTop()) - 0.35,
                     pcbnew.ToMM(bb.GetRight()) + 0.35, pcbnew.ToMM(bb.GetBottom()) + 0.35))

ends = set()
tracks = []
for t in list(b.GetTracks()):
    tracks.append(t)
    if t.Type() == pcbnew.PCB_VIA_T:
        continue
    for p in (t.GetStart(), t.GetEnd()):
        ends.add((round(pcbnew.ToMM(p.x), 2), round(pcbnew.ToMM(p.y), 2)))
    s, e = t.GetStart(), t.GetEnd()
    r = pcbnew.ToMM(t.GetWidth()) / 2 + 0.3
    obst.append((min(pcbnew.ToMM(s.x), pcbnew.ToMM(e.x)) - r,
                 min(pcbnew.ToMM(s.y), pcbnew.ToMM(e.y)) - r,
                 max(pcbnew.ToMM(s.x), pcbnew.ToMM(e.x)) + r,
                 max(pcbnew.ToMM(s.y), pcbnew.ToMM(e.y)) + r))

# 分类过孔：缝合孔（GND、0.6/0.3、没有走线端点）vs 布线孔
old_vias = []
kept_vias = set()          # 留在板上、不能覆盖的布线过孔
stitch = []
for t in tracks:
    if t.Type() != pcbnew.PCB_VIA_T:
        continue
    c = t.GetPosition()
    x, y = pcbnew.ToMM(c.x), pcbnew.ToMM(c.y)
    old_vias.append((round(x, 3), round(y, 3)))
    is_gnd = t.GetNetname() == "GND"
    w = pcbnew.ToMM(t.GetWidth(pcbnew.F_Cu))
    d = pcbnew.ToMM(t.GetDrillValue())
    is_stitch = (is_gnd and abs(w - 0.6) < 0.01 and abs(d - 0.3) < 0.01
                 and (round(x, 2), round(y, 2)) not in ends)
    if is_stitch:
        stitch.append(t)
    else:
        kept_vias.add((round(x, 3), round(y, 3)))
        r = 0.65
        obst.append((x - r, y - r, x + r, y + r))

for t in stitch:
    b.Remove(t)
log("obst=%d  old_vias=%d  recycled_stitch=%d" % (len(obst), len(old_vias), len(stitch)))

# ---------- 2. 重铺铜皮 ----------
for z in list(b.Zones()):
    b.Remove(z)
log("cleared")

def make_zone(layer, coords, rule=False):
    z = pcbnew.ZONE(b)
    z.SetLayer(layer)
    if rule:
        z.SetIsRuleArea(True)
        z.SetDoNotAllowZoneFills(True)
        z.SetDoNotAllowTracks(False)
        z.SetDoNotAllowVias(False)
        z.SetDoNotAllowPads(False)
    else:
        z.SetNet(GND)
        z.SetLocalClearance(pcbnew.FromMM(0.3))
        z.SetMinThickness(pcbnew.FromMM(0.25))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetThermalReliefGap(pcbnew.FromMM(0.3))
        z.SetThermalReliefSpokeWidth(pcbnew.FromMM(0.4))
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        z.SetMinIslandArea(0)
        z.SetAssignedPriority(0)
    pts = pcbnew.SHAPE_POLY_SET()
    pts.NewOutline()
    for (x, y) in coords:
        pts.Append(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
    POLYS.append(pts)
    z.SetOutline(pts)
    b.Add(z)
    return z

m = 0.35
rect = [(m, m), (32 - m, m), (32 - m, 42 - m), (m, 42 - m)]
for layer in (pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu):
    make_zone(layer, rect)
log("zones 4")

for layer in (pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu):
    make_zone(layer, [(KX0, KY0), (KX1, KY0), (KX1, KY1), (KX0, KY1)], rule=True)
log("keepout %.1f..%.1f x %.1f..%.1f" % (KX0, KX1, KY0, KY1))

# ---------- 3. 边缘缝合过孔 ----------
def free(x, y, r=0.45):
    if x < 0.7 or x > 31.3 or y < 0.7 or y > 41.3:
        return False
    if KX0 <= x <= KX1 and KY0 <= y <= KY1:
        return False
    if 9.0 <= x <= 23.0 and y <= 8.6:
        return False
    if (round(x, 3), round(y, 3)) in kept_vias:
        return False
    for (x1, y1, x2, y2) in obst:
        if x1 - r < x < x2 + r and y1 - r < y < y2 + r:
            return False
    return True

vias = []
def add_via(x, y):
    v = pcbnew.PCB_VIA(b)
    v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
    v.SetWidth(pcbnew.FromMM(0.6))
    v.SetDrill(pcbnew.FromMM(0.3))
    v.SetNet(GND)
    v.SetViaType(pcbnew.VIATYPE_THROUGH)
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    b.Add(v)
    vias.append((x, y))
    obst.append((x - 0.35, y - 0.35, x + 0.35, y + 0.35))

step = 3.2
pts = []
x = 0.9
while x <= 31.1:
    pts += [(x, 0.9), (x, 41.1)]
    x += step
y = 0.9
while y <= 41.1:
    pts += [(0.9, y), (31.1, y)]
    y += step
for (x, y) in pts:
    if free(x, y):
        add_via(x, y)
log("vias=%d" % len(vias))

filler = pcbnew.ZONE_FILLER(b)
filler.Fill(b.Zones())
log("filled")

b.BuildListOfNets()
TMP = os.path.join(H, "_pour_tmp.kicad_pcb")
b.Save(TMP)
os.replace(TMP, PCB)
log("saved zones=%d" % len(b.Zones()))
