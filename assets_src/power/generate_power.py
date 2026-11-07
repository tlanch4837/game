#!/usr/bin/env python3
"""Power-grid props, ultra low poly with the detail carried by SPOM volumes (see powerlib.py):

    pylon.glb               33 m double-circuit lattice transmission tower: tapered body, 3 crossarm levels, 6 insulator strings
    pole.glb                12 m creosoted wood distribution pole: crossarm, steel braces, 3 pin insulators
    pole_transformer.glb    the same pole carrying a pole-mount transformer can
    substation.glb          42 x 30 m fenced substation: line gantry, disconnects, breakers, CTs, 2 power transformers, control house

Y up, metres, origin at the ground under the prop's centre. Node extras carry the conductor attach points ("attach": [[x,y,z],...])
so the engine can string wires between props. Run: python generate_power.py
"""
import numpy as np

import powerlib as pl
from powerlib import Mesh, Relief, shed_profile

P = 0.17  # lattice bars' centre plane, metres out from a facade's wall (as the crane's BAR_PLANE)


def flat(c):
    return lambda X, Y: np.broadcast_to(np.asarray(c, np.float32), X.shape + (3,))


def bands(*spec):
    """Colour by height: spec = (y_top, rgb), ...; each band runs up to its y_top."""
    def f(X, Y):
        out = np.zeros(X.shape + (3,), np.float32)
        lo = -1e9
        for top, c in spec:
            out[(Y > lo) & (Y <= top)] = c
            lo = top
        out[Y > lo] = spec[-1][1]
        return out
    return f


# ==============================================================================================================================
# Insulators and other bodies of revolution (one SPOM box each, exact from every horizontal view)
# ==============================================================================================================================
def rev_texture(name, W, H, ppm, build):
    r = Relief(W, H, ppm)
    build(r)
    return r.save(name)


def tex_insulator_string():
    def b(r):
        cx, zc = 0.2, 0.2
        r.rev(cx, zc, shed_profile(0.30, 2.42, 0.022, 0.13, 0.146), flat(pl.GLASS_INS))   # 15 cap-and-pin glass discs
        r.rev(cx, zc, lambda Y: np.where((Y > 0.08) & (Y < 0.30), 0.035, 0.0), flat(pl.STEEL_HW))  # bottom ball-socket
        r.rect(cx - 0.09, 0.0, cx + 0.09, 0.10, zc - 0.03, zc + 0.03, pl.STEEL_HW, bevel=0.02)  # conductor clamp
        r.rev(cx, zc, lambda Y: np.where((Y > 2.42) & (Y < 2.62), 0.03, 0.0), flat(pl.STEEL_HW))
        r.rect(cx - 0.06, 2.58, cx + 0.06, 2.80, zc - 0.012, zc + 0.012, pl.GALV, bevel=0.01)    # yoke plate
        for s in (-1, 1):                                                                      # arcing horns
            r.bar((cx + s * 0.03, 0.12), (cx + s * 0.17, 0.30), 0.012, pl.STEEL_HW, z=zc, square=False)
            r.bar((cx + s * 0.17, 0.30), (cx + s * 0.16, 0.42), 0.012, pl.STEEL_HW, z=zc, square=False)
            r.bar((cx + s * 0.03, 2.55), (cx + s * 0.15, 2.45), 0.012, pl.STEEL_HW, z=zc, square=False)
    return rev_texture("ins_string", 0.4, 2.8, 300, b)


def tex_post_insulator():
    def b(r):
        cx, zc = 0.2, 0.2
        r.rev(cx, zc, lambda Y: np.where(Y < 0.08, 0.13, 0.0), flat(pl.GALV_DARK))
        r.rev(cx, zc, shed_profile(0.08, 1.58, 0.06, 0.13, 0.09, alt=0.105), flat(pl.PORCELAIN_GREY))
        r.rev(cx, zc, lambda Y: np.where((Y > 1.58) & (Y < 1.70), 0.10, 0.0), flat(pl.GALV))
    return rev_texture("post_ins", 0.4, 1.75, 260, b)


def tex_breaker():
    def b(r):
        cx, zc = 0.35, 0.35
        r.rev(cx, zc, lambda Y: np.where(Y < 0.40, 0.22, 0.0), flat(pl.GALV_DARK))            # mechanism housing
        r.rev(cx, zc, shed_profile(0.40, 2.55, 0.10, 0.19, 0.10, alt=0.16), flat(pl.PORCELAIN_GREY))
        r.rev(cx, zc, lambda Y: np.where((Y >= 2.55) & (Y < 3.05), 0.26 - 0.08 * np.abs(Y - 2.8) / 0.25, 0.0),
              flat((0.80, 0.81, 0.82)))                                                         # interrupter head
        for s in (-1, 1):                                                                      # line terminal pads
            r.rect(cx + s * 0.22, 2.74, cx + s * 0.34, 2.86, zc - 0.05, zc + 0.05, (0.8, 0.8, 0.8))
        r.rev(cx, zc, lambda Y: np.where((Y >= 3.05) & (Y < 3.18), 0.05, 0.0), flat(pl.GALV))
    return rev_texture("breaker", 0.7, 3.2, 200, b)


def tex_ct():
    def b(r):
        cx, zc = 0.4, 0.4
        r.rev(cx, zc, lambda Y: np.where(Y < 0.30, 0.25, 0.0), flat(pl.GALV_DARK))
        r.rev(cx, zc, shed_profile(0.30, 1.95, 0.09, 0.17, 0.095, alt=0.14), flat(pl.PORCELAIN_GREY))
        dome = lambda Y: np.where((Y >= 1.95) & (Y < 2.70), 0.32 * np.sqrt(np.clip(1 - ((Y - 2.22) / 0.48) ** 2, 0, 1)) *
                                  np.where(Y < 2.22, 1.0, 1.0), 0.0)
        r.rev(cx, zc, dome, flat((0.55, 0.58, 0.57)))
        for s in (-1, 1):
            r.rect(cx + s * 0.26, 2.15, cx + s * 0.38, 2.27, zc - 0.05, zc + 0.05, (0.8, 0.8, 0.8))
    return rev_texture("ct", 0.8, 2.8, 200, b)


def tex_bushing(name, H, core, shed, pitch):
    def b(r):
        cx, zc = 0.3, 0.3
        r.rev(cx, zc, lambda Y: np.where(Y < 0.18, core * 2.6, 0.0), flat(pl.GALV_DARK))      # turret flange
        r.rev(cx, zc, shed_profile(0.18, H - 0.22, core, shed, pitch, alt=shed * 0.8), flat(pl.PORCELAIN))
        r.rev(cx, zc, lambda Y: np.where((Y >= H - 0.22) & (Y < H - 0.06), core * 1.3, 0.0), flat(pl.GALV))
        r.rev(cx, zc, lambda Y: np.where(Y >= H - 0.06, 0.03, 0.0), flat((0.8, 0.8, 0.8)))      # terminal stud
    return rev_texture(name, 0.6, H, 220, b)


# ==============================================================================================================================
# Lattice faces
# ==============================================================================================================================
def lattice_bay(name, h0, h1, H, ppm, leg_r=0.085, br_r=0.04, panels=1, top_horizontal=True):
    """One face of one bay of a square lattice tower: legs from half-width h0 (bottom) to h1 (top), leaning in depth too.
    The wall sits at h1 - P: the bar plane is P out at the top and P + (h0 - h1) at the bottom."""
    m = 0.12
    W = 2 * h0 + 2 * m
    cx = W / 2
    r = Relief(W, H, ppm, plane=(P + (h0 - h1), P))
    hx = lambda y: h0 + (h1 - h0) * y / H
    # legs (extended past the bay so joints never gap; the box clips them)
    for s in (-1, 1):
        r.bar((cx + s * hx(-0.3), -0.3), (cx + s * hx(H + 0.3), H + 0.3), leg_r, pl.GALV)
    # horizontals at the bay ends (each bay paints its half of the shared one)
    ends = [0.0, H] if top_horizontal else [0.0]
    for y in ends:
        r.bar((cx - hx(y), y), (cx + hx(y), y), br_r * 1.1, pl.GALV)
    # X bracing per panel, with redundant members to the crossing
    for k in range(panels):
        y0, y1 = H * k / panels, H * (k + 1) / panels
        a, b_, c, d = (cx - hx(y0), y0), (cx + hx(y1), y1), (cx + hx(y0), y0), (cx - hx(y1), y1)
        r.bar(a, b_, br_r, pl.GALV)
        r.bar(c, d, br_r, pl.GALV)
        ym = (y0 + y1) / 2
        r.rect(cx - 0.07, ym - 0.07, cx + 0.07, ym + 0.07, r.zp(np.array(ym)) - 0.015, r.zp(np.array(ym)) + 0.015,
               pl.GALV_DARK)                                                                    # crossing gusset + bolt
        for s in (-1, 1):                                                                      # redundants leg -> crossing
            r.bar((cx + s * hx(ym), ym), (cx + s * 0.08, ym), br_r * 0.6, pl.GALV)
        if k:
            r.bar((cx - hx(y0), y0), (cx + hx(y0), y0), br_r * 0.8, pl.GALV)
    # gusset plates where bracing meets the legs
    for y in (0.0, H) if top_horizontal else (0.0,):
        for s in (-1, 1):
            x = cx + s * (hx(y) - 0.12)
            r.rect(x - 0.13, y - 0.13, x + 0.13, y + 0.13, r.zp(np.array(y)) - 0.012, r.zp(np.array(y)) + 0.012, pl.GALV_DARK)
    depth = r.save(name)
    return W, depth


def lattice_face(name, h0, h1, H, bays, ppm, zq=0.12, leg_r=0.085, br_r=0.04):
    """A whole tower face as ONE relief (for a SPOM quad): the face plane leans, so the texture is laid out along the slant.
    bays = list of (y0, y1, panels) in vertical metres. Returns (W, slant length, cx)."""
    Ls = float(np.hypot(H, h0 - h1))
    m = 0.14
    W = 2 * h0 + 2 * m
    cx = W / 2
    k = Ls / H                                    # vertical metres -> slant metres
    r = Relief(W, Ls, ppm, plane=(zq, zq))
    hx = lambda y: h0 + (h1 - h0) * y / H
    pt = lambda s, y: (cx + s * hx(y), y * k)
    for s in (-1, 1):
        r.bar(pt(s, -0.3), pt(s, H + 0.3), leg_r, pl.GALV)
    for y0, y1, panels in bays:
        r.bar(pt(-1, y0), pt(1, y0), br_r * 1.1, pl.GALV)
        for j in range(panels):
            a, b = y0 + (y1 - y0) * j / panels, y0 + (y1 - y0) * (j + 1) / panels
            r.bar(pt(-1, a), pt(1, b), br_r, pl.GALV)
            r.bar(pt(1, a), pt(-1, b), br_r, pl.GALV)
            ym = (a + b) / 2
            r.rect(cx - 0.07, ym * k - 0.07, cx + 0.07, ym * k + 0.07, zq - 0.015, zq + 0.015, pl.GALV_DARK)
            for s in (-1, 1):
                r.bar(pt(s, ym), (cx + s * 0.08, ym * k), br_r * 0.6, pl.GALV)
            if j:
                r.bar(pt(-1, a), pt(1, a), br_r * 0.8, pl.GALV)
        for y in (y0, y1):
            for s in (-1, 1):
                x = cx + s * (hx(y) - 0.12)
                r.rect(x - 0.13, y * k - 0.13, x + 0.13, y * k + 0.13, zq - 0.012, zq + 0.012, pl.GALV_DARK)
    r.bar(pt(-1, H), pt(1, H), br_r * 1.1, pl.GALV)
    r.save(name)
    return W, Ls, cx


def put_face_quads(m, key, y_base, h0, h1, H, W, Ls, cx, zq=0.12, pad=0.14, at=(0.0, 0.0)):
    """Four SPOM quads (one per side, 2 triangles each) whose outline hugs the legs."""
    lean = (h0 - h1) / H
    for nh in ((0, 0, 1), (1, 0, 0), (0, 0, -1), (-1, 0, 0)):
        nh = np.array(nh, float)
        N = nh + np.array([0, lean, 0]); N /= np.linalg.norm(N)
        T = np.cross([0, 1, 0], N); T /= np.linalg.norm(T)
        C0 = nh * h0 + np.array([at[0], y_base, at[1]])        # bottom centre of the face, on the bar plane
        origin = C0 - T * cx - N * zq
        corners = [(cx - h0 - pad, -pad), (cx + h0 + pad, -pad), (cx + h1 + pad, Ls + pad), (cx - h1 - pad, Ls + pad)]
        m.spom_quad(key, origin, N, W, Ls, zq, corners)


def warren_arm(name, L, H_root, H_tip, ppm, mirror_too=True):
    """Side truss of a crossarm, root at x = 0 (tower), tip at x = L: horizontal top chord, sloping bottom chord."""
    W, H = L, H_root + 0.15
    r = Relief(W, H, ppm, plane=(P, P))
    top = H - 0.08
    bot = lambda x: top - (H_root + (H_tip - H_root) * x / L) + 0.08
    r.bar((-0.3, top), (L - 0.05, top), 0.06, pl.GALV)
    r.bar((-0.3, bot(-0.3)), (L - 0.05, bot(L - 0.05)), 0.06, pl.GALV)
    n = max(2, int(round(L / 0.9)))
    xs = [L * i / n * 0.97 for i in range(n + 1)]
    for i, x in enumerate(xs):
        r.bar((x, bot(x)), (x, top), 0.032, pl.GALV)
        if i < n:
            x2 = xs[i + 1]
            if i % 2 == 0:
                r.bar((x, bot(x)), (x2, top), 0.032, pl.GALV)
            else:
                r.bar((x, top), (x2, bot(x2)), 0.032, pl.GALV)
    r.rect(L - 0.30, bot(L) - 0.06, L - 0.02, top + 0.06, P - 0.015, P + 0.015, pl.GALV_DARK)   # tip plate
    d = r.save(name)
    if mirror_too:
        r.save(name + "_m", mirror=True)
    pad = 0.1                                       # outline hugging the truss, for a SPOM quad (root at u = 0)
    outline = [(0.0, max(bot(0.0) - pad, 0.0)), (W, max(bot(W) - pad, 0.0)), (W, H), (0.0, H)]
    return d, outline


def tex_fence():
    W, H = 6.0, 2.75
    r = Relief(W, H, 200, plane=(0.08, 0.08))
    zc = 0.08
    for px in (0.06, 3.06):
        r.bar((px, -0.2), (px, 2.30), 0.045, pl.GALV, square=False)                            # line posts
    r.bar((-0.2, 2.24), (W + 0.2, 2.24), 0.022, pl.GALV, square=False)                         # top rail
    r.bar((-0.2, 0.06), (W + 0.2, 0.06), 0.006, pl.GALV_DARK, square=False)                    # bottom tension wire
    # chain-link diamonds, 60 mm mesh, between the rails
    s, wr = 0.06, 0.0045
    X, Y = r.X, r.Y
    d1 = np.abs(((X + Y) / s + 0.5) % 1.0 - 0.5) * s / np.sqrt(2)
    d2 = np.abs(((X - Y) / s + 0.5) % 1.0 - 0.5) * s / np.sqrt(2)
    d = np.minimum(d1, d2) / wr
    on = (d < 1) & (Y > 0.04) & (Y < 2.24)
    prof = np.sqrt(np.clip(1 - d * d, 0, 1))
    r._add(on, zc - wr * prof, zc + wr * prof, pl.GALV_DARK, prof)
    # barbed-wire outrigger leaning outward (+N), three strands with barbs
    for px in (0.06, 3.06):
        r.bar((px, 2.26), (px, 2.70), 0.02, pl.GALV, z=(0.08, 0.36))
    for k in range(3):
        y = 2.36 + 0.14 * k
        z = 0.08 + (y - 2.26) / 0.44 * 0.28
        r.bar((-0.2, y), (W + 0.2, y), 0.004, pl.STEEL_HW, z=z, square=False)
        for x in np.arange(0.1, W, 0.13):
            r.bar((x - 0.015, y - 0.015), (x + 0.015, y + 0.015), 0.003, pl.STEEL_HW, z=z, square=False)
            r.bar((x - 0.015, y + 0.015), (x + 0.015, y - 0.015), 0.003, pl.STEEL_HW, z=z, square=False)
    return r.save("q_fence")


def tex_gate():
    W, H = 6.0, 2.75
    r = Relief(W, H, 160, plane=(0.08, 0.08))
    for x in (0.08, W - 0.08):
        r.bar((x, -0.2), (x, 2.5), 0.07, pl.GALV, square=False)                                 # gate posts
    for x0, x1 in ((0.2, 2.95), (3.05, W - 0.2)):
        for y in (0.15, 1.15, 2.15):
            r.bar((x0, y), (x1, y), 0.03, pl.GALV, square=False)
        for x in (x0, x1):
            r.bar((x, 0.15), (x, 2.15), 0.03, pl.GALV, square=False)
        for x in np.arange(x0 + 0.12, x1, 0.12):
            r.bar((x, 0.15), (x, 2.30), 0.012, pl.GALV, square=False)                           # vertical pickets
        r.bar((x0, 0.15), (x1, 2.15), 0.02, pl.GALV, square=False)                               # diagonal brace
    # danger sign: yellow plate with a black border and bolt
    r.rect(2.45, 1.30, 3.55, 1.95, 0.095, 0.11, (0.10, 0.10, 0.10))
    r.rect(2.50, 1.35, 3.50, 1.90, 0.10, 0.115, (0.90, 0.72, 0.08))
    zz = lambda x0, y0, x1, y1: r.bar((x0, y0), (x1, y1), 0.025, (0.08, 0.08, 0.08), z=0.115)  # lightning flash
    zz(3.05, 1.82, 2.90, 1.62); zz(2.90, 1.62, 3.08, 1.62); zz(3.08, 1.62, 2.93, 1.42)
    return r.save("q_gate")


def tex_radiator():
    """Transformer radiator bank: pressed-steel fins standing out of the tank, with top and bottom headers."""
    W, H = 3.0, 2.3
    r = Relief(W, H, 220, plane=(0.45, 0.45))
    fin = (0.42, 0.45, 0.44)
    for x in np.arange(0.12, W - 0.08, 0.11):
        r.rect(x - 0.008, 0.25, x + 0.008, H - 0.25, 0.08, 0.82, fin, bevel=0.03)
    for y in (0.17, H - 0.17):
        r.bar((0.02, y), (W - 0.02, y), 0.07, (0.46, 0.49, 0.48), z=0.45, square=False)        # header pipe
    for x in (0.25, W - 0.25):                                                                  # flanged elbows to the tank
        r.rect(x - 0.09, 0.0, x + 0.09, 0.30, 0.0, 0.40, (0.40, 0.43, 0.42), bevel=0.03)
        r.rect(x - 0.09, H - 0.30, x + 0.09, H, 0.0, 0.40, (0.40, 0.43, 0.42), bevel=0.03)
    return r.save("radiator")


# ==============================================================================================================================
# Pole (wood distribution pole)
# ==============================================================================================================================
POLE_ZC = 0.2
pole_r = lambda Y: 0.15 - 0.03 * np.clip(Y / 10.5, 0, 1)


def wood_col(X, Y, base=pl.WOOD):
    rng = np.random.default_rng(3)
    streak = rng.random(4096).astype(np.float32)
    k = (X * 160).astype(int) % 4096
    g = 0.88 + 0.24 * streak[k]
    return np.asarray(base, np.float32)[None, None, :] * g[..., None]


def tex_pole_shaft(name, y0, H, tag=False):
    r = Relief(0.4, H, 260)
    cx, zc = 0.2, POLE_ZC
    r.rev(cx, zc, lambda Y: np.full_like(Y, 1.0) * pole_r(Y + y0), wood_col)
    # ground wire stapled down the front face
    za, zb = zc + pole_r(np.array(y0)) + 0.006, zc + pole_r(np.array(y0 + H)) + 0.006
    r.bar((cx + 0.03, -0.1), (cx + 0.03, H + 0.1), 0.006, (0.55, 0.38, 0.22), z=(float(za), float(zb)), square=False)
    for y in np.arange(0.3, H, 0.6):
        r.rect(cx + 0.015, y, cx + 0.045, y + 0.012, float(zb) - 0.004, float(za) + 0.006, pl.STEEL_HW)
    if tag:  # pole number tag + climbing step bolts
        r.rect(cx - 0.05, 1.7, cx + 0.05, 1.85, zc + 0.145, zc + 0.152, (0.85, 0.80, 0.30))
        for y in np.arange(2.4, H, 0.45):
            s = 1 if int(y / 0.45) % 2 else -1
            r.bar((cx + s * 0.12, y), (cx + s * 0.19, y), 0.01, pl.STEEL_HW, z=zc, square=False)
    return r.save(name)


def tex_pole_head(y0, H):
    W = 2.6
    cx, zc = W / 2, POLE_ZC
    r = Relief(W, H, 300)
    top = 10.5 - y0
    r.rev(cx, zc, lambda Y: np.where(Y < top, pole_r(Y + y0), 0.0), wood_col)
    r.rev(cx, zc, lambda Y: np.where((Y >= top) & (Y < top + 0.03), 0.10, 0.0), flat(pl.STEEL_HW))  # pole cap
    ya = 9.95 - y0
    zf = zc + 0.12
    r.rect(0.10, ya, W - 0.10, ya + 0.12, zf - 0.02, zf + 0.085, pl.WOOD_ARM, bevel=0.012)        # crossarm
    za = zf + 0.033
    for s in (-1, 1):                                                                           # flat steel braces
        r.bar((cx + s * 0.80, ya + 0.02), (cx + s * 0.06, ya - 0.62), 0.018, pl.GALV, z=zf + 0.1)
        r.rect(cx + s * 0.80 - 0.02, ya + 0.04, cx + s * 0.80 + 0.02, ya + 0.08, zf + 0.085, zf + 0.10, pl.STEEL_HW)
    r.rect(cx - 0.025, ya - 0.65, cx + 0.025, ya - 0.59, zc + 0.10, zf + 0.12, pl.STEEL_HW)        # brace bolt
    r.rect(cx - 0.03, ya + 0.03, cx + 0.03, ya + 0.09, zf + 0.08, zf + 0.10, pl.STEEL_HW)        # crossarm through-bolt
    pin = lambda b: (lambda Y: np.where((Y >= b) & (Y < b + 0.07), 0.016,
                                        np.where((Y >= b + 0.07) & (Y < b + 0.27),
                                                 0.05 + 0.03 * np.clip(1 - np.abs(((Y - b - 0.07) / 0.065) % 1 - 0.3) * 3, 0, 1),
                                                 np.where((Y >= b + 0.27) & (Y < b + 0.30), 0.035, 0.0))))
    for s in (-1, 1):
        r.rev(cx + s * 1.05, za, pin(ya + 0.12), bands((ya + 0.19, pl.STEEL_HW), (9e9, pl.PORCELAIN)))
    r.rev(cx, zc, pin(top + 0.03), bands((top + 0.10, pl.STEEL_HW), (9e9, pl.PORCELAIN)))
    r.save("pole_head")
    return W, [(-1.05, 9.95 + 0.12 + 0.27, za - zc), (0.0, 10.5 + 0.03 + 0.27, 0.0), (1.05, 9.95 + 0.12 + 0.27, za - zc)]


def tex_pole_can(y0, H):
    """Pole-mount transformer behind the pole: can, lid, two HV bushings, mounting bracket out to the pole."""
    W, D = 0.9, 0.72
    cx, zc = W / 2, 0.36
    r = Relief(W, H, 260)
    b0, b1 = 7.75 - y0, 8.65 - y0
    can = lambda Y: np.where((Y >= b0) & (Y < b1), 0.30, np.where((Y >= b0 - 0.04) & (Y < b0), 0.27, 0.0))
    r.rev(cx, zc, can, flat((0.55, 0.58, 0.57)))
    r.rev(cx, zc, lambda Y: np.where((Y >= b1) & (Y < b1 + 0.07), 0.31 * np.sqrt(np.clip(1 - ((Y - b1) / 0.07) ** 2, 0.2, 1)), 0.0),
          flat((0.50, 0.53, 0.52)))
    for y in (b0 + 0.06, b1 - 0.06):                                                           # rolled seams
        r.rev(cx, zc, lambda Y, y=y: np.where(np.abs(Y - y) < 0.015, 0.315, 0.0), flat((0.50, 0.53, 0.52)))
    for s in (-1, 1):
        r.rev(cx + s * 0.13, zc, shed_profile(b1 + 0.05, b1 + 0.28, 0.022, 0.05, 0.06), flat(pl.PORCELAIN))
    r.rect(cx - 0.08, b0 + 0.15, cx + 0.08, b0 + 0.30, zc + 0.25, D, pl.GALV)                   # lower hanger bracket
    r.rect(cx - 0.08, b1 - 0.30, cx + 0.08, b1 - 0.15, zc + 0.25, D, pl.GALV)                   # upper hanger bracket
    r.rect(cx - 0.12, b0 + 0.35, cx + 0.12, b0 + 0.55, zc + 0.29, zc + 0.31, (0.85, 0.85, 0.82))  # rating plate
    r.save("pole_can")
    return W, D, [(s * 0.13, b1 + 0.28 + y0) for s in (-1, 1)]


def build_pole(with_can):
    m = Mesh("pole_transformer" if with_can else "pole")
    wall_z = -POLE_ZC
    for name, y0, y1 in (("pole_shaft_a", 0.0, 3.4), ("pole_shaft_b", 3.4, 6.8), ("pole_shaft_c", 6.8, 9.4)):
        m.spom_box(name, (0, (y0 + y1) / 2, wall_z), (0, 0, 1), 0.4, y1 - y0, 0.42)
    m.spom_box("pole_head", (0, (9.4 + 10.9) / 2, wall_z), (0, 0, 1), HEAD_W, 1.5, 0.5)
    m.box("concrete", (0, 0.04, 0), (0.6, 0.08, 0.6))                                          # backfill collar
    attach = [list(a) for a in HEAD_ATTACH]
    if with_can:
        m.spom_box("pole_can", (0, (7.5 + 9.1) / 2, -0.13 - CAN_D), (0, 0, 1), CAN_W, 1.6, CAN_D)
        bz = -0.13 - CAN_D + 0.36
        for (bx, by), (ax, ay, az) in zip(CAN_BUSH, (HEAD_ATTACH[0], HEAD_ATTACH[2])):          # drop leads to the outer phases
            m.cyl("wire", (bx, by, bz), (ax * 0.95, ay - 0.05, az), 0.008, seg=4)
        m.cyl("wire", (0.0, 7.7, bz + 0.3), (0.0, 0.2, 0.17), 0.006, seg=4)                     # tank ground lead
    m.extras = {"attach": attach, "kind": "distribution_pole", "height": 10.8}
    return m


# ==============================================================================================================================
# Pylon (lattice transmission tower)
# ==============================================================================================================================
TAPER = [(0.0, 5.0, 3.5, 2.8), (5.0, 10.0, 2.8, 2.1), (10.0, 14.5, 2.1, 1.5), (14.5, 18.0, 1.5, 1.0)]
TOP_BAYS = [(18.0, 22.0), (22.0, 26.0), (26.0, 30.0)]
ARMS = [(19.6, 4.6), (23.6, 6.2), (27.6, 4.6)]  # (top-chord height, length from the tower face)
PEAK = (30.0, 33.0, 1.0, 0.25)
ARM_HALF = 0.85


def build_pylon():
    m = Mesh("pylon")
    faces = [((0, 0, 1)), ((0, 0, -1)), ((1, 0, 0)), ((-1, 0, 0))]

    def put_bay(key, y0, y1, h0, h1, W, D):
        for N in faces:
            Nv = np.array(N, float)
            m.spom_box(key, tuple(Nv * (h1 - P) + np.array([0, (y0 + y1) / 2, 0])), N, W, y1 - y0, D)

    # body: the whole tapered 18 m as ONE quad per side; the straight top 12 m likewise
    bays = [(y0, y1, 2 if i < 2 else 1) for i, (y0, y1, _, _) in enumerate(TAPER)]
    W, Ls, cx = lattice_face("q_body", 3.5, 1.0, 18.0, bays, 110)
    put_face_quads(m, "q_body", 0.0, 3.5, 1.0, 18.0, W, Ls, cx)
    W, Ls, cx = lattice_face("q_top", 1.0, 1.0, 12.0, [(y0 - 18.0, y1 - 18.0, 1) for y0, y1 in TOP_BAYS], 120)
    put_face_quads(m, "q_top", 18.0, 1.0, 1.0, 12.0, W, Ls, cx)
    y0, y1, h0, h1 = PEAK
    W, Ls, cx = lattice_face("q_peak", h0, h1, y1 - y0, [(0.0, y1 - y0, 1)], 140, leg_r=0.075)
    put_face_quads(m, "q_peak", y0, h0, h1, y1 - y0, W, Ls, cx)
    m.box("galv_steel", (0, y1 + 0.08, 0), (0.7, 0.16, 0.7))                                  # earth-wire peak cap
    m.box("galv_steel", (0, y1 + 0.35, 0), (0.12, 0.4, 0.12))

    attach = []
    for (ya, L) in ARMS:
        key = f"q_arm{int(L * 10)}"
        Hr = 1.7 if L > 5 else 1.4
        d, outline = warren_arm(key, L, Hr, 0.3, 130)
        Hbox = Hr + 0.15
        for sx in (-1, 1):
            xc = sx * (1.0 + L / 2 - 0.05)
            for sz in (-1, 1):
                N = (0, 0, sz)
                T = np.cross([0, 1, 0], N)  # +X for +z faces, -X for -z faces
                root_left = (T[0] * sx) > 0  # uv 0 sits at the root when T points out along the arm
                ol = outline if root_left else [(L - u, v) for u, v in outline][::-1]
                m.spom_quad_at(key if root_left else key + "_m", (xc, ya + 0.08 - Hbox / 2, sz * (ARM_HALF - P)), N, L, Hbox, P, ol)
            tip_x = sx * (1.0 + L - 0.25)
            m.box("galv_steel", (tip_x, ya - 0.3 + 0.02, 0), (0.3, 0.04, ARM_HALF * 2))         # tip plate between the trusses
            m.box("galv_steel", (sx * (1.0 + L / 2), ya + 0.02, 0), (L, 0.03, 0.08))            # walkway angle on top
            # suspension insulator string hanging from the tip
            ytop = ya - 0.3
            m.spom_box("ins_string", (tip_x, ytop - 1.4, -0.2), (0, 0, 1), 0.4, 2.8, 0.4)
            attach.append([round(tip_x, 3), round(ytop - 2.8 + 0.05, 3), 0.0])
    attach.append([0.0, 33.55, 0.0])  # earth wire
    for sx in (-1, 1):
        for sz in (-1, 1):
            m.box("concrete", (sx * 3.5, 0.2, sz * 3.5), (1.1, 0.4, 1.1))                     # foundation chimneys
            m.box("galv_steel", (sx * 3.5, 0.45, sz * 3.5), (0.45, 0.1, 0.45))                # stub-angle base plate
    m.box("galv_steel", (0, 3.0, 3.3), (0.5, 0.35, 0.02))                                     # number / danger plate
    m.extras = {"attach": attach, "kind": "transmission_pylon", "height": 33.6,
                "attach_note": "6 phase conductors (left low, mid, high; right low, mid, high order by x, y), last = earth wire"}
    return m


# ==============================================================================================================================
# Substation
# ==============================================================================================================================
YX, YZ = 12.0, 9.0      # yard half extents: 24 x 18 m, one line bay feeding one transformer
GANTRY_X = -9.5
BAYS_Z = (0.0,)
PH = (-2.0, 0.0, 2.0)
FENCE_W = 6.0           # one fence volume = two 3 m bays (tex_fence)


def build_substation(dims):
    m = Mesh("substation")
    m.box("gravel", (0, 0.05, 0), (YX * 2, 0.1, YZ * 2), bottom=False)
    # fence: 6 m quads (2 triangles each), the 6 m gate in the front (+z) side at x = 0..6
    for zs in (1, -1):
        x = -YX
        while x < YX - 0.01:
            gate = zs > 0 and abs(x) < 0.01
            m.spom_quad_at("q_gate" if gate else "q_fence", (x + 3.0, 1.375, zs * (YZ + 0.2) - 0.08 * zs), (0, 0, zs), 6.0, 2.75, 0.08)
            x += 6.0
    for sx in (-1, 1):
        z = -YZ
        while z < YZ - 0.01:
            m.spom_quad_at("q_fence", (sx * (YX + 0.2) - 0.08 * sx, 1.375, z + 3.0), (sx, 0, 0), 6.0, 2.75, 0.08)
            z += 6.0
    m.box("concrete", (3.0, 0.12, YZ - 2.0), (5.0, 0.04, 4.0), bottom=False)                  # apron inside the gate

    # ---- line gantry: two lattice columns + a lattice beam
    W, Ls, cx = dims["gcol"]
    for cz in (-4.2, 4.2):
        put_face_quads(m, "q_gcol", 0.0, 0.4, 0.4, 8.0, W, Ls, cx, at=(GANTRY_X, cz))
        m.box("concrete", (GANTRY_X, 0.2, cz), (1.2, 0.4, 1.2), bottom=False)
    _, bl = dims["gbeam"]
    for sx in (-1, 1):
        m.spom_quad_at("q_gbeam", (GANTRY_X + sx * (0.4 - P), 7.575, 0.0), (sx, 0, 0), 9.2, 1.15, P, bl)
    attach = []
    for p in PH:
        m.spom_box("ins_string", (GANTRY_X, 7.0 - 1.4, p - 0.2), (0, 0, 1), 0.4, 2.8, 0.4)
        attach.append([GANTRY_X, 7.0 - 2.75, p])

    # ---- per phase: disconnect, breaker, CT; stands are single boxes without bottoms, conductors 4-sided
    for p in PH:
        z = p
        m.box("galv_steel", (-7.0, 1.25, z), (0.25, 2.5, 0.25), bottom=False)                    # disconnect stand
        m.box("galv_steel", (-7.0, 2.55, z), (0.2, 0.1, 1.5), bottom=False)
        for x in (-7.0,):
            m.spom_box("post_ins", (x, 2.6 + 0.875, z - 0.6 - 0.2), (0, 0, 1), 0.4, 1.75, 0.4)
            m.spom_box("post_ins", (x, 2.6 + 0.875, z + 0.6 - 0.2), (0, 0, 1), 0.4, 1.75, 0.4)
        m.box("galv_steel", (-4.0, 1.1, z), (0.35, 2.2, 0.35), bottom=False)
        m.spom_box("breaker", (-4.0, 2.2 + 1.6, z - 0.35), (0, 0, 1), 0.7, 3.2, 0.7)
        m.box("galv_steel", (-1.3, 1.1, z), (0.35, 2.2, 0.35), bottom=False)
        m.spom_box("ct", (-1.3, 2.2 + 1.4, z - 0.4), (0, 0, 1), 0.8, 2.8, 0.8)
        hv = (2.6, 3.6 + 2.2, p * 0.45)
        pts = [(GANTRY_X, 4.3, z), (-7.0, 4.33, z), (-4.0, 5.4, z), (-1.3, 4.8, z), hv]
        for a, b in zip(pts[:-1], pts[1:]):
            m.cyl("aluminium", a, b, 0.035, seg=4)
    m.box("dark_metal", (-4.0, 1.0, 3.2), (0.6, 0.9, 0.35), bottom=False)                      # one breaker control cabinet

    # ---- power transformer
    cx, bz = 4.0, 0.0
    m.box("concrete", (cx, 0.15, bz), (5.6, 0.3, 5.0), bottom=False)
    m.box("tank_grey", (cx, 0.3 + 1.6, bz), (3.6, 3.2, 3.0), bottom=False)
    for sz in (-1, 1):
        m.spom_box("radiator", (cx, 0.3 + 0.35 + 1.15, bz + sz * 1.5), (0, 0, sz), 3.0, 2.3, 0.86)
    for p in PH:
        m.spom_box("hv_bushing", (2.6, 3.5 + 1.1, bz + p * 0.45 - 0.3), (0, 0, 1), 0.6, 2.2, 0.6)
        m.spom_box("lv_bushing", (5.2, 3.5 + 0.45, bz + p * 0.45 - 0.3), (0, 0, 1), 0.6, 0.9, 0.6)
    m.cyl("tank_grey", (5.3, 4.5, bz - 1.1), (5.3, 4.5, bz + 1.1), 0.4, seg=8, caps=True)    # conservator
    m.box("dark_metal", (cx - 1.0, 1.3, bz + 2.5), (0.8, 1.2, 0.4), bottom=False)            # tap-changer cabinet
    attach.append([5.2, 4.4, bz])

    # ---- control house
    hx, hz = 9.0, -6.0
    m.box("wall_block", (hx, 1.6, hz), (4.6, 3.2, 3.6), bottom=False)
    m.box("roof_felt", (hx, 3.3, hz), (4.9, 0.2, 3.9), bottom=False)
    m.box("door_steel", (hx - 2.32, 1.15, hz), (0.06, 2.1, 1.1), bottom=False)
    m.extras = {"attach": attach, "kind": "substation", "footprint": [YX * 2, YZ * 2],
                "attach_note": "first 3 = incoming HV phases at the gantry, last = outgoing LV feeder"}
    return m


# ==============================================================================================================================
if __name__ == "__main__":
    pl.make_plain_textures()
    print("textures: insulators + equipment")
    tex_insulator_string(); tex_post_insulator(); tex_breaker(); tex_ct()
    tex_bushing("hv_bushing", 2.2, 0.07, 0.15, 0.1)
    tex_bushing("lv_bushing", 0.9, 0.06, 0.11, 0.09)
    print("textures: pole")
    tex_pole_shaft("pole_shaft_a", 0.0, 3.4, tag=True)
    tex_pole_shaft("pole_shaft_b", 3.4, 3.4)
    tex_pole_shaft("pole_shaft_c", 6.8, 2.6)
    HEAD_W, HEAD_ATTACH = tex_pole_head(9.4, 1.5)
    CAN_W, CAN_D, CAN_BUSH = tex_pole_can(7.5, 1.6)
    print("textures: fence, gantry, radiator")
    dims = {"fence": tex_fence(), "gate": tex_gate(), "radiator": tex_radiator()}
    dims["gcol"] = lattice_face("q_gcol", 0.4, 0.4, 8.0, [(0.0, 4.0, 1), (4.0, 8.0, 1)], 160, leg_r=0.06, br_r=0.03)
    dims["gbeam"] = warren_arm("q_gbeam", 9.2, 1.0, 1.0, 160, mirror_too=False)
    print("pylon textures + mesh")
    for mesh in (build_pylon(), build_pole(False), build_pole(True), build_substation(dims)):
        size = mesh.write_glb(f"{mesh.name}.glb")
        print(f"{mesh.name}.glb  {mesh.triangles()} tris  {len(mesh.spom)} SPOM sets  {size / 1024:.0f} KB")
