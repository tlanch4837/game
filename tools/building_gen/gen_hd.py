"""High density housing, every building its own: 16 plots (2x2 .. 5x5) x 5 levels = 80 buildings, each under 1200 triangles.

Each building draws its own design (hd_styles.design: the level's family, its colours, windows, balconies, ornament) and its own
massing (slab, blocks, towers, setback tiers, twin towers, tower and wing). Its walls are silhouette-POM volumes over a glass field;
windows, the lobby glazing, the deck balustrade and the pool's water are real panes (GlassPS: Fresnel sky reflection, roughness 0.04).

The podium is a glazed lobby with doors, canopy and the building's name, shopfronts down the sides, a garage door on the driveway
side and a service wall at the back; its roof is the residents' amenity deck: timber or stone paving, a pool (a hot tub on small
plots) with coping, loungers, umbrellas and planters, behind a glass balustrade.

The 5x5 plot carries each level's flagship: the biggest building the plot can take.

Run: python gen_hd.py [levels...] [WxD ...]      Output: assets/buildings/high_density/glb/HD_<W>x<D>_L<level>.glb
"""
import os, sys, math
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
from PIL import Image
import gen_low_density as g
import gen_apartments as ap
import facade_hd as fh
import hd_styles as st
import roomglass as rg
from gen_low_density import Mesh, write_glb, tile_maps, jpg, png, CELL, normal_from_height
from gen_low_density_v2 import drive_tex
from gen_highrise import ROOFS, PAL, OUT, TEX

PODIUM = 8.0          # two storeys: the lobby (double height), shops, and an amenity floor under the pool deck; the 5x5 has three
BUDGET = 1190
FLAT_N = jpg(np.full((8, 8, 3), (128, 128, 255), np.uint8), 95)
MATS = {"glass": dict(rough=0.04, blend=True, alpha=0.2), "field": dict(rough=0.35), "lobby": dict(rough=0.3), "pool": dict(rough=0.15),
        "deck": dict(rough=0.75), "props": dict(rough=0.6), "clad": dict(rough=0.85), "roof": dict(rough=0.9), "signboard": dict(rough=0.5), "roomglass": rg.MAT, "roomglass_lobby": rg.MAT}
PLOTS = [(w, d) for w in range(2, 6) for d in range(2, 6)]   # plots run 2x2 .. 5x5


def fit(v, lo=9.0): return max(lo, math.floor(v / 3.0 + 1e-6) * 3.0)


# ---------------------------------------------------------------- geometry helpers
def quad(m, mat, a, b, c, d, uv):
    m.prims.setdefault(mat, []); m.quad(mat, a, b, c, d, uv)


def cbox(m, mat, x0, x1, y0, y1, z0, z1, uvm=3.0, top=True, uv=None, sides=(0, 1, 2, 3)):
    """A box in one material: four sides (as chosen) and a top. uv: a fixed (u, v) for every vertex (atlas cell) or None for world uv."""
    F = [((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), 0), ((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), 0),
         ((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), 2), ((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0), 2)]
    for k in sides:
        a, b, c, d, ax = F[k]
        quad(m, mat, a, b, c, d, [uv] * 4 if uv else [(q[ax] / uvm, -q[1] / uvm) for q in (a, b, c, d)])
    if top:
        pts = [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]
        quad(m, mat, *pts, [uv] * 4 if uv else [(q[0] / uvm, q[2] / uvm) for q in pts])


def faces_of(x0, x1, z0, z1):
    """(origin x, origin z, T, N, width) for the four faces, origin at the face's left end seen from outside."""
    return [((x0, z1), (1, 0, 0), (0, 0, 1), x1 - x0), ((x1, z0), (-1, 0, 0), (0, 0, -1), x1 - x0),
            ((x1, z1), (0, 0, -1), (1, 0, 0), z1 - z0), ((x0, z0), (0, 0, 1), (-1, 0, 0), z1 - z0)]


def spom_volume(m, mat, origin, N, W, H):
    m.prims.setdefault(mat, [])
    N = np.array(N, np.float32); up = np.array([0, 1, 0], np.float32); T = np.cross(up, N).astype(np.float32)
    O = np.array(origin, np.float32); D = st.DEPTH
    c = {(i, j, k): O + T * i * W + up * j * H + N * k * D for i in (0, 1) for j in (0, 1) for k in (0, 1)}
    centre = O + T * W / 2 + up * H / 2 + N * D / 2
    for f in [((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)), ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)),
              ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)), ((0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)),
              ((1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1))]:
        pts = [c[k] for k in f]
        n = np.cross(pts[1] - pts[0], pts[3] - pts[0])
        if np.dot(n, np.mean(pts, 0) - centre) < 0: f = (f[0], f[3], f[2], f[1]); pts = [c[k] for k in f]
        m.raw_quad(mat, pts, N, [(k[0] * W, k[1] * H) for k in f], [(W, H, k[2] * D) for k in f])


def face_plane(m, mat, o, T, N, w, y0, y1, off, uvfn):
    T = np.array(T, np.float32); N = np.array(N, np.float32)
    O = np.array([o[0], 0, o[1]], np.float32) + N * off
    pts = [O + T * 0 + [0, y0, 0], O + T * w + [0, y0, 0], O + T * w + [0, y1, 0], O + T * 0 + [0, y1, 0]]
    quad(m, mat, *pts, [uvfn(0, y0), uvfn(w, y0), uvfn(w, y1), uvfn(0, y1)])


def runs(n, cap):
    k = max(1, math.ceil(n / cap)); base = n // k; extra = n % k
    out, b = [], 0
    for i in range(k):
        w = base + (1 if i < extra else 0); out.append((b, w)); b += w
    return out


# ---------------------------------------------------------------- a dressed tower box
class Dresser:
    def __init__(self, d, seed, tex, tag=""):
        self.d, self.seed, self.tex, self.tag = d, seed, tex, tag; self.made = {}

    def mat(self, nb, ns, first, top, v, n_face):
        key = f"spom_{self.tag}{nb}x{ns}_{first % 12}_{n_face}_{'t' if top else 'n'}_{v}"
        if key not in self.made:
            self.made[key] = len(self.made)
            if self.tex is not None:
                self.tex[key] = st.strip(self.d, nb, ns, first, self.seed + 7 * len(self.made), top, n_face)
        return key

    def special(self, kind, nb, H=None):
        key = f"spom_{self.tag}sp_{kind}_{nb}" + (f"_{H:.0f}" if H else "")
        if key not in self.made:
            self.made[key] = len(self.made)
            if self.tex is not None: self.tex[key] = st.special_strip(self.d, kind, nb, self.seed + 991 * len(self.made), H)
        return key


def special_kind(dz, g):
    """What storey g (counted from the podium) is, if it breaks the rhythm."""
    if g < 3: return None
    if (g + 1) % dz["mech_every"] == 0: return "mech"
    if (g + 1) % dz["frieze_every"] == 0: return "frieze"
    return None


def dress(m, dz, dresser, box, ns, rng, variants=2, podium=8.0):
    x0, x1, z0, z1, y0, floors = box["x0"], box["x1"], box["z0"], box["z1"], box["y0"], box["floors"]
    g0 = int(round((y0 - podium) / 3.0)); clad = "clad" + dresser.tag
    H = y0 + floors * 3.0; skip = box.get("skip", ())
    vo = float(rng.integers(6)) * 3.0
    for fi, (o, T, N, w) in enumerate(faces_of(x0, x1, z0, z1)):
        f_start = 0
        if fi in skip:                                                     # a wall shared with a neighbour: hidden up to its roof, dressed above
            f_start = int(math.ceil((box.get("cover", {}).get(fi, H) - y0) / 3.0 - 1e-6))
            if f_start >= floors: continue
        ys = y0 + f_start * 3.0
        nb = int(round(w / 3.0)); uo = float(rng.integers(6)) * 3.0
        # the glass field and the pane in front of it
        # the windows: one roomglass plane, a parallax room per 3 m bay and storey (RoomGlassPS)
        rg.pane(m, "roomglass", (o[0], ys, o[1]), T, (0, 1, 0), w, H - ys, room=(3.0, 3.0), seed=float(dresser.seed % 997) + fi * 0.37,
                origin_uv=(uo, vo + ys - y0))
        for (b0, n) in runs(nb, 4):
            f = f_start
            while f < floors:
                sp = special_kind(dz, g0 + f) if f < floors - 2 else None
                if sp is not None:
                    hs = int(st.SPECIAL_H[sp] / 3.0)                               # a sky garden is two storeys high
                    org = np.array([o[0], y0 + f * 3.0, o[1]], np.float32) + np.array(T, np.float32) * b0 * 3.0
                    spom_volume(m, dresser.special(sp, n), org, N, n * 3.0, hs * 3.0); f += hs; continue
                nxt = next((q for q in range(f + 1, floors - 2) if special_kind(dz, g0 + q)), floors)
                k = min(ns, floors - f, nxt - f); top = box["top"] and f + k >= floors
                v = int(rng.integers(variants))
                mat = dresser.mat(n, k, b0, top, v, nb)
                org = np.array([o[0], y0 + f * 3.0, o[1]], np.float32) + np.array(T, np.float32) * b0 * 3.0
                spom_volume(m, mat, org, N, n * 3.0, k * 3.0)
                f += k
    if dz["wall"]:                                                     # corner columns close the wall's thickness
        t = dz["T"] + (0.14 if dz["level"] == 4 else 0.0)
        for (cx, cz, sides) in ((x0 - t, z1, (0, 3)), (x1, z1, (0, 2)), (x0 - t, z0 - t, (1, 3)), (x1, z0 - t, (1, 2))):
            cbox(m, clad, cx, cx + t, y0, H, cz, cz + t, top=False, sides=sides)
    quad(m, "roof", (x0, H, z1), (x1, H, z1), (x1, H, z0), (x0, H, z0), [(x0 / 2.5, z1 / 2.5), (x1 / 2.5, z1 / 2.5), (x1 / 2.5, z0 / 2.5), (x0 / 2.5, z0 / 2.5)])
    return H


def octagon(m, mat, cx, cz, r, y0, y1, uvm=2.0):
    pts = [(cx + r * math.cos(k * math.pi / 4 + math.pi / 8), cz + r * math.sin(k * math.pi / 4 + math.pi / 8)) for k in range(8)]
    for k in range(8):
        (ax, az), (bx, bz) = pts[k], pts[(k + 1) % 8]
        quad(m, mat, (bx, y0, bz), (ax, y0, az), (ax, y1, az), (bx, y1, bz), [(k / 2, -y0 / uvm), ((k + 1) / 2, -y0 / uvm), ((k + 1) / 2, -y1 / uvm), (k / 2, -y1 / uvm)])
    return pts


def crown(m, dz, box, rng, tag="", dresser=None):
    """What stands on the roof, by family: a water tower and chimneys, a glass screen or pyramid, a stepped ziggurat, a lantern, a spire."""
    clad = "clad" + tag
    x0, x1, z0, z1 = box["x0"], box["x1"], box["z0"], box["z1"]; H = box["y0"] + box["floors"] * 3.0
    o = dz["T"] + 0.25 if dz["wall"] else 0.6
    L = dz["level"]
    cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
    if not (L == 3 and dz.get("crown") == "screen"):
        cbox(m, clad, x0 - o, x1 + o, H, H + (0.9 if L in (2, 4) else 0.6), z0 - o, z1 + o, top=False)
    top = H + 1.0
    if L == 2:
        bx = x0 + 1.5 + rng.uniform(0, max(0.1, (x1 - x0) - 6))
        cbox(m, clad, bx, bx + 3.0, H, H + 2.6, z0 + 1.0, z0 + 3.5)                      # the stair bulkhead
        if dz.get("water_tower"):                                                         # a timber water tank on a steel stand
            tx = x1 - 3.0 - rng.uniform(0, max(0.1, (x1 - x0) * 0.3)); tz = z0 + (z1 - z0) * rng.uniform(0.3, 0.6); r = 1.6
            for (lx, lz) in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                cbox(m, "props", tx + lx * 1.1 - 0.06, tx + lx * 1.1 + 0.06, H, H + 3.0, tz + lz * 1.1 - 0.06, tz + lz * 1.1 + 0.06, top=False, uv=cell("metal"), sides=(0, 2))
            pts = octagon(m, "props_wood", tx, tz, r, H + 3.0, H + 6.2)
            tip = (tx, H + 7.6, tz)
            for k in range(8):
                (ax, az), (bx_, bz) = pts[k], pts[(k + 1) % 8]
                m.prims.setdefault("props", []); m.tri("props", (bx_, H + 6.2, bz), (ax, H + 6.2, az), tip, [cell("metal")] * 3)
            top = H + 7.6
        if dz.get("chimneys"):
            for k in range(int(rng.integers(2, 4))):
                qx = rng.uniform(x0 + 1, x1 - 2); qz = rng.choice([z0 + 0.6, z1 - 1.4])
                cbox(m, clad, qx, qx + 0.9, H, H + 2.4, qz, qz + 0.8)
    elif L == 3:
        c = dz.get("crown", "flat")
        if c == "screen" and dresser is not None:                                         # the curtain wall rises past the roof as a framed screen
            h = float(rng.choice([4.5, 6.0, 7.5]))
            for (oo, T_, N_, w) in faces_of(x0, x1, z0, z1):
                nbf = int(round(w / 3.0))
                for (b0, n) in runs(nbf, 4):
                    org = np.array([oo[0], H, oo[1]], np.float32) + np.array(T_, np.float32) * b0 * 3.0
                    spom_volume(m, dresser.special("screen", n, h), org, N_, n * 3.0, h)
            cbox(m, clad, cx - 2.5, cx + 2.5, H, H + h - 1.5, cz - 2, cz + 2)
            top = H + h
        elif c == "pyramid":                                                              # a glass pyramid and a mast
            ap_ = (cx, H + min(x1 - x0, z1 - z0) * 0.45, cz)
            cs = [(x0, H + 0.6, z1), (x1, H + 0.6, z1), (x1, H + 0.6, z0), (x0, H + 0.6, z0)]
            m.prims.setdefault("glass", [])
            for k in range(4): m.tri("glass", cs[k], cs[(k + 1) % 4], ap_, [(0, 0), (1, 0), (0.5, 1)])
            cbox(m, clad, cx - 0.12, cx + 0.12, ap_[1] - 1, ap_[1] + 8, cz - 0.12, cz + 0.12, top=False, sides=(0, 2))
            top = ap_[1] + 8
        else:
            cbox(m, clad, cx - 3, cx + 3, H, H + 3.5, cz - 2.5, cz + 2.5)
            cbox(m, clad, cx - 0.15, cx + 0.15, H + 3.5, H + 12, cz - 0.15, cz + 0.15, top=False, sides=(0, 2)); top = H + 12
    elif L == 4:
        c = dz.get("crown", "spire")
        w, d = (x1 - x0), (z1 - z0)
        if c == "ziggurat":                                                               # stepped crown, each step smaller, a flagpole on top
            y = H
            for k in range(3):
                sw, sd = w * (0.7 - 0.18 * k), d * (0.7 - 0.18 * k); hh = 2.4 - 0.4 * k
                cbox(m, clad, cx - sw / 2, cx + sw / 2, y, y + hh, cz - sd / 2, cz + sd / 2); y += hh
            cbox(m, clad, cx - 0.08, cx + 0.08, y, y + 6, cz - 0.08, cz + 0.08, top=False, sides=(0, 2)); top = y + 6
        elif c == "lantern":                                                              # an octagonal lantern under a pointed cap
            r = min(w, d) * 0.28
            cbox(m, clad, cx - r * 1.2, cx + r * 1.2, H, H + 2.5, cz - r * 1.2, cz + r * 1.2)
            pts = octagon(m, clad, cx, cz, r, H + 2.5, H + 7.5)
            tip = (cx, H + 7.5 + r * 2.2, cz)
            for k in range(8):
                (ax, az), (bx_, bz) = pts[k], pts[(k + 1) % 8]
                m.tri(clad, (bx_, H + 7.5, bz), (ax, H + 7.5, az), tip, [(0, 0), (1, 0), (0.5, 1)])
            top = tip[1]
        else:
            cbox(m, clad, cx - w * 0.2, cx + w * 0.2, H, H + 4, cz - d * 0.2, cz + d * 0.2)
            cbox(m, clad, cx - 0.6, cx + 0.6, H + 4, H + 8, cz - 0.6, cz + 0.6)
            ap_ = (cx, H + 22, cz); cs = [(cx - 0.6, H + 8, cz + 0.6), (cx + 0.6, H + 8, cz + 0.6), (cx + 0.6, H + 8, cz - 0.6), (cx - 0.6, H + 8, cz - 0.6)]
            for k in range(4): m.tri(clad, cs[k], cs[(k + 1) % 4], ap_, [(0, 0), (1, 0), (0.5, 1)])
            top = H + 22
    else:
        w, d = min(5.0, (x1 - x0) * 0.4), min(4.0, (z1 - z0) * 0.4)
        cbox(m, clad, cx - w / 2, cx + w / 2, H, H + 3.0, cz - d / 2, cz + d / 2); top = H + 3.0
    return top


def terrace_planters(m, box, below):
    """Planters along the edge of a setback roof (the part of `below` that `box` does not cover)."""
    y = box["y0"]; n = 0
    for (x, z) in ((below["x0"] + 0.4, below["z1"] - 1.2), (below["x1"] - 1.4, below["z1"] - 1.2), ((below["x0"] + below["x1"]) / 2 - 0.5, below["z1"] - 1.2)):
        if box["x0"] - 0.2 < x < box["x1"] + 0.2 and box["z0"] - 0.2 < z < box["z1"] + 0.2: continue
        cbox(m, "props", x, x + 1.0, y, y + 0.5, z, z + 1.0, uv=cell("planter"), top=False, sides=(0, 1, 2, 3))
        tip = (x + 0.5, y + 1.8, z + 0.5); bs = [(x, y + 0.5, z), (x + 1, y + 0.5, z), (x + 1, y + 0.5, z + 1), (x, y + 0.5, z + 1)]
        for k in range(4): m.tri("props", bs[(k + 1) % 4], bs[k], tip, [cell("leaf")] * 3)
        n += 1


# ---------------------------------------------------------------- massing
def floors_for(level, W, D, rng, big):
    if big: return {1: 26, 2: 22, 3: 60, 4: 72, 5: 50}[level]
    a = W * D
    base = {1: 6 + a * 0.35, 2: 6 + a * 0.3, 3: 12 + a * 0.6, 4: 14 + a * 0.7, 5: 12 + a * 0.5}[level]
    return max(5, int(base + rng.integers(-3, 4)))


def massing(level, W, D, rng, rx0, rx1, rz0, rz1, y0, big):
    rw, rd = rx1 - rx0, rz1 - rz0
    F = floors_for(level, W, D, rng, big)
    boxes = []
    def place(tw, td, x=None):
        x = (rx0 + rx1) / 2 - tw / 2 if x is None else x
        return dict(x0=x, x1=x + tw, z0=rz0, z1=rz0 + td)
    if level == 1:
        tw = fit(min(rw, 33 if big else float(rng.choice([18, 21, 24, 27, 30])))); td = fit(min(rd, 15 if big else float(rng.choice([9, 12]))))
        b = place(tw, td); b.update(y0=y0, floors=F, top=True); boxes.append(b)
        if not big and rw - tw >= 9 and rng.random() < 0.4:                     # a lower wing beside it
            ww = fit(min(rw - tw, 12)); left = rng.random() < 0.5
            b["x0"], b["x1"] = (rx1 - tw, rx1) if left else (rx0, rx0 + tw)
            wx = rx0 if left else rx0 + tw
            boxes.append(dict(x0=wx, x1=wx + ww, z0=rz0, z1=rz0 + 9.0, y0=y0, floors=max(4, int(F * 0.55)), top=True, skip=(2,) if left else (3,)))
    elif level == 2:
        if big:
            tw, td = fit(min(rw, 33)), fit(min(rd, 21)); b = place(tw, td); b.update(y0=y0, floors=F, top=True); boxes.append(b)
        else:
            n = 1 if rw < 18 else int(rng.choice([1, 2, 2, 3])) if rw >= 27 else int(rng.choice([1, 2]))   # a row of blocks, each its own building
            widths = []; left = rw
            for k in range(n):
                wk = fit(min(left - 9 * (n - k - 1), float(rng.choice([9, 12, 15, 18, 21])))); widths.append(wk); left -= wk
            x = (rx0 + rx1) / 2 - sum(widths) / 2
            for k, wk in enumerate(widths):
                fl = F if k == 0 else max(4, int(F * rng.uniform(0.55, 1.35)))
                td = fit(min(rd, float(rng.choice([9, 12, 15]))))
                skip = ((2,) if k < n - 1 else ()) + ((3,) if k > 0 else ())
                boxes.append(dict(x0=x, x1=x + wk, z0=rz0, z1=rz0 + td, y0=y0, floors=fl, top=True, skip=skip, di=k)); x += wk
    elif level == 3:
        tw = fit(min(rw * 0.85, 30 if big else float(rng.choice([15, 18, 21, 24])))); td = fit(min(rd, 30 if big else float(rng.choice([12, 15, 18, 21]))))
        shape = "taper" if big else str(rng.choice(["tower", "setback", "taper", "offset", "slab"]))
        if shape == "slab" and rw >= 24: tw, td = fit(min(rw * 0.9, 33)), 12.0
        b = place(tw, td)
        if shape in ("setback", "taper"):                                       # upper tiers stepping in
            tiers = 2 if shape == "setback" else 3
            fl = [int(F * f) for f in ((0.6, 0.4) if tiers == 2 else (0.45, 0.33, 0.22))]; fl[-1] = F - sum(fl[:-1])
            y = y0; cur = dict(b)
            for k, nf in enumerate(fl):
                t = dict(cur, y0=y, floors=nf, top=k == tiers - 1)
                boxes.append(t); y += nf * 3.0
                nxt = dict(x0=cur["x0"] + 3, x1=cur["x1"] - 3, z0=cur["z0"], z1=cur["z1"] - 3)
                if nxt["x1"] - nxt["x0"] < 9 or nxt["z1"] - nxt["z0"] < 9: t["floors"] += sum(fl[k + 1:]); t["top"] = True; break
                cur = nxt
        elif shape == "offset" and rw >= 21:                                     # two slabs of different heights, overlapping
            wa = fit(min(rw * 0.55, 18)); wb = fit(min(rw - wa, 15))
            xa = (rx0 + rx1) / 2 - (wa + wb) / 2
            boxes.append(dict(x0=xa, x1=xa + wa, z0=rz0, z1=rz0 + td, y0=y0, floors=F, top=True, skip=(2,)))
            boxes.append(dict(x0=xa + wa, x1=xa + wa + wb, z0=rz0, z1=rz0 + fit(min(rd, td - 3 if td > 12 else td)), y0=y0, floors=max(8, int(F * 0.65)), top=True, skip=(3,)))
        else:
            b.update(y0=y0, floors=F, top=True); boxes.append(b)
    elif level == 4:
        sx = fit(min(rw * 0.9, 33 if big else 27), 12.0); sz = fit(min(rd, 30 if big else 24), 12.0)
        nt = 4 if big else int(rng.choice([2, 3, 3, 4]))
        inset = float(rng.choice([3.0, 3.0, 6.0])); back = rng.random() < 0.5          # wedding cake, or stepping only at the front and sides
        cx = (rx0 + rx1) / 2; y = y0
        shares = np.array(sorted(rng.uniform(0.6, 1.4, nt), reverse=True)); shares = shares / shares.sum()
        fl = [max(2, int(F * f)) for f in shares]
        x0_, x1_, z0_, z1_ = cx - sx / 2, cx + sx / 2, rz0, rz0 + sz
        for k, nf in enumerate(fl):
            boxes.append(dict(x0=x0_, x1=x1_, z0=z0_, z1=z1_, y0=y, floors=nf, top=True)); y += nf * 3.0
            x0_ += inset / 2 if inset == 3.0 else 3.0; x1_ -= inset / 2 if inset == 3.0 else 3.0
            x0_, x1_ = cx - fit(x1_ - x0_, 3.0) / 2, cx + fit(x1_ - x0_, 3.0) / 2
            z1_ -= 3.0
            if not back: z0_ += 3.0
            if x1_ - x0_ < 9 or z1_ - z0_ < 9: break
    else:
        mode = "single"
        if big: mode = "single"
        elif rw >= 27 and rng.random() < 0.45: mode = "twin"
        elif rw >= 21 and rng.random() < 0.5: mode = "wing"
        if mode == "twin":
            ts = fit(min((rw - 9) / 2, rd, 18)); gap = rw - 2 * ts
            for k in range(2):
                x = rx0 + k * (ts + gap)
                boxes.append(dict(x0=x, x1=x + ts, z0=rz0, z1=rz0 + fit(min(rd, ts)), y0=y0, floors=F if k == 0 else max(8, int(F * rng.uniform(0.65, 0.9))), top=True))
        elif mode == "wing":
            ts = fit(min(rw * 0.5, rd, 18)); left = rng.random() < 0.5
            tx = rx0 if left else rx1 - ts
            boxes.append(dict(x0=tx, x1=tx + ts, z0=rz0, z1=rz0 + fit(min(rd, ts)), y0=y0, floors=F, top=True))
            ww = fit(min(rw - ts, 15)); wx = tx + ts if left else tx - ww
            boxes.append(dict(x0=wx, x1=wx + ww, z0=rz0, z1=rz0 + 9.0, y0=y0, floors=max(4, int(F * 0.3)), top=True, skip=(3,) if left else (2,)))
        else:
            ts = fit(min(rw * 0.75, rd, 27 if big else float(rng.choice([12, 15, 18, 21]))))
            b = place(ts, fit(min(rd, ts))); b.update(y0=y0, floors=F, top=True); boxes.append(b)
    return boxes


# ---------------------------------------------------------------- textures for the deck
def deck_tex(rng):
    S = 512; kind = rng.choice(["timber", "pavers", "composite"])
    if kind == "timber":
        img = np.zeros((S, S, 3), np.float32); pw = S // 14
        for k in range(14):
            c = np.array((0.55, 0.38, 0.24)) * rng.uniform(0.75, 1.15)
            grain = 1 + 0.06 * np.sin(np.arange(S) / rng.uniform(3, 9))[None, :, None]
            img[k * pw:(k + 1) * pw] = c * grain
            img[k * pw:k * pw + 2] *= 0.35
            cut = rng.integers(S); img[k * pw:(k + 1) * pw, cut:cut + 2] *= 0.4
    else:
        base = np.array((0.78, 0.76, 0.72) if kind == "pavers" else (0.45, 0.45, 0.46))
        img = np.ones((S, S, 3), np.float32) * base; n = 4 if kind == "pavers" else 8; t = S // n
        for i in range(n):
            for j in range(n): img[i * t:(i + 1) * t, j * t:(j + 1) * t] *= rng.uniform(0.88, 1.08)
        for i in range(n): img[i * t:i * t + 3] *= 0.6; img[:, i * t:i * t + 3] *= 0.6
    img *= (0.95 + 0.05 * rng.random((S, S, 1)))
    a = np.clip(img * 255, 0, 255).astype(np.uint8)
    h = (a.astype(np.float32) @ np.array([0.3, 0.59, 0.11]) / 255.0)
    return jpg(a, 88), jpg(normal_from_height(h, 3.0), 90)


def pool_tex(rng):
    S = 512; t = 16
    base = np.array([(0.35, 0.72, 0.82), (0.25, 0.6, 0.78), (0.4, 0.75, 0.75), (0.2, 0.35, 0.45)][rng.integers(4)], np.float32)
    img = np.ones((S, S, 3), np.float32) * base
    for i in range(S // t):
        for j in range(S // t): img[i * t:(i + 1) * t, j * t:(j + 1) * t] *= rng.uniform(0.88, 1.1)
    for i in range(S // t): img[i * t:i * t + 1] *= 0.8; img[:, i * t:i * t + 1] *= 0.8
    yy, xx = np.mgrid[0:S, 0:S] / S * 6.28                                       # caustics
    c = np.abs(np.sin(xx * 3 + np.sin(yy * 2) * 2) * np.sin(yy * 3 + np.sin(xx * 2.3) * 2))
    img *= (1 + 0.25 * (c ** 4))[..., None]
    a = np.clip(img * 255, 0, 255).astype(np.uint8)
    return jpg(a, 88), FLAT_N


PROP_CELLS = {"white": 0, "cushion": 1, "wood": 2, "metal": 3, "leaf": 4, "planter": 5, "umbA": 6, "umbB": 7, "coping": 8}
def props_tex(rng):
    S = 256; c = 64; img = np.zeros((S, S, 3), np.float32)
    umb = st.ACCENTS[rng.integers(len(st.ACCENTS))]
    cols = {"white": (0.93, 0.93, 0.9), "cushion": st.ACCENTS[rng.integers(len(st.ACCENTS))], "wood": (0.5, 0.35, 0.22), "metal": (0.2, 0.2, 0.22),
            "leaf": (0.18, 0.36, 0.14), "planter": (0.35, 0.35, 0.37), "umbA": umb, "umbB": (0.95, 0.94, 0.9) if rng.random() < 0.6 else umb, "coping": (0.88, 0.86, 0.8)}
    for k, i in PROP_CELLS.items():
        y, x = (i // 4) * c, (i % 4) * c; img[y:y + c, x:x + c] = cols[k]
        if k == "leaf": img[y:y + c, x:x + c] *= (0.6 + 0.8 * rng.random((c, c, 1)))
    return jpg(np.clip(img * 255, 0, 255).astype(np.uint8), 90), FLAT_N
def cell(name): i = PROP_CELLS[name]; return ((i % 4 + 0.5) / 4, (i // 4 + 0.5) / 4)


def amenity_deck(m, rng, xl, xr, zb, zf, towers):
    """The podium roof: paving around a pool (or a hot tub), coping, loungers, umbrellas, planters and the water."""
    y = PODIUM
    t_front = max(b["z1"] for b in towers) + 1.5
    dz0, dz1 = t_front, zf - 0.8
    dx0, dx1 = xl + 0.8, xr - 0.8
    depth = dz1 - dz0
    pool = None
    if depth >= 5.5:
        pw = min(5.0, depth - 3.0); pl = min(16.0, (dx1 - dx0) - 3.0)
        px0 = (dx0 + dx1) / 2 - pl / 2; pz1 = dz1 - 0.6; pool = (px0, px0 + pl, pz1 - pw, pz1, 1.3)
    elif depth >= 2.8:
        s = min(2.6, depth - 0.4); px0 = dx1 - s - 0.4; pool = (px0, px0 + s, dz1 - s - 0.2, dz1 - 0.2, 0.9)
    # paving: the roof minus the pool
    def pave(a0, a1, b0, b1):
        if a1 - a0 > 0.01 and b1 - b0 > 0.01:
            quad(m, "deck", (a0, y, b1), (a1, y, b1), (a1, y, b0), (a0, y, b0), [(a0 / 4, b1 / 4), (a1 / 4, b1 / 4), (a1 / 4, b0 / 4), (a0 / 4, b0 / 4)])
    if pool is None: pave(xl, xr, zb, zf); return
    px0, px1, pz0, pz1, pd = pool
    pave(xl, xr, zb, pz0); pave(xl, xr, pz1, zf); pave(xl, px0, pz0, pz1); pave(px1, xr, pz0, pz1)
    yb = y - pd
    for a, b, c_, d_, ax in (((px0, yb, pz0), (px1, yb, pz0), (px1, y, pz0), (px0, y, pz0), 0), ((px1, yb, pz1), (px0, yb, pz1), (px0, y, pz1), (px1, y, pz1), 0),
                             ((px1, yb, pz0), (px1, yb, pz1), (px1, y, pz1), (px1, y, pz0), 2), ((px0, yb, pz1), (px0, yb, pz0), (px0, y, pz0), (px0, y, pz1), 2)):
        quad(m, "pool", a, b, c_, d_, [(q[ax] / 2, -q[1] / 2) for q in (a, b, c_, d_)])
    quad(m, "pool", (px0, yb, pz0), (px1, yb, pz0), (px1, yb, pz1), (px0, yb, pz1), [(px0 / 2, pz0 / 2), (px1 / 2, pz0 / 2), (px1 / 2, pz1 / 2), (px0 / 2, pz1 / 2)])
    yw = y - 0.12
    quad(m, "glass", (px0, yw, pz1), (px1, yw, pz1), (px1, yw, pz0), (px0, yw, pz0), [(0, 0), (1, 0), (1, 1), (0, 1)])
    cw = 0.3; yc = y + 0.04; cp = cell("coping")                         # coping
    for a0, a1, b0, b1 in ((px0 - cw, px1 + cw, pz1, pz1 + cw), (px0 - cw, px1 + cw, pz0 - cw, pz0), (px0 - cw, px0, pz0, pz1), (px1, px1 + cw, pz0, pz1)):
        quad(m, "props", (a0, yc, b1), (a1, yc, b1), (a1, yc, b0), (a0, yc, b0), [cp] * 4)
    # loungers in a row behind the pool, umbrellas between them
    if pd > 1.0:
        n = int(min(7, (px1 - px0) / 1.5)); lz1 = pz0 - 0.7; lz0 = lz1 - 1.9
        if lz0 > t_front - 0.2:
            for k in range(n):
                lx = px0 + 0.4 + k * (px1 - px0 - 0.8) / max(1, n - 1) - 0.35 if n > 1 else (px0 + px1) / 2 - 0.35
                if k % 3 == 2:                                             # an umbrella in every third spot
                    ux = lx + 0.35; uz = (lz0 + lz1) / 2
                    cbox(m, "props", ux - 0.03, ux + 0.03, y, y + 2.2, uz - 0.03, uz + 0.03, top=False, uv=cell("metal"), sides=(0, 2))
                    r = 1.3; tip = (ux, y + 2.65, uz)
                    rim = [(ux - r, y + 2.05, uz - r), (ux + r, y + 2.05, uz - r), (ux + r, y + 2.05, uz + r), (ux - r, y + 2.05, uz + r)]
                    m.prims.setdefault("props", [])
                    for i in range(4):
                        cA = cell("umbA" if i % 2 == 0 else "umbB")
                        m.tri("props", rim[(i + 1) % 4], rim[i], tip, [cA] * 3)       # top
                        m.tri("props", rim[i], rim[(i + 1) % 4], tip, [cA] * 3)       # underside
                    continue
                cbox(m, "props", lx, lx + 0.7, y, y + 0.32, lz0 + 0.5, lz1, uv=cell("cushion"), sides=(0, 2, 3))
                quad(m, "props", (lx, y + 0.32, lz0 + 0.5), (lx + 0.7, y + 0.32, lz0 + 0.5), (lx + 0.7, y + 0.95, lz0), (lx, y + 0.95, lz0), [cell("cushion")] * 4)
    # planters at the deck's corners
    for (qx, qz) in ((xl + 0.3, zf - 1.3), (xr - 1.3, zf - 1.3), (xl + 0.3, t_front - 0.2), (xr - 1.3, t_front - 0.2)):
        if qz < zb + 0.3: continue
        cbox(m, "props", qx, qx + 1.0, y, y + 0.55, qz, qz + 1.0, uv=cell("planter"), top=False, sides=(0, 2, 3, 1))
        tip = (qx + 0.5, y + 1.6, qz + 0.5); base = [(qx, y + 0.55, qz), (qx + 1.0, y + 0.55, qz), (qx + 1.0, y + 0.55, qz + 1.0), (qx, y + 0.55, qz + 1.0)]
        m.prims.setdefault("props", [])
        for i in range(4): m.tri("props", base[(i + 1) % 4], base[i], tip, [cell("leaf")] * 3)


# ---------------------------------------------------------------- the podium
def podium(m, pd, xl, xr, zb, zf, ecx, gx_side, gcz, tex, seed):
    H = PODIUM
    for fi, (o, T, N, w) in enumerate(faces_of(xl, xr, zb, zf)):
        rg.pane(m, "roomglass_lobby", (o[0], 0.0, o[1]), T, (0, 1, 0), w, H, room=(H * 0.75, H), seed=float(seed % 991) + fi)
        face_plane(m, "glass", o, T, N, w, H, H + 1.04, 0.03, lambda u, y: (u / 3, -y / 3))      # the deck's glass balustrade
        kind = "front" if fi == 0 else ("back" if fi == 1 else "side")
        for k, (r0, rn) in enumerate(runs(int(round(w)), 15)):
            rw = float(rn) if k < len(runs(int(round(w)), 15)) - 1 else w - r0
            T_ = np.array(T, np.float32)
            org = np.array([o[0], 0.0, o[1]], np.float32) + T_ * r0
            along = lambda p: float(np.dot(np.array([p[0], 0, p[1]], np.float32) - org, T_))
            ent = along((ecx, zf)) if fi == 0 else None
            gar = along((xr if gx_side > 0 else xl, gcz)) if ((fi == 2 and gx_side > 0) or (fi == 3 and gx_side < 0)) else None
            if ent is not None and not (0 <= ent <= rw): ent = None
            if gar is not None and not (0 <= gar <= rw): gar = None
            mat = f"spom_pod_{fi}_{k}"
            if tex is not None: tex[mat] = st.podium_strip(pd, rw, H, kind, seed + fi * 31 + k, entrance=ent, garage=gar)
            spom_volume(m, mat, org, N, rw, H + 1.2)
            for (a0, a1, b0, b1, z) in st.podium_signs(rw, H, kind, entrance=ent, garage=gar):   # blank signs, 0..1 uv
                Nv = np.array(N, np.float32)
                p0 = org + T_ * a0 + Nv * z; p1 = org + T_ * a1 + Nv * z
                quad(m, "signboard", p0 + [0, b0, 0], p1 + [0, b0, 0], p1 + [0, b1, 0], p0 + [0, b1, 0], [(0, 1), (1, 1), (1, 0), (0, 0)])


# ---------------------------------------------------------------- one building
def build(W, D, level, ns, tex=None):
    m = Mesh()
    seed = level * 100000 + W * 1000 + D * 10
    rng = np.random.default_rng(seed)
    dz = st.design(level, rng); pd = st.podium_design(level, rng, dz)
    if ns > 4:                                                         # a tall building on its budget: special storeys grow rarer, then go
        for k in ("mech_every", "garden_every", "frieze_every"): dz[k] = dz[k] * 2 if ns <= 8 else 99
    big = (W, D) == (5, 5)
    global PODIUM
    PODIUM = 11.0 if big else 8.0
    lw, ld = W * CELL, D * CELL
    side = 1 if (W + D) % 2 == 0 else -1
    xl, xr = -lw / 2 + 1.0, lw / 2 - 1.0
    if side > 0: xr -= ap.DRIVE_W + 0.8
    else: xl += ap.DRIVE_W + 0.8
    zf, zb = ld / 2 - 3.2, -ld / 2 + 1.5
    zone_d = zf - zb
    deck_d = float(np.clip(zone_d * 0.32, 0.0, 9.0))
    if zone_d - deck_d < 9 + 2.6: deck_d = max(0.0, zone_d - 9 - 2.6)
    rx0, rx1 = xl + 1.3, xr - 1.3
    rz0, rz1 = zb + 1.3, zf - deck_d - 1.3
    if rz1 - rz0 < 9: rz1 = rz0 + 9
    ecx = (xl + xr) / 2
    mrng = np.random.default_rng(seed + 1)
    towers = massing(level, W, D, mrng, rx0, rx1, rz0, rz1, PODIUM, big)
    designs = [dz] + [st.design(level, np.random.default_rng(seed + 50 + k)) for k in range(1, 1 + max(b.get("di", 0) for b in towers))]
    dressers = [Dresser(dd, seed + 13 * k, tex, tag=str(k) if k else "") for k, dd in enumerate(designs)]
    top = PODIUM
    drng = np.random.default_rng(seed + 2)
    for b in towers:                                                         # how high each shared wall is covered by its neighbour
        b["cover"] = {}
        for fi in b.get("skip", ()):
            px = b["x1"] if fi == 2 else b["x0"]
            tops = [t["y0"] + t["floors"] * 3.0 for t in towers if t is not b and abs((t["x0"] if fi == 2 else t["x1"]) - px) < 0.05
                    and t["z0"] <= b["z0"] + 0.1 and t["z1"] >= b["z1"] - 0.1 and t["y0"] <= b["y0"] + 0.1]   # only a neighbour covering the whole wall
            b["cover"][fi] = max(tops) if tops else b["y0"]
    for i, b in enumerate(towers):
        k = b.get("di", 0)
        dress(m, designs[k], dressers[k], b, ns, drng, variants=2 if ns <= 3 else 1, podium=PODIUM)
        if b["top"]:
            above = [t for t in towers[i + 1:] if t["y0"] >= b["y0"] + b["floors"] * 3.0 - 0.1 and t["x0"] >= b["x0"] - 0.1 and t["x1"] <= b["x1"] + 0.1]
            if above: terrace_planters(m, above[0], b)
            else: top = max(top, crown(m, designs[k], b, drng, tag=dressers[k].tag, dresser=dressers[k]))
    dresser = dressers[0]
    gcz = zb + (zf - zb) * 0.45
    podium(m, pd, xl, xr, zb, zf, ecx, side, gcz, tex, seed)
    amenity_deck(m, np.random.default_rng(seed + 3), xl, xr, zb, zf, towers)
    class _H: pass
    h = _H(); h.m = m; h.level = level
    ap.add_ground(h, lw, ld, side, ecx, zf + 0.6)
    if tex is not None:
        tex["roomglass"] = rg.maps(seed, "office" if level == 3 else "home")
        tex["roomglass_lobby"] = (rg.room_atlas(seed + 3, "lobby", 0.75), rg.wear_mask(seed + 11))
        tex["glass"] = (png(np.full((4, 4, 3), 200, np.uint8)), FLAT_N)
        tex["signboard"] = (png(np.full((4, 4, 3), 235, np.uint8)), FLAT_N)
        tex["deck"] = deck_tex(np.random.default_rng(seed + 4)); tex["pool"] = pool_tex(np.random.default_rng(seed + 5))
        tex["props"] = props_tex(np.random.default_rng(seed + 6))
        cc = dz.get("paint", dz.get("slab", dz.get("metal", st.C((0.8, 0.8, 0.8)))))
        cl = st.cladding(dz["clad"], dz["clad_m"], dz["paint"], 4, 4, rng, dz["mode"], ppm=128) if dz["wall"] else np.ones((256, 256, 3), np.float32) * cc
        tex["clad"] = (jpg(np.clip(cl * 255, 0, 255).astype(np.uint8), 88), FLAT_N)
    if tex is not None:
        for k, dd in enumerate(designs[1:], 1):
            cl = st.cladding(dd["clad"], dd["clad_m"], dd["paint"], 4, 4, rng, dd["mode"], ppm=128) if dd["wall"] else np.ones((256, 256, 3), np.float32) * 0.8
            tex[f"clad{k}"] = (jpg(np.clip(cl * 255, 0, 255).astype(np.uint8), 88), FLAT_N)
        wood = np.ones((64, 64, 3), np.float32) * np.array((0.42, 0.3, 0.2)); wood[::6] *= 0.6
        tex["props_wood"] = (jpg(np.clip(wood * 255, 0, 255).astype(np.uint8), 90), FLAT_N)
    return m, dz, pd, towers, top, sum(len(x.made) for x in dressers)


def main():
    os.makedirs(OUT, exist_ok=True)
    args = sys.argv[1:]
    levels = [int(a) for a in args if a.isdigit()] or [1, 2, 3, 4, 5]
    plots = [tuple(map(int, a.split("x"))) for a in args if "x" in a] or PLOTS
    for lv in levels:
        common = {"roof": tile_maps(ROOFS[lv], 5.0), "drive": drive_tex(lv)}
        for (W, D) in plots:
            ns = 1
            while True:                                                    # the fewest storeys per volume that fit the budget
                m, *_ = build(W, D, lv, ns)
                if m.tri_count() <= BUDGET or ns >= 20: break
                ns += 1
            tex = dict(common)
            m, dz, pd, towers, top, nmat = build(W, D, lv, ns, tex)
            out = os.path.join(OUT, f"HD_{W}x{D}_L{lv}.glb")
            write_glb(out, m, tex, MATS)
            fl = "/".join(str(b["floors"]) for b in towers)
            if lv == 2: dz["_extra"] = ""
            print(f"HD_{W}x{D}_L{lv}: {m.tri_count():4d} tris, {os.path.getsize(out) / 1e6:.1f} MB, {top:.0f} m, floors {fl}, "
                  f"{ns} storeys/volume, {nmat} reliefs | {st.describe(dz)} | podium '{pd['name']}'", flush=True)


if __name__ == "__main__":
    main()
