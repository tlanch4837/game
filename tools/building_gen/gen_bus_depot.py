"""Bus depot -> GLB (CS2 has the Bus Depot and the Compact Bus Depot; same two here).
Detail is texture + normal map (roll-up bay doors, glazed lobby, window bands, painted yard markings), geometry stays boxes.
No buses, cars, trees, fine props, outer road or sidewalk baked in. Fascia sign panel is left blank for lettering.
Run: python gen_bus_depot.py     Output: assets/buildings/transport/bus_depot/glb + preview.html + catalog.json
"""
import json, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_low_density as g
from gen_low_density import Mesh, normal_from_height, jpg, png, CELL
import spom

BASE = os.path.join(g.ROOT, "assets", "buildings", "transport", "bus_depot")
OUT = os.path.join(BASE, "glb")
rng = np.random.default_rng(11)


# ---------------------------------------------------------------- textures
def noise(n, amp, blur=1.0, seed=0):
    r = np.random.default_rng(seed)
    im = Image.fromarray((128 + r.normal(0, amp, (n, n))).clip(0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(blur))
    return np.asarray(im, np.float32) - 128


def maps(rgb, h, k):
    return jpg(np.clip(rgb, 0, 255).astype(np.uint8)), png(normal_from_height(h, k))


def wall_tex(col):
    """Precast concrete panels, 3 m tile = 2x2 panels with recessed joints and rain streaks."""
    n = 512
    a = np.full((n, n, 3), col, np.float32) + noise(n, 7, 1.2, 1)[..., None]
    h = np.full((n, n), 0.6, np.float32) + noise(n, 6, 1.5, 2)[..., None][..., 0] / 255
    for k in (0, n // 2):
        a[k:k + 5] *= 0.62; a[:, k:k + 5] *= 0.62; h[k:k + 5] = 0.1; h[:, k:k + 5] = 0.1
    streak = noise(n, 14, 0.8, 3)[:1].repeat(n, 0) * np.linspace(1, 0.2, n)[:, None]
    a += streak[..., None] * 0.5
    return maps(a, h, 5.0)


def roof_tex():
    n = 512
    a = np.full((n, n, 3), (150, 154, 156), np.float32) + noise(n, 6, 2, 4)[..., None]
    h = np.full((n, n), 0.5, np.float32)
    for k in range(0, n, n // 4): a[:, k:k + 4] *= 0.8; h[:, k:k + 4] = 0.25
    return maps(a, h, 4.0)


def detail_tex(accent):
    """512 atlas: window (0,0) / wide window band (256,0) / glazed lobby door (0,256) / roll-up bay door (256,256)."""
    S = 512
    col = Image.new("RGB", (S, S), (222, 224, 224)); hg = Image.new("L", (S, S), 128)
    dc, dh = ImageDraw.Draw(col), ImageDraw.Draw(hg)
    def glass(x0, y0, x1, y1):  # reflective-looking gradient glass
        for y in range(y0, y1):
            t = (y - y0) / max(1, y1 - y0)
            dc.line([x0, y, x1, y], fill=(int(70 + 90 * (1 - t)), int(95 + 90 * (1 - t)), int(115 + 80 * (1 - t))))
        dh.rectangle([x0, y0, x1, y1], fill=40)
    glass(40, 30, 216, 226)
    for x in (128,): dc.rectangle([x - 4, 30, x + 4, 226], fill=(60, 64, 68)); dh.rectangle([x - 4, 30, x + 4, 226], fill=200)
    glass(262, 70, 506, 190)
    for i in range(1, 4):
        x = 262 + 244 * i // 4; dc.rectangle([x - 3, 70, x + 3, 190], fill=(60, 64, 68)); dh.rectangle([x - 3, 70, x + 3, 190], fill=200)
    dc.rectangle([256, 62, 511, 72], fill=(60, 64, 68)); dc.rectangle([256, 188, 511, 198], fill=(60, 64, 68))
    # lobby: storefront with double doors
    glass(10, 270, 246, 500)
    dc.rectangle([121, 270, 135, 500], fill=(60, 64, 68)); dh.rectangle([121, 270, 135, 500], fill=200)
    dc.rectangle([10, 258, 246, 270], fill=accent); dh.rectangle([10, 258, 246, 270], fill=210)
    dc.rectangle([100, 380, 106, 430], fill=(200, 200, 200)); dc.rectangle([150, 380, 156, 430], fill=(200, 200, 200))
    # roll-up door: ribbed slats in a steel frame, small vision strip
    dc.rectangle([258, 258, 509, 509], fill=(70, 74, 78)); dh.rectangle([258, 258, 509, 509], fill=210)
    for j in range(10):
        y0 = 268 + j * 24
        dc.rectangle([268, y0, 499, y0 + 20], fill=(188, 192, 194)); dh.rectangle([268, y0, 499, y0 + 20], fill=150)
    dc.rectangle([268, 282, 499, 296], fill=(60, 80, 100)); dh.rectangle([268, 282, 499, 296], fill=60)
    dc.rectangle([268, 498, 499, 505], fill=accent)
    rgb = np.asarray(col, np.float32) * (0.95 + 0.1 * rng.random((S, S, 1)))
    return maps(rgb, np.asarray(hg.filter(ImageFilter.GaussianBlur(1.0)), np.float32) / 255, 5.0)


def yard_tex(W, D, bays, accent):
    """Whole-lot ground texture (1 uv = lot): asphalt yard, painted bus bays along the front, wash lane, lot-edge kerb."""
    px = 14  # px per metre
    w, d = int(W * px), int(D * px)
    a = np.full((d, w, 3), (66, 68, 70), np.float32) + noise(max(w, d), 6, 1.5, 5)[:d, :w, None]
    im = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)); dr = ImageDraw.Draw(im)
    dr.rectangle([0, 0, w - 1, d - 1], outline=(150, 150, 146), width=int(0.5 * px))        # kerb
    for (x0, z0, x1, z1) in bays:                                                               # painted bays
        for x in np.linspace(x0, x1, int(round((x1 - x0) / 3.6)) + 1):
            p = ((x + W / 2) * px, (z0 + D / 2) * px, (x + W / 2) * px, (z1 + D / 2) * px)
            dr.line(p, fill=(235, 235, 225), width=3)
        dr.line(((x0 + W / 2) * px, (z0 + D / 2) * px, (x1 + W / 2) * px, (z0 + D / 2) * px), fill=(235, 235, 225), width=3)
    # circulation arrows: dashed yellow centre line down the yard
    cx = int(W * px / 2)
    for y in range(int(2 * px), d - int(2 * px), int(3 * px)):
        dr.line((cx, y, cx, y + int(1.5 * px)), fill=(220, 190, 40), width=3)
    arr = np.asarray(im.filter(ImageFilter.GaussianBlur(0.6)), np.float32)
    h = np.asarray(im.convert("L"), np.float32) / 255
    return maps(arr, h, 2.0)


# ---------------------------------------------------------------- mesh helpers
AT = {"win": (0, 0, 256, 256), "wide": (256, 0, 512, 256), "door": (0, 256, 256, 512), "garage": (256, 256, 512, 512)}


def auv(k, inset=0.012):
    x0, y0, x1, y1 = (v / 512 for v in AT[k])
    e = inset
    return [(x0 + e, y1 - e), (x1 - e, y1 - e), (x1 - e, y0 + e), (x0 + e, y0 + e)]


def zface(m, x0, x1, y0, y1, z, f, mat, uvs):
    if f > 0: m.quad(mat, (x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z), uvs)
    else: m.quad(mat, (x1, y0, z), (x0, y0, z), (x0, y1, z), (x1, y1, z), uvs)


def xface(m, z0, z1, y0, y1, x, f, mat, uvs):
    if f > 0: m.quad(mat, (x, y0, z1), (x, y0, z0), (x, y1, z0), (x, y1, z1), uvs)
    else: m.quad(mat, (x, y0, z0), (x, y0, z1), (x, y1, z1), (x, y1, z0), uvs)


def wuv(a, b, y0, y1, t=3.0):  # world-space wall uv
    return [(a / t, -y0 / t), (b / t, -y0 / t), (b / t, -y1 / t), (a / t, -y1 / t)]


def mbox(m, x0, x1, y0, y1, z0, z1, wall="wall", top="roof", skip=(), t=3.0, bottom=False):
    if "+z" not in skip: zface(m, x0, x1, y0, y1, z1, 1, wall, wuv(x0, x1, y0, y1, t))
    if "-z" not in skip: zface(m, x0, x1, y0, y1, z0, -1, wall, wuv(x1, x0, y0, y1, t))
    if "+x" not in skip: xface(m, z0, z1, y0, y1, x1, 1, wall, wuv(z1, z0, y0, y1, t))
    if "-x" not in skip: xface(m, z0, z1, y0, y1, x0, -1, wall, wuv(z0, z1, y0, y1, t))
    if bottom: m.quad(wall, (x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), [(x0 / 4, z0 / 4), (x1 / 4, z0 / 4), (x1 / 4, z1 / 4), (x0 / 4, z1 / 4)])
    if top: m.quad(top, (x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0), [(x0 / 4, z1 / 4), (x1 / 4, z1 / 4), (x1 / 4, z0 / 4), (x0 / 4, z0 / 4)])


def parapet(m, x0, x1, y, z0, z1, h=0.5, t=0.25):
    for (a, b, c, d) in ((x0, x1, z1 - t, z1), (x0, x1, z0, z0 + t), (x0, x0 + t, z0 + t, z1 - t), (x1 - t, x1, z0 + t, z1 - t)):
        mbox(m, a, b, y, y + h, c, d, top="roof")


def windows_z(m, x0, x1, y0, y1, z, f, step=4.0, mat="detail", kind="wide", skip=()):
    n = max(1, int((x1 - x0) // step)); w = (x1 - x0) / n
    for i in range(n):
        if i in skip: continue
        zface(m, x0 + i * w + 0.3, x0 + (i + 1) * w - 0.3, y0, y1, z + 0.02 * f, f, mat, auv(kind))


def windows_x(m, z0, z1, y0, y1, x, f, step=4.0, kind="wide"):
    n = max(1, int((z1 - z0) // step)); w = (z1 - z0) / n
    for i in range(n):
        xface(m, z0 + i * w + 0.3, z0 + (i + 1) * w - 0.3, y0, y1, x + 0.02 * f, f, "detail", auv(kind))


# ---------------------------------------------------------------- SPOM volumes (depth for free: 10 tris per whole facade)
FLAT = {"wall": (236 / 512, 240 / 512), "dark": (128 / 512, 128 / 512), "accent": (128 / 512, 264 / 512)}   # flat spots in the detail atlas


def win_spans(xa, xb, step):
    n = max(1, int((xb - xa) // step)); w = (xb - xa) / n
    return [(xa + i * w + 0.3, xa + (i + 1) * w - 0.3) for i in range(n)]


def spom_volume(m, kind, W, H, origin, paint, wallc, accent):
    """One relief volume on a +z facade. paint(add) calls add(x0,x1,y0,y1,z0,z1,tag) in metres from the volume's lower-left.
    Returns (kind, (albedo, normal), boxes, origin); boxes are only used to build the preview approximation."""
    spom.KINDS[kind] = (W, H)
    L = spom.Layers(W, H); boxes = []
    col = {"wall": tuple(min(1.0, v * 0.95 / 255) for v in wallc), "dark": (0.27, 0.29, 0.31), "accent": tuple(v / 255 for v in accent)}
    def add(x0, x1, y0, y1, z0, z1, tag="wall"):
        L.box(x0, x1, y0, y1, z0, z1, col[tag]); boxes.append((x0, x1, y0, y1, z0, z1, tag))
    paint(add)
    m.prims.setdefault("spom_" + kind, [])
    spom.spom_box(m, kind, origin, (0, 0, 1))
    return kind, L.encode(), boxes, origin


def pbox(m, b, origin):
    """Preview-only: a relief box as real geometry, flat colour from the detail atlas."""
    x0, x1, y0, y1, z0, z1, tag = b; ox, oy, oz = origin
    x0, x1, y0, y1, z0, z1 = x0 + ox, x1 + ox, y0 + oy, y1 + oy, z0 + oz, z1 + oz
    U = [FLAT[tag]] * 4
    zface(m, x0, x1, y0, y1, z1, 1, "detail", U); xface(m, z0, z1, y0, y1, x1, 1, "detail", U); xface(m, z0, z1, y0, y1, x0, -1, "detail", U)
    m.quad("detail", (x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0), U)
    m.quad("detail", (x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), U)


# ---------------------------------------------------------------- buildings
def build(cells, name, bays_n, office_floors, accent, wallc, hall_h, wash):
    Wc, Dc = cells
    W, D = Wc * CELL, Dc * CELL
    m = Mesh()
    hx, hz = W / 2 - 1.5, D / 2 - 1.5       # keep inside the lot with a 1.5 m margin
    # yard ground (lot sized quad), painted bays along the front half
    hall_d = 13.0 if W > 26 else 11.0
    hall_z1 = -hz + hall_d
    bays = [(-hx + 1, hx - 1, hall_z1 + 3.0, hall_z1 + 3.0 + 12.0)] if True else []
    m.quad("base", (-W / 2, 0.0, D / 2), (W / 2, 0.0, D / 2), (W / 2, 0.0, -D / 2), (-W / 2, 0.0, -D / 2), [(0, 1), (1, 1), (1, 0), (0, 0)])
    # --- service hall (back of the lot) with roll-up bay doors on its +z face
    hall_w = bays_n * 4.6 + 1.2
    x0, x1 = -hall_w / 2 - (0 if office_floors == 0 else 3.0), hall_w / 2 - (0 if office_floors == 0 else 3.0)
    x0, x1 = max(x0, -hx), min(x1, hx)
    mbox(m, x0, x1, 0.0, hall_h, -hz, hall_z1, skip=("+z",))
    parapet(m, x0, x1, hall_h, -hz, hall_z1)
    bw = (x1 - x0 - 1.2) / bays_n
    zface(m, x0, x1, 0.0, hall_h, hall_z1, 1, "wall", wuv(x0, x1, 0.0, hall_h))      # wall face, doors sit just proud of it
    for i in range(bays_n):
        xa = x0 + 0.6 + i * bw
        zface(m, xa + 0.15, xa + bw - 0.15, 0.0, 4.7, hall_z1 + 0.03, 1, "detail", auv("garage"))
    # clerestory glazing band above the doors, and high windows on the long side
    zface(m, x0 + 0.5, x1 - 0.5, 5.2, hall_h - 0.6, hall_z1 + 0.03, 1, "detail", [(0.5 * k, v) for k, v in ((0, 0.5), (1, 0.5), (1, 0.27), (0, 0.27))][:4] if False else auv("wide"))
    windows_x(m, -hz + 1, hall_z1 - 1, 5.0, hall_h - 0.8, x1, 1, 5.0)
    # --- office / driver building (2 floors) beside the hall, glazed lobby, blank sign panel
    ox1 = min(hx, x1 + (11.0 if W > 26 else 9.0)); ox0 = x1 + 0.0
    if office_floors:
        oh = 3.6 * office_floors + 0.4; od = 9.0 if W > 26 else 7.0
        oz0 = hall_z1 - od; oz1 = hall_z1
        mbox(m, ox0, ox1, 0.0, oh, oz0, oz1, skip=("-x",))
        parapet(m, ox0, ox1, oh, oz0, oz1)
        for fl in range(office_floors):
            y0 = 0.9 + fl * 3.6 if fl else 0.0
            if fl == 0:
                zface(m, ox0 + 0.6, ox0 + 0.6 + 3.6, 0.0, 3.0, oz1 + 0.03, 1, "detail", auv("door"))
                windows_z(m, ox0 + 4.4, ox1 - 0.4, 0.7, 2.9, oz1, 1, step=3.4, kind="win")
            else:
                windows_z(m, ox0 + 0.4, ox1 - 0.4, y0 + 0.2, y0 + 2.3, oz1, 1, step=3.4, kind="win")
        windows_x(m, oz0 + 1, oz1 - 1, 1.0, 3.0, ox1, 1, 3.4, "win")
        # blank fascia sign panel over the lobby (white, left blank for lettering)
        zface(m, ox0 + 0.6, ox1 - 0.6, oh - 0.9, oh - 0.1, oz1 + 0.04, 1, "detail", [(236 / 512, 240 / 512)] * 4)
        # entrance canopy: thin slab, no columns (cantilevered)
        mbox(m, ox0 + 0.2, ox0 + 4.6, 3.2, 3.4, oz1, oz1 + 2.0, wall="wall", top="roof", skip=("-z",))
    # --- SPOM relief: hall front (door piers, header beam, clerestory fins, accent fascia, coping) and office front
    vols = []
    def paint_hall(a):
        Wh = x1 - x0
        for i in range(bays_n + 1):                                   # door piers
            xc = 0.6 + i * bw
            a(xc - 0.3, xc + 0.3, 0.0, hall_h - 0.3, 0.0, 0.32)
            a(xc - 0.3, xc + 0.3, 0.0, 0.5, 0.0, 0.5, "dark")        # kerb guard at the foot of each pier
            a(xc - 0.12, xc + 0.12, 5.35, hall_h - 0.6, 0.0, 0.22)   # clerestory fin
        a(0.0, Wh, 4.7, 5.2, 0.0, 0.5, "dark")                       # steel header beam over the doors
        a(0.0, Wh, 5.2, 5.38, 0.0, 0.62)                             # sill ledge under the glazing
        a(0.0, Wh, hall_h - 0.3, hall_h + 0.05, 0.0, 0.55, "accent") # accent fascia band
        a(0.0, Wh, hall_h + 0.3, hall_h + 0.5, 0.0, 0.4)             # coping drip cap
    vols.append(spom_volume(m, "hall", x1 - x0, hall_h + 0.5, (x0, 0.0, hall_z1), paint_hall, wallc, accent))
    if office_floors:
        def paint_off(a):
            Wo = ox1 - ox0
            a(0.45, 0.65, 0.0, 3.2, 0.0, 0.42, "dark"); a(4.15, 4.35, 0.0, 3.2, 0.0, 0.42, "dark")   # lobby portal jambs
            a(0.45, 4.35, 3.0, 3.2, 0.0, 0.55, "dark")
            wins = [(0.7, 2.9, s) for s in win_spans(4.4, Wo - 0.4, 3.4)]
            for fl in range(1, office_floors): wins += [(0.9 + fl * 3.6 + 0.2, 0.9 + fl * 3.6 + 2.3, s) for s in win_spans(0.4, Wo - 0.4, 3.4)]
            for (wy0, wy1, (sa, sb)) in wins:
                a(sa - 0.1, sb + 0.1, wy1 + 0.12, wy1 + 0.2, 0.0, 0.62, "dark")        # sunshade blade
                a(sa - 0.1, sa - 0.04, wy0, wy1 + 0.2, 0.0, 0.34); a(sb + 0.04, sb + 0.1, wy0, wy1 + 0.2, 0.0, 0.34)   # reveal fins
                a(sa - 0.12, sb + 0.12, wy0 - 0.12, wy0, 0.0, 0.28)                    # sill
            for fl in range(1, office_floors): a(0.0, Wo, 3.5 + (fl - 1) * 3.6, 3.78 + (fl - 1) * 3.6, 0.0, 0.26)   # floor nosing
            a(0.0, Wo, oh - 0.1, oh + 0.3, 0.0, 0.34, "accent"); a(0.0, Wo, oh + 0.3, oh + 0.5, 0.0, 0.3)
        vols.append(spom_volume(m, "office", ox1 - ox0, oh + 0.5, (ox0, 0.0, oz1), paint_off, wallc, accent))
    # --- wash bay: open-front drive-through shed on the far side (columns, roof slab, wet concrete floor)
    if wash:
        wx0, wx1 = -hx, -hx + 6.0; wz0, wz1 = hall_z1 + 4.0, hall_z1 + 4.0 + 16.0
        mbox(m, wx0, wx0 + 0.5, 0.0, 5.5, wz0, wz1, skip=("+z", "-z"), top="roof")      # back wall
        mbox(m, wx0, wx1, 5.5, 6.2, wz0, wz1, top="roof", bottom=True)                                 # roof slab
        for zc in (wz0 + 0.3, wz1 - 0.3):                                                 # outer columns
            mbox(m, wx1 - 0.4, wx1, 0.0, 5.5, zc - 0.2, zc + 0.2, top=None)
        m.quad("drive", (wx0, 0.03, wz1), (wx1, 0.03, wz1), (wx1, 0.03, wz0), (wx0, 0.03, wz0), [(0, 0), (1, 0), (1, 1), (0, 1)])
        bays = [(wx1 + 1.0, hx - 1, hall_z1 + 3.0, hall_z1 + 15.0)]
    # --- fuel island canopy at the front corner (slab + 2 columns), bus fuelling
    fx0, fx1 = hx - 8.0, hx - 1.0; fz0, fz1 = hz - 7.0, hz - 2.5
    mbox(m, fx0, fx1, 4.8, 5.3, fz0, fz1, top="roof", bottom=True)
    for (cx, cz) in ((fx0 + 0.6, fz0 + 0.6), (fx1 - 0.6, fz0 + 0.6)):
        mbox(m, cx - 0.2, cx + 0.2, 0.0, 4.8, cz - 0.2, cz + 0.2, top=None)
    return m, bays, vols


MODELS = [
    # name, cells (W x D), bays, office floors, accent, wall colour, hall height, wash bay
    ("BD_4x5_bus_depot_L1", (4, 5), 4, 2, (210, 120, 20), (186, 184, 176), 7.0, True),
    ("BD_3x3_compact_bus_depot_L1", (3, 3), 3, 1, (30, 110, 170), (172, 178, 182), 6.4, False),
]


def main():
    os.makedirs(OUT, exist_ok=True)
    roof = roof_tex(); catalog = []
    for (name, cells, bays_n, fl, acc, wallc, hh, wash) in MODELS:
        m, bays, vols = build(cells, name, bays_n, fl, acc, wallc, hh, wash)
        W, D = cells[0] * CELL, cells[1] * CELL
        tex = {"wall": wall_tex(wallc), "roof": roof, "detail": detail_tex(acc), "base": yard_tex(W, D, bays, acc),
               "drive": wall_tex((120, 122, 124))}
        for (kind, maps_, boxes, origin) in vols: tex["spom_" + kind] = maps_
        mats = {"detail": {"rough": 0.35, "metal": 0.1}, "base": {"rough": 0.95}, "drive": {"rough": 0.4}}
        m.prims = {k: v for k, v in m.prims.items() if v}
        tex = {k: v for k, v in tex.items() if k in m.prims}
        p = os.path.join(OUT, name + ".glb")
        g.write_glb(p, m, tex, mats)
        tris = m.tri_count()
        spom_tris = sum(2 if c[3] == 4 else 1 for k, l in m.prims.items() if k.startswith("spom_") for c in l)
        # preview approximation: the same relief boxes as real geometry (three.js has no SPOM shader)
        pm = Mesh(); pm.prims = {k: list(v) for k, v in m.prims.items() if not k.startswith("spom_")}
        pm.prims.setdefault("detail", [])
        for (kind, maps_, boxes, origin) in vols:
            for b in boxes: pbox(pm, b, origin)
        os.makedirs(os.path.join(OUT, "preview"), exist_ok=True)
        g.write_glb(os.path.join(OUT, "preview", name + "_spom_preview.glb"), pm, {k: v for k, v in tex.items() if k in pm.prims}, mats)
        catalog.append({"file": name + ".glb", "preview": "preview/" + name + "_spom_preview.glb", "name": name, "lot": list(cells), "lot_m": [W, D],
                        "tris": tris, "spom_tris": spom_tris, "materials": len(m.prims), "kb": os.path.getsize(p) // 1024})
        print(f"{name}: {tris} tris ({spom_tris} are SPOM volumes), {len(m.prims)} materials, {os.path.getsize(p) // 1024} KB")
    json.dump(catalog, open(os.path.join(BASE, "catalog.json"), "w"), indent=1)
    open(os.path.join(BASE, "preview.html"), "w", encoding="utf-8").write(PREVIEW)


PREVIEW = """<!doctype html><meta charset="utf-8"><title>Bus depot</title>
<style>body{margin:0;background:#8aa0b4;font:13px sans-serif;overflow:hidden}#ui{position:fixed;top:8px;left:8px;background:#000b;color:#fff;padding:8px;border-radius:5px}
button{margin:2px}#lab{position:fixed;bottom:8px;left:8px;background:#000b;color:#fff;padding:6px 10px;border-radius:5px}</style>
<div id="ui"><label><input type="checkbox" id="wf"> wireframe</label><br><span id="info">preview shows SPOM volumes as real boxes (engine shader renders them as relief)</span></div><div id="lab"></div>
<script type="importmap">{"imports":{"three":"https://unpkg.com/three@0.160.0/build/three.module.js","three/addons/":"https://unpkg.com/three@0.160.0/examples/jsm/"}}</script>
<script type="module">
import * as THREE from 'three';import {OrbitControls} from 'three/addons/controls/OrbitControls.js';import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
const r=new THREE.WebGLRenderer({antialias:true,preserveDrawingBuffer:true});r.setSize(innerWidth,innerHeight);r.shadowMap.enabled=true;r.toneMapping=THREE.ACESFilmicToneMapping;document.body.appendChild(r.domElement);
const s=new THREE.Scene();s.background=new THREE.Color(0x8aa0b4);
const cam=new THREE.PerspectiveCamera(38,innerWidth/innerHeight,0.5,1500);cam.position.set(30,32,78);const oc=new OrbitControls(cam,r.domElement);oc.target.set(30,3,-4);
s.add(new THREE.HemisphereLight(0xdfe8ff,0x6b6a58,0.9));const sun=new THREE.DirectionalLight(0xfff0d8,2.4);sun.position.set(-60,120,90);sun.castShadow=true;
Object.assign(sun.shadow.camera,{left:-120,right:120,top:120,bottom:-120,far:500});sun.shadow.mapSize.set(2048,2048);sun.shadow.bias=-0.0004;s.add(sun);
const gnd=new THREE.Mesh(new THREE.PlaneGeometry(1000,1000),new THREE.MeshStandardMaterial({color:0x6d7f52}));gnd.rotation.x=-Math.PI/2;gnd.position.y=-0.05;gnd.receiveShadow=true;s.add(gnd);
const cat=await (await fetch('catalog.json')).json();const L=new GLTFLoader();const root=[];let x=0;
for(const e of cat){L.load('glb/'+(e.preview||e.file),g=>{g.scene.traverse(o=>{if(o.isMesh)o.castShadow=o.receiveShadow=true});g.scene.position.x=x+e.lot_m[0]/2;x+=e.lot_m[0]+12;s.add(g.scene);root.push(g.scene);});}
document.getElementById('lab').innerHTML=cat.map(e=>`<b>${e.name}</b> ${e.lot.join('x')} (${e.lot_m.join('x')} m) &mdash; ${e.tris} tris (${e.spom_tris} SPOM) , ${e.materials} mats`).join('<br>');
document.getElementById('wf').onchange=e=>root.forEach(g=>g.traverse(o=>{if(o.isMesh)o.material.wireframe=e.target.checked}));
r.setAnimationLoop(()=>{const w=innerWidth,h=innerHeight;if(w&&(r.domElement.width!==w||r.domElement.height!==h)){r.setSize(w,h);cam.aspect=w/h;cam.updateProjectionMatrix();}oc.update();r.render(s,cam)});
</script>"""

if __name__ == "__main__":
    main()
