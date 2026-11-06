"""L1 high-density housing with character: every building gets its own design, and its walls are real depth (silhouette POM).

The tower box carries only a "glass field" (facade_hd.glass_field): a room, a covering and the sky reflected in every 3 m x 3 m cell.
In front of it stand SPOM wall strips, one per storey per run of up to three bays: a 30 cm wall with real openings, frames 5 cm in
front of the glass, sills, surrounds, projecting balconies, shutters, awnings, air conditioners, flower boxes, satellite dishes and
owner-glazed balconies. The ray march in SpomPS gives them parallax, silhouettes and their own shadows; through an opening the ray
leaves the volume and the glass field behind it shows.

A building's design (cladding, colour, window style, balcony pattern, accents, which owners did what) comes from one seeded draw, so
each of the 25 L1 plots is a different building; inside a building a pool of strip variants spreads the owners' changes around.
"""
import io, os, math
import numpy as np
from PIL import Image, ImageFilter

DEPTH = 1.2                 # kSpomDepth
PPM = 100                   # relief texels per metre
T_WALL = 0.30               # wall thickness in front of the glass
BAY, STOREY = 3.0, 3.0

PAINTS = {"cream": (0.93, 0.88, 0.76), "salmon": (0.92, 0.72, 0.62), "mint": (0.74, 0.86, 0.78), "butter": (0.95, 0.88, 0.62),
          "grey": (0.78, 0.78, 0.76), "terracotta": (0.80, 0.52, 0.40), "sky": (0.72, 0.82, 0.90), "white": (0.95, 0.94, 0.91),
          "ochre": (0.85, 0.68, 0.42), "lilac": (0.82, 0.76, 0.86), "sage": (0.66, 0.72, 0.62), "concrete": (0.70, 0.69, 0.66)}
ACCENTS = [(0.80, 0.25, 0.20), (0.20, 0.42, 0.65), (0.95, 0.75, 0.20), (0.30, 0.55, 0.35), (0.90, 0.50, 0.20), (0.55, 0.30, 0.55),
           (0.15, 0.17, 0.20), (0.92, 0.92, 0.90), (0.40, 0.65, 0.75), (0.70, 0.20, 0.35)]
FRAMES = [(0.93, 0.93, 0.91), (0.93, 0.93, 0.91), (0.22, 0.23, 0.25), (0.40, 0.28, 0.18), (0.24, 0.36, 0.28), (0.60, 0.62, 0.64)]
SHUTTER = [(0.30, 0.45, 0.32), (0.25, 0.35, 0.55), (0.55, 0.25, 0.20), (0.85, 0.82, 0.74), (0.45, 0.32, 0.22)]


# ---------------------------------------------------------------- design
def design(rng, wall_sources):
    d = {}
    d["clad"] = wall_sources[rng.integers(len(wall_sources))]
    d["clad_m"] = float(rng.uniform(2.0, 3.2))
    d["paint"] = np.array(list(PAINTS.values())[rng.integers(len(PAINTS))], np.float32)
    d["paint2"] = np.array(list(PAINTS.values())[rng.integers(len(PAINTS))], np.float32)        # top storeys or plinth band
    d["two_tone"] = rng.random() < 0.35
    d["panels"] = rng.random() < 0.45                                                          # prefab panel joints
    d["window"] = rng.choice(["single", "wide", "tall", "double", "square"], p=[0.25, 0.2, 0.2, 0.2, 0.15])
    d["frame"] = np.array(FRAMES[rng.integers(len(FRAMES))], np.float32)
    d["surround"] = rng.choice(["none", "band", "box"], p=[0.45, 0.3, 0.25])
    d["surround_col"] = np.array(ACCENTS[rng.integers(len(ACCENTS))], np.float32) if rng.random() < 0.4 else d["paint"] * 1.08
    d["sill_col"] = np.array((0.80, 0.79, 0.76), np.float32) * rng.uniform(0.85, 1.05)
    d["balcony"] = rng.choice(["none", "alt", "mid", "all", "ends"], p=[0.12, 0.3, 0.25, 0.18, 0.15])
    d["rail"] = rng.choice(["bars", "solid", "mesh"], p=[0.4, 0.4, 0.2])
    d["multi"] = rng.random() < 0.3                                                            # every balcony a different colour
    d["accent"] = np.array(ACCENTS[rng.integers(len(ACCENTS))], np.float32)
    d["shutters"] = rng.random() < 0.22
    d["shutter_col"] = np.array(SHUTTER[rng.integers(len(SHUTTER))], np.float32)
    d["band"] = rng.random() < 0.5                                                             # a string course at each floor
    d["awning_p"] = 0.05 + 0.15 * rng.random() ** 2
    d["ac_p"] = 0.05 + 0.3 * rng.random()
    d["flower_p"] = 0.04 + 0.2 * rng.random()
    d["enclose_p"] = 0.0 if rng.random() < 0.4 else rng.uniform(0.1, 0.45)                     # owners who glazed their balcony in
    d["dish_p"] = 0.1 * rng.random()
    return d


def describe(d):
    return (f"{d['window']} windows, {d['balcony']} balconies ({d['rail']}), surround {d['surround']}, "
            f"{'shutters, ' if d['shutters'] else ''}{'panels, ' if d['panels'] else ''}enclosed {d['enclose_p']:.0%}")


WINDOWS = {   # openings in a bay: list of (x0, x1, y0, y1, mullions, transom); y up from the storey's floor
    "single": [(0.8, 2.2, 0.9, 2.25, 1, None)],
    "wide": [(0.45, 2.55, 0.9, 2.25, 2, None)],
    "tall": [(0.95, 2.05, 0.15, 2.3, 1, 0.78)],
    "double": [(0.35, 1.3, 0.9, 2.25, 0, 0.75), (1.7, 2.65, 0.9, 2.25, 0, 0.75)],
    "square": [(0.9, 2.1, 1.0, 2.2, 0, 0.7)],
}
BALCONY_OPEN = [(0.3, 1.1, 0.18, 2.3, 0, 0.8), (1.4, 2.7, 0.9, 2.3, 1, None)]   # a door and a window onto the balcony


# ---------------------------------------------------------------- relief painter
class Relief:
    """A two-layer relief over W x H metres (x right, y up), PPM texels per metre."""
    def __init__(self, W, H, clad):
        self.W, self.H = W, H
        self.nx, self.ny = int(round(W * PPM)), int(round(H * PPM))
        self.start = np.zeros((self.ny, self.nx), np.float32); self.end = np.zeros((self.ny, self.nx), np.float32)
        self.col = np.zeros((self.ny, self.nx, 3), np.float32)
        self.clad = clad                                                   # (ny, nx, 3) cladding colour for wall texels
        self.xs = (np.arange(self.nx) + 0.5) / PPM; self.ys = H - (np.arange(self.ny) + 0.5) / PPM

    def mask(self, x0, x1, y0, y1):
        return ((self.ys >= y0) & (self.ys < y1))[:, None] & ((self.xs >= x0) & (self.xs < x1))[None, :]

    def box(self, x0, x1, y0, y1, z0, z1, col):
        m = self.mask(x0, x1, y0, y1)
        self.start[m] = z0; self.end[m] = z1
        c = np.asarray(col, np.float32)
        self.col[m] = c[m] if c.ndim == 3 else c
        return m

    def cut(self, x0, x1, y0, y1):
        m = self.mask(x0, x1, y0, y1); self.start[m] = 0; self.end[m] = 0

    def shade(self, x0, x1, y0, y1, k):
        m = self.mask(x0, x1, y0, y1); self.col[m] *= k

    def disc(self, cx, cy, r, z0, z1, col):
        m = ((self.xs[None, :] - cx) ** 2 + (self.ys[:, None] - cy) ** 2) < r * r
        self.start[m] = z0; self.end[m] = z1; self.col[m] = col

    def sloped(self, x0, x1, y_top, y_bot, z_top, z_bot, thick, col, stripes=None):
        """A plate sloping from (y_top, z_top) down and out to (y_bot, z_bot): an awning."""
        mx = (self.xs >= x0) & (self.xs < x1)
        for r, y in enumerate(self.ys):
            if y > y_top or y < y_bot: continue
            t = (y_top - y) / max(y_top - y_bot, 1e-6); z = z_top + (z_bot - z_top) * t
            self.start[r, mx] = max(0.0, z - thick); self.end[r, mx] = min(DEPTH, z + thick)
            if stripes is None: self.col[r, mx] = col
            else:
                band = ((self.xs[mx] - x0) / 0.25).astype(int) % 2
                self.col[r, mx] = np.where(band[:, None] == 0, col, stripes)

    def encode(self, seed):
        rng = np.random.default_rng(seed)
        n = 0.93 + 0.07 * rng.random((self.ny, self.nx)).astype(np.float32)
        rgb = np.clip(self.col * n[..., None] * 255, 0, 255).astype(np.uint8)
        alb = np.dstack([rgb, np.clip(self.end / DEPTH * 255, 0, 255).astype(np.uint8)])
        lum = self.col @ np.array([0.3, 0.59, 0.11], np.float32)
        e = self.end / DEPTH * 3.0 + lum * 0.04                           # the cladding's own grain rides on the relief's slope
        dx = (np.roll(e, -1, 1) - np.roll(e, 1, 1)) * 0.5; dy = (np.roll(e, -1, 0) - np.roll(e, 1, 0)) * 0.5
        nrm = np.stack([-dx * 3, dy * 3, np.ones_like(e)], -1); nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
        nor = np.dstack([((nrm * 0.5 + 0.5) * 255).astype(np.uint8), np.clip(self.start / DEPTH * 255, 0, 255).astype(np.uint8)])
        return png(alb), png(nor)


def png(arr):
    b = io.BytesIO(); Image.fromarray(arr, "RGBA").save(b, "PNG", optimize=True, compress_level=9); return b.getvalue()


def cladding(d, W, H, rng, top_storey=False):
    """The wall's colour over the strip: the source texture, desaturated, painted, with stains and a little per-strip drift."""
    src = np.asarray(Image.open(d["clad"]).convert("RGB"), np.float32) / 255.0
    per = int(d["clad_m"] * PPM)
    src = np.asarray(Image.fromarray((src * 255).astype(np.uint8)).resize((per, per), Image.LANCZOS), np.float32) / 255.0
    nx, ny = int(round(W * PPM)), int(round(H * PPM))
    ox, oy = rng.integers(per), rng.integers(per)
    ix = (np.arange(nx) + ox) % per; iy = (np.arange(ny) + oy) % per
    t = src[np.ix_(iy, ix)]
    lum = t @ np.array([0.3, 0.59, 0.11], np.float32)
    lum = lum / max(lum.mean(), 1e-3)
    paint = d["paint2"] if (top_storey and d["two_tone"]) else d["paint"]
    c = (0.55 * lum[..., None] + 0.45) * paint * rng.uniform(0.95, 1.04)
    return np.clip(c, 0, 1).astype(np.float32)


# ---------------------------------------------------------------- one bay
def bay(R, d, rng, bx, kind, apartment_rng):
    """Draw one 3 m bay at bx: kind 'win' or 'balc'. apartment_rng decides what this flat's owner did."""
    r = apartment_rng
    T = T_WALL
    opens = BALCONY_OPEN if kind == "balc" else WINDOWS[d["window"]]
    frame = d["frame"] if r.random() > 0.12 else np.array(FRAMES[r.integers(len(FRAMES))], np.float32)   # a replaced window
    enclosed = kind == "balc" and r.random() < d["enclose_p"]
    for (x0, x1, y0, y1, mull, tr) in opens:
        x0 += bx; x1 += bx
        if d["surround"] == "band":
            R.box(x0 - 0.14, x1 + 0.14, y0 - 0.14, y1 + 0.14, 0, T + 0.025, d["surround_col"])
        elif d["surround"] == "box":
            R.box(x0 - 0.1, x1 + 0.1, y0 - 0.02, y1 + 0.1, 0, T + 0.12, d["surround_col"] * 0.97)
            R.shade(x0 - 0.1, x1 + 0.1, y1 + 0.06, y1 + 0.1, 1.12)
        R.cut(x0, x1, y0, y1)
        fw = 0.06
        def member(a0, a1, b0, b1):
            R.box(a0, a1, b0, b1, 0.03, 0.11, frame); R.shade(a0, a1, b1 - 0.012, b1, 1.15)
        member(x0, x1, y0, y0 + fw); member(x0, x1, y1 - fw, y1); member(x0, x0 + fw, y0, y1); member(x1 - fw, x1, y0, y1)
        for k in range(1, mull + 1):
            xm = x0 + (x1 - x0) * k / (mull + 1); member(xm - 0.03, xm + 0.03, y0, y1)
        if tr is not None:
            ym = y0 + (y1 - y0) * tr; member(x0, x1, ym - 0.03, ym + 0.03)
        if y0 > 0.5 and kind == "win":                                                       # sill
            R.box(x0 - 0.07, x1 + 0.07, y0 - 0.06, y0, 0, T + 0.06, d["sill_col"]); R.shade(x0 - 0.07, x1 + 0.07, y0 - 0.012, y0, 1.15)
        if kind == "win" and d["window"] == "tall":                                           # a juliet rail across the french window
            R.box(x0 - 0.05, x1 + 0.05, 0.95, 1.0, T + 0.04, T + 0.09, (0.15, 0.15, 0.17))
            for xx in np.arange(x0 + 0.06, x1, 0.11): R.box(xx, xx + 0.018, y0 + 0.1, 0.95, T + 0.05, T + 0.07, (0.15, 0.15, 0.17))
            R.box(x0, x1, y0 + 0.06, y0 + 0.12, T + 0.04, T + 0.09, (0.15, 0.15, 0.17))
        if d["shutters"] and kind == "win" and x1 - x0 < 1.6:
            sw = (x1 - x0) / 2; scol = d["shutter_col"] * r.uniform(0.85, 1.05)
            state = r.random()
            for side in (0, 1):
                closed = state < 0.15 or (state < 0.3 and side == 0)
                if closed: sx0, z0_ = (x0 + side * sw), 0.12
                else: sx0, z0_ = (x0 - sw - 0.02 if side == 0 else x1 + 0.02), T
                R.box(sx0, sx0 + sw, y0, y1, z0_, z0_ + 0.04, scol)
                for yy in np.arange(y0 + 0.05, y1, 0.07): R.shade(sx0 + 0.04, sx0 + sw - 0.04, yy, yy + 0.025, 0.72)
        if kind == "win" and y0 > 0.5 and r.random() < d["flower_p"]:                        # flower box with flowers
            R.box(x0, x1, y0 - 0.06, y0 + 0.14, T + 0.06, T + 0.26, np.array((0.45, 0.28, 0.18)) * r.uniform(0.7, 1.2))
            for _ in range(int((x1 - x0) * 9)):
                fc = np.array([(0.2, 0.4, 0.15), (0.75, 0.15, 0.2), (0.9, 0.8, 0.3), (0.85, 0.45, 0.65), (0.25, 0.45, 0.18)][r.integers(5)])
                R.disc(r.uniform(x0, x1), y0 + 0.14 + r.uniform(0.02, 0.18), r.uniform(0.04, 0.08), T + 0.08, T + 0.26, fc)
        if kind == "win" and r.random() < d["awning_p"] and y1 > 2.0:                         # a striped awning
            c1 = np.array(ACCENTS[r.integers(len(ACCENTS))], np.float32); c2 = np.array((0.93, 0.92, 0.88), np.float32)
            R.sloped(x0 - 0.1, x1 + 0.1, y1 + 0.3, y1 - 0.35, T + 0.02, T + 0.75, 0.025, c1, c2 if r.random() < 0.7 else None)
    if kind == "win" and r.random() < d["ac_p"]:                                              # an air conditioner on brackets
        x0, x1, y0, y1 = opens[-1][:4]
        ax = bx + (x1 + 0.12 if x1 + 0.95 < 3.0 else x0 - 0.9); ay = max(0.2, y0 - 0.7)
        R.box(ax, ax + 0.8, ay, ay + 0.55, T + 0.05, T + 0.33, (0.88, 0.88, 0.85))
        R.disc(ax + 0.5, ay + 0.27, 0.19, T + 0.33, T + 0.335, (0.35, 0.35, 0.35))
        for k in range(5): R.shade(ax + 0.06, ax + 0.25, ay + 0.1 + k * 0.08, ay + 0.12 + k * 0.08, 0.6)
        R.box(ax + 0.08, ax + 0.12, ay - 0.08, ay + 0.02, T, T + 0.33, (0.3, 0.3, 0.3)); R.box(ax + 0.68, ax + 0.72, ay - 0.08, ay + 0.02, T, T + 0.33, (0.3, 0.3, 0.3))
    if kind == "balc":
        P = 1.12                                                                              # projection
        slab_c = d["sill_col"] * 0.95
        R.box(bx + 0.05, bx + 2.95, 0.0, 0.16, 0, P, slab_c); R.shade(bx + 0.05, bx + 2.95, 0.14, 0.16, 1.1)
        acc = (np.array(ACCENTS[r.integers(len(ACCENTS))], np.float32) if d["multi"] else d["accent"])
        if enclosed:                                                                          # an owner glazed it in: panel below, glass above
            low = np.array(ACCENTS[r.integers(len(ACCENTS))], np.float32) * 0.8 if r.random() < 0.5 else acc
            R.box(bx + 0.05, bx + 2.95, 0.16, 1.0, P - 0.1, P, low)
            gl = np.zeros((1, 1, 3), np.float32)
            R.box(bx + 0.05, bx + 2.95, 1.0, 2.75, P - 0.08, P - 0.03, (0.30, 0.38, 0.46))
            ys = R.ys; m = R.mask(bx + 0.05, bx + 2.95, 1.0, 2.75)
            grad = np.clip((ys - 1.0) / 1.75, 0, 1)[:, None] * np.ones((1, R.nx))
            R.col[m] = (np.array((0.25, 0.33, 0.42)) + np.array((0.35, 0.38, 0.40)) * grad[..., None])[m] * r.uniform(0.85, 1.1)
            fcol = np.array(FRAMES[r.integers(len(FRAMES))], np.float32)
            for xx in np.linspace(bx + 0.05, bx + 2.91, r.integers(3, 6)): R.box(xx, xx + 0.05, 1.0, 2.75, P - 0.1, P - 0.0, fcol)
            R.box(bx + 0.05, bx + 2.95, 1.0, 1.05, P - 0.1, P, fcol); R.box(bx + 0.05, bx + 2.95, 2.7, 2.75, P - 0.1, P, fcol)
            R.box(bx + 0.0, bx + 3.0, 2.75, 2.88, 0, P + 0.03, (0.55, 0.56, 0.58))           # its little roof
            for sx in (bx + 0.05, bx + 2.9): R.box(sx, sx + 0.05, 0.16, 2.75, 0, P, fcol)     # side frames
        else:
            if d["rail"] == "solid":
                R.box(bx + 0.05, bx + 2.95, 0.16, 1.05, P - 0.08, P, acc); R.shade(bx + 0.05, bx + 2.95, 1.0, 1.05, 1.15)
                for sx in (bx + 0.05, bx + 2.88): R.box(sx, sx + 0.07, 0.16, 1.05, 0, P, acc * 0.9)
            else:
                rc = np.array((0.16, 0.16, 0.18), np.float32) if r.random() < 0.8 else acc
                R.box(bx + 0.05, bx + 2.95, 1.0, 1.05, P - 0.06, P, rc)
                step = 0.11 if d["rail"] == "bars" else 0.05
                for xx in np.arange(bx + 0.08, bx + 2.92, step): R.box(xx, xx + (0.018 if d["rail"] == "bars" else 0.01), 0.16, 1.0, P - 0.05, P - 0.02, rc)
                if d["rail"] == "mesh":
                    for yy in np.arange(0.2, 1.0, 0.05): R.box(bx + 0.08, bx + 2.92, yy, yy + 0.01, P - 0.05, P - 0.02, rc)
                for sx in (bx + 0.05, bx + 2.9): R.box(sx, sx + 0.05, 0.16, 1.05, 0, P, rc)
            for _ in range(r.integers(0, 4)):                                                 # what stands on it
                k = r.integers(4); cx = bx + r.uniform(0.4, 2.6)
                if k == 0:                                                                    # a plant, leaves over the rail
                    R.box(cx - 0.12, cx + 0.12, 0.16, 0.45, 0.4, 0.7, (0.55, 0.3, 0.2))
                    for _j in range(6): R.disc(cx + r.uniform(-0.25, 0.25), r.uniform(0.6, 1.3), r.uniform(0.1, 0.2), 0.35, 0.8, np.array((0.2, 0.38, 0.15)) * r.uniform(0.7, 1.3))
                elif k == 1:                                                                  # washing on a line
                    R.box(bx + 0.2, bx + 2.8, 1.75, 1.77, 0.6, 0.62, (0.8, 0.8, 0.8))
                    for xx in np.arange(bx + 0.3, bx + 2.7, r.uniform(0.25, 0.45)):
                        R.box(xx, xx + r.uniform(0.15, 0.35), r.uniform(1.15, 1.45), 1.77, 0.58, 0.64, FRAMES[0] if r.random() < 0.3 else ACCENTS[r.integers(len(ACCENTS))])
                elif k == 2:                                                                  # a chair
                    c = np.array((0.3, 0.25, 0.2)) * r.uniform(0.5, 1.5)
                    R.box(cx - 0.22, cx + 0.22, 0.6, 0.64, 0.35, 0.8, c); R.box(cx - 0.22, cx + 0.22, 0.6, 1.05, 0.33, 0.37, c)
                else:                                                                         # a satellite dish on the rail
                    if r.random() < d["dish_p"] * 5: R.disc(cx, 1.35, 0.3, P - 0.25, P - 0.1, (0.9, 0.9, 0.88))
    if r.random() < d["dish_p"] and kind == "win":
        R.disc(bx + r.uniform(0.4, 2.6), r.uniform(0.3, 0.7), 0.28, T, T + 0.18, (0.9, 0.9, 0.88))


def strip(d, nb, pattern_index, rng, seed, top=False, bottom=False):
    """One storey, nb bays wide. pattern_index: index of the first bay in the face (balcony stacking follows the face)."""
    W, H = nb * BAY, STOREY
    R = Relief(W, H, None)
    clad = cladding(d, W, H, rng, top_storey=top)
    R.box(0, W, 0, H, 0, T_WALL, clad)
    # rain: streaks and a darker band under the floor line
    streak = np.asarray(Image.fromarray((rng.random((R.ny // 16 + 1, R.nx)) * 255).astype(np.uint8)).resize((R.nx, R.ny), Image.BILINEAR), np.float32) / 255
    R.col *= (1 - 0.08 * np.clip(streak * 2 - 1.2, 0, 1))[..., None]
    if d["panels"]:
        for xx in np.arange(0, W + 0.01, 1.5 if d["window"] == "double" else 3.0):
            R.box(xx - 0.012, xx + 0.012, 0, H, 0, T_WALL - 0.015, clad * 0.7)
        R.box(0, W, H - 0.012, H, 0, T_WALL - 0.015, clad[:1] * 0.7 if False else d["paint"] * 0.55)
        R.box(0, W, 0, 0.012, 0, T_WALL - 0.015, d["paint"] * 0.55)
    if d["band"]:
        R.box(0, W, 0.0, 0.16, 0, T_WALL + 0.035, d["surround_col"] * 0.95 if d["surround"] != "none" else d["paint"] * 0.9)
        R.shade(0, W, 0.14, 0.16, 1.15)
    for b in range(nb):
        i = pattern_index + b
        pat = d["balcony"]
        balc = (pat == "all") or (pat == "alt" and i % 2 == 1) or (pat == "mid" and i % 3 == 1) or (pat == "ends" and d["_ends"](i))
        bay(R, d, rng, b * BAY, "balc" if balc else "win", np.random.default_rng(seed * 31 + b * 7 + 1))
    return R.encode(seed)


# ---------------------------------------------------------------- volumes in the mesh
def spom_volume(mesh, mat, origin, N, W, H):
    if mat not in mesh.prims: mesh.prims[mat] = []
    N = np.array(N, np.float32); up = np.array([0, 1, 0], np.float32); T = np.cross(up, N).astype(np.float32)
    O = np.array(origin, np.float32)
    c = {(i, j, k): O + T * i * W + up * j * H + N * k * DEPTH for i in (0, 1) for j in (0, 1) for k in (0, 1)}
    centre = O + T * W / 2 + up * H / 2 + N * DEPTH / 2
    for f in [((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)), ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)),
              ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)), ((0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)),
              ((1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1))]:
        pts = [c[k] for k in f]
        n = np.cross(pts[1] - pts[0], pts[3] - pts[0])
        if np.dot(n, np.mean(pts, 0) - centre) < 0: f = (f[0], f[3], f[2], f[1]); pts = [c[k] for k in f]
        mesh.raw_quad(mat, pts, N, [(k[0] * W, k[1] * H) for k in f], [(W, H, k[2] * DEPTH) for k in f])


def dress_tower(mesh, d, rng, x0, x1, z0, z1, y0, floors, tex, seed, variants=3):
    """SPOM strips over all four faces of a tower box, storey by storey. Adds the materials it uses to tex."""
    faces = [((x0, z1), (1, 0, 0), (0, 0, 1), x1 - x0),        # front: origin, T, N, width
             ((x1, z0), (-1, 0, 0), (0, 0, -1), x1 - x0),
             ((x1, z1), (0, 0, -1), (1, 0, 0), z1 - z0),
             ((x0, z0), (0, 0, 1), (-1, 0, 0), z1 - z0)]
    face_bays = {int(round(w / BAY)) for (_, _, _, w) in faces}
    nbays = max(face_bays)
    d["_ends"] = lambda i, n=nbays: i == 0 or i == n - 1 or i == 1
    made = {}
    def mat_for(nb, first, top, v):
        key = f"spom_s{nb}_{first % 6}_{'t' if top else 'n'}_{v}"
        if key not in made:
            made[key] = True
            tex[key] = strip(d, nb, first, rng, seed + len(made) * 101, top=top)
        return key
    top_floors = 2 if d["two_tone"] else 0
    for (o, T, N, w) in faces:
        nb_face = int(round(w / BAY)); b = 0
        runs = []
        while b < nb_face:
            n = min(3, nb_face - b); runs.append((b, n)); b += n
        for f in range(floors):
            top = f >= floors - top_floors
            for (b0, n) in runs:
                v = int(rng.integers(variants if n == 3 else max(1, variants // 2)))
                mat = mat_for(n, b0, top, v)
                org = np.array([o[0], y0 + f * STOREY, o[1]], np.float32) + np.array(T, np.float32) * b0 * BAY
                spom_volume(mesh, mat, org, N, n * BAY, STOREY)
    return len(made)
