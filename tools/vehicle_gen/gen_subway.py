"""Subway / metro cab car -> GLB, built on the gen_train toolkit (same carbody loft, bogies, wheels and 1.435 m gauge as
the intercity train, so it sits on the same TRACK_*.glb pieces).

Stainless box body, high floor for platform boarding, four bi-parting doors a side, flat cab face with a wide windscreen
at +Z, gangway at -Z, third-rail collector shoes on the bogies, two roof A/C pods. Run cars in pairs (rotate one 180 deg).
Run:  python gen_subway.py
Output: assets/vehicles/subway/glb/SUBWAY_car_<body>_<band>.glb   Units: metres, +Y up, front +Z, y = 0 top of rail.
"""
import os, zlib
import numpy as np
import gen_cars as gc
import gen_transit as gt
import gen_transit_tex as tx
import gen_train as tr
import gen_train_tex as rt
from gen_cars import Mesh, box, METAL, COLOURS

OUT = os.path.join(gc.ROOT, "assets", "vehicles", "subway", "glb")
FL, BD, O, GAUGE = rt.FLAT, rt.BDARK, tx.OVL, tr.GAUGE
T = dict(L=18.4, W=3.05, H=3.70, yb=1.12, r=0.43, wb=2.2, bogies=6.3, win=(1.95, 2.95), doors=[-6.9, -2.3, 2.3, 6.9],
         mix=[(O["clear"], 6), (O["tint_light"], 3), (O["clear_sticker"], 1)])
LIVERIES = [("silver", "orange"), ("silver", "blue"), ("silver", "green"), ("white", "red")]  # body, line-colour band
SIGNS = ["A FAR ROCKAWAY", "7 FLUSHING", "L CANARSIE", "2 WAKEFIELD"]


def subway(m, t, rng):
    hw, L2, H, yb = t["W"] / 2, t["L"] / 2, t["H"], t["yb"]
    tr.carbody(m, t)
    wy0, wy1 = t["win"]; dw = 0.70                                       # half width of a double-door opening
    for s in (+1, -1):
        edges = [-L2 + 0.9] + sum([[zc - dw - 0.10, zc + dw + 0.10] for zc in t["doors"]], []) + [L2 - 0.95]
        for k in range(0, len(edges), 2):                                 # windows between the doors
            a, b = edges[k], edges[k + 1]; n = max(1, int(round((b - a) / 1.6))); st = (b - a) / n
            for j in range(n):
                gt.pane(m, s, hw + 0.004, a + j * st + 0.05, a + (j + 1) * st - 0.05, wy0, wy1, 1, k * 4 + j + (s > 0) * 40, gt.pick(rng, t["mix"]))
        u0, v0, u1, v1 = rt.COACH_DOOR
        for zc in t["doors"]:                                             # bi-parting leaves: door region, mirrored
            gt.side_quad(m, "door", s, hw + 0.008, zc - dw, zc, yb + 0.05, yb + 1.95, (u0, v0, u1, v1))
            gt.side_quad(m, "door", s, hw + 0.008, zc, zc + dw, yb + 0.05, yb + 1.95, (u1, v0, u0, v1))
            box(m, "bogie", s * (hw - 0.02), s * (hw + 0.03), yb + 1.95, yb + 2.02, zc - dw - 0.05, zc + dw + 0.05, BD)
        gt.side_quad(m, "trim", s, hw + 0.003, -L2 + 0.3, L2 - 0.3, wy1 + 0.10, wy1 + 0.28, (*FL, *FL))  # line band
        gt.side_quad(m, "trim", s, hw + 0.003, -L2 + 0.3, L2 - 0.3, yb + 0.10, yb + 0.22, (*FL, *FL))
        box(m, "bogie", s * (hw - 0.01), s * (hw + 0.02), wy1 + 0.02, wy1 + 0.26, 3.66, 5.54, BD)           # sign housing
        gt.side_quad(m, "sign", s, hw + 0.021, 3.70, 5.50, wy1 + 0.04, wy1 + 0.24)                        # side route sign: 9:1 like its texture, over the windows between doors 2 and 3
        gt.pane(m, s, hw + 0.004, L2 - 0.90, L2 - 0.25, wy0, wy1, 1, 99, O["tint_light"])               # cab side window
    ze = L2                                                               # cab face
    gt.end_quad(m, "front", 1, ze + 0.004, hw * 0.92, yb + 0.05, 1.70, rt.NOSE)
    box(m, "bogie", -hw * 0.9, hw * 0.9, 1.80, H - 0.35, ze, ze + 0.01, BD)
    gt.end_quad(m, "interior", 1, ze + 0.016, hw * 0.82, 1.90, H - 0.75, (0, 78, 0.999, 78.999))
    m.quad("sign", [(-0.75, H - 0.68, ze + 0.018), (0.75, H - 0.68, ze + 0.018), (0.75, H - 0.42, ze + 0.018), (-0.75, H - 0.42, ze + 0.018)],
           [(0, 1), (1, 1), (1, 0), (0, 0)], np.array([0, 0, 1]))
    box(m, "bogie", -hw + 0.15, hw - 0.15, yb - 0.30, yb + 0.02, ze - 0.2, ze + 0.12, BD)              # anticlimber
    box(m, "bogie", -0.12, 0.12, 0.70, 0.92, ze, ze + 0.45, BD)                                       # coupler
    box(m, "bogie", -0.30, 0.30, 0.66, 0.96, ze + 0.40, ze + 0.48, BD)
    box(m, "bogie", -hw + 0.2, hw - 0.2, yb - 0.30, yb + 0.02, -L2 - 0.05, -L2 + 0.2, BD)              # rear headstock
    box(m, "bogie", -0.12, 0.12, 0.70, 0.92, -L2 - 0.28, -L2, BD)
    gw, g0, g1, ge = 0.55, yb + 0.05, yb + 2.05, -L2 - 0.25                                            # gangway bellows
    for sx in (+1, -1):
        m.quad("bellows", [(sx * gw, g0, -L2), (sx * gw, g0, ge), (sx * gw, g1, ge), (sx * gw, g1, -L2)], [(0, 1), (2, 1), (2, 0), (0, 0)], np.array([sx, 0, 0]))
    m.quad("bellows", [(-gw, g1, -L2), (gw, g1, -L2), (gw, g1, ge), (-gw, g1, ge)], [(0, 0), (0, 1), (2, 1), (2, 0)], np.array([0, 1, 0]))
    for zc in (-t["bogies"], t["bogies"]):
        tr.bogie(m, t, zc)
        for s in (+1, -1):                                               # third-rail shoe beam + shoe
            box(m, "bogie", s * (GAUGE / 2 + 0.20), s * (GAUGE / 2 + 0.55), 0.30, 0.38, zc - 0.15, zc + 0.15, BD)
            box(m, "metal", s * (GAUGE / 2 + 0.45), s * (GAUGE / 2 + 0.75), 0.16, 0.22, zc - 0.25, zc + 0.25, METAL)
    for zc in (-3.4, 0.6, 4.6):                                          # underfloor equipment
        for s in (+1, -1):
            box(m, "bogie", s * 0.25, s * (hw - 0.35), 0.50, yb, zc - 0.9, zc + 0.9, BD)
            gt.side_quad(m, "bogie", s, hw - 0.349, zc - 0.9, zc + 0.9, 0.50, yb - 0.02, rt.UNDERBOX)
    for zc in (-4.5, 4.5):                                               # roof A/C pods
        box(m, "body", -0.95, 0.95, H - 0.06, H + 0.22, zc - 1.3, zc + 1.3, FL)
        for s in (+1, -1): gt.side_quad(m, "grille", s, 0.951, zc - 1.1, zc + 1.1, H, H + 0.20, rt.LOUVRE)


def main():
    os.makedirs(OUT, exist_ok=True)
    maps = tr.load_maps()
    for i, (bodyc, band) in enumerate(LIVERIES):
        seed = zlib.crc32(f"subway{i}".encode()); rng = np.random.default_rng(seed)
        m = Mesh(); subway(m, T, rng)
        maps["sign"] = (tx.sign(SIGNS[i]), None)
        out = os.path.join(OUT, f"SUBWAY_car_{bodyc}_{band}.glb")
        gc.write_glb(out, m, maps, COLOURS[bodyc], "alloy", trim=COLOURS[band], seat=gt.SEATS[seed % len(gt.SEATS)])
    print(f"subway: {T['L']:.1f} m, {m.count()} tris, {len(LIVERIES)} liveries, {os.path.getsize(out) / 1e6:.2f} MB per GLB")


if __name__ == "__main__":
    main()
