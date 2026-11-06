"""Low-density residential house generator: 7 lot types x 5 levels -> GLB files.

Design: the mesh is a handful of boxes and roof planes (tens of triangles). All detail (siding, shingles, windows,
doors, trim) is carried by tiling albedo + normal maps, because the engine builds its parallax height from the
normal map (src/surface_maps.h) -- so strong normals are the relief. Three materials per house: wall, roof, detail.

Run:  python gen_low_density.py            (needs numpy + Pillow only)
Source textures (Gemini): assets/buildings/low_density/src_tex/L<level>_wall.jpg and L<level>_roof.jpg
Output: assets/buildings/low_density/glb/LD_<W>x<D>_L<level>.glb
Units: metres, +Y up, house front faces +Z. One lot cell is 8 m.
"""
import io, json, os, struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
sys.path.insert(0, os.path.dirname(__file__))
import spom

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC = os.path.join(ROOT, "assets", "buildings", "low_density", "src_tex")
OUT = os.path.join(ROOT, "assets", "buildings", "low_density", "glb")
TEX = os.path.join(ROOT, "assets", "buildings", "low_density", "tex")
CELL = 8.0
LOT_TYPES = [(2, 2), (2, 3), (3, 3), (2, 4), (3, 4), (4, 4), (4, 6)]  # wiki: low density runs 2x2 .. 4x6
TEX_SIZE = 512

# wall/roof tile size in metres, normal-map strength (high on purpose: the normal map is the geometry), palette
LEVELS = {
    1: dict(wall_m=2.4, roof_m=2.0, wall_k=9.0, roof_k=8.0, trim=(70, 62, 52), door=(60, 44, 32), glass=(30, 38, 44)),
    2: dict(wall_m=2.0, roof_m=2.0, wall_k=7.0, roof_k=7.0, trim=(235, 232, 224), door=(120, 40, 36), glass=(34, 44, 56)),
    3: dict(wall_m=2.4, roof_m=2.2, wall_k=8.0, roof_k=7.0, trim=(238, 236, 228), door=(40, 70, 60), glass=(30, 42, 54)),
    4: dict(wall_m=1.5, roof_m=2.4, wall_k=10.0, roof_k=8.0, trim=(90, 70, 52), door=(70, 46, 30), glass=(26, 38, 52)),
    5: dict(wall_m=3.2, roof_m=3.0, wall_k=6.0, roof_k=5.0, trim=(40, 42, 46), door=(150, 110, 70), glass=(40, 62, 80)),
}


# ---------------------------------------------------------------- textures
def seamless(img):
    """Make a tile wrap: cross-fade the image with a half-offset copy using a centre-weighted mask."""
    a = np.asarray(img, dtype=np.float32)
    h, w = a.shape[:2]
    b = np.roll(np.roll(a, h // 2, 0), w // 2, 1)
    y = np.abs(np.linspace(-1, 1, h))[:, None]
    x = np.abs(np.linspace(-1, 1, w))[None, :]
    t = np.clip((1.0 - np.maximum(y, x)) / 0.28, 0, 1)            # 1 across the middle, falling to 0 only in a narrow band at the seams
    m = (t * t * (3 - 2 * t))[..., None]                          # (a wide band blends two different crops into a ghost that repeats too)
    return np.clip(a * m + b * (1 - m), 0, 255).astype(np.uint8)


def height_from_albedo(rgb):
    g = rgb.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32) / 255.0
    im = Image.fromarray((g * 255).astype(np.uint8))
    low = np.asarray(im.filter(ImageFilter.GaussianBlur(12)), np.float32) / 255.0
    mid = np.asarray(im.filter(ImageFilter.GaussianBlur(2)), np.float32) / 255.0
    h = (mid - low) * 1.0 + 0.5 * (mid - 0.5)  # high-pass relief plus a little of the broad luminance
    return h


def normal_from_height(h, k):
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5  # per image row
    n = np.stack([-dx * k, dy * k, np.ones_like(h)], -1) * 1.0
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return ((n * 0.5 + 0.5) * 255).astype(np.uint8)


def png(arr):
    b = io.BytesIO(); Image.fromarray(arr).save(b, "PNG", optimize=True); return b.getvalue()


def jpg(arr, q=88):
    b = io.BytesIO(); Image.fromarray(arr).save(b, "JPEG", quality=q); return b.getvalue()


def delight(rgb, strength=0.9, radius=0.16):
    """Divide out the broad light and colour drift (dark patches, a bright corner): the cues the eye uses to spot a repeating tile."""
    a = rgb.astype(np.float32); h, w = a.shape[:2]
    big = Image.fromarray(np.tile(rgb, (3, 3, 1))).filter(ImageFilter.GaussianBlur(h * radius))   # tiled first, so the blur wraps
    low = np.asarray(big, np.float32)[h:2 * h, w:2 * w]
    flat = a / np.maximum(low, 10.0) * low.mean((0, 1), keepdims=True)
    return np.clip(a * (1 - strength) + flat * strength, 0, 255).astype(np.uint8)


def tile_maps(path, k):
    img = Image.open(path).convert("RGB").resize((TEX_SIZE, TEX_SIZE), Image.LANCZOS)
    rgb = delight(seamless(img))
    return jpg(rgb), png(normal_from_height(height_from_albedo(rgb), k))


def detail_sheet(p):
    """512x512 atlas: window (0,0), wide window (256,0), door (0,256), garage door (256,256), each 256 px."""
    S = 512
    col = Image.new("RGB", (S, S), p["trim"]); hgt = Image.new("L", (S, S), 128)
    dc, dh = ImageDraw.Draw(col), ImageDraw.Draw(hgt)

    def frame(x0, y0, x1, y1, fw, glass_cols, glass_rows, glass_col):
        dc.rectangle([x0, y0, x1, y1], fill=p["trim"]); dh.rectangle([x0, y0, x1, y1], fill=210)  # raised frame
        gx0, gy0, gx1, gy1 = x0 + fw, y0 + fw, x1 - fw, y1 - fw
        dc.rectangle([gx0, gy0, gx1, gy1], fill=glass_col); dh.rectangle([gx0, gy0, gx1, gy1], fill=40)  # recessed glass
        for i in range(1, glass_cols):  # muntins
            x = gx0 + (gx1 - gx0) * i // glass_cols
            dc.rectangle([x - 3, gy0, x + 3, gy1], fill=p["trim"]); dh.rectangle([x - 3, gy0, x + 3, gy1], fill=190)
        for j in range(1, glass_rows):
            y = gy0 + (gy1 - gy0) * j // glass_rows
            dc.rectangle([gx0, y - 3, gx1, y + 3], fill=p["trim"]); dh.rectangle([gx0, y - 3, gx1, y + 3], fill=190)
        # sill
        dc.rectangle([x0 - 6, y1 - 2, x1 + 6, y1 + 8], fill=p["trim"]); dh.rectangle([x0 - 6, y1 - 2, x1 + 6, y1 + 8], fill=235)

    glass = tuple(min(255, c + 26) for c in p["glass"])
    frame(40, 30, 216, 210, 16, 2, 3, glass)
    frame(268, 60, 500, 190, 14, 4, 2, glass)
    # front door: raised frame, recessed panels, transom light, knob
    dc.rectangle([60, 266, 196, 500], fill=p["trim"]); dh.rectangle([60, 266, 196, 500], fill=210)
    dc.rectangle([72, 282, 184, 500], fill=p["door"]); dh.rectangle([72, 282, 184, 500], fill=150)
    for (a, b, c, d) in [(86, 300, 170, 360), (86, 372, 170, 440), (86, 452, 170, 492)]:
        dc.rectangle([a, b, c, d], fill=tuple(int(v * 0.8) for v in p["door"])); dh.rectangle([a, b, c, d], fill=105)
    dc.rectangle([86, 272, 170, 292], fill=glass); dh.rectangle([86, 272, 170, 292], fill=45)
    dc.ellipse([160, 392, 172, 404], fill=(200, 180, 90)); dh.ellipse([160, 392, 172, 404], fill=240)
    # garage door: ribbed panels
    dc.rectangle([270, 270, 498, 500], fill=p["trim"]); dh.rectangle([270, 270, 498, 500], fill=200)
    for j in range(5):
        y0 = 282 + j * 44
        dc.rectangle([282, y0, 486, y0 + 36], fill=tuple(int(v * 0.92) for v in p["trim"])); dh.rectangle([282, y0, 486, y0 + 36], fill=150)
    rgb = np.asarray(col)
    h = np.asarray(hgt.filter(ImageFilter.GaussianBlur(1.2)), np.float32) / 255.0
    # a little paint noise so it is not flat vector art
    rng = np.random.default_rng(7)
    rgb = np.clip(rgb.astype(np.float32) * (0.94 + 0.12 * rng.random((S, S, 1))), 0, 255).astype(np.uint8)
    return jpg(rgb), png(normal_from_height(h, 5.0))


ATLAS = {"win": (0, 0, 256, 256), "wide": (256, 0, 512, 256), "door": (0, 256, 256, 512), "garage": (256, 256, 512, 512)}


# ---------------------------------------------------------------- mesh
class Mesh:
    def __init__(self):
        self.prims = {"wall": [], "roof": [], "detail": [], "spom_balcony": [], "spom_canopy": [], "spom_rail": [], "drive": [], "base": []}  # lists of (pos, nrm, uv) per vertex, 4 per quad / 3 per tri

    def quad(self, mat, p0, p1, p2, p3, uvs):
        """p0..p3 counter-clockwise seen from the front; uvs are 4 (u,v) pairs."""
        p = [np.array(v, np.float32) for v in (p0, p1, p2, p3)]
        n = np.cross(p[1] - p[0], p[3] - p[0]); n /= np.linalg.norm(n)
        self.prims[mat].append(([*p], n, list(uvs), 4))

    def tri(self, mat, p0, p1, p2, uvs):
        p = [np.array(v, np.float32) for v in (p0, p1, p2)]
        n = np.cross(p[1] - p[0], p[2] - p[0]); n /= np.linalg.norm(n)
        self.prims[mat].append(([*p], n, list(uvs), 3))

    def raw_quad(self, mat, pts, normal, uvs, cols):
        """A quad whose normal and per-vertex colour are data for the shader (silhouette-POM volumes: see spom.py)."""
        self.prims[mat].append(([np.array(v, np.float32) for v in pts], np.array(normal, np.float32), list(uvs), 4, list(cols)))

    def tri_count(self):
        return sum(2 if c[3] == 4 else 1 for l in self.prims.values() for c in l)


def wall_uv(p, axis_u, m):  # world-space tiling for a vertical wall
    return (p[axis_u] / m, -p[1] / m)


def box(mesh, x0, x1, y0, y1, z0, z1, wall_m, top="roof", roof_m=2.0, skip=(), uvoff=(0.0, 0.0)):
    """Axis-aligned box with wall-material sides and a flat top in `top` material; no bottom."""
    def side(name, a, b, c, d, axis):
        if name in skip: return
        mesh.quad("wall", a, b, c, d, [(wall_uv(q, axis, wall_m)[0] + uvoff[0], wall_uv(q, axis, wall_m)[1] + uvoff[1]) for q in (a, b, c, d)])
    side("front", (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), 0)
    side("back", (x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), 0)
    side("right", (x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), 2)
    side("left", (x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0), 2)
    if "top" not in skip:
        mesh.quad(top, (x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0),
                  [(q[0] / roof_m, q[2] / roof_m) for q in ((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0))])


def gable_roof(mesh, x0, x1, y, z0, z1, rise, over, wall_m, roof_m, ridge_axis="x"):
    """Gable roof over [x0,x1]x[z0,z1] at eave height y. Ridge runs along `ridge_axis`; gable ends use the wall material."""
    ox0, ox1, oz0, oz1 = x0 - over, x1 + over, z0 - over, z1 + over
    if ridge_axis == "x":  # ridge along X, slopes face +Z / -Z
        zm = (z0 + z1) / 2; yr = y + rise
        rl = np.hypot(oz1 - zm, rise)
        mesh.quad("roof", (ox0, y - over * rise / (z1 - zm), oz1), (ox1, y - over * rise / (z1 - zm), oz1), (ox1, yr, zm), (ox0, yr, zm),
                  [(ox0 / roof_m, 0), (ox1 / roof_m, 0), (ox1 / roof_m, rl / roof_m), (ox0 / roof_m, rl / roof_m)])
        mesh.quad("roof", (ox1, y - over * rise / (zm - z0), oz0), (ox0, y - over * rise / (zm - z0), oz0), (ox0, yr, zm), (ox1, yr, zm),
                  [(ox1 / roof_m, 0), (ox0 / roof_m, 0), (ox0 / roof_m, rl / roof_m), (ox1 / roof_m, rl / roof_m)])
        for xe, a, b in ((x1, (x1, y, z1), (x1, y, z0)), (x0, (x0, y, z0), (x0, y, z1))):
            mesh.tri("wall", a, b, (xe, yr, zm), [wall_uv(a, 2, wall_m), wall_uv(b, 2, wall_m), wall_uv((xe, yr, zm), 2, wall_m)])
    else:  # ridge along Z, slopes face +X / -X
        xm = (x0 + x1) / 2; yr = y + rise
        rl = np.hypot(ox1 - xm, rise)
        mesh.quad("roof", (ox1, y - over * rise / (x1 - xm), oz1), (ox1, y - over * rise / (x1 - xm), oz0), (xm, yr, oz0), (xm, yr, oz1),
                  [(oz1 / roof_m, 0), (oz0 / roof_m, 0), (oz0 / roof_m, rl / roof_m), (oz1 / roof_m, rl / roof_m)])
        mesh.quad("roof", (ox0, y - over * rise / (xm - x0), oz0), (ox0, y - over * rise / (xm - x0), oz1), (xm, yr, oz1), (xm, yr, oz0),
                  [(oz0 / roof_m, 0), (oz1 / roof_m, 0), (oz1 / roof_m, rl / roof_m), (oz0 / roof_m, rl / roof_m)])
        for ze, a, b in ((z1, (x0, y, z1), (x1, y, z1)), (z0, (x1, y, z0), (x0, y, z0))):
            mesh.tri("wall", a, b, (xm, yr, ze), [wall_uv(a, 0, wall_m), wall_uv(b, 0, wall_m), wall_uv((xm, yr, ze), 0, wall_m)])


def atlas_uv(name, u, v, inset=0.01):
    x0, y0, x1, y1 = ATLAS[name]
    return ((x0 + (x1 - x0) * (inset + u * (1 - 2 * inset))) / 512.0, (y0 + (y1 - y0) * (inset + (1 - v) * (1 - 2 * inset))) / 512.0)


GLASS_RECT = {"win": (56 / 256, 200 / 256, 46 / 256, 194 / 256), "wide": (26 / 256, 230 / 256, 74 / 256, 176 / 256)}


def deco(mesh, kind, cx, cy, z, w, h, facing=+1, axis="x", off=0.03):
    """Detail quad (window/door/garage) on a wall. facing=+1 front (+Z), -1 back, axis 'x' = wall runs along X;
    for side walls use axis 'z' with facing=+1 (+X) / -1 (-X) and cx = z position, z = wall x position."""
    x0, x1, y0, y1 = cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2
    if axis == "x":
        zz = z + off * facing
        pts = [(x0, y0, zz), (x1, y0, zz), (x1, y1, zz), (x0, y1, zz)] if facing > 0 else [(x1, y0, zz), (x0, y0, zz), (x0, y1, zz), (x1, y1, zz)]
    else:
        xx = z + off * facing
        pts = [(xx, y0, x1), (xx, y0, x0), (xx, y1, x0), (xx, y1, x1)] if facing > 0 else [(xx, y0, x0), (xx, y0, x1), (xx, y1, x1), (xx, y1, x0)]
    mesh.quad("detail", *pts, [atlas_uv(kind, 0, 0), atlas_uv(kind, 1, 0), atlas_uv(kind, 1, 1), atlas_uv(kind, 0, 1)])
    if kind in GLASS_RECT:                                    # the glass inside the frame: a roomglass pane (a parallax room, see roomglass.py)
        fx0, fx1, fy0, fy1 = GLASS_RECT[kind]                 # fractions of the cell: across, and down from its top
        if axis == "x": right = np.array([1.0 * facing, 0, 0]); p_ll = np.array([x0 if facing > 0 else x1, 0, z + (off + 0.025) * facing])
        else: right = np.array([0, 0, -1.0 * facing]); p_ll = np.array([z + (off + 0.025) * facing, 0, x1 if facing > 0 else x0])
        gw, gh = w * (fx1 - fx0), h * (fy1 - fy0)
        p0 = p_ll + right * w * fx0 + np.array([0, y1 - h * fy1, 0])
        import roomglass
        roomglass.pane(mesh, "roomglass", p0, right, (0, 1, 0), gw, gh, seed=float((cx * 7.31 + cy * 3.17 + z * 1.93) % 97.0))


def canopy_over(m, cx, top_y, z_wall, nx=0.0, nz=1.0):
    """A small canopy (silhouette POM) over an opening whose top edge is at top_y, centred on cx along the wall."""
    N = np.array([nx, 0, nz], np.float32); T = np.cross([0, 1, 0], N)
    spom.spom_box(m, "canopy", np.array([cx, top_y - 0.25, z_wall], np.float32) - T * spom.KINDS["canopy"][0] / 2, N)


def balcony_at(m, cx, floor_y, z_wall, nx=0.0, nz=1.0):
    """A balcony (silhouette POM) whose floor is at floor_y, centred on cx along the wall."""
    N = np.array([nx, 0, nz], np.float32); T = np.cross([0, 1, 0], N)
    spom.spom_box(m, "balcony", np.array([cx, floor_y - 0.34, z_wall], np.float32) - T * spom.KINDS["balcony"][0] / 2, N)


def build_house(W, D, level):
    """Return a Mesh for a lot of W x D cells at the given level (1..5). Front faces +Z, origin at lot centre on the ground."""
    p = LEVELS[level]; m = Mesh()
    lw, ld = W * CELL, D * CELL
    side, front, back = 1.5, 3.0, 2.0
    hw, hd = lw - 2 * side, ld - front - back  # house footprint
    # real low-density houses: roughly 6-14 m wide, 5-11 m deep, growing with level and lot size, never past the yards
    bw = {1: 6.0, 2: 7.5, 3: 8.5, 4: 9.0, 5: 10.0}[level] + 1.4 * (W - 2)
    bd = {1: 5.0, 2: 6.0, 3: 7.0, 4: 7.5, 5: 8.5}[level] + 0.9 * (D - 2)
    fw, fd = min(bw, hw), min(bd, hd)
    cx = 0.0; zc = (back - front) / 2.0
    x0, x1 = cx - fw / 2, cx + fw / 2
    z1 = zc + fd / 2; z0 = zc - fd / 2
    wm, rm = p["wall_m"], p["roof_m"]
    stories = {1: 1, 2: 1, 3: 2, 4: 2, 5: 3 if W * D >= 9 else 2}[level]
    sh = {1: 2.5, 2: 2.7, 3: 2.7, 4: 2.9, 5: 3.1}[level]
    H = stories * sh
    base = 0.25  # plinth above ground so the wall never z-fights the terrain
    if level == 5:  # modern: stacked boxes with a cantilevered upper volume and flat roofs with parapet lip
        box(m, x0, x1, 0, sh + base, z0, z1, wm, top="roof", roof_m=rm)
        ux0, ux1 = x0 - 0.0, x1 + 1.2
        uz1, uz0 = z1 + 1.8, z0 + fd * 0.2  # upper box cantilevers over the front
        box(m, ux0, ux1, sh + base, H + base, uz0, uz1, wm, top="roof", roof_m=rm)
        if stories == 3:
            box(m, x0 + fw * 0.15, x1 - fw * 0.35, H + base, H + sh * 0.9 + base, z0 + 1, z1 - 3, wm, top="roof", roof_m=rm)
        # soffit under the cantilever so it does not read as hollow
        m.quad("wall", (ux0, sh + base, uz1), (ux1, sh + base, uz1), (ux1, sh + base, z1), (ux0, sh + base, z1),
               [(ux0 / wm, 0), (ux1 / wm, 0), (ux1 / wm, 1), (ux0 / wm, 1)])
        deco(m, "wide", (ux0 + ux1) / 2 - 0.8, sh + base + sh / 2, uz1, 4.8, 2.4, +1)
        deco(m, "door", x0 + 1.4, base + 1.1, z1, 1.2, 2.2, +1)
        deco(m, "wide", x1 - 2.0, base + sh / 2, z1, 3.2, 1.8, +1)
        for zz in (z0 + fd * 0.45,):
            deco(m, "win", zz, base + sh / 2 + 0.2, x1, 1.6, 1.6, +1, axis="z")
            deco(m, "win", zz, sh + base + sh / 2, ux1, 1.6, 1.6, +1, axis="z")
        deco(m, "wide", cx, base + sh / 2, z0, 4.0, 1.8, -1)
        balcony_at(m, ux1 - 2.0, sh + base, uz1)               # on the cantilevered volume, second floor
        canopy_over(m, x0 + 1.4, base + 2.2, z1)               # over the front door, under the cantilever
        return m, (lw, ld, H + base)

    # body
    box(m, x0, x1, 0, H + base, z0, z1, wm, skip=("top",))
    # roof: ridge along the longer horizontal axis
    ridge = "x" if fw >= fd else "z"
    short = fd if ridge == "x" else fw
    rise = short * (0.20 if level == 1 else 0.30 if level == 2 else 0.36)
    gable_roof(m, x0, x1, H + base, z0, z1, rise, 0.55, wm, rm, ridge)
    # porch (levels 2+): a small roofed slab on the front with a lean-to roof; the entrance sits under it
    if level >= 2:
        pw = min(fw * 0.4, 4.5); px0 = x0 + 0.8; px1 = px0 + pw
        box(m, px0, px1, 0, 0.3, z1, z1 + 1.8, wm, top="roof", roof_m=rm, skip=("back",))  # deck
        for ppx in (px0 + 0.1, px1 - 0.25):  # two posts
            box(m, ppx, ppx + 0.15, 0.3, sh * 0.82 + base, z1 + 1.55, z1 + 1.7, wm, skip=("top",))
        py = sh * 0.82 + base
        m.quad("roof", (px0 - 0.2, py, z1 + 2.0), (px1 + 0.2, py, z1 + 2.0), (px1 + 0.2, py + 0.5, z1), (px0 - 0.2, py + 0.5, z1),
               [(0, 0), ((pw + 0.4) / rm, 0), ((pw + 0.4) / rm, 1), (0, 1)])
    # chimney (levels 3+): a brick box through the roof
    if level >= 3:
        chx = x0 + fw * 0.7
        box(m, chx, chx + 0.7, 0, H + base + rise + 0.9, (z0 + z1) / 2 - 0.35, (z0 + z1) / 2 + 0.35, wm * 0.7, skip=("top",))
        m.quad("roof", (chx, H + base + rise + 0.9, (z0 + z1) / 2 + 0.35), (chx + 0.7, H + base + rise + 0.9, (z0 + z1) / 2 + 0.35),
               (chx + 0.7, H + base + rise + 0.9, (z0 + z1) / 2 - 0.35), (chx, H + base + rise + 0.9, (z0 + z1) / 2 - 0.35), [(0, 0), (1, 0), (1, 1), (0, 1)])
    # garage wing (level 4, wide lots): a lower box on the right with a garage door
    gx1 = x1
    if level == 4 and W >= 3:
        gw = 3.8
        box(m, x1, x1 + gw, 0, sh + base, z0 + fd * 0.25, z1 - 0.8, wm, skip=("top",))
        gable_roof(m, x1, x1 + gw, sh + base, z0 + fd * 0.25, z1 - 0.8, 1.0, 0.45, wm, rm, "z")
        deco(m, "garage", x1 + gw / 2, base + 1.1, z1 - 0.8, 3.0, 2.2, +1)
        gx1 = x1 + gw
    # windows and door
    door_x = x0 + 0.8 + (min(fw * 0.4, 4.5) / 2 if level >= 2 else fw * 0.25)
    deco(m, "door", door_x, base + 1.1, z1 + (1.6 if False else 0), 1.1, 2.2, +1)
    nwin = max(1, int((fw - 2.0) // 3.0))
    for s in range(stories):
        for i in range(nwin):
            wx = x0 + (i + 0.5) * fw / nwin
            if s == 0 and abs(wx - door_x) < 1.6: continue
            deco(m, "win", wx, base + s * sh + sh * 0.55, z1, 1.2, 1.4, +1)
        for i in range(max(1, int(fw // 3.5))):  # back wall
            deco(m, "win", x0 + (i + 0.5) * fw / max(1, int(fw // 3.5)), base + s * sh + sh * 0.55, z0, 1.2, 1.4, -1)
        for i in range(max(1, int(fd // 3.5))):  # side walls
            zz = z0 + (i + 0.5) * fd / max(1, int(fd // 3.5))
            deco(m, "win", zz, base + s * sh + sh * 0.55, x0, 1.2, 1.4, -1, axis="z")
            deco(m, "win", zz, base + s * sh + sh * 0.55, x1, 1.2, 1.4, +1, axis="z")
    # silhouette-POM volumes: canopies and balconies stand out from the walls, their shape in the relief textures
    if level == 1:
        canopy_over(m, door_x, base + 2.2, z1)
    elif level == 2:
        for i in range(nwin):                                   # an awning over each ground-floor front window
            wx = x0 + (i + 0.5) * fw / nwin
            if abs(wx - door_x) >= 1.6: canopy_over(m, wx, base + sh * 0.55 + 0.7, z1)
    elif level == 3:
        balcony_at(m, x1 - 2.0, base + sh, z1)                  # second floor, away from the porch roof
    elif level == 4:
        balcony_at(m, x1 - 2.0, base + sh, z1)
        if W >= 3: canopy_over(m, gx1 - 1.9, base + 2.2, z1 - 0.8)  # over the garage door
    return m, (lw, ld, H + base + rise)


# ---------------------------------------------------------------- glb
def write_glb(path, mesh, tex, mats=None):
    """tex: dict wall/roof/detail -> (albedo_jpeg, normal_png). mats: optional per-material {rough, metal, blend}; blend with rough < 0.2
    makes a window pane (GlassPS in mesh.hlsl: Fresnel sky reflection over what lies behind)."""
    mats = dict(mats or {})
    if mesh.prims.get("roomglass") and "roomglass" not in tex:   # windows as parallax rooms: this building's own room atlas and wear mask
        import roomglass, zlib
        tex = dict(tex); tex["roomglass"] = roomglass.maps(zlib.crc32(os.path.basename(path).encode()) % 100000)
    for k in mesh.prims:
        if "roomglass" in k: mats.setdefault(k, {"rough": 0.05, "metal": 0.0})
    bin_ = bytearray(); views = []; accs = []; images = []; textures = []; materials = []; prims = []

    def add_view(data, target=None):
        while len(bin_) % 4: bin_.append(0)
        off = len(bin_); bin_.extend(data)
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target: v["target"] = target
        views.append(v); return len(views) - 1

    def add_img(data, mime):
        images.append({"bufferView": add_view(data), "mimeType": mime}); textures.append({"source": len(images) - 1}); return len(textures) - 1

    names = {"wall": "wall", "roof": "roof", "detail": "detail"}
    for name, items in mesh.prims.items():
        if not items: continue
        pos, nrm, uv, idx, col = [], [], [], [], []
        for item in items:
            pts, n, uvs, cnt = item[:4]
            base = len(pos)
            for vi, (q, t) in enumerate(zip(pts, uvs)):
                pos.append(q); nrm.append(n); uv.append(t)
                if len(item) > 4: col.append(item[4][vi])
            idx += [base, base + 1, base + 2] + ([base, base + 2, base + 3] if cnt == 4 else [])
        pa = np.array(pos, np.float32); na = np.array(nrm, np.float32); ua = np.array(uv, np.float32); ia = np.array(idx, np.uint16)
        accs += [
            {"bufferView": add_view(pa.tobytes(), 34962), "componentType": 5126, "count": len(pa), "type": "VEC3",
             "min": pa.min(0).tolist(), "max": pa.max(0).tolist()},
            {"bufferView": add_view(na.tobytes(), 34962), "componentType": 5126, "count": len(na), "type": "VEC3"},
            {"bufferView": add_view(ua.tobytes(), 34962), "componentType": 5126, "count": len(ua), "type": "VEC2"},
            {"bufferView": add_view(ia.tobytes(), 34963), "componentType": 5123, "count": len(ia), "type": "SCALAR"}]
        a = len(accs) - 4
        alb, nor = tex[name]
        is_spom = name.startswith("spom_")
        pbr = {"baseColorTexture": {"index": add_img(alb, "image/png" if (is_spom or alb[:4] == bytes([0x89, 0x50, 0x4E, 0x47])) else "image/jpeg")}, "metallicFactor": 0.0, "roughnessFactor": 0.9}
        if is_spom: pbr["baseColorFactor"] = [1, 1, 1, 1]   # the vertex colour is data here, not a tint
        mp = mats.get(name, {})
        if "rough" in mp: pbr["roughnessFactor"] = mp["rough"]
        if "metal" in mp: pbr["metallicFactor"] = mp["metal"]
        mat = {"name": name, "pbrMetallicRoughness": pbr, "normalTexture": {"index": add_img(nor, "image/jpeg" if nor[:2] == b"\xff\xd8" else "image/png"), "scale": 1.0}}
        if mp.get("blend"): mat["alphaMode"] = "BLEND"; pbr["baseColorFactor"] = [1, 1, 1, mp.get("alpha", 0.25)]
        materials.append(mat)
        attrs = {"POSITION": a, "NORMAL": a + 1, "TEXCOORD_0": a + 2}
        if col:
            ca = np.array(col, np.float32)
            accs.append({"bufferView": add_view(ca.tobytes(), 34962), "componentType": 5126, "count": len(ca), "type": "VEC3"})
            attrs["COLOR_0"] = len(accs) - 1
        prims.append({"attributes": attrs, "indices": a + 3, "material": len(materials) - 1})
    gltf = {"asset": {"version": "2.0", "generator": "cityengine gen_low_density"}, "scene": 0, "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0, "name": os.path.splitext(os.path.basename(path))[0]}], "meshes": [{"primitives": prims}],
            "materials": materials, "textures": textures, "images": images, "accessors": accs, "bufferViews": views,
            "buffers": [{"byteLength": len(bin_)}]}
    j = json.dumps(gltf, separators=(",", ":")).encode()
    j += b" " * (-len(j) % 4)
    while len(bin_) % 4: bin_.append(0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(j) + 8 + len(bin_)))
        f.write(struct.pack("<II", len(j), 0x4E4F534A)); f.write(j)
        f.write(struct.pack("<II", len(bin_), 0x004E4942)); f.write(bytes(bin_))


def main():
    os.makedirs(OUT, exist_ok=True); os.makedirs(TEX, exist_ok=True)
    only = [int(a) for a in sys.argv[1:]] or sorted(LEVELS)
    report = []
    for lv in only:
        p = LEVELS[lv]
        wp, rp = os.path.join(SRC, f"L{lv}_wall.jpg"), os.path.join(SRC, f"L{lv}_roof.jpg")
        if not (os.path.exists(wp) and os.path.exists(rp)):
            print(f"level {lv}: missing source textures, skipped"); continue
        tex = {"wall": tile_maps(wp, p["wall_k"]), "roof": tile_maps(rp, p["roof_k"]), "detail": detail_sheet(p),
               "spom_balcony": spom.relief("balcony", p), "spom_canopy": spom.relief("canopy", p)}
        for name, (a, n) in tex.items():  # keep the processed maps next to the GLBs for inspection
            open(os.path.join(TEX, f"L{lv}_{name}_albedo." + ("png" if name.startswith("spom_") else "jpg")), "wb").write(a); open(os.path.join(TEX, f"L{lv}_{name}_normal.png"), "wb").write(n)
        for (W, D) in LOT_TYPES:
            mesh, dims = build_house(W, D, lv)
            out = os.path.join(OUT, f"LD_{W}x{D}_L{lv}.glb")
            write_glb(out, mesh, tex)
            report.append((W, D, lv, mesh.tri_count(), os.path.getsize(out)))
            print(f"LD_{W}x{D}_L{lv}: {mesh.tri_count():3d} tris, {os.path.getsize(out)/1e6:.2f} MB, height {dims[2]:.1f} m")


if __name__ == "__main__":
    main()
