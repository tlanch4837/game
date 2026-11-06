"""Low-density commercial generator, matched to Cities: Skylines II -- every business type on every lot size it fits.

CS2 low-density business spawns on 18 lot sizes (width along the road x depth, 8 m cells):
  2x2 2x3 2x4 2x5 2x6 / 3x2 3x3 3x4 3x5 3x6 / 4x3 4x4 4x5 4x6 / 5x5 5x6 / 6x4 6x6
Its shops each sell one CS2 commercial resource (17) plus a mixed strip plaza = 18 business types. Every type is built on
every lot it fits (min lot per type below) at CS2's 5 levels, with parking sized to the lot:
  * lots >= 4 cells deep: front car park, 1 or 2 rows of 2.5 x 5 m bays with a 6 m aisle, building behind
  * shallow lots >= 3 cells wide: side car park (one 8 m cell strip, bays + aisle running back from the street)
  * 2-wide shallow lots: no on-lot parking (street parking), like CS2's small corner shops
  * gas / dealer / motel have their own forecourts. All bays are left empty (EMPTY_LOTS).

Textures are shared files, referenced from the GLBs by relative uri (src/mesh_import.h loads them), so 1000+ models
stay small:  assets/buildings/low_commercial/tex/*.jpg|png
  walls  - existing game art: assets/buildings/{row_housing,low_density}/src_tex/*_wall.jpg
  roofs  - assets/roads/tex/gravel|concrete_albedo.png,  car parks - asphalt,  sidewalks/patios - pavers
  detail - procedural atlas per level: storefronts, windows, pumps, paint
  brand  - CL_brand: white awning + blank sign sheet on two materials, brand1 / brand2, whose baseColorFactor is the
           business colour pair (defaults per type in TYPES / catalog.json; override per instance in the game)
Override a level's wall/roof with assets/buildings/low_commercial/src_tex/CL_L<level>_wall.jpg / _roof.jpg.

Run:  python gen_low_commercial.py [level ...]
Output: assets/buildings/low_commercial/glb/CL_<W>x<D>_<type>_L<level>.glb  + catalog.json   Units: m, +Y up, front +Z.
"""
import json, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import gen_low_density as g
from gen_low_density import Mesh, box, tile_maps, normal_from_height, jpg, png, CELL

ROOT = g.ROOT
BASE = os.path.join(ROOT, "assets", "buildings", "low_commercial")
SRC, OUT, TEX = (os.path.join(BASE, d) for d in ("src_tex", "glb", "tex"))
B = os.path.join(ROOT, "assets", "buildings"); RT = os.path.join(ROOT, "assets", "roads", "tex")
EMPTY_LOTS = True  # no baked cars: bays and pumps stay free for the game's traffic

LOTS = [(2, 2), (2, 3), (2, 4), (2, 5), (2, 6), (3, 2), (3, 3), (3, 4), (3, 5), (3, 6),
        (4, 3), (4, 4), (4, 5), (4, 6), (5, 5), (5, 6), (6, 4), (6, 6)]

# key: (CS2 resource, name, archetype, min (W, D), brand colour, extras)
TYPES = {
    "convenience": dict(res="Convenience Food", name="convenience store",       arch="shop",   min=(2, 2), brand=(214, 40, 40), brand2=(250, 210, 40)),
    "liquor":      dict(res="Beverages",        name="liquor & drinks store",   arch="shop",   min=(2, 2), brand=(110, 30, 60), brand2=(220, 190, 120)),
    "pharmacy":    dict(res="Pharmaceuticals",  name="pharmacy",                arch="shop",   min=(2, 2), brand=(30, 140, 80), brand2=(245, 245, 240)),
    "books":       dict(res="Paper",            name="book & stationery shop",  arch="shop",   min=(2, 2), brand=(40, 60, 120), brand2=(230, 200, 120)),
    "bar":         dict(res="Entertainment",    name="bar / pub",               arch="shop",   min=(2, 2), brand=(60, 36, 24), brand2=(200, 150, 60), patio=True),
    "media":       dict(res="Media",            name="media & phone store",     arch="shop",   min=(2, 2), brand=(20, 110, 180), brand2=(240, 240, 240)),
    "restaurant":  dict(res="Meals",            name="restaurant / diner",      arch="shop",   min=(2, 2), brand=(200, 120, 30), brand2=(120, 30, 30), patio=True),
    "boutique":    dict(res="Textiles",         name="clothing boutique",       arch="shop",   min=(2, 2), brand=(30, 30, 34), brand2=(220, 200, 170)),
    "travel":      dict(res="Recreation",       name="travel agency & sports",  arch="shop",   min=(2, 2), brand=(20, 150, 150), brand2=(250, 240, 200)),
    "hardware":    dict(res="Chemicals",        name="hardware & garden store", arch="bigbox", min=(3, 4), brand=(230, 120, 20), brand2=(40, 40, 44)),
    "homeware":    dict(res="Plastics",         name="toy & homeware store",    arch="bigbox", min=(3, 4), brand=(40, 150, 200), brand2=(250, 220, 60)),
    "electronics": dict(res="Electronics",      name="electronics store",       arch="bigbox", min=(3, 4), brand=(30, 70, 170), brand2=(240, 240, 240)),
    "supermarket": dict(res="Food",             name="supermarket",             arch="bigbox", min=(4, 4), brand=(40, 130, 60), brand2=(250, 240, 210)),
    "furniture":   dict(res="Furniture",        name="furniture store",         arch="bigbox", min=(4, 5), brand=(150, 100, 50), brand2=(40, 60, 90)),
    "motel":       dict(res="Lodging",          name="motel",                   arch="motel",  min=(4, 4), brand=(190, 40, 120), brand2=(250, 230, 120)),
    "dealer":      dict(res="Vehicles",         name="car dealership",          arch="dealer", min=(4, 4), brand=(160, 20, 20), brand2=(230, 230, 230)),
    "gas":         dict(res="Petrochemicals",   name="gas station",             arch="gas",    min=(4, 3), brand=(240, 190, 20), brand2=(30, 90, 160)),
    "plaza":       dict(res="mixed",            name="strip plaza",             arch="plaza",  min=(4, 4), brand=(80, 80, 90), brand2=(200, 60, 50)),
}

LEVELS = {  # CS2 levels 1..5: same lot, better building. wall/roof from existing game art.
    1: dict(wall=f"{B}/row_housing/src_tex/RH_L1_wall.jpg", roof=f"{RT}/gravel_albedo.png", wall_m=2.4, wall_k=8,
            sh=3.6, up=3.0, parapet=0.5, upper=0, frame=(70, 72, 76), glass=(40, 52, 60), awn2=(232, 228, 218)),
    2: dict(wall=f"{B}/row_housing/src_tex/RH_L2_wall.jpg", roof=f"{RT}/gravel_albedo.png", wall_m=2.0, wall_k=8,
            sh=3.8, up=3.0, parapet=0.7, upper=0, frame=(60, 60, 64), glass=(36, 48, 58), awn2=(240, 236, 226)),
    3: dict(wall=f"{B}/low_density/src_tex/L3_wall.jpg", roof=f"{RT}/concrete_albedo.png", wall_m=2.0, wall_k=8,
            sh=4.0, up=3.2, parapet=0.9, upper=1, frame=(36, 54, 46), glass=(34, 46, 56), awn2=None),
    4: dict(wall=f"{B}/row_housing/src_tex/RH_L4_wall.jpg", roof=f"{RT}/concrete_albedo.png", wall_m=3.0, wall_k=7,
            sh=4.2, up=3.3, parapet=0.9, upper=1, frame=(40, 34, 30), glass=(30, 42, 54), awn2=None),
    5: dict(wall=f"{B}/low_density/src_tex/L5_wall.jpg", roof=f"{RT}/concrete_albedo.png", wall_m=3.2, wall_k=5,
            sh=4.4, up=3.4, parapet=0.5, upper=1, frame=(28, 30, 34), glass=(52, 76, 94), awn2=None, modern=True),
}

BAY_W, BAY_D, AISLE = 2.5, 5.0, 6.0
SIDEWALK = 2.0


def fits(key, W, D):
    mw, md = TYPES[key]["min"]
    return W >= mw and D >= md


# ------------------------------------------------------------------ detail atlas
ATLAS = {"shop": (0, 0, 256, 256), "win": (0, 256, 256, 512),
         "curtain": (256, 256, 384, 384), "car": (384, 256, 512, 384), "pump": (256, 384, 384, 512), "paint": (392, 392, 504, 504),
         # brand sheet cells (white, tinted by the brand1/brand2 material colour)
         "awning": (0, 0, 512, 256), "sign": (0, 256, 512, 512)}
BRAND_MATS = ("brand1", "brand2")


def srgb_to_linear(c):
    c = np.asarray(c, np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4).tolist()


def brand_sheet():
    """White 512 sheet for the two tintable brand materials: top half fabric awning, bottom half a blank sign panel."""
    S = 512
    col = Image.new("RGB", (S, S), (245, 245, 245)); hgt = Image.new("L", (S, S), 128)
    dc, dh = ImageDraw.Draw(col), ImageDraw.Draw(hgt)
    for i in range(32):  # canvas folds
        dh.rectangle([i * 16, 0, i * 16 + 7, 255], fill=150)
    dc.rectangle([0, 224, 511, 255], fill=(215, 215, 215)); dh.rectangle([0, 224, 511, 255], fill=110)  # hem
    dc.rectangle([0, 256, 511, 511], fill=(232, 232, 232)); dh.rectangle([0, 256, 511, 511], fill=200)  # raised frame
    dc.rectangle([18, 274, 493, 493], fill=(248, 248, 248)); dh.rectangle([18, 274, 493, 493], fill=150)  # empty face
    rgb = np.asarray(col).astype(np.float32)
    rgb = np.clip(rgb * (0.96 + 0.06 * np.random.default_rng(5).random((S, S, 1))), 0, 255).astype(np.uint8)
    h = np.asarray(hgt.filter(ImageFilter.GaussianBlur(1.5)), np.float32) / 255.0
    return jpg(rgb), png(normal_from_height(h, 4.0))


def detail_sheet(p, brand=(128, 128, 128)):
    S = 512
    col = Image.new("RGB", (S, S), p["frame"]); hgt = Image.new("L", (S, S), 128)
    dc, dh = ImageDraw.Draw(col), ImageDraw.Draw(hgt)
    R = lambda bx, c, h: (dc.rectangle(bx, fill=c), dh.rectangle(bx, fill=h))
    gl = p["glass"]; lite = tuple(min(255, c + 22) for c in gl)
    R([0, 0, 255, 255], p["frame"], 200)
    R([10, 40, 96, 228], gl, 40); R([160, 40, 246, 228], gl, 40); R([106, 40, 150, 250], lite, 50)
    R([10, 8, 246, 30], lite, 45); R([10, 232, 96, 250], p["frame"], 170); R([160, 232, 246, 250], p["frame"], 170)
    R([140, 140, 145, 160], (200, 200, 200), 240)
    R([0, 256, 255, 511], p["frame"], 160)
    R([20, 276, 236, 492], p["frame"], 205); R([36, 292, 220, 476], gl, 40); R([125, 292, 131, 476], p["frame"], 190)
    R([256, 256, 383, 383], p["frame"], 200)
    for i in range(4):
        for j in range(4):
            x0, y0 = 259 + i * 31, 259 + j * 31
            R([x0, y0, x0 + 27, y0 + 27], tuple(min(255, c + (i + j) % 3 * 6) for c in gl), 40)
    R([384, 256, 511, 383], (150, 30, 30), 180); R([396, 270, 500, 300], (30, 36, 44), 90)
    R([392, 350, 420, 380], (20, 20, 20), 120); R([476, 350, 504, 380], (20, 20, 20), 120)
    R([256, 384, 383, 511], (230, 230, 226), 190); R([270, 400, 370, 440], (20, 30, 34), 80)
    R([256, 384, 383, 396], (120, 124, 130), 200); R([300, 456, 340, 500], (40, 40, 40), 150)
    R([384, 384, 511, 511], (236, 234, 226), 140)
    rgb = np.asarray(col).astype(np.float32)
    rgb = np.clip(rgb * (0.95 + 0.1 * np.random.default_rng(11).random((S, S, 1))), 0, 255).astype(np.uint8)
    h = np.asarray(hgt.filter(ImageFilter.GaussianBlur(1.2)), np.float32) / 255.0
    return jpg(rgb), png(normal_from_height(h, 5.0))


def auv(name, u, v, inset=0.01):
    x0, y0, x1, y1 = ATLAS[name]
    return ((x0 + (x1 - x0) * (inset + u * (1 - 2 * inset))) / 512.0, (y0 + (y1 - y0) * (inset + (1 - v) * (1 - 2 * inset))) / 512.0)


FULL = lambda k: [auv(k, 0, 0), auv(k, 1, 0), auv(k, 1, 1), auv(k, 0, 1)]


# ------------------------------------------------------------------ mesh helpers
MATS = ("wall", "roof", "detail", "ground", "paving", "brand1", "brand2", "roomglass")
# the glass in each atlas cell, as (x0, x1, y0, y1) fractions of the cell (y down from its top): these become roomglass panes
GLASS_RECTS = {"shop": [(10 / 256, 96 / 256, 40 / 256, 228 / 256), (160 / 256, 246 / 256, 40 / 256, 228 / 256), (106 / 256, 150 / 256, 40 / 256, 250 / 256)],
               "win": [(36 / 256, 220 / 256, 36 / 256, 220 / 256)], "curtain": [(3 / 128, 123 / 128, 3 / 128, 123 / 128)]}


def new_mesh():
    m = Mesh(); m.prims = {k: [] for k in MATS}; return m


def glass_over(m, kind, p_ll, right, w, h, normal, rects=None):
    """Roomglass panes over the glass of a decal (see roomglass.py): a parallax room behind smooth, scratched glass."""
    rects = rects if rects is not None else GLASS_RECTS.get(kind)
    if rects and m.prims.get("roomglass") is not None:
        import roomglass
        roomglass.panes_in_cell(m, rects, p_ll, right, w, h, 0.025, normal, seed=float(abs(p_ll[0] * 3.1 + p_ll[1] * 7.7 + p_ll[2] * 1.3)))


def fq(m, kind, x0, x1, y0, y1, z, f=+1, off=0.03, mat="detail"):
    zz = z + off * f
    pts = [(x0, y0, zz), (x1, y0, zz), (x1, y1, zz), (x0, y1, zz)] if f > 0 else [(x1, y0, zz), (x0, y0, zz), (x0, y1, zz), (x1, y1, zz)]
    m.quad(mat, *pts, FULL(kind))
    if mat == "detail": glass_over(m, kind, pts[0], (f, 0, 0), x1 - x0, y1 - y0, (0, 0, f))


def sq(m, kind, z0, z1, y0, y1, x, f, off=0.03, mat="detail"):
    xx = x + off * f
    pts = [(xx, y0, z1), (xx, y0, z0), (xx, y1, z0), (xx, y1, z1)] if f > 0 else [(xx, y0, z0), (xx, y0, z1), (xx, y1, z1), (xx, y1, z0)]
    m.quad(mat, *pts, FULL(kind))
    if mat == "detail": glass_over(m, kind, pts[0], (0, 0, -f), z1 - z0, y1 - y0, (f, 0, 0))


def sign(m, x0, x1, y0, y1, z, mat="brand2"):
    """Blank sign panel in a brand colour (no lettering)."""
    fq(m, "sign", x0, x1, y0, y1, z, mat=mat)


def awning(m, a, b, z1, ay, striped):
    """Sloped awning 1.4 m deep, 0.6 m drop. Striped: alternating brand1/brand2 strips; else solid brand1."""
    zo = z1 + 1.4
    n = max(1, int((b - a) / 0.9)) if striped else 1
    for i in range(n):
        s0, s1 = a + (b - a) * i / n, a + (b - a) * (i + 1) / n
        mat = "brand1" if i % 2 == 0 else "brand2"
        u0, u1 = i / n, (i + 1) / n
        m.quad(mat, (s0, ay - 0.6, zo), (s1, ay - 0.6, zo), (s1, ay, z1 + 0.05), (s0, ay, z1 + 0.05),
               [auv("awning", u0, 0), auv("awning", u1, 0), auv("awning", u1, 1), auv("awning", u0, 1)])
        m.quad(mat, (s0, ay, z1 + 0.05), (s1, ay, z1 + 0.05), (s1, ay - 0.6, zo), (s0, ay - 0.6, zo),
               [auv("awning", u0, 1), auv("awning", u1, 1), auv("awning", u1, 0), auv("awning", u0, 0)])


def flat(m, mat, x0, x1, z0, z1, y, tile=4.0):
    if x1 - x0 < 0.05 or z1 - z0 < 0.05: return
    m.quad(mat, (x0, y, z1), (x1, y, z1), (x1, y, z0), (x0, y, z0),
           [(x0 / tile, z1 / tile), (x1 / tile, z1 / tile), (x1 / tile, z0 / tile), (x0 / tile, z0 / tile)])


def paint(m, x0, x1, z0, z1, y=0.05):
    m.quad("detail", (x0, y, z1), (x1, y, z1), (x1, y, z0), (x0, y, z0), FULL("paint"))


def dbox(m, kind, x0, x1, y0, y1, z0, z1):
    fq(m, kind, x0, x1, y0, y1, z1, +1, 0); fq(m, kind, x0, x1, y0, y1, z0, -1, 0)
    sq(m, kind, z0, z1, y0, y1, x1, +1, 0); sq(m, kind, z0, z1, y0, y1, x0, -1, 0)
    m.quad("detail", (x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0), FULL(kind))


def shell(m, p, x0, x1, z0, z1, H, rm=4.0):
    box(m, x0, x1, 0, H, z0, z1, p["wall_m"], top="roof", roof_m=rm)
    pt, ph = 0.25, p["parapet"]
    for a, b, c, d in ((x0, x1, z1 - pt, z1), (x0, x1, z0, z0 + pt), (x0, x0 + pt, z0 + pt, z1 - pt), (x1 - pt, x1, z0 + pt, z1 - pt)):
        box(m, a, b, H, H + ph, c, d, p["wall_m"], top="roof", roof_m=rm)
    return H + ph


def storefronts(m, p, x0, x1, z1, sh, units, base=0.15, modern=False):
    uw = (x1 - x0) / units
    for u in range(units):
        a, b = x0 + u * uw + 0.3, x0 + (u + 1) * uw - 0.3
        fq(m, "curtain" if modern else "shop", a, b, base, base + sh * 0.78, z1)
        sign(m, a + 0.2, b - 0.2, base + sh * 0.80, base + sh * 0.97, z1)
        if not modern:
            awning(m, a, b, z1, base + sh * 0.79, striped=p["awn2"] is not None)


def upper_windows(m, p, x0, x1, z0, z1, y0, floors, modern=False):
    for s in range(floors):
        yy = y0 + s * p["up"]
        n = max(1, int((x1 - x0) // 3.2))
        for i in range(n):
            cx = x0 + (i + 0.5) * (x1 - x0) / n; w = (x1 - x0) / n - 0.4 if modern else 1.5
            fq(m, "curtain" if modern else "win", cx - w / 2, cx + w / 2, yy + p["up"] * 0.25, yy + p["up"] * 0.85, z1)
            fq(m, "win", cx - 0.75, cx + 0.75, yy + p["up"] * 0.25, yy + p["up"] * 0.85, z0, -1)


def side_windows(m, p, x0, x1, z0, z1, H):
    n = max(1, int((z1 - z0) // 5.0))
    for i in range(n):
        zc = z0 + (i + 0.5) * (z1 - z0) / n
        for x, f in ((x1, +1), (x0, -1)):
            sq(m, "win", zc - 0.75, zc + 0.75, H * 0.35, min(H - 0.4, H * 0.35 + 1.6), x, f)


def car(m, x0, x1, z0, z1):
    if not EMPTY_LOTS: dbox(m, "car", x0, x1, 0.05, 1.45, z0, z1)


# ------------------------------------------------------------------ parking
def lot_x(m, x0, x1, z0, z1):
    """Car park whose bay rows run along X (bays face +/-Z). Fills [z0,z1] with rows of BAY_D + one AISLE between pairs.
    Returns number of bays."""
    flat(m, "ground", x0, x1, z0, z1, 0.04, 6.0)
    depth = z1 - z0
    rows = []  # z-ranges of bay rows
    z = z0 + 0.3
    while z + BAY_D <= z1 - 0.3:  # pattern: row, aisle, row, (gap), row, aisle, row ...
        rows.append((z, z + BAY_D)); z += BAY_D
        if z + AISLE + BAY_D <= z1 - 0.3:
            z += AISLE; rows.append((z, z + BAY_D)); z += BAY_D + 0.6
        else:
            break
    n_bays = 0
    for (a, b) in rows:
        n = int((x1 - x0 - 0.6) // BAY_W)
        sx = x0 + (x1 - x0 - n * BAY_W) / 2
        for i in range(n + 1):
            lx = sx + i * BAY_W; paint(m, lx - 0.06, lx + 0.06, a, b)
        paint(m, sx, sx + n * BAY_W, a - 0.06, a + 0.06) if a > z0 + 0.5 else None
        for i in range(n): car(m, sx + i * BAY_W + 0.4, sx + (i + 1) * BAY_W - 0.4, a + 0.4, b - 0.3)
        n_bays += n
    return n_bays


def lot_z(m, x0, x1, z0, z1):
    """Narrow car park whose bays line the side (rows along Z, bays face +/-X) with an aisle down the middle."""
    flat(m, "ground", x0, x1, z0, z1, 0.04, 6.0)
    w = x1 - x0
    cols = [(x0 + 0.2, x0 + 0.2 + BAY_D)] if w < 2 * BAY_D + AISLE else [(x0 + 0.2, x0 + 0.2 + BAY_D), (x1 - 0.2 - BAY_D, x1 - 0.2)]
    n_bays = 0
    for (a, b) in cols:
        n = int((z1 - z0 - 0.6) // BAY_W)
        for i in range(n + 1):
            lz = z0 + 0.3 + i * BAY_W; paint(m, a, b, lz - 0.06, lz + 0.06)
        n_bays += n
    return n_bays


# ------------------------------------------------------------------ archetypes
def frame_lot(W, D):
    lw, ld = W * CELL, D * CELL
    return lw, ld, -lw / 2, lw / 2, -ld / 2, ld / 2


def build_store(m, t, p, W, D, level, big=False, plaza=False):
    """Shop / big box / plaza on any lot: decide parking layout from the lot shape, then fill the rest with the building."""
    lw, ld, lx0, lx1, lz0, lz1 = frame_lot(W, D)
    modern = p.get("modern", False); base, sh, up = 0.15, p["sh"], p["up"]
    street = lz1 - SIDEWALK; back = 1.5
    min_d = 16.0 if big else 9.0
    usable = street - (lz0 + back)
    # front car park: as many double rows as leave the building at least its minimum depth (big boxes want 2)
    front = 0.0
    for need in ((2 * BAY_D + AISLE + 1.2) * 2 + 0.6, 2 * BAY_D + AISLE + 1.2, BAY_D + AISLE + 0.6):
        if usable - need >= min_d * (1.3 if big else 1.0):
            front = need; break
    side = 0.0
    if front == 0.0 and W >= 3:
        side = CELL  # one cell strip beside the shop
    side_gap = 0.5 if not (big or plaza) else 1.5
    x0, x1 = lx0 + side_gap + (side if side else 0), lx1 - side_gap
    z1 = street - front - (0.3 if front else 0.0)
    max_d = 30.0 if big else (18.0 if not plaza else 20.0)
    z0 = max(lz0 + back, z1 - max_d)
    floors = 0 if big else p["upper"]
    hb = sh + (1.6 if big else 0)
    H = base + hb + up * floors
    top = shell(m, p, x0, x1, z0, z1, H)
    units = max(2, int((x1 - x0) // 9)) if plaza else (2 if (not big and (x1 - x0) > 20) else 1)
    storefronts(m, p, x0, x1, z1, hb, units, modern=modern)
    if floors: upper_windows(m, p, x0, x1, z0, z1, base + hb, floors, modern)
    side_windows(m, p, x0, x1, z0, z1, H)
    fq(m, "shop", x0 + 1.0, x0 + 2.2, base, base + 2.3, z0, -1)
    if (x1 - x0) * (z1 - z0) > 150:
        n = max(1, int((x1 - x0) // 10))
        for i in range(n):
            ax = x0 + (i + 0.5) * (x1 - x0) / n
            box(m, ax - 0.8, ax + 0.8, H, H + 1.1, z0 + 3, z0 + 4.5, p["wall_m"] * 0.5, top="roof")
    bays = 0
    if front:
        bays = lot_x(m, lx0 + 0.3, lx1 - 0.3, z1 + 0.3, street - 0.2)
    else:
        if t.get("patio") and street - z1 > 0.5:
            flat(m, "paving", x0, x1, z1, street, 0.06, 2.0)
        elif street - z1 > 0.1:
            flat(m, "paving", x0, x1, z1, street, 0.06, 2.0)
        if t.get("patio"):  # tables on the sidewalk / patio
            for i in range(max(1, int((x1 - x0 - 2) // 3))):
                cx = x0 + 1.5 + i * 3
                box(m, cx - 0.4, cx + 0.4, 0.06, 0.78, z1 + 0.5, z1 + 1.3, 1.0, top="roof")
    if side:
        bays += lot_z(m, lx0 + 0.3, lx0 + side + 0.2, lz0 + 1.0, street - 0.2)
    # rear service yard
    flat(m, "paving", x0, x1, lz0 + 0.3, z0, 0.05, 2.0)
    return top, bays


def build_gas(m, t, p, W, D, level):
    lw, ld, lx0, lx1, lz0, lz1 = frame_lot(W, D)
    modern = p.get("modern", False); base, sh, up = 0.15, p["sh"], p["up"]
    street = lz1 - SIDEWALK
    kd = min(10.0, ld * 0.3)                    # kiosk depth
    kw = min(16.0, lw - 10.0)                   # kiosk width
    kx1 = lx1 - 2.0; kx0 = kx1 - kw; kz0 = lz0 + 1.5; kz1 = kz0 + kd
    flat(m, "ground", lx0 + 0.3, lx1 - 0.3, lz0 + 0.3, street - 0.2, 0.04, 6.0)
    H = base + sh + (up if level >= 4 else 0)
    top = shell(m, p, kx0, kx1, kz0, kz1, H)
    storefronts(m, p, kx0, kx1, kz1, sh, 1, modern=modern)
    # canopy over pump islands in the open forecourt
    cz0 = kz1 + 5.0; cz1 = min(street - 3.0, cz0 + 9.0)
    if cz1 - cz0 < 6.0: cz0 = cz1 - 6.0
    n = max(1, int((lw - 12.0) // 8.0)); cw = n * 8.0
    cx0 = lx0 + (lw - cw) / 2; cx1 = cx0 + cw; ch = 5.0
    box(m, cx0, cx1, ch, ch + 0.9, cz0, cz1, 1.0, top="roof")
    sign(m, cx0 + 0.2, cx1 - 0.2, ch + 0.05, ch + 0.85, cz1, mat="brand1")  # canopy fascia
    m.quad("detail", (cx0, ch - 0.01, cz0), (cx1, ch - 0.01, cz0), (cx1, ch - 0.01, cz1), (cx0, ch - 0.01, cz1), FULL("paint"))
    zc = (cz0 + cz1) / 2
    for i in range(n):
        ix = cx0 + 4.0 + i * 8.0
        box(m, ix - 0.25, ix + 0.25, 0.04, ch, zc - 0.25, zc + 0.25, 1.0, skip=("top",))
        box(m, ix - 2.5, ix + 2.5, 0.04, 0.25, zc - 0.6, zc + 0.6, 1.0, top="roof")
        for px in (ix - 1.4, ix + 1.4):
            dbox(m, "pump", px - 0.35, px + 0.35, 0.25, 1.9, zc - 0.3, zc + 0.3)
        for lz in (zc - 4.6, zc + 0.9):  # painted fuelling lanes either side of the island
            paint(m, ix - 3.6, ix - 3.48, lz, lz + 3.7); paint(m, ix + 3.48, ix + 3.6, lz, lz + 3.7)
    # customer bays beside the kiosk
    bays = 0
    if kx0 - (lx0 + 0.5) >= BAY_W * 2:
        nb = int((kx0 - lx0 - 1.0) // BAY_W)
        for i in range(nb + 1):
            lx = kx0 - 0.5 - i * BAY_W; paint(m, lx - 0.06, lx + 0.06, kz0, kz0 + BAY_D)
        bays = nb
    # price pylon at the street corner
    box(m, lx1 - 2.0, lx1 - 1.5, 0.04, 6.5, street - 0.6, street - 0.3, 1.0, top="roof")
    sq(m, "sign", street - 0.6, street - 0.3, 4.0, 6.4, lx1 - 1.5, +1, mat="brand2")
    sign(m, lx1 - 2.0, lx1 - 1.5, 4.0, 6.4, street - 0.3)
    return top, bays


def build_dealer(m, t, p, W, D, level):
    lw, ld, lx0, lx1, lz0, lz1 = frame_lot(W, D)
    base, sh, up = 0.15, p["sh"], p["up"]
    street = lz1 - SIDEWALK
    sd = min(16.0, (street - lz0) * 0.42)       # showroom depth
    front = street - lz0 - 1.5 - sd
    if front > 2 * BAY_D + AISLE + 1.2 and (street - lz0) > 40: front = 2 * BAY_D + AISLE + 1.2  # leave a rear stock lot
    x0, x1 = lx0 + 2.0, lx1 - 2.0; z1 = street - front; z0 = z1 - sd
    H = base + sh + 1.2 + (up if level >= 4 else 0)
    top = shell(m, p, x0, x1, z0, z1, H)
    n = max(2, int((x1 - x0) // 6))
    for i in range(n):
        a = x0 + i * (x1 - x0) / n + 0.2; b = x0 + (i + 1) * (x1 - x0) / n - 0.2
        fq(m, "curtain", a, b, base, H - 0.8, z1)
    sign(m, x0 + 2, x1 - 2, H - 0.75, H - 0.05, z1)
    side_windows(m, p, x0, x1, z0, z1, H)
    bays = lot_x(m, lx0 + 0.3, lx1 - 0.3, z1 + 0.3, street - 0.2)
    if z0 - lz0 > BAY_D + 1.0:
        bays += lot_x(m, lx0 + 0.3, lx1 - 0.3, lz0 + 0.3, z0 - 0.3)
    return top, bays


def build_motel(m, t, p, W, D, level):
    lw, ld, lx0, lx1, lz0, lz1 = frame_lot(W, D)
    modern = p.get("modern", False); base = 0.15
    street = lz1 - SIDEWALK
    floors = 1 if level <= 2 else 2
    H = base + 2.9 * floors + 0.2
    wing = 10.0
    bx0, bx1 = lx0 + 1.5, lx1 - 1.5; bz0 = lz0 + 1.5; bz1 = bz0 + wing
    top = shell(m, p, bx0, bx1, bz0, bz1, H)
    wx1 = bx0 + wing; wz1 = street - 6.0
    shell(m, p, bx0, wx1, bz1, wz1, H)
    for s in range(floors):
        y = base + s * 2.9
        for i in range(int((bx1 - wx1 - 1) // 4)):
            cx = wx1 + 2.5 + i * 4
            fq(m, "shop", cx - 0.5, cx + 0.5, y, y + 2.2, bz1); fq(m, "win", cx + 0.7, cx + 1.9, y + 0.9, y + 2.1, bz1)
        for i in range(int((wz1 - bz1 - 1) // 4)):
            cz = bz1 + 2.5 + i * 4
            sq(m, "shop", cz - 0.5, cz + 0.5, y, y + 2.2, wx1, +1); sq(m, "win", cz + 0.7, cz + 1.9, y + 0.9, y + 2.1, wx1, +1)
        if s > 0:
            box(m, wx1, bx1, y - 0.2, y, bz1, bz1 + 1.6, 1.0, top="roof")
            box(m, wx1, wx1 + 1.6, y - 0.2, y, bz1 + 1.6, wz1, 1.0, top="roof")
    shell(m, p, bx0, wx1, wz1, street - 0.5, base + 3.4)  # office at the street corner
    storefronts(m, p, bx0, wx1, street - 0.5, 3.4, 1, modern=modern)
    box(m, lx1 - 3.0, lx1 - 2.6, 0.04, 7.0, street - 1.0, street - 0.6, 1.0, top="roof")
    sign(m, lx1 - 5.5, lx1 - 0.2, 5.0, 7.4, street - 0.6)
    walk = 1.8 if floors > 1 else 0.3
    bays = lot_x(m, wx1 + walk, lx1 - 0.3, bz1 + walk, street - 1.2)  # court: rows face the room doors
    return top, bays


def build(W, D, key, level):
    t, p, m = TYPES[key], LEVELS[level], new_mesh()
    lw, ld, lx0, lx1, lz0, lz1 = frame_lot(W, D)
    flat(m, "paving", lx0 + 0.1, lx1 - 0.1, lz1 - SIDEWALK, lz1 - 0.1, 0.06, 2.0)  # sidewalk along the street
    a = t["arch"]
    if a == "gas": top, bays = build_gas(m, t, p, W, D, level)
    elif a == "dealer": top, bays = build_dealer(m, t, p, W, D, level)
    elif a == "motel": top, bays = build_motel(m, t, p, W, D, level)
    else: top, bays = build_store(m, t, p, W, D, level, big=(a == "bigbox"), plaza=(a == "plaza"))
    return m, top, bays


# ------------------------------------------------------------------ shared-texture GLB writer
def write_glb(path, mesh, texrefs, colors=None):
    """texrefs: material -> (albedo_uri, normal_uri), relative to the GLB. Textures live outside the GLB, shared."""
    bin_ = bytearray(); views = []; accs = []; images = []; textures = []; materials = []; prims = []; img_ix = {}

    def add_view(data, target=None):
        while len(bin_) % 4: bin_.append(0)
        off = len(bin_); bin_.extend(data)
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target: v["target"] = target
        views.append(v); return len(views) - 1

    def add_img(uri):
        if uri not in img_ix:
            images.append({"uri": uri}); textures.append({"source": len(images) - 1}); img_ix[uri] = len(textures) - 1
        return img_ix[uri]

    for name in MATS:
        items = mesh.prims.get(name)
        if not items: continue
        pos, nrm, uv, idx, col = [], [], [], [], []
        for it in items:
            pts, n, uvs, cnt = it[:4]
            b0 = len(pos)
            for vi, (q, tt) in enumerate(zip(pts, uvs)):
                pos.append(q); nrm.append(n); uv.append(tt)
                if len(it) > 4: col.append(it[4][vi])
            idx += [b0, b0 + 1, b0 + 2] + ([b0, b0 + 2, b0 + 3] if cnt == 4 else [])
        pa = np.array(pos, np.float32); na = np.array(nrm, np.float32); ua = np.array(uv, np.float32); ia = np.array(idx, np.uint16)
        accs += [
            {"bufferView": add_view(pa.tobytes(), 34962), "componentType": 5126, "count": len(pa), "type": "VEC3",
             "min": pa.min(0).tolist(), "max": pa.max(0).tolist()},
            {"bufferView": add_view(na.tobytes(), 34962), "componentType": 5126, "count": len(na), "type": "VEC3"},
            {"bufferView": add_view(ua.tobytes(), 34962), "componentType": 5126, "count": len(ua), "type": "VEC2"},
            {"bufferView": add_view(ia.tobytes(), 34963), "componentType": 5123, "count": len(ia), "type": "SCALAR"}]
        a = len(accs) - 4
        alb, nor = texrefs[name]
        pbr = {"baseColorTexture": {"index": add_img(alb)}, "metallicFactor": 0.0, "roughnessFactor": 0.05 if "roomglass" in name else 0.9}
        if name in BRAND_MATS and colors:  # the pickable business colours: white texture x this factor
            pbr["baseColorFactor"] = srgb_to_linear(colors[name]) + [1.0]
        materials.append({"name": name, "pbrMetallicRoughness": pbr, "normalTexture": {"index": add_img(nor), "scale": 1.0}})
        attrs = {"POSITION": a, "NORMAL": a + 1, "TEXCOORD_0": a + 2}
        if col:                                                     # roomglass: (room width, height, seed) per vertex
            ca = np.array(col, np.float32)
            accs.append({"bufferView": add_view(ca.tobytes(), 34962), "componentType": 5126, "count": len(ca), "type": "VEC3"}); attrs["COLOR_0"] = len(accs) - 1
        prims.append({"attributes": attrs, "indices": a + 3, "material": len(materials) - 1})
    gltf = {"asset": {"version": "2.0", "generator": "cityengine gen_low_commercial"}, "scene": 0, "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0, "name": os.path.splitext(os.path.basename(path))[0]}], "meshes": [{"primitives": prims}],
            "materials": materials, "textures": textures, "images": images, "accessors": accs, "bufferViews": views,
            "buffers": [{"byteLength": len(bin_)}]}
    j = json.dumps(gltf, separators=(",", ":")).encode(); j += b" " * (-len(j) % 4)
    while len(bin_) % 4: bin_.append(0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(j) + 8 + len(bin_)))
        f.write(struct.pack("<II", len(j), 0x4E4F534A)); f.write(j)
        f.write(struct.pack("<II", len(bin_), 0x004E4942)); f.write(bytes(bin_))


def save_tex(name, maps):
    """Write (albedo, normal) bytes into tex/ once; return GLB-relative uris."""
    a, n = maps
    ea = "png" if a[:4] == b"\x89PNG" else "jpg"; en = "png" if n[:4] == b"\x89PNG" else "jpg"
    fa, fn = f"{name}_albedo.{ea}", f"{name}_normal.{en}"
    open(os.path.join(TEX, fa), "wb").write(a); open(os.path.join(TEX, fn), "wb").write(n)
    return f"../tex/{fa}", f"../tex/{fn}"


def main():
    for d in (OUT, TEX, SRC): os.makedirs(d, exist_ok=True)
    levels = [int(a) for a in sys.argv[1:]] or sorted(LEVELS)
    shared = {"ground": save_tex("CL_asphalt", tile_maps(f"{RT}/asphalt_albedo.png", 3.0)),
              "paving": save_tex("CL_pavers", tile_maps(f"{RT}/pavers_albedo.png", 6.0))}
    shared["brand1"] = shared["brand2"] = save_tex("CL_brand", brand_sheet())
    import roomglass
    shared["roomglass"] = save_tex("CL_roomglass", roomglass.maps(41, "shop"))
    catalog = []
    for lv in levels:
        p = LEVELS[lv]
        wp = os.path.join(SRC, f"CL_L{lv}_wall.jpg"); rp = os.path.join(SRC, f"CL_L{lv}_roof.jpg")
        lvtex = dict(shared, wall=save_tex(f"CL_L{lv}_wall", tile_maps(wp if os.path.exists(wp) else p["wall"], p["wall_k"])),
                     roof=save_tex(f"CL_L{lv}_roof", tile_maps(rp if os.path.exists(rp) else p["roof"], 4.0)),
                     detail=save_tex(f"CL_L{lv}_detail", detail_sheet(p)))
        count = 0
        for key, t in TYPES.items():
            refs = lvtex
            colors = {"brand1": t["brand"], "brand2": t["brand2"]}
            for (W, D) in LOTS:
                if not fits(key, W, D): continue
                mesh, h, bays = build(W, D, key, lv)
                name = f"CL_{W}x{D}_{key}_L{lv}"
                write_glb(os.path.join(OUT, name + ".glb"), mesh, refs, colors)
                catalog.append(dict(file=name + ".glb", lot=[W, D], lot_m=[W * CELL, D * CELL], level=lv, type=key, resource=t["res"],
                                    name=t["name"], archetype=t["arch"], colors={"brand1": list(t["brand"]), "brand2": list(t["brand2"])}, height_m=round(h, 1), parking_bays=bays, tris=mesh.tri_count()))
                count += 1
        print(f"level {lv}: {count} buildings")
    with open(os.path.join(BASE, "catalog.json"), "w") as f:
        json.dump(catalog, f, indent=1)
    print(f"total {len(catalog)} GLBs")


if __name__ == "__main__":
    main()
