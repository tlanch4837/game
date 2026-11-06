"""Low-poly, high-detail city cars: 4 body types x 6 paint colours -> GLB files.

Design (same idea as tools/building_gen): the mesh is a few hundred flat-shaded triangles -- an extruded side profile
with real wheel arches and bevelled edges, a tapered greenhouse, mirrors and 14-sided wheels. All the small detail
(door seams, handles, grille, headlights, tail lights, rims, brake discs, tyre sidewalls, interior behind the glass)
lives in Gemini-generated textures plus normal maps derived from them.

Run:  python gen_cars.py            (numpy + Pillow)
Source textures (Gemini, via Chrome): assets/vehicles/cars/src_tex/car_{wheel,front,rear,side,glass}.png
Output: assets/vehicles/cars/glb/CAR_<type>_<colour>.glb    processed maps: assets/vehicles/cars/tex/
Units: metres, +Y up, car front faces +Z, origin on the ground under the car centre.
"""
import io, json, math, os, struct, sys
import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
BASE = os.path.join(ROOT, "assets", "vehicles", "cars")
SRC, OUT, TEX = (os.path.join(BASE, d) for d in ("src_tex", "glb", "tex"))

COLOURS = {"white": (0.92, 0.92, 0.92), "black": (0.06, 0.06, 0.07), "red": (0.62, 0.05, 0.05),
           "blue": (0.08, 0.20, 0.50), "silver": (0.55, 0.57, 0.60), "green": (0.10, 0.30, 0.20),
           "yellow": (0.85, 0.65, 0.05), "orange": (0.80, 0.30, 0.04), "grey": (0.22, 0.23, 0.25),
           "beige": (0.68, 0.60, 0.46), "teal": (0.05, 0.38, 0.40), "maroon": (0.30, 0.04, 0.07)}
WHEEL_STYLES = {"alloy": 1.0, "dark": 0.38, "gold": None}  # multiplier on the wheel texture (gold = tinted)

# L length, W width, wb wheelbase, r wheel radius, yb sill height, belt beltline, hood/deck heights,
# cab = (rear base z, rear roof z, front roof z, front base z) as fractions of L/2, roof height
TYPES = {
    "sedan":  dict(L=4.6, W=1.80, wb=2.70, r=0.32, yb=0.20, belt=0.92, hood=0.86, deck=0.90, roof=1.44, cab=(-0.62, -0.36, 0.12, 0.38)),
    "hatch":  dict(L=4.0, W=1.74, wb=2.50, r=0.30, yb=0.19, belt=0.90, hood=0.84, deck=0.95, roof=1.46, cab=(-0.93, -0.84, 0.10, 0.36)),
    "suv":    dict(L=4.7, W=1.90, wb=2.80, r=0.37, yb=0.32, belt=1.08, hood=1.04, deck=1.10, roof=1.74, cab=(-0.95, -0.90, 0.16, 0.40)),
    "wagon":  dict(L=4.8, W=1.82, wb=2.80, r=0.32, yb=0.20, belt=0.93, hood=0.87, deck=0.95, roof=1.47, cab=(-0.94, -0.88, 0.12, 0.38), rails=1),
    "coupe":  dict(L=4.4, W=1.84, wb=2.62, r=0.33, yb=0.17, belt=0.86, hood=0.78, deck=0.88, roof=1.30, cab=(-0.66, -0.30, 0.02, 0.34), spoiler=1, two_door=1),
    "pickup": dict(L=5.3, W=1.95, wb=3.30, r=0.38, yb=0.36, belt=1.12, hood=1.10, deck=1.12, roof=1.86, cab=(-0.16, -0.12, 0.26, 0.44), flares=1, bed=1),
    "van":    dict(L=5.0, W=1.95, wb=3.10, r=0.34, yb=0.28, belt=1.05, hood=1.00, deck=1.05, roof=2.05, cab=(-0.97, -0.96, 0.52, 0.66), rails=1),
    "taxi":   dict(L=4.6, W=1.80, wb=2.70, r=0.32, yb=0.20, belt=0.92, hood=0.86, deck=0.90, roof=1.44, cab=(-0.62, -0.36, 0.12, 0.38), sign=1, colours=("yellow",)),
    "compact":dict(L=3.5, W=1.66, wb=2.30, r=0.28, yb=0.18, belt=0.88, hood=0.82, deck=0.94, roof=1.50, cab=(-0.94, -0.86, 0.14, 0.42), two_door=1),
}
TYPES["sports"] = dict(L=4.5, W=1.96, wb=2.64, r=0.34, yb=0.13, belt=0.76, hood=0.66, deck=0.80, roof=1.16,
                       cab=(-0.56, -0.20, -0.04, 0.26), spoiler=1, two_door=1)
TYPES["suv"].update(flares=1, rails=1)
DOOR_PITCH = 1.2  # metres per tile of the side texture (one door, seam at u=0.52); the repeats land in arches / off the ends


# ---------------------------------------------------------------- textures
def height_from_albedo(rgb, blur_lo=10, blur_hi=1.5):
    g = rgb.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32) / 255.0
    im = Image.fromarray((g * 255).astype(np.uint8))
    lo = np.asarray(im.filter(ImageFilter.GaussianBlur(blur_lo)), np.float32) / 255.0
    hi = np.asarray(im.filter(ImageFilter.GaussianBlur(blur_hi)), np.float32) / 255.0
    return hi - lo


def normal_from_height(h, k):
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5
    n = np.stack([-dx * k, dy * k, np.ones_like(h)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return ((n * 0.5 + 0.5) * 255).astype(np.uint8)


def enc(arr, fmt):
    b = io.BytesIO(); Image.fromarray(arr).save(b, fmt, **({"quality": 90} if fmt == "JPEG" else {"optimize": True})); return b.getvalue()


HEIGHTS = {}  # material -> float height map in 0..1 (1 = top); written to tex/<mat>_height.png for POM


def groove(h, rows=None, cols=None, width=3, depth=0.45):
    """Cut soft-edged grooves (panel gaps, creases) into a height map at fractional rows/cols."""
    H, W = h.shape
    for f in rows or []:
        y = np.arange(H)[:, None]; h -= depth * np.exp(-((y - f * H) / width) ** 2)
    for f in cols or []:
        x = np.arange(W)[None, :]; h -= depth * np.exp(-((x - f * W) / width) ** 2)
    return h


def finish_height(name, h, k):
    h = (h - h.min()) / max(1e-6, h.max() - h.min())
    HEIGHTS[name] = h
    return enc(normal_from_height(h, k), "PNG")


def load_maps():
    def prep(name, size, crop=None, k=4.0, normal=True, draw=None):
        im = Image.open(os.path.join(SRC, f"car_{name}.png")).convert("RGB")
        if crop: im = im.crop(tuple(int(c * s) for c, s in zip(crop, (im.width, im.height, im.width, im.height))))
        rgb = np.asarray(im.resize(size, Image.LANCZOS))
        alb = enc(rgb, "JPEG")
        if not normal: return alb, None
        lum = rgb.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32) / 255.0
        # relief = fine high-pass + broad luminance (dark grille holes, lamp recesses and spoke gaps sit low)
        h = height_from_albedo(rgb) * 2.0 + 0.35 * (np.asarray(Image.fromarray((lum * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(3)), np.float32) / 255.0)
        if draw: h = draw(h)
        return alb, finish_height(name, h, k * 4)

    def wheel_draw(h):  # tyre tread band: raised blocks with lateral sipes around the outer ring
        n = h.shape[0]; y, x = np.mgrid[0:n, 0:n] / n - 0.5; rr = np.hypot(x, y) * 2; ang = np.arctan2(y, x)
        band = (rr > 0.93) & (rr < 1.0)
        h[band] += 0.15 * (np.cos(ang[band] * 90) > 0.2)
        h[(rr > 0.70) & (rr < 0.74)] -= 0.25  # rim lip groove
        return h

    def front_draw(h):  # deepen the grille and lower intake, crisp outline of the fascia seams
        return groove(h, rows=[0.08, 0.95], width=2.5, depth=0.35)
    side = Image.open(os.path.join(SRC, "car_side.png")).convert("RGB")
    # neutralise the side panel to near-white so baseColorFactor carries the paint colour; keep its shading/seam
    a = np.asarray(side.resize((1024, 1024), Image.LANCZOS)).astype(np.float32)
    lum = a @ np.array([0.299, 0.587, 0.114], np.float32)
    lum = np.clip(lum / np.percentile(lum, 90), 0, 1.05) * 255
    side_rgb = np.clip(np.repeat(lum[..., None], 3, -1), 0, 255).astype(np.uint8)
    # body relief: the photo's door seam + drawn shoulder crease, lower character line and rocker step.
    # side_uv maps v 0.06 = beltline .. 0.94 = sill; the clean patch at (0.06..0.12, 0.80..0.86) stays untouched.
    hb = height_from_albedo(side_rgb) * 1.5
    hb = groove(hb, rows=[0.17], width=2.5, depth=0.30)          # shoulder crease under the window line
    hb = groove(hb, rows=[0.56], width=5.0, depth=0.18)          # lower character line
    hb[int(0.92 * 1024):, :] -= 0.25                             # rocker panel steps in
    hb = groove(hb, cols=[0.52], width=2.0, depth=0.5)           # sharpen the door gap
    hb[int(0.76 * 1024):int(0.90 * 1024), int(0.02 * 1024):int(0.20 * 1024)] = 0.0  # PLAIN patch: dead flat (it is stretched over hoods/roofs)
    maps = {
        "body": (enc(side_rgb, "JPEG"), finish_height("body", hb, 22.0)),
        "front": prep("front", (1024, 384), crop=(0, 0.30, 1, 1), k=4.0, draw=front_draw),  # drop the hood band
        "rear": prep("rear", (1024, 384), crop=(0, 0.22, 1, 1), k=4.0, draw=front_draw),
        "wheel": prep("wheel", (1024, 1024), k=5.0, draw=wheel_draw),
        "glass": prep("glass", (512, 512), normal=False),
    }
    maps["metal"] = (maps["wheel"][0], None)
    os.makedirs(TEX, exist_ok=True)
    for n, (al, no) in maps.items():
        open(os.path.join(TEX, f"{n}_albedo.jpg"), "wb").write(al)
        if no: open(os.path.join(TEX, f"{n}_normal.png"), "wb").write(no)
    for n, h in HEIGHTS.items():
        Image.fromarray((h * 255).astype(np.uint8)).save(os.path.join(TEX, f"{n}_height.png"))
    return maps


# ---------------------------------------------------------------- mesh
MATS = ["body", "glass", "front", "rear", "wheel", "metal", "trim"]  # trim = second livery colour (transit)


class Mesh:
    def __init__(self):
        self.tris = {m: [] for m in MATS}  # (p0,p1,p2, uv0,uv1,uv2)

    def tri(self, mat, p, uv, want=None):
        p = [np.asarray(q, np.float64) for q in p]
        n = np.cross(p[1] - p[0], p[2] - p[0])
        if np.linalg.norm(n) < 1e-9: return
        if want is not None and np.dot(n, want) < 0:
            p = [p[0], p[2], p[1]]; uv = [uv[0], uv[2], uv[1]]
        self.tris.setdefault(mat, []).append((p, uv))

    def quad(self, mat, p, uv, want=None):
        self.tri(mat, [p[0], p[1], p[2]], [uv[0], uv[1], uv[2]], want)
        self.tri(mat, [p[0], p[2], p[3]], [uv[0], uv[2], uv[3]], want)

    def poly(self, mat, pts2d, to3d, uvf, want):
        for a, b, c in earclip(pts2d):
            P = [to3d(pts2d[i]) for i in (a, b, c)]
            self.tri(mat, P, [uvf(pts2d[i]) for i in (a, b, c)], want)

    def grid(self, mat, P, uv, want, nu, nv, disp):
        """Subdivided bilinear patch over corners P[0..3] (uv likewise); disp(u, v) -> 3D offset (crowns, bulges)."""
        P = [np.asarray(p, float) for p in P]; uv = [np.asarray(q, float) for q in uv]
        bl = lambda C, u, v: (C[0] * (1 - u) + C[1] * u) * (1 - v) + (C[3] * (1 - u) + C[2] * u) * v
        pt = lambda u, v: bl(P, u, v) + disp(u, v)
        for i in range(nu):
            for j in range(nv):
                us, vs = (i / nu, (i + 1) / nu), (j / nv, (j + 1) / nv)
                cs = [(us[0], vs[0]), (us[1], vs[0]), (us[1], vs[1]), (us[0], vs[1])]
                self.quad(mat, [pt(*c) for c in cs], [tuple(bl(uv, *c)) for c in cs], want)

    def count(self):
        return sum(len(v) for v in self.tris.values())


def area(poly):
    return 0.5 * sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1] for i in range(len(poly)))


def earclip(poly):
    """Triangulate a simple CCW polygon; returns index triples."""
    idx = list(range(len(poly))); out = []
    def cross(o, a, b): return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    guard = 0
    while len(idx) > 3 and guard < 10000:
        guard += 1
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = poly[i0], poly[i1], poly[i2]
            if cross(a, b, c) <= 1e-10: continue
            if any(cross(a, b, poly[j]) >= 0 and cross(b, c, poly[j]) >= 0 and cross(c, a, poly[j]) >= 0
                   for j in idx if j not in (i0, i1, i2)): continue
            out.append((i0, i1, i2)); idx.pop(k); break
        else:
            break
    if len(idx) == 3: out.append(tuple(idx))
    return out


def offset_poly(poly, d):
    """Inset a CCW polygon by d (vertex normals = averaged edge normals, mitred)."""
    n = len(poly); res = []
    for i in range(n):
        p0, p1, p2 = np.array(poly[i - 1]), np.array(poly[i]), np.array(poly[(i + 1) % n])
        e0 = (p1 - p0) / np.linalg.norm(p1 - p0); e1 = (p2 - p1) / np.linalg.norm(p2 - p1)
        n0 = np.array([-e0[1], e0[0]]); n1 = np.array([-e1[1], e1[0]])  # inward for CCW
        m = n0 + n1; m /= np.linalg.norm(m)
        res.append(tuple(p1 + m * d / max(0.35, np.dot(m, n0))))
    return res


def lower_profile(t):
    """Side silhouette of the lower body in (z, y), CCW, with wheel arches."""
    L, wb, r, yb = t["L"], t["wb"], t["r"], t["yb"]
    zf, zr = L / 2, -L / 2
    ra = r + 0.06; ay = r
    th0 = math.asin(max(-1, min(1, (yb - ay) / ra)))
    def arch(zc):  # traversed from rear side to front side over the top when going CCW (bottom runs +z)
        return [(zc + ra * math.cos(a), ay + ra * math.sin(a)) for a in np.linspace(math.pi - th0, th0, 9)]
    hood, deck, belt = t["hood"], t["deck"], t["belt"]
    cab = [c * L / 2 for c in t["cab"]]
    pts = [(zr + 0.12, yb)]
    pts += arch(-wb / 2)
    pts += arch(wb / 2)
    pts += [(zf - 0.10, yb), (zf, yb + 0.16),           # front bumper bottom chamfer
            (zf, hood - 0.16),                        # front fascia (front texture)
            (zf - 0.14, hood - 0.04), (zf - 0.55, hood), (cab[3], belt)]  # hood
    if cab[0] - zr > 0.35:                            # three-box: a trunk deck behind the cabin
        pts += [(cab[0], belt), (zr + 0.25, deck), (zr + 0.04, deck - 0.06)]
    else:                                             # hatch / wagon / suv: cabin runs to the tail
        pts += [(cab[0], belt), (zr + 0.03, deck - 0.03)]
    pts += [(zr, deck - 0.12), (zr, yb + 0.18)]       # rear fascia (rear texture)
    if area(pts) < 0: pts = pts[::-1]
    return pts


def side_uv(z, y, t):
    seam_z = -0.1
    u = (z - seam_z) / DOOR_PITCH + 0.52
    v = 0.06 + (t["belt"] - y) / (t["belt"] - t["yb"]) * 0.88
    return (u, v)


PLAIN = lambda a, b: (0.06 + 0.06 * a, 0.80 + 0.06 * b)  # a clean patch of the panel texture (no seam/handle)
DARK = (0.012, 0.012)                                    # black corner of the wheel texture


def build_car(t):
    m = Mesh(); hw = t["W"] / 2; bev = 0.07
    prof = lower_profile(t); inner = offset_poly(prof, bev)
    n = len(prof)
    L, zf, zr = t["L"], t["L"] / 2, -t["L"] / 2
    cab_base = (t["cab"][0] * L / 2, t["cab"][3] * L / 2)
    # side caps (inset profile at the full width) + bevel ring + edge strips at hw-bev
    for s in (+1, -1):
        m.poly("body", inner, lambda p, s=s: (s * hw, p[1], p[0]), lambda p: side_uv(p[0], p[1], t), np.array([s, 0, 0]))
    for i in range(n):
        a, b = prof[i], prof[(i + 1) % n]; ia, ib = inner[i], inner[(i + 1) % n]
        dz, dy = b[0] - a[0], b[1] - a[1]
        out = np.array([0, -dz, dy]); out = out / np.linalg.norm(out)  # outward edge normal (CCW in z,y)
        out = np.array([0, out[1], out[2]])
        is_arch = a[1] < t["r"] + 0.1 and b[1] < t["r"] + 0.1 + 0.07 and abs(dz) < 0.5 and not (a[1] <= t["yb"] + 1e-6 and b[1] <= t["yb"] + 1e-6) and (min(a[1], b[1]) > t["yb"] - 1e-6) and (max(a[1], b[1]) > t["yb"] + 0.01)
        bottom = a[1] <= t["yb"] + 1e-6 and b[1] <= t["yb"] + 1e-6
        front = abs(a[0] - zf) < 1e-6 and abs(b[0] - zf) < 1e-6
        rear = abs(a[0] - zr) < 1e-6 and abs(b[0] - zr) < 1e-6
        P = [(-(hw - bev), a[1], a[0]), (hw - bev, a[1], a[0]), (hw - bev, b[1], b[0]), (-(hw - bev), b[1], b[0])]
        if front or rear:
            ys = sorted([a[1], b[1]]); mat = "front" if front else "rear"
            def fuv(p, mat=mat):
                u = (p[0] + hw) / (2 * hw); u = u if mat == "front" else 1 - u
                return (u, (ys[1] - p[1]) / (ys[1] - ys[0]))
            m.quad(mat, P, [fuv(q) for q in P], out)
        elif bottom or is_arch:
            m.quad("wheel", P, [DARK] * 4, out)
        elif out[1] > 0.45:  # hood / deck / roof-line surfaces get a crown across the width
            at_cab = lambda p: (any(abs(p[0] - c) < 1e-6 and abs(p[1] - t["belt"]) < 1e-6 for c in cab_base)
                                or abs(p[0] - zf) < 1e-6 or abs(p[0] - zr) < 1e-6)  # meet the fascia / cabin flush
            ca, cb = (0.0 if at_cab(a) else 1.0), (0.0 if at_cab(b) else 1.0)
            crown = lambda u, v: np.array([0, 0.045 * (1 - (2 * u - 1) ** 2) * (ca * (1 - v) + cb * v), 0])
            m.grid("body", P, [PLAIN(0, 0), PLAIN(1, 0), PLAIN(1, 1), PLAIN(0, 1)], out, 8, 1, crown)
        else:
            m.quad("body", P, [PLAIN(0, 0), PLAIN(1, 0), PLAIN(1, 1), PLAIN(0, 1)], out)
        # bevel strips joining the edge strip (x=hw-bev, full profile) to the side cap (x=hw, inset profile)
        for s in (+1, -1):
            Q = [(s * (hw - bev), a[1], a[0]), (s * (hw - bev), b[1], b[0]), (s * hw, ib[1], ib[0]), (s * hw, ia[1], ia[0])]
            want = out + np.array([s * 0.7, 0, 0])
            if bottom or is_arch:
                m.quad("wheel", Q, [DARK] * 4, want)
            else:
                m.quad("body", Q, [side_uv(q[2], q[1], t) for q in Q], want)
    # front/rear bevel corners already covered by strips; now the greenhouse
    cab = [c * L / 2 for c in t["cab"]]
    belt, roof = t["belt"], t["roof"]
    g2 = [(cab[0], belt), (cab[3], belt), (cab[2], roof), (cab[1], roof)]  # CCW in (z,y)
    hb, hr = hw - 0.06, hw - t.get("tumble", 0.20)  # tumblehome: the roof is narrower than the beltline
    xw = lambda y: hb + (hr - hb) * (y - belt) / (roof - belt)
    for s in (+1, -1):
        to3 = lambda p, s=s, o=0.0: (s * (xw(p[1]) + o), p[1], p[0])
        sn = np.array([s * (roof - belt), hb - hr, 0]); sn /= np.linalg.norm(sn)
        m.poly("body", g2, to3, lambda p: PLAIN(0.5, 0.5), sn)
        gi = offset_poly(g2, 0.07)  # window opening
        gi = [(z, min(y, roof - 0.07)) for z, y in gi]
        gi = [(z, max(y, belt + 0.06)) for z, y in gi]
        to3g = lambda p, s=s: (s * (xw(p[1]) + 0.008), p[1], p[0])
        zs = [p[0] for p in gi]; z0, z1 = min(zs), max(zs)
        m.poly("glass", gi, to3g, lambda p: ((p[0] - z0) / (z1 - z0), 1 - (p[1] - belt) / (roof - belt)), sn)
        # B pillar (and a C pillar on long cabins) over the glass
        span = cab[3] - cab[0]
        pillars = ([cab[0] + span * 0.22] if t.get("two_door") and span > 2.0 else [] if t.get("two_door") else
                   [cab[0] + span * 0.52] + ([cab[0] + span * 0.18] if span > 2.6 else []))
        for zp in pillars:
            q = [(zp - 0.05, belt + 0.03), (zp + 0.05, belt + 0.03), (zp + 0.05, roof - 0.03), (zp - 0.05, roof - 0.03)]
            m.quad("body", [(s * (xw(y) + 0.016), y, z) for z, y in q], [PLAIN(0, 0)] * 4, sn)
        # mirror: a small wedge box at the A-pillar base
        mz, my = cab[3] - 0.12, belt + 0.05; mx0, mx1 = hw - 0.02, hw + 0.17
        mb = [(mx0, my, mz), (mx1, my, mz - 0.04), (mx1, my + 0.13, mz - 0.04), (mx0, my + 0.15, mz),
              (mx0, my, mz - 0.12), (mx1, my + 0.01, mz - 0.12), (mx1, my + 0.12, mz - 0.12), (mx0, my + 0.15, mz - 0.12)]
        mb = [(s * x, y, z) for x, y, z in mb]
        for f, w in (((0, 1, 2, 3), (0, 0, 1)), ((5, 4, 7, 6), (0, 0, -1)), ((1, 5, 6, 2), (s, 0, 0)), ((3, 2, 6, 7), (0, 1, 0)), ((0, 4, 5, 1), (0, -1, 0))):
            m.quad("body", [mb[i] for i in f], [PLAIN(0.3, 0.3)] * 4, np.array(w, float))
    # windshield, rear window, and a chamfered roof (flat top + two bevel strips + end caps)
    ch = 0.045; hr2 = hr - 0.09
    for (a, b), mat in (((g2[1], g2[2]), "glass"), ((g2[3], g2[0]), "glass")):
        P = [(-xw(a[1]), a[1], a[0]), (xw(a[1]), a[1], a[0]), (xw(b[1]), b[1], b[0]), (-xw(b[1]), b[1], b[0])]
        dz, dy = b[0] - a[0], b[1] - a[1]; on = np.array([0, -dz, dy]); on /= np.linalg.norm(on)
        bulge = lambda u, v, on=on: on * 0.055 * (1 - (2 * u - 1) ** 2) * math.sin(math.pi * v)  # curved glass
        m.grid(mat, P, [(0, 1), (1, 1), (1, 0), (0, 0)], on, 8, 4, bulge)
    zA, zB = g2[2][0], g2[3][0]
    pl = [PLAIN(0, 0), PLAIN(1, 0), PLAIN(1, 1), PLAIN(0, 1)]
    m.grid("body", [(-hr2, roof + ch, zA), (hr2, roof + ch, zA), (hr2, roof + ch, zB), (-hr2, roof + ch, zB)], pl, np.array([0, 1, 0]), 8, 4,
           lambda u, v: np.array([0, 0.035 * (1 - (2 * u - 1) ** 2) * math.sin(math.pi * v) ** 0.5, 0]))
    for s in (+1, -1):
        m.quad("body", [(s * hr, roof, zA), (s * hr2, roof + ch, zA), (s * hr2, roof + ch, zB), (s * hr, roof, zB)], pl, np.array([s, 1.5, 0]))
    for z, d in ((zA, 1), (zB, -1)):
        m.quad("body", [(-hr, roof, z), (hr, roof, z), (hr2, roof + ch, z), (-hr2, roof + ch, z)], pl, np.array([0, 0.3, d]))
    # bumpers: dark plastic lips that stick out past the fascia, plus corner returns
    bz = 0.07
    box(m, "wheel", -hw + 0.04, hw - 0.04, t["yb"] + 0.01, t["yb"] + 0.15, zf - 0.08, zf + bz, DARK)
    box(m, "wheel", -hw + 0.04, hw - 0.04, t["yb"] + 0.02, t["yb"] + 0.16, zr - bz, zr + 0.08, DARK)
    # side skirts between the arches
    for s in (+1, -1):
        za, zb_ = -t["wb"] / 2 + t["r"] + 0.08, t["wb"] / 2 - t["r"] - 0.08
        box(m, "wheel", s * hw - 0.02 if s > 0 else -hw - 0.015, s * hw + 0.015 if s > 0 else -hw + 0.02, t["yb"], t["yb"] + 0.09, za, zb_, DARK)
    # wheel-arch flares: a lip ring around every arch (dark cladding on off-roaders, body colour otherwise)
    ra = t["r"] + 0.06; fl = 0.06 if t.get("flares") else 0.025; wd = 0.09 if t.get("flares") else 0.05
    th0 = math.asin(max(-1, min(1, (t["yb"] - t["r"]) / ra)))
    for zc in (t["wb"] / 2, -t["wb"] / 2):
        angs = np.linspace(th0, math.pi - th0, 10)
        for s in (+1, -1):
            for a0, a1 in zip(angs[:-1], angs[1:]):
                c0, c1 = (math.cos(a0), math.sin(a0)), (math.cos(a1), math.sin(a1))
                pin = lambda c: (s * (hw + 0.002), t["r"] + ra * c[1], zc + ra * c[0])
                pout = lambda c: (s * (hw + fl), t["r"] + (ra + wd) * c[1], zc + (ra + wd) * c[0])
                mat, uv = ("wheel", [DARK] * 4) if t.get("flares") else ("body", [PLAIN(0.5, 0.5)] * 4)
                m.quad(mat, [pin(c0), pin(c1), pout(c1), pout(c0)], uv, np.array([s, 0.4 * (c0[1] + c1[1]), 0.4 * (c0[0] + c1[0])]))
                m.quad("wheel", [pin(c0), pin(c1), (s * (hw - 0.06), *pin(c1)[1:]), (s * (hw - 0.06), *pin(c0)[1:])], [DARK] * 4,
                       np.array([0, -(c0[1] + c1[1]), -(c0[0] + c1[0])]))
    # roof rails, spoiler, taxi sign, pickup bed rails
    if t.get("rails"):
        for s in (+1, -1):
            x = s * (hr2 - 0.05)
            box(m, "wheel", x - 0.03, x + 0.03, roof + ch, roof + ch + 0.06, zB + 0.12, zA - 0.12, DARK)
            for zz in (zB + 0.12, zA - 0.18):
                box(m, "wheel", x - 0.025, x + 0.025, roof + ch, roof + ch + 0.06, zz, zz + 0.06, DARK)
    if t.get("spoiler"):
        if cab[0] - zr > 0.35:  # trunk lip
            box(m, "body", -hw + 0.12, hw - 0.12, t["deck"] + 0.02, t["deck"] + 0.08, zr + 0.06, zr + 0.24, PLAIN(0.5, 0.5))
        else:                   # roof wing
            box(m, "body", -hr2, hr2, roof + ch - 0.02, roof + ch + 0.03, zB - 0.12, zB + 0.12, PLAIN(0.5, 0.5))
    if t.get("sign"):
        box(m, "body", -0.30, 0.30, roof + ch, roof + ch + 0.16, (zA + zB) / 2 - 0.10, (zA + zB) / 2 + 0.10, PLAIN(0.2, 0.2))
        box(m, "wheel", -0.32, 0.32, roof + ch + 0.16, roof + ch + 0.19, (zA + zB) / 2 - 0.11, (zA + zB) / 2 + 0.11, DARK)
    if t.get("bed"):
        for s in (+1, -1):
            box(m, "body", s * hw - 0.08 if s > 0 else -hw, s * hw if s > 0 else -hw + 0.08, t["deck"], t["deck"] + 0.22, zr + 0.05, cab[0], PLAIN(0.4, 0.4))
        box(m, "body", -hw, hw, t["deck"], t["deck"] + 0.22, zr + 0.02, zr + 0.10, PLAIN(0.4, 0.4))
        box(m, "wheel", -hw + 0.1, hw - 0.1, t["deck"] + 0.005, t["deck"] + 0.02, zr + 0.10, cab[0] - 0.02, DARK)  # bed liner
    # wheels: 20 sides; the rim face is recessed inside a tyre sidewall ring, tread from the tyre band
    r, tw, seg, rec, rin = t["r"], 0.23 if t.get("flares") else 0.21, t.get("seg", 28), 0.035, 0.70
    for zc in (t["wb"] / 2, -t["wb"] / 2):
        for s in (+1, -1):
            xo, xi, xr = s * (hw - 0.02), s * (hw - 0.02 - tw), s * (hw - 0.02 - rec)
            ring = [(math.cos(2 * math.pi * k / seg), math.sin(2 * math.pi * k / seg)) for k in range(seg)]
            p = lambda c, x, f=1.0: (x, r + r * f * c[1], zc + r * f * c[0])
            tuv = lambda c, f: (0.5 + 0.5 * f * c[0] * s, 0.5 - 0.5 * f * c[1])
            for k in range(seg):
                c0, c1 = ring[k], ring[(k + 1) % seg]
                # sidewall bulges out slightly at mid radius
                m.quad("wheel", [p(c0, xo, rin), p(c1, xo, rin), p(c1, xo - s * 0.01, 1.0), p(c0, xo - s * 0.01, 1.0)],
                       [tuv(c0, rin), tuv(c1, rin), tuv(c1, 1.0), tuv(c0, 1.0)], np.array([s, 0, 0]))
                m.quad("wheel", [p(c0, xo, rin), p(c1, xo, rin), p(c1, xr, rin), p(c0, xr, rin)], [tuv(c0, rin * 0.98), tuv(c1, rin * 0.98), tuv(c1, rin), tuv(c0, rin)],
                       -np.array([0, c0[1] + c1[1], c0[0] + c1[0]]))
                m.tri("wheel", [p((0, 0), xr), p(c0, xr, rin), p(c1, xr, rin)], [(0.5, 0.5), tuv(c0, rin), tuv(c1, rin)], np.array([s, 0, 0]))
                u0, u1 = 0.40 + 0.2 * k / seg, 0.40 + 0.2 * (k + 1) / seg
                m.quad("wheel", [p(c0, xi), p(c0, xo - s * 0.01), p(c1, xo - s * 0.01), p(c1, xi)], [(u0, 0.035), (u0, 0.005), (u1, 0.005), (u1, 0.035)],
                       np.array([0, c0[1] + c1[1], c0[0] + c1[0]]))
    return m


def hexa(m, mat, c, uv):
    """Closed 6-face solid from 8 corners: c[0..3] = back face (z-) ccw, c[4..7] = front face; each corner (x,y,z).
    Corner order per face: bottom-left, bottom-right, top-right, top-left seen from +z."""
    c = [np.array(p, float) for p in c]; ctr = sum(c) / 8
    faces = [(0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4), (3, 2, 6, 7), (1, 2, 6, 5), (0, 3, 7, 4)]
    for f in faces:
        P = [c[i] for i in f]; fc = sum(P) / 4
        m.quad(mat, P, [uv] * 4 if not callable(uv) else [uv(f, k) for k in range(4)], fc - ctr)


def tapered(m, mat, z0, z1, back, front, uv):
    """Solid between two axis-aligned cross-sections: back/front = (half_w, y0, y1) at z0/z1."""
    (bw, by0, by1), (fw, fy0, fy1) = back, front
    hexa(m, mat, [(-bw, by0, z0), (bw, by0, z0), (bw, by1, z0), (-bw, by1, z0),
                  (-fw, fy0, z1), (fw, fy0, z1), (fw, fy1, z1), (-fw, fy1, z1)], uv)


def strut(m, mat, a, b, hw_, ht, uv):
    """Square bar from point a to point b (in the y/z plane, x given by a[0]) -- forks, swingarm, frame."""
    a, b = np.array(a, float), np.array(b, float); d = b - a; d /= np.linalg.norm(d)
    n = np.array([0, -d[2], d[1]]) * ht
    x = np.array([hw_, 0, 0])
    hexa(m, mat, [a - x - n, a + x - n, a + x + n, a - x + n, b - x - n, b + x - n, b + x + n, b - x + n], uv)


MOTO_LOD = 0.36  # segment multiplier for the bike's tubes/lofts (1.0 = full detail)
METAL = (0.45, 0.625)  # bright flat spot on the rim photo: polished alloy / chrome


def tube(m, mat, a, b, ra, uv, seg=8, rb=None, caps=True):
    """Round tube (or cone when rb is given) from a to b."""
    seg = max(4, int(round(seg * MOTO_LOD)))
    caps = caps and max(ra, rb or 0) > 0.03  # thin rods: ends are never seen
    a, b = np.array(a, float), np.array(b, float); d = b - a; d /= np.linalg.norm(d)
    up = np.array([1.0, 0, 0]) if abs(d[0]) < 0.9 else np.array([0, 1.0, 0])
    e1 = np.cross(d, up); e1 /= np.linalg.norm(e1); e2 = np.cross(d, e1); rb = ra if rb is None else rb
    ring = lambda c, r: [c + r * (math.cos(2 * math.pi * k / seg) * e1 + math.sin(2 * math.pi * k / seg) * e2) for k in range(seg)]
    A, B = ring(a, ra), ring(b, rb)
    U = uv if callable(uv) else (lambda *_: uv)
    for k in range(seg):
        k1 = (k + 1) % seg; mid = (A[k] + A[k1] + B[k] + B[k1]) / 4
        m.quad(mat, [A[k], A[k1], B[k1], B[k]], [U(k, 0), U(k1, 0), U(k1, 1), U(k, 1)], mid - (a + b) / 2)
    if caps:
        for C, R, w in ((a, A, -d), (b, B, d)):
            for k in range(seg): m.tri(mat, [C, R[k], R[(k + 1) % seg]], [U(k, 0)] * 3, w)


def loft(m, mat, rings, uv, caps=True):
    """Skin consecutive rings of equal point count; cap the ends with fans."""
    R = [[np.array(p, float) for p in r] for r in rings]; n = len(R[0])
    ctr = [sum(r) / n for r in R]
    for i in range(len(R) - 1):
        for k in range(n):
            k1 = (k + 1) % n; q = [R[i][k], R[i][k1], R[i + 1][k1], R[i + 1][k]]
            m.quad(mat, q, [uv] * 4, sum(q) / 4 - (ctr[i] + ctr[i + 1]) / 2)
    if caps:
        for r, c, s in ((R[0], ctr[0], -1), (R[-1], ctr[-1], 1)):
            w = ctr[-1] - ctr[0] if len(R) > 1 else np.array([0, 0, 1.0])
            for k in range(n): m.tri(mat, [c, r[k], r[(k + 1) % n]], [uv] * 3, s * w)


def sring(z, cx, y0, y1, hw_, n=14, sq=3.0, top_bias=0.0):
    """Superellipse cross-section at depth z (rounded-box look): x half-width hw_, spanning y0..y1."""
    n = max(6, 2 * int(round(n * MOTO_LOD / 2)))
    cy, hy = (y0 + y1) / 2, (y1 - y0) / 2; out = []
    for k in range(n):
        a = 2 * math.pi * k / n; c, s = math.cos(a), math.sin(a)
        x = hw_ * math.copysign(abs(c) ** (2 / sq), c) * (1 - top_bias * max(0, s))
        y = hy * math.copysign(abs(s) ** (2 / sq), s)
        out.append((cx + x, cy + y, z))
    return out


def build_moto():
    """Naked street motorcycle, ~2.1 m long, front +Z. Body colour on tank, nacelle, fenders, side covers and tail."""
    m = Mesh(); r, wb, seg = 0.31, 1.42, 12
    zf, zr = wb / 2, -wb / 2
    PB = PLAIN(0.5, 0.5)
    # ---- wheels: rounded tyre profile (crown + shoulders), recessed textured rim disc each side
    for zc, tw in ((zf, 0.12), (zr, 0.16)):
        ring = [(math.cos(2 * math.pi * k / seg), math.sin(2 * math.pi * k / seg)) for k in range(seg)]
        p = lambda c, x, rr: (x, r + rr * c[1], zc + rr * c[0])
        rs, rc = r - 0.04, r - 0.075  # shoulder radius, rim-face radius
        for k in range(seg):
            c0, c1 = ring[k], ring[(k + 1) % seg]
            u0, u1 = 0.40 + 0.2 * k / seg, 0.40 + 0.2 * (k + 1) / seg
            nn = np.array([0, c0[1] + c1[1], c0[0] + c1[0]])
            m.quad("wheel", [p(c0, -tw * 0.3, r), p(c0, tw * 0.3, r), p(c1, tw * 0.3, r), p(c1, -tw * 0.3, r)], [(u0, 0.03), (u0, 0.008), (u1, 0.008), (u1, 0.03)], nn)
            for s in (+1, -1):  # one sloped sidewall from the tread edge down to the rim face
                g = rc / r
                m.quad("wheel", [p(c0, s * tw * 0.3, r), p(c0, s * tw * 0.36, rc), p(c1, s * tw * 0.36, rc), p(c1, s * tw * 0.3, r)],
                       [(0.5 + 0.5 * c0[0] * s, 0.5 - 0.5 * c0[1]), (0.5 + 0.5 * g * c0[0] * s, 0.5 - 0.5 * g * c0[1]),
                        (0.5 + 0.5 * g * c1[0] * s, 0.5 - 0.5 * g * c1[1]), (0.5 + 0.5 * c1[0] * s, 0.5 - 0.5 * c1[1])], nn + np.array([s * 2.0, 0, 0]))
                # rim face: the photo's rim scaled so its tyre ring is replaced by the modelled tyre
                h = 0.80
                m.tri("wheel", [(s * tw * 0.36, r, zc), p(c0, s * tw * 0.36, rc), p(c1, s * tw * 0.36, rc)],
                      [(0.5, 0.5), (0.5 + 0.5 * h * c0[0] * s, 0.5 - 0.5 * h * c0[1]), (0.5 + 0.5 * h * c1[0] * s, 0.5 - 0.5 * h * c1[1])], np.array([s, 0, 0]))
        # hub and axle nuts
        tube(m, "metal", (-tw * 0.45, r, zc), (tw * 0.45, r, zc), 0.035, METAL, 8, caps=False)
    # ---- brake discs: twin front, single rear (right), with calipers
    for x, zc, rad in ((0.075, zf, 0.15), (-0.075, zf, 0.15), (0.09, zr, 0.12)):
        sx = 1 if x > 0 else -1; nd = 8  # disc: one flat fan facing out
        for k in range(nd):
            a0, a1 = 2 * math.pi * k / nd, 2 * math.pi * (k + 1) / nd
            m.tri("metal", [(x, r, zc), (x, r + rad * math.sin(a0), zc + rad * math.cos(a0)), (x, r + rad * math.sin(a1), zc + rad * math.cos(a1))], [METAL] * 3, np.array([sx, 0, 0]))
        cal = (zc - 0.10, r + 0.09) if zc > 0 else (zc + 0.09, r + 0.07)
        box(m, "wheel", x - 0.02 if x > 0 else x - 0.03, x + 0.03 if x > 0 else x + 0.02, cal[1] - 0.06, cal[1] + 0.06, cal[0] - 0.04, cal[0] + 0.04, DARK)
    # ---- front fork: chrome stanchions into dark lower legs, triple clamps, steering head
    head = np.array([0, 1.00, zf - 0.30])
    for s in (+1, -1):
        bot, top = np.array([s * 0.10, r, zf]), np.array([s * 0.09, 1.04, zf - 0.32])
        mid = bot + (top - bot) * 0.42
        tube(m, "wheel", bot - (top - bot) * 0.02, mid, 0.036, DARK, 10, caps=False)
        tube(m, "metal", mid, top, 0.026, METAL, 10)
    for y in (0.90, 1.04):
        t_ = (y - r) / (1.04 - r); z = zf + (zf - 0.32 - zf) * t_
        box(m, "wheel", -0.13, 0.13, y - 0.025, y + 0.012, z - 0.05, z + 0.04, METAL if y > 1 else DARK)
    # ---- frame: twin spars steering head -> swingarm pivot, subframe under the seat, engine cradle
    piv = (0.0, 0.48, -0.16)
    for s in (+1, -1):
        tube(m, "wheel", (s * 0.06, 0.98, zf - 0.34), (s * 0.12, 0.78, 0.15), 0.028, DARK, 8)
        tube(m, "wheel", (s * 0.12, 0.78, 0.15), (s * 0.12, 0.52, -0.16), 0.028, DARK, 8)
        tube(m, "wheel", (s * 0.05, 0.96, zf - 0.30), (s * 0.08, 0.32, 0.22), 0.018, DARK, 6)  # downtube
    tube(m, "wheel", (0, 1.07, zf - 0.38), (0, 0.92, zf - 0.22), 0.04, DARK, 10)  # steering head
    # ---- swingarm (box-section, tapered) and rear shock with spring
    for s in (+1, -1):
        tube(m, "wheel", (s * 0.11, 0.48, -0.16), (s * 0.11, r, zr), 0.035, DARK, 4, rb=0.022, caps=False)
    tube(m, "metal", (0, 0.50, -0.42), (0, 0.84, -0.30), 0.024, METAL, 8)
    tube(m, "body", (0, 0.58, -0.40), (0, 0.78, -0.33), 0.042, PLAIN(0.5, 0.5), 6, caps=False)  # spring (one sleeve)
    # ---- engine: crankcase with round side covers, finned cylinder, head, radiator
    loft(m, "wheel", [sring(-0.22, 0, 0.26, 0.60, 0.14, 12), sring(0.05, 0, 0.24, 0.64, 0.155, 12), sring(0.28, 0, 0.28, 0.56, 0.13, 12)], DARK)
    for s in (+1, -1):
        tube(m, "metal", (s * 0.15, 0.42, 0.06), (s * 0.19, 0.42, 0.06), 0.12, METAL, 10, rb=0.10)   # clutch / alternator cover
    cb, ct = np.array([0, 0.56, 0.12]), np.array([0, 0.80, 0.30])
    for k in range(2):  # cooling fins on the inclined cylinder
        c = cb + (ct - cb) * (0.25 + k / 2.5)
        tube(m, "wheel", c, c + (ct - cb) * 0.05, 0.13, DARK if k % 2 else METAL, 10)
    tube(m, "wheel", cb, ct, 0.10, DARK, 10)
    box(m, "metal", -0.12, 0.12, 0.80, 0.86, 0.22, 0.36, METAL)  # head cover
    box(m, "front", -0.17, 0.17, 0.40, 0.70, 0.40, 0.43, (0.5, 0.75))  # radiator core (grille texture)
    m.quad("front", [(-0.15, 0.42, 0.432), (0.15, 0.42, 0.432), (0.15, 0.68, 0.432), (-0.15, 0.68, 0.432)],
           [(0.30, 0.95), (0.70, 0.95), (0.70, 0.55), (0.30, 0.55)], np.array([0, 0, 1]))
    # ---- exhaust: two headers curving under the engine into a tapered muffler on the right
    for x in (0.05,):  # single header
        pts = [(x, 0.70, 0.36), (x * 1.6, 0.42, 0.44), (x * 1.6 + 0.04, 0.20, 0.30), (0.10, 0.18, 0.0), (0.18, 0.26, -0.22)]
        for a, b in zip(pts[:-1], pts[1:]): tube(m, "metal", a, b, 0.022, METAL, 8)
    tube(m, "metal", (0.18, 0.26, -0.22), (0.21, 0.36, -0.36), 0.035, METAL, 10, rb=0.06)
    tube(m, "metal", (0.21, 0.36, -0.36), (0.22, 0.50, -0.80), 0.06, METAL, 14, rb=0.05)
    tube(m, "wheel", (0.22, 0.50, -0.80), (0.222, 0.51, -0.83), 0.03, DARK, 10)
    # ---- final drive: front sprocket, chain run, rear sprocket (left side)
    tube(m, "metal", (-0.13, 0.42, -0.05), (-0.145, 0.42, -0.05), 0.05, METAL, 12)
    tube(m, "metal", (-0.13, r, zr), (-0.142, r, zr), 0.11, METAL, 10)
    # ---- fuel tank: rounded loft with knee cut-outs, filler cap
    T = [(-0.12, 0.13, 0.80, 0.95), (0.06, 0.19, 0.77, 1.05), (0.30, 0.18, 0.80, 1.07), (0.46, 0.10, 0.92, 1.00)]
    loft(m, "body", [sring(z, 0, y0, y1, w, 16, 2.6) for z, w, y0, y1 in T], PB)
    tube(m, "metal", (0, 1.06, 0.25), (0, 1.08, 0.25), 0.04, METAL, 10)
    # ---- seat: two-level, rounded
    S = [(-0.08, 0.13, 0.82, 0.90), (-0.50, 0.13, 0.80, 0.89), (-0.78, 0.09, 0.88, 0.96)]
    loft(m, "wheel", [sring(z, 0, y0, y1, w, 12, 3.0) for z, w, y0, y1 in S], DARK)
    # ---- tail cowl, tail light, rear hugger, licence plate bracket
    C = [(-0.48, 0.14, 0.78, 0.90), (-0.78, 0.11, 0.83, 0.93), (-0.96, 0.05, 0.89, 0.95)]
    loft(m, "body", [sring(z, 0, y0, y1, w, 12, 2.4) for z, w, y0, y1 in C], PB)
    m.quad("rear", [(-0.045, 0.895, -0.966), (0.045, 0.895, -0.966), (0.045, 0.945, -0.966), (-0.045, 0.945, -0.966)],
           [(0.06, 0.32), (0.24, 0.32), (0.24, 0.12), (0.06, 0.12)], np.array([0, 0, -1]))
    tube(m, "wheel", (0, 0.86, -0.90), (0, 0.66, -1.02), 0.012, DARK, 6)
    box(m, "wheel", -0.09, 0.09, 0.56, 0.68, -1.04, -1.02, (0.97, 0.97))  # plate (white corner of rim photo? stays dark-grey)
    for s in (+1, -1):  # indicators
        tube(m, "rear", (s * 0.13, 0.84, -0.94), (s * 0.13, 0.84, -0.99), 0.02, (0.15, 0.25), 8)
    # ---- front: round headlight bucket, lens, instrument pod, small fly screen, indicators
    hl = np.array([0, 0.98, zf - 0.20])
    tube(m, "body", hl - np.array([0, 0, 0.10]), hl, 0.07, PB, 16, rb=0.095)
    seg_l = 10
    for k in range(seg_l):  # flat lens sampling the photo's headlight
        a0, a1 = 2 * math.pi * k / seg_l, 2 * math.pi * (k + 1) / seg_l
        P = lambda a: (0.09 * math.cos(a), hl[1] + 0.09 * math.sin(a), hl[2] + 0.002)
        L = lambda a, f=1: (0.13 + 0.09 * f * math.cos(a), 0.23 - 0.14 * f * math.sin(a))
        m.tri("front", [(0, hl[1], hl[2] + 0.002), P(a0), P(a1)], [L(0, 0), L(a0), L(a1)], np.array([0, 0, 1]))
    tube(m, "wheel", (0, 1.08, zf - 0.40), (0, 1.13, zf - 0.34), 0.06, DARK, 12)  # instrument pod
    m.quad("glass", [(-0.10, 1.06, zf - 0.20), (0.10, 1.06, zf - 0.20), (0.08, 1.22, zf - 0.30), (-0.08, 1.22, zf - 0.30)], [(0, 1), (1, 1), (1, 0), (0, 0)], np.array([0, 0.6, 1]))
    for s in (+1, -1):
        tube(m, "front", (s * 0.18, 0.98, zf - 0.27), (s * 0.18, 0.98, zf - 0.21), 0.02, (0.13, 0.23), 8)
    # ---- handlebar, grips, levers, mirrors, reservoir
    bar = [(-0.36, 1.12, zf - 0.40), (-0.16, 1.08, zf - 0.34), (0.16, 1.08, zf - 0.34), (0.36, 1.12, zf - 0.40)]
    for a, b in zip(bar[:-1], bar[1:]): tube(m, "metal", a, b, 0.012, METAL, 8)
    for s in (+1, -1):
        tube(m, "wheel", (s * 0.27, 1.105, zf - 0.38), (s * 0.38, 1.125, zf - 0.41), 0.019, DARK, 8)
        tube(m, "wheel", (s * 0.22, 1.10, zf - 0.36), (s * 0.27, 1.21, zf - 0.40), 0.006, DARK, 6)
        box(m, "wheel", s * 0.24, s * 0.34, 1.20, 1.25, zf - 0.42, zf - 0.40, DARK)
    # ---- fenders: front with thickness, rear hugger
    for zc, a0, a1, w in ((zf, 0.10, 2.10, 0.075), (zr, 1.35, 2.50, 0.09)):  # single-surface hugger arcs
        rf = r + 0.045; A = np.linspace(a0, a1, 5)
        for q0, q1 in zip(A[:-1], A[1:]):
            P = lambda a, x: (x, r + rf * math.sin(a), zc + rf * math.cos(a))
            m.quad("body", [P(q0, -w), P(q0, w), P(q1, w), P(q1, -w)], [PB] * 4, np.array([0, math.sin((q0 + q1) / 2), math.cos((q0 + q1) / 2)]))
    # ---- side covers, belly pan, footpegs, rear sets, levers, kickstand
    for s in (+1, -1):
        loft(m, "body", [[(s * 0.12, 0.60, -0.18), (s * 0.12, 0.60, -0.48), (s * 0.10, 0.80, -0.56), (s * 0.10, 0.80, -0.12)],
                          [(s * 0.15, 0.62, -0.20), (s * 0.15, 0.62, -0.46), (s * 0.135, 0.78, -0.52), (s * 0.135, 0.78, -0.15)]], PB)
        tube(m, "metal", (s * 0.12, 0.34, -0.12), (s * 0.24, 0.33, -0.13), 0.014, METAL, 6)
        tube(m, "metal", (s * 0.18, 0.33, -0.11), (s * 0.20, 0.30, 0.10), 0.007, METAL, 4)  # shifter / brake pedal
    tube(m, "metal", (-0.13, 0.30, -0.08), (-0.30, 0.01, -0.20), 0.012, METAL, 6)
    return m



def box(m, mat, x0, x1, y0, y1, z0, z1, uv):
    """Axis-aligned box (no bottom) with one uv for every corner -- for trims, rails, lips."""
    x0, x1 = min(x0, x1), max(x0, x1); U = [uv] * 4
    m.quad(mat, [(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], U, np.array([0, 1, 0]))
    m.quad(mat, [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], U, np.array([0, 0, 1]))
    m.quad(mat, [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)], U, np.array([0, 0, -1]))
    m.quad(mat, [(x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0)], U, np.array([1, 0, 0]))
    m.quad(mat, [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)], U, np.array([-1, 0, 0]))


# ---------------------------------------------------------------- glb
SMOOTH_DEG = 32
MAT_PBR = {"body": (0.25, 0.30), "glass": (0.0, 0.08), "front": (0.3, 0.45), "rear": (0.3, 0.45), "wheel": (0.2, 0.7), "metal": (0.9, 0.22), "trim": (0.25, 0.30)}  # metal, rough


def write_glb(path, mesh, maps, colour, wheel="alloy", trim=(0.9, 0.9, 0.9), seat=(0.2, 0.25, 0.45)):
    bin_ = bytearray(); views, accs, images, textures, materials, prims = [], [], [], [], [], []
    def view(data, target=None):
        while len(bin_) % 4: bin_.append(0)
        v = {"buffer": 0, "byteOffset": len(bin_), "byteLength": len(data)}; bin_.extend(data)
        if target: v["target"] = target
        views.append(v); return len(views) - 1
    def img(data, mime):
        images.append({"bufferView": view(data), "mimeType": mime}); textures.append({"source": len(images) - 1}); return len(textures) - 1
    for name in MATS + [k for k in mesh.tris if k not in MATS]:
        tris = mesh.tris[name]
        if not tris: continue
        pos, nrm, uv = [], [], []
        # auto-smooth: a corner averages the (area-weighted) normals of faces sharing its position within SMOOTH_DEG,
        # so wheels, arches, fenders and the roof read as curved while hard panel edges stay crisp
        fn = [np.cross(p[1] - p[0], p[2] - p[0]) for p, _ in tris]
        fu = [n / np.linalg.norm(n) for n in fn]
        key = lambda q: (round(q[0], 4), round(q[1], 4), round(q[2], 4))
        share = {}
        for i, (p, _) in enumerate(tris):
            for q in p: share.setdefault(key(q), []).append(i)
        cos_t = math.cos(math.radians(SMOOTH_DEG))
        for i, (p, u) in enumerate(tris):
            for q in p:
                acc = sum((fn[j] for j in share[key(q)] if np.dot(fu[j], fu[i]) > cos_t), np.zeros(3))
                nrm.append(acc / np.linalg.norm(acc))
            pos += p; uv += u
        pa, na, ua = (np.array(x, np.float32) for x in (pos, nrm, uv))
        ia = np.arange(len(pa), dtype=np.uint16)
        a = len(accs)
        accs += [{"bufferView": view(pa.tobytes(), 34962), "componentType": 5126, "count": len(pa), "type": "VEC3", "min": pa.min(0).tolist(), "max": pa.max(0).tolist()},
                 {"bufferView": view(na.tobytes(), 34962), "componentType": 5126, "count": len(na), "type": "VEC3"},
                 {"bufferView": view(ua.tobytes(), 34962), "componentType": 5126, "count": len(ua), "type": "VEC2"},
                 {"bufferView": view(ia.tobytes(), 34963), "componentType": 5123, "count": len(ia), "type": "SCALAR"}]
        alb, nor = maps[name]; met, rough = MAT_PBR.get(name, (0.0, 0.5))
        aidx = img(alb, "image/png" if alb[1:4] == b"PNG" else "image/jpeg")
        mat = {"name": name, "pbrMetallicRoughness": {"baseColorTexture": {"index": aidx}, "metallicFactor": met, "roughnessFactor": rough}}
        if name == "body":
            mat["pbrMetallicRoughness"]["baseColorFactor"] = [*colour, 1.0]
            mat["extensions"] = {"KHR_materials_clearcoat": {"clearcoatFactor": 1.0, "clearcoatRoughnessFactor": 0.06}}
        if name == "interior":  # InteriorPS: vertex colour = seat fabric
            mat["pbrMetallicRoughness"]["baseColorFactor"] = [*seat, 1.0]
        if name == "trim":
            mat["pbrMetallicRoughness"]["baseColorFactor"] = [*trim, 1.0]
        if name == "sign":  # transit destination sign: lit LEDs
            mat["emissiveTexture"] = {"index": aidx}; mat["emissiveFactor"] = [1.0, 1.0, 1.0]
        if name in ("front", "rear"):  # lamps glow a little: the bright parts of the fascia drive emission
            mat["emissiveTexture"] = {"index": aidx}  # the albedo: bright lamp areas glow
            mat["emissiveFactor"] = [0.25, 0.25, 0.25] if name == "front" else [0.35, 0.12, 0.10]
        if name == "beacon":  # emergency lamps: the lens colours glow (service vehicles)
            mat["emissiveTexture"] = {"index": aidx}; mat["emissiveFactor"] = [2.5, 2.5, 2.5]
        if name == "wheel" and wheel != "alloy":
            f = WHEEL_STYLES[wheel]
            mat["pbrMetallicRoughness"]["baseColorFactor"] = [1.0, 0.78, 0.38, 1.0] if f is None else [f, f, f, 1.0]
            if f is None: mat["pbrMetallicRoughness"]["metallicFactor"] = 0.6
        if nor: mat["normalTexture"] = {"index": img(nor, "image/png")}
        materials.append(mat)
        prims.append({"attributes": {"POSITION": a, "NORMAL": a + 1, "TEXCOORD_0": a + 2}, "indices": a + 3, "material": len(materials) - 1})
    g = {"asset": {"version": "2.0", "generator": "cityengine gen_cars"}, "scene": 0, "scenes": [{"nodes": [0]}],
         "nodes": [{"mesh": 0, "name": os.path.splitext(os.path.basename(path))[0]}], "meshes": [{"primitives": prims}],
         "materials": materials, "textures": textures, "images": images, "accessors": accs, "bufferViews": views,
         "buffers": [{"byteLength": len(bin_)}], "extensionsUsed": ["KHR_materials_clearcoat"]}
    j = json.dumps(g, separators=(",", ":")).encode(); j += b" " * (-len(j) % 4)
    while len(bin_) % 4: bin_.append(0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 28 + len(j) + len(bin_)))
        f.write(struct.pack("<II", len(j), 0x4E4F534A)); f.write(j)
        f.write(struct.pack("<II", len(bin_), 0x004E4942)); f.write(bytes(bin_))


def main():
    os.makedirs(OUT, exist_ok=True)
    maps = load_maps()
    if not sys.argv[1:] or "moto" in sys.argv[1:]:
        mesh = build_moto()
        for i, cn in enumerate(COLOURS):
            out = os.path.join(OUT, f"MOTO_{cn}.glb"); write_glb(out, mesh, maps, COLOURS[cn], ("alloy", "dark", "gold")[i % 3])
        print(f"moto: {mesh.count()} tris, {os.path.getsize(out) / 1e6:.2f} MB per GLB")
    for tn in (a for a in (sys.argv[1:] or TYPES) if a != "moto"):
        mesh = build_car(TYPES[tn])
        styles = list(WHEEL_STYLES)
        for i, cn in enumerate(TYPES[tn].get("colours", COLOURS)):
            ws = styles[(i + len(tn)) % 3] if tn != "taxi" else "alloy"  # spread rim styles across the paint range
            out = os.path.join(OUT, f"CAR_{tn}_{cn}.glb"); write_glb(out, mesh, maps, COLOURS[cn], ws)
        print(f"{tn}: {mesh.count()} tris, {os.path.getsize(out) / 1e6:.2f} MB per GLB")


if __name__ == "__main__":
    main()
