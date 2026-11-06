"""Freight train -> GLB: six-axle diesel road-switcher + five cargo cars, on the gen_train toolkit (same rail wheels,
bogie frames, texture atlases and track conventions, so they run on gen_track's TRACK_*.glb).

Types: engine (22.0 m road switcher, Co-Co), boxcar, tank, hopper (covered), gondola (loaded with coal),
       flat (intermodal flat carrying a 40 ft container).
Run:  python gen_freight.py [type ...]
Output: assets/vehicles/train/glb/FREIGHT_<type>_<colour>.glb
Units: metres, +Y up, front faces +Z, y = 0 is the top of the rail, origin under the vehicle centre.
"""
import math, os, sys, zlib
import numpy as np
import gen_cars as gc
import gen_transit as gt
import gen_train as tr
import gen_train_tex as rt
from gen_cars import Mesh, box, tube, METAL, COLOURS

OUT = tr.OUT
FL, BD = rt.FLAT, rt.BDARK
GAUGE = tr.GAUGE
COUPLER_Y = 0.88

TYPES = {
    "engine":  dict(L=22.0, W=3.10, yb=1.55, r=0.52, wb=1.95, bogies=7.0, axles=3),
    "boxcar":  dict(L=17.4, W=3.20, yb=1.10, r=0.46, wb=1.78, bogies=6.1, H=4.60),
    "tank":    dict(L=18.0, W=3.10, yb=1.05, r=0.46, wb=1.78, bogies=6.4, R=1.45),
    "hopper":  dict(L=18.0, W=3.20, yb=1.00, r=0.46, wb=1.78, bogies=6.6, H=4.40),
    "gondola": dict(L=16.0, W=3.15, yb=1.05, r=0.46, wb=1.78, bogies=5.6, H=2.70),
    "flat":    dict(L=19.0, W=2.90, yb=1.05, r=0.46, wb=1.78, bogies=7.0),
}
# (body colour, trim colour) per type: trim tints stripes, the tank dome, the container
LIVERIES = {
    "engine":  [("orange", "black"), ("blue", "yellow"), ("black", "white"), ("green", "yellow")],
    "boxcar":  [("maroon", "white"), ("red", "white"), ("teal", "white"), ("beige", "black")],
    "tank":    [("black", "white"), ("white", "red"), ("silver", "black"), ("grey", "yellow")],
    "hopper":  [("grey", "black"), ("beige", "black"), ("white", "blue"), ("blue", "white")],
    "gondola": [("black", "white"), ("red", "white"), ("green", "white"), ("grey", "yellow")],
    "flat":    [("grey", "blue"), ("black", "red"), ("orange", "green"), ("yellow", "maroon")],
}


# ---------------------------------------------------------------- running gear
def truck(m, t, zc):
    """Bogie with 2 or 3 axles; frame face from the bogie atlas stretched over the wheelbase."""
    n, r, wb = t.get("axles", 2), t["r"], t["wb"]
    zs = [zc + wb * (k - (n - 1) / 2) for k in range(n)]
    for az in zs:
        for s in (+1, -1): tr.rail_wheel(m, az, r, s)
        tube(m, "bogie", (-GAUGE / 2, r, az), (GAUGE / 2, r, az), 0.08, BD, 6, caps=False)
    span = zs[-1] - zs[0]
    Lf = span / rt.FRAME_WB; xf = GAUGE / 2 + 0.20; y0, y1 = r - 0.18, r + 0.42
    box(m, "bogie", -xf + 0.01, xf - 0.01, y0, y1, zc - Lf / 2 + 0.05, zc + Lf / 2 - 0.05, BD)
    for s in (+1, -1): gt.side_quad(m, "bogie", s, xf, zc - Lf / 2, zc + Lf / 2, y0, y1, rt.FRAME)
    box(m, "bogie", -1.05, 1.05, y1, t["yb"] - 0.02, zc - 0.35, zc + 0.35, BD)  # bolster up to the underframe


def couplers(m, t):
    """AAR knuckle couplers on draft gear + end sill, cut lever."""
    L2, hw = t["L"] / 2, t["W"] / 2
    for d in (+1, -1):
        ze = d * L2
        box(m, "bogie", -0.13, 0.13, COUPLER_Y - 0.10, COUPLER_Y + 0.10, ze - d * 0.6, ze + d * 0.18, BD)   # shank
        box(m, "bogie", -0.24, 0.24, COUPLER_Y - 0.17, COUPLER_Y + 0.17, ze + d * 0.18, ze + d * 0.36, BD)  # knuckle head
        tube(m, "metal", (-hw + 0.25, COUPLER_Y + 0.25, ze + d * 0.05), (0.0, COUPLER_Y + 0.22, ze + d * 0.25), 0.02, METAL, 4, caps=False)


def underframe(m, t, z0=None, z1=None):
    L2, hw, yb = t["L"] / 2, t["W"] / 2, t["yb"]
    z0, z1 = (-L2, L2) if z0 is None else (z0, z1)
    box(m, "bogie", -0.30, 0.30, yb - 0.55, yb, z0 + 0.3, z1 - 0.3, BD)                       # centre sill
    for s in (+1, -1):
        box(m, "body", s * (hw - 0.12), s * hw, yb - 0.28, yb + 0.02, z0, z1, FL)            # side sill (body colour)
    for d in (+1, -1):
        box(m, "body", -hw, hw, yb - 0.32, yb + 0.02, d * L2 - d * 0.25, d * L2, FL)           # end sill
        for s in (+1, -1):                                                                     # end ladders / grab irons
            for y in np.arange(yb + 0.35, yb + 1.9, 0.42):
                tube(m, "metal", (s * (hw - 0.05), y, d * (L2 + 0.04)), (s * (hw - 0.45), y, d * (L2 + 0.04)), 0.016, METAL, 4, caps=False)
    box(m, "bogie", -0.45, 0.45, yb - 0.75, yb - 0.30, -1.0, 1.0, BD)                          # air reservoir / brake valve
    tube(m, "bogie", (0.6, yb - 0.45, -1.4), (0.6, yb - 0.45, 1.4), 0.22, BD, 8)


def ribs(m, mat, t, z0, z1, y0, y1, n, depth=0.06, uv=FL):
    hw = t["W"] / 2
    for k in range(n + 1):
        zc = z0 + (z1 - z0) * k / n
        for s in (+1, -1):
            box(m, mat, s * hw, s * (hw + depth), y0, y1, zc - 0.07, zc + 0.07, uv)


def stripe(m, t, z0, z1, y0, y1, out=0.004):
    for s in (+1, -1): gt.side_quad(m, "trim", s, t["W"] / 2 + out, z0, z1, y0, y1, (*FL, *FL))


# ---------------------------------------------------------------- cars
def boxcar(m, t, rng):
    L2, hw, yb, H = t["L"] / 2, t["W"] / 2, t["yb"], t["H"]
    box(m, "body", -hw, hw, yb, H - 0.15, -L2 + 0.1, L2 - 0.1, FL)
    box(m, "body", -hw + 0.08, hw - 0.08, H - 0.15, H, -L2 + 0.15, L2 - 0.15, FL)              # roof
    for k in range(14):                                                                         # roof seams
        zc = -L2 + 0.6 + k * (t["L"] - 1.2) / 13
        box(m, "body", -hw + 0.1, hw - 0.1, H, H + 0.04, zc - 0.05, zc + 0.05, FL)
    ribs(m, "body", t, -L2 + 0.25, -1.9, yb + 0.05, H - 0.2, 5)
    ribs(m, "body", t, 1.9, L2 - 0.25, yb + 0.05, H - 0.2, 5)
    for s in (+1, -1):                                                                          # plug door + tracks
        gt.side_quad(m, "door", s, hw + 0.07, -1.8, 1.8, yb + 0.05, H - 0.35, rt.CAB_DOOR)
        box(m, "metal", s * hw, s * (hw + 0.10), H - 0.32, H - 0.24, -3.9, 3.9, METAL)
        box(m, "metal", s * hw, s * (hw + 0.10), yb + 0.02, yb + 0.10, -3.9, 3.9, METAL)
        for zc in (-1.0, 1.0):
            tube(m, "metal", (s * (hw + 0.12), yb + 0.9, zc), (s * (hw + 0.12), yb + 2.6, zc), 0.025, METAL, 4)
    stripe(m, t, -L2 + 0.4, -2.2, H - 0.85, H - 0.55, out=0.07)
    stripe(m, t, 2.2, L2 - 0.4, H - 0.85, H - 0.55, out=0.07)
    for d in (+1, -1): gt.end_quad(m, "body", d, d * (L2 - 0.1) + d * 0.002, hw, yb, H - 0.15, (*FL, *FL))
    underframe(m, t)


def tank(m, t, rng):
    L2, hw, yb, R = t["L"] / 2, t["W"] / 2, t["yb"], t["R"]
    yc = yb + 0.25 + R
    tube(m, "body", (0, yc, -L2 + 1.0), (0, yc, L2 - 1.0), R, FL, 24, caps=False)
    for d in (+1, -1):                                                                          # dished heads
        z0 = d * (L2 - 1.0)
        for a0, a1 in ((0.0, 0.35), (0.35, 0.65)):
            tube(m, "body", (0, yc, z0 + d * a0), (0, yc, z0 + d * a1), R * math.cos(a0 * 1.6), FL, 24,
                 rb=R * math.cos(a1 * 1.6), caps=False)
        tube(m, "body", (0, yc, z0 + d * 0.65), (0, yc, z0 + d * 0.70), R * math.cos(0.65 * 1.6), FL, 24)
    for zc in (-L2 + 2.5, -4.0, 0.0, 4.0, L2 - 2.5):                                            # bands
        tube(m, "bogie", (0, yc, zc - 0.05), (0, yc, zc + 0.05), R + 0.02, BD, 24, caps=False)
    tube(m, "trim", (0, yc + R * 0.85, 0), (0, yc + R + 0.35, 0), 0.55, FL, 16)                 # dome + manway
    tube(m, "metal", (0, yc + R + 0.35, 0), (0, yc + R + 0.42, 0), 0.35, METAL, 12)
    box(m, "bogie", -0.9, 0.9, yc + R + 0.05, yc + R + 0.10, -1.2, 1.2, BD)                     # platform grating
    for s in (+1, -1):
        for zz in (-1.2, 1.2):
            tube(m, "metal", (s * 0.9, yc + R + 0.1, zz), (s * 0.9, yc + R + 1.05, zz), 0.02, METAL, 4)
        tube(m, "metal", (s * 0.9, yc + R + 1.05, -1.2), (s * 0.9, yc + R + 1.05, 1.2), 0.02, METAL, 4)
        gt.side_quad(m, "trim", s, R + 0.01, -L2 + 3.0, L2 - 3.0, yc - 0.12, yc + 0.12, (*FL, *FL))   # name stripe
    for d in (+1, -1):                                                                          # saddles + end platforms
        box(m, "bogie", -1.0, 1.0, yb, yc - R * 0.7, d * t["bogies"] - 0.5, d * t["bogies"] + 0.5, BD)
        box(m, "bogie", -hw, hw, yb - 0.04, yb + 0.04, d * L2 - d * 1.1, d * L2, BD)
    tube(m, "bogie", (0, yb + 0.15, -0.3), (0, yb - 0.25, -0.3), 0.18, BD, 10)                  # bottom outlet valve
    underframe(m, dict(t, W=2.4), -L2 + 0.0, L2)


def hopper(m, t, rng):
    """Covered hopper: straight-sided box upper, three pocket bays with sloped sheets down to the outlet gates."""
    L2, hw, yb, H = t["L"] / 2, t["W"] / 2, t["yb"], t["H"]
    ys = yb + 1.0                                                                               # bottom of the straight side
    box(m, "body", -hw, hw, ys, H - 0.25, -L2 + 0.8, L2 - 0.8, FL)
    tube(m, "body", (0, H - 0.40, -L2 + 0.8), (0, H - 0.40, L2 - 0.8), hw, FL, 16, caps=True)  # rounded roof
    box(m, "body", -hw, hw, ys, H - 0.40, -L2 + 0.8, L2 - 0.8, FL)
    for k in range(12):
        zc = -L2 + 1.0 + k * (t["L"] - 2.0) / 11
        tube(m, "body", (0, H - 0.40, zc - 0.05), (0, H - 0.40, zc + 0.05), hw + 0.03, FL, 16, caps=False)
    ribs(m, "body", t, -L2 + 0.8, L2 - 0.8, ys, H - 0.40, 11, depth=0.05)
    box(m, "trim", -0.35, 0.35, H + hw - 0.40, H + hw - 0.30, -L2 + 1.5, L2 - 1.5, FL)          # trough hatch strip
    bays = 3; bl = (t["L"] - 1.6) / bays
    for b in range(bays):
        z0 = -L2 + 0.8 + b * bl; z1 = z0 + bl; zm = (z0 + z1) / 2
        g = 0.45; yg = yb - 0.15
        P = lambda x, y, z: (x, y, z)
        for s in (+1, -1):                                                                      # sloped side sheets
            m.quad("body", [P(s * hw, ys, z0), P(s * hw, ys, z1), P(s * 0.5, yg, z1), P(s * 0.5, yg, z0)], [FL] * 4, np.array([s, -0.6, 0]))
            m.quad("body", [P(s * hw, ys, z0), P(s * 0.5, yg, z0), P(s * 0.5, yg, z1), P(s * hw, ys, z1)], [FL] * 4, np.array([-s, 0.6, 0]))
        for d, ze in ((-1, z0), (+1, z1)):                                                      # sloped end sheets
            m.quad("body", [P(-hw, ys, ze), P(hw, ys, ze), P(0.5, yg, zm + d * g), P(-0.5, yg, zm + d * g)], [FL] * 4, np.array([0, -0.6, d]))
            m.quad("body", [P(-hw, ys, ze), P(-0.5, yg, zm + d * g), P(0.5, yg, zm + d * g), P(hw, ys, ze)], [FL] * 4, np.array([0, 0.6, -d]))
        box(m, "bogie", -0.55, 0.55, yg - 0.25, yg, zm - g - 0.05, zm + g + 0.05, BD)           # outlet gate
        tube(m, "metal", (-0.8, yg - 0.12, zm), (0.8, yg - 0.12, zm), 0.04, METAL, 6)
    for d in (+1, -1):                                                                          # end platforms + slope sheet
        box(m, "bogie", -hw, hw, yb - 0.04, yb + 0.04, d * L2 - d * 0.8, d * L2, BD)
        gt.end_quad(m, "body", d, d * (L2 - 0.8) + d * 0.002, hw, ys, H - 0.40, (*FL, *FL))
    stripe(m, t, -L2 + 1.2, L2 - 1.2, H - 0.85, H - 0.65, out=0.055)
    box(m, "bogie", -0.30, 0.30, yb - 0.55, yb + 0.3, -L2 + 0.3, L2 - 0.3, BD)                  # stub centre sill
    for d in (+1, -1):
        for s in (+1, -1):
            for y in np.arange(yb + 0.35, yb + 1.9, 0.42):
                tube(m, "metal", (s * (hw - 0.05), y, d * (L2 - 0.4)), (s * (hw - 0.45), y, d * (L2 - 0.4)), 0.016, METAL, 4, caps=False)


def gondola(m, t, rng):
    L2, hw, yb, H = t["L"] / 2, t["W"] / 2, t["yb"], t["H"]
    w = 0.07
    for s in (+1, -1):                                                                          # side walls
        box(m, "body", s * (hw - w), s * hw, yb, H, -L2 + 0.1, L2 - 0.1, FL)
    for d in (+1, -1):
        box(m, "body", -hw, hw, yb, H, d * (L2 - 0.1), d * (L2 - 0.1 - w), FL)
    box(m, "bogie", -hw + w, hw - w, yb, yb + 0.05, -L2 + 0.2, L2 - 0.2, BD)                     # floor
    ribs(m, "body", t, -L2 + 0.3, L2 - 0.3, yb, H, 12, depth=0.08)
    for s in (+1, -1): box(m, "body", s * (hw - 0.02), s * (hw + 0.10), H - 0.14, H, -L2 + 0.1, L2 - 0.1, FL)  # top chord
    stripe(m, t, -L2 + 1.0, L2 - 1.0, yb + 0.35, yb + 0.55, out=0.085)
    # coal load: heaped grid of dark quads, crowned in the middle
    nx, nz = 6, 28; ytop = H - 0.25
    xs = np.linspace(-hw + w, hw - w, nx + 1); zs = np.linspace(-L2 + 0.2, L2 - 0.2, nz + 1)
    hgt = np.zeros((nz + 1, nx + 1))
    for i, z in enumerate(zs):
        for j, x in enumerate(xs):
            crown = 0.45 * (1 - (x / hw) ** 2) * (1 - (abs(z) / L2) ** 6)
            hgt[i, j] = ytop + crown + rng.uniform(-0.07, 0.07) * (0 < j < nx and 0 < i < nz)
    COAL = (0.99, 0.99)
    for i in range(nz):
        for j in range(nx):
            q = [(xs[j], hgt[i, j], zs[i]), (xs[j + 1], hgt[i, j + 1], zs[i]), (xs[j + 1], hgt[i + 1, j + 1], zs[i + 1]), (xs[j], hgt[i + 1, j], zs[i + 1])]
            m.quad("coal", q, [COAL] * 4, np.array([0, 1, 0]))
    underframe(m, t)


def flat(m, t, rng):
    L2, hw, yb = t["L"] / 2, t["W"] / 2, t["yb"]
    deck = yb + 0.12
    box(m, "body", -hw, hw, yb - 0.10, deck, -L2 + 0.1, L2 - 0.1, FL)
    for s in (+1, -1):                                                                          # fish-belly side sill
        m.quad("body", [(s * hw, yb - 0.10, -L2 + 1.0), (s * hw, yb - 0.10, L2 - 1.0), (s * hw, yb - 0.75, 3.0), (s * hw, yb - 0.75, -3.0)], [FL] * 4, np.array([s, 0, 0]))
        for zc in np.linspace(-L2 + 1.0, L2 - 1.0, 9):                                          # stake pockets
            box(m, "bogie", s * hw, s * (hw + 0.06), yb - 0.08, deck - 0.02, zc - 0.08, zc + 0.08, BD)
    # 40 ft container (12.19 x 2.44 x 2.59 m), corrugated walls in the trim colour
    cl, cw, ch = 12.19, 2.44, 2.59; c0 = deck + 0.02
    ct = dict(W=cw)
    box(m, "trim", -cw / 2, cw / 2, c0, c0 + ch, -cl / 2, cl / 2, FL)
    ribs(m, "trim", ct, -cl / 2 + 0.25, cl / 2 - 0.25, c0 + 0.12, c0 + ch - 0.12, 40, depth=0.03)
    for zc in (-cl / 2, cl / 2):                                                                 # corner posts / castings
        for s in (+1, -1):
            box(m, "bogie", s * (cw / 2 - 0.15), s * (cw / 2 + 0.01), c0, c0 + ch + 0.01, zc - 0.15 * np.sign(zc), zc + 0.01 * np.sign(zc), BD)
    for y in (c0, c0 + ch - 0.12):
        for s in (+1, -1):
            box(m, "bogie", s * (cw / 2 - 0.02), s * (cw / 2 + 0.02), y, y + 0.12, -cl / 2, cl / 2, BD)   # rails
    gt.end_quad(m, "door", 1, cl / 2 + 0.003, cw / 2 - 0.1, c0 + 0.1, c0 + ch - 0.1, rt.CAB_DOOR)          # cargo doors
    for x in (-0.6, -0.25, 0.25, 0.6):
        tube(m, "metal", (x, c0 + 0.2, cl / 2 + 0.03), (x, c0 + ch - 0.2, cl / 2 + 0.03), 0.02, METAL, 4)  # locking bars
    underframe(m, dict(t, yb=yb - 0.05))


# ---------------------------------------------------------------- engine
def engine(m, t, rng):
    """North American six-axle road switcher: walkway deck, long hood behind a full-width safety cab, short nose."""
    L2, hw, yb = t["L"] / 2, t["W"] / 2, t["yb"]
    deck = yb + 0.12
    box(m, "body", -hw, hw, yb - 0.35, deck, -L2 + 0.05, L2 - 0.05, FL)                          # deck / frame
    stripe(m, t, -L2 + 0.1, L2 - 0.1, yb - 0.30, yb - 0.05, out=0.003)
    # zones along z (front = +z): nose L2-0.9 .. L2-2.6, cab .. L2-5.4, long hood .. -L2+1.0
    zn1, zn0 = L2 - 0.9, L2 - 2.6; zc1, zc0 = zn0, L2 - 5.4; zh1, zh0 = zc0, -L2 + 1.0
    hh, cab_h, hwh = 4.25, 4.75, 1.32
    # short nose with a sloped top
    box(m, "body", -1.25, 1.25, deck, deck + 1.55, zn0, zn1, FL)
    m.quad("body", [(-1.25, deck + 1.55, zn1), (1.25, deck + 1.55, zn1), (1.25, deck + 2.15, zn0), (-1.25, deck + 2.15, zn0)], [FL] * 4, np.array([0, 1, 0.6]))
    for s in (+1, -1):
        m.tri("body", [(s * 1.25, deck + 1.55, zn1), (s * 1.25, deck + 1.55, zn0), (s * 1.25, deck + 2.15, zn0)], [FL] * 3, np.array([s, 0, 0]))
    gt.end_quad(m, "front", 1, zn1 + 0.003, 1.2, deck + 0.05, deck + 1.50, rt.NOSE)
    # cab
    box(m, "body", -hw + 0.02, hw - 0.02, deck, cab_h, zc0, zc1, FL)
    box(m, "body", -hw + 0.06, hw - 0.06, cab_h, cab_h + 0.10, zc0 - 0.08, zc1 + 0.08, FL)       # roof overhang
    for s in (+1, -1):                                                                           # windscreens
        x0, x1 = (0.15, hw - 0.25) if s > 0 else (-hw + 0.25, -0.15)
        m.quad("bogie", [(x0, cab_h - 1.05, zc1 + 0.004), (x1, cab_h - 1.05, zc1 + 0.004), (x1, cab_h - 0.20, zc1 + 0.004), (x0, cab_h - 0.20, zc1 + 0.004)], [BD] * 4, np.array([0, 0, 1]))
        m.quad("interior", [(x0 + 0.04, cab_h - 1.01, zc1 + 0.010), (x1 - 0.04, cab_h - 1.01, zc1 + 0.010), (x1 - 0.04, cab_h - 0.24, zc1 + 0.010), (x0 + 0.04, cab_h - 0.24, zc1 + 0.010)],
               [(0, 78), (0.999, 78), (0.999, 78.999), (0, 78.999)], np.array([0, 0, 1]))
        gt.pane(m, s, hw - 0.015, zc0 + 0.35, zc0 + 1.35, cab_h - 1.15, cab_h - 0.30, 1, 90, tr.O["tint_light"])
        gt.pane(m, s, hw - 0.015, zc0 + 1.55, zc1 - 0.25, cab_h - 1.15, cab_h - 0.30, 1, 91, tr.O["tint_light"])
        gt.side_quad(m, "door", s, hw - 0.012, zc0 + 0.30, zc0 + 1.40, deck + 0.10, cab_h - 1.25, rt.CAB_DOOR)
    # long hood with louvres, doors, radiator + dynamic brake + exhaust
    box(m, "body", -hwh, hwh, deck, hh, zh0, zh1, FL)
    for s in (+1, -1):
        nd = 7; step = (zh1 - zh0 - 4.0) / nd
        for k in range(nd):                                                                     # engine-room doors
            z0 = zh0 + 3.6 + k * step
            gt.side_quad(m, "grille" if k % 3 == 1 else "door", s, hwh + 0.006, z0 + 0.06, z0 + step - 0.06, deck + 0.35, hh - 0.30, rt.LOUVRE if k % 3 == 1 else rt.CAB_DOOR)
        gt.side_quad(m, "grille", s, hwh + 0.006, zh0 + 0.2, zh0 + 3.4, hh - 1.2, hh - 0.15, rt.LOUVRE)   # radiators
        stripe(m, dict(t, W=2 * hwh), zh0 + 0.2, zh1 - 0.2, deck + 0.15, deck + 0.30)
    for zc in (zh0 + 1.0, zh0 + 2.4):                                                            # radiator fans
        u0, v0, u1, v1 = rt.FAN
        m.quad("grille", [(-0.6, hh + 0.005, zc - 0.6), (0.6, hh + 0.005, zc - 0.6), (0.6, hh + 0.005, zc + 0.6), (-0.6, hh + 0.005, zc + 0.6)],
               [(u0, v0), (u1, v0), (u1, v1), (u0, v1)], np.array([0, 1, 0]))
    box(m, "body", -hwh, hwh, hh, hh + 0.30, zh1 - 3.2, zh1 - 1.4, FL)                           # dynamic brake blister
    for s in (+1, -1): gt.side_quad(m, "grille", s, hwh + 0.004, zh1 - 3.1, zh1 - 1.5, hh, hh + 0.28, rt.LOUVRE)
    box(m, "bogie", -0.25, 0.25, hh, hh + 0.30, -0.9, -0.1, BD)                                  # exhaust stack
    box(m, "metal", -0.35, 0.35, hh, hh + 0.12, zh1 - 0.8, zh1 - 0.3, METAL)                     # horn base
    for x in (-0.15, 0.0, 0.15):
        tube(m, "metal", (x, hh + 0.12, zh1 - 0.55), (x, hh + 0.20, zh1 - 0.55 + 0.25), 0.05, METAL, 6, rb=0.08)
    box(m, "body", -hwh, hwh, deck, hh - 0.2, zh0, zh0 + 0.02, FL)
    gt.end_quad(m, "front", -1, zh0 - 0.003, hwh - 0.1, deck + 0.3, deck + 1.4, rt.HEADPOD)      # rear headlight / number boards
    # walkway handrails along both sides and at the ends
    for s in (+1, -1):
        xr = s * (hw - 0.06)
        posts = np.linspace(-L2 + 0.3, zc0 - 0.1, 9).tolist() + [zn0 + 0.1, L2 - 0.3]
        for zz in posts: tube(m, "trim", (xr, deck, zz), (xr, deck + 1.05, zz), 0.022, FL, 4, caps=False)
        tube(m, "trim", (xr, deck + 1.05, -L2 + 0.3), (xr, deck + 1.05, zc0 - 0.1), 0.022, FL, 4, caps=False)
        tube(m, "trim", (xr, deck + 1.05, zn0 + 0.1), (xr, deck + 1.05, L2 - 0.3), 0.022, FL, 4, caps=False)
        for d in (+1, -1):                                                                       # steps
            for k in range(3):
                box(m, "bogie", s * (hw - 0.40), s * (hw - 0.02), yb - 0.35 - 0.33 * k - 0.03, yb - 0.35 - 0.33 * k, d * (L2 - 0.95), d * (L2 - 0.35), BD)
    for d in (+1, -1):
        tube(m, "trim", (-hw + 0.06, deck + 1.05, d * (L2 - 0.3)), (-0.5, deck + 1.05, d * (L2 - 0.3)), 0.022, FL, 4, caps=False)
        tube(m, "trim", (0.5, deck + 1.05, d * (L2 - 0.3)), (hw - 0.06, deck + 1.05, d * (L2 - 0.3)), 0.022, FL, 4, caps=False)
        box(m, "bogie", -1.3, 1.3, 0.15, yb - 0.35, d * L2 - d * 0.25, d * L2 + d * 0.05, BD)    # pilot
        gt.end_quad(m, "front", d, d * L2 + d * 0.055, 1.3, 0.15, yb - 0.36, rt.PLOUGH)
    for s in (+1, -1):                                                                           # fuel tank between trucks
        box(m, "bogie", -1.25, 1.25, 0.55, yb - 0.35, -4.4, 4.4, BD)
        gt.side_quad(m, "bogie", s, 1.251, -4.4, 4.4, 0.55, yb - 0.37, rt.UNDERBOX)
    t2 = dict(t, yb=yb - 0.35)
    for zc in (-t["bogies"], t["bogies"]): truck(m, t2, zc)
    couplers(m, t)


BUILD = {"engine": engine, "boxcar": boxcar, "tank": tank, "hopper": hopper, "gondola": gondola, "flat": flat}


def main():
    os.makedirs(OUT, exist_ok=True)
    maps = tr.load_maps()
    import io
    from PIL import Image
    rng0 = np.random.default_rng(7)                                                              # coal: speckled near-black
    a = (np.clip(rng0.normal(22, 10, (64, 64, 1)), 4, 60) * np.ones(3)).astype(np.uint8)
    buf = io.BytesIO(); Image.fromarray(a).save(buf, "JPEG", quality=90); maps["coal"] = (buf.getvalue(), None)
    gc.MAT_PBR["coal"] = (0.0, 0.55)
    for tn in (sys.argv[1:] or TYPES):
        t = TYPES[tn]
        for i, (bodyc, trim) in enumerate(LIVERIES[tn]):
            seed = zlib.crc32(f"fr{tn}{i}".encode()); rng = np.random.default_rng(seed)
            m = Mesh(); BUILD[tn](m, t, rng)
            if tn != "engine":
                for zc in (-t["bogies"], t["bogies"]): truck(m, t, zc)
                couplers(m, t)
            out = os.path.join(OUT, f"FREIGHT_{tn}_{bodyc}.glb")
            gc.write_glb(out, m, maps, COLOURS[bodyc], "alloy", trim=COLOURS[trim], seat=(0.2, 0.2, 0.22))
        print(f"{tn}: {t['L']:.1f} m, {m.count()} tris, {len(LIVERIES[tn])} liveries, {os.path.getsize(out) / 1e6:.2f} MB per GLB")


if __name__ == "__main__":
    main()
