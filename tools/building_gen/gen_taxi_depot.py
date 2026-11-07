"""Taxi depot -> GLB. CS2 has the Taxi Depot and the Small Taxi Depot; same two here (4x3 and 3x2 cells).
Low garage hall (roll-up taxi doors, SPOM piers/header/fins), glazed dispatch office with blank fascia panel,
radio mast, painted taxi bays in the yard. No cars/trees/outer road baked in.
Run: python gen_taxi_depot.py   Output: assets/buildings/transport/taxi_depot/glb + preview.html + catalog.json
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_low_density as g
from gen_low_density import Mesh, CELL
import gen_bus_depot as b

BASE = os.path.join(g.ROOT, "assets", "buildings", "transport", "taxi_depot")
OUT = os.path.join(BASE, "glb")
DOOR_H = 3.8


def build(cells, bays_n, floors, accent, wallc, hall_h, canopy):
    Wc, Dc = cells
    W, D = Wc * CELL, Dc * CELL
    m = Mesh()
    hx, hz = W / 2 - 1.5, D / 2 - 1.5
    hall_d = 12.0 if Dc >= 3 else 9.0
    hall_z1 = -hz + hall_d
    m.quad("base", (-W / 2, 0.0, D / 2), (W / 2, 0.0, D / 2), (W / 2, 0.0, -D / 2), (-W / 2, 0.0, -D / 2), [(0, 1), (1, 1), (1, 0), (0, 0)])
    hall_w = bays_n * 4.4 + 1.2
    off_w = 9.0 if Wc >= 4 else 8.0
    x0 = max(-hx, -(hall_w + off_w) / 2); x1 = x0 + hall_w
    ox0, ox1 = x1, min(hx, x1 + off_w)
    # hall (no +z face drawn by mbox; one wall face with doors proud of it)
    b.mbox(m, x0, x1, 0.0, hall_h, -hz, hall_z1, skip=("+z",))
    bw = (x1 - x0 - 1.2) / bays_n
    b.zface(m, x0, x1, 0.0, hall_h, hall_z1, 1, "wall", b.wuv(x0, x1, 0.0, hall_h))
    for i in range(bays_n):
        xa = x0 + 0.6 + i * bw
        b.zface(m, xa + 0.15, xa + bw - 0.15, 0.0, DOOR_H, hall_z1 + 0.03, 1, "detail", b.auv("garage"))
    b.zface(m, x0 + 0.5, x1 - 0.5, DOOR_H + 0.5, hall_h - 0.6, hall_z1 + 0.03, 1, "detail", b.auv("wide"))
    # dispatch office
    oh = 3.6 * floors + 0.4; od = 7.5 if Dc >= 3 else 6.0
    oz0, oz1 = hall_z1 - od, hall_z1
    b.mbox(m, ox0, ox1, 0.0, oh, oz0, oz1, skip=("-x",))
    b.zface(m, ox0 + 0.6, ox0 + 4.2, 0.0, 3.0, oz1 + 0.03, 1, "detail", b.auv("door"))
    b.windows_z(m, ox0 + 4.4, ox1 - 0.4, 0.7, 2.9, oz1, 1, step=3.2, kind="win")
    if floors > 1:
        b.windows_z(m, ox0 + 0.4, ox1 - 0.4, 1.1 + 3.6 - 0.9, 3.6 + 2.5, oz1, 1, step=3.2, kind="win")
    b.windows_x(m, oz0 + 1, oz1 - 1, 1.0, 3.0, ox1, 1, 3.4, "win")
    b.zface(m, ox0 + 0.6, ox1 - 0.6, oh - 0.9, oh - 0.1, oz1 + 0.04, 1, "detail", [(236 / 512, 240 / 512)] * 4)   # blank sign
    b.mbox(m, ox0 + 0.2, ox0 + 4.4, 3.2, 3.4, oz1, oz1 + 1.8, top="roof", skip=("-z", "-y"))  # canopy
    # radio mast: thin 5-face prism on the office roof
    mx = ox1 - 1.2; mz = oz0 + 1.2
    b.mbox(m, mx - 0.12, mx + 0.12, oh, oh + 7.0, mz - 0.12, mz + 0.12, wall="dark" if False else "wall", top=None)
    # SPOM relief
    vols = []
    def paint_hall(a):
        Wh = x1 - x0
        for i in range(bays_n + 1):
            xc = 0.6 + i * bw
            a(xc - 0.3, xc + 0.3, 0.0, hall_h - 0.3, 0.0, 0.3)
            a(xc - 0.12, xc + 0.12, DOOR_H + 0.65, hall_h - 0.6, 0.0, 0.2)
        a(0.0, Wh, DOOR_H, DOOR_H + 0.4, 0.0, 0.45, "dark")
        a(0.0, Wh, hall_h - 0.5, hall_h - 0.05, 0.0, 0.5, "accent")      # taxi-yellow fascia band
        a(0.0, Wh, hall_h + 0.1, hall_h + 0.3, 0.0, 0.35)
    vols.append(b.spom_volume(m, "hall", x1 - x0, hall_h + 0.3, (x0, 0.0, hall_z1), paint_hall, wallc, accent))
    def paint_off(a):
        Wo = ox1 - ox0
        a(0.45, 0.65, 0.0, 3.2, 0.0, 0.4, "dark"); a(4.15, 4.35, 0.0, 3.2, 0.0, 0.4, "dark"); a(0.45, 4.35, 3.0, 3.2, 0.0, 0.5, "dark")
        for (sa, sb) in b.win_spans(4.4, Wo - 0.4, 3.2):
            a(sa - 0.1, sb + 0.1, 3.0, 3.1, 0.0, 0.55, "dark"); a(sa - 0.1, sa - 0.04, 0.7, 3.0, 0.0, 0.3); a(sb + 0.04, sb + 0.1, 0.7, 3.0, 0.0, 0.3)
        if floors > 1:
            a(0.0, Wo, 3.5, 3.75, 0.0, 0.26)
            for (sa, sb) in b.win_spans(0.4, Wo - 0.4, 3.2):
                a(sa - 0.1, sb + 0.1, 6.2, 6.3, 0.0, 0.5, "dark")
        a(0.0, Wo, oh - 0.1, oh + 0.3, 0.0, 0.34, "accent")
    vols.append(b.spom_volume(m, "office", ox1 - ox0, oh + 0.3, (ox0, 0.0, oz1), paint_off, wallc, accent))
    bays = [(-hx + 1, hx - 1, hall_z1 + 2.5, min(hz - 0.5, hall_z1 + 2.5 + 6.0))]
    if canopy:   # small fuel/charge canopy at the front-left
        fx0, fx1 = -hx, -hx + 7.0; fz0, fz1 = hz - 5.5, hz - 1.5
        b.mbox(m, fx0, fx1, 4.4, 4.8, fz0, fz1, top="roof", skip=("-y",))
        for cx in (fx0 + 0.6, fx1 - 0.6):
            b.mbox(m, cx - 0.2, cx + 0.2, 0.0, 4.4, fz0 + 0.5, fz0 + 0.9, top=None, skip=("-z", "+x"))
        bays = [(fx1 + 1.0, hx - 1, hall_z1 + 2.5, hz - 0.5)]
    return m, bays, vols


MODELS = [
    # name, cells, bays, office floors, accent, wall colour, hall height, canopy
    ("TD_4x3_taxi_depot_L1", (4, 3), 3, 2, (240, 190, 20), (176, 178, 176), 5.4, True),
    ("TD_3x2_small_taxi_depot_L1", (3, 2), 2, 1, (240, 190, 20), (190, 184, 170), 5.0, False),
]


def main():
    os.makedirs(OUT, exist_ok=True)
    roof = b.roof_tex(); catalog = []
    for (name, cells, bays_n, fl, acc, wallc, hh, canopy) in MODELS:
        m, bays, vols = build(cells, bays_n, fl, acc, wallc, hh, canopy)
        W, D = cells[0] * CELL, cells[1] * CELL
        tex = {"wall": b.wall_tex(wallc), "roof": roof, "detail": b.detail_tex(acc), "base": b.yard_tex(W, D, bays, acc)}
        for (kind, maps_, boxes, origin) in vols: tex["spom_" + kind] = maps_
        mats = {"detail": {"rough": 0.35, "metal": 0.1}, "base": {"rough": 0.95}}
        m.prims = {k: v for k, v in m.prims.items() if v}
        tex = {k: v for k, v in tex.items() if k in m.prims}
        p = os.path.join(OUT, name + ".glb")
        g.write_glb(p, m, tex, mats)
        tris = m.tri_count()
        spom_tris = sum(2 if c[3] == 4 else 1 for k, l in m.prims.items() if k.startswith("spom_") for c in l)
        pm = Mesh(); pm.prims = {k: list(v) for k, v in m.prims.items() if not k.startswith("spom_")}
        pm.prims.setdefault("detail", [])
        for (kind, maps_, boxes, origin) in vols:
            for bx in boxes: b.pbox(pm, bx, origin)
        os.makedirs(os.path.join(OUT, "preview"), exist_ok=True)
        g.write_glb(os.path.join(OUT, "preview", name + "_spom_preview.glb"), pm, {k: v for k, v in tex.items() if k in pm.prims}, mats)
        catalog.append({"file": name + ".glb", "preview": "preview/" + name + "_spom_preview.glb", "name": name, "lot": list(cells), "lot_m": [W, D],
                        "tris": tris, "spom_tris": spom_tris, "materials": len(m.prims), "kb": os.path.getsize(p) // 1024})
        print(f"{name}: {tris} tris ({spom_tris} SPOM volumes, {tris - spom_tris} real), {len(m.prims)} materials, {os.path.getsize(p) // 1024} KB")
    json.dump(catalog, open(os.path.join(BASE, "catalog.json"), "w"), indent=1)
    open(os.path.join(BASE, "preview.html"), "w", encoding="utf-8").write(b.PREVIEW.replace("<title>Bus depot</title>", "<title>Taxi depot</title>"))


if __name__ == "__main__":
    main()
