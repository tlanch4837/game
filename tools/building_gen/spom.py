"""Silhouette-POM volumes (balconies, canopies) for the building generators. See SpomPS in src/shaders/mesh.hlsl.

A volume is a plain 5-face box standing out from a wall; the shape is a two-layer relief texture:
  albedo.a = where solid ENDS  (metres out from the wall / DEPTH), normal.a = where solid STARTS (same unit); empty if end <= start.
Vertex data the shader reads raw: normal = the facade's outward normal on every face, uv = (x, y) metres from the volume's
lower-left corner on the wall (x along T = cross(up, N), y up), color = (width, height, z of this vertex out from the wall).
"""
import io
import numpy as np
from PIL import Image, ImageFilter

DEPTH = 1.2            # kSpomDepth in mesh.hlsl
RES = 512
KINDS = {              # volume size in metres: width, height
    "balcony": (3.0, 1.35),
    "canopy": (1.6, 0.5),
    "rail": (3.0, 1.2),      # a porch railing: rails and balusters only, no slab
}


class Layers:
    """Paints boxes and slanted bands into a 2-layer relief. x, y in metres from the volume's lower-left; z in metres out from the wall."""
    def __init__(self, W, H):
        self.W, self.H = W, H
        self.start = np.zeros((RES, RES), np.float32); self.end = np.zeros((RES, RES), np.float32)
        self.col = np.zeros((RES, RES, 3), np.float32)
        self.xs = (np.arange(RES) + 0.5) / RES * W                   # columns
        self.ys = (1.0 - (np.arange(RES) + 0.5) / RES) * H           # rows: image row 0 is the top (y = H)

    def box(self, x0, x1, y0, y1, z0, z1, color):
        mx = (self.xs >= x0) & (self.xs <= x1); my = (self.ys >= y0) & (self.ys <= y1)
        m = my[:, None] & mx[None, :]
        self.start[m] = z0; self.end[m] = z1; self.col[m] = color

    def band(self, x0, x1, y0, y1, zlo, zhi, color):
        """z-interval per row from callables zlo(y), zhi(y) (a slanted plate), over columns x0..x1 and rows y0..y1."""
        mx = (self.xs >= x0) & (self.xs <= x1)
        for r, y in enumerate(self.ys):
            if y < y0 or y > y1: continue
            a, b = max(0.0, zlo(y)), min(DEPTH, zhi(y))
            if b > a + 0.004:
                self.start[r, mx] = a; self.end[r, mx] = b; self.col[r, mx] = color

    def encode(self, noise_seed=3):
        rng = np.random.default_rng(noise_seed)
        n = 0.90 + 0.10 * rng.random((RES, RES, 1)).astype(np.float32)
        n = np.asarray(Image.fromarray((n[..., 0] * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.0)), np.float32)[..., None] / 255.0
        rgb = np.clip(self.col * n * 255.0, 0, 255).astype(np.uint8)
        alb = np.dstack([rgb, np.clip(self.end / DEPTH * 255.0, 0, 255).astype(np.uint8)])
        # the normal map's own detail: a gentle slope from the end layer, so a flat slab is not mirror-flat; alpha = start
        e = self.end / DEPTH
        dx = (np.roll(e, -1, 1) - np.roll(e, 1, 1)) * 0.5; dy = (np.roll(e, -1, 0) - np.roll(e, 1, 0)) * 0.5
        nrm = np.stack([-dx * 2.0, dy * 2.0, np.ones_like(e)], -1); nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
        nrgb = ((nrm * 0.5 + 0.5) * 255).astype(np.uint8)
        nor = np.dstack([nrgb, np.clip(self.start / DEPTH * 255.0, 0, 255).astype(np.uint8)])
        return png(alb), png(nor)


def png(arr):
    b = io.BytesIO(); Image.fromarray(arr, "RGBA").save(b, "PNG", optimize=True); return b.getvalue()


def _tone(c, k):
    return tuple(min(255, max(0, v * k)) / 255.0 for v in c)


def relief(kind, palette):
    """Albedo and normal PNG bytes for a volume kind, coloured from a level's palette (trim, door colours)."""
    W, H = KINDS[kind]
    L = Layers(W, H)
    trim, door = palette["trim"], palette["door"]
    if kind == "balcony":
        P = 1.0
        slab = _tone(trim, 0.85); rail = _tone(trim, 1.0); dark = _tone(trim, 0.6)
        L.box(0.18, 0.28, 0.0, 0.2, 0.0, P * 0.85, dark); L.box(W - 0.28, W - 0.18, 0.0, 0.2, 0.0, P * 0.85, dark)      # corbels
        L.box(0.0, W, 0.2, 0.34, 0.0, P, slab)                                                                              # floor slab
        L.box(0.0, 0.05, 0.34, 1.12, 0.0, P, rail); L.box(W - 0.05, W, 0.34, 1.12, 0.0, P, rail)                            # side panels
        L.box(0.0, W, 1.04, 1.12, P - 0.11, P, rail)                                                                       # top rail
        L.box(0.0, W, 0.34, 0.40, P - 0.09, P, rail)                                                                       # bottom rail
        x = 0.12
        while x < W - 0.12:                                                                                                # balusters, free of the wall
            L.box(x - 0.018, x + 0.018, 0.40, 1.04, P - 0.07, P - 0.03, rail); x += 0.12
    elif kind == "rail":
        P = 1.0; rail = _tone(trim, 1.0)
        L.box(0.0, W, 0.97, 1.06, P - 0.11, P, rail); L.box(0.0, W, 0.10, 0.19, P - 0.11, P, rail)     # top and bottom rails
        x = 0.06
        while x < W - 0.04:
            L.box(x - 0.02, x + 0.02, 0.19, 0.97, P - 0.10, P - 0.02, rail); x += 0.11                   # balusters
    else:
        P = 0.9
        slab = _tone(trim, 0.8); strut = _tone(trim, 0.55); lip = _tone(trim, 1.0)
        top0, drop, thick = 0.40, 0.10, 0.06                       # the plate slopes down toward the front
        L.band(0.0, W, 0.0, H, lambda y: (top0 - thick - y) * P / drop, lambda y: (top0 - y) * P / drop, slab)
        L.box(0.0, W, 0.28, 0.36, P - 0.04, P, lip)                                                                        # front lip
        for xc in (0.18, W - 0.18):                                                                                        # brackets
            L.box(xc - 0.03, xc + 0.03, 0.08, 0.30, 0.0, 0.10, strut)
    return L.encode()


def spom_box(mesh, kind, origin, N, y0=None):
    """Emit a volume of `kind` whose lower-left corner is `origin` (on the wall, world metres), facing out along N (horizontal unit)."""
    W, H = KINDS[kind]
    N = np.array(N, np.float32); up = np.array([0, 1, 0], np.float32)
    T = np.cross(up, N).astype(np.float32)
    O = np.array(origin, np.float32)
    def corner(x, y, z): return O + T * x + up * y + N * z
    c = {(i, j, k): corner(i * W, j * H, k * DEPTH) for i in (0, 1) for j in (0, 1) for k in (0, 1)}
    centre = O + T * W / 2 + up * H / 2 + N * DEPTH / 2
    faces = [((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)),   # front (outermost)
             ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)),   # top
             ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)),   # bottom
             ((0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)),   # left
             ((1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1))]   # right
    for f in faces:
        pts = [c[k] for k in f]
        n = np.cross(pts[1] - pts[0], pts[3] - pts[0])
        if np.dot(n, np.mean(pts, 0) - centre) < 0:         # keep the winding outward whatever the facade's handedness
            f = (f[0], f[3], f[2], f[1]); pts = [c[k] for k in f]
        uvs = [(k[0] * W, k[1] * H) for k in f]
        cols = [(W, H, k[2] * DEPTH) for k in f]
        mesh.raw_quad("spom_" + kind, pts, N, uvs, cols)
