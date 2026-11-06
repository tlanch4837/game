"""Public transit vehicles -> GLB, built on the gen_cars toolkit (Mesh, write_glb, wheel texture).

Types: city bus (12 m), articulated bus (18 m), minibus, coach, school bus, double-decker, tram (3 sections).

Low poly, detail in the maps: each hull section is an extruded side profile with wheel arches and a bevel; everything
finer (panel joints, rivets, service flaps, gaskets, door leaves, louvres, lamp recesses, bellows ribs, LED sign) is relief
in gen_transit_tex's height maps. The engine turns each normal map back into height (surface_maps.h) and runs parallax
occlusion with sun self-shadowing on it (SurfaceRelief, mesh.hlsl), so it reads as depth at street distance.

Variation: every pane picks a tile from a 16-variant window atlas (interiors, passengers, tints, blinds, curtains, frit,
vents, ad wrap) with per-type weights, seeded per livery, and the street side may carry an advert poster. Two livery
colours: body + trim.

Run:  python gen_transit.py [type ...]
Output: assets/vehicles/transit/glb/TRANSIT_<type>_<livery>.glb
Units: metres, +Y up, front faces +Z, origin on the ground under the vehicle centre. Passenger doors on the
right-hand side (-X), for right-hand traffic.
"""
import math, os, sys, zlib
import numpy as np
import gen_cars as gc
import gen_transit_tex as tx
from gen_cars import Mesh, box, tube, offset_poly, area, DARK, METAL, COLOURS

OUT = os.path.join(gc.ROOT, "assets", "vehicles", "transit", "glb")
FL = tx.FLAT

# atlas regions (u0, v0, u1, v1), v down
FASCIA, SCHOOL_GRILLE = (0, 0.205, 1, 0.545), (0.03, 0.61, 0.97, 0.99)
TAIL, LOUVRE = (0, 0.005, 1, 0.445), (0, 0.505, 1, 0.995)

# per-type glass overlay mix: (overlay tile, weight). The interior itself is ray-cast by the engine (InteriorPS).
O = tx.OVL
MIX_CITY = [(O["clear"], 6), (O["clear_sticker"], 2), (O["frit"], 2), (O["hopper"], 2), (O["frit_hopper"], 1), (O["tint_light"], 2), (O["blind_half"], 1)]
MIX = {"citybus": MIX_CITY, "artic": MIX_CITY + [(O["adwrap"], 1)], "double": MIX_CITY,
       "minibus": [(O["clear"], 2), (O["tint_light"], 2), (O["tint_dark"], 2)],
       "coach": [(O["curtain_open"], 4), (O["curtain_half"], 2), (O["tint_light"], 3), (O["tint_dark"], 1), (O["blind_full"], 1)],
       "school": [(O["school"], 5), (O["school_down"], 2)],
       "tram": [(O["clear"], 6), (O["clear_sticker"], 2), (O["frit"], 2), (O["tint_light"], 1), (O["adwrap"], 1)]}
STYLE = {"citybus": 0, "artic": 0, "double": 0, "minibus": 0, "coach": 1, "school": 2, "tram": 3}
END = 4  # windshield / end windows look down the saloon
SEATS = [(0.12, 0.18, 0.42), (0.42, 0.08, 0.10), (0.10, 0.30, 0.26), (0.22, 0.22, 0.25), (0.40, 0.28, 0.08), (0.08, 0.10, 0.12)]


def pane(m, s, x, z0, z1, y0, y1, style, k, variant, normal_z=None):
    """Interior-mapped pane on a side: u = pane index + across, v = style*16 + variant + down. +X runs u toward -z
    so overlay lettering reads correctly from outside on both sides."""
    base = k; variant = style * 16 + variant
    if s < 0: ua, ub = base, base + 1
    else: ua, ub = base + 1, base
    v0, v1 = variant, variant + 0.999
    m.quad("interior", [(s * x, y0, z0), (s * x, y0, z1), (s * x, y1, z1), (s * x, y1, z0)], [(ua, v1), (ub, v1), (ub, v0), (ua, v0)], np.array([s, 0, 0]))


def end_pane(m, d, z, hw_, y0, y1, variant):
    """Interior-mapped window on an end (d=+1 front, -1 rear), looking down the saloon."""
    xs = (-hw_, hw_) if d > 0 else (hw_, -hw_); b = 0; variant = END * 16 + variant
    m.quad("interior", [(xs[0], y0, z), (xs[1], y0, z), (xs[1], y1, z), (xs[0], y1, z)],
           [(b, variant + 0.999), (b + 0.999, variant + 0.999), (b + 0.999, variant), (b, variant)], np.array([0, 0, d]))


TYPES = {
    "citybus": dict(W=2.55, H=3.05, yb=0.32, r=0.50, sections=[dict(z0=-6.0, z1=6.0, axles=[(3.3, 0), (-2.6, 1)], front="nose", rear="tail",
                    win=[(1.25, 2.55)], doors=[(4.55, 5.75), (-0.15, 1.05)])], pod=1,
                    liveries=[("red", "white"), ("blue", "white"), ("green", "white"), ("white", "blue"), ("orange", "grey"), ("teal", "white")]),
    "artic":   dict(W=2.55, H=3.05, yb=0.32, r=0.50, sections=[
                    dict(z0=0.65, z1=9.0, axles=[(6.3, 0), (1.9, 1)], front="nose", rear="joint", win=[(1.25, 2.55)], doors=[(7.55, 8.75), (3.2, 4.4)]),
                    dict(z0=-9.0, z1=-0.65, axles=[(-6.2, 0)], front="joint", rear="tail", win=[(1.25, 2.55)], doors=[(-2.4, -1.2), (-8.4, -7.2)])],
                    pod=1, liveries=[("red", "white"), ("blue", "white"), ("green", "white"), ("white", "teal")]),
    "minibus": dict(W=2.10, H=2.75, yb=0.30, r=0.38, sections=[dict(z0=-3.6, z1=3.6, axles=[(2.3, 0), (-1.9, 0)], front="nose", rear="tail",
                    win=[(1.20, 2.25)], doors=[(1.25, 2.05)])], minibus=1,
                    liveries=[("white", "blue"), ("silver", "red"), ("yellow", "black"), ("white", "green")]),
    "coach":   dict(W=2.55, H=3.65, yb=0.38, r=0.52, sections=[dict(z0=-6.5, z1=6.5, axles=[(4.4, 0), (-2.5, 1), (-3.9, 0)], front="nose", rear="tail",
                    win=[(1.95, 3.25)], doors=[(4.95, 5.95), (-0.6, 0.3)])], coach=1,
                    liveries=[("white", "blue"), ("silver", "maroon"), ("blue", "white"), ("black", "orange")]),
    "school":  dict(W=2.44, H=3.00, yb=0.38, r=0.50, sections=[dict(z0=-5.6, z1=4.0, axles=[(-2.8, 1)], front="flat", rear="tail",
                    win=[(1.55, 2.45)], doors=[(3.0, 3.85)])], school=1, liveries=[("yellow", "black")]),
    "double":  dict(W=2.55, H=4.35, yb=0.32, r=0.50, sections=[dict(z0=-5.5, z1=5.5, axles=[(3.2, 0), (-2.4, 1)], front="nose", rear="tail",
                    win=[(1.25, 2.30), (2.80, 3.95)], doors=[(4.15, 5.25), (-0.4, 0.7)])], double=1,
                    liveries=[("red", "beige"), ("blue", "white"), ("green", "beige"), ("maroon", "white")]),
    "tram":    dict(W=2.40, H=3.45, yb=0.30, r=0.33, sections=[
                    dict(z0=6.1, z1=13.2, axles=[(10.9, 0), (9.1, 0)], front="tram", rear="joint", win=[(1.05, 2.75)], doors=[(7.0, 8.3)]),
                    dict(z0=-3.4, z1=5.4, axles=[], front="joint", rear="joint", win=[(1.05, 2.75)], doors=[(-1.6, -0.3), (2.6, 3.9)]),
                    dict(z0=-13.2, z1=-4.1, axles=[(-10.9, 0), (-9.1, 0)], front="joint", rear="tram_r", win=[(1.05, 2.75)], doors=[(-6.8, -5.5)])],
                    tram=1, liveries=[("red", "white"), ("white", "green"), ("teal", "white"), ("yellow", "grey")]),
}


def body_uv(z, y):
    return (z / tx.BODY_PITCH, 1 - y / tx.BODY_H)


def arch(zc, r, yb, gap=0.07):
    ra = r + gap; th0 = math.asin(max(-1, min(1, (yb - r) / ra)))
    return [(zc + ra * math.cos(a), r + ra * math.sin(a)) for a in np.linspace(math.pi - th0, th0, 7)]


def tram_nose(t):
    """Tram cab profile as (z offset from the end, y), bottom to top: lower skirt, lamp face, raked windscreen, roof slope."""
    yb, H = t["yb"], t["H"]
    return [(-0.30, yb), (0.0, yb + 0.40), (0.03, 1.05), (-0.32, H - 0.42), (-0.80, H)]


def nose_quad(m, mat, t, z_end, d, i, f0, f1, half, uv, out=0.012):
    """Quad lying on nose segment i (between profile points i and i+1), from fraction f0 to f1 along it, offset out of the
    surface by `out`. d = +1 front cab, -1 rear cab. uv (u0, v0, u1, v1), u left->right seen from outside, v0 at the top."""
    P = tram_nose(t); (za, ya), (zb, yb_) = P[i], P[i + 1]
    dz, dy = zb - za, yb_ - ya; L = math.hypot(dz, dy)
    nz, ny = d * dy / L, -dz / L                        # outward, in world z / y
    pt = lambda f: (z_end + d * (za + dz * f) + nz * out, ya + dy * f + ny * out)
    (z0, y0), (z1, y1) = pt(f0), pt(f1)
    xs = (-half, half) if d > 0 else (half, -half); u0, v0, u1, v1 = uv
    m.quad(mat, [(xs[0], y0, z0), (xs[1], y0, z0), (xs[1], y1, z1), (xs[0], y1, z1)], [(u0, v1), (u1, v1), (u1, v0), (u0, v0)], np.array([0, ny, nz]))


def profile(t, sec):
    """CCW side silhouette (z, y) of one hull section with wheel arches; nose/tail get chamfered corners."""
    z0, z1, yb, H, r = sec["z0"], sec["z1"], t["yb"], t["H"], t["r"]
    pts = [(z0 + (0.10 if sec["rear"] == "tail" else 0), yb)]
    if not t.get("tram"):
        for zc, _ in sorted(sec["axles"]): pts += arch(zc, r, yb)
    f, b = sec["front"], sec["rear"]
    if f in ("nose", "flat"):
        pts += [(z1 - 0.08, yb), (z1, yb + 0.14), (z1, H - (0.30 if f == "nose" else 0.12)), (z1 - (0.22 if f == "nose" else 0.10), H)]
    elif f == "tram":
        pts += [(z1 + dz, y) for dz, y in tram_nose(t)]
    else:
        pts += [(z1, yb), (z1, H)]
    if b == "tail":
        pts += [(z0 + 0.15, H), (z0, H - 0.20), (z0, yb + 0.14)]
    elif b == "tram_r":
        pts = [(z0 - tram_nose(t)[0][0], yb)] + pts[1:] + [(z0 - dz, y) for dz, y in reversed(tram_nose(t)[1:])]
    else:
        pts += [(z0, H)]
    if area(pts) < 0: pts = pts[::-1]
    return pts


def extrude(m, t, prof, hw, bev=0.06):
    """Side caps (panel texture) + bevel ring + edge strips (flat spot). Under-arch strips are dark."""
    inner = offset_poly(prof, bev); n = len(prof)
    for s in (+1, -1):
        m.poly("body", inner, lambda p, s=s: (s * hw, p[1], p[0]), lambda p: body_uv(p[0], p[1]), np.array([s, 0, 0]))
    for i in range(n):
        a, b = prof[i], prof[(i + 1) % n]; ia, ib = inner[i], inner[(i + 1) % n]
        dz, dy = b[0] - a[0], b[1] - a[1]
        out = np.array([0, -dz, dy]); out /= np.linalg.norm(out)
        dark = max(a[1], b[1]) < t["r"] * 2 + 0.12 and out[1] < 0.2 and not (abs(dz) < 1e-6)
        dark = dark or (a[1] <= t["yb"] + 1e-6 and b[1] <= t["yb"] + 1e-6)
        P = [(-(hw - bev), a[1], a[0]), (hw - bev, a[1], a[0]), (hw - bev, b[1], b[0]), (-(hw - bev), b[1], b[0])]
        if dark: m.quad("wheel", P, [DARK] * 4, out)
        elif out[1] > 0.45: m.grid("body", P, [FL] * 4, out, 3, 1, lambda u, v: np.array([0, 0.03 * (1 - (2 * u - 1) ** 2), 0]))
        else: m.quad("body", P, [FL] * 4, out)
        for s in (+1, -1):
            Q = [(s * (hw - bev), a[1], a[0]), (s * (hw - bev), b[1], b[0]), (s * hw, ib[1], ib[0]), (s * hw, ia[1], ia[0])]
            m.quad("wheel" if dark else "body", Q, [DARK] * 4 if dark else [body_uv(q[2], q[1]) for q in Q], out + np.array([s * 0.7, 0, 0]))


def wheel(m, zc, r, hw, s, tw=0.28, rec=0.06, seg=14):
    """Tyre + recessed rim from the car wheel photo; 5 triangles per segment (tread, sidewall, rim fan)."""
    rin = 0.66
    xo, xi, xr = s * (hw - 0.03), s * (hw - 0.03 - tw), s * (hw - 0.03 - rec)
    ring = [(math.cos(2 * math.pi * k / seg), math.sin(2 * math.pi * k / seg)) for k in range(seg)]
    p = lambda c, x, f=1.0: (x, r + r * f * c[1], zc + r * f * c[0])
    tuv = lambda c, f: (0.5 + 0.5 * f * c[0] * s, 0.5 - 0.5 * f * c[1])
    for k in range(seg):
        c0, c1 = ring[k], ring[(k + 1) % seg]
        m.quad("wheel", [p(c0, xr, rin), p(c1, xr, rin), p(c1, xo, 1.0), p(c0, xo, 1.0)],  # sidewall slopes from the rim lip out
               [tuv(c0, rin), tuv(c1, rin), tuv(c1, 1.0), tuv(c0, 1.0)], np.array([s, 0, 0]))
        m.tri("wheel", [p((0, 0), xr), p(c0, xr, rin), p(c1, xr, rin)], [(0.5, 0.5), tuv(c0, rin), tuv(c1, rin)], np.array([s, 0, 0]))
        u0, u1 = 0.40 + 0.2 * k / seg, 0.40 + 0.2 * (k + 1) / seg
        m.quad("wheel", [p(c0, xi), p(c0, xo), p(c1, xo), p(c1, xi)], [(u0, 0.035), (u0, 0.005), (u1, 0.005), (u1, 0.035)],
               np.array([0, c0[1] + c1[1], c0[0] + c1[0]]))


def arch_liner(m, zc, r, hw, s, depth=0.5):
    ra = r + 0.07; A = np.linspace(0.15, math.pi - 0.15, 6)
    for a0, a1 in zip(A[:-1], A[1:]):
        P = lambda a, x: (x, r + ra * math.sin(a), zc + ra * math.cos(a))
        m.quad("wheel", [P(a0, s * (hw + 0.001)), P(a1, s * (hw + 0.001)), P(a1, s * (hw - depth)), P(a0, s * (hw - depth))], [DARK] * 4,
               -np.array([0, math.sin((a0 + a1) / 2), math.cos((a0 + a1) / 2)]))


def side_quad(m, mat, s, x, z0, z1, y0, y1, reg=(0, 0, 1, 1)):
    """Quad on a side (s=+1 street side +X, -1 kerb side); reg = atlas region (u0, v0, u1, v1), u running front->back
    on both sides (mirrored on +X so the picture reads left-to-right from outside... either way it's an interior)."""
    u0, v0, u1, v1 = reg
    uv = [(u0, v1), (u1, v1), (u1, v0), (u0, v0)]
    m.quad(mat, [(s * x, y0, z0), (s * x, y0, z1), (s * x, y1, z1), (s * x, y1, z0)], uv if s < 0 else [uv[1], uv[0], uv[3], uv[2]], np.array([s, 0, 0]))


def end_quad(m, mat, d, z, hw_, y0, y1, reg):
    """Quad on a z-facing end (d=+1 front, -1 rear) spanning x -hw_..hw_; reg (u0, v0, u1, v1), u left->right seen from outside."""
    u0, v0, u1, v1 = reg; xs = (-hw_, hw_) if d > 0 else (hw_, -hw_)
    m.quad(mat, [(xs[0], y0, z), (xs[1], y0, z), (xs[1], y1, z), (xs[0], y1, z)], [(u0, v1), (u1, v1), (u1, v0), (u0, v0)], np.array([0, 0, d]))


def pick(rng, mix):
    tiles, w = zip(*mix); w = np.array(w, float)
    return tiles[rng.choice(len(tiles), p=w / w.sum())]


def section(m, t, sec, rng, mix):
    hw, H, yb, r = t["W"] / 2, t["H"], t["yb"], t["r"]
    z0, z1 = sec["z0"], sec["z1"]
    extrude(m, t, profile(t, sec), hw)
    # ---- running gear
    for zc, dual in sec["axles"]:
        for s in (+1, -1):
            if t.get("tram"):
                wheel(m, zc, r, hw - 0.25, s, tw=0.14, rec=0.03, seg=10)
            else:
                wheel(m, zc, r, hw, s, tw=0.42 if dual else 0.30, rec=0.12 if dual else 0.06)
                arch_liner(m, zc, r, hw, s)
    if t.get("tram") and sec["axles"]:  # bogie skirt panel over the wheels
        zm = sum(a for a, _ in sec["axles"]) / len(sec["axles"])
        for s in (+1, -1): side_quad(m, "wheel", s, hw - 0.03, zm - 1.5, zm + 1.5, 0.12, yb + 0.02, (*DARK, *DARK))
    # ---- livery bands (trim colour, flat)
    lo = z0 + (0.2 if sec["rear"] != "joint" else 0.01); hi = z1 - (0.2 if sec["front"] != "joint" else 0.01)
    wy0 = sec["win"][0][0]
    bands = [(wy0 - 0.30, wy0 - 0.12)]
    if t.get("school"): bands += [(wy0 - 0.61, wy0 - 0.55), (wy0 - 0.81, wy0 - 0.75), (0.89, 0.95)]
    if t.get("tram"): bands += [(H - 0.45, H - 0.30)]
    for s in (+1, -1):
        for b0, b1 in bands: side_quad(m, "trim", s, hw + 0.003, lo, hi, b0, b1, (*FL, *FL))
    # ---- windows: contiguous panes, each a random atlas tile (its black border is the pillar)
    for row, (wy0, wy1) in enumerate(sec["win"]):
        a = z0 + (0.6 if sec["rear"] == "tail" else 0.35 if sec["rear"] == "tram_r" else 0.25)
        b = z1 - (1.2 if sec["front"] == "nose" else 0.9 if sec["front"] == "tram" else 0.25)
        if t.get("school") or t.get("minibus"): b = z1 - 0.3
        pitch = 1.25 if not t.get("school") else 0.85
        npanes = max(1, int(round((b - a) / pitch))); step = (b - a) / npanes
        for s in (+1, -1):
            for k in range(npanes):
                p0, p1 = a + k * step, a + (k + 1) * step
                if s < 0 and any(d0 - 0.1 < (p0 + p1) / 2 < d1 + 0.1 for d0, d1 in sec["doors"]) and row == 0: continue  # door there
                kk = k if s < 0 else npanes - 1 - k
                pane(m, s, hw + 0.005, p0, p1, wy0, wy1, t["style"], kk + row * 30, pick(rng, mix))
    # ---- passenger doors (kerb side -X): one textured quad each
    for d0, d1 in sec["doors"]:
        ytop = sec["win"][0][1] + (0.05 if not t.get("coach") else -0.2)
        ybot = yb + 0.06 if not (t.get("coach") and d1 > 0) else 0.40
        side_quad(m, "door", -1, hw + 0.010, d0, d1, ybot, ytop)
    # ---- street-side advert between the axles
    if not t.get("school") and not t.get("minibus") and rng.random() < 0.75 and sec["front"] != "tram":
        za = (max(zc for zc, _ in sec["axles"]) + min(zc for zc, _ in sec["axles"])) / 2 if len(sec["axles"]) > 1 else (z0 + z1) / 2
        ad = int(rng.integers(4)); reg = ((ad % 2) / 2 + 0.003, (ad // 2) / 2 + 0.003, (ad % 2 + 1) / 2 - 0.003, (ad // 2 + 1) / 2 - 0.003)
        zs = sorted(zc for zc, _ in sec["axles"])
        gap = (zs[-1] - zs[0] - 2 * (t["r"] + 0.25)) if len(zs) > 1 else (z1 - z0 - 1.0)  # clear of the arches
        ytop = sec["win"][0][0] - 0.34; hgt = min(0.85, ytop - t["yb"] - 0.20, gap / 2.8)
        if hgt > 0.30: side_quad(m, "ad", 1, hw + 0.004, za - hgt * 1.4, za + hgt * 1.4, ytop - hgt, ytop, reg)
    if t.get("coach"):  # luggage bay door seams
        for s in (+1, -1):
            for k in range(5):
                zz = -2.0 + k * 1.2
                if -3.0 < zz < 3.4: side_quad(m, "wheel", s, hw + 0.004, zz - 0.012, zz + 0.012, 0.55, 1.75, (*DARK, *DARK))
            side_quad(m, "wheel", s, hw + 0.004, -2.3, 3.4, 1.73, 1.76, (*DARK, *DARK))
    # ---- ends
    if sec["front"] in ("nose", "flat"):
        zf = z1 + 0.004; wtop = H - (0.62 if sec["front"] == "nose" else 0.35)
        wbot = 1.15 if not t.get("school") else sec["win"][0][0] + 0.1  # windshield starts above the lamps, as on a real bus
        if t.get("double"): wtop = 2.40
        end_pane(m, 1, zf, hw - 0.08, wbot, wtop, O["windshield"])
        if t.get("double"):
            end_pane(m, 1, zf, hw - 0.10, sec["win"][1][0], sec["win"][1][1], O["clear"])
            end_quad(m, "sign", 1, zf + 0.002, hw - 0.30, 2.50, 2.74, (0, 0, 1, 1))
        elif sec["front"] == "nose":
            end_quad(m, "sign", 1, zf + 0.002, hw - 0.14, wtop + 0.05, H - 0.34, (0, 0, 1, 1))
        end_quad(m, "front", 1, zf, hw - 0.04, yb + 0.06, wbot - 0.03, FASCIA)
        for s in (+1, -1):  # mirrors on arms
            base = np.array([s * (hw - 0.05), H - 0.55, z1 - 0.05]); tip = base + np.array([s * 0.35, 0.10, 0.30])
            tube(m, "wheel", base, tip, 0.025, DARK, 4, caps=False)
            box(m, "wheel", tip[0] - 0.06, tip[0] + 0.06, tip[1] - 0.45, tip[1] + 0.02, tip[2] - 0.04, tip[2] + 0.02, DARK)
    for end, d in (("tram", 1), ("tram_r", -1)):  # tram cabs: every part lies on its own face of the nose profile
        if (sec["front"] if d > 0 else sec["rear"]) != end: continue
        ze = z1 if d > 0 else z0
        nose_quad(m, "front" if d > 0 else "rear", t, ze, d, 1, 0.08, 0.92, hw - 0.12, FASCIA if d > 0 else TAIL)   # lamps
        nose_quad(m, "wheel", t, ze, d, 2, 0.02, 0.99, hw - 0.07, (*DARK, *DARK), out=0.008)                           # black glazing band
        nose_quad(m, "interior", t, ze, d, 2, 0.07, 0.94, hw - 0.14, (0, 78, 0.999, 78.999))                           # windscreen
        nose_quad(m, "wheel", t, ze, d, 3, 0.10, 0.80, hw - 0.20, (*DARK, *DARK), out=0.045)  # clear of the roof crown                           # sign housing
        nose_quad(m, "sign", t, ze, d, 3, 0.16, 0.74, hw - 0.30, (0, 0, 1, 1), out=0.05)                                         # destination
        nose_quad(m, "wheel", t, ze, d, 0, 0.0, 1.0, 0.45, (*DARK, *DARK))                                             # coupler cover
    if sec["rear"] == "tail":
        zr = z0 - 0.004
        end_quad(m, "rear", -1, zr, hw - 0.04, yb + 0.06, yb + 0.95, TAIL)
        if t.get("coach") or t.get("school") or t.get("minibus") or t.get("double"):
            for wy0, wy1 in sec["win"]: end_pane(m, -1, zr, hw - 0.18, wy0, wy1, O["rear"])
        else:  # engine-bay louvre door, small rear window above
            end_quad(m, "rear", -1, zr, hw - 0.22, yb + 1.0, sec["win"][0][0] + 0.25, LOUVRE)
            end_pane(m, -1, zr, hw - 0.28, sec["win"][0][0] + 0.40, sec["win"][0][1] - 0.05, O["rear"])


def bellows(m, t, za, zb, H):
    """Concertina joint: three quads (two sides + roof), ribs from the bellows height map."""
    hw = t["W"] / 2 - 0.10; y0, y1 = t["yb"] + 0.15, H - 0.10; reps = (zb - za) / 0.18 / 8 * 1.0
    for s in (+1, -1):
        m.quad("bellows", [(s * hw, y0, za), (s * hw, y0, zb), (s * hw, y1, zb), (s * hw, y1, za)], [(0, 1), (reps, 1), (reps, 0), (0, 0)], np.array([s, 0, 0]))
    m.quad("bellows", [(-hw, y1, za), (hw, y1, za), (hw, y1, zb), (-hw, y1, zb)], [(0, 0), (0, 1), (reps, 1), (reps, 0)], np.array([0, 1, 0]))


def build(t, seed, mix):
    rng = np.random.default_rng(seed)
    m = Mesh(); hw, H = t["W"] / 2, t["H"]
    secs = t["sections"]
    for sec in secs: section(m, t, sec, rng, mix)
    ordered = sorted(secs, key=lambda s: s["z0"])
    for a, b in zip(ordered[:-1], ordered[1:]): bellows(m, t, a["z1"], b["z0"], H)
    front = max(secs, key=lambda s: s["z1"])
    if t.get("pod"):  # A/C pod: the louvre panel on its sides gives it vents for 10 triangles
        for sec in secs:
            c = (sec["z0"] + sec["z1"]) / 2
            box(m, "trim", -hw + 0.35, hw - 0.35, H, H + 0.22, c - 1.6, c + 1.6, FL)
            for s in (+1, -1): side_quad(m, "rear", s, hw - 0.349, c - 1.3, c + 1.3, H + 0.03, H + 0.19, LOUVRE)
    if t.get("coach"):
        box(m, "body", -hw + 0.30, hw - 0.30, H, H + 0.12, 1.0, 3.6, FL)
    if t.get("school"):  # conventional hood, fenders, grille, warning lamps, stop arm
        z1 = front["z1"]; zh = z1 + 1.55
        gc.tapered(m, "body", z1, zh, (hw - 0.30, 1.05, 1.60), (hw - 0.42, 1.05, 1.35), lambda f, k: FL)
        for s in (+1, -1):
            box(m, "body", s * (hw - 0.30), s * (hw - 0.02), 1.05, 1.18, z1 + 0.05, zh - 0.10, FL)
            wheel(m, z1 + 0.80, t["r"], hw - 0.04, s, tw=0.30)
        end_quad(m, "front", 1, zh + 0.004, hw - 0.44, 0.62, 1.32, SCHOOL_GRILLE)
        box(m, "wheel", -hw + 0.02, hw - 0.02, 0.40, 0.62, zh - 0.05, zh + 0.12, DARK)
        box(m, "wheel", -hw + 0.30, hw - 0.30, 0.55, 1.05, z1, zh - 0.05, DARK)
        for s in (+1, -1):
            for z in (z1 + 0.01, front["z0"] - 0.01):
                box(m, "rear", s * 0.75 - 0.10, s * 0.75 + 0.10, H - 0.30, H - 0.10, z - 0.03, z + 0.03, (0.07, 0.06))
        box(m, "rear", -hw - 0.45, -hw - 0.02, 1.55, 1.95, 2.6, 2.63, (0.07, 0.06))  # folded stop arm
        for s in (+1, -1):
            base = np.array([s * (hw - 0.05), 1.9, z1 + 0.10]); tip = base + np.array([s * 0.30, 0.05, 0.25])
            tube(m, "wheel", base, tip, 0.025, DARK, 4, caps=False)
            box(m, "wheel", tip[0] - 0.05, tip[0] + 0.05, tip[1] - 0.35, tip[1] + 0.02, tip[2] - 0.03, tip[2] + 0.02, DARK)
    if t.get("tram"):
        mid = ordered[1]; c = (mid["z0"] + mid["z1"]) / 2
        box(m, "trim", -0.70, 0.70, H, H + 0.30, c - 1.8, c + 1.8, FL)
        for sec in secs:
            if sec is not mid:
                cc = (sec["z0"] + sec["z1"]) / 2
                box(m, "trim", -0.80, 0.80, H, H + 0.25, cc - 1.4, cc + 1.4, FL)
                for s in (+1, -1): side_quad(m, "rear", s, 0.801, cc - 1.2, cc + 1.2, H + 0.03, H + 0.22, LOUVRE)
        base = H + 0.30; knee = (c - 0.6, base + 0.75); head = (c + 0.1, base + 1.35)
        for x in (-0.35, 0.35):
            tube(m, "metal", (x, base + 0.05, c + 0.5), (x * 0.4, knee[1], knee[0]), 0.025, METAL, 4, caps=False)
            tube(m, "metal", (x * 0.4, knee[1], knee[0]), (x * 0.9, head[1], head[0]), 0.02, METAL, 4, caps=False)
        box(m, "wheel", -0.85, 0.85, head[1], head[1] + 0.04, head[0] - 0.08, head[0] + 0.08, DARK)
    return m


def load_maps():
    maps = gc.load_maps()                 # wheel / metal from the car photos
    maps.update(tx.build_all())           # body, glass, front, rear, door, bellows, ad
    maps["trim"] = (maps["body"][0], None)
    gc.MAT_PBR.update({"interior": (0.0, 0.05), "sign": (0.0, 0.3), "door": (0.1, 0.25), "bellows": (0.0, 0.9), "ad": (0.0, 0.35), "glass": (0.0, 0.10)})
    return maps


# Destination sign per vehicle: "<route> <destination>". Override with  python gen_transit.py --sign "7 AIRPORT" [type ...]
# or add an entry here keyed "type_colour". The sign is its own material ("sign") and texture in every GLB, so swapping that
# one texture is all a runtime route change needs.
SIGNS = {"citybus": ["12 CENTRAL", "4 HARBOUR", "31 UNIVERSITY", "8 OLD TOWN", "22 HOSPITAL", "15 STADIUM"],
         "artic": ["X1 AIRPORT", "50 MARKET SQ", "9 RIVERSIDE", "61 TECH PARK"],
         "minibus": ["101 HILLTOP", "102 VILLAGE", "SHUTTLE", "103 STATION"],
         "coach": ["EXPRESS NORTH", "COASTLINE", "PRIVATE HIRE", "CITY TOURS"],
         "school": ["SCHOOL BUS"], "double": ["73 BRIDGE ST", "38 CENTRAL", "N9 NIGHT BUS", "11 PARK LANE"],
         "tram": ["1 DOWNTOWN", "2 SEAFRONT", "3 MUSEUMS", "4 DEPOT"]}


def main():
    os.makedirs(OUT, exist_ok=True)
    maps = load_maps()
    args = sys.argv[1:]; override = None
    if "--sign" in args:
        k = args.index("--sign"); override = args[k + 1]; del args[k:k + 2]
    for tn in (args or TYPES):
        t = TYPES[tn]
        for i, (body, trim) in enumerate(t["liveries"]):
            seed = zlib.crc32(f"{tn}{i}".encode()); t["style"] = STYLE[tn]
            mesh = build(t, seed=seed, mix=MIX[tn])
            out = os.path.join(OUT, f"TRANSIT_{tn}_{body}.glb")
            text = override or SIGNS.get(f"{tn}_{body}") or SIGNS[tn][i % len(SIGNS[tn])]
            maps["sign"] = (tx.sign(text), None)
            gc.write_glb(out, mesh, maps, COLOURS.get(body, (0.02, 0.02, 0.02)), "dark" if tn == "coach" else "alloy",
                         trim=COLOURS.get(trim, (0.03, 0.03, 0.03)), seat=SEATS[seed % len(SEATS)])
        L = max(s["z1"] for s in t["sections"]) - min(s["z0"] for s in t["sections"])
        print(f"{tn}: {L:.1f} m, {mesh.count()} tris, {len(t['liveries'])} liveries, {os.path.getsize(out) / 1e6:.2f} MB per GLB")


if __name__ == "__main__":
    main()
