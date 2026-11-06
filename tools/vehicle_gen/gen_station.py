"""Passenger railway station -> GLB, sized for gen_train's intercity coaches on gen_track's track.

Layout (track centre line on x = 0, running along z; y = 0 is the top of the rail):
  +x  main platform (1.10 m high, edge 1.70 m from the track centre, 100 m long) with a cantilevered canopy on columns,
      benches, lamps, name boards, departure board, and the station building behind it: two-storey brick hall with
      tall arched-look windows, a clock tower, and a street entrance with a glazed porch facing +x.
  -x  second side platform with two glazed waiting shelters.
  A covered footbridge crosses the track between the platforms, with a stair tower on each.
Textures are procedural (PIL) and tiled with world-space UVs (1 uv unit = SCALE[mat] metres).
Run:  python gen_station.py
Output: assets/rail/station/glb/STATION_passenger.glb (+ the textures inside it)
"""
import io, math, os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import gen_cars as gc
import gen_transit_tex as tx
from gen_cars import Mesh

OUT = os.path.join(gc.ROOT, "assets", "rail", "station", "glb")
NAME = "UNION STATION"
PH, EDGE, Z0, Z1 = 1.10, 1.70, -60.0, 40.0       # platform height, edge offset, platform ends
GROUND = -0.55                                    # surrounding ground level (bottom of the track formation)
SCALE = {"concrete": 4.0, "paving": 2.0, "brick": 2.0, "roof": 2.0, "windows": 3.0, "tactile": 0.6, "steel": 1.0,
         "glass": 2.0, "wood": 1.0, "stone": 2.0}


# ---------------------------------------------------------------- textures
def _jpg(im):
    b = io.BytesIO(); im.convert("RGB").save(b, "JPEG", quality=88); return b.getvalue()


def _png(im):
    b = io.BytesIO(); im.save(b, "PNG"); return b.getvalue()


def _noise(n, amp, seed, blur=1.0):
    rng = np.random.default_rng(seed)
    a = Image.fromarray((128 + rng.normal(0, amp, (n, n))).clip(0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(blur))
    return np.asarray(a, float) - 128


def textures():
    T = {}
    n = 512
    # concrete: 4 m tile, joints every 2 m
    a = np.full((n, n, 3), (150, 148, 142), float) + _noise(n, 18, 1, 2)[..., None]
    a[:, :3] *= 0.6; a[:3] *= 0.6; a[:, n // 2 - 1:n // 2 + 1] *= 0.7; a[n // 2 - 1:n // 2 + 1] *= 0.7
    T["concrete"] = Image.fromarray(a.clip(0, 255).astype(np.uint8))
    # platform paving: 0.5 m slabs on a 2 m tile
    a = np.full((n, n, 3), (172, 166, 155), float) + _noise(n, 12, 2, 1)[..., None]
    for k in range(0, n, n // 4): a[k:k + 3] *= 0.65; a[:, k:k + 3] *= 0.65
    T["paving"] = Image.fromarray(a.clip(0, 255).astype(np.uint8))
    # brick: running bond, 2 m tile = 9 courses x ~9 bricks
    im = Image.new("RGB", (n, n), (190, 185, 175)); d = ImageDraw.Draw(im); rng = np.random.default_rng(3)
    rows, cols = 26, 9; bh, bw = n / rows, n / cols
    for r in range(rows):
        off = (r % 2) * bw / 2
        for c in range(-1, cols + 1):
            x0 = c * bw + off; col = tuple(int(v) for v in np.array((128, 52, 38)) * rng.uniform(0.8, 1.15))
            d.rectangle([x0 + 2, r * bh + 2, x0 + bw - 2, r * bh + bh - 2], fill=col)
    a = np.asarray(im, float) + _noise(n, 8, 4, 0.7)[..., None]
    T["brick"] = Image.fromarray(a.clip(0, 255).astype(np.uint8))
    # stone trim (sills, cornices, plinth)
    a = np.full((n, n, 3), (200, 192, 172), float) + _noise(n, 14, 5, 1.5)[..., None]
    T["stone"] = Image.fromarray(a.clip(0, 255).astype(np.uint8))
    # roof: dark standing-seam metal, seams every 0.5 m
    a = np.full((n, n, 3), (58, 66, 72), float) + _noise(n, 6, 6, 2)[..., None]
    for k in range(0, n, n // 4): a[:, k:k + 6] += 35
    T["roof"] = Image.fromarray(a.clip(0, 255).astype(np.uint8))
    # windows: 3 m tile = one bay; dark glass with reflections, white frame + mullions, lit interior hint
    im = Image.new("RGB", (n, n), (230, 228, 220)); d = ImageDraw.Draw(im)
    g = np.zeros((n, n, 3)); yy = np.linspace(0, 1, n)[:, None]
    g[..., 0] = 40 + 60 * yy; g[..., 1] = 60 + 70 * yy; g[..., 2] = 80 + 70 * yy
    glass = Image.fromarray(g.astype(np.uint8))
    m = 40; im.paste(glass.crop((m, m, n - m, n - m)), (m, m))
    for x in (n // 3, 2 * n // 3): d.rectangle([x - 5, m, x + 5, n - m], fill=(230, 228, 220))
    d.rectangle([m, n // 4 - 5, n - m, n // 4 + 5], fill=(230, 228, 220))
    T["windows"] = im
    # platform-edge tactile strip: yellow with truncated domes
    im = Image.new("RGB", (128, 128), (225, 185, 20)); d = ImageDraw.Draw(im)
    for i in range(4):
        for j in range(4):
            cx, cy = 16 + 32 * i, 16 + 32 * j; d.ellipse([cx - 8, cy - 8, cx + 8, cy + 8], fill=(245, 210, 50), outline=(170, 135, 10))
    T["tactile"] = im
    # painted steel (columns, rails, frames) and canopy glazing, timber bench slats
    T["steel"] = Image.fromarray((np.full((64, 64, 3), (36, 70, 60), float) + _noise(64, 4, 7)[..., None]).clip(0, 255).astype(np.uint8))
    a = np.full((256, 256, 3), (150, 185, 195), float) + _noise(256, 6, 8, 3)[..., None]; a[:, :6] = (40, 70, 60); a[:6] = (40, 70, 60)
    T["glass"] = Image.fromarray(a.clip(0, 255).astype(np.uint8))
    a = np.full((128, 128, 3), (135, 90, 52), float) + _noise(128, 10, 9, 0.6)[..., None] * np.array([1, 0.8, 0.6])
    for k in range(0, 128, 32): a[k:k + 4] *= 0.5
    T["wood"] = Image.fromarray(a.clip(0, 255).astype(np.uint8))
    # name board: white lettering on navy, with a rail logo
    im = Image.new("RGB", (1024, 128), (20, 40, 90)); d = ImageDraw.Draw(im)
    d.rectangle([4, 4, 1019, 123], outline=(240, 240, 240), width=4)
    f = tx.font(80); w = d.textlength(NAME, font=f); d.text(((1024 - w) / 2, 22), NAME, font=f, fill=(245, 245, 245))
    T["namesign"] = im
    # departure board: amber dot-matrix list
    im = Image.new("RGB", (512, 256), (10, 10, 12)); d = ImageDraw.Draw(im); f = tx.font(26, False)
    d.rectangle([0, 0, 511, 34], fill=(20, 40, 90)); d.text((12, 4), "DEPARTURES", font=tx.font(26), fill=(255, 255, 255))
    rows = [("08:15", "204 CHICAGO", "1"), ("08:42", "17 SEATTLE", "2"), ("09:05", "9 NEW ORLEANS", "1"), ("09:30", "88 BOSTON", "2"),
            ("10:10", "204 CHICAGO", "1"), ("10:55", "17 SEATTLE", "2")]
    for i, (tm, dest, pl) in enumerate(rows):
        y = 44 + i * 35; c = (255, 175, 30)
        d.text((12, y), tm, font=f, fill=c); d.text((110, y), dest, font=f, fill=c); d.text((470, y), pl, font=f, fill=c)
    T["board"] = im
    # clock face
    im = Image.new("RGB", (256, 256), (40, 40, 40)); d = ImageDraw.Draw(im)
    d.ellipse([8, 8, 248, 248], fill=(245, 240, 225), outline=(30, 30, 30), width=8)
    for k in range(12):
        a_ = k * math.pi / 6; r0, r1 = (92, 112) if k % 3 else (82, 112)
        d.line([128 + r0 * math.sin(a_), 128 - r0 * math.cos(a_), 128 + r1 * math.sin(a_), 128 - r1 * math.cos(a_)], fill=(20, 20, 20), width=6)
    for a_, r, w in ((math.radians(10 * 30 + 4), 60, 9), (math.radians(10 * 6), 95, 6)):  # 10:10
        d.line([128, 128, 128 + r * math.sin(a_), 128 - r * math.cos(a_)], fill=(20, 20, 20), width=w)
    T["clock"] = im
    # entrance doors: glazed double doors with a transom
    im = Image.new("RGB", (256, 256), (36, 70, 60)); d = ImageDraw.Draw(im)
    for x0 in (14, 134): d.rectangle([x0, 70, x0 + 108, 248], fill=(70, 95, 110))
    d.rectangle([14, 10, 242, 58], fill=(90, 115, 130)); d.rectangle([118, 140, 124, 180], fill=(200, 200, 190)); d.rectangle([132, 140, 138, 180], fill=(200, 200, 190))
    T["entrance"] = im
    # lamp glow
    T["lamp"] = Image.new("RGB", (16, 16), (255, 236, 190))
    return {k: (_png(v) if k in ("namesign", "board", "clock", "tactile") else _jpg(v), None) for k, v in T.items()}


# ---------------------------------------------------------------- geometry helpers (world-space UVs)
def _uv(mat, p, nrm):
    s = SCALE.get(mat, 1.0); ax = np.argmax(np.abs(nrm))
    if ax == 0: return (p[2] / s, -p[1] / s)
    if ax == 1: return (p[0] / s, p[2] / s)
    return (p[0] / s, -p[1] / s)


def quad(m, mat, P, nrm, uv=None):
    nrm = np.array(nrm, float)
    m.quad(mat, [tuple(p) for p in P], uv or [_uv(mat, p, nrm) for p in P], nrm)


def box(m, mat, x0, x1, y0, y1, z0, z1, bottom=False, faces="tbfnlr"):
    x0, x1 = sorted((x0, x1)); y0, y1 = sorted((y0, y1)); z0, z1 = sorted((z0, z1))
    if "t" in faces: quad(m, mat, [(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], (0, 1, 0))
    if bottom: quad(m, mat, [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], (0, -1, 0))
    if "f" in faces: quad(m, mat, [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, 1))
    if "b" in faces: quad(m, mat, [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)], (0, 0, -1))
    if "r" in faces: quad(m, mat, [(x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0)], (1, 0, 0))
    if "l" in faces: quad(m, mat, [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)], (-1, 0, 0))


FULL = [(0, 1), (1, 1), (1, 0), (0, 0)]


def panel_x(m, mat, x, s, z0, z1, y0, y1):
    """Whole-texture quad on a plane x = const facing s*x (u runs left->right seen from outside)."""
    zz = (z1, z0) if s > 0 else (z0, z1)
    quad(m, mat, [(x, y0, zz[0]), (x, y0, zz[1]), (x, y1, zz[1]), (x, y1, zz[0])], (s, 0, 0), FULL)


def panel_z(m, mat, z, s, x0, x1, y0, y1):
    xx = (x0, x1) if s > 0 else (x1, x0)
    quad(m, mat, [(xx[0], y0, z), (xx[1], y0, z), (xx[1], y1, z), (xx[0], y1, z)], (0, 0, s), FULL)


def post(m, mat, x, z, y0, y1, r=0.12):
    box(m, mat, x - r, x + r, y0, y1, z - r, z + r)


# ---------------------------------------------------------------- station parts
def platform(m, s, x_out):
    """Side platform: concrete wall facing the track, paved top, white coping and a tactile warning strip."""
    xe, xo = s * EDGE, s * x_out
    box(m, "concrete", xe, xo, -0.6, PH - 0.05, Z0, Z1, faces="fbnlr")
    box(m, "stone", xe - s * 0.0, xe + s * 0.45, PH - 0.05, PH, Z0, Z1, faces="t" + ("l" if s > 0 else "r") + "fb")  # coping
    box(m, "tactile", xe + s * 0.45, xe + s * 1.05, PH - 0.05, PH, Z0, Z1, faces="t")
    box(m, "paving", xe + s * 1.05, xo, PH - 0.05, PH, Z0, Z1, faces="t")
    # recess under the coping (the dark refuge face) - a slightly darker band just below the edge
    for zc in (Z0, Z1):                                                     # end ramps
        d = -1 if zc == Z0 else 1
        quad(m, "paving", [(xe + s * 1.05, PH, zc), (xo, PH, zc), (xo, GROUND, zc + d * 8.0), (xe + s * 1.05, GROUND, zc + d * 8.0)][::s * d if s * d > 0 else -1], (0, 1, d * 0.14))
        for x in (xe + s * 1.05, xo):
            quad(m, "concrete", [(x, GROUND, zc), (x, PH, zc), (x, GROUND, zc + d * 8.0), (x, GROUND, zc + d * 8.0)], (s if x == xo else -s, 0, 0))
        for x in np.linspace(xe + s * 1.2, xo - s * 0.15, 2):           # ramp handrails
            for t_ in np.linspace(0, 1, 4):
                post(m, "steel", x, zc + d * 8.0 * t_, GROUND + (PH - GROUND) * (1 - t_), GROUND + (PH - GROUND) * (1 - t_) + 1.0, 0.03)
            quad(m, "steel", [(x, PH + 1.0, zc), (x, PH + 0.95, zc), (x, GROUND + 0.95, zc + d * 8.0), (x, GROUND + 1.0, zc + d * 8.0)], (1, 0, 0))
            quad(m, "steel", [(x, PH + 0.95, zc), (x, PH + 1.0, zc), (x, GROUND + 1.0, zc + d * 8.0), (x, GROUND + 0.95, zc + d * 8.0)], (-1, 0, 0))


def canopy(m, x_col, x0, x1, z0, z1, h, pitch=8.0):
    """Cantilevered canopy: steel columns with brackets, sloped standing-seam roof, glazed rooflight, fascia board."""
    zs = np.arange(z0 + 2.0, z1 - 1.0, pitch)
    for z in zs:
        post(m, "steel", x_col, z, PH, PH + h, 0.13)
        for xa, xb in ((x_col, x0 + 0.3), (x_col, x1 - 0.3)):              # brackets
            ya, yb = PH + h - 0.9, PH + h + (0.25 if xb < x_col else -0.05)
            quad(m, "steel", [(xa, ya, z - 0.06), (xa, PH + h, z - 0.06), (xb, yb, z - 0.06), (xb, yb - 0.25, z - 0.06)], (0, 0, -1))
            quad(m, "steel", [(xa, ya, z + 0.06), (xb, yb - 0.25, z + 0.06), (xb, yb, z + 0.06), (xa, PH + h, z + 0.06)], (0, 0, 1))
    yt0, yt1 = PH + h + 0.35, PH + h - 0.10                                  # roof slopes down away from the track
    for (xa, ya), (xb, yb) in (((x0, yt0), (x_col - 0.6, (yt0 * 2 + yt1) / 3)), ((x_col + 0.6, (yt0 + yt1 * 2) / 3), (x1, yt1))):
        quad(m, "roof", [(xa, ya, z0), (xa, ya, z1), (xb, yb, z1), (xb, yb, z0)], (0, 1, 0))
        quad(m, "roof", [(xa, ya - 0.08, z0), (xb, yb - 0.08, z0), (xb, yb - 0.08, z1), (xa, ya - 0.08, z1)], (0, -1, 0))
    ym0, ym1 = (yt0 * 2 + yt1) / 3, (yt0 + yt1 * 2) / 3                      # rooflight between the slopes
    quad(m, "glass", [(x_col - 0.6, ym0, z0), (x_col - 0.6, ym0, z1), (x_col + 0.6, ym1, z1), (x_col + 0.6, ym1, z0)], (0, 1, 0))
    quad(m, "glass", [(x_col - 0.6, ym0 - 0.02, z0), (x_col + 0.6, ym1 - 0.02, z0), (x_col + 0.6, ym1 - 0.02, z1), (x_col - 0.6, ym0 - 0.02, z1)], (0, -1, 0))
    box(m, "steel", x0 - 0.05, x0 + 0.05, yt0 - 0.45, yt0 + 0.05, z0, z1)   # fascia on the track side
    box(m, "steel", x1 - 0.05, x1 + 0.05, yt1 - 0.30, yt1 + 0.05, z0, z1)
    for z in (z0, z1):
        quad(m, "steel", [(x0, yt0 - 0.45, z), (x0, yt0 + 0.05, z), (x1, yt1 + 0.05, z), (x1, yt1 - 0.30, z)][::1 if z == z0 else -1], (0, 0, -1 if z == z0 else 1))
    return zs


def bench(m, x, z, rot_x=False):
    """Timber slat bench on steel legs, 1.8 m long along z, facing +-x."""
    for dz in (-0.75, 0.75):
        box(m, "steel", x - 0.25, x + 0.25, PH, PH + 0.45, z + dz - 0.03, z + dz + 0.03)
        box(m, "steel", x + 0.15, x + 0.20, PH + 0.45, PH + 0.90, z + dz - 0.03, z + dz + 0.03)
    box(m, "wood", x - 0.25, x + 0.20, PH + 0.42, PH + 0.47, z - 0.9, z + 0.9, bottom=True)
    box(m, "wood", x + 0.18, x + 0.24, PH + 0.55, PH + 0.88, z - 0.9, z + 0.9)


def lamp(m, x, z, h=4.2):
    post(m, "steel", x, z, PH, PH + h, 0.06)
    box(m, "steel", x - 0.25, x + 0.25, PH + h, PH + h + 0.08, z - 0.15, z + 0.15)
    box(m, "lamp", x - 0.22, x + 0.22, PH + h - 0.05, PH + h, z - 0.12, z + 0.12, bottom=True, faces="")


def namesign(m, x, z, y=2.6, both=True):
    post(m, "steel", x, z - 1.4, PH, PH + y + 0.5, 0.05); post(m, "steel", x, z + 1.4, PH, PH + y + 0.5, 0.05)
    box(m, "steel", x - 0.04, x + 0.04, PH + y - 0.02, PH + y + 0.52, z - 1.55, z + 1.55)
    panel_x(m, "namesign", x + 0.07, 1, z - 1.5, z + 1.5, PH + y + 0.02, PH + y + 0.48)
    panel_x(m, "namesign", x - 0.07, -1, z - 1.5, z + 1.5, PH + y + 0.02, PH + y + 0.48)


def bin_(m, x, z):
    box(m, "steel", x - 0.22, x + 0.22, PH, PH + 0.9, z - 0.22, z + 0.22)


def shelter(m, s, xc, zc, L=6.0):
    """Glazed waiting shelter: steel frame, glass back and ends, flat roof, bench inside."""
    xb = xc + s * 0.9
    for z in np.linspace(zc - L / 2, zc + L / 2, 4): post(m, "steel", xb, z, PH, PH + 2.5, 0.05)
    for z in (zc - L / 2, zc + L / 2): post(m, "steel", xc - s * 0.6, z, PH, PH + 2.5, 0.05)
    panel_x(m, "glass", xb, s, zc - L / 2, zc + L / 2, PH + 0.1, PH + 2.4); panel_x(m, "glass", xb - s * 0.04, -s, zc - L / 2, zc + L / 2, PH + 0.1, PH + 2.4)
    for z, d in ((zc - L / 2, -1), (zc + L / 2, 1)):
        x0, x1 = sorted((xc - s * 0.6, xb))
        panel_z(m, "glass", z, d, x0, x1, PH + 0.1, PH + 2.4); panel_z(m, "glass", z - d * 0.04, -d, x0, x1, PH + 0.1, PH + 2.4)
    box(m, "roof", xc - s * 0.9, xb + s * 0.15, PH + 2.5, PH + 2.62, zc - L / 2 - 0.2, zc + L / 2 + 0.2, bottom=True)
    bench(m, xb - s * 0.3, zc - 1.2) if s > 0 else bench_l(m, xb - s * 0.3, zc - 1.2)
    bench(m, xb - s * 0.3, zc + 1.2) if s > 0 else bench_l(m, xb - s * 0.3, zc + 1.2)


def bench_l(m, x, z):
    """Bench facing +x (back on the -x side)."""
    for dz in (-0.75, 0.75):
        box(m, "steel", x - 0.25, x + 0.25, PH, PH + 0.45, z + dz - 0.03, z + dz + 0.03)
        box(m, "steel", x - 0.20, x - 0.15, PH + 0.45, PH + 0.90, z + dz - 0.03, z + dz + 0.03)
    box(m, "wood", x - 0.20, x + 0.25, PH + 0.42, PH + 0.47, z - 0.9, z + 0.9, bottom=True)
    box(m, "wood", x - 0.24, x - 0.18, PH + 0.55, PH + 0.88, z - 0.9, z + 0.9)


def building(m, x0, x1, z0, z1):
    """Two-storey brick station hall: stone plinth and cornice, window bays on all sides, hipped-looking parapet roof,
    clock tower in the middle of the platform side, glazed entrance porch on the street side."""
    H1, H = PH + 4.5, PH + 9.0
    box(m, "brick", x0, x1, GROUND, H, z0, z1)
    box(m, "stone", x0 - 0.08, x1 + 0.08, GROUND, PH + 0.6, z0 - 0.08, z1 + 0.08)                      # plinth
    box(m, "stone", x0 - 0.15, x1 + 0.15, H1 - 0.15, H1 + 0.15, z0 - 0.15, z1 + 0.15)              # string course
    box(m, "stone", x0 - 0.25, x1 + 0.25, H - 0.1, H + 0.35, z0 - 0.25, z1 + 0.25)                 # cornice
    box(m, "stone", x0, x1, H + 0.35, H + 1.0, z0, z0 + 0.3); box(m, "stone", x0, x1, H + 0.35, H + 1.0, z1 - 0.3, z1)  # parapet
    box(m, "stone", x0, x0 + 0.3, H + 0.35, H + 1.0, z0, z1); box(m, "stone", x1 - 0.3, x1, H + 0.35, H + 1.0, z0, z1)
    # roof: low hip inside the parapet
    xm, zm, rh = (x0 + x1) / 2, (z0 + z1) / 2, 2.2; i = 0.6
    a, b, c, d = (x0 + i, H + 0.4, z0 + i), (x1 - i, H + 0.4, z0 + i), (x1 - i, H + 0.4, z1 - i), (x0 + i, H + 0.4, z1 - i)
    r0, r1 = (xm, H + rh, z0 + (x1 - x0) / 2), (xm, H + rh, z1 - (x1 - x0) / 2)
    quad(m, "roof", [a, d, r1, r0], (-1, 1, 0)); quad(m, "roof", [b, r0, r1, c], (1, 1, 0))
    m.tri("roof", [a, r0, b], [_uv("roof", p, (0, 0, 1)) for p in (a, r0, b)], np.array([0, 1, -1]))
    m.tri("roof", [c, r1, d], [_uv("roof", p, (0, 0, 1)) for p in (c, r1, d)], np.array([0, 1, 1]))
    # window bays: 3 m pitch, both floors, all four sides
    for s, x in ((1, x1 + 0.03), (-1, x0 - 0.03)):
        n = int((z1 - z0) // 3.0); zs = np.linspace(z0, z1, n + 1)
        for k in range(n):
            za, zb = zs[k] + 0.7, zs[k + 1] - 0.7
            if s > 0 and k in (n // 2 - 1, n // 2): continue                                       # street entrance
            for ya, yb in ((PH + 1.0, H1 - 0.6), (H1 + 0.8, H - 0.9)):
                panel_x(m, "windows", x, s, za, zb, ya, yb)
                box(m, "stone", x - s * 0.05, x + s * 0.12, ya - 0.15, ya, za - 0.1, zb + 0.1)           # sill
                box(m, "stone", x - s * 0.05, x + s * 0.08, yb, yb + 0.35, za - 0.15, zb + 0.15)         # lintel
    for s, z in ((1, z1 + 0.03), (-1, z0 - 0.03)):
        n = int((x1 - x0) // 3.0); xs = np.linspace(x0, x1, n + 1)
        for k in range(n):
            xa, xb = xs[k] + 0.7, xs[k + 1] - 0.7
            for ya, yb in ((PH + 1.0, H1 - 0.6), (H1 + 0.8, H - 0.9)):
                panel_z(m, "windows", z, s, xa, xb, ya, yb)
                box(m, "stone", xa - 0.1, xb + 0.1, ya - 0.15, ya, z, z + s * 0.12)
    # platform-side doors (two pairs) under the canopy
    for zc in ((z0 + z1) / 2 - 6.0, (z0 + z1) / 2 + 6.0):
        panel_x(m, "entrance", x0 - 0.03, -1, zc - 1.3, zc + 1.3, PH, PH + 3.2)
    # street entrance: glazed porch with a canopy and the station name over it, steps down to street level (y = 0)
    zc = (z0 + z1) / 2; pw = 4.2
    box(m, "stone", x1, x1 + 3.0, PH + 4.0, PH + 4.4, zc - pw - 0.5, zc + pw + 0.5, bottom=True)
    for z in (zc - pw - 0.3, zc + pw + 0.3): post(m, "stone", x1 + 2.7, z, GROUND, PH + 4.0, 0.22)
    panel_x(m, "entrance", x1 + 0.03, 1, zc - pw, zc - 0.2, PH, PH + 3.6); panel_x(m, "entrance", x1 + 0.03, 1, zc + 0.2, zc + pw, PH, PH + 3.6)
    panel_x(m, "namesign", x1 + 3.03, 1, zc - pw, zc + pw, PH + 4.42, PH + 5.2)
    box(m, "steel", x1 + 2.95, x1 + 3.0, PH + 4.4, PH + 5.22, zc - pw, zc + pw)
    for k in range(1, 6):                                                                           # steps down to the street
        box(m, "stone", x1, x1 + 3.0 + k * 0.32, GROUND, PH - k * (PH - GROUND) / 6, zc - pw, zc + pw)
    box(m, "stone", x1, x1 + 3.0, GROUND, PH, zc - pw - 0.5, zc + pw + 0.5)
    # clock tower on the platform side
    tx0, tx1, tz0, tz1, th = x0 - 0.5, x0 + 4.5, zc - 2.5, zc + 2.5, H + 7.0
    box(m, "brick", tx0, tx1, H, th, tz0, tz1)
    box(m, "stone", tx0 - 0.2, tx1 + 0.2, th, th + 0.4, tz0 - 0.2, tz1 + 0.2)
    for x, s in ((tx0 - 0.03, -1), (tx1 + 0.03, 1)): panel_x(m, "clock", x, s, zc - 1.6, zc + 1.6, th - 4.0, th - 0.8)
    for z, s in ((tz0 - 0.03, -1), (tz1 + 0.03, 1)): panel_z(m, "clock", z, s, (tx0 + tx1) / 2 - 1.6, (tx0 + tx1) / 2 + 1.6, th - 4.0, th - 0.8)
    xm_ = (tx0 + tx1) / 2; top = (xm_, th + 3.2, zc)                                                 # pyramid spire
    cs = [(tx0 - 0.2, th + 0.4, tz0 - 0.2), (tx1 + 0.2, th + 0.4, tz0 - 0.2), (tx1 + 0.2, th + 0.4, tz1 + 0.2), (tx0 - 0.2, th + 0.4, tz1 + 0.2)]
    for k in range(4):
        p, q = cs[k], cs[(k + 1) % 4]; nrm = np.cross(np.subtract(q, p), np.subtract(top, p))
        m.tri("roof", [p, q, top], [(0, 0), (2, 0), (1, 1.5)], -nrm)
    post(m, "steel", xm_, zc, th + 3.2, th + 4.6, 0.04)


def footbridge(m, zc, xa, xb, w=2.6):
    """Covered footbridge between the platforms: stair towers on each platform, glazed span over the track."""
    yd = PH + 6.2                                                                                    # deck (clearance ~6 m)
    for xc in (xa, xb):
        box(m, "brick", xc - 1.6, xc + 1.6, PH, yd + 3.0, zc - 3.5, zc + 3.5)
        box(m, "roof", xc - 1.8, xc + 1.8, yd + 3.0, yd + 3.25, zc - 3.7, zc + 3.7, bottom=True)
        for s in (-1, 1): panel_z(m, "entrance", zc + s * 3.53, s, xc - 1.1, xc + 1.1, PH, PH + 3.0)
        for s in (-1, 1):
            for ya in (PH + 4.0,):
                panel_x(m, "windows", xc + s * 1.63, s, zc - 2.5, zc + 2.5, ya, ya + 2.2)
    x0, x1 = xa + 1.6, xb - 1.6
    x0, x1 = min(x0, x1), max(x0, x1)
    box(m, "steel", x0, x1, yd - 0.6, yd, zc - w / 2, zc + w / 2, bottom=True)                        # deck girder
    for s in (-1, 1):
        panel_z(m, "glass", zc + s * w / 2, s, x0, x1, yd, yd + 2.5)
        panel_z(m, "glass", zc + s * (w / 2 - 0.04), -s, x0, x1, yd, yd + 2.5)
        box(m, "steel", x0, x1, yd + 2.45, yd + 2.6, zc + s * w / 2 - 0.08, zc + s * w / 2 + 0.08)
        for x in np.linspace(x0, x1, 5): post(m, "steel", x, zc + s * w / 2, yd, yd + 2.5, 0.06)
    quad(m, "roof", [(x0, yd + 2.6, zc - w / 2 - 0.2), (x0, yd + 2.9, zc), (x1, yd + 2.9, zc), (x1, yd + 2.6, zc - w / 2 - 0.2)], (0, 1, -1))
    quad(m, "roof", [(x0, yd + 2.9, zc), (x0, yd + 2.6, zc + w / 2 + 0.2), (x1, yd + 2.6, zc + w / 2 + 0.2), (x1, yd + 2.9, zc)], (0, 1, 1))


def departure_board(m, x, z):
    post(m, "steel", x, z, PH + 3.0, PH + 4.35, 0.04)
    box(m, "steel", x - 0.08, x + 0.08, PH + 2.35, PH + 3.05, z - 0.75, z + 0.75)
    panel_x(m, "board", x + 0.10, 1, z - 0.7, z + 0.7, PH + 2.38, PH + 3.02)
    panel_x(m, "board", x - 0.10, -1, z - 0.7, z + 0.7, PH + 2.38, PH + 3.02)


def build():
    m = Mesh()
    XE = 9.0                                                                                          # main platform width
    platform(m, 1, EDGE + XE)
    platform(m, -1, EDGE + 5.0)
    bz0, bz1 = -32.0, 4.0
    building(m, EDGE + XE, EDGE + XE + 13.0, bz0, bz1)
    zs = canopy(m, EDGE + 5.6, EDGE + 1.2, EDGE + XE + 0.05, -52.0, 30.0, 4.4)
    for k, z in enumerate(zs[:-1]):                                                                   # furniture between columns
        zm = z + 4.0
        if k % 3 == 0: bench(m, EDGE + 4.4, zm)
        elif k % 3 == 1: bin_(m, EDGE + 5.6, zm)
        else: departure_board(m, EDGE + 5.6, zm)
    for z in (-56.0, 34.0): namesign(m, EDGE + 4.0, z)
    for z in (-14.0, 14.0): namesign(m, EDGE + 2.6, z, y=3.1)
    for z in np.arange(-56.0, 39.0, 10.0): lamp(m, -(EDGE + 3.8), z)
    for z in (-56.0, -38.0, 34.0): lamp(m, EDGE + 3.8, z)
    shelter(m, -1, -(EDGE + 3.2), -24.0); shelter(m, -1, -(EDGE + 3.2), 8.0)
    for z in (-46.0, -6.0, 24.0): namesign(m, -(EDGE + 2.6), z)
    for z in (-36.0, -12.0, 18.0): bin_(m, -(EDGE + 4.4), z)
    footbridge(m, 22.0, EDGE + 7.4, -(EDGE + 3.4))
    return m


def main():
    os.makedirs(OUT, exist_ok=True)
    maps = textures()
    maps["glass"] = maps["glass"]
    gc.MAT_PBR.update({"concrete": (0, 0.9), "paving": (0, 0.85), "brick": (0, 0.9), "stone": (0, 0.8), "roof": (0.6, 0.45),
                       "windows": (0.2, 0.15), "tactile": (0, 0.7), "steel": (0.5, 0.4), "glass": (0.1, 0.1), "wood": (0, 0.7),
                       "namesign": (0.1, 0.4), "board": (0, 0.3), "clock": (0, 0.5), "entrance": (0.3, 0.3), "lamp": (0, 0.5)})
    m = build()
    out = os.path.join(OUT, "STATION_passenger.glb")
    gc.write_glb(out, m, maps, (1, 1, 1))
    print(f"station: {m.count()} tris, {os.path.getsize(out) / 1e6:.2f} MB -> {out}")


if __name__ == "__main__":
    main()
