"""City works vehicles for gen_service.py (run that script; this module only adds builders to its TYPES).

  sweeper       mechanical street sweeper: forward-control cab, debris hopper, gutter brooms, main broom, arrow board
  bucket        utility / bucket truck: lockered service body, two-stage insulated boom, fibreglass bucket, outriggers
  tow_flatbed   rollback tow truck: steel deck, headboard, winch, tail roller, wheel-lift forks
  tow_hook      wrecker: toolbox body, light tower, A-boom with cable and hook, underlift with L-arms
  dump          road-maintenance tandem dump truck: ribbed steel body, cab protector, hazard-striped tailgate
  snowplow      dump truck in winter fit: angled front blade with markers, salt V-box spreader + spinner

All except the sweeper sit on the conventional medium-duty cab (gen_service.medium_duty_cab).
"""
import numpy as np
import gen_service as S
import gen_transit as gt
from gen_cars import Mesh, box, tube, DARK, METAL

FL, BEACON, row_uv, lightbar = S.FL, S.BEACON, S.row_uv, S.lightbar
D_HAZARD, D_WORKS, D_TOW, D_SWEEP, D_UTIL = S.D_HAZARD, S.D_WORKS, S.D_TOW, S.D_SWEEP, S.D_UTIL


def flip(q):  # (u0, v0, u1, v1) -> quad corner uvs, bottom edge first
    return [(q[0], q[3]), (q[2], q[3]), (q[2], q[1]), (q[0], q[1])]


# ---------------------------------------------------------------- shared
def chassis(m, CAB, zr, axles, hw=1.18, fender_y=1.02):
    """Frame rails from the cab back to zr, dual rear wheels on each axle, black fender lips + mudflaps, tank, battery box."""
    box(m, "wheel", -0.45, 0.45, 0.50, 0.95, zr, CAB["z0"] + 0.1, DARK)
    for za in axles:
        for s in (+1, -1): gt.wheel(m, za, CAB["r"], hw, s, tw=0.46, rec=0.13, seg=12)
    za0, za1 = min(axles) - 0.70, max(axles) + 0.70
    for s in (+1, -1):
        box(m, "wheel", min(s * 0.55, s * (hw + 0.04)), max(s * 0.55, s * (hw + 0.04)), fender_y - 0.03, fender_y + 0.02, za0, za1, DARK)
        m.quad("wheel", [(s * (hw + 0.02), 0.25, za0 - 0.02), (s * (hw - 0.40), 0.25, za0 - 0.02), (s * (hw - 0.40), 0.85, za0 - 0.02),
                         (s * (hw + 0.02), 0.85, za0 - 0.02)], [DARK] * 4, np.array([0, 0, -1]))
    box(m, "metal", 0.70, 1.05, 0.45, 0.85, CAB["z0"] - 1.4, CAB["z0"] - 0.2, METAL)  # fuel tank
    box(m, "wheel", -1.05, -0.70, 0.50, 0.85, CAB["z0"] - 0.9, CAB["z0"] - 0.2, DARK)  # battery box


def tail_lamps(m, z, hw, y=0.75, bar=True):
    for s in (+1, -1):
        box(m, "rear", s * (hw - 0.22) - 0.12, s * (hw - 0.22) + 0.12, y - 0.12, y + 0.06, z - 0.04, z + 0.005, (0.07, 0.06))
    if bar: box(m, "wheel", -hw, hw, y - 0.25, y - 0.12, z - 0.08, z, DARK)  # under-ride bar


def cab_door_decal(m, CAB, row):
    for s in (+1, -1):
        gt.side_quad(m, "decal", s, CAB["hw"] + 0.008, CAB["z0"] + 0.15, CAB["zw"] - 0.15, 1.05, 1.55, row_uv(row))


def amber_bar(m, CAB, width=1.40):
    lightbar(m, CAB["roof"], (CAB["z0"] + CAB["zw"]) / 2 - 0.05, width, ("amber", "white", "amber"))


def sbox(m, mat, s, xi, xo, *rest):  # box between two signed x values
    box(m, mat, min(s * xi, s * xo), max(s * xi, s * xo), *rest)


# ---------------------------------------------------------------- utility / bucket truck
def build_bucket():
    m = Mesh(); CAB = S.medium_duty_cab(m); zr, zf = -3.95, CAB["z0"] - 0.08
    chassis(m, CAB, zr, [-2.40]); amber_bar(m, CAB); cab_door_decal(m, CAB, D_UTIL)
    hw, yd, yt = 1.22, 1.00, 2.00
    box(m, "box", -hw, hw, yd - 0.05, yd + 0.10, zr, zf, FL)  # deck floor
    for s in (+1, -1):  # compartment boxes along both sides with seams, latches, reflective stripe, corner lamps
        sbox(m, "body", s, hw - 0.55, hw, yd, yt, zr, zf, FL)
        for zz in (zr + 1.10, zr + 2.25, zf - 0.85):
            gt.side_quad(m, "wheel", s, hw + 0.003, zz - 0.012, zz + 0.012, yd + 0.05, yt - 0.05, (*DARK, *DARK))
            sbox(m, "metal", s, hw - 0.02, hw + 0.03, yt - 0.35, yt - 0.25, zz - 0.30, zz - 0.15, METAL)
        gt.side_quad(m, "decal", s, hw + 0.004, zr + 0.05, zf - 0.05, yd + 0.10, yd + 0.24, row_uv(D_HAZARD))
        sbox(m, "beacon", s, hw - 0.02, hw + 0.02, yt - 0.20, yt - 0.08, zr + 0.05, zr + 0.25, BEACON["amber"])
    # turret behind the cab; lower boom runs back to the knuckle, insulated upper boom folds forward over it
    zt, yb0, yk = zf - 0.55, yd + 0.10, 2.95
    box(m, "metal", -0.45, 0.45, yb0, yb0 + 0.25, zt - 0.45, zt + 0.45, METAL)
    box(m, "body", -0.30, 0.30, yb0 + 0.25, 2.65, zt - 0.30, zt + 0.30, FL)
    box(m, "box", -0.16, 0.16, 2.55, 2.85, zr + 0.70, zt + 0.10, FL)
    box(m, "trim", -0.13, 0.13, yk - 0.05, yk + 0.18, zr + 0.40, zt - 0.20, FL)
    box(m, "wheel", -0.20, 0.20, 2.50, yk + 0.20, zr + 0.40, zr + 0.75, DARK)  # knuckle
    box(m, "wheel", -0.30, 0.30, yt, 2.55, zr + 1.40, zr + 1.60, DARK)  # boom rest
    tube(m, "metal", (0.0, 2.10, zt - 0.20), (0.0, 2.60, zt - 1.40), 0.07, METAL, 8)  # lift cylinder
    bz0, bz1 = zr - 0.80, zr - 0.05  # fibreglass bucket hung off the tail
    box(m, "trim", -0.40, 0.40, 1.75, 2.85, bz0, bz1, FL)
    box(m, "wheel", -0.42, 0.42, 2.82, 2.88, bz0 - 0.02, bz1 + 0.02, DARK)
    box(m, "wheel", -0.10, 0.10, 2.85, yk + 0.05, bz1 - 0.15, zr + 0.45, DARK)
    for s in (+1, -1):  # folded outriggers with hazard stripes, feet
        sbox(m, "metal", s, hw - 0.05, hw + 0.15, 0.45, 1.00, zr + 0.20, zr + 0.45, METAL)
        gt.side_quad(m, "decal", s, hw + 0.155, zr + 0.20, zr + 0.45, 0.50, 0.95, row_uv(D_HAZARD, 0, 0.25))
        sbox(m, "wheel", s, hw - 0.10, hw + 0.20, 0.08, 0.14, zr + 0.10, zr + 0.55, DARK)
    tail_lamps(m, zr, hw)
    box(m, "metal", -0.50, 0.50, 0.55, 0.62, zr - 0.25, zr, METAL)  # rear step
    return m


# ---------------------------------------------------------------- tow trucks
def build_tow(kind):
    m = Mesh(); CAB = S.medium_duty_cab(m); hw = 1.22; zf = CAB["z0"] - 0.08
    amber_bar(m, CAB, 1.60); cab_door_decal(m, CAB, D_TOW)
    if kind == "flatbed":
        zr, yd = -5.20, 1.18; chassis(m, CAB, zr + 0.6, [-2.80])
        box(m, "metal", -hw - 0.02, hw + 0.02, yd, yd + 0.12, zr, zf - 0.10, METAL)  # steel deck
        for k in range(14):  # deck grooves (top-down read)
            z = zr + 0.25 + k * 0.40
            m.quad("wheel", [(-hw, yd + 0.123, z), (hw, yd + 0.123, z), (hw, yd + 0.123, z + 0.03), (-hw, yd + 0.123, z + 0.03)], [DARK] * 4, np.array([0, 1, 0]))
        for s in (+1, -1):
            sbox(m, "body", s, hw - 0.02, hw + 0.04, yd - 0.10, yd + 0.20, zr, zf - 0.10, FL)  # painted side rail
            gt.side_quad(m, "decal", s, hw + 0.045, zr + 0.10, zf - 0.30, yd - 0.08, yd + 0.04, row_uv(D_HAZARD))
            sbox(m, "wheel", s, hw - 0.70, hw - 0.05, 0.55, 0.95, zf - 1.60, zf - 0.30, DARK)  # under-deck toolbox
            for zz in (zr + 0.6, -1.0, 0.0): sbox(m, "metal", s, hw - 0.04, hw + 0.04, yd + 0.12, yd + 0.20, zz - 0.06, zz + 0.06, METAL)
            sbox(m, "beacon", s, hw - 0.23, hw - 0.07, yd + 0.12, yd + 0.26, zr + 0.02, zr + 0.18, BEACON["amber"])
            sbox(m, "metal", s, 0.40, 0.70, 0.45, 0.58, zr - 0.40, zr - 0.25, METAL)  # wheel-lift forks
        box(m, "body", -hw, hw, yd + 0.12, 2.40, zf - 0.35, zf - 0.15, FL)  # headboard
        for x in (-0.6, 0.0, 0.6): box(m, "wheel", x - 0.15, x + 0.15, yd + 0.40, 2.25, zf - 0.37, zf - 0.34, DARK)
        box(m, "wheel", -0.25, 0.25, yd + 0.12, yd + 0.40, zf - 0.75, zf - 0.35, DARK)  # winch
        tube(m, "metal", (0.0, yd + 0.25, zf - 0.75), (0.0, yd + 0.20, zr + 0.6), 0.015, METAL, 4, caps=False)
        tube(m, "metal", (-hw + 0.1, yd + 0.05, zr + 0.05), (hw - 0.1, yd + 0.05, zr + 0.05), 0.09, METAL, 8)  # tail roller
        box(m, "wheel", -0.35, 0.35, 0.45, 0.60, zr - 0.30, zr + 0.40, DARK)  # stinger
        tail_lamps(m, zr, hw, y=yd - 0.05, bar=False)
        return m
    zr, yd, yt = -3.35, 1.00, 1.85; chassis(m, CAB, zr, [-2.20])  # ---- hook / wrecker
    box(m, "metal", -hw, hw, yd - 0.05, yd + 0.08, zr, zf, METAL)
    for s in (+1, -1):
        sbox(m, "body", s, hw - 0.50, hw, yd, yt, zr + 0.30, zf, FL)  # toolbox sides
        gt.side_quad(m, "wheel", s, hw + 0.003, -0.90, -0.88, yd + 0.05, yt - 0.05, (*DARK, *DARK))
        sbox(m, "metal", s, hw - 0.02, hw + 0.03, yt - 0.30, yt - 0.22, -0.60, -0.40, METAL)
        gt.side_quad(m, "decal", s, hw + 0.004, zr + 0.35, zf - 0.05, yd + 0.05, yd + 0.20, row_uv(D_HAZARD))
        sbox(m, "metal", s, hw - 0.46, hw - 0.34, yt, 2.70, zf - 0.30, zf - 0.18, METAL)  # light tower posts
        sbox(m, "beacon", s, hw - 0.23, hw - 0.07, yt - 0.15, yt, zr + 0.26, zr + 0.30, BEACON["amber"])
        sbox(m, "metal", s, 0.73, 0.87, 0.40, 0.52, zr - 1.75, zr - 1.40, METAL)  # underlift L-arms
    box(m, "metal", -hw + 0.30, hw - 0.30, 2.62, 2.72, zf - 0.30, zf - 0.18, METAL)
    lightbar(m, 2.72, zf - 0.24, 1.6, ("amber", "amber", "white", "amber", "amber"))
    zb = zr + 0.90  # A-boom angled up and back, lift cylinder, sheave head, cable and hook
    box(m, "body", -0.35, 0.35, yd + 0.08, 1.60, zb - 0.35, zb + 0.35, FL)
    tube(m, "body", (0.0, 1.55, zb), (0.0, 2.85, zr - 0.55), 0.15, FL, 6)
    tube(m, "metal", (0.0, 1.20, zb + 0.20), (0.0, 2.20, zr + 0.05), 0.07, METAL, 6)
    box(m, "wheel", -0.12, 0.12, 2.72, 3.00, zr - 0.70, zr - 0.45, DARK)
    tube(m, "metal", (0.0, 2.75, zr - 0.62), (0.0, 1.05, zr - 0.62), 0.012, METAL, 4, caps=False)
    box(m, "metal", -0.07, 0.07, 0.95, 1.08, zr - 0.67, zr - 0.57, METAL)
    box(m, "metal", -0.02, 0.02, 0.85, 0.95, zr - 0.66, zr - 0.58, METAL)
    box(m, "wheel", -0.20, 0.20, 0.42, 0.58, zr - 1.20, zr + 0.40, DARK)  # underlift arm + crossbar
    box(m, "metal", -1.00, 1.00, 0.40, 0.55, zr - 1.40, zr - 1.20, METAL)
    tail_lamps(m, zr + 0.30, hw, bar=False)
    return m


# ---------------------------------------------------------------- dump truck / snow plow
def dump_body(m, CAB, zr, zf, axles):
    """Ribbed steel dump body with flared sides, cab protector, hazard-striped tailgate, hoist, tandem duals."""
    hw, y0, y1 = 1.25, 1.20, 2.30
    for s in (+1, -1):
        m.quad("body", [(s * (hw - 0.04), y0, zr), (s * (hw - 0.04), y0, zf), (s * hw, y1, zf), (s * hw, y1, zr)], [FL] * 4, np.array([s, 0, 0]))
        m.quad("wheel", [(s * (hw - 0.10), y0 + 0.08, zr), (s * (hw - 0.10), y0 + 0.08, zf), (s * (hw - 0.06), y1, zf), (s * (hw - 0.06), y1, zr)], [DARK] * 4, np.array([-s, 0, 0]))
        for k in range(5):
            z = zr + 0.35 + k * (zf - zr - 0.7) / 4
            sbox(m, "body", s, hw - 0.03, hw + 0.05, y0, y1, z - 0.05, z + 0.05, FL)  # ribs
        sbox(m, "body", s, hw - 0.04, hw + 0.07, y1 - 0.08, y1 + 0.02, zr, zf, FL)  # top rail
        sbox(m, "wheel", s, 0.38, 0.62, 0.95, y0, zr + 0.2, zf - 0.2, DARK)  # long sills
    m.quad("wheel", [(-hw, y0 + 0.08, zr), (hw, y0 + 0.08, zr), (hw, y0 + 0.08, zf), (-hw, y0 + 0.08, zf)], [DARK] * 4, np.array([0, 1, 0]))
    m.quad("body", [(-hw, y0, zr), (hw, y0, zr), (hw, y0, zf), (-hw, y0, zf)], [FL] * 4, np.array([0, -1, 0]))
    box(m, "body", -hw, hw, y0, y1 + 0.20, zf - 0.10, zf, FL)  # front wall
    box(m, "body", -hw, hw, y1 + 0.10, y1 + 0.18, zf, CAB["zw"] - 0.30, FL)  # cab protector
    box(m, "body", -hw, hw, y0, y1, zr - 0.10, zr, FL)  # tailgate
    gt.end_quad(m, "decal", -1, zr - 0.104, hw - 0.05, y0 + 0.05, y0 + 0.45, row_uv(D_HAZARD))
    box(m, "metal", -0.20, 0.20, 0.80, 1.20, zf - 0.60, zf - 0.20, METAL)  # hoist
    chassis(m, CAB, zr - 0.05, axles, fender_y=1.10)
    tail_lamps(m, zr - 0.10, hw, y=0.90)
    return hw, y1


def build_dump(plow=False):
    m = Mesh(); CAB = S.medium_duty_cab(m); zf = CAB["z0"] - 0.12; zr = -4.10
    hw, y1 = dump_body(m, CAB, zr, zf, [-2.40, -3.75])
    cab_door_decal(m, CAB, D_WORKS); amber_bar(m, CAB, 1.60)
    for s in (+1, -1): gt.side_quad(m, "decal", s, hw + 0.06, zr + 0.6, zf - 0.6, 1.70, 2.05, row_uv(D_WORKS, 0.05, 0.95))
    if not plow: return m
    zF = CAB["zF"]
    box(m, "wheel", -0.50, 0.50, 0.45, 0.95, zF, zF + 0.55, DARK)  # hitch frame
    box(m, "metal", -0.08, 0.08, 0.80, 1.30, zF + 0.10, zF + 0.55, METAL)  # lift ram
    ang, W2, zc = np.radians(22), 1.75, zF + 0.95  # blade angled to throw snow right; three facets = curved mouldboard
    P = lambda x, y, d: (x * np.cos(ang), y, zc + d + x * np.sin(ang))
    w = np.array([0, 0.2, 1])
    for ya, yb_, da, db in ((0.30, 0.55, 0.10, 0.00), (0.55, 0.95, 0.00, 0.00), (0.95, 1.20, 0.00, 0.12)):
        m.quad("trim", [P(-W2, ya, da), P(W2, ya, da), P(W2, yb_, db), P(-W2, yb_, db)], [FL] * 4, w)
        m.quad("wheel", [P(-W2, ya, da - 0.05), P(W2, ya, da - 0.05), P(W2, yb_, db - 0.05), P(-W2, yb_, db - 0.05)], [DARK] * 4, -w)
    m.quad("decal", [P(-W2, 1.06, 0.075), P(W2, 1.06, 0.075), P(W2, 1.19, 0.125), P(-W2, 1.19, 0.125)], flip(row_uv(D_HAZARD)), w)
    m.quad("metal", [P(-W2, 0.18, 0.13), P(W2, 0.18, 0.13), P(W2, 0.30, 0.105), P(-W2, 0.30, 0.105)], [METAL] * 4, w)  # cutting edge
    for x in (-W2 + 0.05, W2 - 0.05):  # blade-end marker whips
        p = np.array(P(x, 1.20, 0.10)); tube(m, "trim", p, p + np.array([0, 0.9, 0]), 0.015, FL, 4, caps=False)
        box(m, "beacon", p[0] - 0.04, p[0] + 0.04, p[1] + 0.85, p[1] + 0.95, p[2] - 0.04, p[2] + 0.04, BEACON["amber"])
    box(m, "metal", -hw + 0.20, hw - 0.20, y1 - 0.20, y1 + 0.25, zr + 0.30, zf - 0.40, METAL)  # salt V-box
    m.quad("wheel", [(-hw + 0.25, y1 + 0.255, zr + 0.35), (hw - 0.25, y1 + 0.255, zr + 0.35), (hw - 0.25, y1 + 0.255, zf - 0.45),
                     (-hw + 0.25, y1 + 0.255, zf - 0.45)], [DARK] * 4, np.array([0, 1, 0]))
    box(m, "wheel", -0.18, 0.18, 0.60, 1.10, zr - 0.40, zr - 0.10, DARK)  # chute
    tube(m, "metal", (0.0, 0.55, zr - 0.30), (0.0, 0.50, zr - 0.30), 0.32, METAL, 10)  # spinner
    for s in (+1, -1):
        sbox(m, "beacon", s, hw - 0.18, hw - 0.02, y1 + 0.02, y1 + 0.16, zr + 0.02, zr + 0.18, BEACON["amber"])
    return m


# ---------------------------------------------------------------- street sweeper
def build_sweeper():
    m = Mesh(); W = S.TRUCK["W"]; Hc, Hb = 2.85, 2.95
    S.cab(m, 1.30, 3.20, Hc, 2.30, lambda m, H, zc: lightbar(m, H + 0.01, 2.3, 1.2, ("amber", "white", "amber")))
    S.body_box(m, -2.90, 1.30, Hb, -1.70)
    for s in (+1, -1):
        gt.side_quad(m, "decal", s, W / 2 + 0.006, -2.75, 1.15, 1.55, 2.25, row_uv(D_SWEEP))
        gt.side_quad(m, "decal", s, W / 2 + 0.006, 1.45, 2.95, 1.05, 1.45, row_uv(D_SWEEP, 0.2, 0.8))
        gt.side_quad(m, "wheel", s, W / 2 + 0.004, -2.42, -2.40, 0.80, Hb - 0.10, (*DARK, *DARK))  # hopper door seam
        cx, cz = s * (W / 2 + 0.05), 3.05  # gutter broom on a swing arm under the cab corner
        tube(m, "metal", (s * 0.85, 0.55, 1.95), (cx, 0.30, cz), 0.04, METAL, 4, caps=False)
        tube(m, "wheel", (cx, 0.04, cz), (cx, 0.22, cz), 0.48, DARK, 12)
        tube(m, "trim", (cx, 0.22, cz), (cx, 0.30, cz), 0.22, FL, 8)
    tube(m, "wheel", (-1.05, 0.28, -0.20), (1.05, 0.28, -0.20), 0.26, DARK, 10)  # main pick-up broom
    box(m, "wheel", -1.20, 1.20, 0.08, 0.45, 0.25, 0.32, DARK)  # dust skirt
    box(m, "box", -1.05, 1.05, Hb, Hb + 0.22, -2.40, 0.90, FL)  # water tank
    box(m, "metal", 0.50, 0.70, Hb + 0.22, Hb + 0.35, 0.40, 0.60, METAL)
    box(m, "metal", -1.20, 1.20, Hb - 0.12, Hb, -2.95, -2.88, METAL)  # dump door hinge
    box(m, "wheel", -0.80, 0.80, Hb - 0.75, Hb - 0.20, -2.965, -2.93, DARK)  # arrow board
    for k in range(5):
        x = -0.60 + k * 0.30; box(m, "beacon", x - 0.07, x + 0.07, Hb - 0.55, Hb - 0.40, -2.98, -2.965, BEACON["amber"])
    tail_lamps(m, -2.904, W / 2, y=0.70, bar=False)
    return m


WORKS_TYPES = {
    "sweeper":     (build_sweeper, "truck", [("white", "blue"), ("white", "orange")]),
    "bucket":      (build_bucket, "truck", [("white", "yellow"), ("yellow", "white")]),
    "tow_flatbed": (lambda: build_tow("flatbed"), "truck", [("white", "red"), ("red", "white"), ("black", "yellow")]),
    "tow_hook":    (lambda: build_tow("hook"), "truck", [("white", "red"), ("red", "white"), ("black", "yellow")]),
    "dump":        (build_dump, "truck", [("orange", "orange"), ("white", "orange"), ("yellow", "yellow")]),
    "snowplow":    (lambda: build_dump(plow=True), "truck", [("orange", "yellow"), ("yellow", "yellow"), ("white", "orange")]),
}
