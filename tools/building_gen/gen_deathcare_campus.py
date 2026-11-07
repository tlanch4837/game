"""The crematorium and the cemetery of crematorium_graveyard.html, cut into proper lots (8x12 and 13x12 cells) and rebuilt for the game.
The mock-up was 41,018 triangles; here everything that was a heap of small meshes is relief instead (SPOM sheets, 2 tris a face):
  fences + piers + railings        -> a few wall-plane relief sheets, front faces only (the mock-up had thousands of instanced pickets)
  headstone rows                   -> one relief sheet per row, drawn front and back
  columbarium niches, colonnades   -> relief sheets on the wall / a row of columns in one quad
  facades (buttresses, arches, portals, rose window, cornice, steps) -> relief on the wall
  chimney                          -> a tapered 4-quad stack, brick and stone bands painted in the texture
  paths, plots, car park, kerbs, plaques -> one ground texture per lot.
Left out on purpose (rules): trees, benches, lamp posts, bins, cars, flagpole.
Frame: lot centred on the origin, front (street) at +z, ground y = 0. Crematorium plinth top y = 1.
Run: python gen_deathcare.py   (it builds these two as well)
"""
import os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_low_density as g
from gen_low_density import Mesh, CELL
import gen_bus_depot as b
import gen_harbor as hb
import gen_deathcare as dc
import spom

PAL = {"stone": (0.66, 0.65, 0.62), "stone2": (0.82, 0.8, 0.75), "stone3": (0.48, 0.49, 0.5), "plinth": (0.36, 0.36, 0.35), "iron": (0.09, 0.09, 0.11),
       "col": (0.88, 0.86, 0.8), "gold": (0.72, 0.58, 0.28), "dark": (0.2, 0.21, 0.23), "glass": (0.18, 0.26, 0.34), "wood": (0.3, 0.2, 0.13)}


# ---------------------------------------------------------------- relief helpers
def sheet(kind, W, H, axis="z"):
    sh = hb.Sheet(kind, False, 0.0, 0.0, W, H, axis=axis)
    sh.col.update(PAL)
    return sh


def ring(sh, cx, cy, r0, r1, d0, d1, tag):
    L = sh.L
    X, Y = np.meshgrid(L.xs, L.ys)
    m = (np.hypot(X - cx, Y - cy) <= r1) & (np.hypot(X - cx, Y - cy) >= r0)
    L.start[m] = d0; L.end[m] = d1; L.col[m] = sh.col[tag]


def emit_front(sh, m, y0):
    """Only the front face of the relief volume (2 tris): for thin things seen mostly face-on: fences, headstone rows, column rows."""
    m.prims.setdefault("spom_" + sh.kind, [])
    o, n = sh._frame(y0)
    N = np.array(n, np.float32); up = np.array([0, 1, 0], np.float32); T = np.cross(up, N).astype(np.float32); O = np.array(o, np.float32)
    D = spom.DEPTH
    c = lambda x, y: O + T * x + up * y + N * D
    m.raw_quad("spom_" + sh.kind, [c(0, 0), c(sh.W, 0), c(sh.W, sh.H), c(0, sh.H)], N, [(0, 0), (sh.W, 0), (sh.W, sh.H), (0, sh.H)], [(sh.W, sh.H, D)] * 4)


class Ctx:
    def __init__(self):
        self.m = Mesh(); self.tex = {}; self.prev = []
        for k in ("brick", "stone", "chim"): self.m.prims[k] = []

    def put(self, sh, y0, xw, zo, mirror=False, front_only=False):
        """Place a painted sheet: wall plane `xw` (z for axis z, x for axis x), origin `zo` along the wall, facing +/- by `mirror`."""
        snap = sh.place(xw, zo, mirror)
        (emit_front if front_only else (lambda s, m, y: s.emit(m, y)))(sh, self.m, y0)
        self.prev.append((sh, y0, snap))
        if "spom_" + sh.kind not in self.tex:
            self.tex["spom_" + sh.kind] = sh.L.encode()
            self.tex["sprev_" + sh.kind] = (sh.preview(), hb.flat_normal())


def absorb(c, sub, ry, tx, tz):
    """Add a building context built at the origin (front +z) into c, rotated ry about y and moved to (tx, tz): mesh, textures, preview sheets."""
    merge(c.m, sub.m, ry, tx, tz)
    for k, v in sub.tex.items(): c.tex.setdefault(k, v)
    for (sh, y0, snap) in sub.prev: c.prev.append((sh, y0, snap, (ry, tx, tz)))


def merge(dst, sub, ry, tx, tz):
    """Add a sub-mesh built at the origin (front +z) into dst, rotated ry about y and moved to (tx, tz)."""
    c, s = np.cos(ry), np.sin(ry)
    rot = lambda p: np.array([p[0] * c + p[2] * s, p[1], -p[0] * s + p[2] * c], np.float32)
    for name, items in sub.prims.items():
        dst.prims.setdefault(name, [])
        for it in items:
            pts = [rot(p) + np.array([tx, 0, tz], np.float32) for p in it[0]]
            n = rot(it[1])
            dst.prims[name].append((pts, n, it[2], it[3]) + tuple(it[4:]))


# ---------------------------------------------------------------- textures
def brick_tex(col):
    """Brick 3 m tile: 40 courses of 0.075 m, 0.22 m bricks in running bond, light mortar."""
    n = 512
    r = np.random.default_rng(3)
    a = np.zeros((n, n, 3), np.float32); h = np.full((n, n), 0.2, np.float32)
    ch = n // 40
    for ry in range(40):
        y0 = ry * ch; off = (ry % 2) * 19
        for x in range(-off, n, 38):
            xa, xb = max(x + 2, 0), min(x + 36, n)
            if xb <= xa: continue
            a[y0 + 1:y0 + ch - 1, xa:xb] = np.array(col, np.float32) * (0.82 + 0.3 * r.random())
            h[y0 + 1:y0 + ch - 1, xa:xb] = 0.7
    mort = h < 0.5
    a[mort] = (170, 166, 156)
    return b.maps(a, h, 3.0)


def slate_tex():
    n = 256
    r = np.random.default_rng(4)
    a = np.zeros((n, n, 3), np.float32); h = np.full((n, n), 0.3, np.float32)
    for ry in range(16):
        y0 = ry * 16; off = (ry % 2) * 8
        for x in range(-off, n, 16):
            v = 50 + 22 * r.random()
            a[y0:y0 + 14, max(x, 0):min(x + 15, n)] = (v, v + 4, v + 10); h[y0:y0 + 14, max(x, 0):min(x + 15, n)] = 0.8
    return b.maps(a, h, 3.0)


def chimney_tex():
    """3.2 m wide x 3.6 m tall tile: brick with a pale stone band at the foot of every tier."""
    n = 512
    r = np.random.default_rng(5)
    a = np.zeros((n, n, 3), np.float32); h = np.full((n, n), 0.2, np.float32)
    ch = n // 48
    for ry in range(44):
        y0 = ry * ch; off = (ry % 2) * 19
        for x in range(-off, n, 38):
            xa, xb = max(x + 2, 0), min(x + 36, n)
            if xb > xa:
                a[y0 + 1:y0 + ch - 1, xa:xb] = np.array((150, 84, 64), np.float32) * (0.82 + 0.3 * r.random()); h[y0 + 1:y0 + ch - 1, xa:xb] = 0.7
    a[a.sum(2) == 0] = (170, 166, 156)
    a[44 * ch:] = (196, 190, 176); h[44 * ch:] = 0.9                                                           # stone band
    return b.maps(a, h, 3.0)


def ground(W, D, px, lawn, rects, paint_after=None):
    """rects: (kind, x0, x1, z0, z1[, extra]) in lot metres; kinds: path, plinth, asphalt, bed, line, kerb, ringpaved."""
    w, d = int(W * px), int(D * px)
    im = Image.new("RGB", (w, d), lawn)
    r = np.random.default_rng(6)
    arr0 = np.asarray(im, np.float32) + r.normal(0, 4, (d, w, 1))
    im = Image.fromarray(np.clip(arr0, 0, 255).astype(np.uint8)); dr = ImageDraw.Draw(im)
    X = lambda x: (x + W / 2) * px; Z = lambda z: (z + D / 2) * px
    for it in rects:
        kind, x0, x1, z0, z1 = it[:5]
        box = [X(x0), Z(z0), X(x1), Z(z1)]
        if kind == "path": dr.rectangle(box, fill=(184, 174, 152))
        elif kind == "plinth":
            dr.rectangle(box, fill=(184, 180, 170))
            for x in np.arange(x0, x1, 4.0): dr.line([X(x), Z(z0), X(x), Z(z1)], fill=(156, 152, 144), width=1)
            for z in np.arange(z0, z1, 4.0): dr.line([X(x0), Z(z), X(x1), Z(z)], fill=(156, 152, 144), width=1)
        elif kind == "asphalt": dr.rectangle(box, fill=(66, 68, 72))
        elif kind == "bed": dr.rectangle(box, fill=it[5], outline=(150, 146, 134), width=1)
        elif kind == "line": dr.rectangle(box, fill=(232, 228, 208))
        elif kind == "kerb": dr.rectangle(box, fill=(176, 172, 164))
    dr.rectangle([0, 0, w - 1, d - 1], outline=(128, 126, 118), width=4)
    if paint_after: paint_after(dr, X, Z)
    arr = np.asarray(im.filter(ImageFilter.GaussianBlur(0.6)), np.float32)
    return b.maps(arr, np.asarray(im.convert("L"), np.float32) / 255, 2.0)


# ---------------------------------------------------------------- building helpers
def gable_z(m, x0, x1, y, z0, z1, rise, over=0.4, wall="stone"):
    """Gable roof with the ridge along z (slopes face +/-x)."""
    xm = (x0 + x1) / 2
    e0, e1, f0, f1 = x0 - over, x1 + over, z0 - over, z1 + over
    m.quad("roof", (e1, y - 0.15, f1), (e1, y - 0.15, f0), (xm, y + rise, f0), (xm, y + rise, f1), [(f1 / 3, 0), (f0 / 3, 0), (f0 / 3, 1.5), (f1 / 3, 1.5)])
    m.quad("roof", (e0, y - 0.15, f0), (e0, y - 0.15, f1), (xm, y + rise, f1), (xm, y + rise, f0), [(f0 / 3, 0), (f1 / 3, 0), (f1 / 3, 1.5), (f0 / 3, 1.5)])
    for ze, a, c in ((z1, (x0, y, z1), (x1, y, z1)), (z0, (x1, y, z0), (x0, y, z0))):
        m.tri(wall, a, c, (xm, y + rise - 0.15, ze), [(a[0] / 3, -y / 3), (c[0] / 3, -y / 3), (xm / 3, -(y + rise) / 3)])


def top_quad(m, mat, x0, x1, y, z0, z1, W, D):
    """A quad on the lot's ground texture at height y (a raised plinth top shows the same painted pavers)."""
    m.quad(mat, (x0, y, z1), (x1, y, z1), (x1, y, z0), (x0, y, z0),
           [((x0 + W / 2) / W, (z1 + D / 2) / D), ((x1 + W / 2) / W, (z1 + D / 2) / D), ((x1 + W / 2) / W, (z0 + D / 2) / D), ((x0 + W / 2) / W, (z0 + D / 2) / D)])


# ---------------------------------------------------------------- crematorium lot (8 x 12 cells = 64 x 96 m)
def crematorium_lot():
    cells = (8, 12)
    W, D = cells[0] * CELL, cells[1] * CELL
    c = Ctx(); m = c.m
    Y = 1.0                                                     # plinth top
    # plinth (the base texture is also mapped on its top so the painted pavers show), edge skirts in stone
    m.quad("base", (-W / 2, 0.0, D / 2), (W / 2, 0.0, D / 2), (W / 2, 0.0, -D / 2), (-W / 2, 0.0, -D / 2), [(0, 1), (1, 1), (1, 0), (0, 0)])
    px0, px1, pz0, pz1 = -30.0, 25.0, -32.0, 9.0
    top_quad(m, "base", px0, px1, Y, pz0, pz1, W, D)
    b.zface(m, px0, px1, 0.0, Y, pz1, 1, "stone", b.wuv(px0, px1, 0.0, Y)); b.zface(m, px0, px1, 0.0, Y, pz0, -1, "stone", b.wuv(px1, px0, 0.0, Y))
    b.xface(m, pz0, pz1, 0.0, Y, px1, 1, "stone", b.wuv(pz1, pz0, 0.0, Y)); b.xface(m, pz0, pz1, 0.0, Y, px0, -1, "stone", b.wuv(pz0, pz1, 0.0, Y))
    # ---- main hall
    hx0, hx1, hz0, hz1, hh = -15.0, 19.0, -20.0, 0.0, 9.0
    b.mbox(m, hx0, hx1, Y, Y + hh, hz0, hz1, wall="brick", skip=("+z",), top=None)
    b.zface(m, hx0, hx1, Y, Y + hh, hz1, 1, "brick", b.wuv(hx0, hx1, Y, Y + hh))
    b.mbox(m, hx0 - 0.5, hx1 + 0.5, Y + hh, Y + hh + 0.6, hz0 - 0.5, hz1 + 0.5, wall="stone", top=None)             # cornice slab
    dc.gable(m, hx0 - 1.5, hx1 + 1.5, Y + hh + 0.6, -21.5, 1.5, 6.0, over=0.0, wall="stone")                        # slate roof
    wins = [2.0 + 4.6 * i for i in (-3, -2, 2, 3)]
    for xc in wins: b.zface(m, xc - 1.0, xc + 1.0, 2.2, 8.2, hz1 + 0.03, 1, "detail", b.auv("win"))
    b.windows_x(m, hz0 + 2.0, hz1 - 2.0, 2.4, 7.4, hx1, 1, 4.6, "wide")
    fr = sheet("cr_hall", hx1 - hx0, hh + 0.8)
    for xc in [2.0 + 4.6 * i + 2.3 for i in range(-4, 3)] + [hx0 + 0.5, hx1 - 0.5]:                    # buttresses between windows and at the corners
        xr = xc - hx0
        fr.add(xr - 0.5, xr + 0.5, 0.0, hh - 0.6, 0.0, 0.62, "stone")
    for xc in wins:
        xr = xc - hx0
        fr.add(xr - 1.3, xr + 1.3, 1.2, 1.45, 0.0, 0.55, "stone2")                                                     # sill
        fr.add(xr - 1.15, xr - 1.0, 1.45, 7.2, 0.0, 0.3, "stone2"); fr.add(xr + 1.0, xr + 1.15, 1.45, 7.2, 0.0, 0.3, "stone2")
        for (w_, y0, y1) in ((1.3, 7.2, 7.45), (1.0, 7.45, 7.8), (0.6, 7.8, 8.1)): fr.add(xr - w_, xr + w_, y0, y1, 0.0, 0.5, "stone2")   # arched head in steps
    fr.add(0.0, hx1 - hx0, hh - 0.5, hh, 0.0, 0.55, "stone2")
    c.put(fr, Y, hz1, hx0, False)
    # ---- entrance tower: gable, arched door, rose window, cross
    tx0, tx1, tz0, tz1, th = -3.0, 7.0, -2.0, 6.0, 13.0
    b.mbox(m, tx0, tx1, Y, Y + th, tz0, tz1, wall="brick", skip=("+z",), top=None)
    b.zface(m, tx0, tx1, Y, Y + th, tz1, 1, "brick", b.wuv(tx0, tx1, Y, Y + th))
    b.mbox(m, tx0 - 0.4, tx1 + 0.4, Y + th, Y + th + 0.6, tz0 - 0.4, tz1 + 0.4, wall="stone", top=None)
    gable_z(m, tx0 - 0.5, tx1 + 0.5, Y + th + 0.6, tz0 - 0.5, tz1 + 0.5, 4.5, over=0.0)
    b.zface(m, 2.0 - 1.7, 2.0 + 1.7, Y, Y + 4.4, tz1 + 0.04, 1, "detail", b.auv("door"))
    b.zface(m, 2.0 - 1.7, 2.0 + 1.7, Y + 7.0, Y + 11.0, tz1 + 0.04, 1, "detail", b.auv("win"))                      # rose window glass
    tw = sheet("cr_tower", tx1 - tx0, th + 5.6)
    cx = (tx1 - tx0) / 2
    tw.add(0.0, 0.6, 0.0, th, 0.0, 0.5, "stone"); tw.add((tx1 - tx0) - 0.6, tx1 - tx0, 0.0, th, 0.0, 0.5, "stone")  # corner piers
    for xr in (cx - 2.1, cx + 1.9): tw.add(xr, xr + 0.2, 0.0, 5.0, 0.0, 0.5, "stone2")                              # door jambs
    for (w_, y0, y1) in ((2.4, 4.4, 4.7), (2.0, 4.7, 5.2), (1.4, 5.2, 5.6), (0.7, 5.6, 5.9)): tw.add(cx - w_, cx + w_, y0, y1, 0.0, 0.6, "stone2")   # arched portal head
    ring(tw, cx, 9.0, 1.9, 2.3, 0.0, 0.45, "stone2")                                                               # rose window ring
    for k in range(8):
        a = k * np.pi / 8
        for (r0, r1) in ((0.0, 1.9),): tw.line(cx - np.cos(a) * r1, 9.0 - np.sin(a) * r1, cx + np.cos(a) * r1, 9.0 + np.sin(a) * r1, 0.1, 0.0, 0.25, "stone2")
    tw.add(0.0, tx1 - tx0, th - 0.4, th, 0.0, 0.55, "stone2")
    tw.add(cx - 0.3, cx + 0.3, th + 1.0, th + 4.0, 0.0, 0.3, "stone2"); tw.add(cx - 0.9, cx + 0.9, th + 3.2, th + 3.8, 0.0, 0.3, "stone2")        # cross on the gable
    c.put(tw, Y, tz1, tx0, False)
    # ---- porte-cochere: flat roof on slim columns (columns are relief rows, front and sides)
    pz0, pz1_ = 6.0, 15.0
    b.mbox(m, -4.0, 8.0, Y + 6.5, Y + 7.3, pz0, pz1_, wall="stone", top="roof", bottom=True)
    col = sheet("cr_cols", 12.0, 6.5)
    for xr in (0.5, 11.5): col.add(xr - 0.45, xr + 0.45, 0.0, 6.5, 0.0, 0.7, "col"); col.add(xr - 0.65, xr + 0.65, 0.0, 0.4, 0.0, 0.8, "stone"); col.add(xr - 0.65, xr + 0.65, 6.1, 6.5, 0.0, 0.8, "stone")
    c.put(col, Y, pz1_ - 0.5, -4.0, False, front_only=True)
    c.put(col, Y, pz0 + 0.5, 8.0, True, front_only=True)
    sc = sheet("cr_cols_side", 9.0, 6.5, axis="x")
    for zr in (0.5, 8.5): sc.add(zr - 0.45, zr + 0.45, 0.0, 6.5, 0.0, 0.7, "col"); sc.add(zr - 0.65, zr + 0.65, 0.0, 0.4, 0.0, 0.8, "stone"); sc.add(zr - 0.65, zr + 0.65, 6.1, 6.5, 0.0, 0.8, "stone")
    c.put(sc, Y, 7.6, pz1_, True, front_only=True)                                                                 # right side faces +x
    c.put(sc, Y, -3.6, pz0, False, front_only=True)                                                                # left side faces -x
    # ---- cremator wing (brick block with roller door and a roof plant row) and the chimney
    wx0, wx1, wz0, wz1, wh = 6.0, 22.0, -32.0, -18.0, 7.0
    b.mbox(m, wx0, wx1, Y, Y + wh, wz0, wz1, wall="brick", skip=("+z",), top=None)
    b.zface(m, wx0, wx1, Y, Y + wh, wz1, 1, "brick", b.wuv(wx0, wx1, Y, Y + wh))
    b.mbox(m, wx0 - 0.3, wx1 + 0.3, Y + wh, Y + wh + 0.5, wz0 - 0.3, wz1 + 0.3, wall="stone", top="roof")
    b.zface(m, wx0 + 0.5, wx0 + 5.5, Y, Y + 4.5, wz1 + 0.03, 1, "detail", b.auv("garage"))
    b.zface(m, wx1 - 2.2, wx1 - 0.8, Y, Y + 2.4, wz1 + 0.03, 1, "detail", b.auv("door"))
    pl = sheet("cr_plant", 14.0, 3.0)
    for xr in (1.0, 6.0, 11.0):
        pl.add(xr, xr + 4.0, 0.0, 1.0, 0.0, 0.8, "stone3"); pl.add(xr + 1.0, xr + 3.0, 1.0, 2.6, 0.0, 0.5, "stone3")
    c.put(pl, Y + wh + 0.5, wz1 - 1.5, wx0 + 1.0, False, front_only=True)
    c.put(pl, Y + wh + 0.5, wz1 - 1.5, wx0 + 15.0, True, front_only=True)
    cxm, czm, y_ch = 20.0, -29.0, Y + wh + 0.5
    ht, w0, w1 = 25.2, 3.2, 2.1
    def stack(y0, y1, a0, a1):                                                                                        # tapered square stack: 4 quads
        for (p0, p1, n) in (((-1, 1), (1, 1), "z+"), ((1, 1), (1, -1), "x+"), ((1, -1), (-1, -1), "z-"), ((-1, -1), (-1, 1), "x-")):
            q0 = (cxm + p0[0] * a0 / 2, y0, czm + p0[1] * a0 / 2); q1 = (cxm + p1[0] * a0 / 2, y0, czm + p1[1] * a0 / 2)
            q2 = (cxm + p1[0] * a1 / 2, y1, czm + p1[1] * a1 / 2); q3 = (cxm + p0[0] * a1 / 2, y1, czm + p0[1] * a1 / 2)
            m.quad("chim", q1, q0, q3, q2, [(1, -y0 / 3.6), (0, -y0 / 3.6), (0, -y1 / 3.6), (1, -y1 / 3.6)])
    stack(y_ch, y_ch + ht, w0, w1)
    b.mbox(m, cxm - 1.3, cxm + 1.3, y_ch + ht, y_ch + ht + 0.7, czm - 1.3, czm + 1.3, wall="stone", top="roof")
    b.mbox(m, cxm - 0.8, cxm + 0.8, y_ch + ht + 0.7, y_ch + ht + 1.3, czm - 0.8, czm + 0.8, wall="detail", top="detail")
    # ---- columbarium wall + cloister: niches are relief on both faces, columns a relief row
    kx0, kx1, kz0, kz1 = -24.0, -22.0, -25.0, 5.0
    b.mbox(m, kx0, kx1, Y, Y + 4.0, kz0, kz1, wall="stone", top=None)
    b.mbox(m, kx0 - 0.3, kx1 + 0.3, Y + 4.0, Y + 4.4, kz0 - 0.3, kz1 + 0.3, wall="stone", top="roof")
    b.mbox(m, kx0 - 5.0, kx0 - 1.0, Y + 4.3, Y + 4.6, kz0, kz1, wall="stone", top="roof", bottom=True)             # cloister roof
    nich = sheet("cr_niche", kz1 - kz0, 3.9, axis="x")
    for r_ in range(5):
        for k in range(14):
            zr = 0.9 + k * 2.0
            nich.add(zr - 0.85, zr + 0.85, 0.35 + r_ * 0.72, 0.35 + r_ * 0.72 + 0.6, 0.0, 0.16, "stone2" if (k * 7 + r_ * 3) % 5 else "stone3")
            nich.add(zr - 0.2, zr + 0.2, 0.5 + r_ * 0.72, 0.58 + r_ * 0.72, 0.0, 0.2, "gold")
    c.put(nich, Y, kx1, kz1, True, front_only=True)                                                                # east face looks at the hall (+x)
    c.put(nich, Y, kx0, kz0, False, front_only=True)                                                               # west face looks into the cloister (-x)
    cl = sheet("cr_cloister", kz1 - kz0, 4.3, axis="x")
    for k in range(8):
        zr = 1.0 + k * 3.9
        cl.add(zr - 0.2, zr + 0.2, 0.0, 4.0, 0.0, 0.4, "col")
    cl.add(0.0, kz1 - kz0, 3.95, 4.3, 0.0, 0.5, "stone")
    c.put(cl, Y, kx0 - 4.6, kz0, False, front_only=True)
    c.put(cl, Y, kx0 - 4.6, kz1, True, front_only=True)
    # ---- ground: lawn, plinth pavers, gravel paths, car park with bays, forecourt
    rects = [("plinth", px0, px1, pz0, pz1), ("path", -1.0, 5.0, 9.0, 23.0), ("path", -1.0, 5.0, -48.0, -32.0), ("path", 5.0, 13.0, -9.0, -3.0),
             ("asphalt", -18.0, 22.0, 23.0, 47.0), ("kerb", -18.2, -17.9, 23.0, 47.0), ("kerb", 21.9, 22.2, 23.0, 47.0)]
    for i in range(8): rects.append(("line", -14.0 + i * 4.6, -13.8 + i * 4.6, 26.0, 32.0))
    for i in range(8): rects.append(("line", -14.0 + i * 4.6, -13.8 + i * 4.6, 38.0, 44.0))
    c.tex.update({"base": ground(W, D, 10, (88, 122, 70), rects), "brick": brick_tex((150, 84, 62)), "stone": dc.stone_wall_tex((200, 196, 184)), "roof": slate_tex(),
                  "detail": b.detail_tex((60, 64, 72)), "chim": chimney_tex()})
    return cells, c


# ---------------------------------------------------------------- cemetery lot (13 x 12 cells = 104 x 96 m)
def fence_sheet(seg):
    sh = sheet("fence%d" % round(seg * 2), seg, 3.1)
    sh.add(0, seg, 0, 1.0, 0, 0.45, "stone"); sh.add(0, seg, 1.0, 1.15, 0, 0.55, "stone2")
    posts = (0.0, seg / 2, seg)
    for xc in posts:
        a, c_ = max(0, xc - 0.55), min(seg, xc + 0.55)
        sh.add(a, c_, 0, 2.6, 0, 0.62, "stone"); sh.add(a, c_, 2.6, 2.75, 0, 0.72, "stone2"); sh.add(max(0, xc - 0.35), min(seg, xc + 0.35), 2.75, 2.95, 0, 0.5, "stone2")
    sh.add(0, seg, 1.28, 1.35, 0.15, 0.3, "iron"); sh.add(0, seg, 2.05, 2.12, 0.15, 0.3, "iron")
    x = 0.5
    while x < seg - 0.4:
        if min(abs(x - p) for p in posts) > 0.62:
            sh.add(x - 0.03, x + 0.03, 1.15, 2.45, 0.18, 0.26, "iron"); sh.add(x - 0.05, x + 0.05, 2.45, 2.58, 0.18, 0.24, "iron")
        x += 0.4
    return sh


def stones_variant(L, seed):
    """A headstone row of width L: returns the sheet and the stone centres (metres from the left end)."""
    sh = sheet("gv%d_%d" % (round(L * 2), seed), L, 3.2)
    r = np.random.default_rng(seed * 7 + int(L))
    xs = []
    x = 0.8
    while x < L - 0.6:
        if r.random() > 0.12: xs.append(x)
        x += 1.9
    for xc in xs:
        tag = ("stone", "stone2", "stone3", "stone", "stone2")[int(r.integers(0, 5))]
        sh.add(xc - 0.55, xc + 0.55, 0.0, 0.14, 0.0, 0.4, "plinth")
        form = int(r.integers(0, 5))
        if form == 0:
            sh.add(xc - 0.5, xc + 0.5, 0.14, 1.1, 0.0, 0.18, tag)
            for (w_, y0, y1) in ((0.44, 1.1, 1.2), (0.34, 1.2, 1.3), (0.2, 1.3, 1.37)): sh.add(xc - w_, xc + w_, y0, y1, 0.0, 0.18, tag)
        elif form == 1:
            sh.add(xc - 0.1, xc + 0.1, 0.14, 1.6, 0.0, 0.18, tag); sh.add(xc - 0.45, xc + 0.45, 1.15, 1.35, 0.0, 0.18, tag)
        elif form == 2:
            sh.add(xc - 0.35, xc + 0.35, 0.14, 0.55, 0.0, 0.6, "plinth"); sh.add(xc - 0.12, xc + 0.12, 0.55, 2.4, 0.0, 0.25, tag); sh.add(xc - 0.06, xc + 0.06, 2.4, 2.9, 0.0, 0.2, tag)
        elif form == 3:
            sh.add(xc - 0.55, xc + 0.55, 0.14, 0.9, 0.0, 0.2, tag); sh.add(xc - 0.55, xc + 0.55, 0.9, 1.1, 0.0, 0.2, tag); sh.add(xc - 0.4, xc + 0.4, 1.1, 1.25, 0.0, 0.2, tag)
        else:
            sh.add(xc - 0.3, xc + 0.3, 0.14, 0.6, 0.0, 0.5, tag); sh.add(xc - 0.28, xc + 0.28, 0.6, 1.1, 0.0, 0.4, tag); sh.add(xc - 0.1, xc + 0.1, 1.1, 1.5, 0.0, 0.2, tag)
    return sh, xs


def mausoleum_mesh():
    """Front +z, centred on the origin. Body, gable roof, relief facade (columns, pediment, portal, steps), door glass from the atlas."""
    c = Ctx(); m = c.m
    w, d, h = 4.6, 5.6, 4.1
    b.mbox(m, -w / 2, w / 2, 0.5, h, -d / 2, d / 2, wall="stone", skip=("+z",), top=None)
    b.zface(m, -w / 2, w / 2, 0.5, h, d / 2, 1, "stone", b.wuv(-w / 2, w / 2, 0.5, h))
    gable_z(m, -w / 2 - 0.3, w / 2 + 0.3, h + 0.35, -d / 2 - 0.3, d / 2 + 0.7, 1.6, over=0.0)
    b.zface(m, -0.8, 0.8, 0.5, 3.1, d / 2 + 0.03, 1, "detail", b.auv("door"))
    fa = sheet("maus_front", 5.4, 6.3)
    for xr in (0.5, 4.9): fa.add(xr - 0.28, xr + 0.28, 0.5, 3.7, 0.0, 0.7, "col"); fa.add(xr - 0.4, xr + 0.4, 0.5, 0.75, 0.0, 0.75, "stone"); fa.add(xr - 0.4, xr + 0.4, 3.5, 3.7, 0.0, 0.75, "stone")
    fa.add(0.1, 5.3, 3.7, 4.0, 0.0, 0.75, "stone2")
    for (w_, y0, y1) in ((2.4, 4.0, 4.3), (1.9, 4.3, 4.7), (1.3, 4.7, 5.1), (0.7, 5.1, 5.5)): fa.add(2.7 - w_, 2.7 + w_, y0, y1, 0.0, 0.7, "stone2")     # pediment
    fa.add(0.0, 5.4, 0.0, 0.5, 0.0, 0.9, "stone"); fa.add(0.5, 4.9, 0.0, 0.25, 0.0, 1.1, "stone")                                          # plinth and steps
    fa.add(1.9, 3.5, 0.5, 3.3, 0.0, 0.3, "dark")
    c.put(fa, 0.0, d / 2 + 0.55, -2.7, False)
    return c


def chapel_mesh():
    c = Ctx(); m = c.m
    w, d, h = 8.0, 10.0, 5.0
    b.mbox(m, -w / 2, w / 2, 0.0, h, -d / 2, d / 2, wall="stone", skip=("+z",), top=None)
    b.zface(m, -w / 2, w / 2, 0.0, h, d / 2, 1, "stone", b.wuv(-w / 2, w / 2, 0.0, h))
    gable_z(m, -w / 2 - 0.5, w / 2 + 0.5, h, -d / 2 - 0.5, d / 2 + 0.5, 4.0, over=0.0)
    b.zface(m, -0.8, 0.8, 0.0, 3.0, d / 2 + 0.03, 1, "detail", b.auv("door"))
    for xs, f in ((w / 2, 1), (-w / 2, -1)): b.windows_x(m, -d / 2 + 1.5, d / 2 - 1.5, 1.4, 3.8, xs, f, 3.0, "win")
    fa = sheet("chapel_front", w, h + 5.0)
    for xr in (0.0, w - 0.5): fa.add(xr, xr + 0.5, 0.0, h, 0.0, 0.5, "stone2")
    for (w_, y0, y1) in ((1.2, 3.0, 3.25), (0.95, 3.25, 3.5), (0.6, 3.5, 3.7)): fa.add(w / 2 - w_, w / 2 + w_, y0, y1, 0.0, 0.5, "stone2")
    fa.add(w / 2 - 0.2, w / 2 + 0.2, h + 0.5, h + 3.4, 0.0, 0.3, "stone2"); fa.add(w / 2 - 0.6, w / 2 + 0.6, h + 2.6, h + 3.0, 0.0, 0.3, "stone2")
    fa.add(0.0, w, 0.0, 0.5, 0.0, 0.6, "stone")
    c.put(fa, 0.0, d / 2 + 0.2, -w / 2, False)
    return c


def office_mesh():
    c = Ctx(); m = c.m
    w, d, h = 15.0, 9.0, 4.2
    y0 = 0.5
    b.mbox(m, -8.5, 8.5, 0.0, y0, -5.5, 5.5, wall="stone", top="roof")
    b.mbox(m, -w / 2, w / 2, y0, y0 + h, -d / 2, d / 2, wall="brick", skip=("+z",), top=None)
    b.zface(m, -w / 2, w / 2, y0, y0 + h, d / 2, 1, "brick", b.wuv(-w / 2, w / 2, y0, y0 + h))
    b.mbox(m, -w / 2 - 0.3, w / 2 + 0.3, y0 + h, y0 + h + 0.35, -d / 2 - 0.3, d / 2 + 0.3, wall="stone", top=None)
    dc.gable(m, -w / 2 - 0.5, w / 2 + 0.5, y0 + h + 0.35, -d / 2 - 0.3, d / 2 + 0.3, 3.0, over=0.0, wall="stone")
    for xc in (-5.0, -2.5, 3.0, 5.5): b.zface(m, xc - 0.7, xc + 0.7, y0 + 1.0, y0 + 2.6, d / 2 + 0.03, 1, "detail", b.auv("win"))
    b.zface(m, 0.4 - 0.65, 0.4 + 0.65, y0, y0 + 2.5, d / 2 + 0.03, 1, "detail", b.auv("door"))
    b.windows_x(m, -3.0, 3.0, y0 + 1.0, y0 + 2.6, -w / 2, -1, 2.5, "win")
    b.zface(m, -1.6, 2.4, y0 + 3.0, y0 + 3.7, d / 2 + 0.1, 1, "detail", [(236 / 512, 240 / 512)] * 4)           # blank sign board
    b.mbox(m, -1.4, 2.2, y0 + 3.3, y0 + 3.55, d / 2, d / 2 + 2.4, wall="stone", top="roof", bottom=True)         # porch canopy
    for sx in (-1.2, 2.0): b.mbox(m, sx - 0.1, sx + 0.1, y0, y0 + 3.3, d / 2 + 2.1, d / 2 + 2.3, wall="stone", top=None)
    b.mbox(m, 4.0, 5.0, y0 + h + 1.5, y0 + h + 3.9, -2.0, -1.0, wall="brick", top="roof")
    fa = sheet("office_front", w, h + 0.5)
    for xc in (-5.0, -2.5, 3.0, 5.5):
        xr = xc + w / 2
        fa.add(xr - 0.8, xr + 0.8, 0.7, 0.9, 0.0, 0.3, "stone2"); fa.add(xr - 0.8, xr + 0.8, 2.6, 2.75, 0.0, 0.3, "stone2")
    fa.add(0.0, w, h - 0.1, h + 0.1, 0.0, 0.4, "stone2")
    c.put(fa, y0, d / 2 + 0.2, -w / 2, False)
    return c


def cemetery_lot():
    cells = (13, 12)
    W, D = cells[0] * CELL, cells[1] * CELL
    c = Ctx(); m = c.m
    rng = np.random.default_rng(19)
    m.quad("base", (-W / 2, 0.0, D / 2), (W / 2, 0.0, D / 2), (W / 2, 0.0, -D / 2), (-W / 2, 0.0, -D / 2), [(0, 1), (1, 1), (1, 0), (0, 0)])
    L = lambda x: x - 58.0           # mock-up world -> lot
    Lz = lambda z: z - 10.0
    x0, x1, z0, z1 = L(10.0), L(80.0), Lz(-38.0), Lz(40.0)
    gx = L(45.0)
    # ---- perimeter: stone wall, piers and iron railings as relief sheets, front faces only (inside and outside)
    # axis: 'z' sheets run along x (the sheet's wall plane is a z plane); 'x' sheets run along z
    def runx(a, b_, z):
        n = max(1, int(np.ceil((b_ - a) / 26.0))); seg = (b_ - a) / n
        for i in range(n):
            sh = fence_sheet(seg); sh.axis = "z"; s0 = a + i * seg
            c.put(sh, 0.0, z, s0, False, front_only=True); c.put(sh, 0.0, z, s0 + seg, True, front_only=True)
    def runz(a, b_, x):
        n = max(1, int(np.ceil((b_ - a) / 26.0))); seg = (b_ - a) / n
        for i in range(n):
            sh = fence_sheet(seg); sh.axis = "x"; s0 = a + i * seg
            c.put(sh, 0.0, x, s0, False, front_only=True); c.put(sh, 0.0, x, s0 + seg, True, front_only=True)
    runx(x0, x1, z0); runz(z0, Lz(6.4), x1); runz(Lz(9.6), z1, x1); runz(z0, z1, x0)
    runx(x0, gx - 5.0, z1); runx(gx + 5.0, x1, z1)
    # ---- lychgate
    for (px_, pz_) in ((-4.5, -1.5), (4.5, -1.5), (-4.5, 1.5), (4.5, 1.5)):
        b.mbox(m, gx + px_ - 0.3, gx + px_ + 0.3, 0.0, 3.6, z1 + pz_ - 0.3, z1 + pz_ + 0.3, wall="stone", top=None)
    dc.gable(m, gx - 5.5, gx + 5.5, 3.8, z1 - 2.75, z1 + 2.75, 2.6, over=0.0, wall="stone")
    # ---- central monument: stepped base, cross
    mx_, mz_ = gx, Lz(0.0)
    for s in range(3):
        b.mbox(m, mx_ - (2 - s * 0.55), mx_ + (2 - s * 0.55), s * 0.5, s * 0.5 + 0.5, mz_ - (2 - s * 0.55), mz_ + (2 - s * 0.55), wall="stone", top="stone")
    b.mbox(m, mx_ - 0.45, mx_ + 0.45, 1.5, 8.5, mz_ - 0.45, mz_ + 0.45, wall="stone", top="stone")
    b.mbox(m, mx_ - 1.8, mx_ + 1.8, 7.2, 8.1, mz_ - 0.45, mz_ + 0.45, wall="stone", top="stone")
    # ---- headstone rows: one relief sheet per row, drawn front and back; stones/plots shared with the painted ground
    xs_blocks = [(L(13.0), L(21.5)), (L(26.5), gx - 3.5), (gx + 3.5, L(63.5)), (L(68.5), L(77.0))]
    bands = [(Lz(-35.0), Lz(-14.0)), (Lz(-10.0), Lz(10.0)), (Lz(14.0), Lz(37.0))]
    variants = {}
    def variant(wid, i):
        key = (round(wid * 2), i)
        if key not in variants: variants[key] = stones_variant(wid, i)
        return variants[key]
    plot_rects = []
    row_count = 0
    for bi, (ax, bx_) in enumerate(xs_blocks):
        for (az, bz) in bands:
            zr = az + 0.6
            while zr < bz - 2.6:
                ax_use, wid = ax, bx_ - ax
                if abs(zr + 10.0) < 9.0 and bi in (1, 2):                                      # beside the war memorial: only the outer 9.5 m
                    if bi == 1: ax_use, wid = ax, (gx - 9.0) - ax
                    else: ax_use, wid = gx + 9.0, bx_ - (gx + 9.0)
                sh, xs = variant(wid, row_count % 3)
                c.put(sh, 0.0, zr, ax_use, False, front_only=True)
                c.put(sh, 0.0, zr, ax_use + wid, True, front_only=True)
                for xc in xs:
                    if rng.random() < 0.55:
                        grass = rng.random() < 0.5
                        plot_rects.append(("bed", ax_use + xc - 0.55, ax_use + xc + 0.55, zr + 0.15, zr + 2.35, (150, 168, 112) if grass else (190, 183, 168)))
                row_count += 1
                zr += 3.8
    # ---- mausoleums, chapel of rest (facing the avenue), cemetery office, hearse bay
    maus = mausoleum_mesh()
    for (mx0, mz0, ry) in ((L(17.0), Lz(-30.0), 0.0), (L(73.0), Lz(-30.0), 0.0), (L(17.0), Lz(-10.0), np.pi / 2), (L(73.0), Lz(-10.0), -np.pi / 2)):
        absorb(c, maus, ry, mx0, mz0)
    absorb(c, chapel_mesh(), 0.0, gx, Lz(-30.0))
    absorb(c, office_mesh(), 0.0, L(96.0), Lz(8.0))
    b.mbox(m, L(99.0), L(105.0), 3.8, 4.05, Lz(34.0), Lz(39.0), wall="stone", top="roof", bottom=True)
    for (px_, pz_) in ((99.2, 34.2), (104.8, 34.2), (99.2, 38.8), (104.8, 38.8)):
        b.mbox(m, L(px_) - 0.12, L(px_) + 0.12, 0.0, 3.8, Lz(pz_) - 0.12, Lz(pz_) + 0.12, wall="stone", top=None)
    # ---- ground: lawn, avenue and cross paths, plots, hearse lot with bays, driveway, kerbs
    rects = [("path", gx - 2.5, gx + 2.5, z0 + 1.0, z1 + 14.0), ("path", x0 + 2.0, x1 - 2.0, Lz(-12.0) - 1.75, Lz(-12.0) + 1.75), ("path", x0 + 2.0, x1 - 2.0, Lz(12.0) - 1.75, Lz(12.0) + 1.75),
             ("path", L(24.0) - 1.5, L(24.0) + 1.5, z0 + 1.0, z1 - 1.0), ("path", L(66.0) - 1.5, L(66.0) + 1.5, z0 + 1.0, z1 - 1.0), ("path", x1, x1 + 14.0, Lz(8.0) - 1.25, Lz(8.0) + 1.25),
             ("asphalt", L(85.0), L(107.0), Lz(24.0), Lz(40.0)), ("asphalt", L(99.0), L(105.0), Lz(40.0), Lz(58.0)),
             ("kerb", L(85.0), L(107.0), Lz(24.0) - 0.15, Lz(24.0) + 0.15), ("kerb", L(85.0), L(107.0), Lz(40.0) - 0.15, Lz(40.0) + 0.15),
             ("kerb", L(85.0) - 0.15, L(85.0) + 0.15, Lz(24.0), Lz(40.0)), ("kerb", L(107.0) - 0.15, L(107.0) + 0.15, Lz(24.0), Lz(40.0)),
             ("path", mx_ - 4.0, mx_ + 4.0, mz_ - 4.0, mz_ + 4.0)] + plot_rects
    for i in range(5):
        xx = L(87.0 + i * 4.5)
        rects.append(("line", xx - 0.1, xx + 0.1, Lz(25.0), Lz(32.0)))
    rects.append(("line", L(85.0), L(107.0), Lz(25.0) - 0.1, Lz(25.0) + 0.1))
    c.tex.update({"base": ground(W, D, 8, (88, 122, 70), rects), "stone": dc.stone_wall_tex((200, 196, 184)), "brick": brick_tex((150, 84, 62)), "roof": slate_tex(),
                  "detail": b.detail_tex((34, 52, 40))})
    return cells, c


MODELS = [("CR_8x12_crematorium_campus_L3", crematorium_lot), ("CM_13x12_memorial_cemetery_L3", cemetery_lot)]
