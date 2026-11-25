# -*- coding: utf-8 -*-
"""幂等地把工程规则写成我们要的值（MCP/其他工具可能重置）。"""
import json
P = r"C:\Users\ZhuanZ\Desktop\电子屏显示\hardware\esp32s3-lcd.kicad_pro"
d = json.load(open(P, encoding="utf-8"))
rules = d["board"]["design_settings"]["rules"]
want = {
    "min_clearance": 0.0,
    "min_copper_edge_clearance": 0.3,
    "min_hole_clearance": 0.25,
    "min_through_hole_diameter": 0.3,
    "min_track_width": 0.12,
    "min_via_annular_width": 0.075,
    "min_via_diameter": 0.45,
}
for k, v in want.items():
    rules[k] = v
for nc in d["net_settings"]["classes"]:
    if nc["name"] == "Default":
        nc["clearance"] = 0.15
        nc["track_width"] = 0.2
        nc["via_diameter"] = 0.6
        nc["via_drill"] = 0.3
json.dump(d, open(P, "w", encoding="utf-8"), indent=2, ensure_ascii=False)
open(r"C:\Users\ZhuanZ\Desktop\电子屏显示\hardware\_rules.txt", "w", encoding="utf-8").write(
    "ok " + str(want) + " clearance=0.15")
