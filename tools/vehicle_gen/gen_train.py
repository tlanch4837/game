"""Passenger train -> GLB: diesel-electric locomotive and intercity coaches, on the gen_cars / gen_transit toolkit.

Low poly, detail in the maps (gen_train_tex): the carbody is one cross-section lofted along the train (rounded cantrail and
roof), and the loco nose is the same loft with its top clamped down a nose profile, so the raked windscreen and nose slopes
come out of ~10 rings. Everything finer (panel joints, louvres, springs, bolts, lamps, door leaves) is relief in height maps
that the engine runs parallax occlusion on. Side windows are interior-mapped panes (InteriorPS, coach style).

Types (North American style): loco (21.0 m diesel-electric cab unit), coach (25.9 m single-level fluted stainless),
bilevel (25.9 m, 4.9 m tall, two decks). AAR knuckle couplers and diaphragms, no side buffers.
Run:  python gen_train.py [type ...]
Output: assets/vehicles/train/glb/TRAIN_<type>_<body>_<stripe>.glb
Units: metres, +Y up, front faces +Z, y = 0 is the top of the rail, origin, under the vehicle centre.
"""
import math, os, sys, zlib
import numpy as np
import gen_cars as gc
import gen_transit as gt
import gen_transit_tex as tx
import gen_train_tex as rt
from gen_cars import Mesh, box, tube, METAL, COLOURS

OUT = os.path.join(gc.ROOT, "assets", "vehicles", "train", "glb")
gc.MOTO_LOD = 1.0  # tube() segment counts as given (the cars scale them down)
FL, BD = rt.FLAT, rt.BDARK
O = tx.OVL
GAUGE = 1.435
TYPES = {  # North American intercity: diesel-electric cab unit, single-level stainless coach, bilevel coach
    "loco":    dict(L=21.0, W=3.10, H=4.30, yb=1.05, r=0.53, wb=2.9, bogies=6.4, cab=1),
    "coach":   dict(L=25.9, W=3.15, H=3.95, yb=1.05, r=0.46, wb=2.6, bogies=9.0, pitch=1.30, win=[(2.02, 2.78)],
                    doors=[-11.35, 11.35], gang=(1.35, 3.35),
                    mix=[(O["clear"], 5), (O["tint_light"], 3), (O["blind_half"], 2), (O["clear_sticker"], 1)]),
    "bilevel": dict(L=25.9, W=3.12, H=4.88, yb=0.78, r=0.46, wb=2.6, bogies=9.0, pitch=1.62, win=[(1.42, 2.12), (3.02, 4.02)],
                    doors=[-3.6], gang=(2.62, 4.30), bilevel=1,
                    mix=[(O["clear"], 4), (O["tint_light"], 3), (O["curtain_open"], 2), (O["blind_half"], 1)]),
}
LIVERIES = [("silver", "red"), ("silver", "blue"), ("grey", "red"), ("blue", "silver")]
SIGNS = ["204 CHICAGO", "17 SEATTLE", "9 NEW ORLEANS", "88 BOSTON"]
# nose profile: (distance back from the coupler face, top height). The loft clamps the carbody top to it: the blunt, tall
# cab of a modern American passenger diesel, short raked windscreen over a high, nearly upright nose.
NOSE = [(2.60, None), (2.05, 3.95), (1.20, 2.75), (0.45, 2.30), (0.0, 1.70)]
NOSE_K = {2.60: 1.0, 2.05: 0.985, 1.20: 0.965, 0.45: 0.95, 0.0: 0.94}  # plan-view taper toward the nose


def section(t):
    """Closed CCW cross-section (x, y) seen from +z: underside, straight side, cantrail radius, shallow roof arc."""
    hw, yb, H = t["W"] / 2, t["yb"], t["H"]
    rc, yc = 0.38, H - 0.55                          # cantrail radius / centre height
    right = [(hw - 0.10, yb), (hw, yb + 0.10), (hw, yc)]
    right += [(hw - rc + rc * math.cos(a), yc + rc * math.sin(a)) for a in np.radians([25, 50])]
    xe, ye = hw - rc + rc * math.cos(math.radians(70)), yc + rc * math.sin(math.radians(70))
    right += [(xe * f, H - (H - ye) * f * f) for f in (1.0, 0.72, 0.45, 0.18)]
    left = [(-x, y) for x, y in reversed(right)]
    return [(0.0, yb)] + right + [(0.0, H)] + left


def ring(base, h, k):
    return [(x * k, min(y, h) if h else y) for x, y in base]


def loft(m, t, rings):
    """rings: [(z, pts)] with z increasing; strips between neighbouring rings, one material per strip by its facing."""
    hw = t["W"] / 2
    for (za, A), (zb, B) in zip(rings[:-1], rings[1:]):
        n = len(A)
        for j in range(n):
            a0, a1, b0, b1 = A[j], A[(j + 1) % n], B[j], B[(j + 1) % n]
            P = [(a0[0], a0[1], za), (a1[0], a1[1], za), (b1[0], b1[1], zb), (b0[0], b0[1], zb)]
            ex, ey = a1[0] - a0[0], a1[1] - a0[1]
            if abs(ex) < 1e-6 and abs(ey) < 1e-6: ex, ey = b1[0] - b0[0], b1[1] - b0[1]
            if max(a0[1], a1[1]) <= t["yb"] + 1e-6:                              # underside
                m.quad("bogie", P, [BD] * 4)
            elif abs(ey) > abs(ex) * 2 and min(abs(a0[0]), abs(a1[0])) > hw * 0.9:  # vertical bodyside: panel texture
                m.quad("body", P, [gt.body_uv(p[2], p[1]) for p in P])
            else:
                m.quad("body", P, [FL] * 4)


def cap(m, mat, pts, z, d, uvf):
    pts2 = []
    for p in pts:
        if not pts2 or math.dist(p, pts2[-1]) > 1e-4: pts2.append(p)
    if math.dist(pts2[0], pts2[-1]) < 1e-4: pts2.pop()
    if gc.area(pts2) < 0: pts2 = pts2[::-1]
    m.poly(mat, pts2, lambda p: (p[0], p[1], z), uvf, np.array([0, 0, d]))


def nose_quad(m, mat, z_end, d, i, f0, f1, half, uv, out=0.012):
    """Quad on nose segment i (NOSE[i] -> NOSE[i+1], the clamped top surface), from fraction f0 to f1, offset out."""
    (da, ha), (db, hb) = NOSE[i], NOSE[i + 1]
    dz, dy = -(db - da), hb - ha; L = math.hypot(dz, dy)                    # moving toward the nose: +dz forward
    nz, ny = dy / L, -dz / L
    nz, ny = (nz, ny) if ny > 0 or nz > 0 else (-nz, -ny)
    pt = lambda f: (z_end - d * (da - (da - db) * f) + d * nz * out, ha + dy * f + ny * out)
    (z0, y0), (z1, y1) = pt(f0), pt(f1)
    xs = (-half, half) if d > 0 else (half, -half); u0, v0, u1, v1 = uv
    m.quad(mat, [(xs[0], y0, z0), (xs[1], y0, z0), (xs[1], y1, z1), (xs[0], y1, z1)], [(u0, v0), (u1, v0), (u1, v1), (u0, v1)], np.array([0, ny, d * nz]))


def carbody(m, t):
    base = section(t); L2 = t["L"] / 2; H = t["H"]
    if t.get("cab"):  # cab at +z only
        rings = [(-L2, base)] + sorted([(L2 - dz, ring(base, h or H + 1, NOSE_K[dz])) for dz, h in NOSE], key=lambda r: r[0])
    else:
        rings = [(-L2, base), (L2, base)]
    loft(m, t, rings)
    for (z, pts), d in ((rings[0], -1), (rings[-1], 1)):
        if t.get("cab") and d > 0:
            cap(m, "body", pts, z, d, lambda p: FL)
        else:
            hw = t["W"] / 2; u0, v0, u1, v1 = rt.GANGWAY
            cap(m, "door", pts, z, d, lambda p, d=d: (u0 + (u1 - u0) * (0.5 + d * p[0] / (2 * hw)), v0 + (v1 - v0) * (H - p[1]) / (H - t["yb"])))
    return rings


def rail_wheel(m, zc, r, s, seg=16):
    """Rail wheel: outer face disc from the bogie atlas + tread band, flange lip inboard."""
    xo = s * (GAUGE / 2 + 0.07); xi = s * (GAUGE / 2 - 0.06)
    u0, v0, u1, v1 = rt.RWHEEL; cu, cv, ru, rv = (u0 + u1) / 2, (v0 + v1) / 2, (u1 - u0) / 2 * 0.97, (v1 - v0) / 2 * 0.97
    ring_ = [(math.cos(2 * math.pi * k / seg), math.sin(2 * math.pi * k / seg)) for k in range(seg)]
    p = lambda c, x, f=1.0: (x, r + r * f * c[1], zc + r * f * c[0])
    for k in range(seg):
        c0, c1 = ring_[k], ring_[(k + 1) % seg]
        m.tri("bogie", [p((0, 0), xo), p(c0, xo), p(c1, xo)], [(cu, cv), (cu + ru * c0[0] * s, cv - rv * c0[1]), (cu + ru * c1[0] * s, cv - rv * c1[1])], np.array([s, 0, 0]))
        m.quad("metal", [p(c0, xi), p(c0, xo), p(c1, xo), p(c1, xi)], [METAL] * 4, np.array([0, c0[1] + c1[1], c0[0] + c1[0]]))
        m.quad("bogie", [p(c0, xi, 1.08), p(c0, xi), p(c1, xi), p(c1, xi, 1.08)], [BD] * 4, np.array([0, c0[1] + c1[1], c0[0] + c1[0]]))  # flange


def bogie(m, t, zc):
    r, wb = t["r"], t["wb"]
    for az in (zc - wb / 2, zc + wb / 2):
        for s in (+1, -1): rail_wheel(m, az, r, s)
        tube(m, "bogie", (-GAUGE / 2, r, az), (GAUGE / 2, r, az), 0.08, BD, 6, caps=False)            # axle
    Lf = wb / rt.FRAME_WB; xf = GAUGE / 2 + 0.20; y0, y1 = r - 0.18, t["yb"] - 0.02
    box(m, "bogie", -xf + 0.01, xf - 0.01, y0, y1, zc - Lf / 2 + 0.05, zc + Lf / 2 - 0.05, BD)       # frame volume (dark)
    f0, fv0, f1, fv1 = rt.FRAME                                                                     # frame face covers the
    reg = (f0, fv0, f1, fv1)                                                                        # wheel hubs (axle boxes)
    for s in (+1, -1): gt.side_quad(m, "bogie", s, xf, zc - Lf / 2, zc + Lf / 2, y0, y1, reg)


def side_windows(m, t, rng):
    hw = t["W"] / 2; L2 = t["L"] / 2
    for row, (wy0, wy1) in enumerate(t["win"]):
        a, b = -L2 + 2.3, L2 - 2.3; n = max(1, int(round((b - a) / t["pitch"]))); step = (b - a) / n
        for s in (+1, -1):
            for k in range(n):
                z0, z1 = a + k * step, a + (k + 1) * step
                if row == 0 and any(abs((z0 + z1) / 2 - dz) < 0.95 for dz in t["doors"]): continue  # door bay
                kk = (k if s < 0 else n - 1 - k) + row * 30
                gt.pane(m, s, hw + 0.004, z0, z1, wy0, wy1, 1, kk, gt.pick(rng, t["mix"]))


def coach(m, t, rng):
    hw, L2, H, yb = t["W"] / 2, t["L"] / 2, t["H"], t["yb"]
    carbody(m, t)
    side_windows(m, t, rng)
    lo_win = t["win"][0]
    cuts = sorted(t["doors"]); spans, z = [], -L2 + 0.4
    for dz in cuts: spans.append((z, dz - 0.72)); z = dz + 0.72
    spans.append((z, L2 - 0.4))
    bands = [(lo_win[0] - 0.34, lo_win[0] - 0.22), (lo_win[0] - 0.16, lo_win[0] - 0.10), (lo_win[1] + 0.08, lo_win[1] + 0.14)]
    if t.get("bilevel"): bands += [(t["win"][1][0] - 0.26, t["win"][1][0] - 0.14)]
    top = t["win"][-1][1]
    for s in (+1, -1):
        for zc in t["doors"]:                                                                       # plug doors, both sides
            gt.side_quad(m, "door", s, hw + 0.008, zc - 0.62, zc + 0.62, yb + 0.10, yb + 2.12, rt.COACH_DOOR)
        for b0, b1 in bands:                                                                        # stripes, broken by doors
            for z0, z1 in spans:
                if z1 - z0 > 0.2: gt.side_quad(m, "trim", s, hw + 0.003, z0, z1, b0, b1, (*FL, *FL))
        gt.side_quad(m, "sign", s, hw + 0.009, L2 - 3.6, L2 - 2.4, top + 0.06, top + 0.26)        # destination
    for zc in (-t["bogies"], t["bogies"]): bogie(m, t, zc)
    for zc in ((-4.2, 0.0, 4.2) if not t.get("bilevel") else (-4.6, 4.6)):                         # underframe equipment
        for sd in (+1, -1):
            box(m, "bogie", sd * 0.25, sd * (hw - 0.32), 0.42, yb, zc - 1.1, zc + 1.1, BD)
            gt.side_quad(m, "bogie", sd, hw - 0.319, zc - 1.1, zc + 1.1, 0.42, yb - 0.02, rt.UNDERBOX)
    for zc in (-L2 + 3.2, L2 - 3.2):                                                               # roof HVAC hatches
        box(m, "body", -0.9, 0.9, H - 0.06, H + 0.14, zc - 1.3, zc + 1.3, FL)
        for sd in (+1, -1): gt.side_quad(m, "grille", sd, 0.901, zc - 1.1, zc + 1.1, H, H + 0.12, rt.LOUVRE)
    ends(m, t, (+1, -1))


def knuckle(m, ze, d, y=0.87):
    """AAR knuckle coupler on its shank, cut lever across the end sill."""
    box(m, "bogie", -0.11, 0.11, y - 0.09, y + 0.09, ze, ze + d * 0.55, BD)                       # shank / draft gear
    box(m, "bogie", -0.17, 0.17, y - 0.16, y + 0.16, ze + d * 0.55, ze + d * 0.80, BD)             # knuckle head
    tube(m, "metal", (-1.1, y + 0.30, ze + d * 0.03), (0.0, y + 0.18, ze + d * 0.50), 0.015, METAL, 4, caps=False)  # cut lever


def ends(m, t, which):
    hw, L2, H, yb = t["W"] / 2, t["L"] / 2, t["H"], t["yb"]
    g0, g1 = t.get("gang", (yb + 0.3, H - 0.55))
    for d in which:
        ze = d * L2
        box(m, "bogie", -hw + 0.15, hw - 0.15, max(0.55, yb - 0.40), yb + 0.02, ze - d * 0.30, ze, BD)  # end sill
        knuckle(m, ze, d)
        for s in (+1, -1):                                                                          # end steps + grab irons
            box(m, "bogie", s * (hw - 0.55), s * (hw - 0.05), 0.42, 0.46, ze - d * 0.30, ze, BD)
            tube(m, "metal", (s * (hw - 0.08), max(0.6, yb - 0.3), ze + d * 0.03), (s * (hw - 0.08), g1 - 0.4, ze + d * 0.03), 0.016, METAL, 4)
        gw, ge = 0.62, ze + d * 0.28                                                                # diaphragm
        for s in (+1, -1):
            m.quad("bellows", [(s * gw, g0, ze), (s * gw, g0, ge), (s * gw, g1, ge), (s * gw, g1, ze)], [(0, 1), (2, 1), (2, 0), (0, 0)], np.array([s, 0, 0]))
        m.quad("bellows", [(-gw, g1, ze), (gw, g1, ze), (gw, g1, ge), (-gw, g1, ge)], [(0, 0), (0, 1), (2, 1), (2, 0)], np.array([0, 1, 0]))
        for x0, x1, y0, y1 in ((-gw - 0.08, -gw + 0.08, g0, g1), (gw - 0.08, gw + 0.08, g0, g1), (-gw, gw, g1 - 0.08, g1 + 0.06), (-gw, gw, g0 - 0.06, g0 + 0.08)):
            box(m, "bogie", x0, x1, y0, y1, ge - 0.03, ge + 0.03, BD)                               # face plate, open centre


def loco(m, t, rng):
    """Diesel-electric cab unit: one raked cab at +z, rear end with diaphragm, radiator fans and exhaust on the roof,
    big fuel tank slung between the trucks."""
    hw, L2, H, yb = t["W"] / 2, t["L"] / 2, t["H"], t["yb"]
    carbody(m, t)
    ze, d = L2, 1
    nose_quad(m, "bogie", ze, d, 1, 0.02, 0.98, hw * 0.94, (*BD, *BD), out=0.008)                  # glazing band
    nose_quad(m, "interior", ze, d, 1, 0.08, 0.92, hw * 0.86, (0, 78, 0.999, 78.999), out=0.014)   # windscreen
    nose_quad(m, "front", ze, d, 3, 0.15, 0.85, 0.50, rt.HEADPOD, out=0.012)                        # headlight pod
    gt.end_quad(m, "front", d, ze + 0.004, hw * 0.94 - 0.02, yb + 0.02, NOSE[-1][1] - 0.02, rt.NOSE)
    box(m, "bogie", -1.30, 1.30, 0.10, yb, ze - 0.25, ze + 0.10, BD)                               # pilot
    gt.end_quad(m, "front", d, ze + 0.105, 1.30, 0.10, yb - 0.01, rt.PLOUGH)
    for x in (-1.15, 1.15):                                                                         # ditch lights
        box(m, "bogie", x - 0.09, x + 0.09, yb - 0.02, yb + 0.16, ze + 0.06, ze + 0.16, BD)
        gt.end_quad(m, "front", d, ze + 0.165, 0.08, yb, yb + 0.14, (0.33, 0.65, 0.43, 0.77))
    knuckle(m, ze, d)
    for s in (+1, -1):
        gt.pane(m, s, hw + 0.004, ze - 2.95, ze - 2.15, 2.75, 3.45, 1, 90, O["tint_light"])         # cab side window
        gt.side_quad(m, "door", s, hw + 0.006, ze - 4.05, ze - 3.25, yb + 0.12, yb + 2.25, rt.CAB_DOOR)  # crew door
        for zz in (ze - 4.12, ze - 3.18):
            tube(m, "metal", (s * (hw + 0.06), yb + 0.3, zz), (s * (hw + 0.06), yb + 2.0, zz), 0.018, METAL, 4)
        for k in range(3):                                                                          # step ladder
            box(m, "bogie", s * (hw - 0.12), s * (hw + 0.08), yb - 0.25 - k * 0.22, yb - 0.21 - k * 0.22, ze - 4.05, ze - 3.25, BD)
        box(m, "bogie", s * (hw - 0.02), s * (hw + 0.12), 3.05, 3.30, ze - 2.05, ze - 1.90, BD)  # mirror / camera
        for b0, b1 in ((1.40, 1.62), (1.72, 1.80)):                                                 # livery stripes
            gt.side_quad(m, "trim", s, hw + 0.003, -L2 + 0.3, ze - 1.3, b0, b1, (*FL, *FL))
        nz = 4; a, b = -L2 + 1.0, -L2 + 9.0; step = (b - a) / nz                                    # radiator / engine intakes
        for k in range(nz):
            gt.side_quad(m, "grille", s, hw + 0.005, a + k * step + 0.06, a + (k + 1) * step - 0.06, 2.55, 3.55, rt.LOUVRE)
        box(m, "bogie", s * 0.25, s * (hw - 0.22), 0.35, yb, -3.6, 3.6, BD)                         # fuel tank
        gt.side_quad(m, "bogie", s, hw - 0.219, -3.6, 3.6, 0.35, yb - 0.02, rt.UNDERBOX)
    for zc in (-t["bogies"], t["bogies"]): bogie(m, t, zc)
    u0, v0, u1, v1 = rt.FAN
    for k in range(3):                                                                              # radiator fans
        zc = -L2 + 1.7 + k * 1.75
        box(m, "grille", -0.75, 0.75, H - 0.05, H + 0.10, zc - 0.75, zc + 0.75, rt.FAN[:2])
        m.quad("grille", [(-0.75, H + 0.101, zc - 0.75), (0.75, H + 0.101, zc - 0.75), (0.75, H + 0.101, zc + 0.75), (-0.75, H + 0.101, zc + 0.75)],
               [(u0, v0), (u1, v0), (u1, v1), (u0, v1)], np.array([0, 1, 0]))
    box(m, "body", -0.95, 0.95, H - 0.05, H + 0.12, -3.5, 2.5, FL)                                  # engine hatch
    box(m, "bogie", -0.22, 0.22, H + 0.12, H + 0.32, -1.6, -0.6, BD)                               # exhaust stack
    for x in (-0.12, 0.0, 0.12):                                                                   # 3-chime horn
        tube(m, "metal", (x, H - 0.02, ze - 2.9), (x, H + 0.08, ze - 2.9 - 0.15 * (1 + abs(x) * 5)), 0.035, METAL, 6)
    ends(m, t, (-1,))


BUILD = {"loco": loco, "coach": coach, "bilevel": coach}
if "subway" in globals(): BUILD["subway"] = subway  # added by a parallel session


def load_maps():
    maps = gc.load_maps()                         # metal from the car photos
    maps.update(rt.build_all())
    maps["trim"] = (maps["body"][0], None)
    gc.MAT_PBR.update({"interior": (0.0, 0.05), "sign": (0.0, 0.3), "door": (0.1, 0.35), "bellows": (0.0, 0.9),
                       "bogie": (0.35, 0.65), "grille": (0.4, 0.5), "front": (0.25, 0.45)})
    return maps


def main():
    os.makedirs(OUT, exist_ok=True)
    maps = load_maps()
    for tn in (sys.argv[1:] or TYPES):
        t = TYPES[tn]
        for i, (bodyc, trim) in enumerate(LIVERIES):
            seed = zlib.crc32(f"{tn}{i}".encode()); rng = np.random.default_rng(seed)
            m = Mesh(); BUILD[tn](m, t, rng)
            maps["sign"] = (tx.sign(SIGNS[i % len(SIGNS)]), None)
            out = os.path.join(OUT, f"TRAIN_{tn}_{bodyc}_{trim}.glb")  # body_stripe: liveries can share a body colour
            gc.write_glb(out, m, maps, COLOURS[bodyc], "alloy", trim=COLOURS[trim],
                         seat=gt.SEATS[seed % len(gt.SEATS)])
        print(f"{tn}: {t['L']:.1f} m, {m.count()} tris, {len(LIVERIES)} liveries, {os.path.getsize(out) / 1e6:.2f} MB per GLB")


if __name__ == "__main__":
    main()
