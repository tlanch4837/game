"""Commercial and delivery vehicles -> GLB, built on the gen_cars / gen_transit / gen_service toolkits.

Types:
  van        high-roof cargo van (Sprinter / Transit class, generic): car hull, parcel livery, sliding door, twin rear doors
  box        7.5 t box truck: medium-duty cab + insulated box body, roll-up rear door, tail lift
  semi       tractor (sleeper cab, tandem drive) + 13.6 m dry-van trailer on a triaxle, landing legs
  tanker     rigid fuel tanker: elliptical tank barrel, walkway, hose cabinet, hazard placards
  flatbed    flatbed truck: steel deck, headboard, stake pockets, strapped lumber + pallet load
  mixer      cement mixer: tilted spiral drum on a pedestal, water tank, rear chute, tandem rear
  food       step-van food truck: serving hatch with prop-up awning, menu board, roof vent + gas bottles
  scooter    courier scooter with a top box
  cargobike  long-john cargo bike with a front box

Same approach as the cars and service vehicles: a few hundred flat-shaded triangles, detail in one small decal atlas
drawn in code (assets/vehicles/commercial/tex/decal_*). All brands are fictional.

Run:  python gen_commercial.py [type ...]
Output: assets/vehicles/commercial/glb/COMM_<type>_<colour>.glb
Units: metres, +Y up, front faces +Z, origin on the ground under the vehicle centre.
"""
import math, os, sys
import numpy as np
from PIL import Image
import gen_cars as gc
import gen_transit as gt
import gen_transit_tex as tx
import gen_service as sv
from gen_cars import Mesh, box, tube, DARK, METAL, COLOURS

BASE = os.path.join(gc.ROOT, "assets", "vehicles", "commercial")
OUT, TEX = os.path.join(BASE, "glb"), os.path.join(BASE, "tex")
FL = tx.FLAT
W = tx.WIN
BEACON = sv.BEACON
ROW = 128
D_PARCEL, D_MOVERS, D_FREIGHT, D_FUEL, D_FOOD, D_AWNING, D_COURIER, D_HAZARD = range(8)
def row_uv(r, u0=0.0, u1=1.0, inset=0.004):
    return (u0 + inset, r / 8 + inset, u1 - inset, (r + 1) / 8 - inset)


# ---------------------------------------------------------------- textures
def decal(store):
    t = tx.Tex(1024, 1024, (235, 235, 235), 0.5)
    WHT, BLK, RED, YEL = (245, 245, 245), (14, 14, 16), (200, 30, 30), (240, 200, 30)
    y = D_PARCEL * ROW  # parcel van: orange swoosh + wordmark on white
    t.rect((0, y, 1024, y + ROW), fill=WHT)
    t.d.polygon([(0, y + 92), (1024, y + 40), (1024, y + 70), (0, y + 120)], fill=(235, 110, 20))
    t.d.text((60, y + 10), "SWIFTPARCEL", font=tx.font(70), fill=(30, 40, 110))
    t.d.text((64, y + 84), "next-day delivery", font=tx.font(22, False), fill=(30, 40, 110))
    y = D_MOVERS * ROW  # box truck
    t.rect((0, y, 1024, y + ROW), fill=WHT); t.rect((0, y + 100, 1024, y + 118), fill=(20, 110, 170))
    sv.centred(t, y - 14, "METRO MOVERS  ·  STORAGE", 56, (20, 110, 170))
    y = D_FREIGHT * ROW  # trailer
    t.rect((0, y, 1024, y + ROW), fill=WHT)
    t.d.polygon([(40, y + 110), (140, y + 18), (200, y + 18), (100, y + 110)], fill=RED)
    t.d.text((230, y + 22), "NORTHLINE", font=tx.font(64), fill=BLK)
    t.d.text((234, y + 88), "FREIGHT  ·  LOGISTICS", font=tx.font(24, False), fill=(90, 90, 95))
    y = D_FUEL * ROW  # tanker band: polished-look stripe + lettering
    t.rect((0, y, 1024, y + ROW), fill=(200, 202, 206), hv=0.5); t.rect((0, y + 30, 1024, y + 98), fill=(20, 120, 60))
    sv.centred(t, y, "PETROLINE  FUELS", 52, WHT)
    y = D_FOOD * ROW  # food truck menu / wrap
    t.rect((0, y, 1024, y + ROW), fill=(250, 200, 40))
    t.d.text((30, y + 14), "TACO LOCO", font=tx.font(74), fill=(170, 30, 20))
    t.d.text((520, y + 22), "TACOS  3.50", font=tx.font(30), fill=BLK)
    t.d.text((520, y + 58), "BURRITOS  8.00", font=tx.font(30), fill=BLK)
    t.d.text((520, y + 94), "HORCHATA  3.00", font=tx.font(26), fill=BLK)
    y = D_AWNING * ROW  # awning stripes
    for k in range(16): t.rect((k * 64, y, k * 64 + 63, y + ROW), fill=(200, 30, 30) if k % 2 else WHT)
    y = D_COURIER * ROW  # courier box
    t.rect((0, y, 1024, y + ROW), fill=(40, 190, 140)); sv.centred(t, y, "QUICKDROP", 72, WHT)
    y = D_HAZARD * ROW  # tiles: 0 flammable diamond, 1 UN number plate, 2 rear hazard stripes, 3 tread plate
    t.rect((0, y, 1024, y + ROW), fill=WHT)
    cx, cy = 64, y + 64; t.d.polygon([(cx, cy - 60), (cx + 60, cy), (cx, cy + 60), (cx - 60, cy)], fill=RED)
    t.d.text((cx - 32, cy + 6), "3", font=tx.font(34), fill=WHT)
    t.rect((132, y + 20, 252, y + 108), fill=(235, 120, 20)); t.rect((132, y + 62, 252, y + 66), fill=BLK)
    t.d.text((152, y + 24), "33", font=tx.font(30), fill=BLK); t.d.text((142, y + 70), "1203", font=tx.font(30), fill=BLK)
    st = tx.Tex(128, ROW, WHT)
    for k in range(-4, 4):
        x = k * 40; st.d.polygon([(x, ROW), (x + 20, ROW), (x + 20 + ROW, 0), (x + ROW, 0)], fill=RED)
    t.img.paste(st.img, (256, y))
    t.rect((512, y, 1024, y + ROW), fill=(150, 152, 156))
    for i in range(12):
        for j in range(6):
            xx, yy = 392 + i * 10 + (j % 2) * 5, y + 8 + j * 20
            t.rect((xx, yy, xx + 6, yy + 3), hv=0.8)
    t.rect((384, y, 512, y + ROW), fill=(165, 167, 172))
    t.bake("decal", k=3.0, store=store)


def maps_store(store):
    save = tx.TEX; tx.TEX = TEX
    try: decal(store); sv.beacon(store)
    finally: tx.TEX = save


# ---------------------------------------------------------------- helpers
def hz(tile, n=8):  # tile k of the hazard row
    return row_uv(D_HAZARD, tile / n, (tile + 1) / n)


def shift(m, dz):
    for mat in m.tris:
        m.tris[mat] = [([p + np.array([0, 0, dz]) for p in P], uv) for P, uv in m.tris[mat]]


def centre(m):
    zs = [p[2] for L in m.tris.values() for P, _ in L for p in P]
    shift(m, -(min(zs) + max(zs)) / 2)


def module(m, hw, y0, y1, zr, zf, mat="box", under=True):
    """Square box body: textured sides (truck body texture), flat roof and ends."""
    bu = lambda z, y: (z / tx.BODY_PITCH, 1 - y / tx.BODY_H)
    for s in (+1, -1):
        m.quad(mat, [(s * hw, y0, zr), (s * hw, y0, zf), (s * hw, y1, zf), (s * hw, y1, zr)], [bu(zr, y0), bu(zf, y0), bu(zf, y1), bu(zr, y1)], np.array([s, 0, 0]))
    m.quad(mat, [(-hw, y1, zr), (hw, y1, zr), (hw, y1, zf), (-hw, y1, zf)], [FL] * 4, np.array([0, 1, 0]))
    for z, d in ((zf, 1), (zr, -1)):
        m.quad(mat, [(-hw, y0, z), (hw, y0, z), (hw, y1, z), (-hw, y1, z)], [FL] * 4, np.array([0, 0, d]))
    if under: m.quad("wheel", [(-hw, y0, zr), (hw, y0, zr), (hw, y0, zf), (-hw, y0, zf)], [DARK] * 4, np.array([0, -1, 0]))
    for s in (+1, -1):  # corner posts / rub rails
        for z in (zr, zf): box(m, "metal", s * hw - 0.03, s * hw + 0.01 * s, y0, y1, z - 0.03, z + 0.03, METAL)
        box(m, "metal", s * hw - 0.02, s * hw + 0.02 * s, y0, y0 + 0.08, zr, zf, METAL)


def axle(m, zc, r, hw, dual=True, fender=True, y_top=None):
    for s in (+1, -1):
        gt.wheel(m, zc, r, hw, s, tw=0.46 if dual else 0.30, rec=0.13 if dual else 0.08, seg=12)
        if fender:
            yt = y_top or 2 * r + 0.08
            box(m, "wheel", s * (hw - 0.50), s * (hw + 0.02), yt, yt + 0.04, zc - r - 0.15, zc + r + 0.15, DARK)
            gt.side_quad(m, "wheel", s, hw - 0.25, zc - r - 0.22, zc - r - 0.18, 0.25, yt, (*DARK, *DARK))  # mudflap


def frame(m, z0, z1, y=0.62, hw=0.45):
    box(m, "wheel", -hw, hw, y, y + 0.28, z0, z1, DARK)


def rear_lamps(m, z, hw, y=0.62):
    for s in (+1, -1):
        box(m, "rear", s * (hw - 0.25) - 0.15, s * (hw - 0.25) + 0.15, y, y + 0.16, z - 0.04, z, (0.07, 0.06))
        box(m, "beacon", s * (hw - 0.06) - 0.04, s * (hw - 0.06) + 0.04, y, y + 0.16, z - 0.04, z, BEACON["amber"])


def roll_door(m, z, hw, y0, y1):
    """Roll-up rear door: shutter slats from the service atlas tile row is not available here -> slats in geometry."""
    gt.end_quad(m, "box", -1, z - 0.004, hw - 0.08, y0 + 0.05, y1 - 0.08, (*FL, *FL))
    n = int((y1 - y0) / 0.22)
    for k in range(1, n):
        yy = y0 + k * (y1 - y0) / n
        box(m, "metal", -hw + 0.10, hw - 0.10, yy - 0.012, yy + 0.012, z - 0.012, z - 0.004, METAL)
    box(m, "metal", -0.20, 0.20, y0 + 0.12, y0 + 0.20, z - 0.05, z - 0.004, METAL)  # handle


def stripes(m, z, hw, y0, y1):
    gt.end_quad(m, "decal", -1, z - 0.006, hw, y0, y1, hz(2, 8) if False else row_uv(D_HAZARD, 0.25, 0.375))


# ---------------------------------------------------------------- van
VAN = dict(gc.TYPES["van"], L=5.95, W=2.02, wb=3.67, r=0.37, yb=0.34, belt=1.20, hood=1.16, deck=1.20, roof=2.62,
           cab=(-0.985, -0.98, 0.42, 0.655), rails=0, tumble=0.05)  # Sprinter-class: raked A pillar, upright sides
WS_TOP = 2.05  # windscreen top; above it the slanted face is the painted high-roof cap


def build_van():
    t = dict(VAN, seg=16); m = gc.build_car(t); hw = t["W"] / 2 + 0.006; L = t["L"]
    za = t["wb"] / 2 - t["r"] - 0.16
    belt, roof = t["belt"], t["roof"]; zb, zt = t["cab"][3] * L / 2, t["cab"][2] * L / 2  # A-pillar foot / top
    hb, hr = t["W"] / 2 - 0.06, t["W"] / 2 - t["tumble"]
    xw = lambda y: hb + (hr - hb) * (y - belt) / (roof - belt)
    zA = lambda y: zb + (zt - zb) * (y - belt) / (roof - belt)  # A-pillar line
    nrm = lambda P: np.cross(P[1] - P[0], P[2] - P[0]) / np.linalg.norm(np.cross(P[1] - P[0], P[2] - P[0]))
    # drop the car hull's full-length side glass (painted panel underneath shows) and its belt-to-roof windscreen
    m.tris["glass"] = [(P, uv) for P, uv in m.tris["glass"] if abs(nrm(P)[0]) < 0.8 and nrm(P)[2] < 0.5]
    # windscreen (belt..WS_TOP) + painted roof cap (WS_TOP..roof) on the raked plane
    on = np.array([0, zb - zt, roof - belt]); on /= np.linalg.norm(on)
    def slant(mat, ya, yb_, inset, uv, out):
        P = [(-(xw(y) - inset), y, zA(y)) for y in (ya,)] + [((xw(ya) - inset), ya, zA(ya)), ((xw(yb_) - inset), yb_, zA(yb_)), (-(xw(yb_) - inset), yb_, zA(yb_))]
        P = [np.array(q) + on * out for q in P]
        m.quad(mat, P, uv, on)
    slant("body", WS_TOP, roof, 0.0, [gc.PLAIN(0, 0), gc.PLAIN(1, 0), gc.PLAIN(1, 1), gc.PLAIN(0, 1)], 0.0)
    slant("body", belt, WS_TOP, xw(belt) - 0.10, [gc.PLAIN(0, 0)] * 4, 0.0) if False else None
    for s_ in (+1, -1):  # A pillars beside the screen
        P = [(s_ * xw(belt), belt, zA(belt)), (s_ * (xw(belt) - 0.10), belt, zA(belt)), (s_ * (xw(WS_TOP) - 0.10), WS_TOP, zA(WS_TOP)), (s_ * xw(WS_TOP), WS_TOP, zA(WS_TOP))]
        m.quad("body", P, [gc.PLAIN(0, 0)] * 4, on)
    slant("wheel", belt, WS_TOP, 0.10, [DARK] * 4, -0.25)  # dark cab interior seen through the glass
    slant("glass", belt + 0.05, WS_TOP, 0.10, [(0, 1), (1, 1), (1, 0), (0, 0)], 0.008)
    box(m, "wheel", -0.35, 0.35, WS_TOP + 0.12, WS_TOP + 0.20, zA(WS_TOP + 0.16) - 0.02, zA(WS_TOP + 0.16) + 0.06, DARK)  # cap marker lamp strip
    for x in (-0.55, 0.55): box(m, "beacon", x - 0.06, x + 0.06, WS_TOP + 0.13, WS_TOP + 0.19, zA(WS_TOP + 0.16) + 0.04, zA(WS_TOP + 0.16) + 0.07, BEACON["amber"])
    # cab door window: follows the A pillar, top at the windscreen line, rear edge at the B pillar
    y0, y1 = belt + 0.06, WS_TOP - 0.02
    for s in (+1, -1):
        zr0 = zb - 1.05; x = lambda y: s * (xw(y) + 0.010)
        P = [(x(y0), y0, zr0), (x(y0), y0, zA(y0) - 0.10), (x(y1), y1, zA(y1) - 0.08), (x(y1), y1, zr0)]
        m.quad("glass", P, [(0, 1), (1, 1), (1, 0), (0, 0)], np.array([s, 0.05, 0]))
        gt.side_quad(m, "wheel", s, hw - 0.004, zr0 - 0.02, zr0, 0.50, roof - 0.15, (*DARK, *DARK))  # B-pillar seam
        box(m, "wheel", s * (hw - 0.03), s * (hw + 0.025), 0.40, 0.60, -za, za, DARK)  # lower body cladding between the arches
    for s in (+1, -1):
        gt.side_quad(m, "decal", s, hw, -za, za * 0.55, 1.20, 2.15, row_uv(D_PARCEL))
        for zz in (za * 0.62, -za * 0.10):  # sliding-door seams + rail
            gt.side_quad(m, "wheel", s, hw + 0.001, zz - 0.01, zz + 0.01, 0.45, 2.30, (*DARK, *DARK))
        gt.side_quad(m, "wheel", s, hw + 0.001, -za, za * 0.62, 2.20, 2.22, (*DARK, *DARK))
    zr = -L / 2 - 0.004
    gt.end_quad(m, "wheel", -1, zr, 0.012, 0.45, 2.45, (*DARK, *DARK))  # twin-door split
    for s in (+1, -1):
        box(m, "metal", s * 0.12 - 0.05, s * 0.12 + 0.05, 1.20, 1.25, zr - 0.04, zr, METAL)
        box(m, "rear", s * (hw - 0.07) - 0.06, s * (hw - 0.07) + 0.06, 0.95, 1.55, zr - 0.03, zr + 0.02, (0.07, 0.06))
    box(m, "beacon", -0.30, 0.30, 2.50, 2.56, zr - 0.02, zr + 0.02, BEACON["red"])  # high brake light
    box(m, "metal", -0.30, 0.30, 0.42, 0.50, zr - 0.20, zr, METAL)  # rear step
    return m


# ---------------------------------------------------------------- medium-duty trucks (cab: z 0.30..3.70)
def truck_cab(m, beacon=False):
    C = sv.medium_duty_cab(m)
    frame(m, -6.0, C["z0"] + 0.1)
    if beacon: sv.lightbar(m, C["roof"], (C["z0"] + C["zw"]) / 2 - 0.2, 0.7, ("amber", "amber"))
    hw = C["hw"]
    box(m, "metal", hw - 0.42, hw - 0.04, 0.48, 0.88, C["z0"] - 1.3, C["z0"] - 0.2, METAL)  # fuel tank
    box(m, "wheel", -hw + 0.04, -hw + 0.40, 0.48, 0.86, C["z0"] - 1.0, C["z0"] - 0.3, DARK)  # battery box
    return C


def build_box():
    m = Mesh(); C = truck_cab(m); hw, y0, y1 = 1.25, 1.05, 3.55
    zf, zr = C["z0"] - 0.10, -4.55
    frame(m, zr + 0.2, zf)
    axle(m, -2.60, C["r"], 1.20, fender=False)
    module(m, hw, y0, y1, zr, zf)
    box(m, "box", -1.05, 1.05, C["roof"], y1 + 0.02, zf, zf + 0.55, FL)  # luton over the cab
    m.quad("box", [(-1.05, C["roof"], zf + 0.55), (1.05, C["roof"], zf + 0.55), (1.05, y1 - 0.05, zf + 0.75), (-1.05, y1 - 0.05, zf + 0.75)], [FL] * 4, np.array([0, -0.5, 1]))
    for s in (+1, -1):
        gt.side_quad(m, "decal", s, hw + 0.004, zr + 0.30, zf - 0.30, 1.90, 2.90, row_uv(D_MOVERS))
        box(m, "wheel", s * 0.55, s * 1.22, 1.00, 1.04, -3.4, -1.8, DARK)  # fender lip
        box(m, "wheel", s * (hw - 0.08) - 0.03, s * (hw - 0.08) + 0.03, 0.45, 1.00, -1.4, -0.4, DARK)  # side guard posts
        box(m, "metal", s * hw - 0.04, s * hw, 0.55, 0.62, -1.75, -0.20, METAL)  # side guard rail
    roll_door(m, zr, hw, y0, y1)
    box(m, "metal", -hw + 0.05, hw - 0.05, 0.75, 0.82, zr - 0.15, zr, METAL)  # tail-lift platform (stowed)
    rear_lamps(m, zr - 0.15, hw, 0.80)
    box(m, "beacon", -0.40, 0.40, y1 - 0.06, y1 - 0.01, zr - 0.02, zr + 0.01, BEACON["amber"])
    centre(m); return m


def build_tanker():
    m = Mesh(); C = truck_cab(m, beacon=True); zf, zr = C["z0"] - 0.30, -5.20
    frame(m, zr, zf); hw = 1.20
    for zc in (-2.85, -4.20): axle(m, zc, C["r"], 1.20, fender=False)
    for s in (+1, -1): box(m, "wheel", s * 0.50, s * 1.24, 1.07, 1.10, -4.95, -2.10, DARK)  # tandem fender
    # tank: elliptical barrel approximated by a 16-sided tube squashed via two tubes (main + crown)
    yc, ry = 2.05, 0.92
    for zz0, zz1 in ((zr + 0.10, zf - 0.10),):
        a, b = np.array([0, yc, zz0]), np.array([0, yc, zz1])
        tube(m, "metal", a, b, ry, METAL, 16, caps=True)
    for k, z in enumerate(np.linspace(zr + 0.6, zf - 0.6, 4)):  # baffle bands
        tube(m, "wheel", np.array([0, yc, z - 0.04]), np.array([0, yc, z + 0.04]), ry + 0.02, DARK, 16, caps=False)
    for s in (+1, -1):
        gt.side_quad(m, "decal", s, ry + 0.012, zr + 0.8, zf - 0.8, yc - 0.20, yc + 0.20, row_uv(D_FUEL))
        gt.side_quad(m, "decal", s, ry + 0.014, zr + 0.25, zr + 0.65, yc - 0.20, yc + 0.20, hz(0))
        box(m, "wheel", s * 1.00, s * 1.22, 0.75, 1.15, -2.10, -0.20, DARK)  # hose cabinet
        box(m, "metal", s * 1.20, s * 1.23, 0.80, 1.10, -2.00, -0.30, METAL)
    gt.end_quad(m, "decal", -1, zr + 0.08, 0.30, yc - 0.10, yc + 0.30, hz(0))
    gt.end_quad(m, "decal", -1, zr + 0.07, 0.30, yc - 0.55, yc - 0.20, hz(1))
    # walkway + handrail + manlids on top
    yt = yc + ry
    box(m, "metal", -0.30, 0.30, yt - 0.02, yt + 0.03, zr + 0.4, zf - 0.4, METAL)
    for z in np.linspace(zr + 0.9, zf - 0.9, 4):
        tube(m, "metal", np.array([0, yt, z]), np.array([0, yt + 0.10, z]), 0.22, METAL, 10)
    for s in (+1, -1):
        box(m, "metal", s * 0.40 - 0.02, s * 0.40 + 0.02, yt + 0.40, yt + 0.44, zr + 0.4, zf - 0.4, METAL)
        for z in np.linspace(zr + 0.4, zf - 0.4, 6): box(m, "metal", s * 0.40 - 0.015, s * 0.40 + 0.015, yt, yt + 0.42, z - 0.015, z + 0.015, METAL)
    for k in range(5):  # rear ladder
        box(m, "metal", -0.20, 0.20, 1.2 + k * 0.30, 1.23 + k * 0.30, zr - 0.02, zr + 0.02, METAL)
    stripes(m, zr - 0.10, hw, 0.55, 0.85); box(m, "metal", -hw, hw, 0.50, 0.88, zr - 0.10, zr - 0.02, METAL)
    rear_lamps(m, zr - 0.10, hw, 0.62)
    centre(m); return m


def build_flatbed():
    m = Mesh(); C = truck_cab(m); zf, zr = C["z0"] - 0.10, -4.90; hw, yd = 1.25, 1.10
    frame(m, zr, zf)
    axle(m, -2.75, C["r"], 1.20, fender=False)
    box(m, "metal", -hw, hw, yd - 0.12, yd, zr, zf, METAL)  # deck
    m.quad("decal", [(-hw + 0.02, yd + 0.002, zr), (hw - 0.02, yd + 0.002, zr), (hw - 0.02, yd + 0.002, zf), (-hw + 0.02, yd + 0.002, zf)],
           [(0.38, 1.0), (0.49, 1.0), (0.49, 0.876), (0.38, 0.876)], np.array([0, 1, 0]))  # tread plate tile
    for s in (+1, -1):
        for z in np.linspace(zr + 0.3, zf - 0.3, 6):  # stake pockets
            box(m, "wheel", s * hw - 0.04, s * hw + 0.01 * s, yd - 0.22, yd - 0.02, z - 0.05, z + 0.05, DARK)
        box(m, "wheel", s * 0.55, s * 1.22, 1.00, 1.03, -3.5, -2.0, DARK)
    # headboard (ladder rack)
    box(m, "body", -hw, hw, yd, yd + 1.40, zf - 0.08, zf, FL)
    for x in np.linspace(-hw + 0.15, hw - 0.15, 6): box(m, "wheel", x - 0.02, x + 0.02, yd + 0.1, yd + 1.3, zf - 0.10, zf - 0.08, DARK)
    # load: lumber stack + two pallets with shrink-wrapped cartons
    BRN = (0.30, 0.20)  # wood-ish bit of the rear atlas is unreliable -> use beacon atlas? keep plain trim colour
    box(m, "trim", -1.05, 1.05, yd, yd + 0.70, zr + 2.20, zf - 0.25, FL)
    for k in range(5):
        y = yd + 0.12 + k * 0.13
        box(m, "wheel", -1.06, 1.06, y, y + 0.015, zr + 2.20, zf - 0.25, DARK)  # lumber courses
    for z0 in (zr + 0.20, zr + 1.25):
        box(m, "wheel", -0.55, 0.55, yd, yd + 0.14, z0, z0 + 0.90, DARK)  # pallet
        box(m, "box", -0.52, 0.52, yd + 0.14, yd + 1.10, z0 + 0.03, z0 + 0.87, FL)
    for z in (zr + 0.7, zr + 1.7, zr + 3.0, zf - 0.9):  # straps
        for s in (+1, -1): box(m, "wheel", s * 1.07 - 0.005, s * 1.07 + 0.005, yd, yd + 0.72 if z > zr + 2 else yd + 1.12, z - 0.03, z + 0.03, DARK)
        box(m, "wheel", -1.07, 1.07, (yd + 0.72 if z > zr + 2 else yd + 1.12), (yd + 0.73 if z > zr + 2 else yd + 1.13), z - 0.03, z + 0.03, DARK)
    stripes(m, zr, hw, yd - 0.12, yd)
    rear_lamps(m, zr, hw, 0.62)
    centre(m); return m


def build_mixer():
    m = Mesh(); C = truck_cab(m, beacon=True); zf, zr = C["z0"] - 0.10, -5.10
    frame(m, zr, zf)
    for zc in (-2.70, -4.05): axle(m, zc, C["r"], 1.20, fender=False)
    for s in (+1, -1): box(m, "wheel", s * 0.50, s * 1.24, 1.07, 1.10, -4.85, -1.95, DARK)
    # drum: front cone low, belly, rear cone rising to the opening
    fwd = np.array([0, 1.95, -0.10]); belly = np.array([0, 2.30, -2.20]); back = np.array([0, 2.85, -4.40])
    tube(m, "body", fwd, belly, 0.55, FL, 16, rb=1.12)
    tube(m, "body", belly, back, 1.12, FL, 16, rb=0.45)
    for k in range(5):  # spiral fins hint: dark bands
        f = 0.15 + k * 0.17; p = belly + (back - belly) * f if k > 1 else fwd + (belly - fwd) * (0.5 + f)
        rr = 1.12 - (0.67 * f if k > 1 else 0.57 * (0.5 - f) * 1.0) - 0.0
        tube(m, "wheel", p - np.array([0, 0.01, 0.04]), p + np.array([0, 0.01, 0.04]), max(rr, 0.6) + 0.02, DARK, 16, caps=False)
    for s in (+1, -1): gt.side_quad(m, "decal", s, 1.135, -3.0, -1.4, 2.05, 2.55, row_uv(D_HAZARD, 0.0, 0.0001) if False else row_uv(D_MOVERS, 0.22, 0.78))
    box(m, "wheel", -0.60, 0.60, 0.90, 2.15, -0.45, 0.10, DARK)  # front pedestal / gearbox
    box(m, "wheel", -0.70, 0.70, 0.90, 2.40, -4.75, -4.20, DARK)  # rear pedestal
    tube(m, "metal", np.array([0, 3.15, -4.55]), np.array([0, 2.75, -5.05]), 0.30, METAL, 10, rb=0.42)  # hopper
    tube(m, "metal", np.array([0, 1.75, -5.00]), np.array([0.45, 1.25, -5.70]), 0.18, METAL, 8, caps=False)  # chute
    box(m, "metal", 0.70, 1.05, 1.30, 2.20, -0.70, -0.20, METAL)  # water tank
    for k in range(6): box(m, "metal", -1.05, -0.95, 1.0 + k * 0.33, 1.03 + k * 0.33, -4.8, -4.75, METAL)  # ladder
    stripes(m, zr, 1.20, 0.55, 0.85); box(m, "metal", -1.20, 1.20, 0.50, 0.88, zr - 0.02, zr + 0.06, METAL)
    rear_lamps(m, zr, 1.20, 0.62)
    centre(m); return m


# ---------------------------------------------------------------- semi
def build_semi():
    m = Mesh(); C = sv.medium_duty_cab(m); hw = C["hw"]
    # tractor: lengthen with a sleeper box behind the day cab, tandem drive axles, fifth wheel, exhaust stacks
    zs0, zs1 = -1.25, C["z0"]
    frame(m, -3.10, zs1 + 0.1)
    box(m, "body", -hw, hw, C["yb"] + 0.45, C["roof"] + 0.55, zs0, zs1 + 0.02, FL)
    m.quad("body", [(-hw + 0.1, C["roof"], zs1 + 0.02), (hw - 0.1, C["roof"], zs1 + 0.02), (hw - 0.1, C["roof"] + 0.55, zs1 - 0.25), (-hw + 0.1, C["roof"] + 0.55, zs1 - 0.25)], [FL] * 4)
    m.quad("body", [(-hw + 0.1, C["roof"], zs1 + 0.02), (hw - 0.1, C["roof"], zs1 + 0.02), (hw - 0.1, C["roof"] + 0.55, zs1 + 0.80), (-hw + 0.1, C["roof"] + 0.55, zs1 + 0.80)], [FL] * 4, np.array([0, 0.6, 1]))  # roof fairing
    for s in (+1, -1):
        tube(m, "metal", np.array([s * (hw + 0.10), 1.0, zs1 - 0.15]), np.array([s * (hw + 0.10), C["roof"] + 0.90, zs1 - 0.15]), 0.07, METAL, 8)  # stack
        box(m, "metal", s * (hw - 0.45), s * (hw + 0.02), 0.50, 0.95, zs0 + 0.10, zs1 - 0.30, METAL)  # tanks / side skirt
    for zc in (-1.85, -3.15): axle(m, zc, C["r"], 1.25)
    box(m, "metal", -0.55, 0.55, 1.10, 1.18, -3.00, -1.80, METAL)  # fifth wheel
    # trailer: 13.6 m box from z=-0.6 back, kingpin over the fifth wheel
    hT, y0, y1 = 1.28, 1.25, 4.00; zf, zr = 0.40 - 2.0, 0.40 - 2.0 - 13.6
    module(m, hT, y0, y1, zr, zf)
    for s in (+1, -1):
        gt.side_quad(m, "decal", s, hT + 0.004, zr + 3.0, zf - 1.5, 2.00, 3.40, row_uv(D_FREIGHT))
        for k in range(12): gt.side_quad(m, "wheel", s, hT + 0.003, zr + 0.5 + k * 1.1, zr + 0.52 + k * 1.1, y0 + 0.1, y1 - 0.05, (*DARK, *DARK))  # logistic posts
        box(m, "wheel", s * 0.80 - 0.05, s * 0.80 + 0.05, 0.10, y0, zf - 2.6, zf - 2.5, DARK)  # landing legs
        box(m, "metal", s * 0.80 - 0.12, s * 0.80 + 0.12, 0.0, 0.05, zf - 2.65, zf - 2.45, METAL)
        box(m, "wheel", s * (hT - 0.02), s * hT, 0.55, y0, zr + 4.5, zf - 3.0, DARK)  # side skirt
    for zc in (zr + 1.40, zr + 2.70, zr + 4.00): axle(m, zc, 0.50, 1.25, fender=False)
    for s in (+1, -1): box(m, "wheel", s * 0.50, s * 1.28, 1.05, 1.10, zr + 0.70, zr + 4.70, DARK)
    zq = zr - 0.004  # barn doors with lock rods + stripes
    gt.end_quad(m, "wheel", -1, zq, 0.012, y0, y1 - 0.05, (*DARK, *DARK))
    for x in (-0.95, -0.35, 0.35, 0.95): box(m, "metal", x - 0.02, x + 0.02, y0 + 0.1, y1 - 0.1, zq - 0.04, zq, METAL)
    stripes(m, zr - 0.25, hT, 0.62, 0.78); box(m, "metal", -hT + 0.15, hT - 0.15, 0.60, 0.80, zr - 0.25, zr, METAL)  # ICC bar
    rear_lamps(m, zr - 0.25, hT, 0.82)
    centre(m); return m


# ---------------------------------------------------------------- food truck (step van)
def build_food():
    m = Mesh(); t = dict(W=2.40, H=3.15, yb=0.38, r=0.42); hw = t["W"] / 2
    sec = dict(z0=-3.40, z1=3.40, axles=[(2.25, 0), (-1.70, 1)], front="nose", rear="tail")
    gt.extrude(m, t, gt.profile(t, sec), hw)
    for s in (+1, -1):
        for zc, d in ((2.25, False), (-1.70, True)):
            gt.wheel(m, zc, t["r"], hw, s, tw=0.40 if d else 0.30, rec=0.10, seg=12); gt.arch_liner(m, zc, t["r"], hw, s)
    zf = 3.404
    gt.end_quad(m, "glass", 1, zf, hw - 0.12, 1.65, t["H"] - 0.30, tx.tile_uv(W["windshield"]))
    gt.end_quad(m, "front", 1, zf, hw - 0.05, t["yb"] + 0.06, 1.55, gt.FASCIA)
    box(m, "wheel", -hw - 0.02, hw + 0.02, 0.35, 0.58, 3.30, 3.48, DARK)
    for s in (+1, -1):
        gt.side_quad(m, "glass", s, hw + 0.005, 2.20, 3.05, 1.65, t["H"] - 0.35, tx.tile_uv(W["tinted"]))  # cab door window
        gt.side_quad(m, "wheel", s, hw + 0.004, 2.10, 2.12, 0.50, t["H"] - 0.20, (*DARK, *DARK))
        base = np.array([s * hw, t["H"] - 0.65, 3.20]); tip = base + np.array([s * 0.25, 0.0, 0.15])
        tube(m, "wheel", base, tip, 0.02, DARK, 4, caps=False)
        box(m, "wheel", tip[0] - 0.05, tip[0] + 0.05, tip[1] - 0.40, tip[1], tip[2] - 0.03, tip[2] + 0.02, DARK)
        gt.side_quad(m, "decal", s, hw + 0.006, -3.20, 1.95, 0.80, 1.55, row_uv(D_FOOD, 0.0, 0.48) if s < 0 else row_uv(D_FOOD))
    # serving hatch on the kerb side (-X): dark opening, counter, prop-up awning with stripes
    x = -hw - 0.006; za, zb, ya, yb_ = -2.30, 1.40, 1.60, 2.70
    gt.side_quad(m, "wheel", -1, hw + 0.007, za, zb, ya, yb_, (*DARK, *DARK))
    gt.side_quad(m, "glass", -1, hw + 0.008, za + 0.10, zb - 0.10, ya + 0.20, yb_ - 0.05, tx.tile_uv(W["clear"]))
    box(m, "metal", x - 0.30, x, ya - 0.04, ya + 0.02, za, zb, METAL)  # counter
    ya2 = yb_ + 0.05; xo = x - 0.85; yo = yb_ + 0.45
    m.quad("decal", [(x, ya2, za - 0.1), (x, ya2, zb + 0.1), (xo, yo, zb + 0.1), (xo, yo, za - 0.1)],
           [(0.0, 5 / 8 + 0.004), (1.0, 5 / 8 + 0.004), (1.0, 6 / 8 - 0.004), (0.0, 6 / 8 - 0.004)], np.array([-0.4, 1, 0]))
    m.quad("decal", [(x, ya2 - 0.01, za - 0.1), (xo, yo - 0.01, za - 0.1), (xo, yo - 0.01, zb + 0.1), (x, ya2 - 0.01, zb + 0.1)],
           [(0.0, 5 / 8 + 0.004), (0.0, 6 / 8 - 0.004), (1.0, 6 / 8 - 0.004), (1.0, 5 / 8 + 0.004)], np.array([0.4, -1, 0]))
    for z in (za, zb): tube(m, "metal", np.array([x, ya + 0.6, z]), np.array([xo + 0.05, yo - 0.02, z]), 0.015, METAL, 4, caps=False)
    gt.side_quad(m, "decal", -1, hw + 0.03, za + 0.6, zb - 0.6, yb_ + 0.10, yb_ + 0.40, row_uv(D_FOOD, 0.0, 0.48))
    # roof: extractor hood, A/C, gas bottles on the rear bumper, rear door
    box(m, "metal", -0.45, 0.10, t["H"], t["H"] + 0.35, -1.20, -0.60, METAL)
    box(m, "metal", -0.50, 0.50, t["H"], t["H"] + 0.22, 0.40, 1.30, METAL)
    for xx in (-0.45, 0.45): tube(m, "metal", np.array([xx, 0.62, -3.65]), np.array([xx, 1.25, -3.65]), 0.15, METAL, 10)
    box(m, "metal", -0.85, 0.85, 0.45, 0.62, -3.90, -3.38, METAL)
    gt.end_quad(m, "glass", -1, -3.404, 0.35, 1.70, 2.50, tx.tile_uv(W["tinted"]))
    gt.end_quad(m, "rear", -1, -3.405, hw - 0.04, 0.50, 0.75, gt.TAIL)
    return m


# ---------------------------------------------------------------- two-wheelers
def thin_wheel(m, zc, r, tw=0.10):
    gt.wheel(m, zc, r, tw / 2 + 0.03, +1, tw=tw / 2, rec=0.02, seg=12)
    gt.wheel(m, zc, r, tw / 2 + 0.03, -1, tw=tw / 2, rec=0.02, seg=12)


def build_scooter():
    m = Mesh(); r = 0.22; zf, zr = 0.68, -0.62
    thin_wheel(m, zf, r, 0.11); thin_wheel(m, zr, r, 0.13)
    box(m, "body", -0.17, 0.17, 0.28, 0.36, -0.30, 0.42, FL)  # floorboard
    m.quad("body", [(-0.20, 0.36, 0.42), (0.20, 0.36, 0.42), (0.17, 1.00, 0.60), (-0.17, 1.00, 0.60)], [FL] * 4, np.array([0, 0.2, 1]))  # leg shield
    m.quad("body", [(-0.20, 0.36, 0.40), (-0.17, 1.00, 0.58), (0.17, 1.00, 0.58), (0.20, 0.36, 0.40)], [FL] * 4, np.array([0, -0.2, -1]))
    box(m, "body", -0.20, 0.20, 0.32, 0.72, -0.85, -0.25, FL)  # rear body / engine cover
    box(m, "wheel", -0.17, 0.17, 0.72, 0.82, -0.80, -0.20, DARK)  # seat
    tube(m, "metal", np.array([0, 0.45, zf]), np.array([0, 1.00, 0.56]), 0.025, METAL, 6)  # fork / steerer
    box(m, "body", -0.13, 0.13, 0.95, 1.08, 0.50, 0.66, FL)  # headset with lamp
    box(m, "beacon", -0.06, 0.06, 0.98, 1.04, 0.66, 0.68, BEACON["white"])
    tube(m, "wheel", np.array([-0.36, 1.07, 0.55]), np.array([0.36, 1.07, 0.55]), 0.014, DARK, 6)  # bar
    box(m, "body", -0.06, 0.06, 0.40, 0.48, zf - 0.20, zf + 0.20, FL)  # front fender
    box(m, "rear", -0.08, 0.08, 0.60, 0.68, -0.88, -0.84, (0.07, 0.06))
    # top box
    hb, yb0, yb1, z0, z1 = 0.24, 0.86, 1.30, -0.98, -0.50
    box(m, "box", -hb, hb, yb0, yb1, z0, z1, FL)
    for s in (+1, -1): gt.side_quad(m, "decal", s, hb + 0.003, z0 + 0.02, z1 - 0.02, yb0 + 0.10, yb1 - 0.08, row_uv(D_COURIER, 0.1, 0.9))
    gt.end_quad(m, "decal", -1, z0 - 0.003, hb - 0.02, yb0 + 0.10, yb1 - 0.08, row_uv(D_COURIER, 0.1, 0.9))
    for s in (+1, -1): box(m, "metal", s * 0.17 - 0.012, s * 0.17 + 0.012, 0.55, 0.88, -0.85, -0.55, METAL)  # rack
    return m


def build_cargobike():
    """Long-john: small front wheel under a cargo box, rider frame behind."""
    m = Mesh(); zf, zr = 0.95, -0.85
    thin_wheel(m, zf, 0.25, 0.06); thin_wheel(m, zr, 0.34, 0.06)
    T = lambda a, b, rr=0.022: tube(m, "body", np.array(a), np.array(b), rr, FL, 6)
    T((0, 0.32, zf - 0.30), (0, 0.32, 0.10)); T((0, 0.32, 0.10), (0, 0.78, -0.25))   # boom + downtube
    T((0, 0.34, zr), (0, 0.38, -0.25)); T((0, 0.38, -0.25), (0, 0.95, -0.40))        # chainstay + seat tube
    T((0, 0.34, zr), (0, 0.92, -0.38)); T((0, 0.78, -0.25), (0, 1.02, 0.20))          # seatstay + top tube
    T((0, 1.02, 0.20), (0, 0.60, 0.30)); T((0, 0.25, zf), (0, 0.60, zf - 0.20), 0.016)  # head tube + fork
    tube(m, "metal", np.array([0, 0.62, 0.30]), np.array([0, 0.62, zf - 0.20]), 0.010, METAL, 4, caps=False)  # steering link
    tube(m, "wheel", np.array([-0.30, 1.12, 0.18]), np.array([0.30, 1.12, 0.18]), 0.014, DARK, 6)
    T((0, 1.02, 0.20), (0, 1.12, 0.18), 0.016)
    box(m, "wheel", -0.08, 0.08, 1.00, 1.05, -0.52, -0.30, DARK)  # saddle
    tube(m, "metal", np.array([0, 0.34, -0.20]), np.array([0, 0.34, -0.12]), 0.10, METAL, 10)  # crank / motor
    # cargo box over the boom
    hb, y0, y1, z0, z1 = 0.32, 0.40, 0.88, 0.12, zf - 0.32
    box(m, "box", -hb, hb, y0, y1, z0, z1, FL)
    for s in (+1, -1): gt.side_quad(m, "decal", s, hb + 0.003, z0 + 0.02, z1 - 0.02, y0 + 0.08, y1 - 0.06, row_uv(D_COURIER, 0.1, 0.9))
    gt.end_quad(m, "decal", 1, z1 + 0.003, hb - 0.02, y0 + 0.08, y1 - 0.06, row_uv(D_COURIER, 0.1, 0.9))
    box(m, "beacon", -0.05, 0.05, y1 - 0.12, y1 - 0.05, z1, z1 + 0.02, BEACON["white"])
    box(m, "rear", -0.05, 0.05, 0.70, 0.75, -0.95, -0.93, (0.07, 0.06))
    return m


# ---------------------------------------------------------------- main
TYPES = {
    "van":       (build_van, "car", ["white", "silver", "yellow"]),
    "box":       (build_box, "truck", ["white", "blue", "red"]),
    "semi":      (build_semi, "truck", ["red", "blue", "white", "black"]),
    "tanker":    (build_tanker, "truck", ["white", "green"]),
    "flatbed":   (build_flatbed, "truck", ["blue", "grey", "orange"]),
    "mixer":     (build_mixer, "truck", ["white", "orange", "yellow"]),
    "food":      (build_food, "truck", ["red", "teal", "white"]),
    "scooter":   (build_scooter, "car", ["red", "white", "teal"]),
    "cargobike": (build_cargobike, "car", ["black", "teal", "orange"]),
}
TRIM = {"flatbed": (0.62, 0.45, 0.26)}  # lumber load


def main():
    os.makedirs(OUT, exist_ok=True)
    extra = {}; maps_store(extra)
    car_maps = gc.load_maps(); car_maps.update(extra)
    truck_maps = gt.load_maps(); truck_maps.update(extra)
    for mp in (car_maps, truck_maps): mp["box"] = truck_maps["body"]; mp.setdefault("trim", (truck_maps["body"][0], None))
    car_maps["trim"] = (truck_maps["body"][0], None)
    gc.MAT_PBR.update({"decal": (0.1, 0.35), "beacon": (0.0, 0.15), "box": (0.3, 0.35)})
    for tn in (sys.argv[1:] or TYPES):
        fn, kind, cols = TYPES[tn]
        mesh = fn(); maps = car_maps if kind == "car" else truck_maps
        for c in cols:
            out = os.path.join(OUT, f"COMM_{tn}_{c}.glb")
            gc.write_glb(out, mesh, maps, COLOURS[c], "alloy" if kind == "car" else "dark", trim=TRIM.get(tn, (0.85, 0.86, 0.88)))
        print(f"{tn}: {mesh.count()} tris, {len(cols)} colours, {os.path.getsize(out) / 1e6:.2f} MB per GLB")


if __name__ == "__main__":
    main()
