#!/usr/bin/env python3
"""Waste-management service buildings, ultra low poly with the detail carried by SPOM volumes (powerlib kit, see
assets_src/power/powerlib.py). The set mirrors the four garbage services of a modern city builder:

    landfill.glb            90 x 70 m terraced landfill: capped + active cells, leachate pond, gas wells + flare, weighbridge, office
    incinerator.glb         waste-to-energy plant: tipping hall + ramp, bunker/boiler hall, flue-gas block, 70 m stack, silos
    recycling_center.glb    materials recovery facility: sorting hall, inclined conveyor gallery, bale yard, roll-off skips
    hazardous_waste.glb     industrial waste site: bunded tank farm, drum store with pallet racking, treatment shed, scrubber stack

Facades are SPOM relief (trapezoidal cladding ribs, roller doors with slats, louvres, downpipes, bollards), so each wall is a
12-triangle box. Bales, drums, skips, conveyor trusses, ladders with cages, gas well heads and the flare are all relief too.
Y up, metres, origin at ground centre. Run: python generate_waste.py
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "power"))
import powerlib as pl  # noqa: E402
from powerlib import Mesh, Relief  # noqa: E402

pl.HERE = HERE
pl.TEX = os.path.join(HERE, "tex")
os.makedirs(pl.TEX, exist_ok=True)
import generate_power as gp  # noqa: E402  (fence texture)

DARK = (0.16, 0.17, 0.18)
YELLOW = (0.92, 0.72, 0.10)
ORANGE = (0.88, 0.42, 0.10)
CONC = (0.64, 0.62, 0.58)


def flat(c):
    return np.asarray(c, np.float32)


def extra_textures():
    pl.plain_texture("soil", (0.42, 0.34, 0.24), 0.18, m_per=4.0, rough=0.95, seed=21)
    pl.plain_texture("grass_cap", (0.40, 0.48, 0.26), 0.20, m_per=5.0, rough=0.95, seed=22)

    def trash(col, size):  # mixed refuse: random coloured flecks over dark soil
        rng = np.random.default_rng(23)
        pal = np.array([(0.85, 0.85, 0.82), (0.2, 0.3, 0.6), (0.7, 0.2, 0.2), (0.25, 0.25, 0.25), (0.75, 0.65, 0.4),
                        (0.3, 0.55, 0.3)], np.float32)
        idx = rng.integers(0, len(pal), (size // 4, size // 4))
        m = rng.random((size // 4, size // 4)) < 0.55
        fl = np.repeat(np.repeat(pal[idx] * m[..., None], 4, 0), 4, 1)
        return np.where(fl.sum(-1, keepdims=True) > 0, fl, col)
    pl.plain_texture("refuse", (0.30, 0.27, 0.22), 0.15, m_per=3.0, rough=0.95, seed=24, extra=trash)
    pl.plain_texture("asphalt", (0.25, 0.25, 0.26), 0.06, m_per=3.0, rough=0.9, seed=25)
    pl.plain_texture("water", (0.18, 0.22, 0.20), 0.04, m_per=6.0, metal=0.2, rough=0.15, seed=26)
    pl.plain_texture("liner", (0.10, 0.10, 0.11), 0.03, m_per=2.0, rough=0.4, seed=27)
    pl.plain_texture("roof_metal", (0.55, 0.57, 0.58), 0.05, m_per=2.0, metal=0.4, rough=0.5, seed=28)
    pl.plain_texture("tank_white", (0.86, 0.86, 0.84), 0.03, m_per=3.0, metal=0.2, rough=0.5, seed=29)
    pl.plain_texture("stack_red", (0.66, 0.16, 0.13), 0.04, m_per=3.0, rough=0.6, seed=30)
    pl.plain_texture("skip_yellow", YELLOW, 0.05, m_per=2.0, metal=0.3, rough=0.6, seed=31)


# ==============================================================================================================================
# Facades
# ==============================================================================================================================
def tex_facade(name, W, H, col, doors=(), pdoors=(), louvres=(), windows=(), pipes=(), sign=None, ppm=40, rib=0.25):
    """Trapezoid-ribbed steel cladding over a concrete plinth.
    doors: (x_centre, width, height) roller doors; pdoors: x of personnel doors; louvres/windows: (x0, y0, x1, y1);
    pipes: x of downpipes; sign: (x0, y0, x1, y1, rgb)."""
    ppm = max(20, min(ppm, int((4.0e5 / (W * H)) ** 0.5)))
    r = Relief(W, H, ppm, plane=(0.1, 0.1))
    X, Y = r.X, r.Y
    c = np.asarray(col, np.float32)
    r.rect(0, 0.6, W, H, 0.0, 0.03, c * 0.92)                                     # sheet
    ph = (X % rib) / rib
    prof = np.clip(1 - np.abs(ph - 0.25) / 0.12, 0, 1)                           # trapezoid rib
    on = (prof > 0) & (Y > 0.6) & (Y < H - 0.35)
    r._add(on, np.zeros_like(X), 0.03 + 0.035 * np.clip(prof * 1.6, 0, 1), c, 0.7 + 0.3 * prof)
    r.rect(0, 0, W, 0.62, 0.0, 0.07, CONC, bevel=0.02)                            # plinth
    r.rect(0, H - 0.35, W, H, 0.0, 0.12, c * 0.6, bevel=0.03)                     # eaves flashing
    for x in pipes:                                                              # gutter outlet + downpipe
        r.bar((x, 0.25), (x, H - 0.3), 0.05, c * 0.55, z=0.14, square=False)
        r.rect(x - 0.1, H - 0.5, x + 0.1, H - 0.3, 0.0, 0.18, c * 0.55)
    for x0, y0, x1, y1 in windows:                                               # ribbon glazing
        r.rect(x0 - 0.06, y0 - 0.06, x1 + 0.06, y1 + 0.06, 0.0, 0.09, (0.82, 0.83, 0.82))
        r.rect(x0, y0, x1, y1, 0.0, 0.10, (0.20, 0.27, 0.32))
        for x in np.arange(x0, x1 + 1e-3, 1.2):
            r.bar((x, y0), (x, y1), 0.03, (0.82, 0.83, 0.82), z=0.10)
    for x0, y0, x1, y1 in louvres:
        r.rect(x0, y0, x1, y1, 0.0, 0.10, (0.35, 0.36, 0.37), bevel=0.03)
        for y in np.arange(y0 + 0.08, y1 - 0.04, 0.09):
            r.bar((x0 + 0.06, y), (x1 - 0.06, y - 0.03), 0.018, (0.28, 0.29, 0.30), z=0.11)
    for xc, w, h in doors:                                                       # roller shutter, guides, box, bollards
        x0, x1 = xc - w / 2, xc + w / 2
        r.rect(x0 - 0.2, 0, x1 + 0.2, h + 0.25, 0.0, 0.12, (0.30, 0.31, 0.32))
        r.rect(x0, 0, x1, h, 0.0, 0.08, (0.62, 0.64, 0.66))
        on = (X > x0) & (X < x1) & (Y < h) & ((Y % 0.1) < 0.012)
        r._add(on, np.zeros_like(X), np.full(X.shape, 0.065, np.float32), (0.40, 0.42, 0.44), np.ones_like(X))
        r.rect(x0 - 0.15, h, x1 + 0.15, h + 0.6, 0.0, 0.32, (0.48, 0.50, 0.52), bevel=0.05)  # coil box
        for x in (x0 - 0.45, x1 + 0.45):                                         # yellow/black bollards, exact cylinders
            band = lambda X_, Y_: np.where(((Y_ * 4).astype(int) % 2)[..., None] == 0, flat(YELLOW), flat(DARK))
            r.rev(x, 0.55, lambda Y_: np.where(Y_ < 1.1, 0.11, 0.0), band)
            r.rev(x, 0.55, lambda Y_: np.where((Y_ >= 1.1) & (Y_ < 1.16), 0.08, 0.0), flat(DARK))
        r.rect(x0 - 0.25, h + 0.75, x0 + 0.3, h + 1.05, 0.0, 0.14, (0.9, 0.55, 0.1))  # warning beacon
    for x in pdoors:
        r.rect(x - 0.55, 0, x + 0.55, 2.25, 0.0, 0.10, (0.30, 0.31, 0.32))
        r.rect(x - 0.48, 0, x + 0.48, 2.15, 0.0, 0.07, (0.32, 0.42, 0.48))
        r.bar((x + 0.3, 1.05), (x + 0.42, 1.05), 0.02, (0.8, 0.8, 0.8), z=0.11)  # handle
        r.rect(x - 0.7, 2.35, x + 0.7, 2.55, 0.0, 0.35, (0.4, 0.41, 0.42))       # canopy
        r.rev(x + 0.75, 0.17, lambda Y_: np.where((Y_ > 2.0) & (Y_ < 2.2), 0.08, 0.0), flat((0.95, 0.93, 0.8)))  # lamp
    if sign:
        x0, y0, x1, y1, sc = sign
        r.rect(x0, y0, x1, y1, 0.0, 0.10, sc, bevel=0.02)
        r.rect(x0 + 0.15, (y0 + y1) / 2 - 0.08, x1 - 0.15, (y0 + y1) / 2 + 0.08, 0.0, 0.12, (0.95, 0.95, 0.95))
    return r.save(name)


def clad_building(m, key, cx, cz, w, d, h, col, sides, roof="roof_metal"):
    """Plain core box + a SPOM facade per side. sides: dict of '+z','-z','+x','-x' -> tex_facade kwargs (or {})."""
    m.box("wall_block", (cx, h / 2, cz), (w - 0.02, h, d - 0.02), bottom=False)
    m.box(roof, (cx, h + 0.15, cz), (w + 0.4, 0.3, d + 0.4), bottom=False)
    for s, (N, W, off) in {"+z": ((0, 0, 1), w, d / 2), "-z": ((0, 0, -1), w, d / 2),
                           "+x": ((1, 0, 0), d, w / 2), "-x": ((-1, 0, 0), d, w / 2)}.items():
        kw = sides.get(s, {})
        name = f"{key}_{s[0] == '+' and 'p' or 'n'}{s[1]}"
        dep = tex_facade(name, W, h, col, **kw)
        Nv = np.array(N, float)
        m.spom_box(name, tuple(np.array([cx, h / 2, cz]) + Nv * off), N, W, h, dep + 0.02)


# ==============================================================================================================================
# Free-standing relief props
# ==============================================================================================================================
def tex_ladder(name, H):
    """Caged ladder: stiles, rungs, cage hoops (true semicircles in depth) and vertical cage straps."""
    r = Relief(0.9, H, 120)
    X, Y = r.X, r.Y
    for x in (0.25, 0.65):
        r.bar((x, 0), (x, H), 0.03, YELLOW, z=0.12)
        for y in np.arange(1.0, H, 2.5):
            r.bar((x, y), (x, y + 0.04), 0.03, DARK, z=(0.03, 0.12))  # stand-off brackets
    for y in np.arange(0.3, H, 0.3):
        r.bar((0.25, y), (0.65, y), 0.018, YELLOW, z=0.12, square=False)
    hz = 0.12 + np.sqrt(np.clip(0.36 ** 2 - (X - 0.45) ** 2, 0, None))
    for y in np.arange(2.4, H, 0.9):
        on = (np.abs(Y - y) < 0.025) & (np.abs(X - 0.45) < 0.36)
        r._add(on, hz - 0.02, hz + 0.02, YELLOW, np.ones_like(X))
    for x in (0.11, 0.45, 0.79):
        z = 0.12 + np.sqrt(max(0.36 ** 2 - (x - 0.45) ** 2, 0))
        r.bar((x, 2.4), (x, H), 0.015, YELLOW, z=z)
    return r.save(name)


def tex_bales(name, kind, W=6.0, rows=3):
    """A stack of 1.1 m bales with wire strapping. kind: cardboard | plastic | cans | paper."""
    H = rows * 1.1
    r = Relief(W, H, 90)
    rng = np.random.default_rng(hash(kind) & 0xffff)
    pal = {"cardboard": [(0.62, 0.48, 0.30), (0.55, 0.42, 0.27), (0.70, 0.58, 0.40)],
           "plastic": [(0.2, 0.4, 0.7), (0.85, 0.85, 0.82), (0.75, 0.2, 0.2), (0.2, 0.6, 0.3), (0.9, 0.75, 0.2)],
           "cans": [(0.75, 0.76, 0.78), (0.6, 0.62, 0.64), (0.7, 0.2, 0.2), (0.3, 0.3, 0.6)],
           "paper": [(0.88, 0.86, 0.80), (0.80, 0.80, 0.78), (0.6, 0.6, 0.65)]}[kind]
    pal = np.array(pal, np.float32)
    X, Y = r.X, r.Y
    for j in range(rows):
        x = rng.uniform(-0.3, 0.0)
        while x < W:
            bw = rng.uniform(1.05, 1.25)
            y0, y1 = j * 1.1 + 0.01, j * 1.1 + 1.08
            z0 = rng.uniform(0.0, 0.08)
            on = (X >= x + 0.02) & (X <= x + bw - 0.02) & (Y >= y0) & (Y <= y1)
            k = np.minimum.reduce([X - x, x + bw - X, Y - y0, y1 - Y]) / 0.12
            bulge = z0 + 1.0 + 0.06 * np.clip(k, 0, 1) - 0.05 * (1 - np.clip(k, 0, 1))
            n = rng.integers(0, len(pal), (16, 16))
            ix = np.clip(((X - x) / bw * 16).astype(int), 0, 15)
            iy = np.clip(((Y - y0) / 1.1 * 16).astype(int), 0, 15)
            fleck = pal[n[iy, ix]] * (0.85 + 0.3 * rng.random(X.shape)[..., None])
            col = np.where(on[..., None], fleck, 0)
            r.end = np.where(on, np.maximum(r.end, np.clip(bulge, 0, 1.18)), r.end)
            r.start = np.where(on, np.minimum(r.start, z0), r.start)
            shade = 0.6 + 0.4 * np.clip(k, 0, 1)
            r.rgb = np.where(on[..., None], col * shade[..., None], r.rgb)
            for t in (0.25, 0.5, 0.75):                                      # strapping wires
                sx = x + bw * t
                won = on & (np.abs(X - sx) < 0.015)
                r.rgb = np.where(won[..., None], np.float32(0.25), r.rgb)
                r.end = np.where(won, np.minimum(r.end + 0.01, 1.19), r.end)
            x += bw + rng.uniform(0.02, 0.1)
    return r.save(name)


def tex_drum_rack(name, W=6.0):
    """Two-level orange pallet racking loaded with 205 l drums (each drum an exact cylinder) on timber pallets."""
    H = 2.9
    r = Relief(W, H, 110)
    rng = np.random.default_rng(5)
    for x in np.arange(0.05, W, 2.7):                                         # uprights (front + rear frames)
        for z in (0.06, 1.1):
            r.rect(x, 0, x + 0.09, H, z - 0.04, z + 0.04, (0.20, 0.33, 0.60), bevel=0.02)
        for y in np.arange(0.2, H, 0.6):
            r.bar((x + 0.045, y), (x + 0.045, y + 0.5), 0.02, (0.20, 0.33, 0.60), z=(0.06, 1.1))
    drum_cols = [(0.15, 0.30, 0.65), (0.15, 0.30, 0.65), (0.85, 0.65, 0.1), (0.1, 0.1, 0.1), (0.55, 0.15, 0.12)]
    for lvl, y0 in enumerate((0.0, 1.45)):
        if lvl:
            r.rect(0, y0 - 0.12, W, y0, 0.0, 1.16, ORANGE, bevel=0.03)          # beam pair, seen as one deep beam
        for x0 in np.arange(0.2, W - 1.1, 1.35):
            r.rect(x0, y0, x0 + 1.2, y0 + 0.14, 0.05, 1.12, (0.62, 0.50, 0.33))  # pallet
            for k in range(2):
                cx = x0 + 0.3 + 0.6 * k
                c = drum_cols[rng.integers(0, len(drum_cols))]
                for zc in (0.33, 0.85):
                    yb = y0 + 0.14
                    def prof(Y_, yb=yb):
                        d = (Y_ - yb) / 0.88
                        hoop = (np.abs(d - 0.33) < 0.02) | (np.abs(d - 0.66) < 0.02)
                        return np.where((d > 0) & (d < 1), np.where(hoop, 0.297, 0.285), 0.0)
                    label = lambda X_, Y_, c=c, yb=yb, cx=cx: np.where(
                        ((np.abs(X_ - cx) < 0.13) & (Y_ > yb + 0.4) & (Y_ < yb + 0.62))[..., None],
                        np.array([0.95, 0.95, 0.92], np.float32), np.array(c, np.float32))
                    r.rev(cx, zc, prof, label)
    return r.save(name)


def tex_skip(name, col):
    """Side of a 30 m3 roll-off container: plate with box-section ribs, top rail, hook bar, rollers. Depth = half width."""
    W, H = 6.2, 2.0
    r = Relief(W, H, 90)
    c = np.asarray(col, np.float32)
    r.rect(0.1, 0.25, W - 0.1, H - 0.1, 0.0, 0.03, c * 0.9)
    for x in np.arange(0.35, W - 0.2, 0.75):
        r.rect(x - 0.05, 0.25, x + 0.05, H - 0.15, 0.0, 0.09, c, bevel=0.03)
    r.rect(0.05, H - 0.18, W - 0.05, H, 0.0, 0.12, c * 0.8, bevel=0.03)           # top rail
    r.rect(0.0, 0.1, W, 0.3, 0.0, 0.14, DARK, bevel=0.03)                         # long-member
    for x in (0.4, W - 0.4):
        r.bar((x, 0.12), (x, 0.13), 0.11, DARK, z=0.2, square=False)               # rollers
    r.bar((W - 0.02, 0.6), (W - 0.02, 1.6), 0.04, DARK, z=0.15, square=False)     # hook frame
    r.rect(1.0, 0.9, 2.2, 1.3, 0.0, 0.10, (0.95, 0.95, 0.95))                     # stencil plate
    return r.save(name)


def tex_conveyor(name, L, rise):
    """Inclined, half-clad conveyor gallery seen from the side: Warren truss chords + webs, belt on troughing idlers with
    mixed waste on it, walkway handrail. One box carries the whole gallery (1.1 m wide)."""
    H = rise + 2.2
    r = Relief(L, H, 70)
    X, Y = r.X, r.Y
    yb = lambda x: 0.1 + rise * x / L
    st = (0.30, 0.42, 0.30)
    for z in (0.08, 1.02):                                                       # chords, both trusses
        r.bar((0, yb(0)), (L, yb(L)), 0.07, st, z=z)
        r.bar((0, yb(0) + 1.8), (L, yb(L) + 1.8), 0.06, st, z=z)
    xs = np.arange(0, L + 1e-3, 2.0)
    for i, x in enumerate(xs):
        for z in (0.08, 1.02):
            r.bar((x, yb(x)), (x, yb(x) + 1.8), 0.045, st, z=z)
            if i + 1 < len(xs):
                x2 = xs[i + 1]
                a, b = ((x, yb(x)), (x2, yb(x2) + 1.8)) if i % 2 else ((x, yb(x) + 1.8), (x2, yb(x2)))
                r.bar(a, b, 0.035, st, z=z)
        r.bar((x, yb(x)), (x, yb(x) + 0.01), 0.04, st, z=(0.08, 1.02))        # cross members
    # half-height sheeting on the far truss + roof sheet
    on = (Y > yb(X) + 0.9) & (Y < yb(X) + 1.85)
    r._add(on, np.full(X.shape, 1.05, np.float32), np.full(X.shape, 1.08, np.float32), (0.85, 0.85, 0.82), np.ones_like(X))
    on = np.abs(Y - (yb(X) + 1.9)) < 0.04
    r._add(on, np.full(X.shape, 0.02, np.float32), np.full(X.shape, 1.12, np.float32), (0.75, 0.76, 0.74), np.ones_like(X))
    # belt + idlers + waste
    for x in np.arange(0.5, L, 1.2):
        r.bar((x, yb(x) + 0.35), (x, yb(x) + 0.36), 0.05, DARK, z=(0.3, 0.8), square=False)
    on = np.abs(Y - (yb(X) + 0.45)) < 0.04
    r._add(on, np.full(X.shape, 0.3, np.float32), np.full(X.shape, 0.8, np.float32), (0.12, 0.12, 0.12), np.ones_like(X))
    rng = np.random.default_rng(3)
    pal = np.array([(0.85, 0.85, 0.8), (0.2, 0.4, 0.7), (0.7, 0.2, 0.2), (0.6, 0.48, 0.3), (0.3, 0.6, 0.3)], np.float32)
    lump = rng.random((int(L * 8) + 1,))
    li = np.clip((X * 8).astype(int), 0, len(lump) - 1)
    hgt = 0.05 + 0.18 * lump[li]
    on = (Y > yb(X) + 0.49) & (Y < yb(X) + 0.49 + hgt)
    col_i = rng.integers(0, len(pal), len(lump))[li]
    cc = pal[col_i]
    on3 = on[..., None]
    r.rgb = np.where(on3 & (0.65 > r.end[..., None]), cc, r.rgb)
    r.start = np.where(on, np.minimum(r.start, 0.35), r.start)
    r.end = np.where(on, np.maximum(r.end, 0.75), r.end)
    for y_off in (0.6, 1.1):                                                     # walkway handrail (near side)
        r.bar((0, yb(0) + y_off), (L, yb(L) + y_off), 0.02, YELLOW, z=0.03, square=False)
    return r.save(name)


def tex_gas_well(name):
    def b(r):
        cx, zc = 0.3, 0.3
        r.rev(cx, zc, lambda Y: np.where(Y < 0.25, 0.28, 0.0), flat(CONC))                      # concrete collar
        r.rev(cx, zc, lambda Y: np.where((Y >= 0.25) & (Y < 1.3), 0.09, 0.0), flat((0.95, 0.95, 0.92)))  # HDPE riser
        r.rev(cx, zc, lambda Y: np.where((Y >= 1.3) & (Y < 1.42), 0.14, 0.0), flat((0.25, 0.25, 0.25)))  # wellhead
        r.rev(cx, zc, lambda Y: np.where((Y >= 1.42) & (Y < 1.55), 0.05, 0.0), flat(YELLOW))
        r.bar((cx - 0.18, 1.6), (cx + 0.18, 1.6), 0.02, (0.7, 0.1, 0.1), z=zc, square=False)          # valve wheel
        r.bar((cx, 1.2), (0.6, 1.0), 0.04, (0.95, 0.95, 0.92), z=zc, square=False)                    # header lateral
    r = Relief(0.6, 1.7, 220)
    b(r)
    return r.save(name)


def tex_flare(name):
    """Enclosed landfill-gas flare: skirted shroud on legs, flame arrestor skid, sample ports."""
    r = Relief(1.2, 8.0, 90)
    cx, zc = 0.6, 0.6
    r.rev(cx, zc, lambda Y: np.where((Y > 1.0) & (Y < 7.6), 0.55, 0.0),
          lambda X, Y: np.where(((Y > 6.8) & (Y < 7.6))[..., None], np.array([0.25, 0.22, 0.2], np.float32),
                                np.array([0.78, 0.78, 0.76], np.float32)))
    r.rev(cx, zc, lambda Y: np.where((Y >= 7.6) & (Y < 7.75), 0.58, 0.0), flat((0.3, 0.3, 0.3)))
    for y in (2.5, 4.0, 5.5):
        r.rev(cx, zc, lambda Y, y=y: np.where(np.abs(Y - y) < 0.04, 0.59, 0.0), flat((0.6, 0.6, 0.6)))
    for s in (-1, 1):
        r.bar((cx + s * 0.45, 0), (cx + s * 0.4, 1.05), 0.05, (0.4, 0.4, 0.4), z=zc)
    r.rect(0.05, 0, 1.15, 0.9, 0.2, 1.0, (0.35, 0.45, 0.35), bevel=0.04)                         # blower skid
    r.rect(0.5, 1.4, 0.7, 1.6, 0.0, 0.18, YELLOW)
    return r.save(name)


def tex_acc_legs(name, W, H):
    """Steel portal legs + X-bracing under a raised deck (silo skirt, ACC, hopper)."""
    r = Relief(W, H, 80, plane=(0.15, 0.15))
    for x in (0.15, W - 0.15):
        r.bar((x, 0), (x, H), 0.15, (0.45, 0.47, 0.48))
    r.bar((0.15, 0.3), (W - 0.15, H - 0.3), 0.06, (0.45, 0.47, 0.48))
    r.bar((0.15, H - 0.3), (W - 0.15, 0.3), 0.06, (0.45, 0.47, 0.48), z=0.28)
    r.bar((0, H - 0.1), (W, H - 0.1), 0.12, (0.45, 0.47, 0.48))
    return r.save(name)


# ==============================================================================================================================
# Shared site helpers
# ==============================================================================================================================
def quad_out(m, mat, a, b, c, d, out):
    n = np.cross(np.subtract(b, a), np.subtract(c, a))
    if np.dot(n, out) < 0:
        a, b, c, d = d, c, b, a
    m.quad(mat, a, b, c, d)


def frustum(m, top, side, cx, cz, w0, d0, w1, d1, y0, y1):
    """Sloped-sided mound: footprint w0 x d0 at y0, plateau w1 x d1 at y1."""
    B = [(cx - w0 / 2, y0, cz - d0 / 2), (cx + w0 / 2, y0, cz - d0 / 2), (cx + w0 / 2, y0, cz + d0 / 2), (cx - w0 / 2, y0, cz + d0 / 2)]
    T = [(cx - w1 / 2, y1, cz - d1 / 2), (cx + w1 / 2, y1, cz - d1 / 2), (cx + w1 / 2, y1, cz + d1 / 2), (cx - w1 / 2, y1, cz + d1 / 2)]
    for i in range(4):
        j = (i + 1) % 4
        mid = np.mean([B[i], B[j]], 0) - np.array([cx, 0, cz])
        quad_out(m, side, B[i], B[j], T[j], T[i], (mid[0], 0.5, mid[2]))
    quad_out(m, top, *T, (0, 1, 0))


def fence_ring(m, x0, z0, x1, z1, depth, gap=None):
    """Chain-link + barbed fence along a rectangle, 3 m SPOM panels facing out. gap=(side, centre, width) leaves an opening."""
    for (a, b, fixed, N, along_x) in (((x0, x1, z1, (0, 0, 1), True)), ((x0, x1, z0, (0, 0, -1), True)),
                                      ((z0, z1, x1, (1, 0, 0), False)), ((z0, z1, x0, (-1, 0, 0), False))):
        t = a
        while t < b - 0.01:
            c = t + 1.5
            if gap and gap[0] == N and abs(c - gap[1]) < gap[2] / 2:
                t += 3.0
                continue
            p = (c, 1.375, fixed - 0.08 * N[2]) if along_x else (fixed - 0.08 * N[0], 1.375, c)
            m.spom_box("fence", p, N, 3.0, 2.75, depth)
            t += 3.0


def weighbridge(m, x, z, along_x=True):
    w, d = (18.0, 3.4) if along_x else (3.4, 18.0)
    m.box("concrete", (x, 0.1, z), (w + 0.6, 0.2, d + 0.6), bottom=False)
    m.box("galv_steel", (x, 0.25, z), (w, 0.1, d), bottom=False)
    hx, hz = (x, z - d / 2 - 2.2) if along_x else (x - w / 2 - 2.2, z)
    m.box("wall_block", (hx, 1.5, hz), (3.0, 3.0, 2.4), bottom=False)                     # gatehouse
    m.box("roof_felt", (hx, 3.1, hz), (3.4, 0.2, 2.8), bottom=False)
    m.box("dark_metal", (hx, 1.7, hz + (1.21 if along_x else 0)), (2.0, 0.9, 0.04) if along_x else (0.04, 0.9, 1.6), bottom=False)
    m.box("dark_metal", (x - (w / 2 + 0.5 if along_x else 0), 1.4, z - (0 if along_x else d / 2 + 0.5)), (0.3, 1.2, 0.3))  # ticket post


def site_office(m, key, x, z):
    clad_building(m, key, x, z, 12.0, 7.0, 3.4, (0.80, 0.80, 0.76),
                  {"+z": dict(pdoors=(2.0,), windows=((4.0, 1.1, 11.0, 2.4),)),
                   "-z": dict(windows=((1.0, 1.1, 11.0, 2.4),)), "+x": dict(windows=((1.5, 1.1, 5.5, 2.4),)),
                   "-x": dict(louvres=((2.5, 1.0, 4.0, 2.2),))}, roof="roof_felt")


def tank(m, ladder_key, ldep, x, z, r, h, mat="tank_white", ladder=True):
    m.box("concrete", (x, 0.15, z), (r * 2 + 0.6, 0.3, r * 2 + 0.6))
    m.cyl(mat, (x, 0.3, z), (x, 0.3 + h, z), r, seg=16, caps=True)
    m.cyl(mat, (x, 0.3 + h, z), (x, 0.3 + h + r * 0.15, z), r * 0.5, seg=12, caps=True)       # roof cone stand-in
    m.cyl("galv_steel", (x, 0.3 + h + 1.0, z), (x, 0.3 + h + 1.05, z), r + 0.05, seg=16)     # top handrail ring
    if ladder:
        m.spom_box(ladder_key, (x + r * 0.98, 0.3 + h / 2, z - 0.45), (1, 0, 0), 0.9, h, ldep)


# ==============================================================================================================================
# Buildings
# ==============================================================================================================================
def build_landfill(D):
    m = Mesh("landfill")
    SX, SZ = 45.0, 35.0
    m.box("grass_cap", (0, 0.02, 0), (SX * 2, 0.04, SZ * 2), bottom=False)
    # capped (closed) terraced cell: three benches, grassed
    frustum(m, "grass_cap", "grass_cap", -14, -6, 44, 40, 36, 32, 0.0, 4.0)
    frustum(m, "grass_cap", "soil", -14, -6, 34, 30, 26, 22, 4.0, 8.0)
    frustum(m, "grass_cap", "grass_cap", -14, -6, 24, 20, 14, 10, 8.0, 11.0)
    # active cell: refuse face + liner lip + daily-cover soil strip
    frustum(m, "refuse", "refuse", 22, -4, 30, 34, 22, 26, 0.0, 6.0)
    frustum(m, "soil", "soil", 22, -15, 22, 6, 18, 4, 6.0, 6.3)
    m.box("liner", (22, 0.25, -4 + 17.4), (31, 0.5, 0.6), bottom=False)
    m.box("liner", (22 + 15.3, 0.25, -4), (0.6, 0.5, 34.8), bottom=False)
    # haul road ramp up onto the active face
    quad_out(m, "asphalt", (18.0, 0.06, 20.0), (26.0, 0.06, 20.0), (26.0, 6.0, 9.0), (18.0, 6.0, 9.0), (0, 1, 0.5))
    m.box("asphalt", (5, 0.06, 27), (80, 0.04, 7), bottom=False)                              # perimeter road
    # leachate lagoon + treatment kiosk
    m.box("concrete", (34, 0.25, 26), (14, 0.5, 9), bottom=False)
    m.box("water", (34, 0.42, 26), (13, 0.1, 8), bottom=False)
    m.box("dark_metal", (40, 1.1, 20), (3.0, 2.2, 2.4))
    # landfill gas: wells on the capped cell, header to the flare compound
    wells = [(-28, -18, 4.0), (-28, 6, 4.0), (0, -18, 4.0), (0, 6, 4.0), (-20, -12, 8.0), (-8, -12, 8.0), (-20, 0, 8.0),
             (-8, 0, 8.0), (-14, -6, 11.0)]
    for x, z, y in wells:
        m.spom_box("gas_well", (x, y + 0.85, z - 0.3), (0, 0, 1), 0.6, 1.7, D["gas_well"] + 0.02)
    m.cyl("tank_white", (-36, 0.4, 12), (-36, 0.4, 24), 0.12, seg=6)
    m.box("concrete", (-36, 0.1, 26), (6, 0.2, 5), bottom=False)
    m.spom_box("flare", (-37.5, 4.0, 25.4), (0, 0, 1), 1.2, 8.0, D["flare"] + 0.02)
    m.spom_box("flare", (-34.5, 4.0, 25.4), (0, 0, 1), 1.2, 8.0, D["flare"] + 0.02)
    m.box("dark_metal", (-36, 1.2, 28), (3.0, 2.4, 1.6))                                      # gas engine container
    weighbridge(m, -8, 30)
    site_office(m, "lf_office", -30, 30)
    fence_ring(m, -SX, -SZ, SX, SZ, D["fence"] + 0.04, gap=((0, 0, 1), -8.0, 12.0))
    m.extras = {"kind": "landfill", "footprint": [SX * 2, SZ * 2], "capacity_note": "terraced cells; active face at +x",
                "entrance": [-8.0, 0.0, SZ]}
    return m


def build_incinerator(D):
    m = Mesh("incinerator")
    SX, SZ = 50.0, 32.0
    m.box("asphalt", (0, 0.02, 0), (SX * 2, 0.04, SZ * 2), bottom=False)
    blue = (0.30, 0.42, 0.55)
    grey = (0.78, 0.79, 0.78)
    # tipping hall: low, 6 roller doors on the ramp deck side
    clad_building(m, "inc_tip", -26, 0, 18, 34, 12, grey,
                  {"-x": dict(doors=[(4.5 + 5 * i, 4.2, 6.0) for i in range(6)], pipes=(1.0, 33.0)),
                   "+z": dict(louvres=((3, 7, 7, 9),), pdoors=(14.0,)), "-z": dict(louvres=((11, 7, 15, 9),))})
    # waste bunker + boiler hall: tall block with ribbon glazing and a branding sign band
    clad_building(m, "inc_boil", -2, 0, 30, 30, 34, blue,
                  {"+z": dict(windows=((2, 24, 28, 28),), sign=(3, 30, 15, 32.5, (0.85, 0.30, 0.12)), pipes=(0.6, 29.4),
                              pdoors=(26.0,)),
                   "-z": dict(windows=((2, 24, 28, 28),), louvres=((4, 2, 10, 5),), pipes=(0.6, 29.4)),
                   "+x": dict(louvres=((3, 10, 9, 14), (21, 10, 27, 14)), doors=[(15, 5, 6)])})
    # flue-gas treatment block, stepping down
    clad_building(m, "inc_fgt", 22, 4, 18, 22, 22, grey,
                  {"+z": dict(windows=((2, 16, 16, 19),), doors=[(9, 4.5, 5)]), "-z": dict(louvres=((2, 3, 6, 6),)),
                   "+x": dict(windows=((2, 16, 20, 19),), pipes=(11.0,))})
    # duct bridge boiler -> FGT, FGT -> stack
    m.box("tank_grey", (14.0, 26.0, 4.0), (4.0, 3.0, 3.0))
    m.box("tank_grey", (35.5, 8.0, 4.0), (9.0, 2.6, 2.6))
    # stack: 70 m, red/white aviation bands, two galleries, caged ladder
    sx, sz, sr = 41.0, 4.0, 1.7
    m.box("concrete", (sx, 0.4, sz), (5, 0.8, 5))
    bands = [(0, 58, "concrete"), (58, 62, "stack_red"), (62, 66, "tank_white"), (66, 70, "stack_red")]
    for y0, y1, mat in bands:
        m.cyl(mat, (sx, 0.8 + y0, sz), (sx, 0.8 + y1, sz), sr * (1 - 0.15 * y0 / 70), seg=14, caps=(y1 == 70))
    for gy in (30.0, 62.0):
        rr = sr * (1 - 0.15 * gy / 70)
        m.cyl("galv_steel", (sx, gy, sz), (sx, gy + 0.15, sz), rr + 1.0, seg=14, caps=True)
        m.cyl("galv_steel", (sx, gy + 1.1, sz), (sx, gy + 1.16, sz), rr + 1.0, seg=14)
    for y0 in (0.0, 32.0):
        h = 30.0 if y0 == 0 else 30.0
        rr = sr * (1 - 0.15 * (y0 + h / 2) / 70)
        m.spom_box("ladder30", (sx - 0.45, 0.8 + y0 + h / 2, sz + rr * 0.98), (0, 0, 1), 0.9, h, D["ladder30"] + 0.02)
    # ramp up to the tipping deck
    m.box("concrete", (-42, 0.9, 0), (10, 1.8, 30), bottom=False)
    quad_out(m, "asphalt", (-37, 1.81, -15), (-47, 1.81, -15), (-47, 1.81, 15), (-37, 1.81, 15), (0, 1, 0))
    quad_out(m, "concrete", (-47, 0.0, -28), (-37, 0.0, -28), (-37, 1.8, -15), (-47, 1.8, -15), (0, 1, 0.1))
    # lime + ash silos on braced legs, with caged ladders
    for i, (x, z) in enumerate(((20, -14), (26, -14))):
        m.cyl("tank_white", (x, 6, z), (x, 18, z), 2.2, seg=14, caps=True)
        m.cyl("tank_white", (x, 4, z), (x, 6, z), 1.0, seg=10)
        for N in ((1, 0, 0), (-1, 0, 0), (0, 0, 1), (0, 0, -1)):
            Nv = np.array(N, float)
            m.spom_box("silo_legs", tuple(np.array([x, 3.0, z]) + Nv * 1.85), N, 3.4, 6.0, D["silo_legs"] + 0.02)
        m.spom_box("ladder12", (x - 0.45, 12.0, z + 2.15), (0, 0, 1), 0.9, 12.0, D["ladder12"] + 0.02)
    # bottom-ash bay + transformers
    m.box("concrete", (-2, 1.5, -22), (24, 3.0, 0.4))
    m.box("refuse", (-2, 0.7, -25), (22, 1.4, 5.5), bottom=False)
    for x in (32.0, 38.0):
        m.box("tank_grey", (x, 1.5, -22), (3.0, 3.0, 2.4))
        m.spom_box("radiator", (x, 1.6, -20.8), (0, 0, 1), 3.0, 2.3, D["radiator"] + 0.02)
    fence_ring(m, -SX, -SZ, SX, SZ, D["fence"] + 0.04, gap=((0, 0, -1), -42.0, 12.0))
    weighbridge(m, -42, -24, along_x=False)
    m.extras = {"kind": "incinerator", "footprint": [SX * 2, SZ * 2], "stack_top": [sx, 70.8, sz],
                "entrance": [-42.0, 0.0, -SZ]}
    return m


def build_recycling(D):
    m = Mesh("recycling_center")
    SX, SZ = 40.0, 30.0
    m.box("asphalt", (0, 0.02, 0), (SX * 2, 0.04, SZ * 2), bottom=False)
    green = (0.24, 0.48, 0.32)
    clad_building(m, "mrf_hall", -8, -6, 40, 24, 12, green,
                  {"+z": dict(doors=[(5, 5, 6), (12, 5, 6), (19, 5, 6)], pdoors=(30.0,),
                              sign=(26, 8.5, 38, 10.5, (0.95, 0.95, 0.95)), pipes=(0.6, 39.4)),
                   "-z": dict(windows=((2, 8, 38, 10),), pipes=(0.6, 39.4)),
                   "-x": dict(doors=[(12, 6, 7)], louvres=((2, 8, 6, 10),)),
                   "+x": dict(windows=((2, 8, 22, 10),), doors=[(6, 4, 5)])})
    # baling annex
    clad_building(m, "mrf_bale", 20, -12, 16, 12, 8, (0.80, 0.80, 0.76),
                  {"+z": dict(doors=[(8, 5, 5)]), "+x": dict(louvres=((3, 5, 9, 7),)), "-z": dict(pipes=(8.0,))})
    # feed conveyor: infeed hopper on the yard -> top of the hall gable
    L, rise = 22.0, 9.0
    cz = 9.0
    m.spom_box("conveyor", (-14 + L / 2, (rise + 2.2) / 2, cz - 0.55), (0, 0, 1), L, rise + 2.2, D["conveyor"] + 0.02)
    for t in (0.3, 0.6, 0.9):
        x = -14 + L * t
        y = 0.1 + rise * t
        m.cyl("galv_steel", (x, 0, cz - 0.1), (x, y, cz - 0.1), 0.12, seg=4)
        m.cyl("galv_steel", (x, 0, cz + 0.1), (x, y, cz + 0.1), 0.12, seg=4)
    m.box("dark_metal", (-15.5, 1.6, cz), (4.0, 2.4, 4.0))                                    # infeed hopper
    m.box("concrete", (-15.5, 0.2, cz + 5), (10, 0.4, 6), bottom=False)                        # tipping apron
    m.box("refuse", (-15.5, 0.8, cz + 5), (7, 1.2, 4.5), bottom=False)                         # waiting pile
    # bale yard: 4 stacks, each a core box with relief on all four faces
    kinds = ("cardboard", "plastic", "cans", "paper")
    for i, k in enumerate(kinds):
        bx, bz = 4 + i * 8.0, 14.0
        m.box("concrete", (bx, 0.05, bz), (7.2, 0.1, 3.6), bottom=False)
        m.box("dark_metal", (bx, 1.65, bz), (5.6, 3.2, 1.0), bottom=False)
        for N, W in (((0, 0, 1), 6.0), ((0, 0, -1), 6.0)):
            m.spom_box(f"bale_{k}", (bx, 1.65, bz + N[2] * 0.5), N, W, 3.3, D[f"bale_{k}"] + 0.02)
        for N in ((1, 0, 0), (-1, 0, 0)):
            m.spom_box(f"bale_{k}_s", (bx + N[0] * 2.8, 1.65, bz), N, 2.2, 3.3, D[f"bale_{k}_s"] + 0.02)
    # roll-off skips along the -x fence
    for i, key in enumerate(("skip_y", "skip_b", "skip_y", "skip_g")):
        x, z = -34.0, -20 + i * 3.4
        m.box("skip_yellow" if key == "skip_y" else "dark_metal", (x, 1.1, z), (6.0, 1.8, 2.3))
        m.spom_box(key, (x, 1.1, z + 1.15), (0, 0, 1), 6.2, 2.0, D[key] + 0.02)
        m.spom_box(key, (x, 1.1, z - 1.15), (0, 0, -1), 6.2, 2.0, D[key] + 0.02)
    weighbridge(m, 22, 24)
    site_office(m, "mrf_office", -24, 22)
    fence_ring(m, -SX, -SZ, SX, SZ, D["fence"] + 0.04, gap=((0, 0, 1), 22.0, 12.0))
    m.extras = {"kind": "recycling_center", "footprint": [SX * 2, SZ * 2], "entrance": [22.0, 0.0, SZ]}
    return m


def build_hazardous(D):
    m = Mesh("hazardous_waste")
    SX, SZ = 32.0, 24.0
    m.box("concrete", (0, 0.02, 0), (SX * 2, 0.04, SZ * 2), bottom=False)
    # bunded tank farm: 1.2 m bund walls, 6 vertical tanks, pipe rack
    bx, bz = 14.0, -8.0
    for (cx, cz, w, d) in ((bx, bz - 7.0, 24, 0.3), (bx, bz + 7.0, 24, 0.3), (bx - 12, bz, 0.3, 14), (bx + 12, bz, 0.3, 14)):
        m.box("concrete", (cx, 0.6, cz), (w, 1.2, d))
    m.box("liner", (bx, 0.06, bz), (23.6, 0.04, 13.6), bottom=False)
    for i in range(3):
        for j in range(2):
            tank(m, "ladder8", D["ladder8"] + 0.02, bx - 7.5 + i * 7.5, bz - 3.0 + j * 6.0, 2.4, 8.0,
                 mat="tank_white" if (i + j) % 2 == 0 else "tank_grey")
    for y in (4.5, 5.5):
        m.cyl("galv_steel", (bx - 11, y, bz), (bx + 11, y, bz), 0.12, seg=6)
    m.box("galv_steel", (bx + 11.5, 3.0, bz - 6.5), (2.0, 0.15, 1.0))                         # bund stair landing
    # drum store: open-sided steel shed, racks under a mono-pitch roof
    dx, dz = -16.0, -10.0
    m.box("roof_metal", (dx, 6.2, dz), (24.0, 0.25, 10.0), bottom=False)
    for x in np.arange(dx - 11.5, dx + 12, 4.6):
        for z in (dz - 4.8, dz + 4.8):
            m.box("galv_steel", (x, 3.1, z), (0.25, 6.2, 0.25))
    m.box("concrete", (dx, 0.1, dz), (24.4, 0.2, 10.4), bottom=False)
    for x in (dx - 9.0, dx - 3.0, dx + 3.0, dx + 9.0):
        for z, N in ((dz + 2.5, (0, 0, 1)), (dz - 2.5, (0, 0, -1))):
            m.spom_box("drum_rack", (x, 1.45, z - N[2] * 1.16), N, 6.0, 2.9, D["drum_rack"] + 0.02)
    # treatment / lab building + scrubber stack
    clad_building(m, "hz_treat", -14, 12, 22, 12, 9, (0.86, 0.84, 0.70),
                  {"+z": dict(doors=[(6, 4.5, 5)], pdoors=(14.0, 19.0), windows=((11, 3, 21, 4.5),),
                              sign=(1, 6.5, 9, 7.8, (0.85, 0.65, 0.1))),
                   "-z": dict(louvres=((3, 5, 7, 7), (15, 5, 19, 7)), pipes=(11.0,)),
                   "+x": dict(windows=((2, 3, 10, 4.5),)), "-x": dict(doors=[(6, 4, 5)])})
    m.cyl("galv_steel", (-1.5, 0, 16.5), (-1.5, 22, 16.5), 0.7, seg=10, caps=True)
    m.spom_box("ladder20", (-1.95, 11, 16.5 + 0.68), (0, 0, 1), 0.9, 20.0, D["ladder20"] + 0.02)
    m.box("tank_grey", (2.0, 2.5, 16.5), (3.0, 5.0, 3.0))                                     # scrubber vessel
    m.cyl("tank_grey", (2.0, 4.2, 16.5), (-1.0, 4.2, 16.5), 0.35, seg=8)
    # loading bay with skips
    for i, key in enumerate(("skip_y", "skip_b")):
        x, z = 12.0 + i * 7.5, 14.0
        m.box("skip_yellow" if key == "skip_y" else "dark_metal", (x, 1.1, z), (6.0, 1.8, 2.3))
        for N in ((0, 0, 1), (0, 0, -1)):
            m.spom_box(key, (x, 1.1, z + N[2] * 1.15), N, 6.2, 2.0, D[key] + 0.02)
    fence_ring(m, -SX, -SZ, SX, SZ, D["fence"] + 0.04, gap=((1, 0, 0), 14.0, 12.0))
    m.extras = {"kind": "hazardous_waste", "footprint": [SX * 2, SZ * 2], "entrance": [SX, 0.0, 14.0], "unique": True}
    return m


# ==============================================================================================================================
if __name__ == "__main__":
    pl.make_plain_textures()
    extra_textures()
    D = {"fence": gp.tex_fence(), "radiator": gp.tex_radiator()}
    for n, h in (("ladder8", 8.0), ("ladder12", 12.0), ("ladder20", 20.0), ("ladder30", 30.0)):
        D[n] = tex_ladder(n, h)
    for k in ("cardboard", "plastic", "cans", "paper"):
        D[f"bale_{k}"] = tex_bales(f"bale_{k}", k)
        D[f"bale_{k}_s"] = tex_bales(f"bale_{k}_s", k, W=2.2)
    D["drum_rack"] = tex_drum_rack("drum_rack")
    D["skip_y"] = tex_skip("skip_y", YELLOW)
    D["skip_b"] = tex_skip("skip_b", (0.18, 0.32, 0.62))
    D["skip_g"] = tex_skip("skip_g", (0.22, 0.45, 0.28))
    D["conveyor"] = tex_conveyor("conveyor", 22.0, 9.0)
    D["gas_well"] = tex_gas_well("gas_well")
    D["flare"] = tex_flare("flare")
    D["silo_legs"] = tex_acc_legs("silo_legs", 3.4, 6.0)
    for build in (build_landfill, build_incinerator, build_recycling, build_hazardous):
        mesh = build(D)
        size = mesh.write_glb(os.path.join(HERE, f"{mesh.name}.glb"))
        print(f"{mesh.name}.glb  {mesh.triangles()} tris  {len(mesh.spom)} SPOM sets  {size / 1024:.0f} KB")
