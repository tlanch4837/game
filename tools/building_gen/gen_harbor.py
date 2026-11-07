"""Ferry port + cargo port -> GLB (replaces the procedural Passenger Harbor / Cargo Harbor in src/city/transport.h).
Ultra low poly, detail in textures and silhouette-POM: concrete quay face with painted fenders (1 quad), the whole yard / car park /
quay markings in one ground texture, container stacks as textured boxes, STS crane leg frames as SPOM relief sheets (a lattice drawn
into a 1.2 m thick volume), terminal / warehouse facades as relief volumes. No ships, cars, trucks, trees or lamp posts baked in.
Frame: lot centred on the origin, FRONT (water) at +z. The shoreline sits at z = D*(0.5 - dock) (see BuildingDef::dock): land at z < shore,
quay top at y = QY.
Run: python gen_harbor.py   Output: assets/buildings/transport/harbor/glb + preview.html + catalog.json
"""
import json, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_low_density as g
from gen_low_density import Mesh, CELL
import gen_bus_depot as b
import spom

BASE = os.path.join(g.ROOT, "assets", "buildings", "transport", "harbor")
OUT = os.path.join(BASE, "glb")
QY = 0.5            # quay top above the lot's origin level
rng = np.random.default_rng(5)
CRANE_RED = (0.78, 0.25, 0.18)


# ---------------------------------------------------------------- textures
def quay_face_tex():
    """8 m x 4.5 m tile of the quay wall: cope on top, rubber fenders every 4 m, tide stain at the foot. v = 0 at the cope, 1 at the foot."""
    w, h = 512, 512
    a = np.full((h, w, 3), (128, 126, 120), np.float32) + b.noise(512, 6, 1.4, 21)[..., None]
    hh = np.full((h, w), 0.55, np.float32)
    a[:int(0.07 * h)] = (176, 172, 164); hh[:int(0.07 * h)] = 0.8
    for xc in (0.125, 0.625):
        x0, x1 = int((xc - 0.03) * w), int((xc + 0.03) * w)
        a[int(0.14 * h):int(0.66 * h), x0:x1] = (24, 24, 26); hh[int(0.14 * h):int(0.66 * h), x0:x1] = 0.95
        a[int(0.14 * h):int(0.66 * h), x0 - 3:x0] *= 0.6; hh[int(0.14 * h):int(0.66 * h), x0 - 3:x0] = 0.3
    for k in (0, 256):
        a[:, k:k + 3] *= 0.7; hh[:, k:k + 3] = 0.2
    t = np.clip((np.arange(h) / h - 0.7) / 0.3, 0, 1)[:, None, None]
    a = a * (1 - 0.55 * t) + np.array((50, 70, 52), np.float32) * 0.55 * t
    return b.maps(a, hh, 5.0)


PALETTE = [(176, 66, 52), (44, 98, 160), (214, 150, 52), (60, 128, 98), (206, 208, 204), (132, 138, 144), (150, 78, 58), (40, 74, 120),
           (204, 176, 70), (104, 140, 166), (170, 104, 92), (86, 112, 86)]


P8 = PALETTE[:8]


def _grain(a, n_h, n_w, seed, amp=0.10):
    r = np.random.default_rng(seed)
    return a * (1.0 - amp / 2 + amp * r.random((n_h, n_w, 1))).astype(np.float32)


def container_tex():
    """Side atlas 2048 x 128: eight cells (one per shipping colour), each ONE container side 6.1 m x 2.6 m. u picks the colour, v repeats per tier,
    so every tier of a stack column is the same colour: corrugation, rails, corner posts, a blank logo bar, rust streaks, a grimy foot."""
    C, H = 256, 128
    a = np.zeros((H, C * 8, 3), np.float32); h = np.zeros((H, C * 8), np.float32)
    r = np.random.default_rng(12)
    for ci, col in enumerate(P8):
        x0 = ci * C
        c = np.array(col, np.float32)
        a[:, x0:x0 + C] = c; h[:, x0:x0 + C] = 0.5
        for i in range(12, C - 12, 8):
            a[10:H - 10, x0 + i:x0 + i + 3] *= 0.82; h[10:H - 10, x0 + i:x0 + i + 3] = 0.15
            a[10:H - 10, x0 + i + 3:x0 + i + 6] *= 1.04; h[10:H - 10, x0 + i + 3:x0 + i + 6] = 0.85
        a[0:10, x0:x0 + C] *= 0.68; a[H - 10:H, x0:x0 + C] *= 0.68                      # rails
        a[:, x0:x0 + 10] *= 0.7; a[:, x0 + C - 10:x0 + C] *= 0.7                        # corner posts
        h[0:10] = 0.9; h[H - 10:H] = 0.9; h[:, x0:x0 + 10] = 0.9; h[:, x0 + C - 10:x0 + C] = 0.9
        a[44:56, x0 + 70:x0 + 180] = np.clip(c * 0.4 + 130, 0, 255)                     # logo bar
        for _ in range(3):
            xx = x0 + int(r.integers(24, C - 24)); L0 = int(r.integers(20, 70))
            a[10:10 + L0, xx:xx + 4] *= np.array((0.9, 0.8, 0.72), np.float32)
        a[H - 26:H - 10, x0:x0 + C] *= 0.93
    return b.maps(_grain(a, H, C * 8, 13), h, 1.8)


def container_end_tex():
    """End atlas 2048 x 64: eight cells (same colours); a cell is the 9.76 m end of a block = four 2.44 m container ends with door bars."""
    C, H = 256, 64
    a = np.zeros((H, C * 8, 3), np.float32); h = np.zeros((H, C * 8), np.float32)
    for ci, col in enumerate(P8):
        c = np.array(col, np.float32) * 0.95
        for k in range(4):
            x0 = ci * C + k * 64
            a[:, x0:x0 + 64] = c; h[:, x0:x0 + 64] = 0.5
            a[:, x0:x0 + 4] *= 0.55; a[:, x0 + 60:x0 + 64] *= 0.55; a[0:4, x0:x0 + 64] *= 0.55; a[H - 4:H, x0:x0 + 64] *= 0.55   # frame
            h[:, x0:x0 + 4] = 0.9; h[:, x0 + 60:x0 + 64] = 0.9
            a[6:H - 6, x0 + 31:x0 + 33] *= 0.4                                           # door seam
            for lx in (12, 22, 41, 51): a[6:H - 6, x0 + lx:x0 + lx + 2] *= 0.6; h[6:H - 6, x0 + lx:x0 + lx + 2] = 0.8   # locking bars
    return b.maps(_grain(a, H, C * 8, 14), h, 1.6)


def container_top_tex():
    """Roof atlas 2048 x 512: eight cells (same colours); a cell is one 6.1 m x 9.76 m stack top = four lanes, ribs across the width, rails between."""
    C, H = 256, 512
    a = np.zeros((H, C * 8, 3), np.float32); hh = np.zeros((H, C * 8), np.float32)
    shade = (1.0, 0.93, 1.05, 0.9)
    for ci, col in enumerate(P8):
        for lane in range(4):
            x0, y0 = ci * C, lane * 128
            a[y0:y0 + 128, x0:x0 + C] = np.array(col, np.float32) * shade[lane] * 0.92
            hh[y0:y0 + 128, x0:x0 + C] = 0.5
            for i in range(6, C - 6, 8):
                a[y0 + 8:y0 + 120, x0 + i:x0 + i + 3] *= 0.80; hh[y0 + 8:y0 + 120, x0 + i:x0 + i + 3] = 0.15
            a[y0:y0 + 8, x0:x0 + C] *= 0.5; a[y0 + 120:y0 + 128, x0:x0 + C] *= 0.5
            a[y0:y0 + 128, x0:x0 + 8] *= 0.55; a[y0:y0 + 128, x0 + C - 8:x0 + C] *= 0.55
    return b.maps(_grain(a, H, C * 8, 15, 0.12), hh, 1.6)


def clad_tex(col):
    """3 m tile of trapezoidal metal sheet cladding: ribs every 0.19 m, a dark base course, faint streaks."""
    n = 512
    a = np.full((n, n, 3), col, np.float32) + b.noise(n, 4, 1.0, 31)[..., None]
    h = np.full((n, n), 0.5, np.float32)
    for i in range(0, n, 16):
        a[:, i:i + 5] *= 0.78; a[:, i + 8:i + 11] *= 1.06; h[:, i:i + 5] = 0.15; h[:, i + 8:i + 11] = 0.85
    a[int(n * 0.86):] *= 0.7
    streak = b.noise(n, 10, 0.8, 32)[:1].repeat(n, 0) * np.linspace(1, 0.1, n)[:, None]
    a += streak[..., None] * 0.6
    return b.maps(a, h, 4.0)


def ferry_ground(bx0, bx1, bz0, sz):
    """Whole ground slab at 12 px/m: quay concrete by the water, covered boarding apron, bus bay, car park with painted bays."""
    px = 12
    w, d = int((bx1 - bx0) * px), int((sz - bz0) * px)
    im = Image.new("RGB", (w, d), (66, 68, 70)); dr = ImageDraw.Draw(im)
    X = lambda x: (x - bx0) * px; Z = lambda z: (z - bz0) * px
    dr.rectangle([0, Z(-4.5), w, d], fill=(172, 170, 162))                                       # quay concrete
    for x in np.arange(bx0, bx1, 4.0): dr.line([X(x), Z(-4.5), X(x), d], fill=(150, 148, 142), width=2)
    for z in np.arange(-4.5, sz, 4.0): dr.line([0, Z(z), w, Z(z)], fill=(150, 148, 142), width=2)
    dr.rectangle([0, Z(sz - 0.9), w, d], fill=(228, 182, 40))                                    # yellow safety edge, black chevrons
    for x in np.arange(bx0, bx1, 1.6): dr.polygon([(X(x), Z(sz - 0.9)), (X(x + 0.8), Z(sz - 0.9)), (X(x + 1.2), d), (X(x + 0.4), d)], fill=(30, 30, 30))
    for x in range(-40, 41, 8): dr.ellipse([X(x) - 7, Z(sz - 2.2) - 7, X(x) + 7, Z(sz - 2.2) + 7], fill=(30, 30, 32))   # bollards
    dr.rectangle([X(-11), Z(-9), X(11), Z(-4.5)], fill=(86, 88, 90))                             # drop-off lane in front of the terminal side
    for x in np.arange(-11, 11.1, 2.8): dr.line([X(x), Z(-9), X(x), Z(-4.5)], fill=(235, 235, 225), width=3)
    for row_z0, row_z1 in ((-28.5, -22.5), (-18.0, -12.0)):                                       # parking bays
        for x in np.arange(-40, 40.1, 2.8): dr.line([X(x), Z(row_z0), X(x), Z(row_z1)], fill=(235, 235, 225), width=3)
        dr.line([X(-40), Z(row_z0), X(40), Z(row_z0)], fill=(235, 235, 225), width=3)
        dr.line([X(-40), Z(row_z1), X(40), Z(row_z1)], fill=(235, 235, 225), width=3)
    for x in range(-40, 41, 6): dr.line([X(x), Z(-20.2), X(x + 3), Z(-20.2)], fill=(228, 182, 40), width=3)   # aisle centre line
    dr.rectangle([0, 0, w - 1, d - 1], outline=(130, 130, 126), width=6)
    arr = np.asarray(im.filter(ImageFilter.GaussianBlur(0.6)), np.float32) + b.noise(max(w, d), 3, 1.4, 22)[:d, :w, None]
    return b.maps(arr, np.asarray(im.convert("L"), np.float32) / 255, 2.0)


def cargo_ground(bx0, bx1, bz0, sz, rails, blocks):
    """184 x 70 m ground at 8 px/m: quay slabs, crane rails, painted yard lanes, container pads."""
    px = 8
    w, d = int((bx1 - bx0) * px), int((sz - bz0) * px)
    im = Image.new("RGB", (w, d), (74, 76, 78)); dr = ImageDraw.Draw(im)
    X = lambda x: (x - bx0) * px; Z = lambda z: (z - bz0) * px
    dr.rectangle([0, Z(-5.5), w, d], fill=(160, 160, 154))                                       # quay apron concrete
    for x in np.arange(bx0, bx1, 6.0): dr.line([X(x), Z(-5.5), X(x), d], fill=(138, 138, 132), width=2)
    for z in np.arange(-5.5, sz, 6.0): dr.line([0, Z(z), w, Z(z)], fill=(138, 138, 132), width=2)
    for zr in rails:                                                                              # crane rails with sleepers
        dr.rectangle([0, Z(zr - 0.45), w, Z(zr + 0.45)], fill=(52, 52, 54))
        dr.line([0, Z(zr), w, Z(zr)], fill=(118, 118, 120), width=2)
    dr.rectangle([0, Z(sz - 0.8), w, d], fill=(228, 182, 40))                                    # yellow safety edge
    for x in np.arange(bx0, bx1, 1.6): dr.polygon([(X(x), Z(sz - 0.8)), (X(x + 0.8), Z(sz - 0.8)), (X(x + 1.2), d), (X(x + 0.4), d)], fill=(30, 30, 30))
    for x in range(-84, 85, 12): dr.ellipse([X(x) - 6, Z(sz - 1.8) - 6, X(x) + 6, Z(sz - 1.8) + 6], fill=(30, 30, 32))
    for (x0, x1, z0, z1) in blocks:                                                               # container pads
        dr.rectangle([X(x0 - 1), Z(z0 - 1), X(x1 + 1), Z(z1 + 1)], fill=(98, 100, 102))
    for z in (-21.0, -37.0, -7.2):                                                                # white lane lines across the yard
        dr.line([X(bx0 + 2), Z(z), X(bx1 - 2), Z(z)], fill=(226, 226, 218), width=2)
    dr.rectangle([0, 0, w - 1, d - 1], outline=(130, 130, 126), width=6)
    arr = np.asarray(im.filter(ImageFilter.GaussianBlur(0.5)), np.float32) + b.noise(max(w, d), 3, 1.4, 23)[:d, :w, None]
    return b.maps(arr, np.asarray(im.convert("L"), np.float32) / 255, 2.0)


# ---------------------------------------------------------------- mesh helpers
def flat_normal():
    return spom.png(np.dstack([np.full((4, 4, 3), (128, 128, 255), np.uint8), np.full((4, 4, 1), 255, np.uint8)]))


def quay_relief(m, tex, prev, bx0, bx1, sz, drop, nseg):
    """Fenders and the cope as SPOM relief on the quay wall (the flat wall quad stays behind them). Painted in world x."""
    segw = (bx1 - bx0) / nseg
    for i in range(nseg):
        x0 = bx0 + i * segw
        sh = Sheet("quay%d" % i, False, sz, x0, segw, drop, axis='z')
        kinds = sh
        fx = np.floor(x0 / 4.0) * 4.0 + 1.0                                     # fenders sit at x = 4k + 1, matching the wall texture
        while fx < x0 + segw:
            if fx - 0.25 >= x0 and fx + 0.25 <= x0 + segw:
                sh.add(fx - 0.25, fx + 0.25, 0.9, drop - 1.0, 0.0, 0.38, "dark")
                sh.add(fx - 0.2, fx + 0.2, 0.9, 1.1, 0.0, 0.42, "dark")
            fx += 4.0
        sh.add(x0, x0 + segw, drop - 0.28, drop, 0.0, 0.26, "white")             # cope
        sh.add(x0, x0 + segw, 0.0, 0.18, 0.0, 0.2, "dark")                       # tide line shadow
        sh.emit(m, QY - drop)
        prev.append((sh, QY - drop, sh.place(sz, x0, False)))
        tex["spom_" + sh.kind] = sh.L.encode()
        tex["sprev_" + sh.kind] = (sh.preview(), flat_normal())


def new_mesh():
    m = Mesh()
    for k in ("face", "cont", "contend", "conttop", "clad"): m.prims[k] = []
    return m


def cuv(a, c, y0, y1, ou, ov):
    """Container cell uv for a face spanning a..c along its length and y0..y1 up: one cell = 6.1 x 2.6 m, 4 x 4 cells per tile."""
    return [(a / 24.4 + ou, -y0 / 10.4 + ov), (c / 24.4 + ou, -y0 / 10.4 + ov), (c / 24.4 + ou, -y1 / 10.4 + ov), (a / 24.4 + ou, -y1 / 10.4 + ov)]


def cuve(a, c, y0, y1, ou, ov):
    """End faces: one cell per 2.44 m container end, so the four lanes across a 9.8 m block each get their own colour."""
    return [(a / 9.76 + ou, -y0 / 10.4 + ov), (c / 9.76 + ou, -y0 / 10.4 + ov), (c / 9.76 + ou, -y1 / 10.4 + ov), (a / 9.76 + ou, -y1 / 10.4 + ov)]


def cbox(m, x0, x1, y0, y1, z0, z1, ou, ov):
    b.zface(m, x0, x1, y0, y1, z1, 1, "cont", cuv(x0, x1, y0, y1, ou, ov))
    b.zface(m, x0, x1, y0, y1, z0, -1, "cont", cuv(x1, x0, y0, y1, ou, ov))
    b.xface(m, z0, z1, y0, y1, x1, 1, "cont", cuv(z1, z0, y0, y1, ou + 0.25, ov))
    b.xface(m, z0, z1, y0, y1, x0, -1, "cont", cuv(z0, z1, y0, y1, ou + 0.25, ov))
    m.quad("cont", (x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0),
           [(x0 / 24.4 + ou, z1 / 10.4 + ov), (x1 / 24.4 + ou, z1 / 10.4 + ov), (x1 / 24.4 + ou, z0 / 10.4 + ov), (x0 / 24.4 + ou, z0 / 10.4 + ov)])


class Sheet:
    """A SPOM relief sheet standing on a wall plane x = xw, facing -x (mirror False) or +x (mirror True). Paints in WORLD z and metres above
    the sheet's own y origin. Lines are rasterised per pixel (no stair steps). The preview draws the same relief as an alpha cutout."""
    def __init__(self, kind, mirror, xw, zo, W, H, axis='x'):
        self.kind, self.mirror, self.xw, self.zo, self.W, self.H, self.axis = kind, mirror, xw, zo, W, H, axis
        spom.KINDS[kind] = (W, H)
        self.L = spom.Layers(W, H)
        self.col = {"red": CRANE_RED, "dark": (0.2, 0.21, 0.23), "white": (0.88, 0.88, 0.85)}

    def add(self, z0, z1, y0, y1, d0, d1, tag="red"):
        a, c = (z0 - self.zo, z1 - self.zo) if not self.mirror else (self.zo - z1, self.zo - z0)
        self.L.box(a, c, y0, y1, d0, d1, self.col[tag])

    def line(self, za, ya, zb, yb, t, d0, d1, tag="red"):
        """A strut from (za, ya) to (zb, yb), t thick (perpendicular), rasterised per pixel."""
        L = self.L
        xa, xb = (za - self.zo, zb - self.zo) if not self.mirror else (self.zo - za, self.zo - zb)
        dx, dy = xb - xa, yb - ya
        n = np.hypot(dx, dy)
        if n < 1e-6: return
        X, Y = np.meshgrid(L.xs, L.ys)
        px, py = X - xa, Y - ya
        u = np.clip((px * dx + py * dy) / (n * n), 0.0, 1.0)
        d = np.hypot(px - u * dx, py - u * dy)
        m = d <= t / 2
        L.start[m] = d0; L.end[m] = d1; L.col[m] = self.col[tag]

    def preview(self):
        L = self.L
        solid = L.end > L.start + 0.004
        shade = 0.78 + 0.22 * np.clip(L.end / spom.DEPTH, 0, 1)
        rgb = np.clip(L.col * shade[..., None] * 255, 0, 255).astype(np.uint8)
        if solid.any():                                                        # transparent texels take the mean solid colour so filtering never fringes them black
            rgb[~solid] = rgb[solid].mean(0).astype(np.uint8)
        a = np.where(solid, 255, 0).astype(np.uint8)
        return spom.png(np.dstack([rgb, a]))

    def place(self, xw, zo, mirror):
        self.xw, self.zo, self.mirror = xw, zo, mirror
        return (xw, zo, mirror)

    def _frame(self, y0):
        if self.axis == 'x':
            return (self.xw, y0, self.zo), ((1, 0, 0) if self.mirror else (-1, 0, 0))
        return (self.zo, y0, self.xw), ((0, 0, -1) if self.mirror else (0, 0, 1))

    def emit(self, m, y0=QY):
        m.prims.setdefault("spom_" + self.kind, [])
        o, n = self._frame(y0)
        spom.spom_box(m, self.kind, o, n)

    def emit_preview(self, pm, y0=QY):
        o, n = self._frame(y0)
        N = np.array(n, np.float32)
        up = np.array((0, 1, 0), np.float32)
        T = np.cross(up, N).astype(np.float32)
        O = np.array(o, np.float32) + N * 0.3
        c = [O, O + T * self.W, O + T * self.W + up * self.H, O + up * self.H]
        uv = [(0, 1), (1, 1), (1, 0), (0, 0)]
        pm.prims.setdefault("sprev_" + self.kind, [])
        pm.quad("sprev_" + self.kind, *c, uv)
        pm.quad("sprev_" + self.kind, c[1], c[0], c[3], c[2], [uv[1], uv[0], uv[3], uv[2]])


# ---------------------------------------------------------------- ferry port
def ferry(wallc=(214, 216, 210), accent=(44, 110, 181)):
    W, D = 12 * CELL, 8 * CELL
    sz = D * (0.5 - 0.36)
    m = new_mesh()
    bx0, bx1, bz0 = -44.0, 44.0, -30.0
    m.quad("base", (bx0, QY, sz), (bx1, QY, sz), (bx1, QY, bz0), (bx0, QY, bz0), [(0, 1), (1, 1), (1, 0), (0, 0)])
    uf = lambda x: x / 8.0
    b.zface(m, bx0, bx1, QY - 4.5, QY, sz, 1, "face", [(uf(bx0), 1), (uf(bx1), 1), (uf(bx1), 0), (uf(bx0), 0)])     # quay wall: fenders and cope are painted
    b.zface(m, bx0, bx1, -0.4, QY, bz0, -1, "wall", b.wuv(bx1, bx0, -0.4, QY))                                       # skirts so the slab never floats
    b.xface(m, bz0, sz, -0.4, QY, bx0, -1, "wall", b.wuv(bz0, sz, -0.4, QY))
    b.xface(m, bz0, sz, -0.4, QY, bx1, 1, "wall", b.wuv(sz, bz0, -0.4, QY))
    # --- terminal: glazed ground floor, clad upper floor, deep roof overhang, blank sign panel
    tx0, tx1, tz0, tz1 = -10.0, 10.0, -7.0, 1.2
    H1, Ht = 4.4, 7.8
    b.mbox(m, tx0, tx1, QY, QY + Ht, tz0, tz1, skip=("+z",), top=None)
    b.zface(m, tx0, tx1, QY, QY + Ht, tz1, 1, "wall", b.wuv(tx0, tx1, QY, QY + Ht))
    b.zface(m, -1.9, 1.9, QY, QY + 3.2, tz1 + 0.03, 1, "detail", b.auv("door"))
    b.windows_z(m, tx0 + 0.4, -2.3, QY + 0.3, QY + 3.9, tz1, 1, step=3.5, kind="wide")
    b.windows_z(m, 2.3, tx1 - 0.4, QY + 0.3, QY + 3.9, tz1, 1, step=3.5, kind="wide")
    b.windows_z(m, tx0 + 0.4, tx1 - 0.4, QY + H1 + 0.4, QY + Ht - 0.7, tz1, 1, step=4.9, kind="wide")
    for xs, f in ((tx1, 1), (tx0, -1)): b.windows_x(m, tz0 + 1.2, tz1 - 1.2, QY + 0.3, QY + 3.9, xs, f, 3.4, "wide")
    b.mbox(m, tx0 - 1.2, tx1 + 1.2, QY + Ht, QY + Ht + 0.4, tz0 - 1.2, tz1 + 1.4, top="roof", bottom=True)
    b.zface(m, -3.5, 3.5, QY + Ht + 0.4, QY + Ht + 1.6, tz1 + 0.9, 1, "detail", [(236 / 512, 240 / 512)] * 4)       # blank sign panel
    b.mbox(m, 3.0, 6.0, QY + Ht + 0.4, QY + Ht + 1.6, -5.5, -2.5, top="roof")                                         # roof plant
    b.mbox(m, -6.0, -3.5, QY + Ht + 0.4, QY + Ht + 1.3, -4.5, -2.5, top="roof")
    # --- covered boarding bridge from the first floor out to the berth (bellows end is left open for the ferry)
    gx0, gx1 = -3.4, -1.2
    gz1 = sz + 1.6
    b.mbox(m, gx0, gx1, QY + 4.6, QY + 7.0, tz1, gz1, skip=("-z",), top="roof")
    for xs, f in ((gx1, 1), (gx0, -1)): b.windows_x(m, tz1 + 1.0, gz1 - 1.0, QY + 5.3, QY + 6.5, xs, f, 3.0, "wide")
    b.mbox(m, gx0 + 0.4, gx1 - 0.4, QY, QY + 4.6, sz - 1.8, sz - 0.8, top=None)                                       # support column
    # --- SPOM facade: bay piers, transom beam, sill, blue fascia stripe, parapet cap
    Wt = tx1 - tx0
    def paint(a):
        for xc in (0.0, 7.5, 11.9, Wt - 0.4):
            a(xc, xc + 0.4, 0.0, H1 + 0.6, 0.0, 0.34)
        a(7.5, 11.9, 3.25, 3.5, 0.0, 0.5, "dark"); a(7.5, 7.7, 0.0, 3.5, 0.0, 0.5, "dark"); a(11.7, 11.9, 0.0, 3.5, 0.0, 0.5, "dark")   # door portal
        a(0.0, Wt, H1 - 0.05, H1 + 0.45, 0.0, 0.5, "dark")                                                          # transom beam
        a(0.0, Wt, H1 + 0.45, H1 + 0.6, 0.0, 0.62)                                                                  # sill ledge
        for (sa, sb) in b.win_spans(0.4, Wt - 0.4, 4.9):
            a(sa - 0.1, sa - 0.04, H1 + 0.6, Ht - 0.4, 0.0, 0.3); a(sb + 0.04, sb + 0.1, H1 + 0.6, Ht - 0.4, 0.0, 0.3)
        a(0.0, Wt, Ht - 0.35, Ht, 0.0, 0.5, "accent")                                                              # blue fascia stripe
    vol = b.spom_volume(m, "front", Wt, Ht + 0.2, (tx0, QY, tz1), paint, wallc, accent)
    tex = {"base": ferry_ground(bx0, bx1, bz0, sz), "face": quay_face_tex(), "wall": b.wall_tex(wallc), "roof": b.roof_tex(),
           "detail": b.detail_tex(accent)}
    prev = []
    quay_relief(m, tex, prev, bx0, bx1, sz, 4.5, 1)
    return m, tex, [vol], [], sz, prev


# ---------------------------------------------------------------- cargo port
def cargo(wallc=(196, 200, 202), accent=(200, 64, 47)):
    W, D = 24 * CELL, 14 * CELL
    sz = D * (0.5 - 0.34)
    m = new_mesh()
    bx0, bx1, bz0 = -92.0, 92.0, -52.0
    rails = (sz - 4.4, sz - 18.4)                                      # sea-side and land-side crane rail
    # container stacks: two rows of five blocks, each four bays (6.1 m) of different height so the blocks step like a real yard
    blocks, rows = [], ((-9.0, -18.8), (-26.0, -35.8))
    pat = [(4, 3, 3, 1), (2, 3, 4, 4), (3, 2, 2, 3), (4, 4, 3, 2), (1, 2, 3, 3), (3, 4, 4, 2), (2, 2, 3, 4), (4, 3, 1, 2), (3, 3, 2, 2), (4, 4, 4, 3)]
    k = 0
    for ri, (za, zb) in enumerate(rows):
        for xc in (-68, -34, 0, 34, 68):
            hs = pat[k]; k += 1
            x0 = xc - 12.2
            blocks.append((x0, x0 + 24.4, zb, za))
            rr = np.random.default_rng(900 + k)
            cols, prev = [], -1
            for _ in range(4):                                                  # a colour per bay column, never the same twice in a row
                ci = int(rr.integers(0, 8))
                while ci == prev: ci = int(rr.integers(0, 8))
                cols.append(ci); prev = ci
            for i, hh in enumerate(hs):
                xa, xb = x0 + 6.1 * i, x0 + 6.1 * (i + 1)
                u0, u1 = (cols[i] + 0.03) / 8.0, (cols[i] + 0.97) / 8.0
                top = QY + 2.6 * hh
                b.zface(m, xa, xb, QY, top, za, 1, "cont", [(u0, hh), (u1, hh), (u1, 0), (u0, 0)])
                b.zface(m, xa, xb, QY, top, zb, -1, "cont", [(u1, hh), (u0, hh), (u0, 0), (u1, 0)])
                m.quad("conttop", (xa, top, za), (xb, top, za), (xb, top, zb), (xa, top, zb), [(u0, 1.0), (u1, 1.0), (u1, 0.0), (u0, 0.0)])
                left = hs[i - 1] if i else 0
                right = hs[i + 1] if i < 3 else 0
                if hh > left: b.xface(m, zb, za, QY + 2.6 * left, top, xa, -1, "contend", [(u0, hh - left), (u1, hh - left), (u1, 0), (u0, 0)])
                if hh > right: b.xface(m, zb, za, QY + 2.6 * right, top, xb, 1, "contend", [(u1, hh - right), (u0, hh - right), (u0, 0), (u1, 0)])
    m.quad("base", (bx0, QY, sz), (bx1, QY, sz), (bx1, QY, bz0), (bx0, QY, bz0), [(0, 1), (1, 1), (1, 0), (0, 0)])
    uf = lambda x: x / 8.0
    b.zface(m, bx0, bx1, QY - 5.0, QY, sz, 1, "face", [(uf(bx0), 1), (uf(bx1), 1), (uf(bx1), 0), (uf(bx0), 0)])
    b.zface(m, bx0, bx1, -0.4, QY, bz0, -1, "wall", b.wuv(bx1, bx0, -0.4, QY))
    b.xface(m, bz0, sz, -0.4, QY, bx0, -1, "wall", b.wuv(bz0, sz, -0.4, QY))
    b.xface(m, bz0, sz, -0.4, QY, bx1, 1, "wall", b.wuv(sz, bz0, -0.4, QY))
    # --- STS gantry cranes: relief leg frames + relief truss boom (SPOM) around real cores; the preview draws the relief as cutouts
    vols = []
    zs, zl = sz - 4.4, sz - 18.4                                          # sea leg, land leg
    GX = 4.6                                                              # half the portal width
    LEG_Z0, LEG_Z1, LEG_H = zl - 3.0, zs + 3.0, 34.5
    BM_Z0, BM_Z1, BM_Y0, BM_H = -8.0, 36.0, 28.0, 6.5
    legs = {mi: Sheet("craneLeg" + "AB"[mi], bool(mi), 0, LEG_Z1 if mi else LEG_Z0, LEG_Z1 - LEG_Z0, LEG_H) for mi in (0, 1)}
    booms = {mi: Sheet("craneBoom" + "AB"[mi], bool(mi), 0, BM_Z1 if mi else BM_Z0, BM_Z1 - BM_Z0, BM_H) for mi in (0, 1)}
    for sh in legs.values():
        sh.add(zs - 0.7, zs + 0.7, 0.0, 28.0, 0.0, 1.0); sh.add(zl - 0.7, zl + 0.7, 0.0, 28.0, 0.0, 1.0)             # legs
        sh.add(zs - 1.4, zs + 1.4, 0.0, 0.8, 0.0, 1.2, "dark"); sh.add(zl - 1.4, zl + 1.4, 0.0, 0.8, 0.0, 1.2, "dark")  # bogies
        sh.line(zs, 1.2, zl, 26.8, 0.45, 0.0, 0.7); sh.line(zl, 1.2, zs, 26.8, 0.45, 0.0, 0.7)                       # X bracing
        for yy in (5.0, 10.0, 15.0, 20.0, 25.0): sh.add(zl, zs, yy, yy + 0.4, 0.0, 0.7)                              # struts
        sh.add(zl - 0.7, zs + 0.7, 27.6, 28.8, 0.0, 1.1)                                                              # portal girder
        sh.line(zs, 28.8, zs - 4.0, 34.0, 0.6, 0.0, 0.9); sh.line(zl, 28.8, zs - 4.0, 34.0, 0.6, 0.0, 0.9)           # A-frame
    for sh in booms.values():
        sh.add(BM_Z0, BM_Z1, 0.55, 0.95, 0.0, 0.5, "white"); sh.add(BM_Z0, BM_Z1, 2.35, 2.75, 0.0, 0.5, "white")     # chords
        zz = BM_Z0
        while zz < BM_Z1 - 1.6:                                                                                       # Warren truss
            sh.line(zz, 0.75, zz + 0.8, 2.55, 0.2, 0.0, 0.4, "white"); sh.line(zz + 0.8, 2.55, zz + 1.6, 0.75, 0.2, 0.0, 0.4, "white")
            zz += 1.6
        sh.line(zs - 4.0, 6.0, BM_Z1, 2.6, 0.16, 0.0, 0.25, "dark")                                                   # forestay to the boom tip
        sh.line(zs - 4.0, 6.0, BM_Z0 + 1.0, 2.4, 0.16, 0.0, 0.25, "dark")                                             # backstay
    FW = 2 * GX + 2.0                                                    # front frame width: the two leg boxes and the portal between them
    front = Sheet("craneFront", False, 0, 0, FW, LEG_H, axis="z")
    front.add(0.0, 1.0, 0.0, 28.0, 0.0, 1.2); front.add(FW - 1.0, FW, 0.0, 28.0, 0.0, 1.2)                           # legs seen end-on
    front.add(0.0, 1.0, 0.0, 0.8, 0.0, 1.2, "dark"); front.add(FW - 1.0, FW, 0.0, 0.8, 0.0, 1.2, "dark")
    front.add(0.0, FW, 27.6, 28.8, 0.0, 1.1)                                                                        # portal girder
    for yy in (5.0, 10.0, 15.0, 20.0, 25.0): front.add(1.0, FW - 1.0, yy, yy + 0.35, 0.0, 0.6)
    front.line(1.0, 1.2, FW - 1.0, 26.8, 0.35, 0.0, 0.6); front.line(FW - 1.0, 1.2, 1.0, 26.8, 0.35, 0.0, 0.6)
    front.line(0.5, 28.8, FW / 2, 34.0, 0.6, 0.0, 0.9); front.line(FW - 0.5, 28.8, FW / 2, 34.0, 0.6, 0.0, 0.9)    # A-frame seen from the water
    prev_sheets = []
    for cx in (-30.0, 0.0, 30.0):
        prev_sheets.append((front, QY, front.place(zs - 0.6, cx - GX - 1.0, False))); front.emit(m)                  # water side, faces +z
        prev_sheets.append((front, QY, front.place(zl + 0.6, cx + GX + 1.0, True))); front.emit(m)                   # land side, faces -z
        for mi in (0, 1):
            snap = legs[mi].place(cx + GX if mi else cx - GX, LEG_Z1 if mi else LEG_Z0, bool(mi)); legs[mi].emit(m); prev_sheets.append((legs[mi], QY, snap))
            snap = booms[mi].place(cx + 0.7 if mi else cx - 0.7, BM_Z1 if mi else BM_Z0, bool(mi)); booms[mi].emit(m, QY + BM_Y0); prev_sheets.append((booms[mi], QY + BM_Y0, snap))
        for bx in ((cx - 0.7, cx + 0.7, 28.55, 30.35, BM_Z0, BM_Z1, "wall"),                                          # boom core
                   (cx - GX - 0.3, cx + GX + 0.3, 27.8, 29.0, zs - 0.8, zs + 0.8, "accent"),                         # sea portal beam
                   (cx - GX - 0.3, cx + GX + 0.3, 27.8, 29.0, zl - 0.8, zl + 0.8, "accent"),                         # land portal beam
                   (cx - 1.8, cx + 1.8, 30.4, 33.4, BM_Z0 + 2.0, BM_Z0 + 9.0, "accent")):                            # machinery house
            b.pbox(m, bx, (0, QY, 0))
    sheets = {sh.kind: sh for sh in list(legs.values()) + list(booms.values()) + [front]}
    prev_boxes = []
    # --- warehouse sheds and the office, facing the water
    sheds = [(-66.0, -22.0, -50.0, -39.0), (4.0, 48.0, -50.0, -39.0)]
    shed_h = 11.0
    for (x0, x1, z0, z1) in sheds:
        b.mbox(m, x0, x1, QY, QY + shed_h, z0, z1, wall="clad", skip=("+z",), top=None)
        b.zface(m, x0, x1, QY, QY + shed_h, z1, 1, "clad", b.wuv(x0, x1, QY, QY + shed_h))
        rise = 1.6; zm = (z0 + z1) / 2                                           # low gable roof, ridge along x, 0.6 m eaves
        e0, e1 = z0 - 0.6, z1 + 0.6
        m.quad("roof", (x0 - 0.4, QY + shed_h - 0.2, e1), (x1 + 0.4, QY + shed_h - 0.2, e1), (x1 + 0.4, QY + shed_h + rise, zm), (x0 - 0.4, QY + shed_h + rise, zm),
               [(0, 0), (6, 0), (6, 1.5), (0, 1.5)])
        m.quad("roof", (x1 + 0.4, QY + shed_h - 0.2, e0), (x0 - 0.4, QY + shed_h - 0.2, e0), (x0 - 0.4, QY + shed_h + rise, zm), (x1 + 0.4, QY + shed_h + rise, zm),
               [(0, 0), (6, 0), (6, 1.5), (0, 1.5)])
        m.tri("clad", (x1, QY + shed_h, z1), (x1, QY + shed_h, z0), (x1, QY + shed_h + rise - 0.2, zm), [(0, 0), (4, 0), (2, -0.5)])
        m.tri("clad", (x0, QY + shed_h, z0), (x0, QY + shed_h, z1), (x0, QY + shed_h + rise - 0.2, zm), [(0, 0), (4, 0), (2, -0.5)])
        for xa in np.arange(x0 + 3.0, x1 - 6.0, 10.0): b.zface(m, xa, xa + 6.0, QY, QY + 5.0, z1 + 0.03, 1, "detail", b.auv("garage"))
        b.zface(m, x0 + 2.0, x1 - 2.0, QY + 6.4, QY + shed_h - 1.2, z1 + 0.03, 1, "detail", b.auv("wide"))
    ox0, ox1, oz0, oz1, oh = 62.0, 84.0, -50.0, -36.0, 16.0
    b.mbox(m, ox0, ox1, QY, QY + oh, oz0, oz1, wall="clad", skip=("+z",), top="roof")
    b.zface(m, ox0, ox1, QY, QY + oh, oz1, 1, "clad", b.wuv(ox0, ox1, QY, QY + oh))
    for row in range(4): b.windows_z(m, ox0 + 0.5, ox1 - 0.5, QY + 1.0 + row * 3.7, QY + 3.1 + row * 3.7, oz1, 1, step=3.6, kind="win")
    b.zface(m, ox0 + 1.0, ox1 - 1.0, QY + oh - 0.9, QY + oh - 0.1, oz1 + 0.04, 1, "detail", [(236 / 512, 240 / 512)] * 4)  # blank sign
    def paint_shed(Wv):
        def paint(a):
            for xa in np.arange(3.0, Wv - 6.0, 10.0):
                a(xa - 0.35, xa + 0.05, 0.0, 5.3, 0.0, 0.4); a(xa + 6.0, xa + 6.35, 0.0, 5.3, 0.0, 0.4); a(xa - 0.35, xa + 6.35, 5.0, 5.4, 0.0, 0.5, "dark")
            a(0.0, Wv, 6.1, 6.4, 0.0, 0.5, "dark"); a(0.0, Wv, shed_h - 0.3, shed_h + 0.05, 0.0, 0.55, "accent"); a(0.0, Wv, shed_h + 0.3, shed_h + 0.5, 0.0, 0.4)
        return paint
    for i, (x0, x1, z0, z1) in enumerate(sheds):
        vols.append(b.spom_volume(m, f"shed{i}", x1 - x0, shed_h + 0.5, (x0, QY, z1), paint_shed(x1 - x0), wallc, accent))
    Wo = ox1 - ox0
    def paint_off(a):
        for row in range(4):
            for (sa, sb) in b.win_spans(0.5, Wo - 0.5, 3.6):
                y0 = 1.0 + row * 3.7
                a(sa - 0.1, sb + 0.1, y0 + 2.2, y0 + 2.3, 0.0, 0.6, "dark"); a(sa - 0.1, sb + 0.1, y0 - 0.12, y0, 0.0, 0.3)
            a(0.0, Wo, y0 + 2.7, y0 + 3.0, 0.0, 0.26)
        a(0.0, Wo, oh - 0.1, oh + 0.3, 0.0, 0.34, "accent"); a(0.0, Wo, oh + 0.3, oh + 0.5, 0.0, 0.3)
    vols.append(b.spom_volume(m, "office", Wo, oh + 0.5, (ox0, QY, oz1), paint_off, wallc, accent))
    tex = {"base": cargo_ground(bx0, bx1, bz0, sz, rails, blocks), "face": quay_face_tex(), "cont": container_tex(), "contend": container_end_tex(), "conttop": container_top_tex(), "clad": clad_tex((176, 184, 188)), "wall": b.wall_tex(wallc),
           "roof": b.roof_tex(), "detail": b.detail_tex(accent)}
    extra = []
    for kind, sh in sheets.items():
        tex["spom_" + sh.kind] = sh.L.encode()
        tex["sprev_" + sh.kind] = (sh.preview(), flat_normal())
    quay_relief(m, tex, prev_sheets, bx0, bx1, sz, 5.0, 2)
    return m, tex, vols, prev_boxes, sz, prev_sheets


MODELS = [
    ("HB_12x8_ferry_port_L1", (12, 8), ferry),
    ("HB_24x14_cargo_port_L1", (24, 14), cargo),
]


def main():
    os.makedirs(OUT, exist_ok=True)
    catalog = []
    for (name, cells, fn) in MODELS:
        m, tex, vols, prev_boxes, sz, prev_sheets = fn()
        for (kind, maps_, boxes, origin) in vols: tex["spom_" + kind] = maps_
        mats = {"detail": {"rough": 0.35, "metal": 0.1}, "base": {"rough": 0.95}, "face": {"rough": 0.95}, "cont": {"rough": 0.6, "metal": 0.2}, "conttop": {"rough": 0.6, "metal": 0.2}, "contend": {"rough": 0.6, "metal": 0.2}, "clad": {"rough": 0.55, "metal": 0.3}}
        for k in list(tex):
            if k.startswith("sprev_"): mats[k] = {"rough": 0.6, "blend": True, "alpha": 1.0}
        m.prims = {k: v for k, v in m.prims.items() if v}
        pm_tex = dict(tex)
        tex = {k: v for k, v in tex.items() if k in m.prims}
        p = os.path.join(OUT, name + ".glb")
        g.write_glb(p, m, tex, mats)
        tris = m.tri_count()
        spom_tris = sum(2 if c[3] == 4 else 1 for k, l in m.prims.items() if k.startswith("spom_") for c in l)
        pm = Mesh(); pm.prims = {k: list(v) for k, v in m.prims.items() if not k.startswith("spom_")}
        pm.prims.setdefault("detail", [])
        for (kind, maps_, boxes, origin) in vols:
            for bx in boxes: b.pbox(pm, bx, origin)
        for bx in prev_boxes: b.pbox(pm, bx, (0, 0, 0))
        for (sh, y0, snap) in prev_sheets:
            sh.place(*snap); sh.emit_preview(pm, y0)
        os.makedirs(os.path.join(OUT, "preview"), exist_ok=True)
        g.write_glb(os.path.join(OUT, "preview", name + "_spom_preview.glb"), pm, {k: v for k, v in pm_tex.items() if k in pm.prims}, mats)
        W, D = cells[0] * CELL, cells[1] * CELL
        catalog.append({"file": name + ".glb", "preview": "preview/" + name + "_spom_preview.glb", "name": name, "lot": list(cells), "lot_m": [W, D],
                        "shore_z": sz, "tris": tris, "spom_tris": spom_tris, "materials": len(m.prims), "kb": os.path.getsize(p) // 1024})
        print(f"{name}: {tris} tris ({spom_tris} are SPOM relief, {tris - spom_tris} real), {len(m.prims)} materials, {os.path.getsize(p) // 1024} KB")
    json.dump(catalog, open(os.path.join(BASE, "catalog.json"), "w"), indent=1)
    open(os.path.join(BASE, "preview.html"), "w", encoding="utf-8").write(PREVIEW)


PREVIEW = """<!doctype html><meta charset="utf-8"><title>Harbour: ferry port + cargo port</title>
<style>body{margin:0;background:#8aa0b4;font:13px sans-serif;overflow:hidden}#ui{position:fixed;top:8px;left:8px;background:#000b;color:#fff;padding:8px;border-radius:5px}
#lab{position:fixed;bottom:8px;left:8px;background:#000b;color:#fff;padding:6px 10px;border-radius:5px}</style>
<div id="ui"><label><input type="checkbox" id="wf"> wireframe</label> <button id="f0">Ferry port</button><button id="f1">Cargo port</button><button id="f2">All</button><br>SPOM relief is drawn as real boxes here; the engine renders it as relief. Water is at +z.</div><div id="lab"></div>
<script type="importmap">{"imports":{"three":"https://unpkg.com/three@0.160.0/build/three.module.js","three/addons/":"https://unpkg.com/three@0.160.0/examples/jsm/"}}</script>
<script type="module">
import * as THREE from 'three';import {OrbitControls} from 'three/addons/controls/OrbitControls.js';import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
const r=new THREE.WebGLRenderer({antialias:true,preserveDrawingBuffer:true});r.setSize(innerWidth,innerHeight);r.shadowMap.enabled=true;r.toneMapping=THREE.ACESFilmicToneMapping;document.body.appendChild(r.domElement);
const s=new THREE.Scene();s.background=new THREE.Color(0xa9c3d6);
const cam=new THREE.PerspectiveCamera(38,innerWidth/innerHeight,0.5,3000);cam.position.set(40,120,230);const oc=new OrbitControls(cam,r.domElement);oc.target.set(40,6,-10);
s.add(new THREE.HemisphereLight(0xdfe8ff,0x6b6a58,0.9));const sun=new THREE.DirectionalLight(0xfff0d8,2.4);sun.position.set(-120,200,140);sun.castShadow=true;
Object.assign(sun.shadow.camera,{left:-260,right:260,top:260,bottom:-260,far:900});sun.shadow.mapSize.set(2048,2048);sun.shadow.bias=-0.0004;s.add(sun);
const land=new THREE.Mesh(new THREE.PlaneGeometry(1400,600),new THREE.MeshStandardMaterial({color:0x6d7f52}));land.rotation.x=-Math.PI/2;land.position.set(0,-0.05,-300);land.receiveShadow=true;s.add(land);
const sea=new THREE.Mesh(new THREE.PlaneGeometry(1400,600),new THREE.MeshStandardMaterial({color:0x2f6f93,roughness:0.25}));sea.rotation.x=-Math.PI/2;sea.position.set(0,-4.2,300);s.add(sea);
const cat=await (await fetch('catalog.json')).json();const L=new GLTFLoader();const root=[];let x=-60;
for(const e of cat){const px=x+e.lot_m[0]/2;x+=e.lot_m[0]+30;L.load('glb/'+(e.preview||e.file),g=>{g.scene.traverse(o=>{if(o.isMesh){o.castShadow=o.receiveShadow=true;if(o.material.transparent){o.castShadow=false;o.receiveShadow=false}}});g.scene.position.set(px,0,-e.shore_z);s.add(g.scene);root.push(g.scene);});}
document.getElementById('lab').innerHTML=cat.map(e=>`<b>${e.name}</b> ${e.lot.join('x')} (${e.lot_m.join('x')} m) &mdash; ${e.tris} tris (${e.spom_tris} SPOM), ${e.materials} mats, ${e.kb} KB`).join('<br>');
const foc=[[[-12,22,62],[-12,4,-8]],[[162,50,120],[162,10,-20]],[[75,110,260],[75,6,-10]]];
foc.forEach((v,i)=>document.getElementById('f'+i).onclick=()=>{cam.position.set(...v[0]);oc.target.set(...v[1])});
document.getElementById('f0').click();
document.getElementById('wf').onchange=e=>root.forEach(g=>g.traverse(o=>{if(o.isMesh)o.material.wireframe=e.target.checked}));
r.setAnimationLoop(()=>{const w=innerWidth,h=innerHeight;if(w&&(r.domElement.width!==w||r.domElement.height!==h)){r.setSize(w,h);cam.aspect=w/h;cam.updateProjectionMatrix();}oc.update();r.render(s,cam)});
</script>"""

if __name__ == "__main__":
    main()
