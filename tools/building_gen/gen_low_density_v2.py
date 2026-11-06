"""Low-density houses, v2: composed massing (wings, cross-gables, hips, dormers, porches, garages, carports, chimneys).

A house is a list of WINGS (rectangles with a wall height and a roof type). Each wing's walls and roof are real low-poly geometry; wings
interpenetrate their neighbours, so cross-gables, valleys and hips come out right without any boolean work. Detail (siding, shingles,
windows, doors) stays in the normal-mapped textures, balconies and canopies stay silhouette-POM volumes (spom.py).

Run:  python gen_low_density_v2.py [levels...]     Output: assets/buildings/low_density/glb/LD_<W>x<D>_L<level>.glb  (replaces v1)
Units: metres, +Y up, front faces +Z, origin at the lot centre on the ground. One lot cell is 8 m.
"""
import os, sys, math
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import gen_low_density as g
import spom
from gen_low_density import Mesh, box, deco, write_glb, tile_maps, detail_sheet, canopy_over, balcony_at, LEVELS, LOT_TYPES, CELL, SRC, OUT, TEX

SH = {1: 2.5, 2: 2.7, 3: 2.8, 4: 3.0, 5: 3.1}   # storey height per level


# ---------------------------------------------------------------- geometry helpers
def _scatter(p):
    """A repeatable pseudo-random pair in 0..1 from a polygon's first corner: each roof plane / gable end starts its texture at a different phase."""
    v = np.sin(np.array([p[0][0] * 12.9898 + p[0][2] * 78.233 + p[0][1] * 37.719, p[0][0] * 93.989 + p[0][2] * 67.345 + p[0][1] * 11.135])) * 43758.5453
    return v - np.floor(v)


def _plane_uv(p, rm):
    """UVs for a planar polygon in true surface metres (no stretch on a slope), started at a per-plane random phase."""
    e1 = p[1] - p[0]; e1 /= np.linalg.norm(e1)
    n = np.cross(p[1] - p[0], p[-1] - p[0]); n /= np.linalg.norm(n)
    e2 = np.cross(n, e1)
    off = _scatter(p)
    return [(float(np.dot(q - p[0], e1)) / rm + off[0], float(np.dot(q - p[0], e2)) / rm + off[1]) for q in p]


def roof_poly(m, pts, rm):
    """A roof plane (quad or triangle), wound so its normal faces up."""
    p = [np.array(v, np.float32) for v in pts]
    n = np.cross(p[1] - p[0], p[-1] - p[0])
    if n[1] < 0: p = [p[0]] + p[1:][::-1]
    uv = _plane_uv(p, rm)
    (m.quad if len(p) == 4 else m.tri)("roof", *p, uv)


def wall_poly(m, pts, outward, wm):
    """A wall piece (quad or triangle) wound to face `outward`, UVs in world metres along the wall."""
    p = [np.array(v, np.float32) for v in pts]
    n = np.cross(p[1] - p[0], p[-1] - p[0])
    if np.dot(n, outward) < 0: p = [p[0]] + p[1:][::-1]
    ax = 2 if abs(outward[0]) > abs(outward[2]) else 0
    off = _scatter(p)
    uv = [(float(q[ax]) / wm + off[0], -float(q[1]) / wm + off[1]) for q in p]
    (m.quad if len(p) == 4 else m.tri)("wall", *p, uv)


def gable(m, x0, x1, z0, z1, y, rise, over, wm, rm, axis):
    """Gable roof over a rectangle at eave height y; ridge along `axis`; overhang `over`; gable ends in the wall material."""
    yr = y + rise
    if axis == "x":
        zm = (z0 + z1) / 2; ye = y - over * rise / ((z1 - z0) / 2)
        roof_poly(m, [(x0 - over, ye, z1 + over), (x1 + over, ye, z1 + over), (x1 + over, yr, zm), (x0 - over, yr, zm)], rm)
        roof_poly(m, [(x1 + over, ye, z0 - over), (x0 - over, ye, z0 - over), (x0 - over, yr, zm), (x1 + over, yr, zm)], rm)
        wall_poly(m, [(x1, y, z1), (x1, y, z0), (x1, yr, zm)], (1, 0, 0), wm)
        wall_poly(m, [(x0, y, z0), (x0, y, z1), (x0, yr, zm)], (-1, 0, 0), wm)
    else:
        xm = (x0 + x1) / 2; ye = y - over * rise / ((x1 - x0) / 2)
        roof_poly(m, [(x1 + over, ye, z1 + over), (x1 + over, ye, z0 - over), (xm, yr, z0 - over), (xm, yr, z1 + over)], rm)
        roof_poly(m, [(x0 - over, ye, z0 - over), (x0 - over, ye, z1 + over), (xm, yr, z1 + over), (xm, yr, z0 - over)], rm)
        wall_poly(m, [(x0, y, z1), (x1, y, z1), (xm, yr, z1)], (0, 0, 1), wm)
        wall_poly(m, [(x1, y, z0), (x0, y, z0), (xm, yr, z0)], (0, 0, -1), wm)


def hip(m, x0, x1, z0, z1, y, rise, over, rm):
    """Hip roof: four planes, ridge along the longer side (a pyramid on a square)."""
    w, d = x1 - x0, z1 - z0
    hs = min(w, d) / 2; ye = y - over * rise / hs; yr = y + rise
    xs0, xs1, zs0, zs1 = x0 - over, x1 + over, z0 - over, z1 + over
    if w >= d:
        zm = (z0 + z1) / 2; rx0, rx1 = min(x0 + hs, (x0 + x1) / 2), max(x1 - hs, (x0 + x1) / 2)
        roof_poly(m, [(xs0, ye, zs1), (xs1, ye, zs1), (rx1, yr, zm), (rx0, yr, zm)], rm)
        roof_poly(m, [(xs1, ye, zs0), (xs0, ye, zs0), (rx0, yr, zm), (rx1, yr, zm)], rm)
        roof_poly(m, [(xs0, ye, zs0), (xs0, ye, zs1), (rx0, yr, zm)], rm)
        roof_poly(m, [(xs1, ye, zs1), (xs1, ye, zs0), (rx1, yr, zm)], rm)
    else:
        xm = (x0 + x1) / 2; rz0, rz1 = min(z0 + hs, (z0 + z1) / 2), max(z1 - hs, (z0 + z1) / 2)
        roof_poly(m, [(xs1, ye, zs1), (xs1, ye, zs0), (xm, yr, rz0), (xm, yr, rz1)], rm)
        roof_poly(m, [(xs0, ye, zs0), (xs0, ye, zs1), (xm, yr, rz1), (xm, yr, rz0)], rm)
        roof_poly(m, [(xs0, ye, zs1), (xs1, ye, zs1), (xm, yr, rz1)], rm)
        roof_poly(m, [(xs1, ye, zs0), (xs0, ye, zs0), (xm, yr, rz0)], rm)


def flat_slab(m, x0, x1, z0, z1, y, thick, over, wm, rm):
    """A flat roof: a thin slab with an overhang (wall material on its edge, roof material on top)."""
    box(m, x0 - over, x1 + over, y, y + thick, z0 - over, z1 + over, wm, top="roof", roof_m=rm)


def posts(m, pts, y0, y1, wm, s=0.16):
    for (x, z) in pts:
        box(m, x - s / 2, x + s / 2, y0, y1, z - s / 2, z + s / 2, wm, skip=("top",))


def chimney(m, x, z, y0, y1, wm, rm):
    box(m, x - 0.4, x + 0.4, y0, y1, z - 0.35, z + 0.35, wm * 0.6, skip=("top",))
    box(m, x - 0.5, x + 0.5, y1, y1 + 0.18, z - 0.45, z + 0.45, wm * 0.6, top="roof", roof_m=rm)


class House:
    def __init__(self, p, level):
        self.m = Mesh(); self.p = p; self.level = level
        self.wm, self.rm = p["wall_m"], p["roof_m"]; self.sh = SH[level]
        self.wings = []      # dicts: x0 x1 z0 z1 floors
        self.reserved = []   # (side, a0, a1, y0, y1): spans windows must avoid
        self.top = 0.0
        self._n = 0
        self.front_door = None   # (x, z) of the first door placed
        self.drive = None        # (x centre, z where the car stops, width): set by a garage or carport

    # a wing: walls + the roof you pass
    def wing(self, x0, x1, z0, z1, floors, roof, rise=None, over=0.45, axis=None, extend_back=0.0, name=""):
        m, sh = self.m, self.sh
        H = floors * sh + 0.15
        self._n += 1
        rng = np.random.default_rng(self._n * 7919 + self.level * 104729)
        wmj = self.wm * float(rng.uniform(0.9, 1.1))         # each wing's wall texture has its own scale and phase, so they never line up
        box(m, x0, x1, 0, H, z0 - extend_back, z1, wmj, skip=("top",), uvoff=(float(rng.random()), float(rng.random())))
        box(m, x0 - 0.06, x1 + 0.06, 0, 0.5, z0 - extend_back - 0.06, z1 + 0.06, self.wm * 0.5, top="roof", roof_m=self.rm * 0.5)   # stone plinth with a ledge
        w, d = x1 - x0, z1 - z0
        if axis is None: axis = "x" if w >= d else "z"
        hs = (d if axis == "x" else w) / 2
        if rise is None: rise = hs * 0.62
        if roof == "gable": gable(m, x0, x1, z0 - extend_back, z1, H, rise, over, wmj, self.rm, axis)
        elif roof == "hip": hip(m, x0, x1, z0 - extend_back, z1, H, rise, over, self.rm)
        elif roof == "flat": flat_slab(m, x0, x1, z0 - extend_back, z1, H, 0.22, over, self.wm, self.rm); rise = 0.22
        self.wings.append(dict(x0=x0, x1=x1, z0=z0 - extend_back, z1=z1, floors=floors, H=H, name=name))
        self.top = max(self.top, H + rise)
        return H, rise

    def covered(self, x, z, y, skip):
        for w in self.wings:
            if w is skip: continue
            if w["x0"] - 0.05 < x < w["x1"] + 0.05 and w["z0"] - 0.05 < z < w["z1"] + 0.05 and y < w["H"] + 0.1: return True
        return False

    def windows(self, kind="win", ww=1.2, wh=1.4, spacing=3.1, sides=("front", "back", "left", "right"), only=None, sill=0.95):
        sh = self.sh
        for w in self.wings:
            if only and w["name"] not in only: continue
            for side in sides:
                if side in ("front", "back"):
                    a0, a1, zc, nz = w["x0"], w["x1"], (w["z1"] if side == "front" else w["z0"]), (1 if side == "front" else -1)
                else:
                    a0, a1, xc, nx = w["z0"], w["z1"], (w["x1"] if side == "right" else w["x0"]), (1 if side == "right" else -1)
                L = a1 - a0
                n = max(1, int(L // spacing))
                for f in range(w["floors"]):
                    y = f * sh + sill + wh / 2 + 0.15
                    for i in range(n):
                        c = a0 + (i + 0.5) * L / n
                        if c - ww / 2 < a0 + 0.4 or c + ww / 2 > a1 - 0.4: continue
                        if any(s == side and c + ww / 2 > r0 and c - ww / 2 < r1 and y + wh / 2 > y0 and y - wh / 2 < y1 for (s, r0, r1, y0, y1) in self.reserved if True): continue
                        if side in ("front", "back"):
                            if self.covered(c, zc + nz * 0.4, y, w): continue
                            deco(self.m, kind, c, y, zc, ww, wh, nz, "x")
                        else:
                            if self.covered(xc + nx * 0.4, c, y, w): continue
                            deco(self.m, kind, c, y, xc, ww, wh, nx, "z")

    def door(self, cx, z, kind="door", w=1.1, h=2.2):
        if self.front_door is None: self.front_door = (cx, z)
        deco(self.m, kind, cx, h / 2 + 0.12, z, w, h, +1, "x")
        self.reserved.append(("front", cx - w / 2 - 0.3, cx + w / 2 + 0.3, 0, h + 0.3))

    def porch(self, x0, x1, z_wall, depth, y_roof, shed_rise=0.5, post_step=2.4, gable_front=False):
        """Deck + posts + a roof sloping out from the wall (or a front gable)."""
        m, wm, rm = self.m, self.wm, self.rm
        box(m, x0, x1, 0, 0.3, z_wall, z_wall + depth, wm, top="roof", roof_m=rm * 0.6)
        n = max(2, int(round((x1 - x0) / post_step)) + 1)
        pts = [(x0 + 0.18 + i * (x1 - x0 - 0.36) / (n - 1), z_wall + depth - 0.2) for i in range(n)]
        posts(m, pts, 0.3, y_roof, wm)
        cxp = (x0 + x1) / 2
        box(m, cxp - 0.7, cxp + 0.7, 0, 0.16, z_wall + depth, z_wall + depth + 0.4, wm * 0.5, top="roof", roof_m=rm * 0.5)     # entry step
        if x1 - x0 >= 4.2:                                                                                                # railings (silhouette POM), a gap at the steps
            nseg = int(math.ceil((x1 - x0 - 0.3) / 3.0))
            for i in range(nseg):
                c = x0 + 0.15 + (i + 0.5) * (x1 - x0 - 0.3) / nseg
                if abs(c - cxp) < 1.3 and nseg > 1: continue
                spom.spom_box(m, "rail", np.array([c - 1.5, 0.3, z_wall + depth - 0.2 - 1.0], np.float32), np.array([0, 0, 1], np.float32))
        if gable_front:
            gable(m, x0, x1, z_wall, z_wall + depth, y_roof, (x1 - x0) * 0.28, 0.3, wm, rm, "z")
        else:
            roof_poly(m, [(x0 - 0.25, y_roof, z_wall + depth + 0.3), (x1 + 0.25, y_roof, z_wall + depth + 0.3),
                          (x1 + 0.25, y_roof + shed_rise, z_wall), (x0 - 0.25, y_roof + shed_rise, z_wall)], rm)

    def dormer(self, cx, main, y_eave, rise, w=1.5):
        """A small gable dormer on the main roof's front slope (main ridge along x)."""
        m = self.m
        z1, zm = main["z1"], (main["z0"] + main["z1"]) / 2
        zf = z1 - 0.9
        yr = lambda z: y_eave + rise * (z1 - z) / (z1 - zm)
        yb = yr(zf) - 0.15; yt = yb + 1.55
        box(m, cx - w / 2, cx + w / 2, yb, yt, zm + 0.2, zf, self.wm, skip=("top", "back", "front"))
        wall_poly(m, [(cx - w / 2, yb, zf), (cx + w / 2, yb, zf), (cx + w / 2, yt, zf), (cx - w / 2, yt, zf)], (0, 0, 1), self.wm)
        gable(m, cx - w / 2, cx + w / 2, zm + 0.2, zf, yt, w * 0.42, 0.25, self.wm, self.rm, "z")
        deco(m, "win", cx, yb + 0.8, zf, 0.9, 1.05, +1, "x")

    def bay(self, cx, z_wall, y0, w=2.2, d=0.7, h=1.9):
        m = self.m
        box(m, cx - w / 2, cx + w / 2, y0, y0 + h, z_wall, z_wall + d, self.wm, skip=("top", "back"))
        roof_poly(m, [(cx - w / 2 - 0.15, y0 + h, z_wall + d + 0.15), (cx + w / 2 + 0.15, y0 + h, z_wall + d + 0.15),
                      (cx + w / 2 + 0.15, y0 + h + 0.35, z_wall), (cx - w / 2 - 0.15, y0 + h + 0.35, z_wall)], self.rm)
        deco(m, "wide", cx, y0 + h / 2 + 0.05, z_wall + d, w - 0.3, h - 0.5, +1, "x")


# ---------------------------------------------------------------- the five levels
ROAD_TEX = os.path.join(g.ROOT, "assets", "roads", "tex")
DRIVE_MAT = {1: ("gravel", 3.0), 2: ("asphalt_worn", 4.0), 3: ("asphalt", 4.0), 4: ("pavers", 2.0), 5: ("concrete", 3.0)}   # the road system's materials


def drive_tex(level):
    from PIL import Image
    name, _ = DRIVE_MAT[level]
    alb = Image.open(os.path.join(ROAD_TEX, f"{name}_albedo.png")).convert("RGB").resize((512, 512), Image.LANCZOS)
    nor = Image.open(os.path.join(ROAD_TEX, f"{name}_normal.png")).convert("RGB").resize((512, 512), Image.LANCZOS)
    return g.jpg(np.asarray(alb)), g.png(np.asarray(nor))


def add_ground(h, lw, ld, level):
    """A driveway from the garage (or beside the house) to the lot's front edge, flared where it meets the road's sidewalk,
    and a front walk from the door. Material: the road system's own (assets/roads/tex)."""
    m = h.m; tile = DRIVE_MAT[level][1]; zE = ld / 2; y = 0.03
    def ground(x0, x1, za, zb, yy, flare=0.0):
        """Back edge za, front edge zb (zb > za); flare widens the front edge by that much each side."""
        if zb - za < 0.05: return
        pts = [(x0, yy, za), (x0 - flare, yy, zb), (x1 + flare, yy, zb), (x1, yy, za)]
        m.quad("drive", *pts, [(q[0] / tile, q[2] / tile) for q in pts])
    if h.drive is None:                                         # no garage: a pad on the roomier side of the house
        xs0 = min(w["x0"] for w in h.wings); xs1 = max(w["x1"] for w in h.wings)
        left, right = xs0 + lw / 2, lw / 2 - xs1
        w = 2.8
        if right >= left: xc = min(max(xs1 + 0.8 + w / 2, 0), lw / 2 - 0.6 - w / 2)
        else: xc = max(min(xs0 - 0.8 - w / 2, 0), -lw / 2 + 0.6 + w / 2)
        h.drive = (xc, h.wings[0]["z1"], w)
    xc, zs, w = h.drive
    xc = max(min(xc, lw / 2 - w / 2 - 0.3), -lw / 2 + w / 2 + 0.3)
    ground(xc - w / 2, xc + w / 2, zs, zE - 1.8, y)
    ground(xc - w / 2, xc + w / 2, zE - 1.8, zE, y, flare=0.7)           # the apron
    if h.front_door:
        dx, dz = h.front_door
        if abs(dx - xc) > w / 2 + 1.1: ground(dx - 0.65, dx + 0.65, dz, zE, y + 0.005)


def build(W, D, level):
    p = LEVELS[level]; h = House(p, level); m = h.m
    sh = h.sh; wm, rm = h.wm, h.rm
    lw, ld = W * CELL, D * CELL
    xr = lw / 2 - 1.5                      # buildable x range [-xr, xr]
    zb, zf = -ld / 2 + 2.0, ld / 2 - 3.2   # back and front limits of the building zone
    big = W >= 3                           # room for a garage or carport wing
    deep = D >= 3
    flip = 1 if (W + D) % 2 == 0 else -1   # mirror the composition on alternate lots so a street does not repeat
    X = lambda x: flip * x                 # mirrored x (rects are re-sorted below)

    def rect(xa, xb): a, b = sorted((X(xa), X(xb))); return a, b

    if level == 1:                                                   # shack: one room, a lean-to, a hat of tin
        mw, md = 5.6 + 0.9 * (W - 2), 4.6 + 0.6 * (D - 2)
        x0, x1 = rect(-mw / 2, mw / 2); z1 = min(zf - 1.0, zb + md + 1.0); z0 = z1 - md
        main = dict(); H, rise = h.wing(x0, x1, z0, z1, 1, "gable", rise=md * 0.2, over=0.5, axis="x", name="main")
        hi, sgn = (x0, -1) if flip > 0 else (x1, 1)                     # lean-to on the side, its roof falling away from the main wall
        lo = hi + sgn * 2.4; lz0, lz1 = z0 + 0.5, z1 - 0.5; yhi, ylo = 2.2, 1.8
        roof_poly(m, [(lo + sgn * 0.25, ylo - 0.1, lz1 + 0.2), (hi, yhi, lz1 + 0.2), (hi, yhi, lz0 - 0.2), (lo + sgn * 0.25, ylo - 0.1, lz0 - 0.2)], rm)
        wall_poly(m, [(lo, 0, lz1), (hi, 0, lz1), (hi, yhi, lz1), (lo, ylo, lz1)], (0, 0, 1), wm)
        wall_poly(m, [(hi, 0, lz0), (lo, 0, lz0), (lo, ylo, lz0), (hi, yhi, lz0)], (0, 0, -1), wm)
        wall_poly(m, [(lo, 0, lz1), (lo, 0, lz0), (lo, ylo, lz0), (lo, ylo, lz1)], (sgn, 0, 0), wm)
        h.wings.append(dict(x0=min(lo, hi), x1=max(lo, hi), z0=lz0, z1=lz1, floors=1, H=ylo, name="lean"))
        dx = x0 + mw * (0.28 if flip > 0 else 0.72)
        h.door(dx, z1); canopy_over(m, dx, 2.35, z1)
        h.windows(only=("main",))
        chimney(m, (x0 + x1) / 2 + flip * 0.9, (z0 + z1) / 2 - 0.5, 1.6, H + rise + 0.9, wm, rm)

    elif level == 2:                                                 # cottage: gable + side front-gable wing + gabled porch
        mw, md = 7.4 + 1.2 * (W - 2), 5.8 + 0.7 * (D - 2)
        ww_ = 3.6
        tot = mw + ww_
        xa = -tot / 2
        mx0, mx1 = rect(xa, xa + mw); wx0, wx1 = rect(xa + mw - 0.6, xa + tot)
        z1 = min(zf - 0.5, zb + md + 1.0); z0 = z1 - md
        H, rise = h.wing(mx0, mx1, z0, z1, 1, "gable", rise=md * 0.5 * 0.72, over=0.55, axis="x", name="main")
        h.wing(wx0, wx1, z0 + 1.6, z1 + 1.1, 1, "gable", rise=ww_ * 0.5 * 0.85, over=0.5, axis="z", name="wing")
        px0, px1 = (mx0 + 0.6, mx0 + 3.8) if flip > 0 else (mx1 - 3.8, mx1 - 0.6)
        h.porch(px0, px1, z1, 1.9, 2.55, gable_front=True)
        dx = (px0 + px1) / 2
        h.door(dx, z1)
        h.windows(only=("main", "wing"))
        chimney(m, mx0 + (mx1 - mx0) * (0.72 if flip > 0 else 0.28), (z0 + z1) / 2, 2.4, H + rise + 1.0, wm, rm)

    elif level == 3:                                                 # two-storey: cross gable, full porch, bay, hip garage
        mw, md = 8.6 + 1.2 * (W - 2), 6.4 + 0.8 * (D - 2)
        mx0, mx1 = rect(-mw / 2 - (1.2 if big else 0), mw / 2 - (1.2 if big else 0))
        z1 = min(zf - 1.2, zb + md + 1.0); z0 = z1 - md
        H, rise = h.wing(mx0, mx1, z0, z1, 2, "gable", rise=md * 0.5 * 0.7, over=0.55, axis="x", name="main")
        cw = 4.2
        cx0, cx1 = (mx0 + 0.6, mx0 + 0.6 + cw) if flip > 0 else (mx1 - 0.6 - cw, mx1 - 0.6)
        h.wing(cx0, cx1, z0 + 1.0, z1 + 1.6, 2, "gable", rise=cw * 0.5 * 0.85, over=0.5, axis="z", name="cross")
        # porch on the front of the main body beside the cross gable
        px0, px1 = (cx1, mx1 - 0.4) if flip > 0 else (mx0 + 0.4, cx0)
        h.porch(px0, px1, z1, 2.0, sh + 0.05, shed_rise=0.55)
        h.door((px0 + px1) / 2, z1)
        h.bay((cx0 + cx1) / 2, z1 + 1.6, 0.15)
        h.reserved.append(("front", (cx0 + cx1) / 2 - 1.3, (cx0 + cx1) / 2 + 1.3, 0, 2.3))
        if big:                                                      # hip-roofed garage on the far side
            gw = 3.8
            gx0, gx1 = (mx1, mx1 + gw) if flip > 0 else (mx0 - gw, mx0)
            gz1 = z1 - 0.3; gz0 = z0 + 0.8
            h.wing(gx0, gx1, gz0, gz1, 1, "hip", rise=1.3, over=0.4, name="garage")
            deco(m, "garage", (gx0 + gx1) / 2, 1.2, gz1, 3.0, 2.3, +1, "x")
            canopy_over(m, (gx0 + gx1) / 2, 2.55, gz1)
            h.reserved.append(("front", gx0, gx1, 0, 3))
            h.drive = ((gx0 + gx1) / 2, gz1, 3.4)
        h.windows(only=("main", "cross", "garage"))
        chimney(m, (mx0 + mx1) / 2, (z0 + z1) / 2 - 0.8, 3.0, H + rise + 1.0, wm, rm)

    elif level == 4:                                                 # grand: two front gables, dormers, wrap porch, garage, balcony
        mw, md = 9.6 + 1.2 * (W - 2), 7.0 + 0.8 * (D - 2)
        gar = 4.6 if big else 0.0
        mx0, mx1 = rect(-mw / 2 - gar / 2, mw / 2 - gar / 2)
        z1 = min(zf - 1.4, zb + md + 1.2); z0 = z1 - md
        H, rise = h.wing(mx0, mx1, z0, z1, 2, "gable", rise=md * 0.5 * 0.72, over=0.6, axis="x", name="main")
        w1, w2 = 4.6, 3.8
        a0, a1 = (mx0 + 0.4, mx0 + 0.4 + w1) if flip > 0 else (mx1 - 0.4 - w1, mx1 - 0.4)
        b0, b1 = (mx1 - 0.4 - w2, mx1 - 0.4) if flip > 0 else (mx0 + 0.4, mx0 + 0.4 + w2)
        h.wing(a0, a1, z0 + 1.0, z1 + 2.4, 2, "gable", rise=w1 * 0.5 * 0.9, over=0.55, axis="z", name="gA")
        h.wing(b0, b1, z0 + 1.0, z1 + 1.2, 2, "gable", rise=w2 * 0.5 * 0.8, over=0.5, axis="z", name="gB")
        mid0, mid1 = (a1, b0) if flip > 0 else (b1, a0)
        if mid1 - mid0 > 0.8:
            h.porch(mid0, mid1, z1, 1.8, sh + 0.05, shed_rise=0.5)
            h.door((mid0 + mid1) / 2, z1)
        else:
            h.door((a0 + a1) / 2, z1 + 2.4)
        eave = H
        h.dormer((mid0 + mid1) / 2 if mid1 - mid0 > 2.0 else (mx0 + mx1) / 2, h.wings[0], eave, rise)
        if big:                                                      # garage wing, hipped, set back
            gx0, gx1 = (mx1, mx1 + gar) if flip > 0 else (mx0 - gar, mx0)
            gz1 = z1 - 0.4; gz0 = z0 + 0.5
            h.wing(gx0, gx1, gz0, gz1, 1, "gable", rise=2.0, over=0.5, axis="z", name="garage")
            deco(m, "garage", (gx0 + gx1) / 2, 1.2, gz1, 3.2, 2.3, +1, "x")
            canopy_over(m, (gx0 + gx1) / 2, 2.55, gz1)
            h.reserved.append(("front", gx0, gx1, 0, 3))
            h.drive = ((gx0 + gx1) / 2, gz1, 3.6)
        balcony_at(m, (b0 + b1) / 2, sh + 0.15, z1 + 1.2)
        h.reserved.append(("front", (b0 + b1) / 2 - 1.6, (b0 + b1) / 2 + 1.6, sh, sh * 2))
        h.reserved.append(("front", (a0 + a1) / 2 - 0.9, (a0 + a1) / 2 + 0.9, 0, 3))
        h.windows(only=("main", "gA", "gB", "garage"))
        chimney(m, (mx0 + mx1) / 2 + (1.2 * flip), (z0 + z1) / 2 - 1.0, 3.0, H + rise + 1.0, wm, rm)

    else:                                                            # modern: stacked flat volumes, cantilever, carport, brick stack
        aw, ad = 9.6 + 1.3 * (W - 2), 6.8 + 0.8 * (D - 2)
        ax0, ax1 = rect(-aw / 2, aw / 2)
        z1 = min(zf - 1.4, zb + ad + 1.2); z0 = z1 - ad
        Ha, _ = h.wing(ax0, ax1, z0, z1, 1, "flat", over=0.5, name="A")
        bw = aw * 0.58
        bx0, bx1 = (ax0 + 0.4, ax0 + 0.4 + bw) if flip > 0 else (ax1 - 0.4 - bw, ax1 - 0.4)
        bz1 = z1 + 1.8; bz0 = z0 + 1.0
        # upper volume: walls from the first floor up, cantilevered 1.8 m over the front
        yb = Ha + 0.22
        box(m, bx0, bx1, yb, yb + sh, bz0, bz1, wm, skip=("top",))
        flat_slab(m, bx0, bx1, bz0, bz1, yb + sh, 0.22, 0.45, wm, rm)
        h.wings.append(dict(x0=bx0, x1=bx1, z0=bz0, z1=bz1, floors=1, H=yb + sh, name="B", y0=yb))
        h.top = max(h.top, yb + sh + 0.22)
        wide_w = bw - 1.6
        deco(m, "wide", (bx0 + bx1) / 2, yb + 1.6, bz1, wide_w, 1.8, +1, "x")
        deco(m, "wide", (ax0 + ax1) / 2 + flip * (aw * 0.18), 1.55, z1, aw * 0.36, 1.9, +1, "x")
        dx = ax0 + 1.4 if flip > 0 else ax1 - 1.4
        h.door(dx, z1); canopy_over(m, dx, 2.45, z1)
        if big:                                                      # carport: flat slab on slim posts, with a brick storage stack
            cw = 4.4
            cx0, cx1 = (ax1, ax1 + cw) if flip > 0 else (ax0 - cw, ax0)
            cz0, cz1 = z0 + 1.0, z1 + 0.6
            flat_slab(m, cx0, cx1, cz0, cz1, 2.6, 0.2, 0.3, wm, rm)
            posts(m, [(cx0 + 0.3, cz1 - 0.3), (cx1 - 0.3, cz1 - 0.3), (cx0 + 0.3, cz0 + 0.3), (cx1 - 0.3, cz0 + 0.3)], 0.0, 2.6, wm, s=0.2)
            box(m, (cx0 if flip > 0 else cx1 - 1.4), (cx0 + 1.4 if flip > 0 else cx1), 0, 2.6, cz0 + 0.1, cz0 + 1.2, wm * 0.6, skip=("top",))
            h.top = max(h.top, 2.8)
            h.drive = ((cx0 + cx1) / 2, cz1, 3.6)
        balcony_at(m, bx0 + bw * 0.5 + (bw * 0.3 if flip > 0 else -bw * 0.3), yb + 0.15, bz1)
        h.reserved.append(("front", bx0 + bw * 0.5 + (bw * 0.3 if flip > 0 else -bw * 0.3) - 1.6, bx0 + bw * 0.5 + (bw * 0.3 if flip > 0 else -bw * 0.3) + 1.6, yb, yb + sh))
        h.windows(only=("A",), sides=("back", "left", "right"))
        chimney(m, (ax0 + ax1) / 2 - flip * 1.8, z0 + 1.2, Ha, yb + sh + 1.6, wm, rm)
    add_ground(h, lw, ld, level)
    return h.m, (lw, ld, h.top)


def main():
    os.makedirs(OUT, exist_ok=True); os.makedirs(TEX, exist_ok=True)
    only = [int(a) for a in sys.argv[1:]] or sorted(LEVELS)
    for lv in only:
        p = LEVELS[lv]
        wp, rp = os.path.join(SRC, f"L{lv}_wall.jpg"), os.path.join(SRC, f"L{lv}_roof.jpg")
        tex = {"wall": tile_maps(wp, p["wall_k"]), "roof": tile_maps(rp, p["roof_k"]), "detail": detail_sheet(p),
               "spom_balcony": spom.relief("balcony", p), "spom_canopy": spom.relief("canopy", p), "spom_rail": spom.relief("rail", p),
               "drive": drive_tex(lv)}
        for (W, D) in LOT_TYPES:
            mesh, dims = build(W, D, lv)
            out = os.path.join(OUT, f"LD_{W}x{D}_L{lv}.glb")
            write_glb(out, mesh, tex)
            print(f"LD_{W}x{D}_L{lv}: {mesh.tri_count():3d} tris, {os.path.getsize(out)/1e6:.2f} MB, height {dims[2]:.1f} m")


if __name__ == "__main__":
    main()
