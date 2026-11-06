#!/usr/bin/env python3
"""Shared kit for the power-grid props (pylon, poles, substation): plain low-poly geometry plus silhouette-POM ("SPOM") volumes,
the same technique as assets_src/crane (SpomPS in src/shaders/mesh.hlsl, three.js port in assets/buildings/spom_preview.js).

A SPOM volume is a 12-triangle box standing out of a vertical wall plane. Its real shape is a two-layer relief texture:
    albedo.a = where solid ENDS   (metres out from the wall / 1.2)
    normal.a = where solid STARTS (metres out from the wall / 1.2)
so each texel holds one interval of solid along the wall normal. Two things follow that this kit leans on:
  * a lattice face whose plane LEANS (a tapering pylon) is fine: the bar plane is a function of height, painted into the relief,
    while the box itself stays vertical (SpomPS fixes the volume's up axis to world Y);
  * a body of revolution about a vertical axis (insulator, bushing, pole) is EXACT from every horizontal view: at each height the
    interval [zc - sqrt(R^2 - dx^2), zc + sqrt(R^2 - dx^2)] is the disc of radius R. One box = a round insulator string with sheds.
Needs numpy + Pillow.
"""
import json
import math
import os
import struct

import numpy as np
from PIL import Image, ImageFilter

DEPTH = 1.2  # kSpomDepth in mesh.hlsl: the relief's alpha range in metres
HERE = os.path.dirname(os.path.abspath(__file__))
TEX = os.path.join(HERE, "tex")
os.makedirs(TEX, exist_ok=True)


# ------------------------------------------------------------------------------------------------------------------------------
# Plain textures (procedural, tiling)
# ------------------------------------------------------------------------------------------------------------------------------
def _noise(size, scales, seed):
    rng = np.random.default_rng(seed)
    out = np.zeros((size, size), np.float32)
    for s, w in scales:
        n = rng.random((s, s)).astype(np.float32)
        img = Image.fromarray((n * 255).astype(np.uint8)).resize((size, size), Image.BICUBIC)
        out += np.asarray(img, np.float32) / 255.0 * w
    return out / sum(w for _, w in scales)


def _tile_fix(a):  # blend the edges so the noise tiles
    n = a.shape[0]
    r = np.roll(np.roll(a, n // 2, 0), n // 2, 1)
    y, x = np.mgrid[0:n, 0:n] / (n - 1)
    w = (np.minimum(np.minimum(x, 1 - x), np.minimum(y, 1 - y)) * 2).clip(0, 1)[..., None] if a.ndim == 3 else \
        (np.minimum(np.minimum(x, 1 - x), np.minimum(y, 1 - y)) * 2).clip(0, 1)
    return a * w + r * (1 - w)


PLAIN = {}  # material -> (relative file, metres per repeat, metallic, roughness)


def plain_texture(name, base, var=0.08, scales=((8, 1), (32, 0.6), (128, 0.4)), m_per=2.0, metal=0.0, rough=0.8, seed=1,
                  extra=None):
    size = 256
    n = _noise(size, scales, seed)
    col = np.array(base, np.float32)[None, None, :] * (1.0 + (n[..., None] - 0.5) * 2.0 * var)
    if extra is not None:
        col = extra(col, size)
    col = _tile_fix(col)
    rel = f"tex/{name}.jpg"
    Image.fromarray(np.clip(col * 255, 0, 255).astype(np.uint8)).save(os.path.join(HERE, rel), quality=88)
    PLAIN[name] = (rel, m_per, metal, rough)


def make_plain_textures():
    plain_texture("galv_steel", (0.62, 0.64, 0.64), 0.10, m_per=2.0, metal=0.6, rough=0.55, seed=2)
    plain_texture("concrete", (0.66, 0.64, 0.60), 0.10, m_per=2.0, seed=3)
    plain_texture("dark_metal", (0.26, 0.28, 0.29), 0.06, m_per=2.0, metal=0.4, rough=0.6, seed=4)
    plain_texture("tank_grey", (0.50, 0.53, 0.52), 0.05, m_per=3.0, metal=0.3, rough=0.55, seed=5)
    plain_texture("aluminium", (0.78, 0.79, 0.80), 0.04, m_per=1.0, metal=0.8, rough=0.35, seed=6)
    plain_texture("wire", (0.30, 0.31, 0.32), 0.05, m_per=0.5, metal=0.7, rough=0.5, seed=7)

    def gravel(col, size):
        rng = np.random.default_rng(11)
        g = np.asarray(Image.fromarray((rng.random((size, size)) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8)),
                       np.float32) / 255.0
        return col * (0.75 + 0.5 * g[..., None])
    plain_texture("gravel", (0.55, 0.54, 0.52), 0.15, m_per=1.5, rough=0.95, seed=8, extra=gravel)

    def block(col, size):  # painted block-work: mortar lines every 0.2 m (at 4 m per repeat, 20 courses)
        y, x = np.mgrid[0:size, 0:size]
        course = size // 20
        row = y // course
        joint = (y % course < 1) | (((x + (row % 2) * (course * 2)) % (course * 4)) < 1)
        return np.where(joint[..., None], col * 0.82, col)
    plain_texture("wall_block", (0.80, 0.77, 0.70), 0.05, m_per=4.0, seed=9, extra=block)
    plain_texture("roof_felt", (0.22, 0.22, 0.23), 0.08, m_per=3.0, rough=0.9, seed=10)
    plain_texture("door_steel", (0.30, 0.38, 0.42), 0.04, m_per=2.0, metal=0.3, rough=0.6, seed=12)


# ------------------------------------------------------------------------------------------------------------------------------
# Relief painter
# ------------------------------------------------------------------------------------------------------------------------------
class Relief:
    """A W x H metre relief canvas (origin lower-left, y up), painted with solids that each cover one z interval per texel.
    plane=(z_bottom, z_top): the default centre plane of members, linear in y, so a leaning lattice face is one call per bar."""

    def __init__(self, W, H, ppm, plane=(0.2, 0.2)):
        self.W, self.H, self.ppm = W, H, ppm
        self.pw, self.ph = max(4, int(round(W * ppm))), max(4, int(round(H * ppm)))
        xs = (np.arange(self.pw) + 0.5) / self.pw * W
        ys = H - (np.arange(self.ph) + 0.5) / self.ph * H  # row 0 = top
        self.X, self.Y = np.meshgrid(xs, ys)
        self.start = np.full((self.ph, self.pw), 9.0, np.float32)
        self.end = np.zeros((self.ph, self.pw), np.float32)
        self.rgb = np.zeros((self.ph, self.pw, 3), np.float32)
        self.plane = plane
        self.rng = np.random.default_rng(int(W * 1000 + H * 7))

    def zp(self, Y=None):
        Y = self.Y if Y is None else Y
        zb, zt = self.plane
        return zb + (zt - zb) * (Y / self.H)

    def _add(self, on, s, e, col, shade):
        col = np.asarray(col, np.float32)
        grain = 1.0 + (self.rng.random(on.shape).astype(np.float32) - 0.5) * 0.06
        c = col[None, None, :] * (0.55 + 0.45 * shade)[..., None] * grain[..., None]
        front = on & (e > self.end)
        self.rgb = np.where(front[..., None], c, self.rgb)
        self.start = np.where(on, np.minimum(self.start, s), self.start)
        self.end = np.where(on, np.maximum(self.end, e), self.end)

    def bar(self, a, b, r, col, z=None, square=True):
        """A member from a to b (metres), radius r. square: a squarish (galvanised angle / timber) section, else round."""
        ax, ay = a; bx, by = b
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy + 1e-12
        t = np.clip(((self.X - ax) * dx + (self.Y - ay) * dy) / L2, 0.0, 1.0)
        px, py = ax + t * dx, ay + t * dy
        d = np.hypot(self.X - px, self.Y - py) / r
        on = d < 1.0
        if not on.any():
            return
        prof = np.clip(1.0 - d ** (6 if square else 2), 0, 1) ** (1 / 6 if square else 0.5)
        zc = self.zp(py) if z is None else (z if np.isscalar(z) else z[0] + (z[1] - z[0]) * t)
        self._add(on, zc - r * prof, zc + r * prof, col, prof if not square else 0.75 + 0.25 * prof)

    def rect(self, x0, y0, x1, y1, z0, z1, col, bevel=0.0):
        on = (self.X >= x0) & (self.X <= x1) & (self.Y >= y0) & (self.Y <= y1)
        if not on.any():
            return
        e = np.full(self.X.shape, z1, np.float32)
        if bevel > 0:
            k = np.minimum.reduce([self.X - x0, x1 - self.X, self.Y - y0, y1 - self.Y]) / bevel
            e = z0 + (z1 - z0) * np.clip(k, 0.3, 1.0)
        self._add(on, np.full(self.X.shape, z0, np.float32), e, col, np.full(self.X.shape, 1.0, np.float32))

    def rev(self, cx, zc, profile, col_fn):
        """Body of revolution about the vertical axis x = cx, z = zc. profile(y) -> radius array (0 = none);
        col_fn(X, Y) -> rgb array (ph, pw, 3) or a flat colour."""
        R = profile(self.Y)
        dx = np.abs(self.X - cx)
        on = (R > 0) & (dx < R)
        if not on.any():
            return
        h = np.sqrt(np.clip(R * R - dx * dx, 0, None))
        shade = np.where(R > 0, h / np.maximum(R, 1e-6), 0)
        col = col_fn(self.X, self.Y) if callable(col_fn) else col_fn
        col = np.asarray(col, np.float32)
        if col.ndim == 1:
            col = np.broadcast_to(col, self.X.shape + (3,))
        grain = 1.0 + (self.rng.random(on.shape).astype(np.float32) - 0.5) * 0.05
        c = col * (0.5 + 0.5 * shade)[..., None] * grain[..., None]
        e = zc + h
        front = on & (e > self.end)
        self.rgb = np.where(front[..., None], c, self.rgb)
        self.start = np.where(on, np.minimum(self.start, zc - h), self.start)
        self.end = np.where(on, np.maximum(self.end, zc + h), self.end)

    def save(self, name, mirror=False):
        """Writes tex/spom_<name>_albedo.png + _normal.png. Returns the max relief depth (metres), for sizing the box."""
        start, end, rgb = self.start.copy(), self.end.copy(), self.rgb.copy()
        if mirror:
            start, end, rgb = start[:, ::-1], end[:, ::-1], rgb[:, ::-1]
        solid = end > 0.0
        start[~solid] = 0.0
        assert end.max() <= DEPTH, f"{name}: relief {end.max():.2f} m deeper than kSpomDepth"
        assert start[solid].min() >= 0.0 if solid.any() else True, f"{name}: relief behind the wall"
        rng = np.random.default_rng(7)

        def q8(v):  # dither so round profiles do not terrace (8-bit = 4.7 mm steps at 1.2 m)
            o = np.clip(np.floor(v / DEPTH * 255.0 + rng.random(v.shape)), 0, 255).astype(np.uint8)
            return np.where(solid, o, 0).astype(np.uint8)
        a_end, a_start = q8(end), q8(start)
        a_end = np.where(solid & (a_end <= a_start), np.minimum(a_start + 1, 255), a_end).astype(np.uint8)
        alb = np.dstack([np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8), a_end])
        e = end / DEPTH
        gx = (np.roll(e, -1, 1) - np.roll(e, 1, 1)) * 0.5 * self.ppm
        gy = (np.roll(e, -1, 0) - np.roll(e, 1, 0)) * 0.5 * self.ppm
        nrm = np.stack([-gx * 0.02, gy * 0.02, np.ones_like(e)], -1)
        nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
        nor = np.dstack([((nrm * 0.5 + 0.5) * 255).astype(np.uint8), a_start])
        Image.fromarray(alb, "RGBA").save(os.path.join(TEX, f"spom_{name}_albedo.png"), optimize=True)
        Image.fromarray(nor, "RGBA").save(os.path.join(TEX, f"spom_{name}_normal.png"), optimize=True)
        return float(end.max())


# Colours (sRGB 0..1, as the shader reads albedo raw and linearises)
GALV = (0.66, 0.68, 0.68)
GALV_DARK = (0.50, 0.52, 0.52)
GLASS_INS = (0.42, 0.62, 0.58)      # toughened-glass cap-and-pin discs
PORCELAIN = (0.47, 0.30, 0.20)      # brown glazed porcelain
PORCELAIN_GREY = (0.70, 0.71, 0.70)
WOOD = (0.36, 0.27, 0.19)           # creosoted pole
WOOD_ARM = (0.44, 0.35, 0.25)
STEEL_HW = (0.40, 0.41, 0.42)


def shed_profile(y0, y1, core, shed, pitch, cap=0.0, alt=None):
    """Insulator radius vs height: a core with sheds every `pitch` (alternating big/small if alt), each shed a sloped disc."""
    def f(Y):
        R = np.zeros_like(Y)
        inside = (Y >= y0) & (Y <= y1)
        R = np.where(inside, core, R)
        k = (Y - y0) / pitch
        ph = k - np.floor(k)                     # 0..1 within a shed period
        idx = np.floor(k)
        big = shed if alt is None else np.where(idx % 2 == 0, shed, alt)
        # a shed: thin rim at the bottom of the period, sloping up toward the core (rain shed shape)
        sh = np.where(ph < 0.45, core + (big - core) * np.clip(1.0 - ph / 0.45, 0, 1) ** 0.6, core)
        R = np.where(inside, np.maximum(R, sh), R)
        if cap > 0:
            R = np.where(((Y >= y0 - 0.06) & (Y < y0)) | ((Y > y1) & (Y <= y1 + 0.06)), cap, R)
        return R
    return f


# ------------------------------------------------------------------------------------------------------------------------------
# Mesh
# ------------------------------------------------------------------------------------------------------------------------------
class Mesh:
    def __init__(self, name):
        self.name = name
        self.tris = {}    # material -> list of (p, n) triples
        self.spom = {}    # texture key -> list of (p, N, uv, (W, H, z))
        self.extras = {}

    # plain geometry
    def tri(self, mat, a, b, c, n=None):
        if n is None:
            e1, e2 = np.subtract(b, a), np.subtract(c, a)
            n = np.cross(e1, e2)
            ln = np.linalg.norm(n)
            n = n / ln if ln > 1e-9 else (0, 1, 0)
        self.tris.setdefault(mat, []).append(((tuple(a), tuple(b), tuple(c)), tuple(map(float, n))))

    def quad(self, mat, a, b, c, d, n=None):
        self.tri(mat, a, b, c, n)
        self.tri(mat, a, c, d, n)

    def box(self, mat, c, s, bottom=True):
        cx, cy, cz = c; hx, hy, hz = s[0] / 2, s[1] / 2, s[2] / 2
        P = lambda i, j, k: (cx + (2 * i - 1) * hx, cy + (2 * j - 1) * hy, cz + (2 * k - 1) * hz)
        self.quad(mat, P(0, 0, 1), P(1, 0, 1), P(1, 1, 1), P(0, 1, 1), (0, 0, 1))
        self.quad(mat, P(1, 0, 0), P(0, 0, 0), P(0, 1, 0), P(1, 1, 0), (0, 0, -1))
        self.quad(mat, P(1, 0, 1), P(1, 0, 0), P(1, 1, 0), P(1, 1, 1), (1, 0, 0))
        self.quad(mat, P(0, 0, 0), P(0, 0, 1), P(0, 1, 1), P(0, 1, 0), (-1, 0, 0))
        self.quad(mat, P(0, 1, 1), P(1, 1, 1), P(1, 1, 0), P(0, 1, 0), (0, 1, 0))
        if bottom:
            self.quad(mat, P(0, 0, 0), P(1, 0, 0), P(1, 0, 1), P(0, 0, 1), (0, -1, 0))

    def cyl(self, mat, a, b, r, seg=6, caps=False):
        a, b = np.array(a, float), np.array(b, float)
        ax = b - a
        ax /= np.linalg.norm(ax)
        ref = np.array([0, 1, 0]) if abs(ax[1]) < 0.9 else np.array([1, 0, 0])
        u = np.cross(ax, ref); u /= np.linalg.norm(u)
        v = np.cross(ax, u)
        ring = [(math.cos(2 * math.pi * i / seg), math.sin(2 * math.pi * i / seg)) for i in range(seg)]
        for i in range(seg):
            c0, s0 = ring[i]; c1, s1 = ring[(i + 1) % seg]
            n0, n1 = u * c0 + v * s0, u * c1 + v * s1
            self.tri(mat, a + n0 * r, a + n1 * r, b + n1 * r, (n0 + n1) / np.linalg.norm(n0 + n1))
            self.tri(mat, a + n0 * r, b + n1 * r, b + n0 * r, (n0 + n1) / np.linalg.norm(n0 + n1))
        if caps:
            for p, s in ((a, -1), (b, 1)):
                for i in range(1, seg - 1):
                    q = [p + (u * ring[k][0] + v * ring[k][1]) * r for k in (0, i, i + 1)]
                    if s < 0:
                        q = [q[0], q[2], q[1]]
                    self.tri(mat, *q, ax * s)

    # SPOM volume
    def spom_box(self, key, wall_center, N, W, H, D):
        """A volume whose wall rectangle (W wide along T = cross(up, N), H tall) is centred at wall_center, depth D along N."""
        N = np.array(N, float)
        up = np.array([0.0, 1.0, 0.0])
        T = np.cross(up, N)
        c = np.array(wall_center, float)
        def P(i, j, k):
            return c + T * (i - 0.5) * W + up * (j - 0.5) * H + N * (k * D)
        faces = []
        for axis in range(3):
            for side in (0, 1):
                q = []
                for du, dv in ((0, 0), (1, 0), (1, 1), (0, 1)):
                    idx = [0, 0, 0]
                    idx[axis], idx[(axis + 1) % 3], idx[(axis + 2) % 3] = side, du, dv
                    q.append(tuple(idx))
                out = [0, 0, 0]; out[axis] = 1 if side else -1
                faces.append((q, T * out[0] + up * out[1] + N * out[2]))
        lst = self.spom.setdefault(key, [])
        for q, outward in faces:
            for tri in ((0, 1, 2), (0, 2, 3)):
                ids = [q[t] for t in tri]
                pts = [P(*i) for i in ids]
                if np.dot(np.cross(pts[1] - pts[0], pts[2] - pts[0]), outward) < 0:
                    ids = [ids[0], ids[2], ids[1]]
                for i in ids:
                    lst.append((tuple(P(*i)), tuple(N), (i[0] * W, i[1] * H), (W, H, i[2] * D)))

    def spom_quad(self, key, origin, N, W, H, zq, corners):
        """A single-quad SPOM volume ("SPOM quad", 2 triangles): it lies ON the relief's centre plane zq instead of boxing it,
        and the shader steps each ray back to the volume's entry, so it shows the relief from both sides. N may lean: the frame
        is T = cross(up, N), B = cross(N, T). origin = the volume's lower-left on its wall (z = 0); corners = 4 (u, v) metres,
        counter-clockwise seen from +N. key must start with "q_" (the preview draws those double-sided)."""
        N = np.array(N, float); N /= np.linalg.norm(N)
        T = np.cross([0.0, 1.0, 0.0], N); T /= np.linalg.norm(T)
        B = np.cross(N, T)
        O = np.array(origin, float)
        lst = self.spom.setdefault(key, [])
        for tri in ((0, 1, 2), (0, 2, 3)):
            for k in tri:
                u, v = corners[k]
                lst.append((tuple(O + T * u + B * v + N * zq), tuple(N), (u, v), (W, H, zq + 10.0)))

    def spom_quad_at(self, key, wall_center, N, W, H, zq, corners=None):
        """spom_quad placed like spom_box (wall rectangle centred at wall_center, upright N). corners default: the whole W x H."""
        N = np.array(N, float)
        T = np.cross([0.0, 1.0, 0.0], N); T /= np.linalg.norm(T)
        origin = np.array(wall_center, float) - T * W / 2 - np.array([0.0, H / 2, 0.0])
        self.spom_quad(key, origin, N, W, H, zq, corners or [(0, 0), (W, 0), (W, H), (0, H)])

    def triangles(self):
        return sum(len(v) for v in self.tris.values()) + sum(len(v) // 3 for v in self.spom.values())

    # export
    def write_glb(self, path):
        bin_data = bytearray()
        views, accs, images, textures, materials, prims = [], [], [], [], [], []

        def view(data):
            while len(bin_data) % 4:
                bin_data.append(0)
            views.append({"buffer": 0, "byteOffset": len(bin_data), "byteLength": len(data)})
            bin_data.extend(data)
            return len(views) - 1
        tcache = {}

        def tex(rel):
            if rel not in tcache:
                with open(os.path.join(HERE, rel), "rb") as f:
                    images.append({"bufferView": view(f.read()), "mimeType": "image/png" if rel.endswith(".png") else "image/jpeg"})
                textures.append({"source": len(images) - 1, "sampler": 0})
                tcache[rel] = len(textures) - 1
            return tcache[rel]

        def attr(vals, comps, with_minmax=False):
            flat = [x for v in vals for x in v]
            a = {"bufferView": view(struct.pack(f"<{len(flat)}f", *flat)), "componentType": 5126, "count": len(vals),
                 "type": {2: "VEC2", 3: "VEC3"}[comps]}
            if with_minmax:
                a["min"] = [min(v[i] for v in vals) for i in range(comps)]
                a["max"] = [max(v[i] for v in vals) for i in range(comps)]
            accs.append(a)
            return len(accs) - 1

        for key, verts in self.spom.items():  # material named spom_* so mesh_import.h tags it (vertex alpha 0.8)
            p = attr([v[0] for v in verts], 3, True)
            n = attr([v[1] for v in verts], 3)
            t = attr([v[2] for v in verts], 2)
            c = attr([v[3] for v in verts], 3)
            materials.append({"name": f"spom_{key}", "pbrMetallicRoughness": {
                "baseColorTexture": {"index": tex(f"tex/spom_{key}_albedo.png")}, "baseColorFactor": [1, 1, 1, 1],
                "metallicFactor": 0.0, "roughnessFactor": 0.8}, "normalTexture": {"index": tex(f"tex/spom_{key}_normal.png")}})
            prims.append({"attributes": {"POSITION": p, "NORMAL": n, "TEXCOORD_0": t, "COLOR_0": c}, "material": len(materials) - 1})

        for mat, tris in self.tris.items():
            rel, mper, metal, rough = PLAIN[mat]
            P, Nn, UV = [], [], []
            for pts, n in tris:
                ax_ = [abs(x) for x in n]
                for q in pts:
                    if ax_[1] >= ax_[0] and ax_[1] >= ax_[2]:
                        u, v = q[0], q[2]
                    elif ax_[0] >= ax_[2]:
                        u, v = q[2], q[1]
                    else:
                        u, v = q[0], q[1]
                    P.append(q); Nn.append(n); UV.append((u / mper, v / mper))
            materials.append({"name": mat, "pbrMetallicRoughness": {"baseColorTexture": {"index": tex(rel)},
                                                                    "metallicFactor": metal, "roughnessFactor": rough}})
            prims.append({"attributes": {"POSITION": attr(P, 3, True), "NORMAL": attr(Nn, 3), "TEXCOORD_0": attr(UV, 2)},
                          "material": len(materials) - 1})
        while len(bin_data) % 4:
            bin_data.append(0)
        node = {"name": self.name, "mesh": 0}
        if self.extras:
            node["extras"] = self.extras
        gltf = {"asset": {"version": "2.0", "generator": "assets_src/power/powerlib.py"}, "scene": 0,
                "scenes": [{"nodes": [0]}], "nodes": [node], "meshes": [{"name": self.name, "primitives": prims}],
                "materials": materials, "textures": textures, "images": images,
                "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
                "accessors": accs, "bufferViews": views, "buffers": [{"byteLength": len(bin_data)}]}
        js = json.dumps(gltf, separators=(",", ":")).encode()
        js += b" " * (-len(js) % 4)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_data)))
            f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
            f.write(struct.pack("<II", len(bin_data), 0x004E4942) + bytes(bin_data))
        return os.path.getsize(path)
