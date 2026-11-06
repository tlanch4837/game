"""Industrial-zone factory generator: generic factories for every lot from 2x2 to 6x6, CS2-style 5 levels.

Low poly, ultra high detail: walls, roofs and props are a handful of quads each; the detail lives in a procedural
1024 px atlas (steel multi-pane windows, roll-up and dock doors, louvres, fan grilles, north-light glazing, tank
plate with weld seams, ladders, hazard stripes, trucks, containers, pallets, solar cells) and in 512 px tileable wall
sheets with strong normal maps (corrugated steel, factory brick, insulated panel, tilt-up concrete, composite panel).
Cheap geometric relief carries the silhouette at city-builder distance: gables, sawtooth roofs, chimneys, silos,
tank farms, pipe racks, stacks, dock canopies, rooftop units, solar rows.

Styles by level (same lot, bigger and cleaner plant):
  L1 workshop   corrugated steel shed, low gable to the street, roll-up doors, lean-to office, pallet yard, flue
  L2 brickworks red brick hall with pilasters + tall steel windows, sawtooth north-light roof, brick chimney, water tank
  L3 plant      insulated-panel hall, 2-floor office front, loading docks with canopy + trucks, silos, roof fans
  L4 process    tilt-up concrete hall with clerestory, tank farm in a bund, pipe rack, steel stack, conveyor gallery
  L5 hi-tech    composite-panel hall with glass band, glazed office cube with fins, solar roof, landscaped car park

Brand materials brand1 (sign bands, office accents) and brand2 (fins, skip bins, door frames) are white x
baseColorFactor, tintable per business, same as the office/commercial sets.

Run:  python gen_industry.py [level ...] [--all-variants]
      default: one model per lot, each lot size a different design (unique_pick); --all-variants: 4 per lot into industry_variants_backup/
Output: assets/buildings/industry/glb/IN_<W>x<D>_L<level>.glb + catalog.json + preview.html.  m, +Y up, front +Z.
"""
import json, math, os, sys
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import gen_low_density as g
import gen_low_commercial as c
import gen_low_office as o
from gen_low_density import Mesh, tile_maps, normal_from_height, jpg, png, CELL

ROOT = g.ROOT
BASE = os.path.join(ROOT, "assets", "buildings", "industry")
OUT, TEX = (os.path.join(BASE, d) for d in ("glb", "tex"))
RT = os.path.join(ROOT, "assets", "roads", "tex")

LOTS = [(2, 2), (2, 3), (2, 4), (3, 3), (3, 4), (3, 5), (4, 4), (4, 5), (4, 6), (5, 5), (5, 6), (6, 6)]
MATS = ("wall", "wall2", "trim", "roof", "detail", "ground", "paving", "grass", "brand1", "brand2")
SIDEWALK = 2.0

LEVELS = {
    1: dict(style="workshop", wall="corrugated", wall_c=(150, 162, 158), wall_m=2.0, wall2="corrugated", wall2_c=(196, 190, 172),
            roof="corr_roof", roof_c=(160, 160, 156), trim=(178, 176, 170), frame=(70, 74, 76), glass=(52, 62, 66),
            brand=(200, 120, 30), brand2=(40, 90, 150), workers=6, pollution=3),
    2: dict(style="brickworks", wall="brick", brick=((150, 70, 50), (176, 168, 154), 0.12, 0.10), wall_m=2.0,
            wall2="panel", wall2_c=(200, 196, 186), roof="corr_roof", roof_c=(92, 96, 100), trim=(196, 186, 166),
            frame=(46, 56, 52), glass=(58, 70, 72), brand=(30, 60, 110), brand2=(150, 40, 36), workers=8, pollution=4),
    3: dict(style="plant", wall="panel", wall_c=(208, 204, 192), wall_m=4.0, wall2="panel", wall2_c=(70, 88, 110),
            roof="gravel", trim=(150, 152, 150), frame=(60, 66, 72), glass=(40, 56, 70),
            brand=(30, 80, 140), brand2=(220, 170, 30), workers=10, pollution=3),
    4: dict(style="process", wall="tiltup", wall_c=(176, 172, 162), wall_m=6.0, wall2="panel", wall2_c=(120, 128, 132),
            roof="gravel", trim=(136, 136, 132), frame=(50, 56, 60), glass=(44, 58, 68),
            brand=(170, 40, 30), brand2=(230, 190, 40), workers=12, pollution=5),
    5: dict(style="hitech", wall="composite", wall_c=(226, 228, 228), wall_m=4.0, wall2="composite", wall2_c=(58, 62, 68),
            roof="membrane", trim=(200, 202, 204), frame=(36, 38, 42), glass=(46, 66, 84),
            brand=(20, 120, 110), brand2=(36, 40, 46), workers=14, pollution=1),
}

# ------------------------------------------------------------------ variants
# Every lot x level gets VARIANTS models. Each variant has its own palette (wall, second wall, roof, brand colours),
# odd variants are mirrored left-right, and a seeded RNG drives shape knobs: hall height, yard depth, roof pitch,
# chimney/stack/silo placement and count, roof kit, yard props.
VARIANTS = 4
PALETTES = {  # per level: list of overrides, one per variant (v0 = the base look above)
    1: [{}, dict(wall_c=(176, 120, 92), wall2_c=(214, 210, 198), roof_c=(120, 124, 126), brand=(170, 40, 40), brand2=(60, 60, 64)),
        dict(wall_c=(196, 198, 194), wall2_c=(150, 170, 150), roof_c=(176, 100, 70), brand=(30, 110, 60), brand2=(220, 170, 30)),
        dict(wall_c=(92, 112, 140), wall2_c=(200, 196, 180), roof_c=(150, 152, 150), brand=(230, 230, 226), brand2=(200, 80, 30))],
    2: [{}, dict(brick=((176, 98, 66), (200, 190, 170), 0.10, 0.04), roof_c=(70, 76, 82), brand=(20, 20, 24), brand2=(170, 140, 60)),
        dict(brick=((112, 52, 42), (150, 144, 136), 0.12, 0.20), roof_c=(110, 92, 80), brand=(120, 30, 30), brand2=(40, 80, 60)),
        dict(brick=((186, 150, 110), (210, 204, 190), 0.08, 0.02), roof_c=(96, 104, 98), brand=(30, 80, 70), brand2=(30, 50, 100))],
    3: [{}, dict(wall_c=(176, 186, 190), wall2_c=(40, 44, 50), brand=(190, 40, 40), brand2=(40, 40, 44)),
        dict(wall_c=(214, 200, 168), wall2_c=(120, 60, 40), brand=(40, 110, 60), brand2=(230, 120, 30)),
        dict(wall_c=(150, 168, 156), wall2_c=(220, 220, 214), brand=(240, 170, 20), brand2=(30, 60, 110))],
    4: [{}, dict(wall_c=(150, 146, 138), wall2_c=(170, 60, 40), brand=(30, 70, 130), brand2=(230, 230, 226)),
        dict(wall_c=(196, 186, 168), wall2_c=(70, 90, 80), brand=(40, 120, 70), brand2=(240, 200, 30)),
        dict(wall_c=(162, 168, 172), wall2_c=(40, 60, 100), brand=(220, 110, 20), brand2=(60, 60, 64))],
    5: [{}, dict(wall_c=(60, 64, 70), wall2_c=(230, 232, 232), brand=(200, 50, 50), brand2=(220, 222, 224)),
        dict(wall_c=(214, 216, 210), wall2_c=(150, 110, 70), brand=(40, 90, 170), brand2=(150, 110, 70)),
        dict(wall_c=(196, 210, 214), wall2_c=(30, 70, 90), brand=(120, 180, 40), brand2=(30, 70, 90))],
}


class _V:  # the current variant's knobs; set by set_variant() before each build
    def __init__(self, seed=0):
        r = self.rng = np.random.default_rng(seed)
        self.v = seed % VARIANTS
        self.hk = float(r.uniform(0.9, 1.18))        # hall height factor
        self.yk = float(r.uniform(0.85, 1.2))        # yard / apron depth factor
        self.pitch = float(r.uniform(0.15, 0.34))    # L1 roof pitch
        self.saw = float(r.uniform(5.0, 7.2))        # L2 sawtooth pitch
        self.a = r.random(); self.b = r.random(); self.c = r.random()   # free toggles per builder
        self.n = int(r.integers(0, 3))               # small counts (silos, tanks, docks...)
        self.cont = "cont_a" if r.random() < 0.5 else "cont_b"


V = _V(0)


def set_variant(W, D, level, v):
    global V
    V = _V(1000 * level + 37 * W + 101 * D + 7919 * v); V.v = v


def mirror(m):
    """Flip the model left-right (x -> -x) keeping faces outward: reverse each polygon's winding."""
    for name, items in m.prims.items():
        out = []
        for it in items:
            pts, n, uvs, cnt = it[:4]
            pts = [np.array((-q[0], q[1], q[2]), np.float32) for q in pts][::-1]
            out.append((pts, np.array((-n[0], n[1], n[2]), np.float32), list(uvs)[::-1], cnt) + tuple(it[4:]))
        m.prims[name] = out


# ------------------------------------------------------------------ detail atlas (1024, 8x8 grid of 128 px cells)
S = 1024
A = lambda i, j, w=1, h=1: (i * 128, j * 128, (i + w) * 128, (j + h) * 128)
ATLAS = {
    "win": A(0, 0), "win_tall": A(1, 0, 1, 2), "ribbon": A(2, 0, 2, 1), "rollup": A(4, 0, 2, 2), "dock": A(6, 0, 2, 2),
    "pdoor": A(0, 1), "louvre": A(2, 1), "fan": A(3, 1),
    "sawglass": A(0, 2, 4, 1), "hazard": A(4, 2), "pipe": A(5, 2), "tank": A(6, 2), "tank_top": A(7, 2),
    "glass": A(0, 3, 2, 1), "solar": A(2, 3), "skylight": A(3, 3), "paint": A(4, 3), "yellow": A(5, 3), "leaf": A(6, 3), "bark": A(7, 3),
    "cont_a": A(0, 4, 2, 1), "cont_b": A(2, 4, 2, 1), "cont_end": A(4, 4), "pallet": A(5, 4), "metal": A(6, 4), "soot": A(7, 4),
    "trailer": A(0, 5, 3, 1), "trailer_end": A(3, 5), "cab_side": A(4, 5), "cab_front": A(5, 5), "tyre": A(6, 5), "car": A(7, 5),
    "ladder": A(0, 6), "grate": A(1, 6), "rust": A(2, 6), "concrete": A(3, 6), "gdoor": A(4, 6), "car_top": A(5, 6),
    "rtu": A(6, 6), "warn": A(7, 6),
    "skip": A(0, 7), "sheet_gable": A(1, 7), "conveyor": A(2, 7, 2, 1), "water": A(4, 7), "curtain": A(5, 7), "dark": A(6, 7), "lamp": A(7, 7),
}
SIGN = (0, 256, 512, 512)  # brand sheet (512 px): blank raised sign panel


def auv(k, u, v, inset=0.01):
    x0, y0, x1, y1 = ATLAS[k]
    return ((x0 + (x1 - x0) * (inset + u * (1 - 2 * inset))) / S, (y0 + (y1 - y0) * (inset + (1 - v) * (1 - 2 * inset))) / S)


def FULL(k):
    if k == "sign":
        x0, y0, x1, y1 = SIGN
        return [(x0 / 512, y1 / 512), (x1 / 512, y1 / 512), (x1 / 512, y0 / 512), (x0 / 512, y0 / 512)]
    return [auv(k, 0, 0), auv(k, 1, 0), auv(k, 1, 1), auv(k, 0, 1)]


def shade(cc, k):
    return tuple(int(np.clip(v * k, 0, 255)) for v in cc)


def detail_sheet(p):
    fr, gl = p["frame"], p["glass"]
    lite, dark = shade(gl, 1.55), shade(gl, 0.7)
    col = Image.new("RGB", (S, S), (128, 128, 128)); hgt = Image.new("L", (S, S), 128)
    dc, dh = ImageDraw.Draw(col), ImageDraw.Draw(hgt)
    rng = np.random.default_rng(17)

    def R(bx, cc, hh=128):
        dc.rectangle(bx, fill=cc); dh.rectangle(bx, fill=hh)

    def glass(x0, y0, x1, y1, refl=True):
        for y in range(int(y0), int(y1) + 1):
            t = (y - y0) / max(1, y1 - y0)
            dc.line([x0, y, x1, y], fill=tuple(int(lite[k] * (1 - t) + dark[k] * t) for k in range(3)))
        dh.rectangle([x0, y0, x1, y1], fill=40)
        if refl and x1 - x0 > 6:
            w = x1 - x0
            dc.polygon([(x0 + w * 0.55, y0), (x0 + w * 0.72, y0), (x0 + w * 0.38, y1), (x0 + w * 0.21, y1)], fill=shade(lite, 1.12))

    def panes(x0, y0, x1, y1, nx, ny, bar=3, frame=6, fc=fr, opener=None):
        """Steel window: frame, nx x ny glazing bars, optional hopper opener (i, j) shown darker and tilted."""
        R([x0, y0, x1, y1], fc, 215)
        iw = (x1 - x0 - 2 * frame - (nx - 1) * bar) / nx; ih = (y1 - y0 - 2 * frame - (ny - 1) * bar) / ny
        for i in range(nx):
            for j in range(ny):
                a = x0 + frame + i * (iw + bar); b = y0 + frame + j * (ih + bar)
                glass(a, b, a + iw, b + ih, refl=(i + j) % 3 == 0)
                if opener and (i, j) in opener:
                    dc.rectangle([a, b, a + iw, b + ih * 0.35], fill=shade(dark, 0.6)); dh.rectangle([a, b, a + iw, b + ih], fill=70)

    def grime(box, strength=0.25, streaks=12):
        """Rain streaks + darker base on a cell (applied to the colour only)."""
        x0, y0, x1, y1 = box
        for _ in range(streaks):
            sx = rng.integers(x0, x1); L = rng.integers((y1 - y0) // 4, y1 - y0)
            for y in range(y0, y0 + L):
                px = col.getpixel((int(sx), int(y))); k = 1 - strength * (1 - (y - y0) / L) * 0.6
                col.putpixel((int(sx), int(y)), shade(px, k))

    # -- win: 4x4 steel factory window with two hopper openers
    panes(*ATLAS["win"], 4, 4, bar=4, frame=8, opener={(1, 1), (2, 1)})
    # -- win_tall: 4 x 9 tall steel window with a stone sill painted at the bottom
    x0, y0, x1, y1 = ATLAS["win_tall"]
    panes(x0, y0, x1, y1 - 10, 4, 9, bar=3, frame=7, opener={(1, 3), (2, 3), (1, 6), (2, 6)})
    R([x0, y1 - 10, x1, y1], shade(p["trim"], 1.0), 200)
    # -- ribbon: continuous strip, mullions every 32 px, transom bar
    x0, y0, x1, y1 = ATLAS["ribbon"]
    R([x0, y0, x1, y1], fr, 215)
    for i in range(8):
        glass(x0 + 4 + i * 32, y0 + 8, x0 + 30 + i * 32, y0 + 82); glass(x0 + 4 + i * 32, y0 + 88, x0 + 30 + i * 32, y1 - 8, refl=False)
    # -- rollup: slatted steel roll-up door in a painted steel frame, guide rails, hood, vision panel, hazard kick strip
    x0, y0, x1, y1 = ATLAS["rollup"]
    R([x0, y0, x1, y1], shade(p["frame"], 1.2), 220)
    R([x0 + 4, y0 + 4, x1 - 4, y0 + 26], shade(p["frame"], 0.8), 240)                               # roll hood
    R([x0 + 14, y0 + 26, x1 - 14, y1], (176, 180, 182), 150)                                             # curtain
    for k in range(int((y1 - y0 - 26) / 6)):
        yy = y0 + 28 + k * 6
        R([x0 + 14, yy, x1 - 14, yy + 1], (132, 136, 140), 120); R([x0 + 14, yy + 2, x1 - 14, yy + 3], (198, 200, 202), 175)
    for k in range(6): glass(x0 + 34 + k * 32, y0 + 120, x0 + 56 + k * 32, y0 + 128, refl=False)          # vision slots
    for k in range(10):
        R([x0 + 14 + k * 23, y1 - 14, x0 + 26 + k * 23, y1 - 2], (230, 190, 30), 160)
        R([x0 + 26 + k * 23, y1 - 14, x0 + 37 + k * 23, y1 - 2], (30, 30, 30), 160)
    R([x0 + 4, y0 + 26, x0 + 14, y1], shade(p["frame"], 0.7), 235); R([x1 - 14, y0 + 26, x1 - 4, y1], shade(p["frame"], 0.7), 235)
    grime((x0 + 14, y0 + 26, x1 - 14, y1), 0.3, 30)
    # -- dock: insulated sectional dock door, black dock seal, rubber bumpers, dock light, number plate
    x0, y0, x1, y1 = ATLAS["dock"]
    R([x0, y0, x1, y1], (34, 34, 36), 230)                                                              # seal / shelter
    R([x0 + 22, y0 + 26, x1 - 22, y1 - 34], (214, 216, 214), 160)
    for k in range(5):
        yy = y0 + 26 + k * (y1 - y0 - 60) / 5
        R([x0 + 22, yy, x1 - 22, yy + 2], (150, 152, 152), 120)
    glass(x0 + 50, y0 + 26 + (y1 - y0 - 60) * 0.42, x1 - 50, y0 + 26 + (y1 - y0 - 60) * 0.42 + 14, refl=False)
    R([x0 + 22, y1 - 34, x1 - 22, y1], (90, 90, 88), 190)                                              # leveller lip
    for xx in (x0 + 28, x1 - 52):
        R([xx, y1 - 30, xx + 24, y1 - 6], (20, 20, 20), 250)                                           # bumpers
    R([x0 + 4, y0 + 4, x0 + 18, y0 + 18], (250, 230, 160), 250)                                         # dock light
    R([x1 - 40, y0 + 6, x1 - 8, y0 + 22], (240, 240, 236), 200); R([x1 - 30, y0 + 9, x1 - 18, y0 + 19], (20, 20, 20), 210)
    # -- pdoor: steel personnel door with kick plate, push bar, exit sign above
    x0, y0, x1, y1 = ATLAS["pdoor"]
    R([x0, y0, x1, y1], shade(p["trim"], 0.9), 200)
    R([x0 + 30, y0 + 22, x1 - 30, y1], shade(p["brand2"], 0.9) if p["style"] != "hitech" else (70, 76, 82), 170)
    R([x0 + 30, y0 + 22, x1 - 30, y0 + 25], (40, 40, 40), 140)
    glass(x0 + 50, y0 + 34, x1 - 50, y0 + 60, refl=False)
    R([x0 + 36, y0 + 80, x1 - 36, y0 + 84], (200, 200, 200), 250); R([x0 + 30, y1 - 16, x1 - 30, y1], (150, 150, 150), 180)
    R([x0 + 48, y0 + 4, x1 - 48, y0 + 16], (30, 160, 70), 220)
    # -- louvre: deep blade louvre in a frame
    x0, y0, x1, y1 = ATLAS["louvre"]
    R([x0, y0, x1, y1], (100, 104, 106), 210)
    for k in range(12):
        yy = y0 + 6 + k * 10
        R([x0 + 6, yy, x1 - 6, yy + 4], (156, 160, 162), 240); R([x0 + 6, yy + 4, x1 - 6, yy + 9], (44, 46, 48), 50)
    # -- fan: round extract fan grille (top of roof fans) with hub
    x0, y0, x1, y1 = ATLAS["fan"]; cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    R([x0, y0, x1, y1], (150, 154, 156), 200)
    dc.ellipse([x0 + 6, y0 + 6, x1 - 6, y1 - 6], fill=(40, 42, 44)); dh.ellipse([x0 + 6, y0 + 6, x1 - 6, y1 - 6], fill=40)
    for r in range(56, 8, -8):
        dc.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(170, 172, 174), width=2); dh.ellipse([cx - r, cy - r, cx + r, cy + r], outline=230, width=2)
    for a in range(0, 360, 30):
        dc.line([cx, cy, cx + 56 * math.cos(math.radians(a)), cy + 56 * math.sin(math.radians(a))], fill=(170, 172, 174), width=2)
    dc.ellipse([cx - 9, cy - 9, cx + 9, cy + 9], fill=(90, 92, 94)); dh.ellipse([cx - 9, cy - 9, cx + 9, cy + 9], fill=250)
    # -- sawglass: north-light glazing, wired glass panes with heavy bars
    x0, y0, x1, y1 = ATLAS["sawglass"]
    panes(x0, y0, x1, y1, 16, 2, bar=5, frame=6, opener={(3, 0), (7, 0), (11, 0)})
    # -- hazard stripes
    x0, y0, x1, y1 = ATLAS["hazard"]
    R([x0, y0, x1, y1], (232, 186, 24), 180)
    for k in range(-4, 8):
        dc.polygon([(x0 + k * 32, y1), (x0 + k * 32 + 16, y1), (x0 + k * 32 + 16 + 128, y0), (x0 + k * 32 + 128, y0)], fill=(26, 26, 26))
    dc.rectangle([x1, y0, x1 + 8, y1], fill=(128, 128, 128))  # keep the neighbour clean (polygon overflow)
    # -- pipe: cylindrical shading across u (galvanised), flange bands
    x0, y0, x1, y1 = ATLAS["pipe"]
    for x in range(x0, x1):
        t = (x - x0) / (x1 - x0); k = 0.62 + 0.45 * math.sin(t * math.pi * 2) ** 2
        dc.line([x, y0, x, y1], fill=shade((176, 180, 182), k))
    for yy in (y0 + 20, y0 + 84):
        R([x0, yy, x1, yy + 6], (120, 124, 126), 240)
    # -- tank: painted steel plate, horizontal + staggered vertical weld seams, rust weeps, ladder-less
    x0, y0, x1, y1 = ATLAS["tank"]
    tc = shade(p["trim"], 1.25)
    R([x0, y0, x1, y1], tc, 150)
    for r in range(4):
        yy = y0 + r * 32
        R([x0, yy, x1, yy + 1], shade(tc, 0.8), 200)
        for k in range(4):
            xx = x0 + ((k * 32 + (16 if r % 2 else 0)) % 128)
            R([xx, yy, xx + 1, yy + 32], shade(tc, 0.85), 190)
    grime((x0, y0, x1, y1), 0.35, 26)
    # -- tank_top: radial roof plates + manway + vent
    x0, y0, x1, y1 = ATLAS["tank_top"]; cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    R([x0, y0, x1, y1], shade(tc, 0.92), 160)
    for a in range(0, 360, 30):
        dc.line([cx, cy, cx + 70 * math.cos(math.radians(a)), cy + 70 * math.sin(math.radians(a))], fill=shade(tc, 0.75), width=1)
    dc.ellipse([cx - 10, cy - 10, cx + 10, cy + 10], fill=(80, 84, 86)); dh.ellipse([cx - 10, cy - 10, cx + 10, cy + 10], fill=240)
    R([cx + 20, cy - 6, cx + 34, cy + 6], (90, 90, 90), 230)
    # -- glass: office curtain wall 2 x 3 lites, slim mullions
    x0, y0, x1, y1 = ATLAS["glass"]
    R([x0, y0, x1, y1], fr, 220)
    for i in range(4):
        for j in range(2):
            glass(x0 + 3 + i * 64, y0 + 3 + j * 64, x0 + 61 + i * 64, y0 + 61 + j * 64)
    # -- solar: 6 x 10 cells, silver frame, busbars
    x0, y0, x1, y1 = ATLAS["solar"]
    R([x0, y0, x1, y1], (190, 194, 198), 210)
    for i in range(6):
        for j in range(10):
            a, b = x0 + 5 + i * 20, y0 + 5 + j * 12
            R([a, b, a + 18, b + 10], (22, 34, 66) if (i + j) % 2 else (26, 40, 76), 80)
            R([a + 8, b, a + 9, b + 10], (60, 70, 100), 90)
    # -- skylight: polycarbonate rooflight strip with ribs
    x0, y0, x1, y1 = ATLAS["skylight"]
    R([x0, y0, x1, y1], (180, 184, 186), 200)
    for k in range(10):
        R([x0 + 6 + k * 12, y0 + 6, x0 + 14 + k * 12, y1 - 6], (214, 226, 230), 150)
    # -- paint (white line), yellow line
    R(ATLAS["paint"], (236, 234, 226), 140); R(ATLAS["yellow"], (232, 196, 40), 140)
    # -- leaf, bark
    x0, y0, x1, y1 = ATLAS["leaf"]
    R([x0, y0, x1, y1], (62, 98, 48), 140)
    for _ in range(320):
        x, y = rng.integers(x0, x1), rng.integers(y0, y1); r = int(rng.integers(2, 7)); s = int(rng.integers(-26, 30))
        cc = (max(0, 62 + s), max(0, 98 + s), max(0, 48 + s // 2)); dc.ellipse([x - r, y - r, x + r, y + r], fill=cc); dh.ellipse([x - r, y - r, x + r, y + r], fill=int(128 + s * 2))
    x0, y0, x1, y1 = ATLAS["bark"]
    R([x0, y0, x1, y1], (92, 72, 56), 130)
    for k in range(14): R([x0 + k * 9, y0, x0 + 3 + k * 9, y1], (70, 54, 42), 100)
    # -- containers: corrugated sides (red / blue), corner castings, door end with lock rods
    for key, cc in (("cont_a", (160, 52, 38)), ("cont_b", (40, 86, 140))):
        x0, y0, x1, y1 = ATLAS[key]
        R([x0, y0, x1, y1], cc, 150)
        for k in range(0, x1 - x0, 8):
            R([x0 + k, y0 + 8, x0 + k + 3, y1 - 8], shade(cc, 0.78), 110); R([x0 + k + 4, y0 + 8, x0 + k + 7, y1 - 8], shade(cc, 1.12), 190)
        R([x0, y0, x1, y0 + 8], shade(cc, 0.7), 220); R([x0, y1 - 8, x1, y1], shade(cc, 0.7), 220)
        for xx in (x0, x1 - 10):
            R([xx, y0, xx + 10, y0 + 10], (60, 60, 60), 240); R([xx, y1 - 10, xx + 10, y1], (60, 60, 60), 240)
        grime((x0, y0, x1, y1), 0.3, 20)
    x0, y0, x1, y1 = ATLAS["cont_end"]
    R([x0, y0, x1, y1], (150, 140, 120), 150)
    R([x0 + 63, y0 + 4, x0 + 65, y1 - 4], (60, 56, 50), 80)
    for xx in (x0 + 24, x0 + 44, x0 + 84, x0 + 104):
        R([xx, y0 + 6, xx + 3, y1 - 6], (200, 196, 186), 240)
    # -- pallet stack: wrapped goods on a timber pallet
    x0, y0, x1, y1 = ATLAS["pallet"]
    R([x0, y0, x1, y1 - 20], (186, 160, 112), 170)
    for k in range(4): R([x0, y0 + k * 27, x1, y0 + k * 27 + 2], (150, 126, 86), 120)
    for k in range(3): R([x0 + 6 + k * 42, y0 + 40, x0 + 30 + k * 42, y0 + 56], (240, 240, 236), 180)
    R([x0, y1 - 20, x1, y1], (150, 112, 70), 180)
    for k in range(3): R([x0 + 10 + k * 46, y1 - 14, x0 + 34 + k * 46, y1 - 2], (40, 30, 22), 60)
    # -- metal, soot, rust, concrete, grate, dark
    R(ATLAS["metal"], (150, 154, 158), 200); R([ATLAS["metal"][0], ATLAS["metal"][1] + 64, ATLAS["metal"][2], ATLAS["metal"][1] + 66], (110, 112, 116), 160)
    x0, y0, x1, y1 = ATLAS["soot"]
    for y in range(y0, y1):
        t = (y - y0) / (y1 - y0); dc.line([x0, y, x1, y], fill=shade((60, 58, 56), 0.4 + 0.6 * t))
    x0, y0, x1, y1 = ATLAS["rust"]
    R([x0, y0, x1, y1], (128, 74, 44), 140)
    for _ in range(400):
        x, y = rng.integers(x0, x1), rng.integers(y0, y1); r = int(rng.integers(1, 4))
        cc = shade((128, 74, 44), float(rng.uniform(0.6, 1.3))); dc.ellipse([x - r, y - r, x + r, y + r], fill=cc)
    x0, y0, x1, y1 = ATLAS["concrete"]
    a = np.asarray(col).copy(); a[y0:y1, x0:x1] = np.clip(168 + rng.normal(0, 8, (y1 - y0, x1 - x0, 1)), 0, 255).astype(np.uint8); col = Image.fromarray(a); dc = ImageDraw.Draw(col)
    x0, y0, x1, y1 = ATLAS["grate"]
    R([x0, y0, x1, y1], (36, 38, 40), 60)
    for k in range(0, 128, 8): R([x0 + k, y0, x0 + k + 2, y1], (120, 124, 126), 220); R([x0, y0 + k, x1, y0 + k + 1], (100, 104, 106), 200)
    R(ATLAS["dark"], (30, 32, 34), 80)
    # -- trailer: box trailer side with brand-neutral stripe, chassis, two axle tandem
    x0, y0, x1, y1 = ATLAS["trailer"]
    R([x0, y0, x1, y1 - 34], (232, 232, 228), 160)
    for k in range(0, x1 - x0, 24): R([x0 + k, y0, x0 + k + 2, y1 - 34], (200, 200, 196), 200)
    R([x0, y1 - 54, x1, y1 - 46], (40, 90, 150), 180)
    R([x0, y1 - 34, x1, y1 - 26], (40, 40, 42), 200)
    R([x0, y1 - 26, x1, y1], (128, 128, 128), 128)                                                     # sky between wheels -> neutral
    for xx in (x0 + 270, x0 + 316):
        dc.ellipse([xx, y1 - 34, xx + 34, y1], fill=(24, 24, 24)); dh.ellipse([xx, y1 - 34, xx + 34, y1], fill=200)
        dc.ellipse([xx + 11, y1 - 23, xx + 23, y1 - 11], fill=(150, 150, 150))
    x0, y0, x1, y1 = ATLAS["trailer_end"]
    R([x0, y0, x1, y1], (220, 220, 216), 160); R([x0 + 63, y0 + 4, x0 + 65, y1 - 30], (120, 120, 120), 90)
    for xx in (x0 + 30, x0 + 96): R([xx, y0 + 8, xx + 3, y1 - 30], (150, 150, 150), 240)
    R([x0, y1 - 30, x1, y1 - 18], (40, 40, 42), 220)
    for xx in (x0 + 6, x1 - 22): R([xx, y1 - 28, xx + 16, y1 - 20], (200, 30, 30), 230)
    R([x0, y1 - 18, x1, y1], (24, 24, 24), 150)
    # -- cab: side (door, window, fuel tank, wheel) and front (windscreen, grille, lamps)
    cab = p["brand"]
    x0, y0, x1, y1 = ATLAS["cab_side"]
    R([x0, y0, x1, y1], cab, 170); glass(x0 + 20, y0 + 12, x0 + 100, y0 + 52, refl=True)
    R([x0 + 14, y0 + 8, x0 + 16, y1 - 30], shade(cab, 0.6), 100); R([x0 + 104, y0 + 8, x0 + 106, y1 - 30], shade(cab, 0.6), 100)
    R([x0 + 64, y0 + 74, x0 + 120, y0 + 92], (190, 190, 190), 220)
    dc.ellipse([x0 + 10, y1 - 40, x0 + 50, y1], fill=(24, 24, 24)); dc.ellipse([x0 + 22, y1 - 28, x0 + 38, y1 - 12], fill=(150, 150, 150))
    x0, y0, x1, y1 = ATLAS["cab_front"]
    R([x0, y0, x1, y1], cab, 170); glass(x0 + 8, y0 + 10, x1 - 8, y0 + 56)
    R([x0 + 24, y0 + 66, x1 - 24, y0 + 104], (60, 62, 64), 120)
    for k in range(6): R([x0 + 26, y0 + 68 + k * 6, x1 - 26, y0 + 70 + k * 6], (130, 132, 134), 220)
    for xx in (x0 + 6, x1 - 22): R([xx, y0 + 90, xx + 16, y0 + 102], (250, 246, 220), 250)
    R([x0, y1 - 18, x1, y1], (40, 40, 42), 220)
    R(ATLAS["tyre"], (26, 26, 26), 100)
    # -- car (side view smear for parked cars) + roof
    x0, y0, x1, y1 = ATLAS["car"]
    R([x0, y0, x1, y1], (128, 128, 128), 128); R([x0, y0 + 40, x1, y1 - 20], (120, 130, 150), 170)
    glass(x0 + 22, y0 + 46, x1 - 22, y0 + 70)
    for xx in (x0 + 14, x1 - 44): dc.ellipse([xx, y1 - 40, xx + 30, y1 - 10], fill=(24, 24, 24))
    x0, y0, x1, y1 = ATLAS["car_top"]
    R([x0, y0, x1, y1], (120, 130, 150), 170); glass(x0 + 20, y0 + 30, x1 - 20, y0 + 50, refl=False); glass(x0 + 20, y1 - 46, x1 - 20, y1 - 30, refl=False)
    # -- ladder: caged ladder over wall-coloured backdrop
    x0, y0, x1, y1 = ATLAS["ladder"]
    R([x0, y0, x1, y1], (128, 128, 128), 100)
    R([x0 + 40, y0, x0 + 46, y1], (210, 190, 40), 250); R([x1 - 46, y0, x1 - 40, y1], (210, 190, 40), 250)
    for k in range(0, 128, 14): R([x0 + 40, y0 + k, x1 - 40, y0 + k + 3], (210, 190, 40), 240)
    for k in range(0, 128, 32): R([x0 + 24, y0 + k, x1 - 24, y0 + k + 3], (170, 160, 60), 220)
    for xx in (x0 + 24, x1 - 27, x0 + 62): R([xx, y0, xx + 3, y1], (170, 160, 60), 220)
    # -- gdoor: glass office entry doors with steel frame
    x0, y0, x1, y1 = ATLAS["gdoor"]
    R([x0, y0, x1, y1], fr, 210); glass(x0 + 8, y0 + 8, x0 + 60, y1 - 4); glass(x0 + 68, y0 + 8, x1 - 8, y1 - 4)
    R([x0 + 48, y0 + 60, x0 + 54, y0 + 76], (210, 210, 210), 250); R([x0 + 74, y0 + 60, x0 + 80, y0 + 76], (210, 210, 210), 250)
    # -- rtu: rooftop unit side, condenser coil grid + access panels
    x0, y0, x1, y1 = ATLAS["rtu"]
    R([x0, y0, x1, y1], (196, 198, 196), 190)
    R([x0 + 6, y0 + 10, x0 + 70, y1 - 10], (70, 74, 76), 60)
    for k in range(0, 64, 4): R([x0 + 6 + k, y0 + 10, x0 + 7 + k, y1 - 10], (130, 134, 136), 180)
    for k in range(3): R([x0 + 78, y0 + 10 + k * 38, x1 - 6, y0 + 44 + k * 38], (206, 208, 206), 210); R([x1 - 16, y0 + 24 + k * 38, x1 - 10, y0 + 30 + k * 38], (80, 80, 80), 240)
    # -- warn: safety plate (yellow triangle on white)
    x0, y0, x1, y1 = ATLAS["warn"]
    R([x0, y0, x1, y1], (240, 240, 236), 200)
    dc.polygon([(x0 + 64, y0 + 14), (x1 - 16, y1 - 18), (x0 + 16, y1 - 18)], fill=(240, 200, 20), outline=(20, 20, 20))
    R([x0 + 61, y0 + 46, x0 + 67, y0 + 86], (20, 20, 20), 210); R([x0 + 61, y0 + 92, x0 + 67, y0 + 100], (20, 20, 20), 210)
    # -- skip bin side
    x0, y0, x1, y1 = ATLAS["skip"]
    R([x0, y0, x1, y1], (226, 226, 222), 170)
    for k in range(4): R([x0 + 14 + k * 30, y0 + 6, x0 + 20 + k * 30, y1 - 6], (196, 196, 192), 230)
    R([x0, y0, x1, y0 + 8], (180, 180, 176), 230)
    # -- sheet_gable: corrugated gable vent (triangular louvre reads as a box vent)
    x0, y0, x1, y1 = ATLAS["sheet_gable"]
    R([x0, y0, x1, y1], (90, 94, 96), 200)
    for k in range(9): R([x0 + 8, y0 + 8 + k * 13, x1 - 8, y0 + 13 + k * 13], (40, 42, 44), 60)
    # -- conveyor: lattice gallery side panels with window slots
    x0, y0, x1, y1 = ATLAS["conveyor"]
    R([x0, y0, x1, y1], (200, 200, 196), 170)
    for k in range(0, 256, 32):
        R([x0 + k, y0, x0 + k + 3, y1], (140, 140, 136), 230)
        glass(x0 + k + 8, y0 + 30, x0 + k + 26, y0 + 56, refl=False)
    R([x0, y1 - 10, x1, y1], (120, 120, 116), 230)
    # -- water (tank trough / pond), curtain (L5 frit band), lamp head
    R(ATLAS["water"], (52, 86, 104), 60)
    x0, y0, x1, y1 = ATLAS["curtain"]
    R([x0, y0, x1, y1], fr, 220)
    for i in range(2):
        for j in range(3):
            glass(x0 + 3 + i * 62, y0 + 3 + j * 41, x0 + 59 + i * 62, y0 + 38 + j * 41)
    R(ATLAS["lamp"], (240, 236, 210), 220)

    rgb = np.asarray(col).astype(np.float32)
    rgb = np.clip(rgb * (0.95 + 0.08 * np.random.default_rng(11).random((S, S, 1))), 0, 255).astype(np.uint8)
    h = np.asarray(hgt.filter(ImageFilter.GaussianBlur(1.0)), np.float32) / 255.0
    return jpg(rgb, 90), png(normal_from_height(h, 6.0))


# ------------------------------------------------------------------ tileable wall / roof sheets (512 px)
def _finish(rgb, h, k, rng, grime=0.0):
    if grime:  # rain-wash streaks from the top edge (tile wraps vertically, so streaks fade within the tile)
        Sx = rgb.shape[1]
        for _ in range(int(40 * grime)):
            x = int(rng.integers(0, Sx)); w = int(rng.integers(1, 5)); L = int(rng.integers(80, 400)); st = float(rng.uniform(0.08, 0.22)) * grime
            fall = (1 - np.linspace(0, 1, L))[:, None, None] * st
            rgb[:L, x:x + w] *= (1 - fall)
    rgb = np.clip(rgb, 0, 255).astype(np.uint8)
    return jpg(rgb), png(normal_from_height(np.asarray(Image.fromarray((np.clip(h, 0, 1) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8)), np.float32) / 255.0, k))


def corrugated(cc, seed=1, ribs=26, sheets=2, rust=0.4):
    """Vertical corrugated steel: sine ribs, sheet laps, fastener rows, rust weeps at fasteners."""
    Z = 512; rng = np.random.default_rng(seed)
    x = np.arange(Z) / Z
    prof = 0.5 + 0.5 * np.sin(x * ribs * 2 * np.pi)
    h = np.tile(prof, (Z, 1))
    shade_ = 0.82 + 0.3 * prof
    rgb = np.ones((Z, Z, 3), np.float32) * np.array(cc, np.float32) * shade_[None, :, None]
    rgb *= 0.94 + 0.08 * rng.random((Z // 16, Z // 16)).repeat(16, 0).repeat(16, 1)[..., None]
    for s in range(sheets):  # vertical laps
        lx = int(Z * s / sheets) + 3
        rgb[:, lx:lx + 3] *= 0.7; h[:, lx:lx + 3] = 1.0
    for fy in (int(Z * 0.08), int(Z * 0.58)):  # fastener rows + rust weeps
        for k in range(ribs):
            fx = int((k + 0.25) / ribs * Z)
            rgb[fy - 2:fy + 2, fx - 2:fx + 2] = (90, 92, 92); h[fy - 2:fy + 2, fx - 2:fx + 2] = 1.0
            if rng.random() < rust:
                L = int(rng.integers(20, 120)); a = np.linspace(0.35, 0, L)[:, None, None]
                rgb[fy:fy + L, fx - 1:fx + 2] = rgb[fy:fy + L, fx - 1:fx + 2] * (1 - a) + np.array((120, 70, 40), np.float32) * a
    return _finish(rgb, h, 5.0, rng, grime=0.6)


def panel_sheet(cc, seed=2, rows=4, ribs=8):
    """Insulated sandwich panel, horizontal: 4 panels per tile, shallow micro-ribs, recessed joints with fixings."""
    Z = 512; rng = np.random.default_rng(seed)
    y = np.arange(Z) / Z
    ph = (y * rows) % 1.0
    h = 0.6 + 0.08 * np.sin(ph * ribs * 2 * np.pi)
    h = np.tile(h[:, None], (1, Z))
    rgb = np.ones((Z, Z, 3), np.float32) * np.array(cc, np.float32)
    rgb *= (0.96 + 0.06 * np.sin(ph * ribs * 2 * np.pi))[:, None, None]
    rgb *= 0.96 + 0.05 * rng.random((Z // 32, Z // 32)).repeat(32, 0).repeat(32, 1)[..., None]
    for r in range(rows):
        jy = int(r / rows * Z)
        rgb[jy:jy + 4] *= 0.6; h[jy:jy + 4] = 0.1
        for fx in range(16, Z, 64):
            rgb[jy + 6:jy + 9, fx:fx + 3] *= 0.7; h[jy + 6:jy + 9, fx:fx + 3] = 1.0
    rgb[:, :3] *= 0.75; h[:, :3] = 0.2  # vertical butt joint once per tile
    return _finish(rgb, h, 3.0, rng, grime=0.35)


def tiltup_sheet(cc, seed=3):
    """Tilt-up concrete: 6 m tile, two 3 m panels, recessed joints, form-tie holes, lifting-insert patches, stains."""
    Z = 512; rng = np.random.default_rng(seed)
    n = rng.random((Z // 8, Z // 8)).repeat(8, 0).repeat(8, 1)
    rgb = np.ones((Z, Z, 3), np.float32) * np.array(cc, np.float32) * (0.92 + 0.12 * n)[..., None]
    rgb *= 0.96 + 0.06 * rng.random((Z, Z, 1))
    h = 0.6 + 0.1 * n
    for jx in (0, Z // 2):
        rgb[:, jx:jx + 5] *= 0.62; h[:, jx:jx + 5] = 0.05
    rgb[Z // 3:Z // 3 + 3] *= 0.85; h[Z // 3:Z // 3 + 3] = 0.4  # reveal line
    for gx in range(32, Z, 64):
        for gy in range(40, Z, 85):
            rgb[gy:gy + 4, gx:gx + 4] *= 0.65; h[gy:gy + 4, gx:gx + 4] = 0.2
    for px in (Z // 4, 3 * Z // 4):
        rgb[24:44, px - 10:px + 10] *= 1.06
    return _finish(rgb, h, 2.5, rng, grime=0.9)


def composite_sheet(cc, seed=4):
    """Rainscreen composite cassettes 1.33 x 1 m, crisp open joints, faint sheen gradient per cassette."""
    Z = 512; rng = np.random.default_rng(seed)
    rgb = np.ones((Z, Z, 3), np.float32) * np.array(cc, np.float32); h = np.ones((Z, Z), np.float32) * 0.7
    cw, ch = Z // 3, Z // 4
    for i in range(3):
        for j in range(4):
            k = 0.97 + 0.05 * rng.random(); gy = np.linspace(1.02, 0.97, ch)[:, None, None]
            rgb[j * ch:(j + 1) * ch, i * cw:(i + 1) * cw] *= k * gy
    for i in range(4): rgb[:, min(Z - 1, i * cw):min(Z, i * cw + 4)] *= 0.35; h[:, min(Z - 1, i * cw):min(Z, i * cw + 4)] = 0.0
    for j in range(4): rgb[j * ch:j * ch + 4] *= 0.35; h[j * ch:j * ch + 4] = 0.0
    return _finish(rgb, h, 3.0, rng, grime=0.15)


def corr_roof(cc, seed=5):
    """Trapezoidal roof sheet: ribs along the slope (texture v), translucent rooflight sheets every few runs."""
    Z = 512; rng = np.random.default_rng(seed)
    x = np.arange(Z) / Z
    prof = np.clip(np.abs(((x * 12) % 1.0) - 0.5) * 4 - 0.6, 0, 1)
    h = np.tile(prof, (Z, 1))
    rgb = np.ones((Z, Z, 3), np.float32) * np.array(cc, np.float32) * (0.85 + 0.25 * prof)[None, :, None]
    rgb *= 0.92 + 0.1 * rng.random((Z // 32, Z // 32)).repeat(32, 0).repeat(32, 1)[..., None]
    lx0, lx1 = int(Z * 5 / 12), int(Z * 6 / 12)
    rgb[Z // 4:Z // 2, lx0:lx1] = rgb[Z // 4:Z // 2, lx0:lx1] * 0.4 + np.array((200, 214, 210), np.float32) * 0.6  # rooflight sheet
    rgb[Z // 2:Z // 2 + 3] *= 0.75; h[Z // 2:Z // 2 + 3] = 1.0  # end lap
    for k in range(12):
        lx = int((k + 0.5) / 12 * Z); rgb[Z // 2 + 8, lx - 1:lx + 2] = (80, 80, 80)
    return _finish(rgb, h, 4.0, rng, grime=0.2)


def membrane(cc=(186, 190, 192), seed=6):
    """Single-ply roof membrane: 2 m rolls with welded laps, slight ponding stains."""
    Z = 512; rng = np.random.default_rng(seed)
    rgb = np.ones((Z, Z, 3), np.float32) * np.array(cc, np.float32) * (0.95 + 0.05 * rng.random((Z, Z, 1)))
    blot = np.asarray(Image.fromarray((rng.random((16, 16)) * 255).astype(np.uint8)).resize((Z, Z), Image.BICUBIC), np.float32) / 255
    rgb *= (0.92 + 0.1 * blot)[..., None]
    h = np.ones((Z, Z), np.float32) * 0.5
    for jx in (0, Z // 2):
        rgb[:, jx:jx + 6] *= 0.88; h[:, jx:jx + 6] = 0.9
    return _finish(rgb, h, 2.0, rng)


def wall_maps(kind, p, which="wall"):
    cc = p.get(which + "_c", (180, 180, 180)); kd = p[which]
    if kd == "brick": return o.brick_sheet(*p["brick"][:3], seed=7, iron=p["brick"][3])
    if kd == "corrugated": return corrugated(cc, seed=hash(which) % 97)
    if kd == "panel": return panel_sheet(cc)
    if kd == "tiltup": return tiltup_sheet(cc)
    if kd == "composite": return composite_sheet(cc)
    raise ValueError(kd)


# ------------------------------------------------------------------ geometry helpers
def new_mesh():
    m = Mesh(); m.prims = {k: [] for k in MATS}; return m


def mbox(m, mat, x0, x1, y0, y1, z0, z1, k=None, skip=(), tm=2.0, side_k=None, top_k=None):
    """Box in any material. k = atlas cell on every face (side_k for the +-X faces, top_k for the top); else world-tiled."""
    def uvq(pts, axis, kk):
        if kk: return FULL(kk)
        return [(q[axis] / tm, -q[1] / tm) for q in pts]
    F = [("front", ((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)), 0, k),
         ("back", ((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)), 0, k),
         ("right", ((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)), 2, side_k or k),
         ("left", ((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)), 2, side_k or k)]
    for nm, pts, ax, kk in F:
        if nm not in skip: m.quad(mat, *pts, uvq(pts, ax, kk))
    if "top" not in skip:
        pts = ((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)); kk = top_k or k
        m.quad(mat, *pts, FULL(kk) if kk else [(q[0] / tm, q[2] / tm) for q in pts])
    if "bot" not in skip and y0 > 0.3:
        pts = ((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)); kk = top_k or k
        m.quad(mat, *pts, FULL(kk) if kk else [(q[0] / tm, q[2] / tm) for q in pts])


def face_z(m, k, x0, x1, y0, y1, z, f=+1, off=0.03, mat="detail"):
    zz = z + off * f
    pts = [(x0, y0, zz), (x1, y0, zz), (x1, y1, zz), (x0, y1, zz)] if f > 0 else [(x1, y0, zz), (x0, y0, zz), (x0, y1, zz), (x1, y1, zz)]
    m.quad(mat, *pts, FULL(k))


def face_x(m, k, z0, z1, y0, y1, x, f, off=0.03, mat="detail"):
    xx = x + off * f
    pts = [(xx, y0, z1), (xx, y0, z0), (xx, y1, z0), (xx, y1, z1)] if f > 0 else [(xx, y0, z0), (xx, y0, z1), (xx, y1, z1), (xx, y1, z0)]
    m.quad(mat, *pts, FULL(k))


def flat(m, mat, x0, x1, z0, z1, y, tile=4.0):
    c.flat(m, mat, x0, x1, z0, z1, y, tile)


def dflat(m, k, x0, x1, z0, z1, y):
    if x1 - x0 < 0.05 or z1 - z0 < 0.05: return
    m.quad("detail", (x0, y, z1), (x1, y, z1), (x1, y, z0), (x0, y, z0), FULL(k))


def oquad(m, mat, pts, uvs, inside):
    """Quad whose winding is fixed so its normal points away from `inside` (used for round things)."""
    P = [np.array(q, np.float32) for q in pts]
    n = np.cross(P[1] - P[0], P[3] - P[0])
    if np.dot(n, np.mean(P, 0) - np.array(inside, np.float32)) < 0:
        pts = [pts[0], pts[3], pts[2], pts[1]]; uvs = [uvs[0], uvs[3], uvs[2], uvs[1]]
    m.quad(mat, *pts, uvs)


def otri(m, mat, pts, uvs, inside):
    P = [np.array(q, np.float32) for q in pts]
    n = np.cross(P[1] - P[0], P[2] - P[0])
    if np.dot(n, np.mean(P, 0) - np.array(inside, np.float32)) < 0:
        pts = [pts[0], pts[2], pts[1]]; uvs = [uvs[0], uvs[2], uvs[1]]
    m.tri(mat, *pts, uvs)


def cyl(m, k, cx, cz, r, y0, y1, n=10, top="tank_top", cone=0.0, mat="detail", bottom=False):
    """Vertical n-gon prism (tank, silo, stack). Side wraps atlas cell k once around; top = flat cap or cone."""
    ring = [(cx + r * math.cos(2 * math.pi * i / n), cz - r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    mid = (cx, (y0 + y1) / 2, cz)
    for i in range(n):
        (ax, az), (bx, bz) = ring[i], ring[(i + 1) % n]
        u0, u1 = i / n, (i + 1) / n
        oquad(m, mat, [(ax, y0, az), (bx, y0, bz), (bx, y1, bz), (ax, y1, az)],
              [auv(k, u0, 0), auv(k, u1, 0), auv(k, u1, 1), auv(k, u0, 1)], (cx, mid[1], cz))
        if top:
            tu = lambda x, z: auv(top, 0.5 + (x - cx) / (2 * r), 0.5 + (z - cz) / (2 * r))
            ap = (cx, y1 + cone, cz)
            otri(m, mat, [(ax, y1, az), (bx, y1, bz), ap], [tu(ax, az), tu(bx, bz), (tu(cx, cz))], (cx, y1 - 1.0, cz))
        if bottom:
            otri(m, mat, [(ax, y0, az), (bx, y0, bz), (cx, y0, cz)], [auv("dark", 0, 0)] * 3, (cx, y0 + 1.0, cz))


def hcyl(m, k, x0, x1, cy, cz, r, n=8, cap="tank_top", mat="detail"):
    """Horizontal prism along X (pipe, horizontal tank)."""
    ring = [(cy + r * math.cos(2 * math.pi * i / n), cz + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    for i in range(n):
        (ay, az), (by, bz) = ring[i], ring[(i + 1) % n]
        u0, u1 = i / n, (i + 1) / n
        oquad(m, mat, [(x0, ay, az), (x1, ay, az), (x1, by, bz), (x0, by, bz)],
              [auv(k, u0, 0), auv(k, u0, 1), auv(k, u1, 1), auv(k, u1, 0)], ((x0 + x1) / 2, cy, cz))
        if cap:
            for xx, ins in ((x0, x0 + 1), (x1, x1 - 1)):
                tu = lambda yy, zz: auv(cap, 0.5 + (zz - cz) / (2 * r), 0.5 + (yy - cy) / (2 * r))
                otri(m, mat, [(xx, ay, az), (xx, by, bz), (xx, cy, cz)], [tu(ay, az), tu(by, bz), tu(cy, cz)], (ins, cy, cz))


def zcyl(m, k, z0, z1, cx, cy, r, n=8, mat="detail"):
    """Horizontal prism along Z (pipe runs front to back)."""
    ring = [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    for i in range(n):
        (ax, ay), (bx, by) = ring[i], ring[(i + 1) % n]
        u0, u1 = i / n, (i + 1) / n
        oquad(m, mat, [(ax, ay, z0), (ax, ay, z1), (bx, by, z1), (bx, by, z0)],
              [auv(k, u0, 0), auv(k, u0, 1), auv(k, u1, 1), auv(k, u1, 0)], (cx, cy, (z0 + z1) / 2))


def taper(m, mat, cx, cz, r0, r1, y0, y1, tm=2.0):
    """Square tapered shaft (brick chimney)."""
    B = [(cx - r0, cz + r0), (cx + r0, cz + r0), (cx + r0, cz - r0), (cx - r0, cz - r0)]
    T = [(cx - r1, cz + r1), (cx + r1, cz + r1), (cx + r1, cz - r1), (cx - r1, cz - r1)]
    for i in range(4):
        j = (i + 1) % 4
        pts = [(B[i][0], y0, B[i][1]), (B[j][0], y0, B[j][1]), (T[j][0], y1, T[j][1]), (T[i][0], y1, T[i][1])]
        ax = 0 if i % 2 == 0 else 2
        m.quad(mat, *pts, [(q[ax] / tm * (1 if i < 2 else -1), -q[1] / tm) for q in pts])


def gable_z(m, x0, x1, z0, z1, H, rise, o_=0.35, roof="roof", wall="wall", tm=2.0):
    """Gable roof with the ridge along Z (gable ends face front/back) + gable wall triangles. Returns ridge y."""
    a, b, c0, d = x0 - o_, x1 + o_, z0 - o_, z1 + o_; cx = (x0 + x1) / 2
    slope = rise / ((x1 - x0) / 2); ya = H - o_ * slope; yr = H + rise
    o.slope_quad(m, roof, (a, ya, c0), (a, ya, d), (cx, yr, d), (cx, yr, c0), 2, tm=3.0)
    o.slope_quad(m, roof, (b, ya, d), (b, ya, c0), (cx, yr, c0), (cx, yr, d), 2, tm=3.0)
    # undersides (soffit), visible at low angles
    m.quad("trim", (a, ya - 0.02, d), (a, ya - 0.02, c0), (cx, yr - 0.02, c0), (cx, yr - 0.02, d), [(0, 0), (1, 0), (1, 1), (0, 1)])
    m.quad("trim", (b, ya - 0.02, c0), (b, ya - 0.02, d), (cx, yr - 0.02, d), (cx, yr - 0.02, c0), [(0, 0), (1, 0), (1, 1), (0, 1)])
    for z, f in ((z1, +1), (z0, -1)):
        pts = [(x0, H, z), (x1, H, z), (cx, yr, z)] if f > 0 else [(x1, H, z), (x0, H, z), (cx, yr, z)]
        m.tri(wall, *pts, [(q[0] / tm, -q[1] / tm) for q in pts])
    # barge boards + ridge cap + gutters
    for z in (d, c0):
        for (p0, p1) in (((a, ya, z), (cx, yr, z)), ((cx, yr, z), (b, ya, z))):
            pass
    mbox(m, "trim", cx - 0.18, cx + 0.18, yr - 0.05, yr + 0.08, c0, d, tm=1.0)
    for x in (a, b - 0.18):
        mbox(m, "trim", x, x + 0.18, ya - 0.2, ya, c0, d, tm=1.0, skip=("top",))
    return yr


def sawtooth(m, x0, x1, z0, z1, H, h=2.4, pitch=6.0):
    """North-light sawtooth: glazing faces -Z (back), roof slopes face +Z/up. Returns peak height."""
    n = max(1, round((z1 - z0) / pitch)); t = (z1 - z0) / n
    for i in range(n):
        a = z0 + i * t; b = a + t
        o.slope_quad(m, "roof", (x0, H, b), (x1, H, b), (x1, H + h, a), (x0, H + h, a), 0, tm=3.0)
        nx = max(1, round((x1 - x0) / 8.0))
        for k in range(nx):  # glazing strips, a steel post between runs
            u0 = x0 + k * (x1 - x0) / nx; u1 = u0 + (x1 - x0) / nx
            face_z(m, "sawglass", u0 + 0.06, u1 - 0.06, H + 0.15, H + h - 0.12, a, -1, off=0.0)
            face_z(m, "metal", u1 - 0.06, u1 + 0.06, H, H + h, a, -1, off=0.0) if k < nx - 1 else None
        face_z(m, "metal", x0, x1, H, H + 0.15, a, -1, off=0.0)
        for xx, f in ((x1, +1), (x0, -1)):
            pts = [(xx, H, b), (xx, H, a), (xx, H + h, a)] if f > 0 else [(xx, H, a), (xx, H, b), (xx, H + h, a)]
            m.tri("wall", *pts, [(q[2] / 2.0, -q[1] / 2.0) for q in pts])
        mbox(m, "trim", x0 - 0.1, x1 + 0.1, H + h - 0.05, H + h + 0.12, a - 0.12, a + 0.15, tm=1.0)  # ridge flashing
    return H + h


def parapet(m, x0, x1, z0, z1, H, ht=0.9, t=0.25, mat="wall", tm=2.0, cap=True):
    """Parapet ring above a flat roof: outer faces are the wall continued, coping on top, inner faces trim."""
    mbox(m, mat, x0, x1, H, H + ht, z0, z1, tm=tm, skip=("top", "bot"))
    for (a, b, cz0, cz1) in ((x0, x1, z1 - t, z1), (x0, x1, z0, z0 + t), (x0, x0 + t, z0 + t, z1 - t), (x1 - t, x1, z0 + t, z1 - t)):
        if cap: mbox(m, "trim", a - 0.04, b + 0.04, H + ht, H + ht + 0.08, cz0 - 0.04, cz1 + 0.04, tm=1.0, skip=("bot",))
    # inner faces
    i0, i1, k0, k1 = x0 + t, x1 - t, z0 + t, z1 - t
    m.quad("trim", (i1, H, k1), (i0, H, k1), (i0, H + ht, k1), (i1, H + ht, k1), [(0, 0), (1, 0), (1, 1), (0, 1)])
    m.quad("trim", (i0, H, k0), (i1, H, k0), (i1, H + ht, k0), (i0, H + ht, k0), [(0, 0), (1, 0), (1, 1), (0, 1)])
    m.quad("trim", (i0, H, k1), (i0, H, k0), (i0, H + ht, k0), (i0, H + ht, k1), [(0, 0), (1, 0), (1, 1), (0, 1)])
    m.quad("trim", (i1, H, k0), (i1, H, k1), (i1, H + ht, k1), (i1, H + ht, k0), [(0, 0), (1, 0), (1, 1), (0, 1)])
    flat(m, "roof", i0, i1, k0, k1, H + 0.02, 4.0)
    return H + ht + 0.08


def tree(m, x, z, h=6.0, r=2.0):
    mbox(m, "detail", x - 0.15, x + 0.15, 0.0, h * 0.45, z - 0.15, z + 0.15, k="bark", skip=("top",))
    cy = h * 0.62; top = (x, h, z); bot = (x, h * 0.3, z)
    ring = [(x + r * math.cos(a), cy, z + r * math.sin(a)) for a in (0.3, 0.3 + math.pi / 2, 0.3 + math.pi, 0.3 + 1.5 * math.pi)]
    uv = [auv("leaf", 0.5, 1), auv("leaf", 0, 0), auv("leaf", 1, 0)]
    for i in range(4):
        a, b = ring[i], ring[(i + 1) % 4]
        m.tri("detail", top, b, a, uv); m.tri("detail", bot, a, b, uv)


def rtu(m, x, z, H, w=2.4, d=1.6, h=1.3):
    mbox(m, "detail", x - w / 2, x + w / 2, H, H + h, z - d / 2, z + d / 2, k="rtu", top_k="fan")


def roof_fan(m, x, z, H, r=0.55):
    cyl(m, "metal", x, z, r, H, H + 0.7, n=8, top="fan")


def pallets(m, x, z, n=2, along="x"):
    for i in range(n):
        dx, dz = (i * 1.3, 0) if along == "x" else (0, i * 1.3)
        hh = 1.1 + 0.25 * (i % 2)
        mbox(m, "detail", x + dx - 0.6, x + dx + 0.6, 0.03, hh, z + dz - 0.5, z + dz + 0.5, k="pallet", top_k="pallet")


def skip_bin(m, x, z, rot=False):
    if rot: mbox(m, "brand2", x - 0.9, x + 0.9, 0.03, 1.4, z - 1.6, z + 1.6, k="skip", top_k="rust")
    else: mbox(m, "brand2", x - 1.6, x + 1.6, 0.03, 1.4, z - 0.9, z + 0.9, k="skip", top_k="rust")


def container(m, x, z, rot=False, kind="cont_a", y=0.03):
    L, Wd, Hh = 6.06, 2.44, 2.6
    if rot: mbox(m, "detail", x - Wd / 2, x + Wd / 2, y, y + Hh, z - L / 2, z + L / 2, k="cont_end", side_k=kind, top_k="rust")
    else: mbox(m, "detail", x - L / 2, x + L / 2, y, y + Hh, z - Wd / 2, z + Wd / 2, k=kind, side_k="cont_end", top_k="rust")


def truck(m, x, z_back, facing=+1, L=13.6):
    """Semi trailer + cab along Z. z_back is the trailer's rear (against the dock); the cab is toward `facing`."""
    zb, zf = (z_back, z_back + L) if facing > 0 else (z_back - L, z_back)
    w = 1.25
    mbox(m, "detail", x - w, x + w, 1.05, 4.0, zb, zf, k="trailer_end", side_k="trailer", top_k="metal")
    mbox(m, "detail", x - 0.5, x + 0.5, 0.45, 1.05, zb + 1.0, zf - 1.0, k="tyre", skip=("top",))
    for zz in ((zb + 0.8, zb + 3.2) if facing > 0 else (zf - 3.2, zf - 0.8)):
        pass
    az = (zb + 1.0, zb + 3.4) if facing > 0 else (zf - 3.4, zf - 1.0)
    mbox(m, "detail", x - w + 0.05, x + w - 0.05, 0.03, 1.05, *az, k="tyre", side_k="tyre")
    cz0, cz1 = (zf + 0.3, zf + 2.6) if facing > 0 else (zb - 2.6, zb - 0.3)
    mbox(m, "detail", x - w, x + w, 0.45, 3.4, cz0, cz1, k="cab_front", side_k="cab_side", top_k="metal")
    mbox(m, "detail", x - w + 0.05, x + w - 0.05, 0.03, 0.45, cz0 - (0.3 if facing > 0 else 0), cz1 + (0 if facing > 0 else 0.3), k="tyre", side_k="tyre")
    if facing < 0:  # cab front faces -Z: back face of the cab box got the front texture already (mbox maps k to both)
        pass


def car(m, x, z, rot=False):
    if rot: mbox(m, "detail", x - 0.9, x + 0.9, 0.04, 1.45, z - 2.2, z + 2.2, k="car", side_k="car", top_k="car_top")
    else: mbox(m, "detail", x - 2.2, x + 2.2, 0.04, 1.45, z - 0.9, z + 0.9, k="car", side_k="car", top_k="car_top")


def stalls(m, x0, x1, z0, z1, cars=False, seed=0):
    """Row of 2.5 m stalls along X, cars facing +-Z. Returns bay count."""
    flat(m, "ground", x0, x1, z0, z1, 0.045, 6.0)
    n = int((x1 - x0) // 2.5); sx = x0 + (x1 - x0 - n * 2.5) / 2
    rng = np.random.default_rng(seed)
    for i in range(n + 1):
        lx = sx + i * 2.5; dflat(m, "paint", lx - 0.06, lx + 0.06, z0 + 0.2, z1 - 0.2, 0.06)
    for i in range(n):
        if cars and rng.random() < 0.6: car(m, sx + (i + 0.5) * 2.5, (z0 + z1) / 2, rot=True)
    return n


def lamp(m, x, z, h=7.0):
    mbox(m, "detail", x - 0.08, x + 0.08, 0.0, h, z - 0.08, z + 0.08, k="metal", skip=("top",))
    mbox(m, "detail", x - 0.25, x + 0.25, h - 0.18, h, z - 0.8, z + 0.08, k="metal", top_k="metal")
    dflat(m, "lamp", x - 0.2, x + 0.2, z - 0.75, z - 0.2, h - 0.19)


def bollards(m, x0, x1, z, n):
    for xx in np.linspace(x0, x1, n):
        mbox(m, "detail", xx - 0.1, xx + 0.1, 0.0, 0.9, z - 0.1, z + 0.1, k="hazard", top_k="yellow")


def ladder_x(m, z, y0, y1, x, f):
    face_x(m, "ladder", z - 0.45, z + 0.45, y0, y1, x, f, off=0.08)


def ladder_z(m, x, y0, y1, z, f):
    face_z(m, "ladder", x - 0.45, x + 0.45, y0, y1, z, f, off=0.08)


def sign_z(m, x0, x1, y0, y1, z, mat="brand1", f=+1):
    face_z(m, "sign", x0, x1, y0, y1, z, f, off=0.06, mat=mat)


def window_band_z(m, k, a, b, y0, y1, z, f, w=1.6, gap=3.0, skip=()):
    n = max(1, int((b - a) // gap))
    for i in range(n):
        cx = a + (i + 0.5) * (b - a) / n
        if any(s0 < cx < s1 for s0, s1 in skip): continue
        face_z(m, k, cx - w / 2, cx + w / 2, y0, y1, z, f)


def window_band_x(m, k, a, b, y0, y1, x, f, w=1.6, gap=3.0):
    n = max(1, int((b - a) // gap))
    for i in range(n):
        cz = a + (i + 0.5) * (b - a) / n
        face_x(m, k, cz - w / 2, cz + w / 2, y0, y1, x, f)


def downpipes(m, xs, z, H, f=+1):
    for x in xs:
        mbox(m, "trim", x - 0.07, x + 0.07, 0.0, H, *((z, z + 0.14) if f > 0 else (z - 0.14, z)), tm=1.0, skip=("top",))


def plinth(m, x0, x1, z0, z1, h=0.6):
    mbox(m, "trim", x0 - 0.06, x1 + 0.06, 0.0, h, z0 - 0.06, z1 + 0.06, skip=("top",), tm=1.0)


def frame_lot(W, D):
    lw, ld = W * CELL, D * CELL
    return lw, ld, -lw / 2, lw / 2, -ld / 2, ld / 2


def site(m, W, D, green=False):
    lw, ld, lx0, lx1, lz0, lz1 = frame_lot(W, D)
    street = lz1 - SIDEWALK
    flat(m, "paving", lx0 + 0.1, lx1 - 0.1, street, lz1 - 0.1, 0.06, 2.0)
    flat(m, "grass" if green else "ground", lx0 + 0.1, lx1 - 0.1, lz0 + 0.1, street, 0.03, 4.0 if green else 6.0)
    mbox(m, "trim", lx0 + 0.1, lx1 - 0.1, 0.0, 0.18, street - 0.15, street, tm=1.0)  # kerb
    return lw, ld, lx0, lx1, lz0, lz1, street


def driveway(m, x0, x1, street, z_in):
    flat(m, "ground", x0, x1, z_in, street + SIDEWALK - 0.1, 0.07, 6.0)


# ------------------------------------------------------------------ L1 workshop
def build_workshop(W, D, p):
    m = new_mesh(); lw, ld, lx0, lx1, lz0, lz1, street = site(m, W, D)
    yd = float(np.clip(ld * 0.3 * V.yk, 4.5, 11.0))
    annex = (4.0 + 2.0 * V.b) if (W >= 3 and V.a < 0.8) else 0.0
    x0, x1 = lx0 + 1.0, lx1 - 1.0 - annex
    z1 = street - yd; z0 = lz0 + 1.0
    H = (4.6 + (0.8 if W * D >= 12 else 0.0) + (0.6 if W * D >= 25 else 0.0)) * V.hk
    hw = x1 - x0; rise = hw / 2 * V.pitch
    plinth(m, x0, x1, z0, z1, 0.4)
    mbox(m, "wall", x0, x1, 0.0, H, z0, z1, tm=p["wall_m"], skip=("top", "bot"))
    top = gable_z(m, x0, x1, z0, z1, H, rise)
    # front gable: roll-up doors + personnel door + sign + gable vent
    nd = max(1, int(hw // 7.5)); dw = min(4.2, hw / nd - 1.6); dh = min(4.2, H - 0.5)
    doors = []
    for i in range(nd):
        cx = x0 + (i + 0.5) * hw / nd
        face_z(m, "rollup", cx - dw / 2, cx + dw / 2, 0.05, dh, z1)
        mbox(m, "brand2", cx - dw / 2 - 0.15, cx + dw / 2 + 0.15, dh, dh + 0.18, z1, z1 + 0.12, k="metal")
        bollards(m, cx - dw / 2 - 0.4, cx + dw / 2 + 0.4, z1 + 0.5, 2)
        flat(m, "paving", cx - dw / 2 - 0.5, cx + dw / 2 + 0.5, z1, z1 + 4.0, 0.05, 2.0)
        doors.append(cx)
    pdx = x1 - 0.9 if nd == 1 and hw > dw + 3.0 else None
    if pdx: face_z(m, "pdoor", pdx - 0.5, pdx + 0.5, 0.05, 2.2, z1)
    cx = (x0 + x1) / 2
    sign_z(m, cx - min(3.5, hw * 0.35), cx + min(3.5, hw * 0.35), H + 0.1, H + min(1.1, rise * 0.7), z1)
    face_z(m, "sheet_gable", cx - 0.6, cx + 0.6, H + rise * 0.72, H + rise * 0.72 + 0.5, z1) if rise > 1.2 else None
    # sides: steel windows high up, downpipes, side door
    for xx, f in ((x1, +1), (x0, -1)):
        if not (annex and f > 0):
            window_band_x(m, "win", z0 + 1, z1 - 1, H - 1.9, H - 0.5, xx, f, w=1.6, gap=3.6)
    face_x(m, "pdoor", z0 + 1.0, z0 + 2.0, 0.05, 2.2, x0, -1)
    window_band_z(m, "win", x0 + 1, x1 - 1, H - 1.9, H - 0.5, z0, -1, w=1.6, gap=4.0)
    downpipes(m, (x0 + 0.2, x1 - 0.2), z1, H, +1)
    # roof flue + vent
    cyl(m, "pipe", x0 + hw * 0.25, (z0 + z1) / 2, 0.18, H + rise * 0.5, top + 1.4, n=6, top="soot")
    cyl(m, "metal", x0 + hw * 0.25, (z0 + z1) / 2, 0.32, top + 1.4, top + 1.6, n=6, top="soot")
    # lean-to office on the right
    if annex:
        ax0, ax1 = x1, lx1 - 1.0; az1 = z1; az0 = max(z0, z1 - 8.0); ah = 3.3
        mbox(m, "wall2", ax0, ax1, 0.0, ah, az0, az1, tm=2.0, skip=("bot", "top"))
        o.slope_quad(m, "roof", (ax1 + 0.3, ah, az1 + 0.3), (ax1 + 0.3, ah, az0 - 0.3), (ax0, ah + 0.6, az0 - 0.3), (ax0, ah + 0.6, az1 + 0.3), 2, tm=3.0)
        mbox(m, "wall2", ax0, ax1 + 0.3, ah, ah + 0.6, az1 + 0.3, az1 + 0.3, skip=("top", "bot", "back", "left", "right"))
        face_z(m, "gdoor", ax0 + 0.6, ax0 + 1.8, 0.05, 2.3, az1)
        face_z(m, "win", ax0 + 2.3, ax1 - 0.4, 1.0, 2.4, az1)
        face_x(m, "win", az0 + 1.0, az0 + 2.6, 1.0, 2.4, ax1, +1)
        if az1 - az0 > 5: face_x(m, "win", az1 - 3.2, az1 - 1.6, 1.0, 2.4, ax1, +1)
        mbox(m, "brand1", ax0 + 0.3, ax0 + 2.1, 2.4, 2.55, az1, az1 + 1.0, k="metal")
        flat(m, "paving", ax0 + 0.3, ax0 + 2.1, az1, street, 0.05, 2.0)
        flat(m, "paving", ax0 + 0.3, ax1, az0, az1, 0.0, 2.0) if False else None
        mbox(m, "trim", ax0, ax1, 0.0, 0.3, az1, az1 + 0.2, tm=1.0)
    # yard: pallet stacks, skip bin, van or container, driveway
    pallets(m, lx0 + 1.8, z1 + yd * 0.5, 2, "z") if yd > 6 else pallets(m, lx0 + 1.6, z1 + 1.4, 1)
    if hw > 9: skip_bin(m, x1 - 2.2, z1 + yd - 2.4)
    if yd >= 8 and W * D >= 9:
        container(m, lx0 + 3.2 + (2 if W >= 4 else 0), z1 + 2.5, kind=V.cont) if W >= 3 else None
        if V.c < 0.4 and W >= 4: container(m, lx0 + 3.2 + 2, z1 + 2.5, kind="cont_a" if V.cont == "cont_b" else "cont_b", y=2.63)
    for cx in doors:
        driveway(m, cx - dw / 2 - 0.5, cx + dw / 2 + 0.5, street, z1 + 4.0)
    if W * D >= 6: lamp(m, lx1 - 1.0, street - 0.6, 6.0)
    return m, top


# ------------------------------------------------------------------ L2 brickworks
def build_brickworks(W, D, p):
    m = new_mesh(); lw, ld, lx0, lx1, lz0, lz1, street = site(m, W, D)
    yd = float(np.clip(ld * 0.24 * V.yk, 3.5, 9.0))
    x0, x1 = lx0 + 1.0, lx1 - 1.0
    tower = W * D >= 16
    if tower: x1 -= 0.0
    z1 = street - yd; z0 = lz0 + 1.0
    H = (6.2 if W * D < 12 else 7.0) * V.hk
    plinth(m, x0, x1, z0, z1, 0.7)
    mbox(m, "wall", x0, x1, 0.0, H, z0, z1, tm=p["wall_m"], skip=("top", "bot"))
    # pilasters + tall windows between them on every face; string course; corbelled eaves band
    def bays(a, b, step=4.2):
        n = max(1, int((b - a) // step)); return [a + i * (b - a) / n for i in range(n + 1)], (b - a) / n
    cx = (x0 + x1) / 2; door_w = min(4.0, (x1 - x0) * 0.3)
    for zz, f in ((z1, +1), (z0, -1)):
        piers, bw = bays(x0, x1)
        for px in piers:
            mbox(m, "wall", px - 0.3, px + 0.3, 0.0, H + 0.6, *((zz, zz + 0.25) if f > 0 else (zz - 0.25, zz)), tm=p["wall_m"], skip=("bot",))
        for i in range(len(piers) - 1):
            mx = (piers[i] + piers[i + 1]) / 2
            if f > 0 and abs(mx - cx) < door_w / 2 + 0.6: continue
            ww = min(2.4, bw - 1.2)
            face_z(m, "win_tall", mx - ww / 2, mx + ww / 2, 1.1, H - 1.1, zz, f)
    for xx, f in ((x1, +1), (x0, -1)):
        piers, bw = bays(z0, z1)
        for pz in piers[1:-1]:
            mbox(m, "wall", *((xx, xx + 0.25) if f > 0 else (xx - 0.25, xx)), 0.0, H + 0.6, pz - 0.3, pz + 0.3, tm=p["wall_m"], skip=("bot",))
        for i in range(len(piers) - 1):
            mz = (piers[i] + piers[i + 1]) / 2; ww = min(2.4, bw - 1.2)
            if f > 0 and i == 0 and D >= 3:
                face_x(m, "rollup", mz - ww / 2 - 0.3, mz + ww / 2 + 0.3, 0.05, min(4.2, H - 1.2), xx, f)
                continue
            face_x(m, "win_tall", mz - ww / 2, mz + ww / 2, 1.1, H - 1.1, xx, f)
    # main doorway: arched-look brick surround + roll-up + sign band across the front parapet
    face_z(m, "rollup", cx - door_w / 2, cx + door_w / 2, 0.05, min(4.6, H - 1.0), z1, off=0.04)
    mbox(m, "trim", cx - door_w / 2 - 0.4, cx + door_w / 2 + 0.4, min(4.6, H - 1.0), min(4.6, H - 1.0) + 0.35, z1, z1 + 0.3, tm=1.0)
    flat(m, "paving", cx - door_w / 2 - 0.6, cx + door_w / 2 + 0.6, z1, street, 0.05, 2.0)
    driveway(m, cx - door_w / 2 - 0.6, cx + door_w / 2 + 0.6, street, z1 + 0.1)
    # parapet front (stepped gable-look) hides the sawtooth from the street
    mbox(m, "wall", x0, x1, H, H + 1.6, z1 - 0.35, z1, tm=p["wall_m"], skip=("bot",))
    mbox(m, "trim", x0 - 0.1, x1 + 0.1, H + 1.6, H + 1.75, z1 - 0.45, z1 + 0.1, tm=1.0)
    mbox(m, "trim", x0 - 0.05, x1 + 0.05, H - 0.35, H - 0.1, z0 - 0.05, z1 + 0.3, tm=1.0, skip=("top",))  # string course
    sign_z(m, cx - min(5.0, (x1 - x0) * 0.35), cx + min(5.0, (x1 - x0) * 0.35), H + 0.25, H + 1.35, z1 + 0.0)
    top = sawtooth(m, x0, x1, z0, z1 - 0.35, H, h=2.4 if W * D < 12 else 3.0, pitch=V.saw)
    top = max(top, H + 1.75)
    downpipes(m, (x0 + 0.5, x1 - 0.5), z0, H, -1)
    # chimney at the back corner (always on 3x3+); square tapered brick with a soot-black crown
    if W * D >= 9:
        ch = 16.0 + 2.0 * min(W, D)
        ccx, ccz = [(x1 - 2.0, z0 + 2.0), (x0 + 2.0, z0 + 2.0), ((x0 + x1) / 2, z0 + 2.0)][int(V.a * 3)]
        ch *= 0.8 + 0.45 * V.b
        taper(m, "wall", ccx, ccz, 1.1, 0.75, 0.0, ch, tm=p["wall_m"])
        mbox(m, "trim", ccx - 0.95, ccx + 0.95, ch - 0.1, ch + 0.5, ccz - 0.95, ccz + 0.95, tm=1.0, top_k=None)
        dflat(m, "soot", ccx - 0.7, ccx + 0.7, ccz - 0.7, ccz + 0.7, ch + 0.52)
        mbox(m, "detail", ccx - 0.8, ccx + 0.8, ch - 1.6, ch - 1.3, ccz - 0.8, ccz + 0.8, k="soot", top_k="soot")
        ladder_z(m, ccx, H + 3.0, ch - 1.6, ccz + 0.78, +1)
        top = max(top, ch + 0.5)
    # steel water tank on legs (big lots)
    if W * D >= 12 and V.c < 0.7:
        tx, tz, ty = x0 + 3.5, z0 + 3.5, top - 0.5 if top < H + 6 else H + 6.0
        ty = H + 5.5
        for lx, lz in ((-1.2, -1.2), (1.2, -1.2), (1.2, 1.2), (-1.2, 1.2)):
            mbox(m, "detail", tx + lx - 0.1, tx + lx + 0.1, H + 2.0, ty, tz + lz - 0.1, tz + lz + 0.1, k="rust", skip=("top",))
        cyl(m, "tank", tx, tz, 1.7, ty, ty + 3.0, n=10, top="tank_top", cone=0.7)
        top = max(top, ty + 3.7)
    # yard
    pallets(m, x0 + 1.4, z1 + min(2.4, yd - 1.0), 2)
    if W >= 3: skip_bin(m, x1 - 2.4, z1 + min(2.2, yd - 1.0))
    if W * D >= 6: lamp(m, lx0 + 1.0, street - 0.6, 6.0)
    return m, top


# ------------------------------------------------------------------ L3 plant (docks)
def dock_row(m, a, b, z, H, n_max=8, trucks_room=0.0, f=+1, canopy=True):
    """Dock doors along a front face z (facing +Z); returns dock centre xs."""
    n = min(n_max, int((b - a) // 4.0))
    if n <= 0: return []
    xs = [a + (i + 0.5) * (b - a) / n for i in range(n)]
    for i, x in enumerate(xs):
        face_z(m, "dock", x - 1.55, x + 1.55, 0.05, 3.6, z, f)
        dflat(m, "yellow", x - 1.7, x - 1.55, z + 0.3, z + min(14.0, max(4.0, trucks_room)), 0.06)
        dflat(m, "yellow", x + 1.55, x + 1.7, z + 0.3, z + min(14.0, max(4.0, trucks_room)), 0.06)
        if trucks_room >= 16.5 and i % 2 == 0:
            pass  # no vehicles baked in: trucks are game agents
    if canopy:
        mbox(m, "trim", a - 0.3, b + 0.3, 4.2, 4.45, z, z + 2.2, tm=1.0)
        mbox(m, "detail", a - 0.3, b + 0.3, 4.05, 4.2, z + 2.05, z + 2.2, k="hazard", skip=("top",))
    return xs


def office_block(m, p, x0, x1, z0, z1, floors=2, fh=3.5, glass=False, fins=False, mat="wall2"):
    H = 0.15 + floors * fh
    mbox(m, mat, x0, x1, 0.0, H, z0, z1, tm=2.0, skip=("top", "bot"))
    if glass:
        for s in range(floors):
            y = 0.15 + s * fh
            for zz, f in ((z1, +1),):
                n = max(1, int((x1 - x0) // 1.5))
                for i in range(n):
                    u0 = x0 + i * (x1 - x0) / n
                    face_z(m, "curtain", u0, u0 + (x1 - x0) / n, y + 0.05, y + fh - 0.05, zz, f, off=0.01)
            for xx, f in ((x1, +1), (x0, -1)):
                n = max(1, int((z1 - z0) // 1.5))
                for i in range(n):
                    u0 = z0 + i * (z1 - z0) / n
                    face_x(m, "curtain", u0, u0 + (z1 - z0) / n, y + 0.05, y + fh - 0.05, xx, f, off=0.01)
            mbox(m, "trim", x0 - 0.02, x1 + 0.02, y + fh - 0.1, y + fh + 0.1, z1, z1 + 0.06, tm=1.0, skip=("top",))
    else:
        for s in range(floors):
            y = 0.15 + s * fh
            L = x1 - x0; reps = max(1, round(L / 6.0))
            for r in range(reps):
                a = x0 + 0.4 + r * (L - 0.8) / reps; b = a + (L - 0.8) / reps
                if s == 0 and r == 0: a += 2.2
                if b - a > 1.0: face_z(m, "ribbon", a, b, y + 0.9, y + fh - 0.5, z1, +1)
            for xx, f in ((x1, +1), (x0, -1)):
                Lz = z1 - z0; rz = max(1, round(Lz / 6.0))
                for r in range(rz):
                    a = z0 + 0.4 + r * (Lz - 0.8) / rz; b = a + (Lz - 0.8) / rz
                    face_x(m, "ribbon", a, b, y + 0.9, y + fh - 0.5, xx, f)
    face_z(m, "gdoor", x0 + 0.6, x0 + 2.4, 0.05, 2.6, z1, off=0.05)
    mbox(m, "brand1", x0 + 0.2, x0 + 2.9, 2.85, 3.05, z1, z1 + 1.6, k="metal")
    top = parapet(m, x0, x1, z0, z1, H, ht=0.6, mat=mat)
    if fins:
        n = int((x1 - x0) // 1.5)
        for i in range(n + 1):
            fx = x0 + i * (x1 - x0) / n
            mbox(m, "brand2", fx - 0.06, fx + 0.06, 0.15, H + 0.6, z1, z1 + 0.7, k="metal")
    return H, top


def build_plant(W, D, p):
    m = new_mesh(); lw, ld, lx0, lx1, lz0, lz1, street = site(m, W, D)
    apron = float(np.clip(ld * 0.42 * V.yk, 5.0, 19.0))
    x0, x1 = lx0 + 1.0, lx1 - 1.0
    z1 = street - apron; z0 = lz0 + 1.0
    silo = W >= 3 and D >= 3 and V.a < 0.75
    if silo: x0 += 5.0
    H = (8.0 if W * D < 12 else 9.5) * V.hk
    plinth(m, x0, x1, z0, z1, 0.5)
    mbox(m, "wall", x0, x1, 0.0, H, z0, z1, tm=p["wall_m"], skip=("top", "bot"))
    top = parapet(m, x0, x1, z0, z1, H, ht=0.8)
    # colour band (brand1) under the parapet + sign
    mbox(m, "brand1", x0 - 0.03, x1 + 0.03, H - 0.6, H + 0.8, z1 - 0.01, z1 + 0.03, k="metal", skip=("top", "bot", "back", "left", "right"))
    hw = x1 - x0
    ow = min(11.0, hw * 0.42) if hw > 12 else 0.0
    if ow:  # office in front of the hall, left side
        oz1 = min(street - 4.0, z1 + 6.5)
        _, otop = office_block(m, p, x0, x0 + ow, z1, oz1, floors=2 if W * D >= 9 else 1, fh=3.5)
        flat(m, "paving", x0, x0 + ow, oz1, street, 0.05, 2.0)
        window_band_z(m, "ribbon", x0 + 0.5, x0 + ow - 0.5, H - 3.0, H - 1.6, z1, +1, w=ow * 0.3, gap=ow * 0.5) if False else None
    da = x0 + ow + 1.0 if ow else x0 + 1.0
    docks = dock_row(m, da, x1 - 1.0, z1, H, trucks_room=apron - 0.4)
    driveway(m, da, x1 - 0.6, street, z1 + 0.2)
    if not docks: face_z(m, "rollup", x1 - 5.0, x1 - 1.0, 0.05, 4.2, z1)
    # side and back walls: high steel windows, personnel doors, ladder to the roof
    for xx, f in ((x1, +1), (x0, -1)):
        window_band_x(m, "win", z0 + 1.0, z1 - 1.0, H - 2.4, H - 1.0, xx, f, w=2.0, gap=4.5)
        face_x(m, "pdoor", z1 - 2.6, z1 - 1.6, 0.05, 2.2, xx, f)
    window_band_z(m, "win", x0 + 1, x1 - 1, H - 2.4, H - 1.0, z0, -1, w=2.0, gap=5.0)
    ladder_x(m, z0 + 1.6, 0.4, H + 0.8, x1, +1)
    face_x(m, "warn", z1 - 3.6, z1 - 3.0, 1.4, 2.0, x1, +1)
    # roof: rooftop units, extract fans, rooflight strips
    nr = max(1, int(hw // 9))
    for i in range(nr):
        rx = x0 + (i + 0.5) * hw / nr
        rtu(m, rx, z0 + (z1 - z0) * 0.35, H + 0.02)
        roof_fan(m, rx + 2.5 if hw > 8 else rx, z0 + (z1 - z0) * 0.7, H + 0.02)
    for i in range(max(1, int(hw // 6))):
        sx = x0 + (i + 0.5) * hw / max(1, int(hw // 6))
        dflat(m, "skylight", sx - 0.6, sx + 0.6, z0 + 1.5, z1 - 1.5, H + 0.05) if (z1 - z0) > 6 else None
    # silos with a chute into the hall
    if silo:
        n = 1 + V.n + (D >= 5); r = 1.5 + 0.5 * V.b; sh = H + 3.0 + 4.0 * V.c
        for i in range(n):
            sz = z0 + 2.0 + i * 3.9
            if sz + r > z1: break
            cyl(m, "tank", lx0 + 3.2, sz, r, 2.2, sh, n=12, top="tank_top", cone=1.0)
            cyl(m, "metal", lx0 + 3.2, sz, 0.5, 0.0, 2.2, n=6, top=None)  # hopper outlet
            for lx, lz in ((-1.2, -1.2), (1.2, -1.2), (1.2, 1.2), (-1.2, 1.2)):
                mbox(m, "detail", lx0 + 3.2 + lx - 0.08, lx0 + 3.2 + lx + 0.08, 0.0, 2.3, sz + lz - 0.08, sz + lz + 0.08, k="metal", skip=("top",))
            mbox(m, "detail", lx0 + 3.2, x0, sh - 1.4, sh - 0.8, sz - 0.3, sz + 0.3, k="metal", side_k="metal")
        ladder_x(m, z0 + 2.0, 2.4, sh, lx0 + 3.2 - r, -1)
        top = max(top, sh + 1.0)
    if W * D >= 6: lamp(m, lx1 - 1.0, street - 0.6, 8.0); lamp(m, (da + x1) / 2, z1 + apron * 0.6, 8.0) if apron > 12 else None
    bollards(m, da, da + 0.01, z1 + 0.6, 1)
    return m, top


# ------------------------------------------------------------------ L4 process plant
def build_process(W, D, p):
    m = new_mesh(); lw, ld, lx0, lx1, lz0, lz1, street = site(m, W, D)
    apron = float(np.clip(ld * 0.36 * V.yk, 5.0, 18.0))
    farm = min(15.0, lw * (0.3 + 0.1 * V.b)) if (W >= 3 and V.a < 0.8) else 0.0
    x0, x1 = lx0 + 1.0, lx1 - 1.0 - farm
    z1 = street - apron; z0 = lz0 + 1.0
    H = (10.0 if W * D < 12 else 12.0) * V.hk
    plinth(m, x0, x1, z0, z1, 0.5)
    mbox(m, "wall", x0, x1, 0.0, H, z0, z1, tm=p["wall_m"], skip=("top", "bot"))
    top = parapet(m, x0, x1, z0, z1, H, ht=0.9, tm=p["wall_m"])
    hw = x1 - x0
    # clerestory bands all round
    for zz, f in ((z1, +1), (z0, -1)):
        window_band_z(m, "ribbon", x0 + 0.6, x1 - 0.6, H - 2.4, H - 1.0, zz, f, w=5.0, gap=6.0)
    for xx, f in ((x1, +1), (x0, -1)):
        window_band_x(m, "ribbon", z0 + 0.6, z1 - 0.6, H - 2.4, H - 1.0, xx, f, w=5.0, gap=6.0)
    # small 2-storey office at the front left, docks to its right
    ow = min(9.0, hw * 0.4) if hw > 11 else 0.0
    if ow:
        oz1 = min(street - 4.0, z1 + 6.0)
        office_block(m, p, x0, x0 + ow, z1, oz1, floors=2, fh=3.6, mat="wall2")
        flat(m, "paving", x0, x0 + ow, oz1, street, 0.05, 2.0)
    da = x0 + ow + 1.0 if ow else x0 + 1.0
    docks = dock_row(m, da, x1 - 0.8, z1, H, n_max=4, trucks_room=apron - 0.4)
    if not docks: face_z(m, "rollup", x0 + 1.0, x1 - 1.0, 0.05, 4.6, z1)
    driveway(m, da, x1 - 0.5, street, z1 + 0.2)
    sign_z(m, x0 + ow + 1.0 if ow else x0 + 1.0, x1 - 1.0, H - 0.9, H + 0.7, z1, mat="brand1")
    face_x(m, "pdoor", z0 + 1.0, z0 + 2.0, 0.05, 2.2, x0, -1)
    ladder_x(m, z0 + 2.4, 0.4, H + 0.9, x0, -1)
    # roof: cooling units + big extract + steel stack
    nr = max(1, int(hw // 8))
    for i in range(nr):
        rx = x0 + (i + 0.5) * hw / nr
        rtu(m, rx, z0 + (z1 - z0) * 0.3, H + 0.02, w=3.0, d=2.0, h=1.6)
        roof_fan(m, rx, z0 + (z1 - z0) * 0.65, H + 0.02, r=0.8)
    sh = H + (6.0 + 8.0 * V.c) + 2.0 * min(W, D)
    sx, sz = [(x0 + 2.2, z0 + 2.2), (x1 - 2.2, z0 + 2.2), ((x0 + x1) / 2, z0 + 2.2)][V.n]
    cyl(m, "pipe", sx, sz, 0.75, H, sh, n=10, top="soot")
    for y in (sh - 0.6, H + (sh - H) * 0.5):
        cyl(m, "metal", sx, sz, 0.9, y, y + 0.3, n=10, top="grate")
    mbox(m, "detail", sx + 0.7, sx + 0.95, H + 0.5, sh - 0.6, sz - 0.45, sz + 0.45, k="ladder", side_k="ladder", skip=("top",))
    top = max(top, sh)
    # tank farm: vertical tanks in a concrete bund, pipe rack into the hall
    if farm:
        fx0, fx1 = x1 + 1.0, lx1 - 0.8; fz0, fz1 = z0, min(z1, z0 + 22.0)
        mbox(m, "trim", fx0, fx1, 0.0, 0.8, fz0, fz1, tm=1.0, skip=("top",))
        flat(m, "paving", fx0 + 0.2, fx1 - 0.2, fz0 + 0.2, fz1 - 0.2, 0.3, 2.0)
        r = min(2.4, (fx1 - fx0) / 4.6)
        cols = max(1, int((fx1 - fx0) // (2 * r + 1.0))); rows = max(1, int((fz1 - fz0) // (2 * r + 1.4)))
        th = 5.0 + 2.0 * (W * D >= 20) + 3.0 * V.b
        for i in range(cols):
            for j in range(rows):
                tx = fx0 + (i + 0.5) * (fx1 - fx0) / cols; tz = fz0 + (j + 0.5) * (fz1 - fz0) / rows
                cyl(m, "tank", tx, tz, r, 0.3, th + (j % 2) * 1.5, n=12, top="tank_top", cone=0.4)
        # pipe rack at y=5 across to the hall wall
        ry = 5.2; rz = fz0 + (fz1 - fz0) * 0.5 if rows == 1 else fz0 + (fz1 - fz0) / rows
        for xx in np.linspace(x1 + 0.6, fx1 - 0.6, max(2, int((fx1 - x1) // 5) + 1)):
            mbox(m, "detail", xx - 0.12, xx + 0.12, 0.3, ry, rz - 0.12, rz + 0.12, k="metal", skip=("top",))
            mbox(m, "detail", xx - 0.12, xx + 0.12, ry, ry + 0.2, rz - 1.0, rz + 1.0, k="metal")
        for dz in (-0.55, 0.0, 0.55):
            hcyl(m, "pipe", x1, fx1 - 0.3, ry + 0.45, rz + dz, 0.22, n=6, cap=None)
        # front fence line of the farm: bollards + warning sign
        face_z(m, "warn", fx0 + 0.6, fx0 + 1.2, 0.25, 0.75, fz1, +1)
        if fz1 + 3 < street:
            hcyl(m, "tank", fx0 + 1.2, fx1 - 1.2, 1.6, fz1 + 2.0, 1.2, n=10)  # horizontal storage tank on saddles
            for sx_ in (fx0 + 2.0, fx1 - 2.0):
                mbox(m, "trim", sx_ - 0.3, sx_ + 0.3, 0.0, 0.8, fz1 + 1.1, fz1 + 2.9, tm=1.0)
        # conveyor gallery from a ground hopper up to the hall roof (big lots)
        if W * D >= 20 and z1 - fz1 > 0:
            pass
    if W * D >= 16 and not farm:
        pass
    if W * D >= 20:  # conveyor gallery: sloped box from a hopper near the back to the hall roof
        gx = x0 + hw * 0.62; gz0, gz1 = z0 - 0.0, z0 + (z1 - z0) * 0.45
        hx0, hx1 = gx - 1.2, gx + 1.2
        m.quad("detail", (hx1, H + 3.0, gz0 + 4.0), (hx0, H + 3.0, gz0 + 4.0), (hx0, 2.8, gz0 - 0.01), (hx1, 2.8, gz0 - 0.01), FULL("metal")) if False else None
        cyl(m, "tank", gx, z0 + (z1 - z0) * 0.5, 2.0, H, H + 4.0, n=10, top="tank_top", cone=0.8)  # process vessel on roof
        top = max(top, H + 4.8)
    if W * D >= 6: lamp(m, lx0 + 1.0, street - 0.6, 8.0)
    return m, top


# ------------------------------------------------------------------ L5 hi-tech
def glass_band(m, x0, x1, z0, z1, y0, y1, sides=True, step=1.5):
    hw = x1 - x0
    for zz, f in ((z1, +1), (z0, -1)):
        n = max(1, int(hw // step))
        for i in range(n):
            u0 = x0 + i * hw / n; face_z(m, "curtain", u0, u0 + hw / n, y0, y1, zz, f, off=0.01)
    if sides:
        for xx, f in ((x1, +1), (x0, -1)):
            n = max(1, int((z1 - z0) // step))
            for i in range(n):
                u0 = z0 + i * (z1 - z0) / n; face_x(m, "curtain", u0, u0 + (z1 - z0) / n, y0, y1, xx, f, off=0.01)


def solar_rows(m, x0, x1, z0, z1, H):
    zr = z0 + 1.0
    while zr + 1.9 < z1 - 0.8:
        y0, y1 = H + 0.35, H + 1.05
        m.quad("detail", (x0 + 0.8, y0, zr + 1.7), (x1 - 0.8, y0, zr + 1.7), (x1 - 0.8, y1, zr), (x0 + 0.8, y1, zr), FULL("solar"))
        m.quad("detail", (x1 - 0.8, y0, zr + 1.7), (x0 + 0.8, y0, zr + 1.7), (x0 + 0.8, y1, zr), (x1 - 0.8, y1, zr), FULL("metal"))
        zr += 2.6


def monitor(m, x0, x1, z0, z1, H, h=1.6):
    """Raised roof monitor: glazed long sides, flat top."""
    mbox(m, "wall2", x0, x1, H, H + h, z0, z1, tm=4.0, skip=("bot",))
    face_z(m, "sawglass", x0 + 0.2, x1 - 0.2, H + 0.2, H + h - 0.2, z1, +1, off=0.02)
    face_z(m, "sawglass", x0 + 0.2, x1 - 0.2, H + 0.2, H + h - 0.2, z0, -1, off=0.02)


def entry(m, x, z, w=3.4):
    mbox(m, "wall2", x - w / 2, x + w / 2, 3.2, 3.45, z, z + 2.4, tm=2.0)
    for xx in (x - w / 2 + 0.2, x + w / 2 - 0.2):
        mbox(m, "detail", xx - 0.06, xx + 0.06, 0.0, 3.2, z + 2.2, z + 2.32, k="metal", skip=("top",))
    face_z(m, "gdoor", x - 1.0, x + 1.0, 0.05, 2.6, z, off=0.05)


def landscape(m, lw, lx0, lx1, street, front, avoid=()):
    for i, xx in enumerate(np.linspace(lx0 + 2.0, lx1 - 2.0, max(2, int(lw // (8 + 4 * V.b))))):
        if any(a < xx < b for a, b in avoid): continue
        tree(m, xx, street - 0.4 - (0 if front < 7.0 else V.c * 3.0), 5.0 + 1.5 * ((i + V.n) % 2), 1.6 + 0.4 * V.a)


def build_hitech(W, D, p):
    """Four different hi-tech plants, one per variant: campus, lab, data / clean-room, robotic logistics."""
    m = new_mesh(); lw, ld, lx0, lx1, lz0, lz1, street = site(m, W, D, green=True)
    kind = V.v % 4
    front = float(np.clip(ld * (0.18 + 0.12 * V.yk), 5.0, 16.0))
    x0, x1 = lx0 + 1.2, lx1 - 1.2
    z1 = street - front; z0 = lz0 + 1.0
    side = W >= 4 and kind in (0, 3)
    if side: x1 -= 7.0
    H = (9.0 if W * D < 12 else 11.0) * V.hk * (1.3 if kind == 3 else 1.0)
    hw = x1 - x0; cxm = (x0 + x1) / 2
    plinth(m, x0, x1, z0, z1, 0.4)
    mbox(m, "wall", x0, x1, 0.0, H, z0, z1, tm=p["wall_m"], skip=("top", "bot"))
    top = parapet(m, x0, x1, z0, z1, H, ht=0.7, tm=p["wall_m"])
    avoid = []
    if kind == 0:  # campus: glass band, glazed office cube on one side, optional fins, solar roof
        glass_band(m, x0, x1, z0, z1, H - 3.0, H - 1.4)
        ow = min(14.0, hw * 0.5) if hw > 10 else hw
        ox0 = x0 if V.a < 0.5 else x1 - ow
        oz1 = z1 + min(7.0, front - 3.5)
        oH, otop = office_block(m, p, ox0, ox0 + ow, z1, oz1, floors=3 if W * D >= 12 else 2, fh=3.6, glass=True, fins=V.b < 0.6, mat="wall2")
        entry(m, ox0 + 1.9, oz1)
        sign_z(m, ox0 + 0.6, ox0 + min(ow - 0.6, 7.0), oH + 0.05, oH + 0.6, oz1 + 0.7)
        flat(m, "paving", ox0 + 0.6, ox0 + 3.2, oz1, street, 0.05, 2.0)
        solar_rows(m, x0, x1, z0, z1, H); avoid = [(ox0 - 0.5, ox0 + ow + 0.5)]
        top = max(top, otop)
    elif kind == 1:  # lab: full-width low front wing with green roof, glass atrium tower, roof monitors, green hall roof
        wz1 = min(street - 3.5, z1 + 6.0); wH = 4.4
        mbox(m, "wall2", x0, x1, 0.0, wH, z1, wz1, tm=4.0, skip=("bot", "top"))
        glass_band(m, x0, x1, z1, wz1, 0.9, wH - 0.6, sides=False, step=2.0)
        parapet(m, x0, x1, z1, wz1, wH, ht=0.4, mat="wall2", tm=4.0)
        flat(m, "grass", x0 + 0.4, x1 - 0.4, z1 + 0.4, wz1 - 0.4, wH + 0.06, 3.0)
        aw = min(7.0, hw * 0.35); ax = cxm + (V.a - 0.5) * (hw - aw) * 0.6
        aH = H + 3.0 + 3.0 * V.c
        mbox(m, "wall2", ax - aw / 2, ax + aw / 2, 0.0, aH, wz1 - 4.0, wz1 + 0.6, tm=4.0, skip=("bot",))
        glass_band(m, ax - aw / 2, ax + aw / 2, wz1 - 4.0, wz1 + 0.6, 0.2, aH - 0.4, step=1.2)
        entry(m, ax, wz1 + 0.6)
        sign_z(m, ax - aw / 2 + 0.3, ax + aw / 2 - 0.3, aH - 1.1, aH - 0.4, wz1 + 0.6)
        flat(m, "paving", ax - 1.4, ax + 1.4, wz1, street, 0.05, 2.0)
        flat(m, "grass", x0 + 0.6, x1 - 0.6, z0 + 0.6, z1 - 0.6, H + 0.05, 3.0)
        for i in range(max(1, int((z1 - z0) // 9))):
            mz = z0 + 2.0 + i * 9.0
            if mz + 3 < z1 - 1: monitor(m, x0 + 2.0, x1 - 2.0, mz, mz + 3.0, H)
        face_x(m, "dock", z0 + 1.5, z0 + 4.6, 0.05, 3.6, x1, +1)
        top = max(top, aH); avoid = [(ax - aw / 2 - 1, ax + aw / 2 + 1)]
    elif kind == 2:  # data / clean-room: blind hall with louvre band, cooling-unit field on the roof, glass stair tower, generators
        nl = max(1, int(hw // 3.0))
        for zz, f in ((z1, +1), (z0, -1)):
            for i in range(nl):
                u0 = x0 + i * hw / nl; face_z(m, "louvre", u0 + 0.1, u0 + hw / nl - 0.1, H - 2.2, H - 0.8, zz, f)
        mbox(m, "brand1", x0 - 0.03, x1 + 0.03, 0.6, 1.0, z1, z1 + 0.03, k="metal", skip=("top", "bot", "back", "left", "right"))
        cols = max(1, int(hw // 3.4)); rows = max(1, int((z1 - z0 - 2) // 3.0))
        for i in range(cols):
            for j in range(rows):
                if (i + j + V.n) % 5 == 0: continue
                rtu(m, x0 + (i + 0.5) * hw / cols, z0 + 1.5 + (j + 0.5) * (z1 - z0 - 3) / rows, H + 0.02, w=2.6, d=2.0, h=1.4)
        tx = x0 + 2.5 if V.a < 0.5 else x1 - 2.5
        mbox(m, "wall2", tx - 2.2, tx + 2.2, 0.0, H + 3.0, z1, z1 + 4.0, tm=4.0, skip=("bot",))
        glass_band(m, tx - 2.2, tx + 2.2, z1, z1 + 4.0, 0.2, H + 2.6, step=1.1)
        entry(m, tx, z1 + 4.0, w=3.0)
        sign_z(m, cxm - min(5, hw * 0.3), cxm + min(5, hw * 0.3), H - 0.7, H + 0.5, z1)
        flat(m, "paving", tx - 1.2, tx + 1.2, z1 + 4.0, street, 0.05, 2.0)
        if front >= 7.0:  # generator enclosures along the front behind bollards
            gx = x0 + 5.5 if tx < cxm else x0 + 0.5
            for k in range(int(max(0, min(3, (hw - 6) // 6.6)))):
                ga = gx + k * 6.6
                mbox(m, "wall2", ga, ga + 6.0, 0.03, 2.8, z1 + 1.2, z1 + 3.6, tm=4.0)
                face_z(m, "louvre", ga + 0.5, ga + 5.5, 0.6, 2.4, z1 + 3.6, +1)
                cyl(m, "pipe", ga + 5.0, z1 + 2.4, 0.2, 2.8, 4.6, n=6, top="soot")
            bollards(m, x0, x1, z1 + 4.6, max(2, int(hw // 2.5)))
        top = max(top, H + 3.8); avoid = [(tx - 2.5, tx + 2.5)]
    else:  # robotic logistics: tall hall, vertical glass slots, roof monitors, office bridge on columns
        ns = max(2, int(hw // 6.0))
        for i in range(ns):
            sx_ = x0 + (i + 0.5) * hw / ns
            face_z(m, "curtain", sx_ - 0.7, sx_ + 0.7, 0.6, H - 0.6, z1, +1, off=0.02)
            mbox(m, "brand2", sx_ - 0.9, sx_ + 0.9, H - 0.6, H + 0.75, z1, z1 + 0.15, k="metal", skip=("bot",))
        for i in range(max(1, int((z1 - z0) // 8))):
            mz = z0 + 1.5 + i * 8.0
            if mz + 3 < z1 - 1: monitor(m, x0 + 1.5, x1 - 1.5, mz, mz + 2.6, H, h=1.3)
        bw = min(16.0, hw * 0.6); bx0 = x0 + (hw - bw) * V.a; by = 4.2
        bz1 = min(street - 3.0, z1 + 5.0)
        mbox(m, "wall2", bx0, bx0 + bw, by, by + 3.8, z1, bz1, tm=4.0)
        glass_band(m, bx0, bx0 + bw, z1, bz1, by + 0.4, by + 3.4, step=1.4)
        for xx in (bx0 + 0.6, bx0 + bw - 0.6):
            mbox(m, "trim", xx - 0.25, xx + 0.25, 0.0, by, bz1 - 0.8, bz1 - 0.3, tm=1.0, skip=("top",))
        mbox(m, "wall2", bx0 + 0.4, bx0 + 2.8, 0.0, by, z1, z1 + 2.4, tm=4.0, skip=("top",))
        face_z(m, "gdoor", bx0 + 0.7, bx0 + 2.5, 0.05, 2.6, z1 + 2.4, off=0.04)
        sign_z(m, bx0 + 0.6, bx0 + bw - 0.6, by + 3.85, by + 4.5, bz1 - 0.3)
        flat(m, "paving", bx0 + 0.4, bx0 + 2.8, z1 + 2.4, street, 0.05, 2.0)
        top = max(top, by + 4.5, H + 2.0); avoid = [(bx0 - 1, bx0 + bw + 1)]
    # service side: truck yard with docks (no vehicles baked in), else a side roll-up
    if side:
        yx0, yx1 = x1 + 0.5, lx1 - 0.8
        flat(m, "ground", yx0, yx1, z0, street, 0.05, 6.0)
        n = max(1, min(4, int((z1 - z0 - 2) // 4.0)))
        for i in range(n):
            dz = z0 + 1.5 + (i + 0.5) * (z1 - z0 - 3) / n
            face_x(m, "dock", dz - 1.55, dz + 1.55, 0.05, 3.6, x1, +1)
            dflat(m, "yellow", yx0 + 0.2, yx1 - 0.2, dz - 1.75, dz - 1.6, 0.06)
        mbox(m, "trim", x1, x1 + 2.0, 4.2, 4.4, z0 + 1.0, z1 - 1.0, tm=1.0)
        driveway(m, yx0, yx1, street, z1)
        if yx1 - yx0 > 3 and V.c < 0.6: container(m, (yx0 + yx1) / 2, z0 + 4.0, rot=True, kind=V.cont)
    elif kind != 1:
        face_x(m, "rollup", z0 + 1.0, z0 + 4.8, 0.05, 4.5, x1 if V.a >= 0.5 else x0, +1 if V.a >= 0.5 else -1)
    if kind in (0, 3) and front >= 7.0:  # staff car park, empty (cars are agents)
        px0 = max([b for a, b in avoid if b < x1] + [x0]) + 0.5
        if px0 + 5 < x1:
            stalls(m, px0, x1, street - 5.4, street - 0.4); driveway(m, px0, px0 + 5.0, street, street - 5.4)
    landscape(m, lw, lx0, lx1, street, front, avoid)
    lamp(m, x0 + 3.4 if V.a >= 0.5 else x1 - 3.4, street - 0.6, 6.0)
    return m, top


BUILDERS = {"workshop": build_workshop, "brickworks": build_brickworks, "plant": build_plant, "process": build_process, "hitech": build_hitech}


def build(W, D, level, v=0, p=None, mir=None):
    """v picks the design (seed knobs; for L5 the archetype v % 4), mir mirrors left-right (default: odd v)."""
    set_variant(W, D, level, v)
    p = p or LEVELS[level]
    m, top = BUILDERS[p["style"]](W, D, p)
    if (v % 2 if mir is None else mir): mirror(m)
    return m, top


def unique_pick(i):
    """Default set: one model per lot, and neighbouring lot sizes never share a design.
    Lot index i -> (design, palette, mirrored). Design cycles every lot, palette is offset so a design never
    repeats with the same colours, mirroring flips every other pair."""
    return i % 4, (i + i // 4) % 4, (i // 2) % 2


def write_preview():
    html = """<!doctype html>
<meta charset="utf-8"><title>Industry</title>
<style>body{margin:0;background:#8aa0b4;font:12px sans-serif;overflow:hidden}#ui{position:fixed;top:6px;left:6px;background:#000a;color:#fff;padding:6px;border-radius:4px;max-width:96vw}button{margin:1px}</style>
<div id="ui"></div>
<script type="importmap">{"imports":{"three":"https://unpkg.com/three@0.160.0/build/three.module.js","three/addons/":"https://unpkg.com/three@0.160.0/examples/jsm/"}}</script>
<script type="module">
import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
const r=new THREE.WebGLRenderer({antialias:true,preserveDrawingBuffer:true});r.setSize(innerWidth,innerHeight);document.body.appendChild(r.domElement);r.shadowMap.enabled=true;r.toneMapping=THREE.ACESFilmicToneMapping;
const s=new THREE.Scene();s.background=new THREE.Color(0x8aa0b4);
const cam=new THREE.PerspectiveCamera(40,innerWidth/innerHeight,0.1,800);
const oc=new OrbitControls(cam,r.domElement);
s.add(new THREE.HemisphereLight(0xdfe8ff,0x6b6a58,0.9));
const sun=new THREE.DirectionalLight(0xfff0d8,2.4);sun.position.set(-24,40,30);sun.castShadow=true;Object.assign(sun.shadow.camera,{left:-60,right:60,top:60,bottom:-60});sun.shadow.mapSize.set(2048,2048);sun.shadow.bias=-0.0004;s.add(sun);
const ground=new THREE.Mesh(new THREE.PlaneGeometry(400,400),new THREE.MeshStandardMaterial({color:0x6d7f52}));ground.rotation.x=-Math.PI/2;ground.position.y=-0.01;ground.receiveShadow=true;s.add(ground);
const cat=await (await fetch('catalog.json?'+Date.now())).json();const ui=document.getElementById('ui');
const lots=[...new Set(cat.map(c=>c.lot.join('x')))];let [lot,lv,vv]=(location.hash.slice(1)||'4x4_5_0').split('_');lv=+lv;vv=+(vv||0);
const loader=new GLTFLoader();let cur=null;
function frame(e){const R=Math.max(e.lot_m[0],e.lot_m[1],e.height_m)*1.25;cam.position.set(R*0.8,R*0.62,R*1.05);oc.target.set(0,Math.min(6,e.height_m*0.3),0);}
function load(){const e=cat.find(c=>c.lot.join('x')==lot&&c.level==lv&&(!c.file.includes('_v')||c.variant==vv));if(cur)s.remove(cur);frame(e);
 loader.load('glb/'+e.file+'?t='+Date.now(),g=>{cur=g.scene;cur.traverse(o=>{if(o.isMesh){o.castShadow=o.receiveShadow=true;if(o.material.map)o.material.map.anisotropy=8;}});s.add(cur);
 ui.innerHTML='Lot: '+lots.map(l=>`<button ${l==lot?'disabled':''} data-lot="${l}">${l}</button>`).join('')+'<br>'+[1,2,3,4,5].map(l=>`<button ${l==lv?'disabled':''} data-l="${l}">L${l}</button>`).join('')+' '+[0,1,2,3].map(v=>`<button ${v==vv?'disabled':''} data-v="${v}">v${v}</button>`).join('')+` <b>${e.style}</b> | ${e.lot_m.join('x')} m | ${e.height_m} m | ${e.workers} workers | ${e.tris} tris`;});}
ui.onclick=e=>{const b=e.target.dataset;if(b.lot)lot=b.lot;if(b.l)lv=+b.l;if(b.v)vv=+b.v;location.hash=lot+'_'+lv+'_'+vv;load();};load();
r.setAnimationLoop(()=>{const w=innerWidth,h=innerHeight;if(r.domElement.width!==w||r.domElement.height!==h){r.setSize(w,h);cam.aspect=w/h;cam.updateProjectionMatrix();}oc.update();r.render(s,cam)});
</script>
"""
    open(os.path.join(BASE, "preview.html"), "w", encoding="utf-8").write(html)


def write_grid():
    """All 60 models on one sheet: rows = levels, columns = lots."""
    html = """<!doctype html>
<meta charset="utf-8"><title>Industry grid</title>
<style>body{margin:0;background:#8aa0b4;overflow:hidden;font:12px sans-serif}#l{position:fixed;left:8px;top:6px;color:#fff;background:#0008;padding:4px 8px;border-radius:4px}</style>
<div id="l">Industry: rows L1-L5, columns 2x2 .. 6x6</div>
<script type="importmap">{"imports":{"three":"https://unpkg.com/three@0.160.0/build/three.module.js","three/addons/":"https://unpkg.com/three@0.160.0/examples/jsm/"}}</script>
<script type="module">
import * as THREE from 'three';import {OrbitControls} from 'three/addons/controls/OrbitControls.js';import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
const r=new THREE.WebGLRenderer({antialias:true,preserveDrawingBuffer:true});r.setSize(innerWidth,innerHeight);r.shadowMap.enabled=true;r.toneMapping=THREE.ACESFilmicToneMapping;document.body.appendChild(r.domElement);
const s=new THREE.Scene();s.background=new THREE.Color(0x8aa0b4);
const cam=new THREE.PerspectiveCamera(35,innerWidth/innerHeight,1,3000);cam.position.set(330,330,420);const oc=new OrbitControls(cam,r.domElement);oc.target.set(230,0,120);
s.add(new THREE.HemisphereLight(0xdfe8ff,0x6b6a58,0.9));const sun=new THREE.DirectionalLight(0xfff0d8,2.4);sun.position.set(-150,300,200);sun.castShadow=true;
Object.assign(sun.shadow.camera,{left:-500,right:500,top:500,bottom:-500,far:1500});sun.shadow.mapSize.set(4096,4096);sun.shadow.bias=-0.0005;sun.target.position.set(230,0,120);s.add(sun,sun.target);
const gnd=new THREE.Mesh(new THREE.PlaneGeometry(3000,3000),new THREE.MeshStandardMaterial({color:0x6d7f52}));gnd.rotation.x=-Math.PI/2;gnd.receiveShadow=true;s.add(gnd);
let cat=await (await fetch('catalog.json')).json();const L=new GLTFLoader();const qL=+new URLSearchParams(location.search).get('level');if(qL)cat=cat.filter(e=>e.level===qL);document.getElementById('l').textContent=qL?`Industry L${qL}: rows = variants v0-v3`:(cat.some(e=>e.file.includes('_v'))?'Industry: rows = L1v0..L5v3':'Industry: rows L1-L5, columns 2x2 .. 6x6');
const lots=[...new Set(cat.map(c=>c.lot.join('x')))];let xs={},x=0;for(const l of lots){const [w]=l.split('x').map(Number);xs[l]=x+w*4;x+=w*8+8;}
for(const e of cat){L.load('glb/'+e.file,g=>{g.scene.traverse(o=>{if(o.isMesh)o.castShadow=o.receiveShadow=true});const VV=e.file.includes('_v');g.scene.position.set(xs[e.lot.join('x')],0,(VV?(qL?e.variant:(e.level-1)*4+e.variant):e.level-1)*62);s.add(g.scene);});}
r.setAnimationLoop(()=>{oc.update();r.render(s,cam)});
</script>
"""
    open(os.path.join(BASE, "grid.html"), "w", encoding="utf-8").write(html)


ALL_VARIANTS = False


def main():
    global ALL_VARIANTS, BASE, OUT, TEX
    args = sys.argv[1:]
    if "--all-variants" in args:  # the 4-variants-per-lot set, kept as a backup folder
        ALL_VARIANTS = True; args.remove("--all-variants")
        BASE = os.path.join(ROOT, "assets", "buildings", "industry_variants_backup")
        OUT, TEX = (os.path.join(BASE, d) for d in ("glb", "tex"))
    for d in (OUT, TEX): os.makedirs(d, exist_ok=True)
    c.TEX = TEX
    c.MATS = MATS
    levels = [int(a) for a in args] or sorted(LEVELS)
    shared = {"ground": c.save_tex("IN_asphalt", tile_maps(f"{RT}/asphalt_worn_albedo.png", 3.0)),
              "paving": c.save_tex("IN_concrete", tile_maps(f"{RT}/concrete_albedo.png", 4.0)),
              "grass": c.save_tex("IN_grass", tile_maps(f"{RT}/grass_albedo.png", 4.0))}
    shared["brand1"] = shared["brand2"] = c.save_tex("IN_brand", c.brand_sheet())
    roofs = {"gravel": c.save_tex("IN_gravel", tile_maps(f"{RT}/gravel_albedo.png", 4.0)),
             "membrane": c.save_tex("IN_membrane", membrane())}
    catpath = os.path.join(BASE, "catalog.json")
    catalog = [e for e in (json.load(open(catpath)) if os.path.exists(catpath) else []) if e["level"] not in levels and "palette" in e]
    for lv in levels:
        base = LEVELS[lv]
        trim = c.save_tex(f"IN_L{lv}_trim", o.trim_sheet(base["trim"]))
        detail = c.save_tex(f"IN_L{lv}_detail", detail_sheet(base))
        texs, pals = {}, {}
        for v in range(VARIANTS):
            p = pals[v] = dict(base, **PALETTES[lv][v])
            roof = roofs.get(p["roof"]) or c.save_tex(f"IN_L{lv}v{v}_roof", corr_roof(p["roof_c"]))
            texs[v] = dict(shared, wall=c.save_tex(f"IN_L{lv}v{v}_wall", wall_maps("wall", p, "wall")),
                           wall2=c.save_tex(f"IN_L{lv}v{v}_wall2", wall_maps("wall2", p, "wall2")),
                           roof=roof, trim=trim, detail=detail)
        jobs = ([(W, D, v, v, None, f"IN_{W}x{D}_L{lv}_v{v}") for v in range(VARIANTS) for (W, D) in LOTS] if ALL_VARIANTS else
                [(W, D, *unique_pick(i), f"IN_{W}x{D}_L{lv}") for i, (W, D) in enumerate(LOTS)])
        for (W, D, v, pal, mir, name) in jobs:
                p, tex = pals[pal], texs[pal]
                mesh, h = build(W, D, lv, v, p, mir)
                c.write_glb(os.path.join(OUT, name + ".glb"), mesh, tex, {"brand1": p["brand"], "brand2": p["brand2"]})
                catalog.append(dict(file=name + ".glb", lot=[W, D], lot_m=[W * CELL, D * CELL], level=lv, variant=v, palette=pal, style=p["style"],
                                    zone="industrial", height_m=round(h, 1), tris=mesh.tri_count(),
                                    workers=int(W * D * p["workers"] * 0.5), pollution=p["pollution"],
                                    colors={"brand1": list(p["brand"]), "brand2": list(p["brand2"])}))
        rows = [e for e in catalog if e["level"] == lv]
        print(f"level {lv} {base['style']}: {len(rows)} factories, tris {min(e['tris'] for e in rows)}-{max(e['tris'] for e in rows)}")
    catalog.sort(key=lambda e: (e["level"], e["lot"], e.get("variant", 0)))
    json.dump(catalog, open(catpath, "w"), indent=1)
    write_preview(); write_grid()
    print(f"total {len(catalog)} GLBs -> {OUT}")


if __name__ == "__main__":
    main()
