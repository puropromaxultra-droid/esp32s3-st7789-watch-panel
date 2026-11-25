import pcbnew, os

PCB = r"C:\Users\ZhuanZ\Desktop\电子屏显示\hardware\esp32s3-lcd.kicad_pcb"
LOG = os.path.join(os.environ["TEMP"], "geom.txt")
board = pcbnew.LoadBoard(PCB)
PW, PH = 32.0, 42.0
out = []

rects = []
for fp in board.GetFootprints():
    xs, ys = [], []
    for g in fp.GraphicalItems():
        if g.GetLayer() in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
            bb = g.GetBoundingBox()
            xs += [pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetRight())]
            ys += [pcbnew.ToMM(bb.GetTop()), pcbnew.ToMM(bb.GetBottom())]
    if not xs:
        out.append("%-3s NO COURTYARD" % fp.GetReference())
        continue
    r = (fp.GetReference(), fp.GetValue(), min(xs), max(xs), min(ys), max(ys))
    out.append("%-3s %-22s x %6.2f..%6.2f  y %6.2f..%6.2f" % (r[0], r[1][:22], r[2], r[3], r[4], r[5]))
    rects.append(r)

out.append("")
out.append("--- out of board ---")
for r in rects:
    if r[2] < -0.01 or r[3] > PW + 0.01 or r[4] < -0.01 or r[5] > PH + 0.01:
        out.append("%s OUT x[%.2f,%.2f] y[%.2f,%.2f]" % (r[0], r[2], r[3], r[4], r[5]))

out.append("")
out.append("--- courtyard overlaps ---")
n = 0
for i in range(len(rects)):
    for j in range(i + 1, len(rects)):
        a, c = rects[i], rects[j]
        ox = min(a[3], c[3]) - max(a[2], c[2])
        oy = min(a[5], c[5]) - max(a[4], c[4])
        if ox > 0 and oy > 0:
            out.append("%s <-> %s : %.2f x %.2f mm" % (a[0], c[0], ox, oy))
            n += 1
out.append("overlaps=%d" % n)
out.append("")
out.append("nets=%d" % board.GetNetCount())
open(LOG, "w", encoding="utf-8").write("\n".join(out))
