"""Public service vehicles -> GLB, built on the gen_cars + gen_transit toolkits.

Types:
  police_sedan, police_suv   car hulls from gen_cars + roof lightbar + checker side decal
  ambulance                  van hull + green/yellow battenburg decal + lightbar
  fire                       9 m fire engine: crew cab, roller-shutter lockers, roof ladder, rear chevrons
  refuse                     9.4 m refuse truck: crew cab, compactor body, rear hopper, amber beacon

Same approach as the cars and buses: a few hundred flat-shaded triangles, detail in textures. New here are two small
atlases drawn in code (assets/vehicles/service/tex): `decal` (livery bands, shutters, chevrons) and `beacon`
(lens colours, written as emissive so lightbars glow).

Run:  python gen_service.py [type ...]
Output: assets/vehicles/service/glb/SERVICE_<type>_<livery>.glb
Units: metres, +Y up, front faces +Z, origin on the ground under the vehicle centre.
"""
import os, sys
import numpy as np
from PIL import Image
import gen_cars as gc
import gen_transit as gt
import gen_transit_tex as tx
from gen_cars import Mesh, box, tube, DARK, METAL, COLOURS

BASE = os.path.join(gc.ROOT, "assets", "vehicles", "service")
OUT, TEX = os.path.join(BASE, "glb"), os.path.join(BASE, "tex")
FL = tx.FLAT
W = tx.WIN

# ---------------------------------------------------------------- textures
ROW, NR = 128, 16  # decal atlas: 1024 x 2048, sixteen 128 px rows
D_POLICE, D_AMBULANCE, D_FIRE, D_REFUSE, D_SHUTTER, D_CHEVRON, D_AMB_FRONT, D_POLICE_PLAIN, D_WORKS, D_TOW, D_HAZARD, D_SWEEP, D_UTIL = range(13)
BEACON = {"red": (0.125, 0.5), "blue": (0.375, 0.5), "amber": (0.625, 0.5), "white": (0.875, 0.5)}


def row_uv(r, u0=0.0, u1=1.0, inset=0.004):
    return (u0 + inset, r / NR + inset / 2, u1 - inset, (r + 1) / NR - inset / 2)


def checker(t, y0, rows, size, a, b, x0=0, x1=1024):
    for j in range(rows):
        for i in range((x1 - x0) // size + 1):
            t.rect((x0 + i * size, y0 + j * size, x0 + (i + 1) * size - 1, y0 + (j + 1) * size - 1), fill=a if (i + j) % 2 else b, hv=0.5)


def centred(t, y0, text, px, fill, box_fill=None, pad=26):
    f = tx.font(px); w = t.d.textlength(text, font=f)
    if box_fill: t.rect((512 - w / 2 - pad, y0 + 14, 512 + w / 2 + pad, y0 + ROW - 14), fill=box_fill, hv=0.55)
    t.d.text((512 - w / 2, y0 + (ROW - px) / 2 - px * 0.12), text, font=f, fill=fill)


def decal(store):
    t = tx.Tex(1024, 1024 * NR // 8, (235, 235, 235), 0.5)
    BLUE, YEL, GRN, RED, WHT = (20, 45, 140), (235, 215, 30), (20, 140, 60), (190, 20, 20), (245, 245, 245)
    BLK, GOLD, URED = (12, 12, 14), (200, 160, 50), (200, 25, 30)
    y = D_POLICE * ROW  # US black-and-white: white door panel, gold star badge, black lettering
    t.rect((0, y, 1024, y + ROW), fill=WHT)
    cx, cy = 300, y + 64
    pts = [(cx + (40 if k % 2 == 0 else 17) * np.sin(k * np.pi / 5), cy - (40 if k % 2 == 0 else 17) * np.cos(k * np.pi / 5)) for k in range(10)]
    t.d.polygon(pts, fill=GOLD); t.ell((cx - 9, cy - 9, cx + 9, cy + 9), fill=(150, 110, 30))
    t.d.text((370, y + 26), "POLICE", font=tx.font(58), fill=BLK)
    t.d.text((372, y + 92), "TO PROTECT AND SERVE", font=tx.font(18, False), fill=BLK)
    y = D_AMBULANCE * ROW  # US: blue AMBULANCE on white; Star of Life tile at u 0.875..1
    t.rect((0, y, 1024, y + ROW), fill=WHT)
    f = tx.font(78); w = t.d.textlength("AMBULANCE", font=f); t.d.text((448 - w / 2, y + 18), "AMBULANCE", font=f, fill=(25, 60, 170))
    cx, cy = 960, y + 64; SB = (20, 70, 170)
    for a in (0, 60, 120):
        r_ = np.radians(a); c, s_ = np.cos(r_), np.sin(r_)
        t.d.polygon([(cx + c * dx - s_ * dy, cy + s_ * dx + c * dy) for dx, dy in ((-14, -58), (14, -58), (14, 58), (-14, 58))], fill=SB)
    t.rect((cx - 3, cy - 40, cx + 3, cy + 40), fill=WHT)
    y = D_FIRE * ROW
    t.rect((0, y, 1024, y + ROW), fill=RED); t.rect((0, y + 8, 1024, y + 30), fill=YEL); t.rect((0, y + ROW - 30, 1024, y + ROW - 8), fill=YEL)
    centred(t, y, "FIRE  &  RESCUE", 56, WHT)
    y = D_REFUSE * ROW
    t.rect((0, y, 1024, y + ROW), fill=(25, 110, 55)); t.rect((0, y + ROW - 22, 1024, y + ROW), fill=(240, 240, 240))
    centred(t, y - 10, "CITY WASTE  ·  RECYCLING", 50, WHT)
    y = D_SHUTTER * ROW  # eight 128 px roller-shutter tiles: horizontal slats, frame, lift bar
    for k in range(8):
        x = k * 128; t.rect((x, y, x + 127, y + 127), fill=(70, 72, 75), hv=0.3)
        for j in range(14):
            yy = y + 6 + j * 8; t.rect((x + 5, yy, x + 122, yy + 6), fill=(200, 203, 207), hv=0.75)
        t.rect((x + 18, y + 112, x + 110, y + 120), fill=(40, 40, 42), hv=0.9)
    y = D_CHEVRON * ROW
    t.rect((0, y, 1024, y + ROW), fill=RED)
    for k in range(-2, 20):
        x = k * 80; t.d.polygon([(x, y + ROW), (x + 40, y + ROW), (x + 40 + ROW, y), (x + ROW, y)], fill=YEL)
    y = D_AMB_FRONT * ROW  # mirror-reversed, for the bonnet
    t.rect((0, y, 1024, y + ROW), fill=WHT); tmp = tx.Tex(1024, ROW, WHT); centred(tmp, 0, "AMBULANCE", 76, URED)
    t.img.paste(tmp.img.transpose(Image.FLIP_LEFT_RIGHT), (0, y))
    y = D_POLICE_PLAIN * ROW; t.rect((0, y, 1024, y + ROW), fill=WHT); centred(t, y, "1 A 17", 96, BLK)  # roof unit number
    y = D_WORKS * ROW  # city public works: orange band, white lettering, reflective white edge stripes
    ORG = (225, 110, 20)
    t.rect((0, y, 1024, y + ROW), fill=ORG); t.rect((0, y + 6, 1024, y + 18), fill=WHT); t.rect((0, y + ROW - 18, 1024, y + ROW - 6), fill=WHT)
    centred(t, y, "CITY  PUBLIC  WORKS", 54, WHT)
    y = D_TOW * ROW  # towing: dark band, yellow lettering
    t.rect((0, y, 1024, y + ROW), fill=(25, 25, 30)); t.rect((0, y + ROW - 16, 1024, y + ROW - 6), fill=YEL)
    centred(t, y - 6, "24 HR  TOWING  &  RECOVERY", 46, YEL)
    y = D_HAZARD * ROW  # orange / white diagonal hazard stripes (plow blade, outriggers, tailgates)
    t.rect((0, y, 1024, y + ROW), fill=WHT)
    for k in range(-2, 20):
        x = k * 80; t.d.polygon([(x, y + ROW), (x + 40, y + ROW), (x + 40 + ROW, y), (x + ROW, y)], fill=ORG)
    y = D_SWEEP * ROW
    t.rect((0, y, 1024, y + ROW), fill=WHT); t.rect((0, y + 10, 1024, y + 26), fill=(30, 90, 170)); t.rect((0, y + ROW - 26, 1024, y + ROW - 10), fill=(30, 90, 170))
    centred(t, y, "CLEAN  STREETS", 56, (30, 90, 170))
    y = D_UTIL * ROW
    t.rect((0, y, 1024, y + ROW), fill=WHT); t.rect((0, y + ROW - 30, 1024, y + ROW - 10), fill=(30, 90, 170))
    centred(t, y - 8, "CITY  POWER  &  LIGHT", 52, (30, 60, 140))
    t.bake("decal", k=3.0, store=store)


def beacon(store):
    t = tx.Tex(256, 64, (0, 0, 0), 0.5)
    for i, c in enumerate([(255, 25, 15), (20, 70, 255), (255, 150, 0), (255, 255, 240)]):
        t.rect((i * 64 + 2, 2, i * 64 + 61, 61), fill=c)
    t.bake("beacon", k=0.5, store=store)


def service_maps(store):
    save = tx.TEX; tx.TEX = TEX
    try: decal(store); beacon(store)
    finally: tx.TEX = save


# ---------------------------------------------------------------- shared parts
def lightbar(m, y, zc, width, colours=("red", "blue"), depth=0.26, h=0.11):
    hw = width / 2
    box(m, "wheel", -hw, hw, y, y + 0.04, zc - depth / 2, zc + depth / 2, DARK)
    n = len(colours); seg = width / n
    for i, c in enumerate(colours):
        x0 = -hw + i * seg
        box(m, "beacon", x0 + 0.01, x0 + seg - 0.01, y + 0.04, y + 0.04 + h, zc - depth / 2 + 0.02, zc + depth / 2 - 0.02, BEACON[c])


def car_band(m, t, row, y0, y1):
    """Livery decal along both doors, kept clear of the wheel arches."""
    hw = t["W"] / 2 + 0.006; za = t["wb"] / 2 - t["r"] - 0.14
    for s in (+1, -1): gt.side_quad(m, "decal", s, hw, -za, za, y0, y1, row_uv(row))


def car_roof(t):
    L = t["L"]; return t["roof"] + 0.045, (t["cab"][1] + t["cab"][2]) / 2 * L / 2


# ---------------------------------------------------------------- cars
POLICE_BODY = {"sedan": dict(gc.TYPES["sedan"], L=5.10, W=1.88, wb=2.95, r=0.34, roof=1.48),
               # unbranded police utility (Explorer-class): long, tall, steep D-pillar, short overhangs
               "suv": dict(gc.TYPES["suv"], L=5.05, W=2.00, wb=3.03, r=0.38, yb=0.30, belt=1.10, hood=1.06, deck=1.12,
                           roof=1.78, cab=(-0.90, -0.82, 0.12, 0.40))}


def build_police(base):
    t = dict(POLICE_BODY[base], seg=16); m = gc.build_car(t)
    car_band(m, t, D_POLICE, t["yb"] + 0.10, t["belt"] - 0.03)
    y, zc = car_roof(t); v0, v1 = D_POLICE_PLAIN / NR + 0.005, (D_POLICE_PLAIN + 1) / NR - 0.005
    m.quad("decal", [(-0.55, y + 0.003, zc - 0.95), (-0.55, y + 0.003, zc - 0.20), (0.55, y + 0.003, zc - 0.20), (0.55, y + 0.003, zc - 0.95)],
           [(0.05, v1), (0.95, v1), (0.95, v0), (0.05, v0)], np.array([0, 1, 0]))
    lightbar(m, y, zc, 1.25, ("red", "white", "blue"))
    for s in (+1, -1):  # A-pillar spotlights
        zp = t["cab"][3] * t["L"] / 2
        box(m, "metal", s * (t["W"] / 2 - 0.02) - 0.05, s * (t["W"] / 2 - 0.02) + 0.05, t["belt"] + 0.06, t["belt"] + 0.15, zp - 0.15, zp - 0.02, METAL)
    # push bar
    zf = t["L"] / 2
    for s in (+1, -1): box(m, "wheel", s * 0.30 - 0.03, s * 0.30 + 0.03, t["yb"] + 0.05, t["hood"] - 0.10, zf, zf + 0.10, DARK)
    box(m, "wheel", -0.36, 0.36, t["yb"] + 0.35, t["yb"] + 0.42, zf + 0.04, zf + 0.10, DARK)
    return m


def medium_duty_cab(m):
    """Conventional medium-duty truck cab (Freightliner M2 / International class, generic): long square hood, upright
    flat windscreen, tall square cab. Extruded side silhouette with a front wheel arch; chrome grille, headlamps,
    bumper, fenders, mirrors on arms, step. Painted with the body colour. Returns the key dimensions."""
    C = dict(r=0.48, yb=0.55, hw=1.10, zF=3.70, zw=1.95, z0=0.30, hood=1.62, roof=2.85, fa=2.85)
    t = dict(TRUCK, W=2 * C["hw"], r=C["r"], yb=C["yb"], H=C["roof"])
    zF, zw, z0, hood, roof, yb = C["zF"], C["zw"], C["z0"], C["hood"], C["roof"], C["yb"]
    prof = [(z0, yb)] + gt.arch(C["fa"], C["r"], yb) + [(zF - 0.05, yb), (zF, yb + 0.10), (zF, hood - 0.12), (zF - 0.18, hood),
            (zw + 0.15, hood + 0.06), (zw, hood + 0.10), (zw - 0.32, roof - 0.06), (zw - 0.42, roof), (z0 + 0.06, roof), (z0, roof - 0.06)]
    if gc.area(prof) < 0: prof = prof[::-1]
    gt.extrude(m, t, prof, C["hw"], bev=0.05)
    hw = C["hw"]
    for s in (+1, -1):
        gt.wheel(m, C["fa"], C["r"], hw + 0.02, s, tw=0.32, rec=0.08, seg=12); gt.arch_liner(m, C["fa"], C["r"], hw, s)
        # side glass: door window + small rear quarter, door seam
        gt.side_quad(m, "glass", s, hw + 0.005, z0 + 0.95, zw - 0.20, hood + 0.20, roof - 0.18, tx.tile_uv(W["tinted"]))
        gt.side_quad(m, "glass", s, hw + 0.005, z0 + 0.20, z0 + 0.80, hood + 0.20, roof - 0.18, tx.tile_uv(W["tinted"]))
        for zz in (z0 + 0.88, zw - 0.05):
            gt.side_quad(m, "wheel", s, hw + 0.004, zz - 0.01, zz + 0.01, 0.75, roof - 0.10, (*DARK, *DARK))
        box(m, "metal", s * (hw - 0.02) - 0.12, s * (hw - 0.02) + 0.12, 0.60, 0.66, z0 + 0.95, zw - 0.10, METAL)  # step
        base = np.array([s * hw, hood + 0.35, zw - 0.10]); tip = base + np.array([s * 0.30, 0.10, 0.10])
        tube(m, "metal", base, tip, 0.02, METAL, 4, caps=False)
        box(m, "wheel", tip[0] - 0.05, tip[0] + 0.05, tip[1] - 0.45, tip[1] + 0.02, tip[2] - 0.03, tip[2] + 0.02, DARK)
        box(m, "body", s * (hw - 0.02) - 0.10 * s, s * (hw + 0.10), C["r"] * 2 + 0.05, C["r"] * 2 + 0.12, C["fa"] - 0.70, C["fa"] + 0.70, FL)  # fender flare
        box(m, "beacon", s * 0.78 - 0.17, s * 0.78 + 0.17, hood - 0.40, hood - 0.18, zF, zF + 0.02, BEACON["white"])  # headlamp
        box(m, "rear", s * 0.98 - 0.06, s * 0.98 + 0.06, hood - 0.40, hood - 0.18, zF - 0.10, zF + 0.01, (0.30, 0.20))  # amber side marker
    # windscreen (slanted), grille, bumper
    wuv = tx.tile_uv(W["windshield"])
    A, B = np.array([hood + 0.10, zw]), np.array([roof - 0.06, zw - 0.32])  # windscreen edge of the silhouette (y, z)
    d = (B - A) / np.linalg.norm(B - A); nrm = np.array([-d[1], d[0]]) * -1  # outward (up/forward)
    if nrm[1] < 0: nrm = -nrm
    P = lambda f, x: (x, *(A + (B - A) * f + nrm * 0.012))
    m.quad("glass", [P(0.06, -hw + 0.10), P(0.06, hw - 0.10), P(0.94, hw - 0.10), P(0.94, -hw + 0.10)],
           [(wuv[0], wuv[3]), (wuv[2], wuv[3]), (wuv[2], wuv[1]), (wuv[0], wuv[1])], np.array([0, nrm[0], nrm[1]]))
    box(m, "metal", -0.55, 0.55, yb + 0.25, hood - 0.08, zF - 0.02, zF + 0.03, METAL)    # chrome grille surround
    m.quad("wheel", [(-0.47, yb + 0.32, zF + 0.035), (0.47, yb + 0.32, zF + 0.035), (0.47, hood - 0.15, zF + 0.035), (-0.47, hood - 0.15, zF + 0.035)], [DARK] * 4, np.array([0, 0, 1]))
    for k in range(5):
        yy = yb + 0.40 + k * 0.14
        box(m, "metal", -0.47, 0.47, yy, yy + 0.03, zF + 0.03, zF + 0.05, METAL)  # grille bars
    box(m, "metal", -hw - 0.05, hw + 0.05, 0.40, 0.62, zF, zF + 0.18, METAL)          # bumper
    C["t"] = t
    return C


def build_ambulance():
    """US Type I on a medium-duty chassis: tall short-nosed cab + square module. Wide livery band (trim colour) with a
    white pinstripe across cab and module, blue AMBULANCE, Stars of Life (sides, rear, big one on the roof for the top-down
    camera), red/yellow chevrons on the rear, compartment doors with chrome latches, roof A/C condenser, diamond-plate step."""
    m = Mesh(); CAB = medium_duty_cab(m)
    zr, zf = -4.10, CAB["z0"] - 0.05
    for zc in (-2.55,):  # dual rear wheels under the module + dark fender/chassis
        for s_ in (+1, -1):
            gt.wheel(m, zc, CAB["r"], 1.18, s_, tw=0.46, rec=0.13, seg=12)
    box(m, "wheel", -0.45, 0.45, 0.50, 1.00, zr + 0.3, zf + 0.1, DARK)  # frame rails
    for s_ in (+1, -1):
        box(m, "wheel", s_ * 0.55, s_ * 1.20, 0.98, 1.02, -3.25, -1.85, DARK)  # fender lip over the duals
    hw, y0, y1 = 1.22, 1.00, 3.30
    bu = lambda z, y: (z / tx.BODY_PITCH, 1 - y / tx.BODY_H)
    for s in (+1, -1):
        m.quad("box", [(s * hw, y0, zr), (s * hw, y0, zf), (s * hw, y1, zf), (s * hw, y1, zr)], [bu(zr, y0), bu(zf, y0), bu(zf, y1), bu(zr, y1)], np.array([s, 0, 0]))
    m.quad("box", [(-hw, y1, zr), (hw, y1, zr), (hw, y1, zf), (-hw, y1, zf)], [FL] * 4, np.array([0, 1, 0]))
    for z, d in ((zf, 1), (zr, -1)):
        m.quad("box", [(-hw, y0, z), (hw, y0, z), (hw, y1, z), (-hw, y1, z)], [FL] * 4, np.array([0, 0, d]))
    m.quad("wheel", [(-hw, y0, zr), (hw, y0, zr), (hw, y0, zf), (-hw, y0, zf)], [DARK] * 4, np.array([0, -1, 0]))
    # livery band + pinstripe: module sides and cab doors
    for s in (+1, -1):
        xu = hw + 0.004; x = s * xu; xc = CAB["hw"] + 0.006
        gt.side_quad(m, "trim", s, xu, zr, zf, y0 + 0.05, 1.75, (*FL, *FL))
        gt.side_quad(m, "box", s, xu + 0.001, zr, zf, 1.42, 1.50, (*FL, *FL))
        gt.side_quad(m, "trim", s, xc, CAB["z0"] + 0.05, CAB["zF"] - 0.05, 1.02, 1.62, (*FL, *FL))
        gt.side_quad(m, "box", s, xc + 0.001, CAB["z0"] + 0.05, CAB["zF"] - 0.05, 1.30, 1.36, (*FL, *FL))
        gt.side_quad(m, "decal", s, xu + 0.002, zr + 2.35, zf - 0.12, 2.00, 2.42, row_uv(D_AMBULANCE, 0.06, 0.82))
        gt.side_quad(m, "decal", s, xu + 0.002, zr + 1.30, zr + 2.20, 2.02, 2.92, row_uv(D_AMBULANCE, 0.875, 1.0))
        # compartment doors (seams) + chrome latches, side window, lamps
        for za, zb in ((zr + 1.45, zr + 2.35), (zf - 1.05, zf - 0.15), (zf - 2.10, zf - 1.15)):
            for zz in (za, zb): gt.side_quad(m, "wheel", s, xu + 0.003, zz - 0.01, zz + 0.01, y0 + 0.05, y1 - 0.35, (*DARK, *DARK))
            gt.side_quad(m, "wheel", s, xu + 0.003, za, zb, y1 - 0.36, y1 - 0.34, (*DARK, *DARK))
            box(m, "metal", x - 0.01 * s, x + 0.03 * s, 1.55, 1.65, zb - 0.22, zb - 0.10, METAL)
        gt.side_quad(m, "glass", s, xu + 0.003, zf - 0.95, zf - 0.30, 2.50, 2.88, tx.tile_uv(W["tinted"]))
        for z, c in ((zr + 0.15, "red"), (zf - 0.15, "red")):
            box(m, "beacon", x - 0.03 * s, x + 0.02 * s, y1 - 0.25, y1 - 0.08, z - 0.12, z + 0.12, BEACON[c])
        for z in (zr + 1.0, (zr + zf) / 2 + 0.6):
            box(m, "beacon", x - 0.03 * s, x + 0.02 * s, y1 - 0.25, y1 - 0.10, z - 0.15, z + 0.15, BEACON["white"])  # scene lights
        box(m, "beacon", x - 0.03 * s, x + 0.02 * s, 1.20, 1.35, (zr + zf) / 2 - 0.08, (zr + zf) / 2 + 0.08, BEACON["red"])  # mid intersection lamp
    # rear face: chevrons, doors with windows, grab handles, lamp columns
    zq = zr - 0.004
    gt.end_quad(m, "decal", -1, zq, hw - 0.01, y0 + 0.02, y1 - 0.40, row_uv(D_CHEVRON, 0, 0.45))
    gt.end_quad(m, "decal", -1, zq, hw - 0.01, y1 - 0.40, y1 - 0.02, row_uv(D_CHEVRON, 0.5, 1.0))
    for s in (+1, -1):
        xa, xb = s * 0.12, s * 0.80
        m.quad("box", [(xb, y0 + 0.10, zq - 0.003), (xa, y0 + 0.10, zq - 0.003), (xa, y1 - 0.45, zq - 0.003), (xb, y1 - 0.45, zq - 0.003)], [FL] * 4, np.array([0, 0, -1]))
        wa, wb_ = s * 0.25, s * 0.68
        m.quad("glass", [(wb_, 2.25, zq - 0.006), (wa, 2.25, zq - 0.006), (wa, 2.75, zq - 0.006), (wb_, 2.75, zq - 0.006)],
               [(lambda q: [(q[0], q[3]), (q[2], q[3]), (q[2], q[1]), (q[0], q[1])])(tx.tile_uv(W["rearwin"]))][0], np.array([0, 0, -1]))
        box(m, "metal", s * 0.45 - 0.20, s * 0.45 + 0.20, 1.65, 1.70, zq - 0.06, zq - 0.003, METAL)  # grab handle
        xl = s * (hw - 0.17)
        for k, c in enumerate(("red", "red", "amber", "white")):
            yy = y1 - 0.25 - k * 0.42 if k else y1 - 0.25
            box(m, "beacon" if k != 3 else "rear", xl - 0.12, xl + 0.12, yy - 0.15, yy, zq - 0.03, zq, BEACON[c] if k != 3 else (0.07, 0.06))
    box(m, "beacon", -0.25, 0.25, y1 - 0.25, y1 - 0.08, zq - 0.03, zq, BEACON["amber"])
    box(m, "metal", -hw, hw, 0.55, 0.68, zr - 0.40, zr, METAL)  # diamond-plate step bumper
    # front of module over the cab: red + white
    for s in (+1, -1):
        box(m, "beacon", s * (hw - 0.17) - 0.13, s * (hw - 0.17) + 0.13, y1 - 0.25, y1 - 0.08, zf, zf + 0.03, BEACON["red"])
    box(m, "beacon", -0.22, 0.22, y1 - 0.25, y1 - 0.08, zf, zf + 0.03, BEACON["white"])
    # roof: big Star of Life (reads from the top-down camera) + A/C condenser with fans + roof lamps
    v0, v1 = D_AMBULANCE / NR + 0.002, (D_AMBULANCE + 1) / NR - 0.002; zc = zr + 1.5
    m.quad("decal", [(-0.85, y1 + 0.003, zc - 0.85), (-0.85, y1 + 0.003, zc + 0.85), (0.85, y1 + 0.003, zc + 0.85), (0.85, y1 + 0.003, zc - 0.85)],
           [(0.879, v1), (0.996, v1), (0.996, v0), (0.879, v0)], np.array([0, 1, 0]))
    box(m, "metal", -hw + 0.15, hw - 0.15, y1, y1 + 0.18, zf - 0.75, zf - 0.08, METAL)
    for k in range(4):
        xx = -0.75 + k * 0.5
        m.quad("wheel", [(xx - 0.18, y1 + 0.183, zf - 0.60), (xx - 0.18, y1 + 0.183, zf - 0.24), (xx + 0.18, y1 + 0.183, zf - 0.24), (xx + 0.18, y1 + 0.183, zf - 0.60)], [DARK] * 4, np.array([0, 1, 0]))
    lightbar(m, CAB["roof"], (CAB["z0"] + CAB["zw"]) / 2 - 0.1, 1.40, ("red", "white", "red"))
    zf2, yh = CAB["zF"], CAB["hood"] + 0.035; v0, v1 = D_AMB_FRONT / NR + 0.002, (D_AMB_FRONT + 1) / NR - 0.002  # mirrored on the hood
    hy = lambda z: CAB["hood"] + 0.06 * (CAB["zF"] - 0.18 - z) / (CAB["zF"] - 0.33 - CAB["zw"]) + 0.045  # follows the hood slope, above its crown
    m.quad("decal", [(0.55, hy(zf2 - 0.75), zf2 - 0.75), (-0.55, hy(zf2 - 0.75), zf2 - 0.75), (-0.55, hy(zf2 - 0.45), zf2 - 0.45), (0.55, hy(zf2 - 0.45), zf2 - 0.45)],
           [(0.004, v1), (0.996, v1), (0.996, v0), (0.004, v0)], np.array([0, 1, 0]))
    return m


# ---------------------------------------------------------------- trucks
TRUCK = dict(W=2.50, yb=0.42, r=0.50)


def cab(m, z0, z1, H, front_axle, roof_kit):
    """Crew cab with forward-control flat front: windscreen, fascia, two side window rows, door seams, mirrors."""
    t = dict(TRUCK, H=H); hw = t["W"] / 2
    sec = dict(z0=z0, z1=z1, axles=[(front_axle, 0)], front="flat", rear="joint")
    gt.extrude(m, t, gt.profile(t, sec), hw)
    for s in (+1, -1):
        gt.wheel(m, front_axle, t["r"], hw, s, tw=0.32, rec=0.08, seg=12); gt.arch_liner(m, front_axle, t["r"], hw, s)
    zf = z1 + 0.004
    gt.end_quad(m, "glass", 1, zf, hw - 0.10, 1.70, H - 0.25, tx.tile_uv(W["windshield"]))
    gt.end_quad(m, "front", 1, zf, hw - 0.05, t["yb"] + 0.06, 1.62, gt.FASCIA)
    box(m, "wheel", -hw - 0.02, hw + 0.02, 0.32, 0.55, z1 - 0.05, z1 + 0.12, DARK)  # bumper
    for s in (+1, -1):
        gt.side_quad(m, "glass", s, hw + 0.005, z1 - 1.05, z1 - 0.20, 1.72, H - 0.30, tx.tile_uv(W["tinted"]))
        gt.side_quad(m, "glass", s, hw + 0.005, z0 + 0.25, z1 - 1.20, 1.72, H - 0.30, tx.tile_uv(W["tinted"]))
        for zz in (z1 - 1.12, z0 + 0.15):
            gt.side_quad(m, "wheel", s, hw + 0.004, zz - 0.012, zz + 0.012, 0.80, H - 0.20, (*DARK, *DARK))
        base = np.array([s * (hw - 0.05), H - 0.45, z1 - 0.10]); tip = base + np.array([s * 0.32, 0.05, 0.20])
        tube(m, "wheel", base, tip, 0.025, DARK, 4, caps=False)
        box(m, "wheel", tip[0] - 0.06, tip[0] + 0.06, tip[1] - 0.50, tip[1] + 0.02, tip[2] - 0.04, tip[2] + 0.02, DARK)
        box(m, "wheel", s * (hw - 0.02) - 0.10, s * (hw - 0.02) + 0.10, 0.62, 0.68, z1 - 1.0, z1 - 0.40, DARK)  # step
    roof_kit(m, H, (z0 + z1) / 2 + 0.3)
    box(m, "metal", hw - 0.40, hw - 0.02, 0.45, 0.85, z0 - 1.6, z0 - 0.5, METAL)  # fuel tank (street side)
    box(m, "wheel", -hw + 0.02, -hw + 0.40, 0.45, 0.85, z0 - 1.6, z0 - 0.9, DARK)  # battery box
    return t


def body_box(m, z0, z1, H, rear_axle, rear="tail"):
    t = dict(TRUCK, H=H); hw = t["W"] / 2
    sec = dict(z0=z0, z1=z1, axles=[(rear_axle, 1)], front="joint", rear=rear)
    gt.extrude(m, t, gt.profile(t, sec), hw)
    for s in (+1, -1):  # mudflaps behind the rear wheels
        gt.side_quad(m, "wheel", s, hw - 0.20, rear_axle - 0.75, rear_axle - 0.70, 0.25, 0.75, (*DARK, *DARK))
        m.quad("wheel", [(s * (hw - 0.02), 0.22, rear_axle - 0.68), (s * (hw - 0.46), 0.22, rear_axle - 0.68), (s * (hw - 0.46), 0.80, rear_axle - 0.68), (s * (hw - 0.02), 0.80, rear_axle - 0.68)], [DARK] * 4, np.array([0, 0, -1]))
    for s in (+1, -1):
        gt.wheel(m, rear_axle, t["r"], hw, s, tw=0.44, rec=0.13, seg=12); gt.arch_liner(m, rear_axle, t["r"], hw, s)
    return t


def build_fire():
    m = Mesh(); hw = TRUCK["W"] / 2; Hc, Hb = 3.0, 3.05
    cab(m, 2.35, 4.55, Hc, 3.35, lambda m, H, zc: lightbar(m, H + 0.01, 3.9, 1.8, ("red", "white", "red")))
    body_box(m, -4.45, 2.35, Hb, -2.30)
    for s in (+1, -1):
        gt.side_quad(m, "decal", s, hw + 0.006, 2.45, 4.25, 1.05, 1.55, row_uv(D_FIRE))
        gt.side_quad(m, "decal", s, hw + 0.006, -4.30, 2.25, 0.52, 0.72, row_uv(D_CHEVRON, 0, 0.5))
        for k, (a, b) in enumerate([(1.20, 2.20), (0.15, 1.15), (-1.10, 0.10), (-4.30, -3.10)]):  # roller-shutter lockers
            gt.side_quad(m, "decal", s, hw + 0.008, a, b, 0.82, Hb - 0.18, row_uv(D_SHUTTER, k / 8, (k + 1) / 8))
        gt.side_quad(m, "decal", s, hw + 0.008, -2.95, -1.65, 1.30, Hb - 0.18, row_uv(D_SHUTTER, 4 / 8, 5 / 8))  # over the arch
    gt.end_quad(m, "decal", -1, -4.454, hw - 0.04, 0.45, 1.20, row_uv(D_CHEVRON))
    gt.end_quad(m, "decal", -1, -4.452, hw - 0.30, 1.30, Hb - 0.20, row_uv(D_SHUTTER, 5 / 8, 6 / 8))
    gt.end_quad(m, "rear", -1, -4.456, hw - 0.04, 0.45, 0.70, gt.TAIL)
    # roof ladder: two rails + rungs on a turntable at the rear, cradle at the cab
    yl = Hb + 0.30
    box(m, "metal", -0.70, 0.70, Hb, Hb + 0.12, -4.10, -3.10, METAL)  # turntable
    box(m, "wheel", -0.20, 0.20, Hb + 0.12, yl, -3.90, -3.30, DARK)
    for x in (-0.45, 0.45): box(m, "metal", x - 0.03, x + 0.03, yl, yl + 0.12, -4.4, 3.7, METAL)
    for k in range(23):
        z = -4.2 + k * 0.35
        m.quad("metal", [(-0.42, yl + 0.10, z), (0.42, yl + 0.10, z), (0.42, yl + 0.10, z + 0.05), (-0.42, yl + 0.10, z + 0.05)], [METAL] * 4, np.array([0, 1, 0]))
    box(m, "wheel", -0.55, 0.55, Hc, yl, 3.50, 3.65, DARK)  # cradle
    for s in (+1, -1):  # rear beacons
        box(m, "beacon", s * (hw - 0.18) - 0.07, s * (hw - 0.18) + 0.07, Hb, Hb + 0.12, -4.40, -4.25, BEACON["red"])
    return m


def build_refuse():
    m = Mesh(); hw = TRUCK["W"] / 2; Hc, Hb = 3.0, 3.35
    cab(m, 2.45, 4.65, Hc, 3.45, lambda m, H, zc: lightbar(m, H + 0.01, 3.9, 0.9, ("amber", "amber")))
    body_box(m, -4.00, 2.40, Hb, -2.10)
    for s in (+1, -1):
        gt.side_quad(m, "decal", s, hw + 0.006, -3.80, 2.20, 1.65, 2.35, row_uv(D_REFUSE))
        gt.side_quad(m, "decal", s, hw + 0.006, 2.55, 4.40, 1.20, 1.50, row_uv(D_REFUSE, 0.30, 0.70))
        for zz in (-1.0, 0.6):  # body panel ribs
            box(m, "trim", s * hw - 0.03, s * hw + 0.03 * s, 0.90, Hb - 0.10, zz - 0.05, zz + 0.05, FL)
    # rear hopper / tailgate: slanted box with chevrons, bin lift
    z0, z1 = -5.30, -4.00
    box(m, "trim", -hw + 0.02, hw - 0.02, 0.55, Hb - 0.15, z0 + 0.45, z1, FL)
    m.quad("trim", [(-hw + 0.02, 0.55, z0), (hw - 0.02, 0.55, z0), (hw - 0.02, Hb - 0.15, z0 + 0.45), (-hw + 0.02, Hb - 0.15, z0 + 0.45)],
           [FL] * 4, np.array([0, 0.3, -1]))
    for s in (+1, -1):
        m.quad("trim", [(s * (hw - 0.02), 0.55, z0), (s * (hw - 0.02), 0.55, z0 + 0.45), (s * (hw - 0.02), Hb - 0.15, z0 + 0.45), (s * (hw - 0.02), Hb - 0.15, z0 + 0.449)],
               [FL] * 4, np.array([s, 0, 0]))
    m.quad("decal", [(-hw + 0.06, 0.62, z0 - 0.005), (hw - 0.06, 0.62, z0 - 0.005), (hw - 0.06, 1.20, z0 + 0.17), (-hw + 0.06, 1.20, z0 + 0.17)],
           (lambda q: [(q[0], q[3]), (q[2], q[3]), (q[2], q[1]), (q[0], q[1])])(row_uv(D_CHEVRON)), np.array([0, 0.3, -1]))
    box(m, "wheel", -hw + 0.25, hw - 0.25, 1.30, 2.40, z0 + 0.10, z0 + 0.20, DARK)  # hopper mouth
    box(m, "metal", -0.85, 0.85, 0.45, 0.60, z0 - 0.25, z0 - 0.05, METAL)  # bin lift bar
    for s in (+1, -1):
        box(m, "beacon", s * (hw - 0.18) - 0.07, s * (hw - 0.18) + 0.07, Hb - 0.15, Hb - 0.03, z0 + 0.40, z0 + 0.55, BEACON["amber"])
        box(m, "rear", s * (hw - 0.30) - 0.12, s * (hw - 0.30) + 0.12, 0.60, 0.85, z0 - 0.04, z0 + 0.01, (0.07, 0.06))
    return m


# ---------------------------------------------------------------- main
TYPES = {
    "police_sedan": (lambda: build_police("sedan"), "car", ["black"]),
    "police_suv":   (lambda: build_police("suv"), "car", ["black"]),
    "ambulance":    (build_ambulance, "truck", [("white", "orange"), ("white", "red"), ("red", "red")]),
    "fire":         (build_fire, "truck", ["red", "yellow"]),
    "refuse":       (build_refuse, "truck", ["white", "green"]),
}


def all_types():
    from gen_service_works import WORKS_TYPES  # street sweeper, bucket, tow, dump, snow plow (imports this module)
    return {**TYPES, **WORKS_TYPES}


def main():
    os.makedirs(OUT, exist_ok=True)
    extra = {}; service_maps(extra)
    car_maps = gc.load_maps(); car_maps.update(extra)
    truck_maps = gt.load_maps(); truck_maps.update(extra); car_maps["box"] = truck_maps["body"]; car_maps["trim"] = (truck_maps["body"][0], None); truck_maps["box"] = truck_maps["body"]
    gc.MAT_PBR.update({"decal": (0.1, 0.35), "beacon": (0.0, 0.15), "box": (0.3, 0.35)})
    types = all_types()
    for tn in (sys.argv[1:] or types):
        fn, kind, liveries = types[tn]
        mesh = fn(); maps = car_maps if kind == "car" else truck_maps
        for lv in liveries:
            col, band = lv if isinstance(lv, tuple) else (lv, None)
            if tn == "ambulance": name = f"{band}" if col == "white" else f"{col}cab"
            else: name = col if band is None else f"{col}_{band}"
            out = os.path.join(OUT, f"SERVICE_{tn}_{name}.glb")
            gc.write_glb(out, mesh, maps, COLOURS[col], "alloy" if kind == "car" else "dark",
                         trim=COLOURS[band] if band else (0.85, 0.86, 0.88))
        print(f"{tn}: {mesh.count()} tris, {len(liveries)} liveries, {os.path.getsize(out) / 1e6:.2f} MB per GLB")


if __name__ == "__main__":
    main()
