"""Low-density office generator: the ordinary brick multi-tenant suite buildings of suburbia, CS2-style 5 levels.

Low poly, high detail: every surface is a few quads, the detail lives in a procedural 512 atlas (window frames, mullions,
spandrels, louvres, doors, solar cells) plus cheap geometric relief that reads at city-builder camera distance:
cornices, sills/lintels, recessed entrances, vertical fins, brise-soleil, roof plant, planters, low-poly trees.

Styles by level (same lot, better building; all brick, multiple suites inside, glass kept to the L5 entry bay):
  L1 strip     1 floor tan brick suite strip, a door + sign panel per tenant, shingled mansard fascia
  L2 suites    2 floors red brick, ground-floor suite doors with little canopies, paired windows upstairs, mansard
  L3 garden    2 floors brown brick, shared lobby under a gabled porch, paired windows, hip shingle roof
  L4 pro       3 floors deep red brick, precast sill bands, single windows in rhythm, cornice, side suite doors
  L5 upscale   3 floors ironspot brick on a limestone base, taller windows, glass lobby bay, heavy cornice

Parking: front car park when the lot is >= 4 cells deep, side strip on wide shallow lots, none on 2-wide lots.
Brand materials brand1 (fascia / monument sign) and brand2 (accent fins / entry) are white x baseColorFactor, tintable.

Run:  python gen_low_office.py [level ...]
Output: assets/buildings/low_office/glb/OL_<W>x<D>_L<level>.glb + catalog.json + preview.html.  m, +Y up, front +Z.
"""
import json, math, os, sys
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import gen_low_density as g
import gen_low_commercial as c
from gen_low_density import Mesh, tile_maps, normal_from_height, jpg, png, CELL

ROOT = g.ROOT
BASE = os.path.join(ROOT, "assets", "buildings", "low_office")
SRC, OUT, TEX = (os.path.join(BASE, d) for d in ("src_tex", "glb", "tex"))
B = os.path.join(ROOT, "assets", "buildings"); RT = os.path.join(ROOT, "assets", "roads", "tex")

LOTS = [(2, 2), (2, 3), (2, 4), (3, 3), (3, 4), (3, 5), (4, 4), (4, 5), (4, 6), (5, 5), (5, 6), (6, 6)]
MATS = ("wall", "trim", "roof", "shingle", "detail", "ground", "paving", "grass", "brand1", "brand2", "roomglass")
_q = lambda a, b, c_, d: (a / 128, b / 128, c_ / 128, d / 128)
GLASS_RECTS = {"win": [_q(10, 118, 10, 118)], "sash": [_q(10, 116, 8, 119)], "door": [_q(8, 60, 8, 124), _q(68, 120, 8, 124)], "door2": [_q(8, 46, 10, 124)],
               "ribbon": [(4 / 256, 254 / 256, 8 / 128, 122 / 128)], "curtain": [_q(4, 124, 4, 122)], "curtain_op": [_q(4, 124, 4, 122)]}
c.MATS = MATS  # writer iterates this
c.ATLAS["paint"] = (8, 392, 120, 504)  # car-park lines from c.lot_x hit this atlas's paint cell

BZ, BRONZE, TINT = f"{B}/low_density/src_tex", (64, 52, 40), (30, 38, 44)
LEVELS = {  # all brick, multi-tenant. brick=(colour, mortar, variation, ironspot) -> procedural; else wall photo
    1: dict(style="strip", brick=((196, 166, 124), (214, 208, 196), 0.08, 0.0), wall_m=2.0, floors=1, fh=3.6, win_h=1.6,
            shingle=f"{BZ}/L2_roof.jpg", trim=(222, 214, 198), frame=BRONZE, glass=TINT, brand=(40, 70, 120), brand2=(150, 40, 40)),
    2: dict(style="suites", wall=f"{BZ}/L3_wall.jpg", wall_k=8, wall_m=2.0, floors=2, fh=3.4, win_h=1.6,
            shingle=f"{BZ}/L3_roof.jpg", trim=(226, 222, 210), frame=BRONZE, glass=TINT, brand=(30, 80, 60), brand2=(40, 60, 110)),
    3: dict(style="garden", brick=((124, 80, 58), (190, 182, 168), 0.10, 0.05), wall_m=2.0, floors=2, fh=3.5, win_h=1.6,
            shingle=f"{BZ}/L2_roof.jpg", trim=(232, 228, 216), frame=(236, 232, 222), glass=TINT, brand=(90, 40, 40), brand2=(50, 70, 90)),
    4: dict(style="pro", brick=((152, 64, 48), (200, 194, 182), 0.09, 0.03), wall_m=2.0, floors=3, fh=3.6, win_h=1.9, cornice=0.5,
            shingle=f"{BZ}/L2_roof.jpg", trim=(206, 198, 180), frame=BRONZE, glass=TINT, brand=(30, 50, 90), brand2=(170, 130, 50)),
    5: dict(style="upscale", brick=((96, 72, 62), (170, 164, 156), 0.12, 0.18), wall_m=2.0, floors=3, fh=3.8, win_h=2.1, cornice=0.7,
            stone_base=True, glass_bay=True, shingle=f"{BZ}/L2_roof.jpg", trim=(226, 216, 196), frame=(40, 40, 42), glass=(44, 60, 72),
            brand=(30, 30, 34), brand2=(150, 120, 60)),
}
SIDEWALK = 2.0
BAY_W, BAY_D, AISLE = 2.5, 5.0, 6.0

# ------------------------------------------------------------------ detail atlas (512, 4x4 grid of 128 cells)
A = lambda i, j, w=1, h=1: (i * 128, j * 128, (i + w) * 128, (j + h) * 128)
ATLAS = {"win": A(0, 0), "sash": A(1, 0), "door": A(2, 0), "door2": A(3, 0),
         "ribbon": A(0, 1, 2, 1), "spandrel": A(2, 1), "louvre": A(3, 1),
         "curtain": A(0, 2), "curtain_op": A(1, 2), "solar": A(2, 2), "vent": A(3, 2),
         "paint": A(0, 3), "leaf": A(1, 3), "bark": A(2, 3), "metal": A(3, 3),
         "sign": (0, 256, 512, 512)}  # brand sheet: reused from commercial (bottom half = sign panel)


def auv(name, u, v, inset=0.02):
    x0, y0, x1, y1 = ATLAS[name]
    return ((x0 + (x1 - x0) * (inset + u * (1 - 2 * inset))) / 512.0, (y0 + (y1 - y0) * (inset + (1 - v) * (1 - 2 * inset))) / 512.0)


def FULL(k, ru=1.0):
    return [auv(k, 0, 0), auv(k, ru, 0), auv(k, ru, 1), auv(k, 0, 1)]


def detail_sheet(p):
    S = 512
    fr, gl = p["frame"], p["glass"]; lite = tuple(min(255, v + 28) for v in gl); dark = tuple(max(0, v - 14) for v in gl)
    col = Image.new("RGB", (S, S), fr); hgt = Image.new("L", (S, S), 128)
    dc, dh = ImageDraw.Draw(col), ImageDraw.Draw(hgt)
    R = lambda bx, cc, hh: (dc.rectangle(bx, fill=cc), dh.rectangle(bx, fill=hh))

    def glass(x0, y0, x1, y1):  # glass with a sky gradient + reflection streak
        for y in range(y0, y1 + 1):
            t = (y - y0) / max(1, y1 - y0)
            cc = tuple(int(lite[k] * (1 - t) + dark[k] * t) for k in range(3))
            dc.line([x0, y, x1, y], fill=cc)
        dh.rectangle([x0, y0, x1, y1], fill=40)
        dc.polygon([(x0 + (x1 - x0) * 0.55, y0), (x0 + (x1 - x0) * 0.7, y0), (x0 + (x1 - x0) * 0.35, y1), (x0 + (x1 - x0) * 0.2, y1)],
                   fill=tuple(min(255, v + 18) for v in lite))
    # punched window: frame, 2 panes, transom
    R(A(0, 0), fr, 210); glass(10, 10, 60, 92); glass(68, 10, 118, 92); glass(10, 100, 118, 118)
    # sash: 6-over-6 for brick
    R(A(1, 0), fr, 210)
    for i in range(3):
        for j in range(4):
            glass(128 + 10 + i * 37, 8 + j * 29, 128 + 42 + i * 37, 32 + j * 29)
    # doors: double glass door with push bars / solid office door + sidelight
    R(A(2, 0), fr, 210); glass(264, 8, 316, 124); glass(324, 8, 376, 124)
    R([304, 60, 312, 66], (200, 200, 200), 250); R([328, 60, 336, 66], (200, 200, 200), 250)
    R(A(3, 0), (90, 70, 50), 170); glass(392, 10, 430, 124); R([440, 10, 504, 124], (110, 86, 60), 190); R([490, 64, 496, 72], (210, 190, 120), 240)
    # ribbon: long horizontal glazing with mullions every 32 px
    R(A(0, 1, 2, 1), fr, 210)
    for i in range(8):
        glass(4 + i * 32, 136, 30 + i * 32, 250)
    # spandrel panel (render/metal), louvre, vent
    R(A(2, 1), tuple(min(255, v + 0) for v in p["trim"]), 160)
    for j in range(4): R([256, 160 + j * 28, 383, 162 + j * 28], tuple(max(0, v - 20) for v in p["trim"]), 140)
    R(A(3, 1), (90, 94, 98), 160)
    for j in range(10): R([388, 132 + j * 12, 508, 138 + j * 12], (130, 134, 138), 220)
    # curtain wall: 2x3 lites with thin mullions; second variant with opening vent
    for k, x in ((0, 0), (1, 128)):
        R([x, 256, x + 127, 383], fr, 220)
        for i in range(2):
            for j in range(3):
                glass(x + 4 + i * 62, 260 + j * 41, x + 62 + i * 62, 296 + j * 41)
        if k: R([x + 70, 302, x + 120, 336], tuple(min(255, v + 40) for v in gl), 90)
    # solar cells
    R(A(2, 2), (180, 184, 190), 200)
    for i in range(4):
        for j in range(6): R([260 + i * 31, 260 + j * 20, 288 + i * 31, 277 + j * 20], (24, 36, 70), 90)
    # HVAC vent grille
    R(A(3, 2), (170, 172, 172), 190)
    for i in range(12): R([388 + i * 10, 268, 393 + i * 10, 372], (60, 62, 64), 60)
    # paint (line white), leaf, bark, metal
    R(A(0, 3), (236, 234, 226), 140)
    rng = np.random.default_rng(3)
    R(A(1, 3), (66, 104, 52), 140)
    for _ in range(260):
        x, y = rng.integers(128, 256), rng.integers(384, 512); r = int(rng.integers(2, 6)); s = int(rng.integers(-26, 30))
        cc = (max(0, 66 + s), max(0, 104 + s), max(0, 52 + s // 2)); dc.ellipse([x - r, y - r, x + r, y + r], fill=cc); dh.ellipse([x - r, y - r, x + r, y + r], fill=int(128 + s * 2))
    R(A(2, 3), (92, 72, 56), 130)
    for i in range(14): R([256 + i * 9, 384, 259 + i * 9, 511], (70, 54, 42), 100)
    R(A(3, 3), (150, 154, 158), 200); R([384, 448, 511, 450], (110, 112, 116), 160)
    rgb = np.asarray(col).astype(np.float32)
    rgb = np.clip(rgb * (0.95 + 0.08 * np.random.default_rng(11).random((S, S, 1))), 0, 255).astype(np.uint8)
    h = np.asarray(hgt.filter(ImageFilter.GaussianBlur(1.0)), np.float32) / 255.0
    return jpg(rgb), png(normal_from_height(h, 5.0))


def trim_sheet(rgb_base):
    """Smooth precast/render trim: cornices, sills, columns, fins."""
    S = 256; rng = np.random.default_rng(7)
    base = np.ones((S, S, 3), np.float32) * np.array(rgb_base, np.float32)
    n = rng.random((S // 8, S // 8)).repeat(8, 0).repeat(8, 1)[..., None]
    rgb = np.clip(base * (0.93 + 0.1 * n) * (0.97 + 0.05 * rng.random((S, S, 1))), 0, 255).astype(np.uint8)
    h = np.asarray(Image.fromarray((n[..., 0] * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(3)), np.float32) / 255.0
    return jpg(rgb), png(normal_from_height(h, 1.5))


# ------------------------------------------------------------------ geometry helpers
def new_mesh():
    m = Mesh(); m.prims = {k: [] for k in MATS}; return m


def mbox(m, mat, x0, x1, y0, y1, z0, z1, k=None, skip=(), tm=2.0):
    """Box in any material. k = atlas cell (each face maps the full cell) else world-tiled by tm metres."""
    def uvq(pts, axis):
        if k: return FULL(k)
        return [((q[axis]) / tm, -q[1] / tm) for q in pts]
    F = [("front", ((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)), 0),
         ("back", ((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)), 0),
         ("right", ((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)), 2),
         ("left", ((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)), 2)]
    for nm, pts, ax in F:
        if nm not in skip: m.quad(mat, *pts, uvq(pts, ax))
    if "top" not in skip:
        pts = ((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0))
        m.quad(mat, *pts, FULL(k) if k else [(q[0] / tm, q[2] / tm) for q in pts])
    if "bottom" in ("bottom",) and "bot" not in skip and y0 > 0.3:  # soffits of overhangs are visible
        pts = ((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1))
        m.quad(mat, *pts, FULL(k) if k else [(q[0] / tm, q[2] / tm) for q in pts])


def face_z(m, k, x0, x1, y0, y1, z, f=+1, off=0.03, mat="detail", ru=1.0):
    zz = z + off * f
    pts = [(x0, y0, zz), (x1, y0, zz), (x1, y1, zz), (x0, y1, zz)] if f > 0 else [(x1, y0, zz), (x0, y0, zz), (x0, y1, zz), (x1, y1, zz)]
    m.quad(mat, *pts, FULL(k, ru) if k != "ribbon" else [(auv(k, 0, 0)), (auv(k, 1, 0)), auv(k, 1, 1), auv(k, 0, 1)])
    if mat == "detail" and k in GLASS_RECTS:
        n = max(1, int(round(ru))); step = (x1 - x0) / n                      # a repeated cell: one set of panes per repeat
        for r in range(n):
            p = (x0 + r * step, y0, zz) if f > 0 else (x1 - r * step, y0, zz)
            c.glass_over(m, k, p, (f, 0, 0), step, y1 - y0, (0, 0, f), GLASS_RECTS[k])


def face_x(m, k, z0, z1, y0, y1, x, f, off=0.03, mat="detail"):
    xx = x + off * f
    pts = [(xx, y0, z1), (xx, y0, z0), (xx, y1, z0), (xx, y1, z1)] if f > 0 else [(xx, y0, z0), (xx, y0, z1), (xx, y1, z1), (xx, y1, z0)]
    m.quad(mat, *pts, FULL(k))
    if mat == "detail" and k in GLASS_RECTS: c.glass_over(m, k, pts[0], (0, 0, -f), z1 - z0, y1 - y0, (f, 0, 0), GLASS_RECTS[k])


def flat(m, mat, x0, x1, z0, z1, y, tile=4.0):
    c.flat(m, mat, x0, x1, z0, z1, y, tile)


def paint(m, x0, x1, z0, z1, y=0.05):
    m.quad("detail", (x0, y, z1), (x1, y, z1), (x1, y, z0), (x0, y, z0), FULL("paint"))


def tree(m, x, z, h=6.0, r=2.0):
    """12-tri trunk + 8-tri octahedral crown."""
    mbox(m, "detail", x - 0.15, x + 0.15, 0.0, h * 0.45, z - 0.15, z + 0.15, k="bark", skip=("top",))
    cy = h * 0.62; top = (x, h, z); bot = (x, h * 0.3, z)
    ring = [(x + r * math.cos(a), cy, z + r * math.sin(a)) for a in (0.3, 0.3 + math.pi / 2, 0.3 + math.pi, 0.3 + 1.5 * math.pi)]
    uv = [auv("leaf", 0.5, 1), auv("leaf", 0, 0), auv("leaf", 1, 0)]
    for i in range(4):
        a, b = ring[i], ring[(i + 1) % 4]
        m.tri("detail", top, b, a, uv); m.tri("detail", bot, a, b, uv)


def planter(m, x0, x1, z0, z1, h=0.45):
    mbox(m, "trim", x0, x1, 0.0, h, z0, z1, skip=("top",), tm=1.0)
    flat(m, "grass", x0 + 0.12, x1 - 0.12, z0 + 0.12, z1 - 0.12, h - 0.05, 3.0)


def hvac(m, x, z, H):
    mbox(m, "detail", x - 1.0, x + 1.0, H, H + 1.2, z - 0.7, z + 0.7, k="vent")
    mbox(m, "detail", x - 0.5, x + 0.5, H + 1.2, H + 1.3, z - 0.5, z + 0.5, k="metal")


def lot_x(m, x0, x1, z0, z1):
    return c.lot_x(m, x0, x1, z0, z1)


def lot_z(m, x0, x1, z0, z1):
    return c.lot_z(m, x0, x1, z0, z1)


# ------------------------------------------------------------------ building pieces
def block(m, p, x0, x1, z0, z1, y0, H, cornice=True):
    """Main wall volume + parapet/cornice. Returns roof height."""
    mbox(m, "wall", x0, x1, y0, H, z0, z1, tm=p["wall_m"], skip=("top", "bot"))
    flat(m, "roof", x0, x1, z0, z1, H, 4.0)
    if cornice:  # projecting cornice band = 4 boxes (thin, cheap, big read)
        o, t = 0.25, 0.45
        mbox(m, "trim", x0 - o, x1 + o, H, H + t, z1 - 0.3, z1 + o, tm=1.0)
        mbox(m, "trim", x0 - o, x1 + o, H, H + t, z0 - o, z0 + 0.3, tm=1.0)
        mbox(m, "trim", x0 - o, x0 + 0.3, H, H + t, z0 + 0.3, z1 - 0.3, tm=1.0)
        mbox(m, "trim", x1 - 0.3, x1 + o, H, H + t, z0 + 0.3, z1 - 0.3, tm=1.0)
        return H + t
    return H


def plinth(m, x0, x1, z0, z1):
    mbox(m, "trim", x0 - 0.08, x1 + 0.08, 0.0, 0.5, z0 - 0.08, z1 + 0.08, skip=("top",), tm=1.0)


def punched(m, p, x0, x1, z0, z1, floors, fh, base, sash, skip_mid=False):
    """Punched windows with sill + lintel relief on front/back, windows on sides."""
    k = "sash" if sash else "win"
    for s in range(floors):
        y = base + s * fh
        for z, f, a, b in ((z1, +1, x0, x1), (z0, -1, x0, x1)):
            n = max(1, int((b - a) // 2.6))
            for i in range(n):
                cx = a + (i + 0.5) * (b - a) / n
                if skip_mid and s == 0 and f > 0 and abs(cx - (a + b) / 2) < 2.0: continue
                face_z(m, k, cx - 0.65, cx + 0.65, y + 0.9, y + 2.6, z, f)
                mbox(m, "trim", cx - 0.8, cx + 0.8, y + 0.8, y + 0.92, *((z, z + 0.14) if f > 0 else (z - 0.14, z)), tm=1.0)  # sill
                mbox(m, "trim", cx - 0.75, cx + 0.75, y + 2.6, y + 2.8, *((z, z + 0.06) if f > 0 else (z - 0.06, z)), tm=1.0, skip=("top",))  # lintel
        for x, f in ((x1, +1), (x0, -1)):
            n = max(1, int((z1 - z0) // 3.5))
            for i in range(n):
                cz = z0 + (i + 0.5) * (z1 - z0) / n
                face_x(m, k, cz - 0.6, cz + 0.6, y + 0.9, y + 2.6, x, f)


def ribbons(m, p, x0, x1, z0, z1, floors, fh, base, fins=0.0, sides=True):
    """Continuous ribbon glazing over spandrels, optional vertical fins every 3 m on the front."""
    for s in range(floors):
        y = base + s * fh
        for z, f in ((z1, +1), (z0, -1)):
            L = x1 - x0; reps = max(1, round(L / 6.0))
            for r in range(reps):
                a = x0 + 0.3 + r * (L - 0.6) / reps; b = a + (L - 0.6) / reps
                face_z(m, "ribbon", a, b, y + 1.0, y + fh - 0.35, z, f)
            face_z(m, "spandrel", x0 + 0.3, x1 - 0.3, y + 0.15, y + 1.0, z, f, off=0.02)
        if sides:
            for x, f in ((x1, +1), (x0, -1)):
                L = z1 - z0; reps = max(1, round(L / 6.0))
                for r in range(reps):
                    a = z0 + 0.3 + r * (L - 0.6) / reps; b = a + (L - 0.6) / reps
                    face_x(m, "ribbon", a, b, y + 1.0, y + fh - 0.35, x, f)
    if fins:
        n = int((x1 - x0) // 3.0)
        for i in range(n + 1):
            fx = x0 + 0.15 + i * (x1 - x0 - 0.3) / n
            mbox(m, "brand2", fx - 0.08, fx + 0.08, base, base + floors * fh, z1, z1 + fins, k="metal")


def curtain(m, x0, x1, z0, z1, floors, fh, base, sides=True):
    """Full-height curtain wall, mullion caps every 1.5 m (thin boxes on the front only)."""
    for s in range(floors):
        y = base + s * fh
        for z, f, a, b in ((z1, +1, x0, x1), (z0, -1, x0, x1)):
            n = max(1, int((b - a) // 1.5))
            for i in range(n):
                u0 = a + i * (b - a) / n; u1 = u0 + (b - a) / n
                face_z(m, "curtain_op" if (i + s) % 5 == 2 else "curtain", u0, u1, y, y + fh, z, f, off=0.01)
        if sides:
            for x, f in ((x1, +1), (x0, -1)):
                n = max(1, int((z1 - z0) // 1.5))
                for i in range(n):
                    u0 = z0 + i * (z1 - z0) / n
                    face_x(m, "curtain", u0, u0 + (z1 - z0) / n, y, y + fh, x, f, off=0.01)
    n = max(1, int((x1 - x0) // 1.5))
    for i in range(n + 1):
        fx = x0 + i * (x1 - x0) / n
        mbox(m, "detail", fx - 0.04, fx + 0.04, base, base + floors * fh, z1, z1 + 0.14, k="metal", skip=("top",))
    for s in range(floors + 1):
        mbox(m, "detail", x0, x1, base + s * fh - 0.06, base + s * fh + 0.06, z1, z1 + 0.12, k="metal", skip=("left", "right"))


def entrance(m, p, cx, z1, style, base=0.15):
    """Recessed glass entry with canopy; brick gets a columned porch with pediment-less entablature."""
    w = 3.2
    face_z(m, "door" if style != "brick" else "door2", cx - w / 2 + 0.3, cx + w / 2 - 0.3, base, base + 2.6, z1, off=-0.02 if style == "brick" else 0.02)
    mbox(m, "trim", cx - w / 2 - 1.2, cx + w / 2 + 1.2, 0.0, base, z1, z1 + 2.2, tm=1.0)  # entry step / landing
    if style == "brick":
        for xx in (cx - w / 2 - 0.6, cx + w / 2 + 0.6):
            mbox(m, "trim", xx - 0.18, xx + 0.18, base, 3.1, z1 + 1.7, z1 + 2.06, tm=1.0, skip=("top",))
        mbox(m, "trim", cx - w / 2 - 1.0, cx + w / 2 + 1.0, 3.1, 3.55, z1, z1 + 2.25, tm=1.0)
    else:
        mbox(m, "brand1", cx - w / 2 - 1.4, cx + w / 2 + 1.4, 3.0, 3.25, z1, z1 + 2.6, k="metal")
        face_z(m, "sign", cx - w / 2 - 1.2, cx + w / 2 + 1.2, 3.25, 3.75, z1 + 2.3, mat="brand1")


def monument(m, x, z):
    """Brand monument sign by the street: plinth + 2 faces."""
    mbox(m, "trim", x - 1.4, x + 1.4, 0.0, 0.35, z - 0.35, z + 0.35, tm=1.0)
    mbox(m, "brand1", x - 1.2, x + 1.2, 0.35, 1.5, z - 0.2, z + 0.2, k="metal", skip=("front", "back"))
    face_z(m, "sign", x - 1.15, x + 1.15, 0.45, 1.42, z + 0.2, mat="brand2", off=0.01)
    face_z(m, "sign", x - 1.15, x + 1.15, 0.45, 1.42, z - 0.2, -1, mat="brand2", off=0.01)


def roof_plant(m, p, x0, x1, z0, z1, H, solar=False, green=False):
    area = (x1 - x0) * (z1 - z0)
    if green:
        flat(m, "grass", x0 + 0.6, x1 - 0.6, z0 + 0.6, z1 - 0.6, H + 0.04, 3.0)
    if solar:  # tilted panel rows on the back half
        zr = z0 + 1.0
        while zr + 1.6 < (z0 + z1) / 2:
            y0, y1 = H + 0.25, H + 0.9
            m.quad("detail", (x0 + 1.0, y0, zr + 1.4), (x1 - 1.0, y0, zr + 1.4), (x1 - 1.0, y1, zr), (x0 + 1.0, y1, zr),
                   FULL("solar", 1.0))
            zr += 2.2
    n = 1 if area < 200 else (2 if area < 500 else 3)
    for i in range(n):
        hvac(m, x0 + (i + 0.5) * (x1 - x0) / n, (z0 + z1) / 2 + (2.0 if solar else 0.0), H)
    # stair / lift bulkhead
    mbox(m, "wall", x1 - 3.6, x1 - 1.0, H, H + 2.6, z0 + 1.0, z0 + 4.0, tm=p["wall_m"])
    face_x(m, "door", z0 + 2.0, z0 + 3.0, H, H + 2.1, x1 - 3.6, -1)


def brick_sheet(brick, mortar, var=0.10, seed=1, iron=0.0):
    """Procedural running-bond brick, 512 px = wall_m metres (2 m -> 26 courses x ~9 bricks)."""
    S, rows, cols = 512, 26, 9
    rng = np.random.default_rng(seed)
    col = Image.new("RGB", (S, S), mortar); hgt = Image.new("L", (S, S), 90)
    dc, dh = ImageDraw.Draw(col), ImageDraw.Draw(hgt)
    ch, cw = S / rows, S / cols
    for r in range(rows):
        off = (cw / 2) * (r % 2)
        for k in range(-1, cols + 1):
            x0 = k * cw + off; y0 = r * ch
            s = 1 + rng.normal(0, var); t = rng.normal(0, var * 0.5)
            cc = [brick[0] * s, brick[1] * (s + t), brick[2] * (s - t)]
            if iron and rng.random() < iron: cc = [v * 0.55 for v in cc]
            cc = tuple(int(np.clip(v, 0, 255)) for v in cc)
            bx = [x0 + 1.5, y0 + 1.5, x0 + cw - 1.5, y0 + ch - 1.5]
            dc.rectangle(bx, fill=cc); dh.rectangle(bx, fill=int(170 + rng.integers(-15, 15)))
    rgb = np.asarray(col).astype(np.float32)
    rgb = np.clip(rgb * (0.9 + 0.14 * rng.random((S, S, 1))), 0, 255).astype(np.uint8)
    h = np.asarray(hgt.filter(ImageFilter.GaussianBlur(0.8)), np.float32) / 255.0
    return jpg(rgb), png(normal_from_height(h, 3.0))


def slope_quad(m, mat, p0, p1, p2, p3, along, tm=2.0):
    """Roof plane: u along the eave axis, v = distance up the slope from the eave (shingle rows stay parallel)."""
    e0 = np.array(p0, float)
    def uv(q):
        q = np.array(q, float); d = q - e0
        u = q[along]; vv = math.sqrt(sum(d[i] ** 2 for i in range(3) if i != along))
        return (u / tm, -vv / tm)
    m.quad(mat, p0, p1, p2, p3, [uv(q) for q in (p0, p1, p2, p3)])


def slope_tri(m, mat, p0, p1, p2, along, tm=2.0):
    e0 = np.array(p0, float)
    def uv(q):
        q = np.array(q, float); d = q - e0
        return (q[along] / tm, -math.sqrt(sum(d[i] ** 2 for i in range(3) if i != along)) / tm)
    m.tri(mat, p0, p1, p2, [uv(q) for q in (p0, p1, p2)])


def hip_roof(m, x0, x1, z0, z1, y, pitch=0.42, o=0.5):
    """Hip roof with overhang o: 2 trapezoids + 2 triangles + soffit. Returns ridge height."""
    a, b, c0, d = x0 - o, x1 + o, z0 - o, z1 + o
    h = (d - c0) / 2; rise = h * pitch; zc = (c0 + d) / 2; yr = y + rise
    ra, rb = a + h, b - h
    if rb < ra: ra = rb = (a + b) / 2
    slope_quad(m, "shingle", (a, y, d), (b, y, d), (rb, yr, zc), (ra, yr, zc), 0)   # front
    slope_quad(m, "shingle", (b, y, c0), (a, y, c0), (ra, yr, zc), (rb, yr, zc), 0)  # back
    slope_tri(m, "shingle", (b, y, d), (b, y, c0), (rb, yr, zc), 2)                   # right
    slope_tri(m, "shingle", (a, y, c0), (a, y, d), (ra, yr, zc), 2)                   # left
    m.quad("trim", (a, y, c0), (b, y, c0), (b, y, d), (a, y, d), [(0, 0), (1, 0), (1, 1), (0, 1)])  # soffit
    mbox(m, "trim", a, b, y - 0.22, y, d - 0.04, d, tm=1.0, skip=("top",))  # fascia boards
    mbox(m, "trim", a, b, y - 0.22, y, c0, c0 + 0.04, tm=1.0, skip=("top",))
    return yr


def mansard(m, x0, x1, z0, z1, H, h=1.5, o=0.8):
    """Shingled mansard fascia ring that hides a flat roof - the classic 80s suite-building band."""
    a, b, c0, d = x0 - o, x1 + o, z0 - o, z1 + o; yt = H + h
    slope_quad(m, "shingle", (a, H, d), (b, H, d), (x1, yt, z1), (x0, yt, z1), 0)
    slope_quad(m, "shingle", (b, H, c0), (a, H, c0), (x0, yt, z0), (x1, yt, z0), 0)
    slope_quad(m, "shingle", (b, H, d), (b, H, c0), (x1, yt, z0), (x1, yt, z1), 2)
    slope_quad(m, "shingle", (a, H, c0), (a, H, d), (x0, yt, z1), (x0, yt, z0), 2)
    m.quad("trim", (a, H, c0), (b, H, c0), (b, H, d), (a, H, d), [(0, 0), (1, 0), (1, 1), (0, 1)])
    mbox(m, "trim", a - 0.05, b + 0.05, H - 0.25, H, d - 0.05, d + 0.05, tm=1.0, skip=("top",))
    mbox(m, "trim", a - 0.05, b + 0.05, H - 0.25, H, c0 - 0.05, c0 + 0.05, tm=1.0, skip=("top",))
    flat(m, "roof", x0, x1, z0, z1, yt - 0.3, 4.0)
    mbox(m, "trim", x0, x1, yt - 0.3, yt, z1 - 0.15, z1, tm=1.0, skip=("bot",))  # cap
    return yt


def sill_band(m, x0, x1, z0, z1, y, h=0.14, o=0.08):
    """One continuous precast sill/string band per floor instead of a box per window."""
    mbox(m, "trim", x0 - o, x1 + o, y - h, y, z0 - o, z1 + o, tm=1.0)


def window_row(m, z, f, a, b, y, w=1.2, ht=1.6, gap=2.6, pair=False, k="win"):
    n = max(1, int((b - a) // (gap * (2 if pair else 1))))
    for i in range(n):
        cx = a + (i + 0.5) * (b - a) / n
        if pair:
            for dx in (-0.68, 0.68):
                face_z(m, k, cx + dx - w / 2 * 0.9, cx + dx + w / 2 * 0.9, y, y + ht, z, f)
        else:
            face_z(m, k, cx - w / 2, cx + w / 2, y, y + ht, z, f)
        mbox(m, "trim", cx - (1.5 if pair else 0.75), cx + (1.5 if pair else 0.75), y + ht, y + ht + 0.22, *((z, z + 0.05) if f > 0 else (z - 0.05, z)), tm=1.0, skip=("top",))  # soldier lintel


def side_rows(m, x0, x1, z0, z1, y, ht=1.6, k="win"):
    for x, f in ((x1, +1), (x0, -1)):
        n = max(1, int((z1 - z0) // 3.6))
        for i in range(n):
            cz = z0 + (i + 0.5) * (z1 - z0) / n
            face_x(m, k, cz - 0.6, cz + 0.6, y, y + ht, x, f)


def suite_front(m, x0, x1, z1, fh, base, canopy=False, piers=True):
    """Row of tenant suites: own door, own sign panel (alternating brand colours), 1-2 windows, brick piers between."""
    n = max(2, int((x1 - x0) // 6.5)); uw = (x1 - x0) / n; doors = []
    for u in range(n):
        a, b = x0 + u * uw, x0 + (u + 1) * uw
        dx = a + 1.4 if u % 2 == 0 else b - 1.4  # doors paired at shared piers
        face_z(m, "door2", dx - 0.65, dx + 0.65, base, base + 2.4, z1)
        for wx in np.arange(a + 1.0, b - 1.0, 2.0):
            if abs(wx + 0.75 - dx) > 1.6 and wx + 1.5 < b - 0.4:
                face_z(m, "win", wx, wx + 1.5, base + 0.7, base + 2.4, z1)
        face_z(m, "sign", a + 0.5, b - 0.5, base + 2.6, base + 3.15, z1, mat="brand1" if u % 2 == 0 else "brand2", off=0.06)
        if canopy:
            mbox(m, "trim", dx - 1.0, dx + 1.0, base + 2.5, base + 2.65, z1, z1 + 1.1, tm=1.0)
        doors.append(dx)
        if piers and u:
            mbox(m, "wall", a - 0.25, a + 0.25, 0.0, base + fh, z1, z1 + 0.18, tm=2.0, skip=("top",))
    return doors


def lobby(m, cx, z1, base, gable_y=None, glass_bay=False, H=0.0):
    """Shared lobby entrance: double glass door; optional gabled porch on posts or a glass entry bay (L5)."""
    if glass_bay:
        bx0, bx1 = cx - 2.6, cx + 2.6
        mbox(m, "trim", bx0 - 0.3, bx1 + 0.3, 0.0, H + 0.8, z1, z1 + 1.6, tm=1.0, skip=("top",))
        for s in range(int((H + 0.4) // 1.9)):
            face_z(m, "curtain", bx0, bx1, 0.3 + s * 1.9, 0.3 + (s + 1) * 1.9 - 0.08, z1 + 1.6, off=0.02)
        flat(m, "roof", bx0 - 0.3, bx1 + 0.3, z1, z1 + 1.6, H + 0.8, 4.0)
        face_z(m, "door", cx - 1.2, cx + 1.2, base, base + 2.5, z1 + 1.6, off=0.04)
        return
    face_z(m, "door", cx - 1.2, cx + 1.2, base, base + 2.6, z1)
    mbox(m, "trim", cx - 2.4, cx + 2.4, 0.0, base, z1, z1 + 2.6, tm=1.0)
    if gable_y:
        for xx in (cx - 1.9, cx + 1.9):
            mbox(m, "trim", xx - 0.15, xx + 0.15, base, gable_y, z1 + 2.15, z1 + 2.45, tm=1.0, skip=("top",))
        y = gable_y; r = 1.0; a, b = cx - 2.4, cx + 2.4; zf = z1 + 2.7
        slope_quad(m, "shingle", (cx, y + r, z1), (cx, y + r, zf), (b, y, zf), (b, y, z1), 2)
        slope_quad(m, "shingle", (a, y, z1), (a, y, zf), (cx, y + r, zf), (cx, y + r, z1), 2)
        m.tri("trim", (a + 0.1, y, zf - 0.05), (b - 0.1, y, zf - 0.05), (cx, y + r - 0.08, zf - 0.05), [(0, 0), (1, 0), (0.5, 1)])
    else:
        mbox(m, "trim", cx - 2.0, cx + 2.0, base + 2.7, base + 2.9, z1, z1 + 1.6, tm=1.0)


# ------------------------------------------------------------------ building
def frame_lot(W, D):
    lw, ld = W * CELL, D * CELL
    return lw, ld, -lw / 2, lw / 2, -ld / 2, ld / 2


def build(W, D, level):
    p, m = LEVELS[level], new_mesh()
    st = p["style"]
    lw, ld, lx0, lx1, lz0, lz1 = frame_lot(W, D)
    street = lz1 - SIDEWALK
    flat(m, "paving", lx0 + 0.1, lx1 - 0.1, street, lz1 - 0.1, 0.06, 2.0)
    flat(m, "grass", lx0 + 0.1, lx1 - 0.1, lz0 + 0.1, street, 0.02, 4.0)

    usable = street - (lz0 + 1.5)
    front = 0.0
    for need in (2 * BAY_D + AISLE + 1.2, BAY_D + AISLE + 0.6):
        if usable - need >= 11.0 and D >= 4: front = need; break
    side = CELL if (front == 0.0 and W >= 3) else 0.0
    x0, x1 = lx0 + 1.5 + side, lx1 - 1.5
    setback = 2.6 if front == 0.0 else 1.4  # sidewalk/planting strip in front of the suites
    z1 = street - front - setback
    z0 = max(lz0 + 1.5, z1 - (14.0 if st in ("strip", "suites") else 18.0))
    floors = p["floors"] if W * D > 4 else min(p["floors"], 2)
    fh, base = p["fh"], 0.15
    H = base + floors * fh
    mx = (x0 + x1) / 2

    plinth(m, x0, x1, z0, z1)
    mbox(m, "wall", x0, x1, 0.0, H, z0, z1, tm=p["wall_m"], skip=("top", "bot"))
    if p.get("stone_base"):  # limestone ground course
        mbox(m, "trim", x0 - 0.06, x1 + 0.06, 0.0, base + 0.75, z0 - 0.06, z1 + 0.06, tm=1.0)
    doors = []
    if st in ("strip", "suites"):
        doors = suite_front(m, x0, x1, z1, fh, base, canopy=(st == "suites"))
        side_rows(m, x0, x1, z0, z1, base + 0.9)
        window_row(m, z0, -1, x0 + 1, x1 - 1, base + 0.9, gap=4.0)
        for s in range(1, floors):
            y = base + s * fh
            sill_band(m, x0, x1, z0, z1, y + 0.85)
            window_row(m, z1, +1, x0 + 0.5, x1 - 0.5, y + 0.85, pair=True)
            window_row(m, z0, -1, x0 + 0.5, x1 - 0.5, y + 0.85, pair=True)
            side_rows(m, x0, x1, z0, z1, y + 0.85)
        top = mansard(m, x0, x1, z0, z1, H)
    else:
        cx = mx
        for s in range(floors):
            y = base + s * fh
            sill_band(m, x0, x1, z0, z1, y + 0.85, h=0.18 if st == "pro" else 0.14)
            for z, f in ((z1, +1), (z0, -1)):
                gapw = 1.6 if (f > 0 and s == 0) or p.get("glass_bay") else 0.0
                if f > 0 and (s == 0 or p.get("glass_bay")):  # leave room for the lobby
                    window_row(m, z, f, x0 + 0.6, cx - 3.0 - gapw, y + 0.85, pair=(st != "pro"), ht=p["win_h"])
                    window_row(m, z, f, cx + 3.0 + gapw, x1 - 0.6, y + 0.85, pair=(st != "pro"), ht=p["win_h"])
                else:
                    window_row(m, z, f, x0 + 0.6, x1 - 0.6, y + 0.85, pair=(st != "pro"), ht=p["win_h"])
            side_rows(m, x0, x1, z0, z1, y + 0.85, ht=p["win_h"])
            if st == "pro" and s == 0:  # side suite doors so tenants can enter directly
                face_x(m, "door2", z0 + 2.0, z0 + 3.2, base, base + 2.4, x1, +1)
        if st == "garden":
            lobby(m, cx, z1, base, gable_y=base + 3.0)
            top = hip_roof(m, x0, x1, z0, z1, H, pitch=0.38)
        else:
            lobby(m, cx, z1, base, glass_bay=p.get("glass_bay", False), H=H)
            top = block_cornice(m, x0, x1, z0, z1, H, p.get("cornice", 0.5))
            roof_plant(m, p, x0, x1, z0, z1, top - 0.0, solar=False, green=False)
        doors = [cx]

    # site
    for dx in doors:
        flat(m, "paving", dx - 0.9, dx + 0.9, z1, z1 + setback, 0.05, 2.0)
    flat(m, "paving", x0, x1, z1, z1 + 1.6, 0.05, 2.0)  # walk along the suites
    if front:
        bays = lot_x(m, lx0 + 0.3, lx1 - 0.3, z1 + setback, street - 0.2)
        for xx in (lx0 + 1.6, lx1 - 1.6): tree(m, xx, street - 1.0, 5.5, 1.8)
    else:
        bays = 0
        flat(m, "paving", doors[0] - 0.9, doors[0] + 0.9, z1, street, 0.05, 2.0)
        for xx in np.linspace(x0 + 1.5, x1 - 1.5, max(2, int((x1 - x0) // 8))):  # shrubs along the frontage
            if all(abs(xx - d) > 1.6 for d in doors): planter(m, xx - 1.0, xx + 1.0, z1 + 1.7, z1 + 2.4, 0.4)
    if side:
        bays += lot_z(m, lx0 + 0.3, lx0 + side + 1.2, lz0 + 1.0, street - 0.2)
    for i, xx in enumerate(np.linspace(x0 + 2, x1 - 2, max(1, int((x1 - x0) // 9)))):
        if z0 - lz0 > 2.5 or i % 2 == 0: tree(m, xx, max(lz0 + 1.2, z0 - 1.6), 6.0 + (i % 3) * 0.8, 2.0)
    if W >= 3:
        monument(m, lx1 - 3.0, street - 1.0)
    for i in range(3):
        bx = doors[0] + 2.0 + i * 0.6
        mbox(m, "detail", bx - 0.03, bx + 0.03, 0.05, 0.85, z1 + 0.4, z1 + 1.1, k="metal")
    return m, top, bays, floors


def block_cornice(m, x0, x1, z0, z1, H, t):
    flat(m, "roof", x0, x1, z0, z1, H, 4.0)
    if t <= 0: return H
    o = 0.3
    mbox(m, "trim", x0 - o, x1 + o, H, H + t, z1 - 0.3, z1 + o, tm=1.0)
    mbox(m, "trim", x0 - o, x1 + o, H, H + t, z0 - o, z0 + 0.3, tm=1.0)
    mbox(m, "trim", x0 - o, x0 + 0.3, H, H + t, z0 + 0.3, z1 - 0.3, tm=1.0)
    mbox(m, "trim", x1 - 0.3, x1 + o, H, H + t, z0 + 0.3, z1 - 0.3, tm=1.0)
    return H + t


def write_preview():
    html = """<!doctype html>
<meta charset="utf-8"><title>Low density office</title>
<style>body{margin:0;background:#8aa0b4;font:12px sans-serif;overflow:hidden}#ui{position:fixed;top:6px;left:6px;background:#000a;color:#fff;padding:6px;border-radius:4px}button{margin:1px}</style>
<div id="ui"></div>
<script type="importmap">{"imports":{"three":"https://unpkg.com/three@0.160.0/build/three.module.js","three/addons/":"https://unpkg.com/three@0.160.0/examples/jsm/"}}</script>
<script type="module">
import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
import {patchRoomGlass} from '../spom_preview.js';
const r=new THREE.WebGLRenderer({antialias:true});r.setSize(innerWidth,innerHeight);document.body.appendChild(r.domElement);r.shadowMap.enabled=true;
const s=new THREE.Scene();s.background=new THREE.Color(0x8aa0b4);
const cam=new THREE.PerspectiveCamera(40,innerWidth/innerHeight,0.1,600);cam.position.set(34,26,48);
const oc=new OrbitControls(cam,r.domElement);oc.target.set(0,4,0);
s.add(new THREE.HemisphereLight(0xdfe8ff,0x6b6a58,0.8));
const sun=new THREE.DirectionalLight(0xfff0d8,2.2);sun.position.set(-14,24,18);sun.castShadow=true;Object.assign(sun.shadow.camera,{left:-60,right:60,top:60,bottom:-60});sun.shadow.mapSize.set(2048,2048);s.add(sun);
const ground=new THREE.Mesh(new THREE.PlaneGeometry(300,300),new THREE.MeshStandardMaterial({color:0x6d7f52}));ground.rotation.x=-Math.PI/2;ground.position.y=-0.01;ground.receiveShadow=true;s.add(ground);
const cat=await (await fetch('catalog.json?'+Date.now())).json();const ui=document.getElementById('ui');
const lots=[...new Set(cat.map(c=>c.lot.join('x')))];let [lot,lv]=(location.hash.slice(1)||'4x5_3').split('_');lv=+lv;
const loader=new GLTFLoader();let cur=null;
function load(){const e=cat.find(c=>c.lot.join('x')==lot&&c.level==lv);if(cur)s.remove(cur);
 loader.load('glb/'+e.file+'?t='+Date.now(),g=>{cur=g.scene;patchRoomGlass(cur,new THREE.Vector3(-12,20,16));cur.traverse(o=>{if(o.isMesh){o.castShadow=o.receiveShadow=true;o.material.side=THREE.DoubleSide;if(o.material.map)o.material.map.anisotropy=8;}});s.add(cur);
 ui.innerHTML='Lot: '+lots.map(l=>`<button ${l==lot?'disabled':''} data-lot="${l}">${l}</button>`).join('')+'<br>'+[1,2,3,4,5].map(l=>`<button ${l==lv?'disabled':''} data-l="${l}">L${l}</button>`).join('')+` <b>${e.style}</b> · ${e.lot_m.join('×')} m · ${e.floors} floors · ${e.height_m} m · ${e.parking_bays} bays · ${e.workers} workers · ${e.tris} tris`;});}
ui.onclick=e=>{const b=e.target.dataset;if(b.lot)lot=b.lot;if(b.l)lv=+b.l;location.hash=lot+'_'+lv;load();};load();
r.setAnimationLoop(()=>{const w=innerWidth,h=innerHeight;if(r.domElement.width!==w||r.domElement.height!==h){r.setSize(w,h);cam.aspect=w/h;cam.updateProjectionMatrix();}oc.update();r.render(s,cam)});
</script>
"""
    open(os.path.join(BASE, "preview.html"), "w", encoding="utf-8").write(html)


def main():
    for d in (OUT, TEX, SRC): os.makedirs(d, exist_ok=True)
    c.TEX = TEX
    levels = [int(a) for a in sys.argv[1:]] or sorted(LEVELS)
    shared = {"ground": c.save_tex("OL_asphalt", tile_maps(f"{RT}/asphalt_albedo.png", 3.0)),
              "paving": c.save_tex("OL_pavers", tile_maps(f"{RT}/pavers_albedo.png", 6.0)),
              "grass": c.save_tex("OL_grass", tile_maps(f"{RT}/grass_albedo.png", 4.0)),
              "roof": c.save_tex("OL_roof", tile_maps(f"{RT}/gravel_albedo.png", 4.0))}
    shared["brand1"] = shared["brand2"] = c.save_tex("OL_brand", c.brand_sheet())
    import roomglass
    shared["roomglass"] = c.save_tex("OL_roomglass", roomglass.maps(43, "office"))
    catpath = os.path.join(BASE, "catalog.json")
    catalog = [e for e in (json.load(open(catpath)) if os.path.exists(catpath) else []) if e["level"] not in levels]
    for lv in levels:
        p = LEVELS[lv]
        wp = os.path.join(SRC, f"OL_L{lv}_wall.jpg")
        wall = tile_maps(wp, 8) if os.path.exists(wp) else (brick_sheet(*p["brick"][:3], seed=lv, iron=p["brick"][3]) if "brick" in p else tile_maps(p["wall"], p["wall_k"]))
        tex = dict(shared, wall=c.save_tex(f"OL_L{lv}_wall", wall), shingle=c.save_tex(f"OL_L{lv}_shingle", tile_maps(p["shingle"], 4.0)),
                   trim=c.save_tex(f"OL_L{lv}_trim", trim_sheet(p["trim"])),
                   detail=c.save_tex(f"OL_L{lv}_detail", detail_sheet(p)))
        # brand sheet's sign cell uses the commercial ATLAS layout: map "sign" via the brand sheet coordinates
        for (W, D) in LOTS:
            mesh, h, bays, floors = build(W, D, lv)
            name = f"OL_{W}x{D}_L{lv}"
            c.write_glb(os.path.join(OUT, name + ".glb"), mesh, tex, {"brand1": p["brand"], "brand2": p["brand2"]})
            catalog.append(dict(file=name + ".glb", lot=[W, D], lot_m=[W * CELL, D * CELL], level=lv, style=p["style"],
                                floors=floors, height_m=round(h, 1), parking_bays=bays, tris=mesh.tri_count(),
                                workers=int(W * D * floors * 2.5), colors={"brand1": list(p["brand"]), "brand2": list(p["brand2"])}))
        print(f"level {lv}: {len(LOTS)} offices, tris {min(e['tris'] for e in catalog if e['level']==lv)}-{max(e['tris'] for e in catalog if e['level']==lv)}")
    catalog.sort(key=lambda e: (e["level"], e["lot"]))
    json.dump(catalog, open(catpath, "w"), indent=1)
    write_preview()
    print(f"total {len(catalog)} GLBs -> {OUT}")


if __name__ == "__main__":
    main()

