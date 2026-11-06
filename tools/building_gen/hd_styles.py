"""Facade families for high-density housing, drawn as silhouette-POM reliefs (SpomPS in src/shaders/mesh.hlsl).

A tower face is a plain box carrying a glass field (facade_hd.glass_field: a room behind every window); in front of it stand SPOM
volumes, each a run of up to four bays and one or more storeys, whose two-layer relief is the real wall: openings, frames, sills,
balconies, fins, pilasters, fire escapes. A volume extends up to DEPTH (1.2 m) out of the face.

Five families, one per level, each with its own design draw so no two buildings match:
  1 slab       pastel or concrete housing slabs: punched windows, balconies, owners' glazed loggias, AC units, awnings, shutters
  2 brick      NYC walk-up / London sash / warehouse conversion: sash windows, lintels, fire escapes, iron balconies, cornices
  3 glass      curtain wall: mullions, spandrels, fins and sunshades in a building's own metal and glass tint
  4 deco       stone towers: deep reveals, pilasters, ornamented spandrel panels, friezes, arched top storeys
  5 modern     floor-to-ceiling glass behind slab balconies: flush, staggered, checkered or waving; glass/rod/solid/timber rails;
               privacy screens and, on green buildings, planters spilling over
and the podium every building stands on: a glazed lobby with doors, a canopy and the building's name, shopfronts, a garage door,
service walls, and the pool deck's balustrade posts along the top.
"""
import io, os, math
import numpy as np
from PIL import Image, ImageDraw, ImageFont

DEPTH = 1.2
PPM = 60
MAXY = 1024           # tall reliefs (many storeys) are seen from afar: their height in texels is capped
BAY, STOREY = 3.0, 3.0
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC = {"md": os.path.join(ROOT, "assets", "buildings", "medium_apartments", "src_tex"),
       "rh": os.path.join(ROOT, "assets", "buildings", "row_housing", "src_tex"),
       "ld": os.path.join(ROOT, "assets", "buildings", "low_density", "src_tex")}
def src(k, f): return os.path.join(SRC[k], f)
CONCRETE = [src("md", "MD_L1_wall.jpg"), src("md", "MD_L5_wall.jpg"), src("md", "MD_L4_wall.jpg"), src("ld", "L5_wall.jpg")]
BRICK = src("rh", "RH_L3_wall.jpg")

PAINTS = [(0.93, 0.88, 0.76), (0.92, 0.72, 0.62), (0.74, 0.86, 0.78), (0.95, 0.88, 0.62), (0.78, 0.78, 0.76), (0.80, 0.52, 0.40),
          (0.72, 0.82, 0.90), (0.95, 0.94, 0.91), (0.85, 0.68, 0.42), (0.82, 0.76, 0.86), (0.66, 0.72, 0.62), (0.70, 0.69, 0.66)]
ACCENTS = [(0.80, 0.25, 0.20), (0.20, 0.42, 0.65), (0.95, 0.75, 0.20), (0.30, 0.55, 0.35), (0.90, 0.50, 0.20), (0.55, 0.30, 0.55),
           (0.15, 0.17, 0.20), (0.92, 0.92, 0.90), (0.40, 0.65, 0.75), (0.70, 0.20, 0.35)]
FRAMES = [(0.93, 0.93, 0.91), (0.93, 0.93, 0.91), (0.22, 0.23, 0.25), (0.40, 0.28, 0.18), (0.24, 0.36, 0.28), (0.60, 0.62, 0.64)]
METALS = {"silver": (0.72, 0.74, 0.76), "bronze": (0.42, 0.32, 0.22), "black": (0.12, 0.12, 0.13), "white": (0.92, 0.92, 0.9),
          "champagne": (0.78, 0.70, 0.56), "graphite": (0.30, 0.31, 0.33)}
GLASS_TINTS = {"blue": (40, 70, 100), "green": (50, 90, 80), "bronze": (90, 70, 50), "grey": (60, 66, 72), "silver": (110, 120, 130)}
SHOPS = ["CAFE", "DELI", "PHARMACY", "FLOWERS", "BAKERY", "GYM", "BOOKS", "WINE", "NOODLES", "DENTAL", "OPTICIAN", "LAUNDRY", "SUSHI", "BIKES"]
NAMES_A = ["THE", "THE", "", ""]
NAMES_B = ["AURELIA", "MERIDIAN", "HALCYON", "BELVEDERE", "ALDER", "CARLTON", "SOLANA", "VERANDA", "HARBOR", "LINDEN", "ONYX", "ASTOR",
           "MARLOWE", "SAVOY", "CYPRESS", "ELARA", "KENSINGTON", "PARKSIDE", "THE MERCER", "NOVA", "ARDEN", "BEACON", "WILLOW", "IVY"]


def C(c): return np.array(c, np.float32)


# ---------------------------------------------------------------- the relief
class Relief:
    """Two layers per texel ([start, end] metres out of the wall) and a colour, over W x H metres. Drawing calls are relative to an
    origin set with at(), so a bay or storey routine draws in its own coordinates."""
    def __init__(self, W, H, ppm=PPM):
        self.W, self.H = W, H
        self.nx, self.ny = max(8, int(round(W * ppm))), max(8, min(MAXY, int(round(H * ppm))))
        self.start = np.zeros((self.ny, self.nx), np.float32); self.end = np.zeros((self.ny, self.nx), np.float32)
        self.col = np.zeros((self.ny, self.nx, 3), np.float32)
        self.xs0 = (np.arange(self.nx) + 0.5) / self.nx * W; self.ys0 = H - (np.arange(self.ny) + 0.5) / self.ny * H
        self.at(0, 0)

    def at(self, ox, oy):
        self.ox, self.oy = ox, oy; self.xs = self.xs0 - ox; self.ys = self.ys0 - oy; return self

    def mask(self, x0, x1, y0, y1):
        """The texels with x0 <= x < x1 and y0 <= y < y1, as a window of slices (xs rises, ys falls down the rows)."""
        c0 = int(np.searchsorted(self.xs, x0, "left")); c1 = int(np.searchsorted(self.xs, x1, "left"))
        nys = -self.ys
        r0 = int(np.searchsorted(nys, -y1, "right")); r1 = int(np.searchsorted(nys, -y0, "right"))
        return (slice(r0, max(r0, r1)), slice(c0, max(c0, c1)))

    behind = False   # True: only fill empty texels. A texel holds one solid span, so a pot drawn behind a rail would replace the rail

    def _put(self, m, z0, z1, col):
        c = np.asarray(col, np.float32)
        if self.behind:
            lm = self.end[m] <= self.start[m] + 0.004
            if z0 is not None: self.start[m][lm] = max(0.0, z0); self.end[m][lm] = min(DEPTH, z1)
            self.col[m][lm] = c[m][lm] if c.ndim == 3 else c
            return
        if z0 is not None: self.start[m] = max(0.0, z0); self.end[m] = min(DEPTH, z1)
        self.col[m] = c[m] if c.ndim == 3 else c

    def _put_local(self, w, lm, z0, z1, col):
        if self.behind: lm = lm & (self.end[w] <= self.start[w] + 0.004)
        if z0 is not None: self.start[w][lm] = max(0.0, z0); self.end[w][lm] = min(DEPTH, z1)
        self.col[w][lm] = np.asarray(col, np.float32)

    def box(self, x0, x1, y0, y1, z0, z1, col):
        m = self.mask(x0, x1, y0, y1); self._put(m, z0, z1, col); return m

    def cut(self, x0, x1, y0, y1):
        m = self.mask(x0, x1, y0, y1); self.start[m] = 0; self.end[m] = 0; return m

    def shade(self, x0, x1, y0, y1, k):
        m = self.mask(x0, x1, y0, y1); self.col[m] *= k

    def grad(self, x0, x1, y0, y1, z0, z1, c_bot, c_top):
        m = self.mask(x0, x1, y0, y1)
        t = np.clip((self.ys[m[0]] - y0) / max(y1 - y0, 1e-6), 0, 1)[:, None, None]
        sub = (C(c_bot) * (1 - t) + C(c_top) * t) * np.ones((1, m[1].stop - m[1].start, 1), np.float32)
        if z0 is not None: self.start[m] = max(0.0, z0); self.end[m] = min(DEPTH, z1)
        self.col[m] = sub

    def _local(self, x0, x1, y0, y1):
        w = self.mask(x0, x1, y0, y1)
        return w, self.xs[w[1]][None, :], self.ys[w[0]][:, None]

    def disc(self, cx, cy, r, z0, z1, col, ry=None):
        ry = r if ry is None else ry
        w, X, Y = self._local(cx - r, cx + r, cy - ry, cy + ry)
        lm = ((X - cx) / r) ** 2 + ((Y - cy) / ry) ** 2 < 1
        self._put_local(w, lm, z0, z1, col)

    def dots(self, x0, x1, y0, y1, pitch, rad, z0, z1, col):
        w, X, Y = self._local(x0, x1, y0, y1)
        lm = (((X - x0) % pitch - pitch / 2) ** 2 + ((Y - y0) % pitch - pitch / 2) ** 2) < rad * rad
        self._put_local(w, lm, z0, z1, col)

    def ring(self, cx, cy, r0, r1, ymin, z0, z1, col):
        w, X, Y = self._local(cx - r1, cx + r1, max(ymin, cy - r1), cy + r1)
        d = np.sqrt((X - cx) ** 2 + (Y - cy) ** 2)
        self._put_local(w, (d >= r0) & (d < r1) & (Y >= ymin), z0, z1, col)

    def cut_disc(self, cx, cy, r, ymin):
        w, X, Y = self._local(cx - r, cx + r, max(ymin, cy - r), cy + r)
        lm = (((X - cx) ** 2 + (Y - cy) ** 2) < r * r) & (Y >= ymin)
        self.start[w][lm] = 0; self.end[w][lm] = 0

    def sloped(self, x0, x1, y_top, y_bot, z_top, z_bot, thick, col, stripes=None):
        w = self.mask(x0, x1, y_bot, y_top)
        xs = self.xs[w[1]]
        for r in range(w[0].start, w[0].stop):
            y = self.ys[r]
            t = (y_top - y) / max(y_top - y_bot, 1e-6); z = z_top + (z_bot - z_top) * t
            self.start[r, w[1]] = max(0.0, z - thick); self.end[r, w[1]] = min(DEPTH, z + thick)
            if stripes is None: self.col[r, w[1]] = col
            else:
                band = ((xs - x0) / 0.25).astype(int) % 2
                self.col[r, w[1]] = np.where(band[:, None] == 0, C(col), C(stripes))

    def text(self, s, x0, y0, x1, y1, z0, z1, col, bold=True):
        """Letters fitted into the box x0..x1, y0..y1 (centred)."""
        if not s: return
        try: font = ImageFont.truetype("arialbd.ttf" if bold else "arial.ttf", 96)
        except OSError: font = ImageFont.load_default()
        bb = ImageDraw.Draw(Image.new("L", (8, 8))).textbbox((0, 0), s, font=font)
        tw, th = bb[2] - bb[0] + 8, bb[3] - bb[1] + 8
        im = Image.new("L", (tw, th), 0); ImageDraw.Draw(im).text((4 - bb[0], 4 - bb[1]), s, fill=255, font=font)
        sc = min((x1 - x0) / tw, (y1 - y0) / th)
        w_m, h_m = tw * sc, th * sc; ox = x0 + (x1 - x0 - w_m) / 2; oy = y0 + (y1 - y0 - h_m) / 2
        w, X, Y = self._local(ox, ox + w_m, oy, oy + h_m)
        if X.size == 0 or Y.size == 0: return
        u = np.clip(((X - ox) / w_m * (tw - 1)).astype(int), 0, tw - 1); v = np.clip(((1 - (Y - oy) / h_m) * (th - 1)).astype(int), 0, th - 1)
        lm = np.asarray(im, np.uint8)[v, u] > 110
        self._put_local(w, lm, z0, z1, col)

    def encode(self, seed):
        rgb = np.clip(self.col * 255, 0, 255).astype(np.uint8)          # no added noise: cladding carries its own grain, and noise defeats PNG
        alb = np.dstack([rgb, np.clip(self.end / DEPTH * 255, 0, 255).astype(np.uint8)])
        lum = self.col @ np.array([0.3, 0.59, 0.11], np.float32)
        e = self.end / DEPTH * 3.0 + lum * 0.05
        dx = (np.roll(e, -1, 1) - np.roll(e, 1, 1)) * 0.5; dy = (np.roll(e, -1, 0) - np.roll(e, 1, 0)) * 0.5
        nrm = np.stack([-dx * 3, dy * 3, np.ones_like(e)], -1); nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
        nor = np.dstack([((nrm * 0.5 + 0.5) * 255).astype(np.uint8), np.clip(self.start / DEPTH * 255, 0, 255).astype(np.uint8)])
        return png(alb), png(nor)


def png(arr):
    b = io.BytesIO(); Image.fromarray(arr, "RGBA").save(b, "PNG", optimize=False, compress_level=7); return b.getvalue()


_CLAD_CACHE = {}
def cladding(path, period_m, paint, W, H, rng, mode="paint", ppm=PPM):
    """A wall colour over W x H: the source picture repeated every period_m, painted (luminance x paint) or tinted (colour x paint)."""
    key = (path, period_m, ppm)
    if key not in _CLAD_CACHE:
        per = max(16, int(period_m * ppm))
        _CLAD_CACHE[key] = np.asarray(Image.open(path).convert("RGB").resize((per, per), Image.LANCZOS), np.float32) / 255.0
    t0 = _CLAD_CACHE[key]; per = t0.shape[0]
    nx, ny = max(8, int(round(W * ppm))), max(8, min(MAXY, int(round(H * ppm))))
    ix = (np.arange(nx) + rng.integers(per)) % per; iy = ((np.arange(ny) * (H * ppm / ny)).astype(int) + rng.integers(per)) % per
    t = t0[np.ix_(iy, ix)]
    if mode == "paint":
        lum = t @ np.array([0.3, 0.59, 0.11], np.float32); lum = lum / max(lum.mean(), 1e-3)
        c = (0.5 * lum[..., None] + 0.5) * C(paint)
    else:
        lum = (t @ np.array([0.3, 0.59, 0.11], np.float32))[..., None]
        c = (0.6 * t / max(t.mean(), 1e-3) + 0.4 * lum / max(lum.mean(), 1e-3)) * 0.5 * C(paint)
    return np.clip(c * rng.uniform(0.96, 1.04), 0, 1).astype(np.float32)


# ---------------------------------------------------------------- designs
def design(level, rng):
    d = dict(level=level, glass=GLASS_TINTS["grey"], field_kind="street", head=2.3, F=0.3, lit=0.08, kinds=None)
    pick = lambda xs: xs[rng.integers(len(xs))]
    if level == 1:
        d.update(T=0.30, wall=True, clad=pick(CONCRETE), clad_m=rng.uniform(2.0, 3.2), mode="paint", paint=C(pick(PAINTS)),
                 paint2=C(pick(PAINTS)), two_tone=rng.random() < 0.35, panels=rng.random() < 0.45,
                 window=rng.choice(["single", "wide", "tall", "double", "square"]), frame=C(pick(FRAMES)),
                 surround=rng.choice(["none", "band", "box"], p=[0.45, 0.3, 0.25]), sill=C((0.8, 0.79, 0.76)) * rng.uniform(0.85, 1.05),
                 balcony=rng.choice(["none", "alt", "mid", "all", "ends"], p=[0.12, 0.3, 0.25, 0.18, 0.15]),
                 rail=rng.choice(["bars", "solid", "mesh"]), multi=rng.random() < 0.3, accent=C(pick(ACCENTS)),
                 shutters=rng.random() < 0.22, shutter=C(pick([(0.3, 0.45, 0.32), (0.25, 0.35, 0.55), (0.55, 0.25, 0.2), (0.85, 0.82, 0.74)])),
                 band=rng.random() < 0.5, awning_p=0.04 + 0.12 * rng.random() ** 2, ac_p=0.05 + 0.3 * rng.random(),
                 flower_p=0.04 + 0.2 * rng.random(), enclose_p=0.0 if rng.random() < 0.4 else rng.uniform(0.1, 0.45), dish_p=0.08 * rng.random())
        d["surround_col"] = C(pick(ACCENTS)) if rng.random() < 0.4 else d["paint"] * 1.08
        d["field_lit"] = 0.08
    elif level == 2:
        flavour = rng.choice(["nyc", "london", "warehouse"])
        bricks = {"red": (1.0, 0.95, 0.92), "brown": (0.72, 0.6, 0.55), "buff": (1.25, 1.12, 0.85), "dark": (0.55, 0.5, 0.5),
                  "orange": (1.1, 0.9, 0.75)}
        bname = pick(list(bricks)); painted = rng.random() < 0.15
        d.update(T=0.32, wall=True, flavour=flavour, clad=BRICK, clad_m=rng.uniform(2.6, 3.4), mode="paint" if painted else "tint",
                 paint=C((0.93, 0.92, 0.88)) if painted else C(bricks[bname]), brick=bname if not painted else "painted",
                 frame=C(pick([(0.94, 0.93, 0.9), (0.94, 0.93, 0.9), (0.1, 0.1, 0.11), (0.2, 0.3, 0.22), (0.5, 0.15, 0.12), (0.25, 0.27, 0.32)])),
                 stone=C(pick([(0.85, 0.82, 0.74), (0.78, 0.74, 0.66), (0.92, 0.9, 0.86), (0.6, 0.58, 0.55)])),
                 lintel=rng.choice(["flat", "key", "arch", "soldier"] if flavour != "warehouse" else ["arch", "soldier"]),
                 rhythm=rng.choice(["single", "pair"]) if flavour != "warehouse" else "wide",
                 escape_cols={1} if flavour == "nyc" and rng.random() < 0.8 else set(),
                 window_ac_p=rng.uniform(0.15, 0.45) if flavour == "nyc" else 0.03, juliet=flavour == "london" and rng.random() < 0.7,
                 cornice=rng.random() < 0.8, band=rng.random() < 0.6, flower_p=rng.uniform(0.0, 0.2), muntins=rng.choice([1, 2, 3]))
        d["field_lit"] = 0.1
    elif level == 3:
        tint = pick(list(GLASS_TINTS)); metal = pick(list(METALS))
        d.update(T=0.05, wall=False, tint=tint, glass=GLASS_TINTS[tint], metal=C(METALS[metal]), metal_name=metal, field_kind="tower",
                 F=0.45, module=float(rng.choice([1.0, 1.5, 1.5, 3.0])), fin=float(rng.choice([0.0, 0.25, 0.45, 0.7])),
                 fin_every=int(rng.choice([1, 2])), sunshade=rng.random() < 0.35,
                 spandrel=rng.choice(["glass", "metal", "frit", "stone"]), vision=float(rng.uniform(2.15, 2.5)),
                 spandrel_col=C(pick([(0.2, 0.24, 0.28), (0.85, 0.85, 0.82), (0.55, 0.45, 0.35), (0.3, 0.32, 0.3)])),
                 kinds=dict(none=0.25, roller=0.4, venetian=0.2, curtains=0.1, net=0.05), head=2.4)
    elif level == 4:
        stones = {"limestone": (0.88, 0.83, 0.72), "buff": (0.82, 0.74, 0.6), "grey granite": (0.62, 0.62, 0.62), "red granite": (0.62, 0.42, 0.36),
                  "white brick": (0.92, 0.91, 0.88), "terracotta": (0.75, 0.48, 0.36), "black granite": (0.28, 0.28, 0.3)}
        sname = pick(list(stones))
        d.update(T=0.42, wall=True, clad=pick(CONCRETE), clad_m=rng.uniform(1.8, 3.0), mode="paint", paint=C(stones[sname]), stone=sname,
                 orn=C(pick([(0.45, 0.33, 0.2), (0.3, 0.5, 0.42), (0.75, 0.6, 0.3), (0.15, 0.15, 0.16), (0.6, 0.3, 0.22)])),
                 frame=C(pick([(0.3, 0.24, 0.17), (0.12, 0.12, 0.13), (0.35, 0.45, 0.4)])), rhythm=rng.choice(["single", "pair", "triple"]),
                 pilaster=float(rng.uniform(0.3, 0.55)), top_arch=rng.random() < 0.5, chevron=rng.random() < 0.6, head=2.45)
    else:
        d.update(T=0.06, wall=False, slab=C(pick([(0.93, 0.93, 0.91), (0.93, 0.93, 0.91), (0.25, 0.26, 0.28), (0.82, 0.76, 0.66), (0.6, 0.6, 0.6)])),
                 frame=C(pick([(0.15, 0.15, 0.16), (0.85, 0.85, 0.83), (0.4, 0.4, 0.42)])),
                 pattern=rng.choice(["flush", "jenga", "checker", "wave", "alt"]),
                 rail=rng.choice(["glass", "glass", "rods", "solid", "timber"]), screens=rng.random() < 0.5,
                 green=rng.random() < 0.3, field_kind="tower", F=0.38, head=2.75, glass=GLASS_TINTS[pick(["blue", "grey", "green"])],
                 kinds=dict(none=0.35, curtains=0.3, net=0.2, roller=0.15))
    if level == 2:
        f = d["flavour"]
        d.update(replaced_p=rng.uniform(0.0, 0.35), awning_p=float(rng.choice([0.0, 0.08, 0.25])) if f != "warehouse" else 0.0,
                 awn_col=C(pick([(0.15, 0.32, 0.2), (0.45, 0.1, 0.12), (0.12, 0.18, 0.35), (0.1, 0.1, 0.1), (0.7, 0.45, 0.15)])), awn_stripe=rng.random() < 0.4,
                 oriel_cols={int(rng.integers(4))} if f != "warehouse" and rng.random() < 0.45 else set(),
                 oriel_col=C(pick([(0.92, 0.92, 0.9), (0.3, 0.35, 0.3), (0.55, 0.45, 0.35), (0.6, 0.62, 0.65)])),
                 sleeve_p=rng.uniform(0.05, 0.3) if f == "nyc" else 0.0, flag_p=float(rng.choice([0.0, 0.02, 0.06])), dish_p=rng.uniform(0, 0.06),
                 poly=rng.random() < 0.3, poly_col=C(pick([(0.85, 0.78, 0.6), (0.3, 0.25, 0.25), (0.9, 0.88, 0.84)])), pipe=rng.random() < 0.6,
                 water_tower=f == "nyc" and rng.random() < 0.85, chimneys=f == "london" or rng.random() < 0.3)
        d["oriel_cols"] = d["oriel_cols"] - d["escape_cols"]
        d["window_ac_p"] = d["window_ac_p"] * rng.uniform(0.5, 1.5)
    if level == 3:
        d.update(corner_balc=rng.random() < 0.45, accent_p=float(rng.choice([0.0, 0.0, 0.08, 0.25])),
                 accents=[C(pick(ACCENTS)) for _ in range(3)], fin_col=C(pick(list(METALS.values()))) if rng.random() < 0.4 else d["metal"] * 0.95,
                 mech_every=int(rng.choice([10, 12, 15, 99])), garden_every=int(rng.choice([18, 24, 99, 99])),
                 crown=str(rng.choice(["screen", "screen", "pyramid", "flat"])))
    if level == 4:
        d.update(replaced_p=rng.uniform(0.0, 0.25), ac_p=rng.uniform(0.0, 0.3), flower_p=rng.uniform(0.0, 0.15), awning_p=float(rng.choice([0.0, 0.1, 0.3])),
                 awn_col=C(pick([(0.15, 0.32, 0.2), (0.45, 0.1, 0.12), (0.12, 0.18, 0.35), (0.1, 0.1, 0.1)])),
                 balc_cols={int(rng.integers(3))} if rng.random() < 0.6 else set(), frieze_every=int(rng.choice([6, 8, 10, 99])),
                 crown=str(rng.choice(["ziggurat", "lantern", "spire"])))
    if level == 5:
        d.update(garden_every=int(rng.choice([12, 16, 99, 99])))
    d.setdefault("mech_every", 99); d.setdefault("garden_every", 99); d.setdefault("frieze_every", 99)
    d["seed_offset"] = int(rng.integers(1 << 20))
    return d


def describe(d):
    L = d["level"]
    if L == 1: return f"slab: {d['window']} windows, {d['balcony']} balconies ({d['rail']}), surround {d['surround']}{', shutters' if d['shutters'] else ''}"
    if L == 2: return (f"brick {d['flavour']}: {d['brick']}, {d['rhythm']} windows, {d['lintel']} lintels{', fire escapes' if d['escape_cols'] else ''}"
                       f"{', bay windows' if d['oriel_cols'] else ''}{', awnings' if d['awning_p'] else ''}{', water tower' if d['water_tower'] else ''}")
    if L == 3: return f"glass: {d['tint']} glass, {d['metal_name']} {d['module']} m mullions, fins {d['fin']} m, spandrel {d['spandrel']}{', sunshades' if d['sunshade'] else ''}{', corner balconies' if d['corner_balc'] else ''}, {d['crown']} crown"
    if L == 4: return f"deco: {d['stone']}, {d['rhythm']} windows{', arched top' if d['top_arch'] else ''}, {d['crown']} crown{', balconies' if d['balc_cols'] else ''}{', awnings' if d['awning_p'] else ''}"
    return f"modern: {d['pattern']} balconies, {d['rail']} rails{', green' if d['green'] else ''}{', screens' if d['screens'] else ''}"


# ---------------------------------------------------------------- shared pieces
def frame_members(R, x0, x1, y0, y1, col, mull=0, transom=None, fw=0.06, z=(0.03, 0.11), grid=0):
    def mem(a0, a1, b0, b1): R.box(a0, a1, b0, b1, z[0], z[1], col); R.shade(a0, a1, b1 - 0.012, b1, 1.15)
    mem(x0, x1, y0, y0 + fw); mem(x0, x1, y1 - fw, y1); mem(x0, x0 + fw, y0, y1); mem(x1 - fw, x1, y0, y1)
    for k in range(1, mull + 1):
        xm = x0 + (x1 - x0) * k / (mull + 1); mem(xm - fw / 2, xm + fw / 2, y0, y1)
    if transom is not None:
        ym = y0 + (y1 - y0) * transom; mem(x0, x1, ym - fw / 2, ym + fw / 2)
    if grid:                                                                      # sash muntins
        for k in range(1, grid + 1):
            xm = x0 + (x1 - x0) * k / (grid + 1); R.box(xm - 0.012, xm + 0.012, y0, y1, z[0] + 0.02, z[1] - 0.02, col)
        for ys_ in ([y0 + (y1 - y0) * f for f in (0.25, 0.75)] if transom else []):
            R.box(x0, x1, ys_ - 0.012, ys_ + 0.012, z[0] + 0.02, z[1] - 0.02, col)


def plant(R, rng, cx, y0, z0, z1, h=(0.4, 1.2), n=6):
    was = R.behind; R.behind = True
    for _ in range(n):
        R.disc(cx + rng.uniform(-0.3, 0.3), y0 + rng.uniform(*h), rng.uniform(0.1, 0.22), z0, z1, C((0.18, 0.34, 0.14)) * rng.uniform(0.7, 1.3))
    R.behind = was


def balcony_things(R, rng, x0, x1, z_front, enclosed_ok=True):
    """Pots, washing, chairs, tables on a balcony: drawn behind the rail already there, so they show above it and between its bars."""
    was = R.behind; R.behind = True
    _balcony_things(R, rng, x0, x1, z_front)
    R.behind = was


def _balcony_things(R, rng, x0, x1, z_front):
    for _ in range(rng.integers(0, 4)):
        k = rng.integers(4); cx = rng.uniform(x0 + 0.4, x1 - 0.4)
        if k == 0:
            R.box(cx - 0.12, cx + 0.12, 0.16, 0.45, z_front * 0.4, z_front * 0.6, (0.55, 0.3, 0.2)); plant(R, rng, cx, 0.4, z_front * 0.3, z_front * 0.7, (0.2, 0.9))
        elif k == 1:
            R.box(x0 + 0.2, x1 - 0.2, 1.75, 1.77, z_front * 0.5, z_front * 0.52, (0.8, 0.8, 0.8))
            for xx in np.arange(x0 + 0.3, x1 - 0.3, rng.uniform(0.25, 0.45)):
                R.box(xx, xx + rng.uniform(0.15, 0.35), rng.uniform(1.15, 1.45), 1.77, z_front * 0.48, z_front * 0.55,
                      FRAMES[0] if rng.random() < 0.3 else ACCENTS[rng.integers(len(ACCENTS))])
        elif k == 2:
            c = C((0.3, 0.25, 0.2)) * rng.uniform(0.5, 1.5)
            R.box(cx - 0.22, cx + 0.22, 0.6, 0.64, z_front * 0.3, z_front * 0.7, c); R.box(cx - 0.22, cx + 0.22, 0.6, 1.05, z_front * 0.28, z_front * 0.32, c)
        else:
            R.box(cx - 0.35, cx + 0.35, 0.72, 0.76, z_front * 0.3, z_front * 0.75, (0.6, 0.6, 0.6)); R.box(cx - 0.03, cx + 0.03, 0.16, 0.72, z_front * 0.5, z_front * 0.55, (0.5, 0.5, 0.5))


# ---------------------------------------------------------------- L1 slab
L1_WIN = {"single": [(0.8, 2.2, 0.9, 2.25, 1, None)], "wide": [(0.45, 2.55, 0.9, 2.25, 2, None)], "tall": [(0.95, 2.05, 0.15, 2.3, 1, 0.78)],
          "double": [(0.35, 1.3, 0.9, 2.25, 0, 0.75), (1.7, 2.65, 0.9, 2.25, 0, 0.75)], "square": [(0.9, 2.1, 1.0, 2.2, 0, 0.7)]}
L1_BALC = [(0.3, 1.1, 0.18, 2.3, 0, 0.8), (1.4, 2.7, 0.9, 2.3, 1, None)]


def l1_feature(d, i, n):
    p = d["balcony"]
    return (p == "all") or (p == "alt" and i % 2 == 1) or (p == "mid" and i % 3 == 1) or (p == "ends" and (i <= 0 or i >= n - 1))


def l1_bay(R, d, kind, r, top):
    T = d["T"]
    opens = L1_BALC if kind == "balc" else L1_WIN[d["window"]]
    frame = d["frame"] if r.random() > 0.12 else C(FRAMES[r.integers(len(FRAMES))])
    for (x0, x1, y0, y1, mull, tr) in opens:
        if d["surround"] == "band": R.box(x0 - 0.14, x1 + 0.14, y0 - 0.14, y1 + 0.14, 0, T + 0.025, d["surround_col"])
        elif d["surround"] == "box": R.box(x0 - 0.1, x1 + 0.1, y0 - 0.02, y1 + 0.1, 0, T + 0.12, d["surround_col"] * 0.97)
        R.cut(x0, x1, y0, y1)
        frame_members(R, x0, x1, y0, y1, frame, mull, tr)
        if y0 > 0.5 and kind == "win":
            R.box(x0 - 0.07, x1 + 0.07, y0 - 0.06, y0, 0, T + 0.06, d["sill"]); R.shade(x0 - 0.07, x1 + 0.07, y0 - 0.012, y0, 1.15)
        if kind == "win" and d["window"] == "tall":
            R.box(x0 - 0.05, x1 + 0.05, 0.95, 1.0, T + 0.04, T + 0.09, (0.15, 0.15, 0.17))
            for xx in np.arange(x0 + 0.06, x1, 0.11): R.box(xx, xx + 0.018, y0 + 0.1, 0.95, T + 0.05, T + 0.07, (0.15, 0.15, 0.17))
        if d["shutters"] and kind == "win" and x1 - x0 < 1.6:
            sw = (x1 - x0) / 2; sc = d["shutter"] * r.uniform(0.85, 1.05); st = r.random()
            for side in (0, 1):
                closed = st < 0.15 or (st < 0.3 and side == 0)
                sx0, z0 = ((x0 + side * sw), 0.12) if closed else ((x0 - sw - 0.02 if side == 0 else x1 + 0.02), T)
                R.box(sx0, sx0 + sw, y0, y1, z0, z0 + 0.04, sc)
                for yy in np.arange(y0 + 0.05, y1, 0.07): R.shade(sx0 + 0.04, sx0 + sw - 0.04, yy, yy + 0.025, 0.72)
        if kind == "win" and y0 > 0.5 and r.random() < d["flower_p"]:
            R.box(x0, x1, y0 - 0.06, y0 + 0.14, T + 0.06, T + 0.26, C((0.45, 0.28, 0.18)) * r.uniform(0.7, 1.2))
            for _ in range(int((x1 - x0) * 9)):
                R.disc(r.uniform(x0, x1), y0 + 0.14 + r.uniform(0.02, 0.18), r.uniform(0.04, 0.08), T + 0.08, T + 0.26,
                       [(0.2, 0.4, 0.15), (0.75, 0.15, 0.2), (0.9, 0.8, 0.3), (0.85, 0.45, 0.65)][r.integers(4)])
        if kind == "win" and r.random() < d["awning_p"] and y1 > 2.0:
            R.sloped(x0 - 0.1, x1 + 0.1, y1 + 0.3, y1 - 0.35, T + 0.02, T + 0.75, 0.025, C(ACCENTS[r.integers(len(ACCENTS))]), (0.93, 0.92, 0.88) if r.random() < 0.7 else None)
    if kind == "win" and r.random() < d["ac_p"]:
        x0, x1, y0, y1 = opens[-1][:4]
        ax = (x1 + 0.12 if x1 + 0.95 < 3.0 else x0 - 0.9); ay = max(0.2, y0 - 0.7)
        R.box(ax, ax + 0.8, ay, ay + 0.55, T + 0.05, T + 0.33, (0.88, 0.88, 0.85)); R.disc(ax + 0.5, ay + 0.27, 0.19, T + 0.33, T + 0.335, (0.35, 0.35, 0.35))
        for k in range(5): R.shade(ax + 0.06, ax + 0.25, ay + 0.1 + k * 0.08, ay + 0.12 + k * 0.08, 0.6)
    if kind == "balc":
        P = 1.12; R.box(0.05, 2.95, 0.0, 0.16, 0, P, d["sill"] * 0.95)
        acc = C(ACCENTS[r.integers(len(ACCENTS))]) if d["multi"] else d["accent"]
        if r.random() < d["enclose_p"]:
            R.box(0.05, 2.95, 0.16, 1.0, P - 0.1, P, C(ACCENTS[r.integers(len(ACCENTS))]) * 0.8 if r.random() < 0.5 else acc)
            R.grad(0.05, 2.95, 1.0, 2.75, P - 0.08, P - 0.03, C((0.25, 0.33, 0.42)) * r.uniform(0.85, 1.1), C((0.6, 0.7, 0.8)) * r.uniform(0.85, 1.1))
            fc = C(FRAMES[r.integers(len(FRAMES))])
            for xx in np.linspace(0.05, 2.91, r.integers(3, 6)): R.box(xx, xx + 0.05, 1.0, 2.75, P - 0.1, P, fc)
            R.box(0.05, 2.95, 1.0, 1.05, P - 0.1, P, fc); R.box(0.05, 2.95, 2.7, 2.75, P - 0.1, P, fc)
            R.box(0.0, 3.0, 2.75, 2.88, 0, P + 0.03, (0.55, 0.56, 0.58))
            for sx in (0.05, 2.9): R.box(sx, sx + 0.05, 0.16, 2.75, 0, P, fc)
        else:
            if d["rail"] == "solid":
                R.box(0.05, 2.95, 0.16, 1.05, P - 0.08, P, acc); R.shade(0.05, 2.95, 1.0, 1.05, 1.15)
                for sx in (0.05, 2.88): R.box(sx, sx + 0.07, 0.16, 1.05, 0, P, acc * 0.9)
            else:
                rc = C((0.16, 0.16, 0.18)) if r.random() < 0.8 else acc
                R.box(0.05, 2.95, 1.0, 1.05, P - 0.06, P, rc)
                step = 0.11 if d["rail"] == "bars" else 0.05
                for xx in np.arange(0.08, 2.92, step): R.box(xx, xx + (0.018 if d["rail"] == "bars" else 0.01), 0.16, 1.0, P - 0.05, P - 0.02, rc)
                if d["rail"] == "mesh":
                    for yy in np.arange(0.2, 1.0, 0.05): R.box(0.08, 2.92, yy, yy + 0.01, P - 0.05, P - 0.02, rc)
                for sx in (0.05, 2.9): R.box(sx, sx + 0.05, 0.16, 1.05, 0, P, rc)
            balcony_things(R, r, 0.1, 2.9, P)
    if r.random() < d["dish_p"] and kind == "win": R.disc(r.uniform(0.4, 2.6), r.uniform(0.3, 0.7), 0.28, T, T + 0.18, (0.9, 0.9, 0.88))


def l1_storey(R, d, W, top):
    if d.get("panels"):
        for xx in np.arange(0, W + 0.01, 1.5 if d["window"] == "double" else 3.0): R.box(xx - 0.012, xx + 0.012, 0, 3.0, 0, d["T"] - 0.015, d["paint"] * 0.55)
        R.box(0, W, 2.988, 3.0, 0, d["T"] - 0.015, d["paint"] * 0.55)
    if d.get("band"):
        R.box(0, W, 0.0, 0.16, 0, d["T"] + 0.035, d["surround_col"] * 0.95 if d["surround"] != "none" else d["paint"] * 0.9); R.shade(0, W, 0.14, 0.16, 1.15)


# ---------------------------------------------------------------- L2 brick
def window_glass(R, x0, x1, y0, y1, z0, z1, r):
    """A pane drawn into the relief itself (on a bay window, where the glass field cannot show): sky over a dim room."""
    R.grad(x0, x1, y0, y1, z0, z1, C((0.16, 0.18, 0.2)) * r.uniform(0.8, 1.2), C((0.5, 0.58, 0.66)) * r.uniform(0.85, 1.1))


def awning(R, r, x0, x1, ytop, T, col, stripes=None, drop=0.7, out=0.8):
    R.sloped(x0 - 0.08, x1 + 0.08, ytop, ytop - drop, T + 0.02, T + out, 0.025, col, stripes)
    R.box(x0 - 0.08, x1 + 0.08, ytop - drop - 0.12, ytop - drop, T + out - 0.05, T + out + 0.02, col * 0.85)   # its valance


def l2_bay(R, d, kind, r, top):
    T = d["T"]; st = d["stone"]
    replaced = r.random() < d["replaced_p"]                                                  # an owner swapped in white uPVC
    fr = C((0.95, 0.95, 0.94)) if replaced else d["frame"]
    if kind == "oriel":                                                                      # a projecting bay window, stacked up the face
        P = 0.55; body = d["oriel_col"]
        R.box(0.3, 2.7, 0.25, 2.8, 0, T + P, body); R.shade(0.3, 2.7, 2.7, 2.8, 1.15); R.shade(0.3, 2.7, 0.25, 0.32, 0.6)
        for (a, b) in ((0.45, 1.15), (1.25, 1.75), (1.85, 2.55)):
            window_glass(R, a, b, 0.75, 2.45, T + P - 0.02, T + P - 0.01, r)
            frame_members(R, a, b, 0.75, 2.45, fr, transom=0.5, fw=0.05, z=(T + P - 0.03, T + P + 0.02))
        if r.random() < d["flower_p"] * 1.5:
            R.box(0.4, 2.6, 0.55, 0.75, T + P, T + P + 0.18, C((0.25, 0.25, 0.27)))
            for _ in range(18): R.disc(r.uniform(0.45, 2.55), r.uniform(0.75, 0.95), r.uniform(0.05, 0.09), T + P, T + P + 0.2, [(0.2, 0.42, 0.15), (0.8, 0.2, 0.25), (0.9, 0.85, 0.85)][r.integers(3)])
        return
    if d["rhythm"] == "single": opens = [(0.85, 2.15)]
    elif d["rhythm"] == "pair": opens = [(0.45, 1.35), (1.65, 2.55)]
    else: opens = [(0.35, 2.65)]
    y0, y1 = (0.15 if (d["juliet"] and kind == "feat") else 0.75), 2.45
    for (x0, x1) in opens:
        wide = x1 - x0 > 1.6
        if (d["lintel"] == "arch" or wide) and not replaced:
            cx = (x0 + x1) / 2; rr = (x1 - x0) * 0.75; cy = y1 - rr + 0.22
            R.cut(x0, x1, y0, y1); R.cut_disc(cx, cy, rr, y1 - 0.01)
            R.ring(cx, cy, rr, rr + 0.12, y1 - 0.05, 0, T + 0.02, C((0.55, 0.3, 0.22)) * (d["paint"] if d["mode"] == "tint" else 1.0) * 0.9)
            frame_members(R, x0, x1, y0, y1, fr, mull=2 if wide else 1, transom=0.55, fw=0.06, grid=4 if wide else d["muntins"])
        else:
            R.cut(x0, x1, y0, y1)
            frame_members(R, x0, x1, y0, y1, fr, mull=1 if replaced else 0, transom=None if replaced else 0.5, fw=0.07 if replaced else 0.065,
                          grid=0 if replaced else d["muntins"] - 1)
            if d["lintel"] in ("flat", "key"):
                R.box(x0 - 0.12, x1 + 0.12, y1, y1 + 0.24, 0, T + 0.03, st); R.shade(x0 - 0.12, x1 + 0.12, y1, y1 + 0.02, 0.7)
                if d["lintel"] == "key": R.box((x0 + x1) / 2 - 0.1, (x0 + x1) / 2 + 0.1, y1 - 0.04, y1 + 0.3, 0, T + 0.06, st * 1.05)
            else:
                for k, xx in enumerate(np.arange(x0 - 0.06, x1 + 0.06, 0.085)):
                    R.box(xx, xx + 0.075, y1, y1 + 0.22, 0, T + 0.01, C((0.5, 0.28, 0.2)) * (0.85 + 0.15 * (k % 3) / 2))
        if y0 > 0.5:
            R.box(x0 - 0.08, x1 + 0.08, y0 - 0.08, y0, 0, T + 0.07, st); R.shade(x0 - 0.08, x1 + 0.08, y0 - 0.015, y0, 1.15)
            u = r.random()
            if u < d["window_ac_p"]:                                                         # a window unit in the lower sash
                R.box(x0 + 0.05, x1 - 0.05, y0, y0 + 0.42, 0.05, T + 0.28, C((0.84, 0.82, 0.76)) * r.uniform(0.85, 1.05))
                for k in range(6): R.shade(x0 + 0.1, x1 - 0.1, y0 + 0.06 + k * 0.055, y0 + 0.08 + k * 0.055, 0.6)
            elif u < d["window_ac_p"] + d["flower_p"]:
                R.box(x0, x1, y0, y0 + 0.2, T + 0.07, T + 0.27, C((0.2, 0.2, 0.22)) if r.random() < 0.6 else C((0.45, 0.28, 0.18)))
                for _ in range(int((x1 - x0) * 10)):
                    R.disc(r.uniform(x0, x1), y0 + 0.2 + r.uniform(0.02, 0.22), r.uniform(0.04, 0.09), T + 0.08, T + 0.3,
                           [(0.2, 0.42, 0.15), (0.8, 0.2, 0.25), (0.92, 0.85, 0.85), (0.9, 0.55, 0.2), (0.6, 0.3, 0.7)][r.integers(5)])
            if r.random() < d["sleeve_p"]:                                                   # a through-wall AC sleeve under the sill
                R.box((x0 + x1) / 2 - 0.35, (x0 + x1) / 2 + 0.35, y0 - 0.55, y0 - 0.15, 0, T + 0.04, (0.7, 0.7, 0.68))
                for k in range(5): R.shade((x0 + x1) / 2 - 0.32, (x0 + x1) / 2 + 0.32, y0 - 0.5 + k * 0.07, y0 - 0.48 + k * 0.07, 0.55)
        elif d["juliet"]:
            R.box(x0 - 0.1, x1 + 0.1, 0.0, 0.08, 0, T + 0.35, st)
            for xx in np.arange(x0 - 0.05, x1 + 0.08, 0.1): R.box(xx, xx + 0.02, 0.08, 1.0, T + 0.3, T + 0.33, (0.08, 0.08, 0.09))
            R.box(x0 - 0.1, x1 + 0.1, 0.95, 1.0, T + 0.28, T + 0.35, (0.08, 0.08, 0.09))
            if r.random() < 0.4: plant(R, r, (x0 + x1) / 2, 0.08, T + 0.05, T + 0.33, (0.1, 0.6), 5)
        if r.random() < d["awning_p"]:
            awning(R, r, x0, x1, y1 + 0.32, T, d["awn_col"], (0.93, 0.92, 0.88) if d["awn_stripe"] else None, drop=0.6, out=0.7)
    if r.random() < d["flag_p"]:                                                             # a flag on an angled pole
        fx = r.uniform(0.3, 1.6)
        for t in np.linspace(0, 1, 14): R.box(fx + 0.6 * t, fx + 0.6 * t + 0.04, 2.0 + 0.5 * t, 2.04 + 0.5 * t, T, T + 0.04 + 0.5 * t, (0.8, 0.8, 0.8))
        R.box(fx + 0.6, fx + 1.3, 2.05, 2.5, T + 0.5, T + 0.53, ACCENTS[r.integers(len(ACCENTS))])
    if r.random() < d["dish_p"]: R.disc(r.uniform(0.3, 2.7), r.uniform(0.3, 0.6), 0.27, T, T + 0.2, (0.9, 0.9, 0.88))
    if kind == "escape":                                                                     # a fire escape platform and its stair
        ink = C((0.07, 0.07, 0.08)); P = 1.05
        R.box(0.05, 2.95, 0.0, 0.06, 0, P, ink)
        for xx in np.arange(0.08, 2.95, 0.12): R.box(xx, xx + 0.015, 0.06, 0.95, P - 0.05, P - 0.02, ink)
        R.box(0.05, 2.95, 0.92, 0.97, P - 0.06, P, ink)
        for sx in (0.05, 2.9): R.box(sx, sx + 0.05, 0.06, 0.97, 0, P, ink)
        for k in range(14):
            t = k / 13; sx = 0.3 + 2.3 * t; sy = 0.06 + 2.9 * t
            R.box(sx, sx + 0.22, sy, sy + 0.035, P * 0.35, P * 0.75, ink)
        for t in np.linspace(0, 1, 40):
            sx = 0.3 + 2.3 * t; sy = 0.06 + 2.9 * t
            R.box(sx, sx + 0.05, sy + 0.5, sy + 0.55, P * 0.7, P * 0.76, ink)
        for _ in range(r.integers(0, 4)):                                                    # life on the landing: pots, a chair, a bike
            k = r.integers(3); cx = r.uniform(0.4, 2.0)
            if k == 0: R.box(cx - 0.1, cx + 0.1, 0.06, 0.3, 0.2, 0.45, (0.55, 0.3, 0.2)); plant(R, r, cx, 0.25, 0.15, 0.6, (0.05, 0.5), 5)
            elif k == 1: R.box(cx - 0.2, cx + 0.2, 0.45, 0.49, 0.2, 0.6, ACCENTS[r.integers(len(ACCENTS))]); R.box(cx - 0.2, cx + 0.2, 0.45, 0.9, 0.18, 0.22, ACCENTS[r.integers(len(ACCENTS))])
            else: R.ring(cx, 0.35, 0.26, 0.3, 0.0, 0.4, 0.45, (0.1, 0.1, 0.1)); R.ring(cx + 0.9, 0.35, 0.26, 0.3, 0.0, 0.4, 0.45, (0.1, 0.1, 0.1))


def l2_storey(R, d, W, top):
    if d["band"]: R.box(0, W, 0.0, 0.12, 0, d["T"] + 0.03, d["stone"] * 0.95); R.shade(0, W, 0.1, 0.12, 1.12)
    if d["poly"]:                                                                            # polychrome brick bands
        R.box(0, W, 1.55, 1.75, 0, d["T"], d["poly_col"])
        for xx in np.arange(0, W, 0.23): R.shade(xx, xx + 0.01, 1.55, 1.75, 0.7)
    if d["pipe"]:                                                                            # a downpipe at the run's left edge
        R.box(0.08, 0.16, 0, 3.0, d["T"], d["T"] + 0.1, C((0.15, 0.15, 0.16)))
    if top and d["cornice"]:
        st = d["stone"] if d["flavour"] != "nyc" else C((0.55, 0.5, 0.45))
        R.box(0, W, 2.55, 2.75, 0, d["T"] + 0.25, st * 0.9)
        R.box(0, W, 2.75, 3.0, 0, d["T"] + 0.55, st); R.shade(0, W, 2.75, 2.8, 0.6)
        for xx in np.arange(0.05, W, 0.35): R.box(xx, xx + 0.12, 2.6, 2.75, 0, d["T"] + 0.4, st * 0.95)


# ---------------------------------------------------------------- L3 glass
def l3_bay(R, d, kind, r, top):
    m = d["metal"]; vis = d["vision"]
    if kind == "loggia":                                                                     # a corner balcony carved out of the glass
        R.box(0, 3.0, 0, 0.25, 0, 1.0, C((0.75, 0.75, 0.73))); R.shade(0, 3.0, 0.22, 0.25, 1.15)
        R.grad(0.02, 2.98, 0.25, 1.3, 0.95, 0.98, C((0.45, 0.55, 0.6)), C((0.7, 0.78, 0.84))); R.box(0.0, 3.0, 1.27, 1.32, 0.93, 1.0, m)
        R.box(0, 3.0, 2.85, 3.0, 0, 1.0, C((0.7, 0.7, 0.7)))
        frame_members(R, 0.03, 2.97, 0.25, 2.85, m, mull=2, fw=0.05, z=(0.0, 0.06))
        balcony_things(R, r, 0.3, 2.7, 0.8)
        return
    sp = d["spandrel"]
    acc = None
    if r.random() < d["accent_p"]: acc = d["accents"][r.integers(len(d["accents"]))]
    if acc is not None: R.box(0, 3.0, vis, 3.0, 0, 0.04, acc)
    elif sp == "glass": R.grad(0, 3.0, vis, 3.0, 0, 0.03, C((0.12, 0.15, 0.18)), C((0.28, 0.33, 0.38)))
    elif sp == "metal": R.box(0, 3.0, vis, 3.0, 0, 0.04, d["spandrel_col"])
    elif sp == "stone": R.box(0, 3.0, vis, 3.0, 0, 0.06, C((0.82, 0.8, 0.75)) * r.uniform(0.95, 1.05))
    else:
        R.grad(0, 3.0, vis, 3.0, 0, 0.03, C((0.2, 0.24, 0.28)), C((0.3, 0.35, 0.4)))
        R.dots(0, 3.0, vis, 3.0, 0.05, 0.013, 0, 0.032, (0.85, 0.85, 0.82))
    if r.random() < 0.08: R.box(2.0, 2.92, vis - 0.6, vis - 0.05, 0, 0.02, (0.04, 0.05, 0.06))
    for xx in np.arange(0, 3.0, d["module"]):
        R.box(xx - 0.03, xx + 0.03, 0, 3.0, 0, 0.12, m); R.shade(xx - 0.03, xx - 0.015, 0, 3.0, 1.15)
    R.box(0, 3.0, vis - 0.03, vis + 0.03, 0, 0.1, m); R.box(0, 3.0, 2.97, 3.0, 0, 0.1, m * 0.9)
    if d["fin"] > 0:
        for k, xx in enumerate(np.arange(0, 3.0, d["module"])):
            if k % d["fin_every"] == 0: R.box(xx - 0.04, xx + 0.04, 0, 3.0, 0, d["fin"], d["fin_col"])
    if d["sunshade"]:
        for k in range(3): R.box(0.0, 3.0, vis + 0.06 + k * 0.1, vis + 0.1 + k * 0.1, 0.15 + 0.12 * k, 0.65, m * (1.05 - 0.05 * k))


def l3_storey(R, d, W, top):
    if top: R.box(0, W, 2.4, 3.0, 0, 0.22, d["metal"] * 0.9)


# ---------------------------------------------------------------- L4 deco
def l4_bay(R, d, kind, r, top):
    T = d["T"]; pw = d["pilaster"]; orn = d["orn"]
    replaced = r.random() < d["replaced_p"]
    fr = C((0.92, 0.92, 0.9)) if replaced else d["frame"]
    R.box(0, pw, 0, 3.0, 0, T + 0.14, d["paint"] * 1.06); R.shade(0, 0.03, 0, 3.0, 1.12); R.shade(pw - 0.03, pw, 0, 3.0, 0.8)
    inner0, inner1 = pw + 0.12, 3.0 - 0.12
    n = {"single": 1, "pair": 2, "triple": 3}[d["rhythm"]]
    w = (inner1 - inner0 - 0.12 * (n - 1)) / n
    y0, y1 = 0.62, d["head"]
    for k in range(n):
        x0 = inner0 + k * (w + 0.12); x1 = x0 + w
        if top and d["top_arch"]:
            R.cut(x0, x1, y0, y1 - w / 2); R.cut_disc((x0 + x1) / 2, y1 - w / 2, w / 2, y1 - w / 2 - 0.01)
            frame_members(R, x0, x1, y0, y1 - w / 2, fr, mull=0, transom=0.8, fw=0.05, z=(0.08, 0.16))
        else:
            R.cut(x0, x1, y0, y1)
            frame_members(R, x0, x1, y0, y1, fr, mull=1 if w > 0.9 else 0, transom=None if replaced else 0.82, fw=0.05, z=(0.08, 0.16))
        u = r.random()
        if u < d["ac_p"]:
            R.box(x0 + 0.04, x1 - 0.04, y0, y0 + 0.4, 0.08, T + 0.25, C((0.84, 0.82, 0.76)) * r.uniform(0.85, 1.05))
            for j in range(5): R.shade(x0 + 0.08, x1 - 0.08, y0 + 0.06 + j * 0.06, y0 + 0.08 + j * 0.06, 0.6)
        elif u < d["ac_p"] + d["flower_p"]:
            R.box(x0, x1, y0 - 0.02, y0 + 0.16, T, T + 0.2, C((0.25, 0.25, 0.27)))
            for _ in range(int(w * 10)):
                R.disc(r.uniform(x0, x1), y0 + 0.16 + r.uniform(0.02, 0.2), r.uniform(0.04, 0.08), T + 0.02, T + 0.22, [(0.2, 0.42, 0.15), (0.8, 0.2, 0.25), (0.92, 0.85, 0.85)][r.integers(3)])
        if r.random() < d["awning_p"] and not (top and d["top_arch"]):
            awning(R, r, x0, x1, y1 + 0.25, T, d["awn_col"], None, drop=0.55, out=0.6)
    if kind == "balc":                                                                       # a small iron balcony
        R.box(inner0 - 0.05, inner1 + 0.05, 0.0, 0.12, 0, T + 0.6, d["paint"] * 0.95)
        for xx in np.arange(inner0, inner1, 0.09): R.box(xx, xx + 0.018, 0.12, 1.0, T + 0.54, T + 0.57, (0.08, 0.08, 0.09))
        R.box(inner0 - 0.05, inner1 + 0.05, 0.95, 1.0, T + 0.52, T + 0.6, (0.08, 0.08, 0.09))
        for sx in (inner0 - 0.05, inner1): R.box(sx, sx + 0.05, 0.12, 1.0, T, T + 0.6, (0.08, 0.08, 0.09))
        if r.random() < 0.5: plant(R, r, r.uniform(inner0 + 0.3, inner1 - 0.3), 0.12, T + 0.1, T + 0.55, (0.1, 0.6), 5)
    R.box(inner0, inner1, 0.08, 0.55, 0, T - 0.14, orn)
    if d["chevron"]:
        for xx in np.arange(inner0, inner1, 0.18):
            for t in np.linspace(0, 1, 6):
                R.box(xx + 0.09 * t, xx + 0.09 * t + 0.03, 0.15 + 0.25 * t, 0.15 + 0.25 * t + 0.03, 0, T - 0.1, orn * 1.25)
                R.box(xx + 0.09 + 0.09 * t, xx + 0.09 + 0.09 * t + 0.03, 0.4 - 0.25 * t, 0.4 - 0.25 * t + 0.03, 0, T - 0.1, orn * 1.25)
    else:
        R.box(inner0 + 0.08, inner1 - 0.08, 0.16, 0.47, 0, T - 0.1, orn * 1.2)


def l4_storey(R, d, W, top):
    if top:
        R.box(0, W, 2.62, 3.0, 0, d["T"] + 0.3, d["paint"] * 0.95)
        for xx in np.arange(0, W, 0.3):
            R.box(xx, xx + 0.15, 2.66, 2.96, d["T"] + 0.3, d["T"] + 0.38, d["orn"])
        R.shade(0, W, 2.62, 2.66, 0.6)


# ---------------------------------------------------------------- special storeys
SPECIAL_H = {"mech": 3.0, "garden": 6.0, "frieze": 3.0}


def special_strip(d, kind, nb, seed, H=None):
    """Storeys that break the rhythm:
      mech    a plant floor: dark louvre blades in a frame, deep shadow between them
      garden  a double-height sky garden: paving, trees and shrubs in planters, benches, a soffit above, posts and a handrail
      frieze  an ornamental belt (deco): rosettes, a cornice, small square windows
      screen  the curtain wall carried past the roof (H metres) to hide the plant: framed reflective panels, a metal cap"""
    rng = np.random.default_rng(seed); W = nb * BAY
    H = H or SPECIAL_H[kind]
    R = Relief(W, H)
    metal = d.get("metal", d.get("frame", C((0.3, 0.3, 0.32))))
    if kind == "mech":
        frame = C((0.34, 0.35, 0.37)) * 0.6 + metal * 0.4 * 0.6
        R.box(0, W, 0, H, 0, 0.03, C((0.05, 0.055, 0.06)))                              # the dark plant room behind
        for yy in np.arange(0.3, H - 0.3, 0.16):                                            # blades, lit on top, shadowed under
            R.box(0, W, yy, yy + 0.1, 0.06, 0.26, C((0.4, 0.41, 0.43)))
            R.shade(0, W, yy + 0.075, yy + 0.1, 1.35); R.shade(0, W, yy, yy + 0.025, 0.5)
        for xx in np.arange(0, W + 0.01, 1.5): R.box(xx - 0.05, xx + 0.05, 0, H, 0, 0.3, frame)
        R.box(0, W, 0, 0.3, 0, 0.32, frame * 1.1); R.box(0, W, H - 0.3, H, 0, 0.32, frame * 1.1); R.shade(0, W, H - 0.3, H - 0.27, 0.6)
    elif kind == "garden":
        R.grad(0, W, 0.35, H - 0.35, 0, 0.02, C((0.1, 0.12, 0.14)), C((0.32, 0.38, 0.44)))   # glazing at the back of the terrace
        for xx in np.arange(0, W + 0.01, 1.5): R.box(xx - 0.03, xx + 0.03, 0.35, H - 0.35, 0, 0.08, metal)
        R.box(0, W, 3.0, 3.06, 0, 0.08, metal)
        R.box(0, W, 0, 0.35, 0, 1.15, C((0.72, 0.7, 0.66))); R.shade(0, W, 0.32, 0.35, 1.15)    # the terrace slab
        R.box(0, W, H - 0.4, H, 0, 1.15, C((0.78, 0.78, 0.76))); R.shade(0, W, H - 0.4, H - 0.35, 0.45)   # the soffit above
        for x in np.arange(rng.uniform(0.8, 1.6), W - 0.4, rng.uniform(2.2, 3.2)):         # trees in big planters
            R.box(x - 0.55, x + 0.55, 0.35, 0.9, 0.25, 0.95, C((0.32, 0.32, 0.34)))
            R.box(x - 0.05, x + 0.05, 0.9, 3.2, 0.55, 0.65, (0.32, 0.24, 0.16))
            for _ in range(26):
                R.disc(x + rng.uniform(-1.0, 1.0), rng.uniform(2.6, 5.1), rng.uniform(0.3, 0.6), 0.15, 1.12, C((0.16, 0.34, 0.12)) * rng.uniform(0.6, 1.45))
        for x in np.arange(0.4, W - 0.3, 0.45):                                             # shrubs along the edge
            if rng.random() < 0.7: R.disc(x, 0.6, rng.uniform(0.18, 0.3), 0.7, 1.1, C((0.2, 0.4, 0.15)) * rng.uniform(0.7, 1.3))
        if rng.random() < 0.6:                                                              # a bench
            bx = rng.uniform(0.5, W - 2.0); R.box(bx, bx + 1.4, 0.75, 0.8, 0.2, 0.6, (0.5, 0.36, 0.22)); R.box(bx, bx + 1.4, 0.35, 0.75, 0.25, 0.3, (0.2, 0.2, 0.2))
        for xx in np.arange(0.05, W, 1.5): R.box(xx, xx + 0.04, 0.35, 1.45, 1.09, 1.13, (0.7, 0.71, 0.73))   # posts and rail only:
        R.box(0, W, 1.4, 1.46, 1.07, 1.15, (0.76, 0.77, 0.79))                             # the planting stays in view
    elif kind == "screen":
        tint = C(d.get("glass", (60, 66, 72))) / 255.0
        lo, hi = tint * 0.9 + 0.08, tint * 0.6 + C((0.45, 0.5, 0.55))
        R.grad(0, W, 0, H - 0.35, 0, 0.03, lo, hi)                                          # framed reflective panels
        for xx in np.arange(0, W + 0.01, d.get("module", 1.5)): R.box(xx - 0.04, xx + 0.04, 0, H, 0, 0.14, metal)
        for yy in np.arange(1.5, H - 0.4, 1.5): R.box(0, W, yy - 0.03, yy + 0.03, 0, 0.1, metal)
        if d.get("fin", 0) > 0:
            for k, xx in enumerate(np.arange(0, W + 0.01, d.get("module", 1.5))):
                if k % d.get("fin_every", 1) == 0: R.box(xx - 0.04, xx + 0.04, 0, H, 0, d["fin"], d.get("fin_col", metal))
        R.box(0, W, H - 0.35, H, 0, 0.35, metal * 1.05); R.shade(0, W, H - 0.35, H - 0.3, 0.6)
    else:                                                                                    # frieze
        T = d["T"]
        R.box(0, W, 0, H, 0, T, cladding(d["clad"], d["clad_m"], d["paint"], W, H, rng, d["mode"]))
        R.box(0, W, 2.55, 3.0, 0, T + 0.35, d["paint"] * 0.95); R.shade(0, W, 2.55, 2.6, 0.6)
        R.box(0, W, 0.0, 0.3, 0, T + 0.2, d["paint"] * 0.9)
        for x in np.arange(0.75, W, 1.5):
            R.disc(x, 1.9, 0.32, 0, T + 0.12, d["orn"]); R.disc(x, 1.9, 0.18, 0, T + 0.18, d["orn"] * 1.3)
            R.cut(x - 0.3, x + 0.3, 0.55, 1.35); frame_members(R, x - 0.3, x + 0.3, 0.55, 1.35, d["frame"], fw=0.04, z=(0.08, 0.14))
        for x in np.arange(0, W, 0.25): R.box(x, x + 0.12, 2.6, 2.75, T + 0.35, T + 0.42, d["orn"] * 0.9)
    return R.encode(seed)


# ---------------------------------------------------------------- L5 modern
def l5_projection(d, i, s, r):
    p = d["pattern"]
    if p == "flush": return 1.12
    if p == "jenga": return float(r.choice([0.35, 0.7, 1.12]))
    if p == "checker": return 1.12 if (i + s) % 2 == 0 else 0.3
    if p == "wave": return 0.35 + 0.77 * (0.5 + 0.5 * math.sin(i * 0.9 + s * 0.45))
    return 1.12 if i % 2 == 0 else 0.3


def l5_bay(R, d, kind, r, top, P=1.12):
    sl = d["slab"]; fr = d["frame"]
    R.box(0, 3.0, 0.0, 0.28, 0, P, sl); R.shade(0, 3.0, 0.26, 0.28, 1.12); R.shade(0, 3.0, 0.0, 0.03, 0.7)
    frame_members(R, 0.04, 2.96, 0.28, 3.0, fr, mull=2 if r.random() < 0.6 else 1, transom=None, fw=0.05, z=(0.0, 0.07))
    if P > 0.6:
        rail = d["rail"]
        if rail == "glass":
            R.grad(0.03, 2.97, 0.32, 1.35, P - 0.035, P - 0.015, C((0.5, 0.62, 0.66)), C((0.72, 0.82, 0.86)))
            R.box(0.03, 2.97, 1.3, 1.36, P - 0.05, P, (0.75, 0.76, 0.78))
        elif rail == "rods":
            for xx in np.arange(0.06, 2.96, 0.09): R.box(xx, xx + 0.015, 0.28, 1.3, P - 0.04, P - 0.02, fr)
            R.box(0.03, 2.97, 1.28, 1.32, P - 0.05, P, fr)
        elif rail == "solid":
            R.box(0.03, 2.97, 0.28, 1.15, P - 0.06, P, sl * 0.98); R.shade(0.03, 2.97, 1.12, 1.15, 1.1)
        else:
            for yy in np.arange(0.32, 1.25, 0.11): R.box(0.03, 2.97, yy, yy + 0.08, P - 0.05, P, C((0.62, 0.45, 0.3)) * r.uniform(0.9, 1.1))
        if d["screens"]: R.box(0.0, 0.05, 0.28, 2.95, 0, P, C((0.82, 0.85, 0.86)))
        if d["green"]:
            R.box(0.05, 2.95, 0.28, 0.75, P - 0.45, P - 0.06, C((0.45, 0.42, 0.4)))
            for _ in range(r.integers(6, 14)):
                R.disc(r.uniform(0.1, 2.9), r.uniform(0.75, 1.5), r.uniform(0.12, 0.3), P - 0.55, min(DEPTH, P + 0.06), C((0.16, 0.34, 0.12)) * r.uniform(0.6, 1.4))
            if r.random() < 0.3:
                tx = r.uniform(0.6, 2.4); R.box(tx - 0.03, tx + 0.03, 0.75, 1.9, P - 0.3, P - 0.25, (0.3, 0.22, 0.15))
                for _ in range(10): R.disc(tx + r.uniform(-0.5, 0.5), r.uniform(1.6, 2.6), r.uniform(0.2, 0.4), P - 0.6, P - 0.05, C((0.2, 0.38, 0.14)) * r.uniform(0.7, 1.3))
        else:
            balcony_things(R, r, 0.2, 2.8, P - 0.1)


STYLE = {1: (l1_bay, l1_storey), 2: (l2_bay, l2_storey), 3: (l3_bay, l3_storey), 4: (l4_bay, l4_storey)}


def strip(d, nb, ns, first, seed, top, n_face):
    """One SPOM relief: nb bays by ns storeys. first: the face index of its first bay; top: its uppermost storey is the building's."""
    rng = np.random.default_rng(seed)
    W, H = nb * BAY, ns * STOREY
    R = Relief(W, H)
    if d["wall"]:
        clad = cladding(d["clad"], d["clad_m"], d["paint"], W, H, rng, d["mode"])
        R.box(0, W, 0, H, 0, d["T"], clad)
        streak = np.asarray(Image.fromarray((rng.random((R.ny // 16 + 1, R.nx)) * 255).astype(np.uint8)).resize((R.nx, R.ny), Image.BILINEAR), np.float32) / 255
        R.col *= (1 - 0.07 * np.clip(streak * 2 - 1.2, 0, 1))[..., None]
    L = d["level"]
    for s in range(ns):
        is_top = top and s == ns - 1
        if L == 1 and is_top and d["two_tone"]:
            m = R.at(0, s * 3).mask(0, W, 0, 3.0); R.col[m] = cladding(d["clad"], d["clad_m"], d["paint2"], W, H, rng)[m]
        for b in range(nb):
            i = first + b; r = np.random.default_rng(seed * 7919 + s * 131 + b * 17 + 1)
            R.at(b * BAY, s * STOREY)
            if L == 1: l1_bay(R, d, "balc" if l1_feature(d, i, n_face) else "win", r, is_top)
            elif L == 2: l2_bay(R, d, "escape" if (i % 4) in d["escape_cols"] else ("oriel" if (i % 4) in d["oriel_cols"] else ("feat" if i % 2 == 0 else "win")), r, is_top)
            elif L == 3: l3_bay(R, d, "loggia" if d["corner_balc"] and (i == 0 or i == n_face - 1) else "win", r, is_top)
            elif L == 4: l4_bay(R, d, "balc" if (i % 3) in d["balc_cols"] and r.random() < 0.8 else "win", r, is_top)
            else: l5_bay(R, d, "win", r, is_top, P=l5_projection(d, i, s, r))
        R.at(0, s * STOREY)
        if L in STYLE: STYLE[L][1](R, d, W, is_top)
    if L == 4:                                                                       # a closing pilaster on the run's right edge
        R.at(0, 0); R.box(W - d["pilaster"] * 0.5, W, 0, H, 0, d["T"] + 0.14, d["paint"] * 1.06)
    return R.encode(seed)


# ---------------------------------------------------------------- the podium
def podium_design(level, rng, d):
    pick = lambda xs: xs[rng.integers(len(xs))]
    clads = [(CONCRETE[0], "paint", (0.32, 0.32, 0.34)), (CONCRETE[1], "paint", (0.85, 0.82, 0.76)), (BRICK, "tint", (1.0, 0.95, 0.92)),
             (CONCRETE[2], "paint", (0.62, 0.6, 0.58)), (CONCRETE[3], "paint", (0.9, 0.88, 0.84)), (CONCRETE[0], "paint", (0.2, 0.2, 0.22))]
    c = pick(clads)
    if level == 2 and rng.random() < 0.6: c = (BRICK, "tint", tuple(d["paint"]))
    name = (pick(NAMES_A) + " " + pick(NAMES_B)).strip()
    return dict(clad=c[0], mode=c[1], paint=C(c[2]), frame=C(pick([(0.12, 0.12, 0.13), (0.42, 0.32, 0.22), (0.7, 0.72, 0.74), (0.2, 0.2, 0.22)])),
                metal=C(pick([(0.75, 0.76, 0.78), (0.8, 0.68, 0.45), (0.15, 0.15, 0.16)])), name=name, number=str(int(rng.integers(10, 990))),
                canopy=C(pick([(0.15, 0.15, 0.16), (0.85, 0.85, 0.83), (0.45, 0.35, 0.25)])), planters=rng.random() < 0.7,
                shop_awning=rng.random() < 0.6)


def podium_signs(W, H, kind, entrance=None, garage=None):
    """Where a podium run's signs go, as (x0, x1, y0, y1, z) in the run's metres: the building's name over the entrance, its number on
    the canopy's fascia, a shop sign over every shopfront. Each becomes a separate blank 'signboard' quad (0..1 uv) for any texture or
    lettering; podium_strip leaves a recess there."""
    out = []
    nb = max(1, int(round(W / 3.0))); bw = W / nb
    nearest = lambda x: None if x is None else int(np.clip(x // bw, 0, nb - 1))
    eb, gb = nearest(entrance), nearest(garage)
    two = H > 6.0; g1 = 3.6 if two else H - 0.5
    for b in range(nb):
        x0 = b * bw; cx = x0 + bw / 2
        if b == eb:
            out.append((cx - 2.5, cx + 2.5, 3.5, min(H - 0.4, 4.5), 0.06))
            out.append((cx - 2.5, cx + 2.5, 3.07, 3.28, 1.205))
        elif b != gb and kind == "side" and b % 2 == 0:
            gx0, gx1 = x0 + 0.28, x0 + bw - 0.28
            sy0, sy1 = (g1 + 0.1, g1 + 0.7) if two else (g1 - 0.55, g1)
            out.append((gx0 + 0.06, gx1 - 0.06, sy0 + 0.05, sy1 - 0.05, 0.06))
    return out


def podium_strip(pd, W, H, kind, seed, entrance=None, garage=None):
    """A podium face run: W wide, H (podium height) plus 1.2 m of balustrade posts above. kind: front / side / back.
    entrance, garage: x of their centre within the run, or None."""
    rng = np.random.default_rng(seed)
    R = Relief(W, H + 1.2)
    T = 0.3
    clad = cladding(pd["clad"], 2.6, pd["paint"], W, H + 1.2, rng, pd["mode"])
    R.box(0, W, 0, H, 0, T, clad)
    R.box(0, W, H - 0.3, H, 0, T + 0.1, pd["paint"] * 1.08); R.shade(0, W, H - 0.3, H - 0.27, 0.65)        # coping
    for xx in np.arange(0.05, W, 1.5): R.box(xx, xx + 0.05, H, H + 1.08, 0.0, 0.06, pd["metal"])          # balustrade posts
    R.box(0, W, H + 1.04, H + 1.1, 0.0, 0.08, pd["metal"])
    nb = max(1, int(round(W / 3.0))); bw = W / nb
    nearest = lambda x: None if x is None else int(np.clip(x // bw, 0, nb - 1))
    eb, gb = nearest(entrance), nearest(garage)
    order = [b for b in range(nb) if b != eb] + ([eb] if eb is not None else [])     # the entrance last: its canopy and name overlap its neighbours
    for b in order:
        x0 = b * bw; cx = x0 + bw / 2
        if gb == b:
            R.box(cx - 1.7, cx + 1.7, 0, 3.0, 0, 0.12, C((0.55, 0.56, 0.58)))
            for yy in np.arange(0.05, 3.0, 0.12): R.shade(cx - 1.7, cx + 1.7, yy, yy + 0.02, 0.7)
            R.box(cx - 1.85, cx + 1.85, 3.0, 3.15, 0, T + 0.04, pd["paint"] * 0.8)
            continue
        two = H > 6.0                                                             # a two- or three-storey podium: ground floor, then amenity floors
        g1 = 3.6 if two else H - 0.5                                              # top of the ground-floor glazing
        if eb == b:
            R.cut(cx - 1.3, cx + 1.3, 0.0, 2.9)
            frame_members(R, cx - 1.3, cx + 1.3, 0.0, 2.9, pd["frame"], mull=3, transom=0.82, fw=0.07, z=(0.05, 0.16))
            for hx in (cx - 0.1, cx + 0.06): R.box(hx, hx + 0.04, 0.9, 1.6, 0.16, 0.22, pd["metal"])
            R.box(cx - 2.6, cx + 2.6, 3.05, 3.3, 0, 1.18, pd["canopy"]); R.shade(cx - 2.6, cx + 2.6, 3.27, 3.3, 1.2)
            ny0, ny1 = 3.5, min(H - 0.4, 4.5)                                     # the name: a recess for a separate sign quad (podium_signs)
            R.box(cx - 2.6, cx + 2.6, ny0 - 0.06, ny1 + 0.06, 0, T + 0.04, pd["metal"]); R.cut(cx - 2.5, cx + 2.5, ny0, ny1)
            if two:                                                               # the double-height lobby window above the name
                R.cut(cx - 1.3, cx + 1.3, 4.8, H - 0.6); frame_members(R, cx - 1.3, cx + 1.3, 4.8, H - 0.6, pd["frame"], mull=2, transom=0.5, fw=0.06, z=(0.05, 0.14))
            if pd["planters"]:
                for px in (cx - 2.3, cx + 1.7):
                    R.box(px, px + 0.6, 0, 0.6, T, T + 0.65, C((0.3, 0.3, 0.32))); plant(R, rng, px + 0.3, 0.55, T + 0.05, T + 0.7, (0.0, 0.6), 7)
            continue
        if kind == "front" or (kind == "side" and b % 2 == 0):
            gx0, gx1 = x0 + 0.28, x0 + bw - 0.28
            R.box(x0, x0 + 0.28, 0, H - 0.3, 0, T + 0.12, pd["paint"] * 1.05); R.box(x0 + bw - 0.28, x0 + bw, 0, H - 0.3, 0, T + 0.12, pd["paint"] * 1.05)
            if kind == "front" and two:                                           # lobby: one tall window, double height
                R.cut(gx0, gx1, 0.25, H - 0.6)
                frame_members(R, gx0, gx1, 0.25, H - 0.6, pd["frame"], mull=1, transom=0.45, fw=0.06, z=(0.05, 0.14))
                R.box(gx0, gx1, 3.55, 3.65, 0.05, 0.16, pd["frame"])
            else:
                R.cut(gx0, gx1, 0.25, g1)
                frame_members(R, gx0, gx1, 0.25, g1, pd["frame"], mull=1, transom=0.72, fw=0.06, z=(0.05, 0.14))
                if two:                                                           # the amenity floor above: gym, lounge, co-working
                    R.cut(gx0, gx1, 4.6, H - 0.6)
                    frame_members(R, gx0, gx1, 4.6, H - 0.6, pd["frame"], mull=2, transom=None, fw=0.05, z=(0.04, 0.12))
                    R.box(gx0 - 0.1, gx1 + 0.1, 4.45, 4.6, 0, T + 0.08, pd["paint"] * 1.1)
            if kind == "side":                                                    # a shop: sign and awning
                sy0, sy1 = (g1 + 0.1, g1 + 0.7) if two else (g1 - 0.55, g1)
                R.box(gx0, gx1, sy0, sy1, 0.0, T + 0.02, C(ACCENTS[rng.integers(len(ACCENTS))]) * 0.8)   # a frame round
                R.cut(gx0 + 0.06, gx1 - 0.06, sy0 + 0.05, sy1 - 0.05)                                     # a blank sign quad
                if pd["shop_awning"]:
                    ay = g1 - (0.05 if two else 0.6)
                    R.sloped(gx0, gx1, ay, ay - 0.8, T, 1.15, 0.025, C(ACCENTS[rng.integers(len(ACCENTS))]), (0.94, 0.93, 0.9) if rng.random() < 0.6 else None)
            elif pd["planters"] and rng.random() < 0.4:
                R.box(gx0 + 0.2, gx1 - 0.2, 0, 0.5, T, T + 0.55, C((0.3, 0.3, 0.32))); plant(R, rng, (gx0 + gx1) / 2, 0.45, T + 0.05, T + 0.6, (0.0, 0.5), 9)
        else:                                                                        # service wall: louvres, a door, a high window
            k = rng.integers(3)
            if k == 0:
                R.box(cx - 0.6, cx + 0.6, 2.4, 3.4, 0, T - 0.05, C((0.4, 0.4, 0.42)))
                for yy in np.arange(2.45, 3.4, 0.08): R.box(cx - 0.6, cx + 0.6, yy, yy + 0.03, 0, T + 0.02, (0.5, 0.5, 0.52))
            elif k == 1:
                R.box(cx - 0.5, cx + 0.5, 0, 2.2, 0, T - 0.08, C((0.35, 0.38, 0.42))); R.box(cx + 0.3, cx + 0.38, 1.0, 1.06, 0, T, (0.7, 0.7, 0.7))
            else:
                R.cut(x0 + 0.4, x0 + bw - 0.4, 2.9, 3.7); frame_members(R, x0 + 0.4, x0 + bw - 0.4, 2.9, 3.7, pd["frame"], mull=2, fw=0.05)
            if two:                                                               # upper floors at the back: a row of windows
                R.cut(x0 + 0.5, x0 + bw - 0.5, 5.0, H - 0.9); frame_members(R, x0 + 0.5, x0 + bw - 0.5, 5.0, H - 0.9, pd["frame"], mull=1, fw=0.05)
    return R.encode(seed)
