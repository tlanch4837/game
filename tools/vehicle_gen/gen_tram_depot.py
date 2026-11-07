"""Tram depots -> GLB (CS2 set: the base Tram Depot and the City Stations pack's Compact Refurbished Tram Depot).

Ultra low poly: every face is one quad and all detail (brick, cladding, portals, doors, glazing, rails set in the yard paving,
parking stalls, fences) is texture + normal map, from a height field painted alongside the albedo. Three materials per model:
  yard   whole-lot ground quad (rails embedded in concrete, as tram yards are)
  walls  one atlas: every wall face maps a region of its own (no tiling, so one draw)
  roofs  one atlas: roof membranes, standing seam, north-light glazing
No trams, cars, catenary poles or lamp posts are baked in. Sign panels are left blank.
Local frame: x across the lot, z toward the street (front, +z), y up. Lot cell = 8 m.
Run:  python gen_tram_depot.py
Out:  assets/rail/tram_depot/glb/TRAMDEPOT_19x17_tram_depot_L1.glb, TRAMDEPOT_10x8_compact_refurbished_L1.glb
"""
import io, json, math, os, struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import gen_cars as gc
from gen_cars import Mesh
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'building_gen'))
import spom

OUT = os.path.join(gc.ROOT, "assets", "rail", "tram_depot", "glb")
CELL = 8.0


# ------------------------------------------------------------------------------------------------ painting with relief
class Atlas:
    """Albedo + height painted together; regions are allocated by a shelf packer, sized in metres at `ppm` pixels/metre."""

    def __init__(self, n, ppm, base=(128, 128, 128)):
        self.n, self.ppm = n, ppm
        self.alb = np.zeros((n, n, 3), float) + base
        self.h = np.zeros((n, n), float)
        self.x = self.y = self.row = 0

    def region(self, w, h):
        pw, ph = int(math.ceil(w * self.ppm)) + 2, int(math.ceil(h * self.ppm)) + 2
        if self.x + pw > self.n: self.x, self.y, self.row = 0, self.y + self.row, 0
        assert self.y + ph <= self.n, "atlas full"
        r = Region(self, self.x + 1, self.y + 1, w, h)
        self.x += pw; self.row = max(self.row, ph)
        return r

    def maps(self, strength=3.0, jpg=True):
        a = Image.fromarray(self.alb.clip(0, 255).astype(np.uint8))
        hb = np.asarray(Image.fromarray((self.h * 40).clip(0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.7)), float) / 40
        gy, gx = np.gradient(hb * self.ppm * 0.08 * strength)
        nz = np.ones_like(gx); l = np.sqrt(gx ** 2 + gy ** 2 + 1)
        nm = np.stack([-gx / l, gy / l, nz / l], -1) * 0.5 + 0.5
        b1 = io.BytesIO(); (a.save(b1, "JPEG", quality=88) if jpg else a.save(b1, "PNG"))
        b2 = io.BytesIO(); Image.fromarray((nm * 255).astype(np.uint8)).save(b2, "PNG")
        return b1.getvalue(), b2.getvalue()


class Region:
    """A w x h metre rectangle of an atlas. Paint with metre coordinates, (0, 0) = bottom-left. uv() maps metres to atlas uv."""

    def __init__(self, at, px, py, w, h):
        self.at, self.px, self.py, self.w, self.h = at, px, py, w, h

    def _sl(self, x0, y0, x1, y1):
        p = self.at.ppm; H = self.h
        c0, c1 = self.px + int(round(x0 * p)), self.px + int(round(x1 * p))
        r0, r1 = self.py + int(round((H - y1) * p)), self.py + int(round((H - y0) * p))
        return slice(max(r0, self.py), min(r1, self.py + int(H * p))), slice(max(c0, self.px), min(c1, self.px + int(self.w * p)))

    def rect(self, x0, y0, x1, y1, col, h=None, noise=0.0, seed=0):
        rs, cs = self._sl(x0, y0, x1, y1)
        blk = self.at.alb[rs, cs]
        if blk.size == 0: return
        c = np.array(col, float)
        if noise:
            rng = np.random.default_rng(seed)
            blk[:] = c + rng.normal(0, noise, blk.shape[:2])[..., None]
        else: blk[:] = c
        if h is not None: self.at.h[rs, cs] = h

    def add_h(self, x0, y0, x1, y1, dh):
        rs, cs = self._sl(x0, y0, x1, y1); self.at.h[rs, cs] += dh

    def shade(self, x0, y0, x1, y1, f):
        rs, cs = self._sl(x0, y0, x1, y1); self.at.alb[rs, cs] *= f

    def grain(self, amt, seed=1):
        rs, cs = self._sl(0, 0, self.w, self.h)
        rng = np.random.default_rng(seed); blk = self.at.alb[rs, cs]
        blk += rng.normal(0, amt, blk.shape[:2])[..., None]

    def uv(self, x, y):
        p, n = self.at.ppm, self.at.n
        return ((self.px + x * p) / n, (self.py + (self.h - y) * p) / n)


def brick(r, x0, y0, x1, y1, base=(138, 60, 44), seed=3, h=0.0):
    """Running-bond brick, 0.075 m courses drawn as 0.15 m bands (readable at game zoom)."""
    rng = np.random.default_rng(seed)
    r.rect(x0, y0, x1, y1, (150, 140, 128), h - 0.02)
    ch, bw = 0.15, 0.45
    y = y0; k = 0
    while y < y1:
        off = (k % 2) * bw / 2; x = x0 - off
        while x < x1:
            c = np.array(base) * rng.uniform(0.82, 1.12)
            r.rect(max(x, x0) + 0.02, y + 0.025, min(x + bw, x1) - 0.02, min(y + ch, y1), c, h)
            x += bw
        y += ch; k += 1


def window(r, x0, y0, x1, y1, mull=1.2, frame=(55, 58, 62), sky=True, h0=0.0):
    """Recessed window: frame, glass with a sky gradient + streak reflections, mullions every `mull` m."""
    r.rect(x0, y0, x1, y1, frame, h0 - 0.06)
    g0 = np.array((92, 116, 138)); g1 = np.array((178, 196, 210))
    rs, cs = r._sl(x0 + 0.06, y0 + 0.06, x1 - 0.06, y1 - 0.06)
    blk = r.at.alb[rs, cs]
    if blk.size:
        t = np.linspace(1, 0, blk.shape[0])[:, None, None] if sky else 0.5
        blk[:] = g0 + (g1 - g0) * t
        xs = np.arange(blk.shape[1])[None, :, None]
        blk += 22 * (np.sin(xs * 0.09 + np.arange(blk.shape[0])[:, None, None] * 0.05) > 0.85)
        r.at.h[rs, cs] = h0 - 0.32
    n = max(1, int(round((x1 - x0) / mull)))
    for i in range(1, n):
        xm = x0 + (x1 - x0) * i / n; r.rect(xm - 0.04, y0, xm + 0.04, y1, frame, h0 - 0.08)


def cladding(r, x0, y0, x1, y1, col, pitch=0.3, seed=5):
    """Ribbed metal cladding (vertical ribs)."""
    r.rect(x0, y0, x1, y1, col, 0.0, noise=3, seed=seed)
    x = x0
    while x < x1:
        r.rect(x, y0, x + 0.08, y1, np.array(col) * 1.08, 0.04); r.rect(x + 0.08, y0, x + 0.11, y1, np.array(col) * 0.8, 0.02)
        x += pitch


def portal(r, x0, x1, y1, open_=0.85, col=(196, 200, 204)):
    """A tram portal: steel frame, roll-up door raised to `open_`, dark interior with the rails' glint below."""
    r.rect(x0 - 0.25, 0, x1 + 0.25, y1 + 0.35, (70, 74, 78), 0.06)
    r.rect(x0, 0, x1, y1, (24, 26, 28), -0.86)
    r.rect(x0 + 0.2, 0, x1 - 0.2, 0.7, (38, 40, 42), -0.86)
    door_bot = y1 * open_
    r.rect(x0, door_bot, x1, y1, col, -0.2)
    yy = door_bot
    while yy < y1:
        r.rect(x0, yy, x1, yy + 0.04, np.array(col) * 0.75, -0.22); yy += 0.18


def label_panel(r, x0, y0, x1, y1):
    """Blank sign panel (any lettering texture can be applied over it)."""
    r.rect(x0 - 0.08, y0 - 0.08, x1 + 0.08, y1 + 0.08, (40, 42, 46), 0.12)
    r.rect(x0, y0, x1, y1, (236, 236, 232), 0.14)


# ------------------------------------------------------------------------------------------------ geometry helpers
def face(m, mat, reg, P, nrm, ext=None):
    """One quad P (bottom-left, bottom-right, top-right, top-left seen from outside) showing the whole region."""
    w, h = ext or (reg.w, reg.h)
    m.quad(mat, P, [reg.uv(0, 0), reg.uv(w, 0), reg.uv(w, h), reg.uv(0, h)], np.array(nrm, float))


def wall_x(m, reg, x, s, z0, z1, y0, y1):
    """Wall on plane x, facing s*x."""
    if isinstance(reg, SpomFace): return reg.add((x, y0, z1 if s > 0 else z0), (s, 0, 0), z1 - z0, y1 - y0)
    zz = (z1, z0) if s > 0 else (z0, z1)
    face(m, "walls", reg, [(x, y0, zz[0]), (x, y0, zz[1]), (x, y1, zz[1]), (x, y1, zz[0])], (s, 0, 0))


def wall_z(m, reg, z, s, x0, x1, y0, y1):
    if isinstance(reg, SpomFace): return reg.add((x0 if s > 0 else x1, y0, z), (0, 0, s), x1 - x0, y1 - y0)
    xx = (x0, x1) if s > 0 else (x1, x0)
    face(m, "walls", reg, [(xx[0], y0, z), (xx[1], y0, z), (xx[1], y1, z), (xx[0], y1, z)], (0, 0, s))


def roof_flat(m, reg, x0, x1, z0, z1, y):
    m.quad("roofs", [(x0, y, z1), (x1, y, z1), (x1, y, z0), (x0, y, z0)],
           [reg.uv(0, 0), reg.uv(reg.w, 0), reg.uv(reg.w, reg.h), reg.uv(0, reg.h)], np.array((0, 1, 0.0)))


def block(m, x0, x1, z0, z1, y0, y1, front, back, left, right, roof, faces="fblr"):
    if "f" in faces: wall_z(m, front, z1, 1, x0, x1, y0, y1)
    if "b" in faces: wall_z(m, back, z0, -1, x0, x1, y0, y1)
    if "l" in faces: wall_x(m, left, x0, -1, z0, z1, y0, y1)
    if "r" in faces: wall_x(m, right, x1, 1, z0, z1, y0, y1)
    if roof is not None: roof_flat(m, roof, x0, x1, z0, z1, y1)


def ground(m, reg, hw, hd, y=0.02):
    m.quad("yard", [(-hw, y, hd), (hw, y, hd), (hw, y, -hd), (-hw, y, -hd)], [(0, 0), (1, 0), (1, 1), (0, 1)], np.array((0, 1, 0.0)))


# ------------------------------------------------------------------------------------------------ yard texture
class Yard:
    """The lot's ground: concrete with embedded grooved rails, painted at ppm px/m. Coordinates = local x, z."""

    def __init__(self, hw, hd, ppm=12):
        self.hw, self.hd, self.p = hw, hd, ppm
        W, H = int(2 * hw * ppm), int(2 * hd * ppm)
        rng = np.random.default_rng(7)
        a = np.full((H, W, 3), (150, 148, 142), float) + rng.normal(0, 6, (H, W))[..., None]
        self.im = Image.fromarray(a.clip(0, 255).astype(np.uint8)); self.d = ImageDraw.Draw(self.im)
        self.hm = Image.new("L", (W, H), 128); self.dh = ImageDraw.Draw(self.hm)

    def P(self, x, z): return ((x + self.hw) * self.p, (self.hd - z) * self.p)

    def rect(self, x0, z0, x1, z1, col, h=None):
        a, b = self.P(x0, z1), self.P(x1, z0)
        self.d.rectangle([a, b], fill=col)
        if h is not None: self.dh.rectangle([a, b], fill=h)

    def slabs(self, x0, z0, x1, z1, step=6.0, col=(118, 116, 110)):
        x = x0
        while x <= x1: self.d.line([self.P(x, z0), self.P(x, z1)], fill=col, width=1); x += step
        z = z0
        while z <= z1: self.d.line([self.P(x0, z), self.P(x1, z)], fill=col, width=1); z += step

    def track(self, pts, gauge=1.435):
        """Embedded grooved rails along a polyline (list of (x, z)): a darker trackbed strip, two steel lines with grooves."""
        pts = np.array(pts, float)
        def off(d):
            out = []
            for i in range(len(pts)):
                a = pts[max(i - 1, 0)]; b = pts[min(i + 1, len(pts) - 1)]
                t = b - a; t /= np.linalg.norm(t); n = np.array((-t[1], t[0]))
                out.append(self.P(*(pts[i] + n * d)))
            return out
        self.d.line(off(0), fill=(122, 120, 116), width=int(2.6 * self.p), joint="curve")
        for s in (-1, 1):
            g = off(s * (gauge / 2 + 0.05))
            self.d.line(g, fill=(52, 50, 48), width=max(2, int(0.12 * self.p)), joint="curve")
            self.dh.line(g, fill=100, width=max(2, int(0.12 * self.p)))
            self.d.line(off(s * gauge / 2), fill=(196, 194, 188), width=max(1, int(0.08 * self.p)), joint="curve")

    def stalls(self, x0, z0, x1, z1, w=2.7, along_x=True):
        self.rect(x0, z0, x1, z1, (66, 68, 70))
        x = x0
        while x <= x1 + 1e-6:
            self.d.line([self.P(x, z0 + 0.4), self.P(x, z0 + 5.4)], fill=(230, 230, 222), width=2)
            self.d.line([self.P(x, z1 - 5.4), self.P(x, z1 - 0.4)], fill=(230, 230, 222), width=2)
            x += w

    def fence(self, x0, z0, x1, z1):
        for a, b in (((x0, z0), (x1, z0)), ((x1, z0), (x1, z1)), ((x1, z1), (x0, z1)), ((x0, z1), (x0, z0))):
            self.d.line([self.P(*a), self.P(*b)], fill=(70, 72, 74), width=2)
            # the fence's shadow to the north-east, so it reads as standing
            sa = self.P(a[0] + 0.5, a[1] - 0.5); sb = self.P(b[0] + 0.5, b[1] - 0.5)
            self.d.line([sa, sb], fill=(110, 110, 106), width=3)

    def grass(self, x0, z0, x1, z1):
        self.rect(x0, z0, x1, z1, (92, 112, 62))

    def maps(self):
        a = self.im.filter(ImageFilter.GaussianBlur(0.4))
        h = np.asarray(self.hm, float) / 255
        gy, gx = np.gradient(h * 6)
        l = np.sqrt(gx ** 2 + gy ** 2 + 1)
        nm = np.stack([-gx / l, gy / l, 1 / l], -1) * 0.5 + 0.5
        b1 = io.BytesIO(); a.convert("RGB").save(b1, "JPEG", quality=86)
        b2 = io.BytesIO(); Image.fromarray((nm * 255).astype(np.uint8)).save(b2, "PNG")
        return b1.getvalue(), b2.getvalue()


def bez(p0, p1, p2, p3, n=14):
    t = np.linspace(0, 1, n)[:, None]
    p0, p1, p2, p3 = (np.array(p, float) for p in (p0, p1, p2, p3))
    return list(map(tuple, (1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * p1 + 3 * (1 - t) * t ** 2 * p2 + t ** 3 * p3))


def sawtooth(m, reg_slope, reg_glass, reg_tri, x0, x1, z0, z1, y, pitch=10.0, rise=3.2):
    """North-light sawtooth roof: ridges along x, glazing facing -z (back). 4 tris per tooth + 2 gable triangles each end."""
    n = int(round((z1 - z0) / pitch)); p = (z1 - z0) / n
    for i in range(n):
        za, zb = z0 + i * p, z0 + (i + 1) * p
        # sloped metal: high at za (the back), low at zb
        m.quad("roofs", [(x0, y, zb), (x1, y, zb), (x1, y + rise, za), (x0, y + rise, za)],
               [reg_slope.uv(0, 0), reg_slope.uv(reg_slope.w, 0), reg_slope.uv(reg_slope.w, reg_slope.h), reg_slope.uv(0, reg_slope.h)],
               np.array((0, p, rise)))
        # vertical glazing facing back
        m.quad("roofs", [(x1, y, za), (x0, y, za), (x0, y + rise, za), (x1, y + rise, za)],
               [reg_glass.uv(0, 0), reg_glass.uv(reg_glass.w, 0), reg_glass.uv(reg_glass.w, reg_glass.h), reg_glass.uv(0, reg_glass.h)],
               np.array((0, 0, -1.0)))
        for x, s in ((x0, -1), (x1, 1)):
            m.tri("walls", [(x, y, za), (x, y, zb), (x, y + rise, za)],
                  [reg_tri.uv(0, 0), reg_tri.uv(reg_tri.w, 0), reg_tri.uv(0, reg_tri.h)], np.array((s, 0, 0.0)))
    return n


# ------------------------------------------------------------------------------------------------ L1: Tram Depot 19 x 17
def tram_depot():
    hw, hd = 19 * CELL / 2, 17 * CELL / 2          # 76 x 68
    m = Mesh(); W = Atlas(2048, 18, (150, 150, 150)); R = Atlas(1024, 10, (120, 120, 120))
    # shed: 8 roads at 5.5 m, 90 m long (3 x 30 m trams per road)
    sx0, sx1, sz0, sz1, SH = -48.0, 0.0, -62.0, 28.0, 9.0
    roads = [-24.0 + 5.5 * (i - 3.5) for i in range(8)]
    # annex (wheel lathe + bogie drop, 2 roads), 11 m tall for the crane
    ax0, ax1, az0, az1, AH = 0.0, 22.0, -62.0, -12.0, 11.0
    # office (2 storeys, glass lobby) and traction substation
    ox0, ox1, oz0, oz1, OH = 30.0, 70.0, 30.0, 47.0, 8.4
    bx0, bx1, bz0, bz1, BH = 54.0, 66.0, -58.0, -50.0, 4.5

    # --- walls atlas
    side = W.region(sz1 - sz0, SH)                                    # shed long walls (shared both sides)
    side.rect(0, 0, side.w, side.h, (120, 128, 134))
    cladding(side, 0, 1.2, side.w, side.h, (128, 138, 146))
    brick(side, 0, 0, side.w, 1.2, (122, 118, 112), 11)              # concrete-block plinth
    for z in np.arange(0, side.w + 0.1, 7.5):
        side.rect(z - 0.25, 0, z + 0.25, side.h, (96, 100, 104), 0.18)   # steel columns / pilasters
    for z in np.arange(0.6, side.w - 7.0, 7.5):
        window(side, z + 0.5, 5.6, z + 6.4, 8.2, 1.5)                 # clerestory band
        window(side, z + 2.6, 1.8, z + 4.4, 3.8, 0.9)                 # staff-level windows
    for z in (22.5 + 1.0, 60.0 + 1.0):                                # personnel doors
        side.rect(z, 0, z + 1.1, 2.3, (70, 82, 96), -0.08)
    side.rect(0, side.h - 0.4, side.w, side.h, (86, 90, 94), 0.1)     # coping
    side.grain(3)

    endw = W.region(sx1 - sx0, SH)                                    # shed front/back end walls: 8 portals
    endw.rect(0, 0, endw.w, endw.h, (120, 128, 134))
    cladding(endw, 0, 1.2, endw.w, endw.h, (132, 140, 148))
    brick(endw, 0, 0, endw.w, 1.2, (122, 118, 112), 12)
    for i, x in enumerate(roads):
        u = x - sx0
        portal(endw, u - 2.1, u + 2.1, 5.8, open_=(0.95, 0.4, 0.95, 0.95, 0.1, 0.95, 0.6, 0.95)[i])
    for x in roads[:-1]:
        u = x - sx0 + 2.75; endw.rect(u - 0.3, 0, u + 0.3, endw.h, (96, 100, 104), 0.18)
    window(endw, 2, 6.6, endw.w - 2, 8.3, 1.8)
    endw.rect(0, endw.h - 0.4, endw.w, endw.h, (86, 90, 94), 0.1)
    endw.grain(3)

    tooth = W.region(10.0, 3.2)                                       # sawtooth gable triangles (cladding)
    cladding(tooth, 0, 0, tooth.w, tooth.h, (128, 138, 146))

    anx_s = W.region(az1 - az0, AH)                                   # annex long walls
    brick(anx_s, 0, 0, anx_s.w, AH, (150, 92, 64), 21)
    for z in np.arange(1.5, anx_s.w - 4, 6.0):
        window(anx_s, z, 4.0, z + 4.0, 9.6, 1.0)
    anx_s.rect(0, AH - 0.5, anx_s.w, AH, (96, 92, 88), 0.12)
    anx_s.grain(3)
    anx_f = W.region(ax1 - ax0, AH)                                   # annex front: 2 tall portals + blank panel
    brick(anx_f, 0, 0, anx_f.w, AH, (150, 92, 64), 22)
    for u in (5.5, 16.5): portal(anx_f, u - 2.1, u + 2.1, 6.4, open_=0.95)
    label_panel(anx_f, 3.0, 8.0, 19.0, 9.6)
    anx_f.rect(0, AH - 0.5, anx_f.w, AH, (96, 92, 88), 0.12)

    of = W.region(ox1 - ox0, OH)                                      # office front: glass lobby + ribbon windows
    of.rect(0, 0, of.w, OH, (206, 202, 192), 0.0, noise=3, seed=31)
    window(of, 1.0, 0.0, 15.0, 3.6, 1.6)                              # lobby curtain wall
    of.rect(6.4, 0, 9.6, 2.6, (70, 84, 96), -0.25)                    # entrance doors
    of.rect(0.6, 3.6, 15.4, 4.1, (60, 64, 70), 0.3)                   # canopy band
    label_panel(of, 2.0, 4.4, 14.0, 5.3)
    for x in np.arange(17.0, of.w - 1.5, 3.0): window(of, x, 1.0, x + 2.2, 3.0, 2.2)
    window(of, 17.0, 5.0, of.w - 1.0, 7.4, 1.5)
    window(of, 1.0, 5.6, 1.6, 7.4, 1.0)
    of.rect(0, OH - 0.4, of.w, OH, (150, 148, 142), 0.12)
    of.grain(2)
    os_ = W.region(oz1 - oz0, OH)                                     # office sides / back
    os_.rect(0, 0, os_.w, OH, (206, 202, 192), 0.0, noise=3, seed=32)
    for x in np.arange(1.5, os_.w - 2, 3.5):
        window(os_, x, 1.0, x + 2.2, 3.0, 2.2); window(os_, x, 5.0, x + 2.2, 7.2, 2.2)
    os_.rect(0, OH - 0.4, os_.w, OH, (150, 148, 142), 0.12)

    sub = W.region(bx1 - bx0, BH)                                     # substation: precast with louvres
    sub.rect(0, 0, sub.w, BH, (176, 172, 162), 0.0, noise=4, seed=41)
    for x in (1.5, 6.0, 9.0):
        sub.rect(x, 1.0, x + 1.6, 3.2, (90, 94, 96), -0.05)
        for y in np.arange(1.1, 3.2, 0.2): sub.rect(x, y, x + 1.6, y + 0.07, (60, 62, 64), -0.12)
    sub.rect(3.6, 0, 5.0, 2.4, (80, 96, 110), -0.1)

    # --- roofs atlas
    slope = R.region(sx1 - sx0, math.hypot(10.0, 3.2))
    slope.rect(0, 0, slope.w, slope.h, (150, 154, 156))
    for x in np.arange(0, slope.w, 0.6): slope.rect(x, 0, x + 0.06, slope.h, (176, 180, 182), 0.08)
    slope.grain(4)
    glazing = R.region(sx1 - sx0, 3.2)
    window(glazing, 0, 0.3, glazing.w, 3.0, 1.5)
    anx_r = R.region(ax1 - ax0, az1 - az0)
    anx_r.rect(0, 0, anx_r.w, anx_r.h, (110, 112, 112), 0.0, noise=6, seed=51)
    anx_r.rect(0, 0, anx_r.w, 0.4, (150, 146, 140), 0.25); anx_r.rect(0, anx_r.h - 0.4, anx_r.w, anx_r.h, (150, 146, 140), 0.25)
    anx_r.rect(0, 0, 0.4, anx_r.h, (150, 146, 140), 0.25); anx_r.rect(anx_r.w - 0.4, 0, anx_r.w, anx_r.h, (150, 146, 140), 0.25)
    for z in np.arange(4, anx_r.h - 4, 8): window(anx_r, 7, z, 15, z + 3, 2.0)          # skylights
    off_r = R.region(ox1 - ox0, oz1 - oz0)
    off_r.rect(0, 0, off_r.w, off_r.h, (196, 196, 192), 0.0, noise=5, seed=52)
    for a, b, c, d in ((0, 0, off_r.w, 0.4), (0, off_r.h - 0.4, off_r.w, off_r.h), (0, 0, 0.4, off_r.h), (off_r.w - 0.4, 0, off_r.w, off_r.h)):
        off_r.rect(a, b, c, d, (150, 148, 142), 0.3)
    for x in (8.0, 18.0, 28.0): off_r.rect(x, 6, x + 3.5, 10, (120, 124, 126), 0.4); off_r.rect(x + 0.3, 6.3, x + 3.2, 9.7, (88, 92, 94), 0.5)  # HVAC
    sub_r = R.region(bx1 - bx0, bz1 - bz0)
    sub_r.rect(0, 0, sub_r.w, sub_r.h, (104, 106, 106), 0.0, noise=5, seed=53)

    # --- yard
    Y = Yard(hw, hd)
    Y.grass(-hw, -hd, hw, hd)
    Y.rect(-hw + 2, -hd + 2, hw - 2, hd - 2, (150, 148, 142))
    Y.slabs(-hw + 2, -hd + 2, hw - 2, hd - 2)
    Y.grass(26, -hd + 2, hw - 2, 24)                                   # landscaped strip behind the office
    Y.rect(50, -62, 70, -46, (128, 126, 120)); Y.fence(50, -62, 70, -46)   # substation compound (gravel)
    Y.stalls(30, 50, 70, 64)                                           # staff parking
    Y.rect(70, 50, hw, 64, (66, 68, 70)); Y.rect(26, 47, 30, hd, (66, 68, 70))
    Y.rect(30, 47, 70, 50, (180, 178, 172))                            # walk in front of the office
    # throat: 8 roads leave the portals, fan pairwise into 2 leads that reach the street at x = -27, -21
    leads = (-26.0, -20.5)
    for i, x in enumerate(roads):
        lx = leads[0] if i < 4 else leads[1]
        Y.track([(x, sz0), (x, sz1 + 4)] + bez((x, sz1 + 4), (x, sz1 + 18), (lx, sz1 + 16), (lx, sz1 + 30))[1:] + [(lx, hd)])
    for x in (5.5, 16.5):                                              # annex roads, looping round to the east lead
        Y.track([(x, az0), (x, az1 + 30)] + bez((x, az1 + 30), (x, sz1 + 22), (leads[1], sz1 + 18), (leads[1], sz1 + 34))[1:])
    Y.track([(-60.0, sz1 + 8)] + bez((-60.0, sz1 + 8), (-60.0, sz1 + 24), (leads[0], sz1 + 20), (leads[0], sz1 + 36))[1:])  # run-round
    Y.track([(-60.0, -hd + 6), (-60.0, sz1 + 8)])
    Y.rect(-36, hd - 3, -12, hd, (130, 128, 124))                      # the apron at the street

    # --- geometry
    # the faces with openings become SPOM relief faces: portals, doors and windows are real recesses
    endw, side, anx_f, of = (to_spom(r, n) for r, n in ((endw, "spom_shed_end"), (side, "spom_shed_side"), (anx_f, "spom_annex_front"), (of, "spom_office_front")))
    ground(m, None, hw, hd)
    block(m, sx0, sx1, sz0, sz1, 0, SH, endw, endw, side, side, None, faces="fblr")
    n = sawtooth(m, slope, glazing, tooth, sx0, sx1, sz0, sz1, SH)
    block(m, ax0, ax1, az0, az1, 0, AH, anx_f, anx_f, anx_s, anx_s, anx_r, faces="fbr")    # left side abuts the shed
    block(m, ox0, ox1, oz0, oz1, 0, OH, of, os_, os_, os_, off_r)
    block(m, bx0, bx1, bz0, bz1, 0, BH, sub, sub, sub, sub, sub_r)
    return m, {"yard": Y.maps(), "walls": W.maps(), "roofs": R.maps(), "faces": (endw, side, anx_f, of)}


# ------------------------------------------------------------------------------------------------ Compact Refurbished 10 x 8
def compact_depot():
    hw, hd = 10 * CELL / 2, 8 * CELL / 2           # 40 x 32
    m = Mesh(); W = Atlas(2048, 22, (150, 150, 150)); R = Atlas(1024, 14, (120, 120, 120))
    cx0, cx1, cz0, cz1, CH, RIDGE = -36.0, -6.0, -29.0, 13.0, 7.5, 11.5   # 1920s brick carbarn, 4 roads
    roads = [-21.0 + 6.0 * (i - 1.5) for i in range(4)]
    mon_w, mon_h = 5.0, 1.6                                              # clerestory monitor on the ridge
    wx0, wx1, wz0, wz1, WH = -6.0, 22.0, -6.0, 13.0, 7.0                 # refurbished office wing (glass + brick)
    hx0, hx1, hz0, hz1, HH = 24.0, 37.0, -28.0, -12.0, 4.0              # sand / compressor house

    red = (148, 66, 46)
    side = W.region(cz1 - cz0, CH)
    brick(side, 0, 0, side.w, CH, red, 61)
    for z in np.arange(0, side.w + 0.1, 6.0): side.rect(z - 0.35, 0, z + 0.35, CH, np.array(red) * 0.92, 0.12)   # piers
    for z in np.arange(1.2, side.w - 4.5, 6.0):
        window(side, z, 2.2, z + 3.6, 5.6, 0.6, frame=(40, 62, 54))   # tall steel-sash windows, green frames
        side.rect(z - 0.15, 5.6, z + 3.75, 6.0, (196, 184, 160), 0.1)  # cast-stone lintels
        side.rect(z - 0.1, 2.0, z + 3.7, 2.2, (196, 184, 160), 0.08)   # sills
    side.rect(0, 0, side.w, 0.6, (176, 166, 148), 0.06)               # stone plinth
    side.rect(0, CH - 0.5, side.w, CH, (196, 184, 160), 0.15)         # cornice
    side.grain(3)

    front = W.region(cx1 - cx0, RIDGE)                                  # gable end: 4 arched doors + oculus + blank panel
    brick(front, 0, 0, front.w, RIDGE, red, 62)
    for x in roads:
        u = x - cx0
        portal(front, u - 2.0, u + 2.0, 5.2, open_=0.95, col=(52, 84, 70))
        for k in range(10):                                            # brick arch voussoirs over each door
            a = math.pi * k / 9
            px, py = u + 2.4 * math.cos(a), 5.2 + 0.9 * math.sin(a)
            front.rect(px - 0.25, py - 0.15, px + 0.25, py + 0.25, (196, 184, 160), 0.12)
    for x in roads[:-1]:
        u = x - cx0 + 3.0; front.rect(u - 0.35, 0, u + 0.35, CH, np.array(red) * 0.9, 0.12)
    front.rect(0, CH - 0.5, front.w, CH, (196, 184, 160), 0.15)
    label_panel(front, front.w / 2 - 6.0, 7.6, front.w / 2 + 6.0, 8.6)
    window(front, front.w / 2 - 0.9, 9.2, front.w / 2 + 0.9, 10.9, 0.9, frame=(40, 62, 54))
    front.rect(0, 0, front.w, 0.6, (176, 166, 148), 0.06)
    front.grain(3)

    mon = W.region(cz1 - cz0, mon_h)                                    # monitor clerestory glazing
    mon.rect(0, 0, mon.w, mon_h, (40, 62, 54), 0.05)
    window(mon, 0.2, 0.15, mon.w - 0.2, mon_h - 0.15, 1.0, frame=(40, 62, 54))
    mon_e = W.region(mon_w, mon_h)
    mon_e.rect(0, 0, mon_w, mon_h, (40, 62, 54)); window(mon_e, 0.3, 0.2, mon_w - 0.3, mon_h - 0.2, 1.0, frame=(40, 62, 54))

    wf = W.region(wx1 - wx0, WH)                                        # wing front: glass ground floor (lobby), brick above
    brick(wf, 0, 3.6, wf.w, WH, (120, 116, 110), 71)                   # grey-brown brick of the new wing
    window(wf, 0.4, 0, 13.0, 3.6, 1.4, frame=(34, 36, 40))
    wf.rect(5.2, 0, 8.0, 2.5, (70, 84, 96), -0.25)
    wf.rect(13.0, 0, wf.w, 3.6, (180, 176, 168), 0.0, noise=3, seed=72)
    for x in np.arange(14.0, wf.w - 1.5, 3.4): window(wf, x, 0.9, x + 2.4, 3.0, 2.4, frame=(34, 36, 40))
    for x in np.arange(1.0, wf.w - 2, 3.4): window(wf, x, 4.4, x + 2.4, 6.4, 1.2, frame=(34, 36, 40))
    label_panel(wf, 1.2, 3.75, 11.5, 4.25)
    wf.rect(0, WH - 0.4, wf.w, WH, (70, 72, 76), 0.15)
    ws = W.region(wz1 - wz0, WH)
    brick(ws, 0, 0, ws.w, WH, (120, 116, 110), 73)
    for x in np.arange(1.2, ws.w - 2, 3.4): window(ws, x, 1.0, x + 2.4, 3.0, 1.2, frame=(34, 36, 40)); window(ws, x, 4.4, x + 2.4, 6.4, 1.2, frame=(34, 36, 40))
    ws.rect(0, WH - 0.4, ws.w, WH, (70, 72, 76), 0.15)

    hs = W.region(hx1 - hx0, HH)
    brick(hs, 0, 0, hs.w, HH, red, 81)
    hs.rect(2.0, 0, 5.0, 3.2, (52, 84, 70), -0.12); window(hs, 7.0, 1.4, 11.0, 3.0, 1.0, frame=(40, 62, 54))
    hs.rect(0, HH - 0.3, hs.w, HH, (196, 184, 160), 0.1)

    # --- roofs: slate-grey standing seam on the gable, membrane on the wing
    slope_len = math.hypot((cx1 - cx0) / 2 - mon_w / 2, RIDGE - CH)
    sl = R.region(cz1 - cz0, slope_len)
    sl.rect(0, 0, sl.w, sl.h, (78, 82, 86))
    for z in np.arange(0, sl.w, 0.55): sl.rect(z, 0, z + 0.06, sl.h, (100, 104, 108), 0.08)
    for z in np.arange(3.0, sl.w - 3, 6.0): window(sl, z, 1.5, z + 1.6, sl.h - 1.5, 1.6)   # rooflights between bays
    sl.grain(3)
    mr = R.region(cz1 - cz0, mon_w)
    mr.rect(0, 0, mr.w, mr.h, (78, 82, 86))
    for z in np.arange(0, mr.w, 0.55): mr.rect(z, 0, z + 0.06, mr.h, (100, 104, 108), 0.08)
    wr = R.region(wx1 - wx0, wz1 - wz0)
    wr.rect(0, 0, wr.w, wr.h, (190, 190, 186), 0.0, noise=5, seed=91)
    for a, b, c, d in ((0, 0, wr.w, 0.35), (0, wr.h - 0.35, wr.w, wr.h), (0, 0, 0.35, wr.h), (wr.w - 0.35, 0, wr.w, wr.h)):
        wr.rect(a, b, c, d, (120, 120, 118), 0.3)
    wr.rect(16, 6, 20, 10, (120, 124, 126), 0.4); wr.rect(16.3, 6.3, 19.7, 9.7, (88, 92, 94), 0.5)
    # solar panels on the wing (refurbished)
    for x in np.arange(1.5, 14, 2.2):
        for z in np.arange(2.0, wr.h - 3, 3.2): wr.rect(x, z, x + 2.0, z + 2.8, (34, 46, 74), 0.25); wr.rect(x + 0.95, z, x + 1.05, z + 2.8, (120, 130, 150), 0.26)
    hr = R.region(hx1 - hx0, hz1 - hz0)
    hr.rect(0, 0, hr.w, hr.h, (104, 106, 106), 0.0, noise=5, seed=92)

    # --- yard: 4 roads fan into one lead at the street (x = -21)
    Y = Yard(hw, hd, ppm=16)
    Y.rect(-hw, -hd, hw, hd, (150, 148, 142)); Y.slabs(-hw, -hd, hw, hd, 5.0)
    Y.grass(24, -10, hw - 1, 13)
    Y.rect(24, -30, 38, -10, (128, 126, 120)); Y.fence(23, -30, 39, -10)
    Y.stalls(-4, 15, 30, 30)                                            # small front lot for staff
    Y.rect(-6, 13, 22, 15, (180, 178, 172))
    for x in roads:
        Y.track([(x, cz0), (x, cz1 + 2)] + bez((x, cz1 + 2), (x, cz1 + 10), (-21.0, cz1 + 8), (-21.0, cz1 + 16))[1:] + [(-21.0, hd)])

    side, wf = to_spom(side, "spom_barn_side"), to_spom(wf, "spom_wing_front")
    front_sp = to_spom(front, "spom_barn_front", CH)                   # the gable triangle above stays in the walls atlas
    ground(m, None, hw, hd)
    # carbarn: walls + gable ends + two roof slopes + monitor (2 sides, 2 ends, roof)
    xm = (cx0 + cx1) / 2
    block(m, cx0, cx1, cz0, cz1, 0, CH, None, None, side, side, None, faces="lr")
    for z, s in ((cz1, 1), (cz0, -1)):                                 # pentagonal gable ends: wall quad + gable triangle
        xx = (cx0, cx1) if s > 0 else (cx1, cx0)
        wall_z(m, front_sp, z, s, cx0, cx1, 0, CH)
        u0, u1 = (0, front.w) if s > 0 else (front.w, 0)
        m.tri("walls", [(xx[0], CH, z), (xx[1], CH, z), (xm, RIDGE, z)],
              [front.uv(u0 if s > 0 else 0, CH), front.uv(u1 if s > 0 else front.w, CH), front.uv(front.w / 2, RIDGE)], np.array((0, 0, s), float))
    for s in (-1, 1):                                                  # slopes up to the monitor
        xa = cx0 if s < 0 else cx1; xb = xm + s * mon_w / 2
        yb = CH + (RIDGE - CH) * (abs(xa - xm) - mon_w / 2) / abs(xa - xm)
        ov = 0.6
        xa2 = xa + s * ov; ya2 = CH - (RIDGE - CH) / abs(xa - xm) * ov
        m.quad("roofs", [(xa2, ya2, cz0 - 0.4), (xa2, ya2, cz1 + 0.4), (xb, yb, cz1 + 0.4), (xb, yb, cz0 - 0.4)],
               [sl.uv(0, 0), sl.uv(sl.w, 0), sl.uv(sl.w, sl.h), sl.uv(0, sl.h)], np.array((s, 2, 0.0)))
        wall_x(m, mon, xb, s, cz0, cz1, yb, yb + mon_h)
    yb = CH + (RIDGE - CH) * (abs(cx0 - xm) - mon_w / 2) / abs(cx0 - xm)
    for z, s in ((cz1, 1), (cz0, -1)): wall_z(m, mon_e, z, s, xm - mon_w / 2, xm + mon_w / 2, yb, yb + mon_h)
    m.quad("roofs", [(xm - mon_w / 2 - 0.3, yb + mon_h, cz1 + 0.3), (xm + mon_w / 2 + 0.3, yb + mon_h, cz1 + 0.3),
                     (xm + mon_w / 2 + 0.3, yb + mon_h, cz0 - 0.3), (xm - mon_w / 2 - 0.3, yb + mon_h, cz0 - 0.3)],
           [mr.uv(0, 0), mr.uv(0, mr.h), mr.uv(mr.w, mr.h), mr.uv(mr.w, 0)], np.array((0, 1, 0.0)))
    block(m, wx0, wx1, wz0, wz1, 0, WH, wf, ws, ws, ws, wr, faces="fbr")    # left abuts the carbarn
    block(m, hx0, hx1, hz0, hz1, 0, HH, hs, hs, hs, hs, hr)
    return m, {"yard": Y.maps(), "walls": W.maps(), "roofs": R.maps(), "faces": (side, wf, front_sp)}


# ------------------------------------------------------------------------------------------------ SPOM relief faces
# A wall with openings is one "SPOM quad" (SpomPS, color.b > 5): 2 triangles on the centre plane of a W x H x kSpomDepth volume
# whose relief is the facade itself. The wall surface sits FACE m out from the volume's back, pilasters and cornices stand proud
# of it, windows sink 0.3 m and the tram portals ~0.85 m with the roll-up doors set back inside, so reveals, sills and soffits are
# real silhouettes with self-shadowing. The quad is placed so the wall surface lands on the building's wall line.
FACE = 0.9


class SpomFace:
    def __init__(self, name, w, h, alb, hgt, ppm):
        self.name, self.w, self.h, self.alb, self.hgt, self.ppm, self.q = name, w, h, alb, hgt, ppm, []

    def add(self, corner, N, W, H):
        N = np.array(N, np.float32); up = np.array((0, 1, 0), np.float32); T = np.cross(up, N)
        c = np.array(corner, np.float32) - N * (FACE - spom.DEPTH / 2)        # the centre plane, behind the wall line
        self.q.append(([c, c + T * W, c + T * W + up * H, c + up * H], N, [(0, 0), (W, 0), (W, H), (0, H)], [(W, H, 10.0 + spom.DEPTH / 2)] * 4))

    def encode(self):
        end = np.clip(FACE + self.hgt, 0.02, spom.DEPTH) / spom.DEPTH
        hb = np.asarray(Image.fromarray((np.clip(self.hgt + 1, 0, 2) * 120).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.7)), float) / 120
        gy, gx = np.gradient(hb * self.ppm * 0.08 * 2.0)
        l = np.sqrt(gx ** 2 + gy ** 2 + 1)
        nm = (np.stack([-gx / l, gy / l, 1 / l], -1) * 0.5 + 0.5) * 255
        alb = np.dstack([self.alb.clip(0, 255), end * 255]).astype(np.uint8)
        nor = np.dstack([nm, np.zeros_like(end)]).astype(np.uint8)
        return spom.png(alb), spom.png(nor)


def to_spom(reg, name, h=None):
    """Lift a painted atlas region (its bottom h metres) out into a SPOM relief face of its own."""
    at, p = reg.at, reg.at.ppm; h = h or reg.h
    r0, r1 = reg.py + int(round((reg.h - h) * p)), reg.py + int(round(reg.h * p))
    c0, c1 = reg.px, reg.px + int(round(reg.w * p))
    return SpomFace(name, reg.w, h, at.alb[r0:r1, c0:c1].copy(), at.h[r0:r1, c0:c1].copy(), p)


class Faces:
    def __init__(self, faces): self.q = {f.name: f.q for f in faces}
    def tris(self): return sum(2 * len(v) for v in self.q.values())


def append_spom(path, S, reliefs):
    """Add the SPOM cards to a GLB written by gen_cars.write_glb: one primitive per relief, COLOR_0 = (W, H, 10 + depth)."""
    with open(path, "rb") as f: b = f.read()
    jl = struct.unpack("<I", b[12:16])[0]; g = json.loads(b[20:20 + jl]); bin_ = bytearray(b[20 + jl + 8:])
    def view(data, target=None):
        while len(bin_) % 4: bin_.append(0)
        v = {"buffer": 0, "byteOffset": len(bin_), "byteLength": len(data)}; bin_.extend(data)
        if target: v["target"] = target
        g["bufferViews"].append(v); return len(g["bufferViews"]) - 1
    def img(data):
        g["images"].append({"bufferView": view(data), "mimeType": "image/png"}); g["textures"].append({"source": len(g["images"]) - 1})
        return len(g["textures"]) - 1
    for name, quads in S.q.items():
        pos, nrm, uv, col, idx = [], [], [], [], []
        for pts, n, uvs, cols in quads:
            k = len(pos); pos += pts; nrm += [n] * 4; uv += uvs; col += cols; idx += [k, k + 1, k + 2, k, k + 2, k + 3]
        arr = [np.array(x, np.float32) for x in (pos, nrm, uv, col)]; ia = np.array(idx, np.uint16)
        a = len(g["accessors"])
        g["accessors"] += [{"bufferView": view(arr[0].tobytes(), 34962), "componentType": 5126, "count": len(pos), "type": "VEC3", "min": arr[0].min(0).tolist(), "max": arr[0].max(0).tolist()},
                           {"bufferView": view(arr[1].tobytes(), 34962), "componentType": 5126, "count": len(pos), "type": "VEC3"},
                           {"bufferView": view(arr[2].tobytes(), 34962), "componentType": 5126, "count": len(pos), "type": "VEC2"},
                           {"bufferView": view(arr[3].tobytes(), 34962), "componentType": 5126, "count": len(pos), "type": "VEC3"},
                           {"bufferView": view(ia.tobytes(), 34963), "componentType": 5123, "count": len(ia), "type": "SCALAR"}]
        alb, nor = reliefs[name]
        g["materials"].append({"name": name, "pbrMetallicRoughness": {"baseColorTexture": {"index": img(alb)}, "baseColorFactor": [1, 1, 1, 1],
                               "metallicFactor": 0.05, "roughnessFactor": 0.75}, "normalTexture": {"index": img(nor)}})
        g["meshes"][0]["primitives"].append({"attributes": {"POSITION": a, "NORMAL": a + 1, "TEXCOORD_0": a + 2, "COLOR_0": a + 3},
                                             "indices": a + 4, "material": len(g["materials"]) - 1})
    while len(bin_) % 4: bin_.append(0)
    g["buffers"][0]["byteLength"] = len(bin_)
    j = json.dumps(g, separators=(",", ":")).encode(); j += b" " * (-len(j) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 28 + len(j) + len(bin_)))
        f.write(struct.pack("<II", len(j), 0x4E4F534A)); f.write(j)
        f.write(struct.pack("<II", len(bin_), 0x004E4942)); f.write(bytes(bin_))


def main():
    os.makedirs(OUT, exist_ok=True)
    gc.MAT_PBR.update({"yard": (0.0, 0.92), "walls": (0.05, 0.7), "roofs": (0.25, 0.55)})
    for name, fn in (("TRAMDEPOT_19x17_tram_depot_L1", tram_depot), ("TRAMDEPOT_10x8_compact_refurbished_L1", compact_depot)):
        m, maps = fn()
        faces = maps.pop("faces"); S = Faces(faces); reliefs = {f.name: f.encode() for f in faces}
        out = os.path.join(OUT, name + ".glb")
        gc.write_glb(out, m, maps, (1, 1, 1))
        append_spom(out, S, reliefs)
        mats = sum(1 for v in m.tris.values() if v) + len(S.q)
        print(f"{name}: {m.count()} + {S.tris()} SPOM = {m.count() + S.tris()} tris, {mats} materials, {os.path.getsize(out) / 1e6:.2f} MB")


if __name__ == "__main__":
    main()
