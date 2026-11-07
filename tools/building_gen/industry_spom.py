"""SPOM volume reliefs for the specialized-industry vehicles and props (SpomPS in src/shaders/mesh.hlsl).

Every object here is drawn as one or two 12-triangle boxes; its whole shape lives in a two-layer relief:
  albedo.rgb colour, albedo.a = where solid ENDS / DEPTH; normal.rgb fine normal, normal.a = where solid STARTS / DEPTH.
Coordinates: a = metres along the volume (x, along T = cross(up, N)), h = metres up, s = metres across. A volume covers
a0 .. a0 + L, 0 .. H, and s from sIn to sIn + DEPTH (z = s - sIn out from its inner plane along N).
Each texture is also written mirrored (<name>_f) for the volume on the far side, whose T runs the other way.

    python tools/building_gen/industry_spom.py   -> assets/industry/spom/<name>_a.png, _n.png (+ _f), src/city/industry_spom.h
"""
import math
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

DEPTH = 1.2
ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
OUT = os.path.join(ROOT, 'assets', 'industry', 'spom')
rng = np.random.default_rng(5)


def C(hexv):
    return np.array([(hexv >> 16) & 255, (hexv >> 8) & 255, hexv & 255], np.float32) / 255.0


class Vol:
    def __init__(self, name, a0, L, H, sIn, ppm=None):
        self.name, self.a0, self.L, self.H, self.sIn = name, a0, L, H, sIn
        ppm = ppm or min(96.0, 512.0 / max(L, H))
        ppm = min(ppm, 1600.0 / max(L, H))
        self.W_, self.H_ = max(8, int(round(L * ppm))), max(8, int(round(H * ppm)))
        self.A = a0 + (np.arange(self.W_) + 0.5) / self.W_ * L                 # columns: a
        self.Hh = (1.0 - (np.arange(self.H_) + 0.5) / self.H_) * H             # rows: h (row 0 = top)
        self.AA, self.HH = np.meshgrid(self.A, self.Hh)
        self.s0 = np.full((self.H_, self.W_), np.inf, np.float32)
        self.s1 = np.full((self.H_, self.W_), -np.inf, np.float32)
        self.col = np.zeros((self.H_, self.W_, 3), np.float32)

    # ---- merge an interval [zs, ze] (in s) where mask; colour where it reaches furthest out on either side
    def put(self, mask, zs, ze, col):
        zs = np.broadcast_to(np.asarray(zs, np.float32), mask.shape)
        ze = np.broadcast_to(np.asarray(ze, np.float32), mask.shape)
        m = mask & (ze > zs + 1e-4)
        if not m.any():
            return
        col = np.asarray(col, np.float32)
        colv = col if col.ndim == 3 else np.broadcast_to(col, self.col.shape)
        paint = m & ((ze >= self.s1 - 1e-4) | (zs <= self.s0 + 1e-4))
        self.col[paint] = colv[paint]
        self.s0[m] = np.minimum(self.s0[m], zs[m])
        self.s1[m] = np.maximum(self.s1[m], ze[m])

    def box(self, a0, a1, h0, h1, s0, s1, col):
        m = (self.AA >= a0) & (self.AA <= a1) & (self.HH >= h0) & (self.HH <= h1)
        self.put(m, s0, s1, col)

    def poly(self, pts, s0, s1, col):
        img = Image.new('L', (self.W_, self.H_), 0)
        px = [((a - self.a0) / self.L * self.W_, (1 - h / self.H) * self.H_) for a, h in pts]
        ImageDraw.Draw(img).polygon(px, fill=255)
        self.put(np.asarray(img) > 127, s0, s1, col)

    def disc(self, a, h, r, s0, s1, col, rim=None, rimk=0.58):
        d = np.hypot(self.AA - a, self.HH - h)
        m = d <= r
        c = np.broadcast_to(np.asarray(col, np.float32), self.col.shape).copy()
        if rim is not None:
            c[d <= r * rimk] = rim
            c[(d <= r * rimk) & (d >= r * (rimk - 0.06))] = np.asarray(rim) * 0.6
        self.put(m, s0, s1, c)

    def rod(self, a0, h0, sa, a1, h1, sb, r, col):
        """a round bar from (a0, h0, sa) to (a1, h1, sb), radius r: its cross-section seen from the side."""
        da, dh = a1 - a0, h1 - h0
        l2 = max(da * da + dh * dh, 1e-9)
        t = np.clip(((self.AA - a0) * da + (self.HH - h0) * dh) / l2, 0, 1)
        d = np.hypot(self.AA - (a0 + t * da), self.HH - (h0 + t * dh))
        m = d <= r
        sc = sa + t * (sb - sa)
        w = np.sqrt(np.maximum(r * r - d * d, 0))
        self.put(m, sc - w, sc + w, col)

    def ell(self, a, h, s, ra, rh, rs, col):
        q = 1 - ((self.AA - a) / ra) ** 2 - ((self.HH - h) / rh) ** 2
        m = q > 0
        w = rs * np.sqrt(np.maximum(q, 0))
        self.put(m, s - w, s + w, col)

    def band(self, a0, a1, h0, h1, fs0, fs1, col):
        """per-texel interval from functions of (a, h) arrays: ramps, dividers."""
        m = (self.AA >= a0) & (self.AA <= a1) & (self.HH >= h0) & (self.HH <= h1)
        self.put(m, fs0(self.AA, self.HH), fs1(self.AA, self.HH), col)

    def tint(self, fn):
        """recolour solid texels: fn(a, h, col) -> col."""
        m = self.s1 > self.s0
        self.col[m] = fn(self.AA[m], self.HH[m], self.col[m])

    def save(self):
        os.makedirs(OUT, exist_ok=True)
        solid = self.s1 > self.s0
        z0 = np.where(solid, np.clip(self.s0 - self.sIn, 0, DEPTH), 0)
        z1 = np.where(solid, np.clip(self.s1 - self.sIn, 0, DEPTH), 0)
        empty = z1 <= z0 + 0.004
        z0[empty] = 0; z1[empty] = 0
        n = 0.93 + 0.07 * rng.random((self.H_, self.W_, 1)).astype(np.float32)
        col = np.clip(self.col * n, 0, 1)
        # baked occlusion: texels sunk below their neighbours' outer surface, and a dark seam where the depth jumps
        e = z1 / DEPTH
        blur = np.asarray(Image.fromarray((e * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(3)), np.float32) / 255
        col *= np.clip(1 - np.maximum(blur - e, 0) * 2.5, 0.55, 1)[..., None]
        for flip in (False, True):
            c, a0, a1 = col, z1, z0
            if flip:
                c, a0, a1 = c[:, ::-1], a0[:, ::-1], a1[:, ::-1]
            ee = a0 / DEPTH
            dx = (np.roll(ee, -1, 1) - np.roll(ee, 1, 1)) * 0.5
            dy = (np.roll(ee, -1, 0) - np.roll(ee, 1, 0)) * 0.5
            nrm = np.stack([-dx * 2, dy * 2, np.ones_like(ee)], -1)
            nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
            # depth to 8 bits with half a step of dither: smooth shapes would otherwise shade in contour lines
            dz = lambda z: np.clip(np.where(z > 0, z / DEPTH * 255 + rng.uniform(-0.5, 0.5, z.shape), 0) + 0.5, 0, 255).astype(np.uint8)
            alb = np.dstack([(c * 255 + 0.5).astype(np.uint8), dz(a0)])
            nor = np.dstack([((nrm * 0.5 + 0.5) * 255).astype(np.uint8), dz(a1)])
            tag = self.name + ('_f' if flip else '')
            Image.fromarray(alb, 'RGBA').save(os.path.join(OUT, tag + '_a.png'))
            Image.fromarray(nor, 'RGBA').save(os.path.join(OUT, tag + '_n.png'))
        print(self.name, self.W_, 'x', self.H_)
        VOLS.append(self)


VOLS = []
TYRE, RIM, DARK, GLASS, STEEL = C(0x1A1A1C), C(0x8A8C8F), C(0x29292B), C(0x22303A), C(0x6E7174)


def cab(v, a0, a1, h0, h1, s0, s1, rake, frame, roof):
    """glass cab: tinted panes, corner pillars standing proud, roof cap (in the side volume)."""
    v.poly([(a0, h0), (a1, h0), (a1 - rake, h1), (a0, h1)], s0 + 0.04, s1 - 0.04, GLASS)
    v.tint(lambda A, Hh, c: np.where(((Hh > h0) & (Hh < h1) & (A > a0) & (A < a1))[:, None], c * (0.85 + 0.5 * np.clip((Hh[:, None] - h0) / (h1 - h0), 0, 1)), c))
    for a in (a0, a1):
        v.rod(a, h0, s1 - 0.05, a - (rake if a == a1 else 0), h1, s1 - 0.05, 0.06, frame)
    v.box(a0 - 0.1, a1 - rake + 0.1, h1, h1 + 0.14, s0, s1 + 0.06, roof)


def tyre(v, a, h, r, s0, s1, outer=True):
    v.disc(a, h, r, s0, s1, TYRE, RIM if outer else None)
    # tread lugs on the rolling face, seen from above / ahead
    v.tint(lambda A, Hh, c: np.where(((np.hypot(A - a, Hh - h) > r * 0.9) & (np.hypot(A - a, Hh - h) <= r) & (np.sin(np.arctan2(Hh - h, A - a) * 22) > 0.3))[:, None], c * 0.6, c))


# ============================================================================== vehicles: side volumes (left = +s; _f = right)
# ---- ore haul truck (40 t class): 9.6 m long, 5.2 m wide, cab on the left of the deck
YEL, YELD = C(0xC79A2C), C(0x9A7522)
def haul(left):
    v = Vol('haul_' + ('l' if left else 'r'), -4.8, 9.7, 5.9, 1.4)
    tyre(v, -2.5, 1.3, 1.3, 1.45, 2.55)
    tyre(v, 2.9, 1.3, 1.3, 1.45, 2.25)
    v.box(-4.0, 3.6, 1.0, 1.9, 1.4, 1.6, DARK)                                    # frame rail
    v.poly([(-4.4, 2.6), (-3.4, 1.9), (2.3, 2.2), (2.8, 3.6), (3.9, 5.6), (-4.6, 4.4)], 1.4, 2.5, YEL * 0.92)  # dump body side
    for a in np.arange(-3.8, 2.4, 1.2):
        v.rod(a, 2.1, 2.55, a + 0.1, 4.3, 2.55, 0.07, YELD)                        # body ribs
    v.box(-4.6, 3.9, 4.25, 4.45, 1.4, 2.6, YELD)                                  # top rail
    v.box(2.4, 4.4, 3.3, 3.5, 1.4, 2.6, YEL)                                      # deck
    for a in (2.5, 3.4, 4.3):
        v.rod(a, 3.5, 2.55, a, 4.5, 2.55, 0.03, YELD)                             # handrail posts
    v.rod(2.5, 4.5, 2.55, 4.3, 4.5, 2.55, 0.03, YELD)
    v.box(4.2, 4.4, 1.0, 3.3, 1.4, 1.6, YEL)                                      # front fender face
    v.box(4.35, 4.5, 2.4, 2.7, 1.4, 2.0, C(0xE6E2D0))                             # headlights
    v.box(-4.7, -4.4, 1.2, 2.0, 1.4, 1.6, C(0x7A1E18))                            # tail lights
    if left:
        cab(v, 2.5, 4.0, 3.5, 5.3, 1.4, 2.4, 0.35, DARK, YEL)
        v.rod(4.35, 4.2, 2.45, 4.35, 4.7, 2.6, 0.05, DARK)                         # mirror
        v.box(4.3, 4.4, 4.5, 4.9, 2.45, 2.6, DARK)
    else:
        v.rod(2.7, 3.4, 1.6, 2.7, 5.4, 1.6, 0.16, C(0x333333))                     # exhaust stack
    v.save()
haul(True); haul(False)

# ---- combine: body side (left), the unloading auger folded along it; the cab sits left of centre
CRED, CREDD = C(0x8C2218), C(0x5E1912)
def combine(left):
    v = Vol('combine_' + ('l' if left else 'r'), -4.7, 7.8, 5.4, 0.85)
    v.poly([(-4.3, 1.0), (-4.4, 2.7), (-3.6, 3.5), (-0.6, 3.6), (1.1, 3.4), (1.6, 1.7), (1.1, 1.0), (-1.0, 0.75)], 0.85, 1.35, CRED)
    v.box(-4.1, 1.1, 2.05, 2.15, 0.85, 1.38, C(0xD2CEC2))                         # white stripe
    for a in np.arange(-3.8, 0.8, 0.9):
        v.box(a, a + 0.5, 1.2, 1.8, 1.30, 1.37, CREDD)                            # louvred side panels
    v.band(-3.0, 0.6, 3.5, 4.5, lambda A, Hh: 0.85 + 0 * A, lambda A, Hh: 1.4 + (Hh - 3.5) * 0.35, CRED * 1.05)  # tank flare
    v.box(-3.1, 0.7, 4.5, 4.85, 0.85, 1.95, DARK)                                 # tank extension
    tyre(v, 0.6, 0.9, 0.9, 1.4, 2.05)
    tyre(v, -3.3, 0.55, 0.55, 1.1, 1.5)
    if left:
        v.rod(-2.8, 4.0, 1.6, 2.0, 4.35, 1.65, 0.13, CRED)                        # unloading auger
        cab(v, 0.85, 2.55, 3.45, 5.05, 0.85, 1.35, 0.3, DARK, C(0xD9D6CE))
        v.rod(2.3, 4.6, 1.3, 2.4, 4.6, 1.75, 0.03, DARK)                           # mirror arm
    else:
        v.rod(1.3, 0.5, 0.9, 1.6, 3.4, 0.9, 0.17, DARK)                            # ladder
        v.rod(-3.2, 3.5, 0.9, -3.2, 4.9, 0.9, 0.08, DARK)                          # exhaust
    v.save()
combine(True); combine(False)
# header front volume: N = forward, a = across (8 m), s = along the machine from 3.55 to 4.75
v = Vol('header', -3.9, 7.8, 2.0, 3.55)
v.box(-3.9, 3.9, 0.3, 0.45, 3.55, 4.7, STEEL * 0.8)                                # floor
v.box(-3.9, 3.9, 0.3, 1.3, 3.55, 3.75, CRED * 0.8)                                 # back sheet
v.rod(-3.9, 0.7, 3.95, 3.9, 0.7, 3.95, 0.22, C(0x8E8F90))                          # cross auger
for a in np.arange(-3.6, 3.7, 0.3):
    v.box(a, a + 0.05, 0.3, 0.38, 4.6, 4.75, STEEL)                                # knife guards
for a in (-3.9, 3.85):
    v.band(a, a + 0.08, 0.3, 1.35, lambda A, Hh: 3.55 + 0 * A, lambda A, Hh: 4.75 - (Hh - 0.3) * 0.9, C(0x707377))  # dividers
v.save()

# ---- tractor (row-crop, 2.6 m wide): almost all of it is in the two side volumes
GRN, GRND = C(0x2F4A2A), C(0x1F321C)
def tractor(left):
    v = Vol('tractor_' + ('l' if left else 'r'), -2.4, 5.8, 3.4, 0.12)
    v.box(-1.7, 2.8, 0.55, 1.0, 0.12, 0.42, DARK)                                  # frame
    v.poly([(-0.1, 1.0), (2.6, 1.0), (2.75, 1.45), (2.55, 1.85), (-0.1, 2.05)], 0.12, 0.48, GRN)  # hood
    for a in np.arange(0.4, 2.4, 0.18):
        v.box(a, a + 0.08, 1.25, 1.75, 0.46, 0.5, GRND)                            # hood side louvres
    v.box(2.75, 2.8, 1.08, 1.7, 0.12, 0.38, DARK)                                  # grille
    v.box(2.8, 3.3, 0.6, 1.05, 0.12, 0.5, DARK)                                    # weights
    v.box(-1.75, -0.05, 1.0, 1.75, 0.12, 0.75, DARK)                               # cab base
    cab(v, -1.75, -0.05, 1.75, 3.1, 0.12, 0.82, 0.12, DARK, GRN)
    v.poly([(-2.15, 1.55), (-1.7, 2.05), (-0.3, 2.05), (0.15, 1.55), (-0.1, 1.55), (-0.45, 1.92), (-1.55, 1.92), (-1.9, 1.55)], 0.62, 1.32, GRN)  # fender
    tyre(v, -1.0, 0.95, 0.95, 0.65, 1.3)
    tyre(v, 2.25, 0.6, 0.6, 0.55, 0.98)
    if left:
        v.rod(0.25, 1.9, 0.42, 0.25, 3.35, 0.42, 0.06, DARK)                       # exhaust
    v.rod(-1.6, 0.9, 0.35, -2.4, 0.6, 0.35, 0.05, DARK)                           # hitch arm
    v.box(2.7, 2.78, 1.5, 1.65, 0.3, 0.45, C(0xE6E2D0))                            # light
    v.save()
tractor(True); tractor(False)

# ---- landfill compactor: spiked steel drums, blade, cab
CY, CYD = C(0xB98E2A), C(0x8A6A20)
def compactor(left):
    v = Vol('compactor_' + ('l' if left else 'r'), -3.9, 8.2, 4.4, 0.8)
    v.poly([(-3.2, 1.0), (-3.4, 2.6), (-1.4, 2.8), (1.2, 2.4), (1.6, 1.2), (-1.0, 0.9)], 0.8, 1.1, CY)
    v.box(-3.3, -2.5, 1.4, 2.4, 1.08, 1.12, DARK)                                  # radiator louvre
    cab(v, -1.2, 0.7, 2.6, 4.0, 0.8, 0.8, 0.15, DARK, CY)
    for a in (-2.6, 2.2):
        v.disc(a, 0.95, 0.95, 1.0, 1.9, C(0x5F6264), C(0x4A4C4E), 0.4)
        for k in range(10):                                                      # cleats round the drum
            t = k / 10 * 2 * math.pi
            v.box(a + math.cos(t) * 1.0 - 0.07, a + math.cos(t) * 1.0 + 0.07, 0.95 + math.sin(t) * 1.0 - 0.07, 0.95 + math.sin(t) * 1.0 + 0.07, 1.0, 1.9, C(0x707375))
    v.poly([(3.3, 0.2), (3.6, 0.2), (3.9, 2.1), (3.4, 2.2)], 0.8, 2.0, CYD)       # blade end
    v.rod(1.2, 1.4, 0.95, 3.4, 1.0, 0.95, 0.12, CYD)                              # push arm
    if left:
        v.rod(-2.2, 2.6, 0.8, -2.2, 3.6, 0.8, 0.08, C(0x262626))
    v.save()
compactor(True); compactor(False)

# ---- mining shovel (parked): tracks and house side; boom and stick stay in the core
def excavator(left):
    v = Vol('excavator_' + ('l' if left else 'r'), -2.7, 4.8, 3.5, 0.55)
    v.poly([(-2.4, 0.3), (-2.1, 0), (2.1, 0), (2.4, 0.3), (2.1, 0.95), (-2.1, 0.95)], 0.85, 1.75, DARK)  # track
    for a in np.arange(-2.0, 2.1, 0.3):
        v.box(a, a + 0.12, 0.0, 0.95, 1.7, 1.77, C(0x3A3A3C))                     # track shoes
    for a in (-1.6, -0.6, 0.4, 1.4):
        v.disc(a, 0.4, 0.22, 1.3, 1.78, C(0x4A4A4C))                              # rollers
    v.poly([(-2.2, 1.1), (-2.3, 2.4), (0.0, 2.5), (0.6, 2.3), (0.6, 1.1)], 0.55, 1.25, YEL)  # house side
    v.box(-2.25, -1.4, 1.3, 2.2, 1.2, 1.27, DARK)                                  # engine grille
    if not left:
        cab(v, 0.6, 2.0, 1.1, 3.3, 0.55, 1.2, 0.1, DARK, YEL)
    v.save()
excavator(True); excavator(False)

# ---- forwarder (loaded): bogies, stakes, the log bunk's side
ORG = C(0xA8702A)
def forwarder(left):
    v = Vol('forwarder_' + ('l' if left else 'r'), -4.3, 8.3, 4.4, 0.5)
    v.poly([(1.4, 1.0), (3.6, 1.0), (3.7, 2.0), (2.4, 2.3), (1.4, 2.2)], 0.5, 1.1, ORG)
    cab(v, 1.5, 2.8, 2.2, 3.6, 0.5, 0.85, 0.1, DARK, ORG)
    v.box(-4.0, 1.2, 1.0, 1.4, 0.5, 1.1, ORG * 0.85)                               # bunk frame
    for a in (-3.6, -1.4, 0.8):
        v.rod(a, 1.4, 1.1, a, 2.9, 1.15, 0.06, DARK)                              # stakes
    LOG, END = C(0x5A4430), C(0xB89A6C)
    for r, (h, s) in enumerate([(1.62, 0.75), (1.62, 0.33), (1.98, 0.55), (2.34, 0.75), (2.34, 0.33)]):
        v.rod(-3.95, h, s, 1.0, h + 0.02, s, 0.19, LOG)                           # logs
        v.disc(-3.95, h, 0.19, s - 0.19, s + 0.19, END)                           # cut ends
    for a in (-2.6, -0.4, 2.6):
        tyre(v, a, 0.7, 0.7, 1.1, 1.7)
    v.save()
forwarder(True); forwarder(False)

# ============================================================================== cows: one volume each, the whole animal
# Sculpted as a 3D signed-distance field (blended muscle masses, jointed legs, head, dewlap, udder, tail) and ray-cast across
# the volume into the two-layer relief: per texel, the first and last s where the body is solid. Colour comes from the surface
# point the eye sees from either flank: breed markings in 3D noise, fur grain, baked occlusion from the field itself.
# Legs are staggered (a walking stance) so no two share a texel column seen from the side: one layer per texel.


import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cow_sculpt import _smin, _ell, _cap, _noise3, cow_sdf


def cow(name, breed, graze):
    v = Vol(name, -0.18, 2.95, 1.75, -0.6, ppm=230)
    if os.environ.get('SPOM_SKIP_COWS'):  # keep the table entry, skip the 5-minute bake (textures already on disk)
        VOLS.append(v)
        return
    A, Hh = v.AA, v.HH
    S = np.linspace(-0.45, 0.45, 91, dtype=np.float32)                              # cast across the animal, 1 cm steps
    s0 = np.full(A.shape, np.inf, np.float32)
    s1 = np.full(A.shape, -np.inf, np.float32)
    has_udder = breed == 'hol'
    for s in S:
        d, part, udder = cow_sdf((A, Hh, np.full_like(A, s)), graze)
        if has_udder:
            d = _smin(d, udder, 0.04)
        inside = d <= 0
        s0 = np.where(inside & (s < s0), s, s0)
        s1 = np.where(inside & (s > s1), s, s1)
    solid = s1 >= s0
    s0 = np.where(solid, s0 - 0.005, s0)
    s1 = np.where(solid, s1 + 0.005, s1)
    # the surface the eye sees on the near flank: colour it there
    Ps = (A, Hh, np.where(solid, s1, 0))
    d, part, udder = cow_sdf(Ps, graze)
    if has_udder:
        part = np.where(udder <= np.minimum(d, 0) + 0.012, 4, part)
    # occlusion from the field: how far the surface sits inside its neighbourhood (creases, under the belly, between legs)
    occ = np.ones(A.shape, np.float32)
    for dl in (0.04, 0.09, 0.16):
        nd, _, _ = cow_sdf((A, Hh + dl * 0.3, Ps[2] + dl), graze)
        occ *= np.clip(0.55 + 0.45 * nd / dl, 0.45, 1.0)
    occ *= np.clip(0.7 + 0.3 * Hh / 1.0, 0.7, 1.0)                                  # ground bounce dims the underside
    fur = 1 + 0.025 * np.sin(A * 40 + np.sin(Hh * 13) * 2) * np.sin(Hh * 55 + A * 7)  # hair grain, along the body (coarse: no moire)
    P3 = (A, Hh, Ps[2])
    if breed == 'hol':
        base = np.broadcast_to(C(0xDAD5CA), A.shape + (3,)).copy()
        n = _noise3(P3, 5.0, 11) + 0.45 * _noise3(P3, 11.0, 12)
        black = (n > 0.12) & (part != 4)
        base[black] = C(0x151413)
        hd = (part == 1) | (part == 8)
        base[hd] = C(0x151413)                                                       # black head
        fwd = (A - (2.3 if graze else 2.45)) * (1 if graze else 1)
        blaze = hd & (np.abs(Ps[2]) < 0.06) & (fwd > -0.15)                          # white blaze down the face
        base[blaze] = C(0xDAD5CA)
        base[part == 2] = np.where((Hh[part == 2] < 0.45)[:, None], C(0xDAD5CA), base[part == 2])
        base[part == 4] = C(0xD5A39A)
    elif breed == 'angus':
        base = np.broadcast_to(C(0x161412), A.shape + (3,)).copy()
        base *= (1 + 0.25 * np.clip(Hh - 0.9, 0, 1))[..., None]                     # sheen along the back
    else:  # hereford
        base = np.broadcast_to(C(0x7B3A1F), A.shape + (3,)).copy()
        white = (part == 1) | (part == 5) | (part == 8)
        white |= (Hh < 0.86) & (A > 0.45) & (A < 1.75) & (part == 0)                # belly line
        white |= (A > 1.75) & (Hh < 1.02) & (part == 0)                             # brisket, dewlap
        white |= (part == 2) & (Hh < 0.35)                                          # socks
        white |= (part == 7)
        white |= (A < 0.1) & (Hh > 1.2) & (part == 0)                               # crest of the tail
        base[white] = C(0xE7E0D2)
        base[(part == 1) & (Hh > (1.0 if not graze else 0.0)) & (np.abs(Ps[2]) > 0.13)] = C(0x7B3A1F)  # red round the eyes
    base[part == 3] = C(0x2A2420)                                                    # hooves
    base[part == 5] = base[part == 5] * 0.55 + C(0x4A3A36) * 0.45                    # muzzle
    base[part == 6] = C(0x0A0808)                                                    # eyes
    if breed != 'angus':
        base[part == 7] = C(0x1A1816) if breed == 'hol' else C(0xE7E0D2)
    col = base * (occ * fur)[..., None]
    v.s0, v.s1, v.col = np.where(solid, s0, np.inf).astype(np.float32), np.where(solid, s1, -np.inf).astype(np.float32), col.astype(np.float32)
    v.save()


cow('cow_hol', 'hol', False)
cow('cow_hol_g', 'hol', True)
cow('cow_angus_g', 'angus', True)
cow('cow_here', 'here', False)

# ============================================================================== props
# split-rail fence, 8 m: posts every 2.67 m, two rails; s across 0 .. 1.2 with the fence on 0.5 .. 0.7
WOOD, WOODD = C(0x7C6450), C(0x5C4A3A)
v = Vol('fence_rail', 0, 8.0, 1.4, 0.0, ppm=64)
for a in (0.08, 2.72, 5.36, 7.92):
    v.box(a - 0.07, a + 0.07, 0, 1.35, 0.53, 0.67, WOODD)
for h in (0.55, 1.05):
    v.rod(0, h, 0.6, 8.0, h + 0.03, 0.6, 0.06, WOOD)
v.save()
# landfill litter fence, 8 m x 3 m: steel posts, top rail, chain-link mesh
v = Vol('fence_litter', 0, 8.0, 3.2, 0.0, ppm=64)
for a in (0.06, 4.0, 7.94):
    v.rod(a, 0, 0.6, a, 3.1, 0.6, 0.04, C(0x4A4D50))
v.rod(0, 3.0, 0.6, 8.0, 3.0, 0.6, 0.025, C(0x5A5D60))
mesh = (np.abs(np.sin((v.AA + v.HH) * math.pi / 0.12)) < 0.22) | (np.abs(np.sin((v.AA - v.HH) * math.pi / 0.12)) < 0.22)
v.put(mesh & (v.HH > 0.05) & (v.HH < 3.0), 0.59, 0.61, C(0x8A8D90))
v.save()
# windmill tower, 1.2 m base, 14 m: four tapering angle-iron legs, X bracing on every face, girts, platform
v = Vol('windmill_tower', -0.6, 1.2, 14.4, -0.6, ppm=110)
TOP = 0.16
legs = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
def at(i, y, b=0.56):
    k = b + (TOP - b) * y / 13.8
    return legs[i][0] * k, legs[i][1] * k
for i in range(4):
    (a0_, s0_), (a1_, s1_) = at(i, 0), at(i, 13.8)
    v.rod(a0_, 0, s0_, a1_, 13.8, s1_, 0.035, C(0x8C8F92))
ys = [0, 2.6, 5.0, 7.2, 9.2, 11.0, 12.6, 13.8]
for y0, y1 in zip(ys[:-1], ys[1:]):
    for i in range(4):
        j = (i + 1) % 4
        (ai0, si0), (aj1, sj1) = at(i, y0), at(j, y1)
        (aj0, sj0), (ai1, si1) = at(j, y0), at(i, y1)
        v.rod(ai0, y0, si0, aj1, y1, sj1, 0.015, C(0x7A7D80))
        v.rod(aj0, y0, sj0, ai1, y1, si1, 0.015, C(0x7A7D80))
        (ai, si), (aj, sj) = at(i, y1), at(j, y1)
        v.rod(ai, y1, si, aj, y1, sj, 0.02, C(0x7A7D80))
v.box(-0.45, 0.45, 13.8, 13.9, -0.45, 0.45, C(0x6A6D70))
v.save()
# oil pipe rack, 7 m x 5 m: three bents, three lines (insulated, yellow gas, blue water)
v = Vol('pipe_rack', 0, 7.0, 5.0, 0.0, ppm=64)
for a in (0.3, 3.5, 6.7):
    for s in (0.12, 1.08):
        v.box(a - 0.1, a + 0.1, 0, 4.15, s - 0.1, s + 0.1, C(0x4A4D50))
    v.box(a - 0.1, a + 0.1, 3.95, 4.15, 0.02, 1.18, C(0x4A4D50))
for s, r, c in ((0.3, 0.16, C(0x9A9C9E)), (0.62, 0.12, C(0xB89A30)), (0.92, 0.12, C(0x2E4A6A))):
    v.rod(0, 4.15 + r, s, 7.0, 4.15 + r, s, r, c)
v.save()
# pumpjack frame (the moving beam, pitman arms and cranks are geometry): skid, samson post, gear box, motor, wellhead
v = Vol('pumpjack', -2.8, 7.2, 5.6, -0.6, ppm=64)
v.box(-1.6, 2.4, 0.3, 0.7, -0.45, 0.45, C(0x3A3D40))                               # skid
v.box(-2.6, 4.4, 0.0, 0.3, -0.6, 0.6, C(0x8C877C))                                 # concrete pad
for s in (-0.55, 0.55):
    v.rod(-0.2, 0.7, s, 0.7, 5.2, s * 0.2, 0.09, C(0x3A3D40))                      # samson post legs
    v.rod(1.7, 0.7, s, 0.7, 5.2, s * 0.2, 0.09, C(0x3A3D40))
    v.rod(0.1, 2.6, s * 0.7, 1.4, 2.6, s * 0.7, 0.04, C(0x3A3D40))                 # brace
v.box(0.45, 0.95, 5.1, 5.35, -0.18, 0.18, C(0x2E3033))                             # saddle bearing
v.box(-1.85, -0.75, 0.7, 1.8, -0.4, 0.4, C(0x2E5E8A))                              # gear box
v.box(-1.75, -0.85, 1.75, 1.85, -0.42, 0.42, C(0x23486A))
v.box(-2.45, -1.75, 0.7, 1.3, -0.3, 0.3, C(0x6E7174))                              # motor
v.box(-2.1, -1.1, 0.95, 1.05, -0.32, 0.32, C(0x1E1E1E))                            # belt guard
v.rod(3.9, 0.3, 0, 3.9, 1.5, 0, 0.2, C(0x8E9092))                                  # wellhead
v.rod(3.9, 1.5, 0, 3.9, 2.1, 0, 0.1, C(0xB0B2B4))
v.rod(3.9, 0.65, 0, 3.9, 0.65, 0.6, 0.08, C(0x6E7174))                             # flow tee
v.save()

# ---- round bale (1.5 m across, 1.2 m wide), lying on its barrel with the axis across the volume (along N). The relief carries
# the whole shape: a barrel that sags a little under its own weight, ends domed by the baler's belts, the net wrap folded over
# the edges, ragged straw at the rim, the straw spiral on both ends and the ground-stained underside.
v = Vol('bale', -0.8, 1.6, 1.55, -0.62, ppm=300)
A, Hh = v.AA, v.HH
cx, cy = 0.0, 0.74
ang = np.arctan2(Hh - cy, A - cx)
rx = 0.76 * (1 + 0.004 * np.sin(ang * 9))                                          # near-round: the barrel stays smooth
ry = 0.73 * (1 + 0.004 * np.sin(ang * 9)) - 0.03 * np.clip(-np.sin(ang), 0, 1)  # sag
q = ((A - cx) / rx) ** 2 + ((Hh - cy) / ry) ** 2
r = np.sqrt(q)
inside = q <= 1
half = 0.57 + 0.035 * np.clip(1 - q, 0, 1) - 0.03 * np.clip(r - 0.9, 0, 1) / 0.1   # domed ends, rounded shoulder
STRAW, STRAW_D, NET, DIRT = C(0xC9A961), C(0x8E7240), C(0xD8D6C8), C(0x6E5A3C)
spiral = 0.5 + 0.5 * np.sin(r * 2 * np.pi * 15 + ang)                               # the wound layers seen on the end
col = STRAW[None, None] * (0.82 + 0.25 * spiral[..., None])
col = np.where((rng.random(A.shape) < 0.08)[..., None], STRAW_D, col)                # loose dark stalks
col = np.where((r < 0.18)[..., None], col * 0.8, col)                                # tight dark core
net = (r > 0.9)
# the rim texels are what the barrel shows along its whole length: a smooth net-over-straw colour (no per-texel
# pattern, which would streak), with the net's fold visible on the ends as fine rings just inside the edge
wrap = NET * 0.7 + STRAW * 0.3
col = np.where((r > 0.97)[..., None], wrap[None, None] * (1 + 0.04 * np.sin(ang * 3))[..., None], col)
col = np.where((net & (r <= 0.97))[..., None], np.where((np.abs(np.sin(r * 160)) < 0.3)[..., None], wrap, col * 0.92), col)
col = np.where(((Hh < 0.18) & inside)[..., None], col * 0.6 + DIRT * 0.4, col)         # stained where it sits
v.put(inside, -half, half, col)
v.save()

# ---- the table the engine reads (sizes must match the volumes exactly)
lines = ['// Generated by tools/building_gen/industry_spom.py: one SPOM volume texture per entry (assets/industry/spom/<file>_a/_n.png,',
         '// and <file>_f for the mirrored far side). a0, L: span along the volume; H: height; sIn: its inner plane across.',
         '#pragma once', 'namespace indspom {',
         'struct Vol { const char* file; float a0, L, H, sIn; };', 'enum Id : int {']
lines += ['    %s,' % v_.name.upper() for v_ in VOLS] + ['    COUNT', '};', 'constexpr Vol kVol[COUNT] = {']
lines += ['    {"%s", %.3ff, %.3ff, %.3ff, %.3ff},' % (v_.name, v_.a0, v_.L, v_.H, v_.sIn) for v_ in VOLS]
lines += ['};', '} // namespace indspom', '']
open(os.path.join(ROOT, 'src', 'city', 'industry_spom.h'), 'w').write('\n'.join(lines))
print(len(VOLS), 'volumes')
