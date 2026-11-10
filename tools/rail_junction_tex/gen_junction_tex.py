"""Rail junction SPOM textures: one albedo + height picture per canonical junction layout, drawn for the junction's flat
SPOM box (src/city/rail_junction_plane.h), rendered by the plain track's own parallax shader (TrackPS).

Same pipeline and frame as the plain track (tools/vehicle_gen/gen_track.py): heights in metres with 0 = rail head and
FORMATION = -0.55 the box bottom, stored as 8-bit height maps. What it draws, from obj/rail_junction_layouts.json
(tools/rail_junction_tex/dump_layouts.bat, so the picture matches the C++ geometry to the centimetre):
  plain track    stamped straight from the plain track tile (gen_track.bake) along each route, in the same sleeper phase
                 (routes are whole pitches long and start half a pitch before their first sleeper), so the junction's
                 ends continue into the plain track sweep with no seam: same scale, colours, ballast and edge profile.
  switch zone    ballast at crib level with the long concrete bearers (TieStations: Long), rails of every route on top
                 (stock rails, blades, closure rails), frog castings and check rails (PlanFrogs).
  gaps           ballast fill between tracks that run within ~6 m of each other (the wedges of turnouts, wyes, 3-ways,
                 crossovers, slips and diamonds), falling off at 1:1.5 like the track's shoulders, then holes.
  level crossing the deck flush with the rail head across the road and sidewalks, in the road's own asphalt (sampled
                 from assets/roads/tex/asphalt_albedo.png at its 4 m world tiling), panel joints and open flangeways.
Holes (height byte 0) are where the box has no surface: the shader discards there (terrain shows through).
Texel ~2 cm (long side capped at 2048, sizes multiples of 4 for ASTC). Deterministic (fixed seeds).
Output: assets/rail/junction/tex/<id>_{albedo.jpg, height.png} and props_albedo.png (the props atlas); previews in
docs/rail_junction_tex/.
Run:  tools\\rail_junction_tex\\dump_layouts.bat  then  python tools/rail_junction_tex/gen_junction_tex.py
"""
import json, math, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "vehicle_gen"))
import gen_track as gt  # noqa: E402

OUT = os.path.join(ROOT, "assets", "rail", "junction", "tex")
DOCS = os.path.join(ROOT, "docs", "rail_junction_tex")
F, CRIB, SL_TOP = gt.FORMATION, gt.CRIB, gt.SLEEPER_TOP
RAIL_X, HEAD_W = gt.RAIL_X, gt.HEAD_W
SHOULDER = 1.65                       # gen_track: crib level out to here, then 1:1.5 down
FILL_GAP = 6.2                        # two tracks this close (centre to centre) share one ballast bed
ASPHALT = [os.path.join(ROOT, "assets", "roads", "tex", "asphalt_albedo.png"),
           os.path.join(ROOT, "..", "cityengine", "assets", "roads", "tex", "asphalt_albedo.png")]


# ---------------------------------------------------------------------------------------------------- source tiles
def track_tile():
    """The plain track tile (gen_track.bake), box-filtered 8x to ~2 cm: H [NV/8, NU/8] and colour."""
    _, _, H, col, _ = gt.bake()
    k = 8
    nv, nu = H.shape[0] // k, H.shape[1] // k
    Hs = H[:nv * k, :nu * k].reshape(nv, k, nu, k).mean((1, 3))
    Cs = col[:nv * k, :nu * k].reshape(nv, k, nu, k, 3).mean((1, 3))
    return H, col, Hs.astype(np.float32), Cs.astype(np.float32)


def sample_tile(T, C, lat, along):
    """Bilinear: lat metres across (clamped to the tile's 4.4 m), along metres (wraps every pitch)."""
    nv, nu = T.shape
    u = np.clip((lat + gt.WIDTH / 2) / gt.WIDTH * nu - 0.5, 0, nu - 1.001)
    v = (np.mod(along, gt.PITCH) / gt.PITCH * nv - 0.5) % nv
    u0, v0 = np.floor(u).astype(int), np.floor(v).astype(int)
    fu, fv = u - u0, v - v0
    u1, v1 = np.minimum(u0 + 1, nu - 1), (v0 + 1) % nv
    def bl(A):
        a = A[v0, u0] * (1 - fu)[..., None] + A[v0, u1] * fu[..., None] if A.ndim == 3 else A[v0, u0] * (1 - fu) + A[v0, u1] * fu
        b = A[v1, u0] * (1 - fu)[..., None] + A[v1, u1] * fu[..., None] if A.ndim == 3 else A[v1, u0] * (1 - fu) + A[v1, u1] * fu
        return a * (1 - fv)[..., None] + b * fv[..., None] if A.ndim == 3 else a * (1 - fv) + b * fv
    return bl(T), bl(C)


def ballast_tile(size=2.4, n=600, seed=2401):
    """Crib-level ballast that tiles every `size` metres both ways, gen_track's stones and palette; 4 mm texels."""
    rng = np.random.default_rng(seed)
    x = (np.arange(n) + 0.5) / n * size
    X, Z = np.meshgrid(x, x)
    H = np.full(X.shape, CRIB, np.float32)
    col = np.zeros(X.shape + (3,), np.float32); col[:] = (0.42, 0.41, 0.40)
    cnt = int(985 * size * size)
    sx, sz = rng.uniform(0, size, cnt), rng.uniform(0, size, cnt)
    rad = rng.uniform(0.018, 0.034, cnt); asp = rng.uniform(0.7, 1.3, cnt); rot = rng.uniform(0, math.pi, cnt)
    tone = rng.uniform(0.30, 0.62, cnt); warm = rng.uniform(-0.03, 0.04, cnt)
    px = size / n
    for i in range(cnt):
        r = rad[i] * 1.6
        for ox in (-size, 0.0, size):
            for oz in (-size, 0.0, size):
                cx, cz = sx[i] + ox, sz[i] + oz
                if cx + r < 0 or cx - r > size or cz + r < 0 or cz - r > size: continue
                i0, i1 = max(0, int((cx - r) / px)), min(n, int((cx + r) / px) + 1)
                j0, j1 = max(0, int((cz - r) / px)), min(n, int((cz + r) / px) + 1)
                if i1 <= i0 or j1 <= j0: continue
                dx, dz = X[j0:j1, i0:i1] - cx, Z[j0:j1, i0:i1] - cz
                c, s = math.cos(rot[i]), math.sin(rot[i])
                u, v = (dx * c + dz * s) / (rad[i] * asp[i]), (-dx * s + dz * c) / (rad[i] / asp[i])
                d2 = u * u + v * v
                facet = 1 - 0.18 * np.abs(np.sin(3 * np.arctan2(v, u) + rot[i] * 7))
                h = CRIB - rad[i] * 0.35 + rad[i] * 0.85 * np.sqrt(np.clip(1 - d2, 0, 1)) * facet
                sub = H[j0:j1, i0:i1]; w = (d2 < 1) & (h > sub); sub[w] = h[w]
                shade = tone[i] * (0.85 + 0.15 * np.sqrt(np.clip(1 - d2, 0, 1)))
                col[j0:j1, i0:i1][w] = np.stack([shade + warm[i], shade + warm[i] * 0.5, shade - warm[i] * 0.3], -1)[w]
    k = 5  # to ~2 cm
    m = n // k
    return size, H.reshape(m, k, m, k).mean((1, 3)), col.reshape(m, k, m, k, 3).mean((1, 3))


def sample_wrap(T, C, size, x, z):
    m = T.shape[0]
    i = (np.floor(np.mod(z, size) / size * m).astype(int)) % m
    j = (np.floor(np.mod(x, size) / size * m).astype(int)) % m
    return T[i, j], C[i, j]


# ---------------------------------------------------------------------------------------------------- one layout
def project(route, X, Z):
    """Nearest point of a route's centre line for every texel: (s, lat, dist, inside, dx, dz); inside = not beyond either
    end, (dx, dz) from the texel to that point."""
    P = np.array(route["pts"], np.float64)
    best = np.full(X.shape, 1e9); S = np.zeros(X.shape); LAT = np.zeros(X.shape); IN = np.zeros(X.shape, bool)
    PX = np.zeros(X.shape); PZ = np.zeros(X.shape)
    for k in range(1, len(P)):
        ax, az, bx, bz = P[k - 1, 0], P[k - 1, 1], P[k, 0], P[k, 1]
        lo_x, hi_x, lo_z, hi_z = min(ax, bx) - 3, max(ax, bx) + 3, min(az, bz) - 3, max(az, bz) + 3
        cols = np.where((Z[0] >= lo_z) & (Z[0] <= hi_z))[0]; rows = np.where((X[:, 0] >= lo_x) & (X[:, 0] <= hi_x))[0]
        if not len(cols) or not len(rows): continue
        sl = (slice(rows[0], rows[-1] + 1), slice(cols[0], cols[-1] + 1))
        qx, qz = X[sl] - ax, Z[sl] - az
        ex, ez = bx - ax, bz - az; L2 = ex * ex + ez * ez; L = math.sqrt(L2)
        tu = (qx * ex + qz * ez) / L2
        t = np.clip(tu, 0, 1)
        dx, dz = qx - t * ex, qz - t * ez
        d = np.hypot(dx, dz)
        w = d < best[sl]
        inside = ~(((k == 1) & (tu < 0)) | ((k == len(P) - 1) & (tu > 1)))
        fx, fz = ex / L, ez / L
        best[sl] = np.where(w, d, best[sl])
        S[sl] = np.where(w, P[k - 1, 4] + t * L, S[sl])
        LAT[sl] = np.where(w, -dx * fz + dz * fx, LAT[sl])  # signed: RightOf(f) = (-f.z, f.x)
        IN[sl] = np.where(w, inside, IN[sl])
        PX[sl] = np.where(w, -dx, PX[sl]); PZ[sl] = np.where(w, -dz, PZ[sl])   # texel -> nearest point
    return S, LAT, best, IN, PX, PZ


def bake_layout(L, track, ballast, asphalt):
    TH, TC = track
    bsize, BH, BC = ballast
    x0, z0, x1, z1 = L["rect"]; nA, nX = L["tex"]
    xs = x0 + (np.arange(nA) + 0.5) * (x1 - x0) / nA
    zs = z0 + (np.arange(nX) + 0.5) * (z1 - z0) / nX
    X, Z = np.meshgrid(xs, zs, indexing="ij")           # rows along x, columns across z
    H = np.full(X.shape, -9.0, np.float32)               # -9: hole
    col = np.zeros(X.shape + (3,), np.float32); col[:] = (0.36, 0.31, 0.25)
    def put(mask, h, c):
        nonlocal H
        w = mask & (h > H); H = np.where(w, h, H); col[w] = c[w] if np.ndim(c) == 3 else c
    gb, gc = sample_wrap(BH, BC, bsize, X, Z)            # ballast fill, world tiled
    routes = [r for r in L["routes"] if not r["virt"]]
    proj = [project(r, X, Z) for r in routes]
    # ---- ballast bed: each route's profile (crib to 1.65 m, then 1:1.5), and the shared bed between close tracks
    for S, LAT, D, IN, _, _ in proj:
        a = np.abs(LAT)
        h = np.where(a <= SHOULDER, gb, CRIB - (a - SHOULDER) / 1.5 + (gb - CRIB))
        put(IN & (h > F + 0.004), h.astype(np.float32), gc)
    for i in range(len(proj)):
        for j in range(i + 1, len(proj)):
            Di, Dj = proj[i][2], proj[j][2]
            between = proj[i][4] * proj[j][4] + proj[i][5] * proj[j][5] < 0  # the two nearest points on opposite sides
            # the shared bed falls off 1:1.5 towards the box's edges, as a shoulder: where it meets an edge (between two
            # tracks leaving the box, or in a diamond's acute angle) it does not stand as a cut wall over the ground
            edge = np.minimum(np.minimum(X - x0, x1 - X), np.minimum(Z - z0, z1 - Z))
            hf = np.minimum(gb, F + (edge - 0.05) / 1.5 + (gb - CRIB)).astype(np.float32)
            put(proj[i][3] & proj[j][3] & between & (Di + Dj < FILL_GAP) & (hf > F + 0.004), hf, gc)
    # ---- plain track: stamped from the plain tile, in phase
    ties = [r["ties"] for r in routes]
    for (S, LAT, D, IN, _, _), T in zip(proj, ties):
        st = np.array([t[5] for t in T]) if T else np.zeros(0)
        idx = np.clip(np.floor(S / gt.PITCH).astype(int), 0, max(len(st) - 1, 0))
        plain = IN & (np.abs(LAT) < gt.WIDTH / 2) & (st[idx] == 1 if len(st) else False)
        h, c = sample_tile(TH, TC, LAT, S)
        put(plain, h, c)
    # ---- long bearers under the switches
    conc = sample_tile(TH, TC, np.array([0.3]), np.array([gt.PITCH / 2]))[1][0]
    for T in ties:
        for s, cx, cz, fx, fz, state, lo, hi in T:
            if state != 2: continue
            nx, nz = -fz, fx
            qx, qz = X - cx, Z - cz
            al, ac = qx * fx + qz * fz, qx * nx + qz * nz
            edge = np.minimum(gt.SLEEPER_W / 2 - np.abs(al), np.minimum(ac - lo, hi - ac))
            m = edge > 0
            if not m.any(): continue
            h = SL_TOP - np.clip(0.02 - edge, 0, None)
            stain = 0.03 * np.sin(ac * 23.0) * np.cos(al * 31.0)
            put(m, h.astype(np.float32), np.clip(conc[None, None, :] * (1 + stain[..., None]), 0, 1))
    return X, Z, H, col, proj, routes


def finish_layout(L, X, Z, H, col, proj, routes, track, asphalt):
    TH, TC = track
    def put(mask, h, c):
        nonlocal H
        w = mask & (h > H); H = np.where(w, h, H); col[w] = c[w] if np.ndim(c) == 3 else c
    # ---- rails of every route (stock rails, blades, closure rails; the union makes the frogs' crossings)
    for S, LAT, D, IN, _, _ in proj:
        near = IN & (np.abs(np.abs(LAT) - RAIL_X) < 0.08)
        h, c = sample_tile(TH, TC, LAT, np.zeros_like(LAT))   # a crib row: rail without sleeper or clips
        put(near & (h > SL_TOP + 0.003), h, c)
    def box(cx, cz, fx, fz, half_l, half_w, h, c):
        qx, qz = X - cx, Z - cz
        m = (np.abs(qx * fx + qz * fz) < half_l) & (np.abs(-qx * fz + qz * fx) < half_w)
        put(m, np.full(X.shape, h, np.float32), np.array(c, np.float32))
    for fx_, fz_, dx, dz in L["frogs"]: box(fx_, fz_, dx, dz, 0.7, 0.11, -0.012, (0.33, 0.31, 0.29))
    for cx, cz, fx, fz in L["checks"]: box(cx, cz, fx, fz, 1.6, 0.03, -0.012, (0.42, 0.38, 0.34))
    # ---- level crossing deck: flush with the rail head over the road and sidewalks, the road's asphalt, open flangeways
    lv = L["level"]
    if lv:
        rx, rz = lv["r"]; nx, nz = -rz, rx
        dh, g = lv["deckHalf"], L["gauge"]
        deck = (np.abs(Z) <= dh) & (np.abs(X * nx + Z * nz) <= lv["outer"])
        ah, aw = asphalt.shape[:2]
        ac = asphalt[(np.floor(np.mod(Z, 4.0) / 4.0 * ah).astype(int)) % ah, (np.floor(np.mod(X, 4.0) / 4.0 * aw).astype(int)) % aw]
        hz = np.zeros(X.shape, np.float32)
        cz = ac * 0.97
        az = np.abs(Z)
        joint = np.abs(np.mod(X + 0.9, 1.8) - 0.9) < 0.012
        hz = np.where(joint, -0.012, hz); cz = np.where(joint[..., None], cz * 0.55, cz)
        flange = (az >= g / 2 - lv["flange"]) & (az < g / 2)
        hz = np.where(flange, -0.07, hz); cz = np.where(flange[..., None], np.array([0.09, 0.09, 0.09], np.float32), cz)
        head = (az >= g / 2) & (az < g / 2 + HEAD_W)
        rh, rc = sample_tile(TH, TC, Z, np.zeros_like(Z))
        hz = np.where(head, rh, hz); cz = np.where(head[..., None], rc, cz)
        bevel = az > dh - 0.08
        hz = np.where(bevel & ~head, np.minimum(hz, -(az - (dh - 0.08)) * 0.8), hz)
        H = np.where(deck, hz, H); col[deck] = cz[deck]
    return H, col


def encode(H, col):
    hole = H < F + 0.002
    a = np.where(hole, 0, 1 + np.round(254 * np.clip((H - F) / -F, 0, 1))).astype(np.uint8)
    rgb = np.clip(col, 0, 1)
    rgb[hole] = (0.36, 0.31, 0.25)
    Hn = np.where(hole, F, H)
    return (rgb * 255).astype(np.uint8), a, Hn


def normal_map(Hn, du, dv):
    gx = np.gradient(Hn, axis=1) / du; gz = np.gradient(Hn, axis=0) / dv
    n = np.stack([-gx, -gz, np.ones_like(Hn)], -1); n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return ((n * 0.5 + 0.5) * 255).astype(np.uint8)


def props_atlas():
    """The props atlas (rail_junction_mesh.h JMat::Prop: four cells across u: point machine / relay cabinet, gate mast card
    (crossbuck over two red lights, pole, base; transparent around it), gate boom stripes, deck). Same drawing as the
    preview's canvas (docs/rail_junction_spom.js atlas()); RGBA, alpha 0 is cut out by the game's material."""
    from PIL import ImageDraw
    w, h = 512, 256
    q = w // 4
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    g.rectangle([0, 0, q - 1, h - 1], fill=(0x9a, 0xa0, 0x92, 255))
    g.rectangle([10, 10, q - 10, h // 2 - 10], outline=(0x5e, 0x63, 0x59, 255), width=3)
    g.rectangle([10, h // 2 + 10, q - 10, h - 10], outline=(0x5e, 0x63, 0x59, 255), width=3)
    mx = q + q // 2
    g.rectangle([mx - 4, 0, mx + 4, h - 1], fill=(0xd9, 0xd9, 0xd9, 255))
    for a in (0.6, -0.6):  # the crossbuck: two white boards with a black rim, crossed
        for half_l, half_w, col in ((56, 9, (17, 17, 17, 255)), (53, 6, (255, 255, 255, 255))):
            c, s = math.cos(a), math.sin(a)
            pts = [(mx + x * c - y * s, 40 + x * s + y * c) for x, y in ((-half_l, -half_w), (half_l, -half_w), (half_l, half_w), (-half_l, half_w))]
            g.polygon(pts, fill=col)
    g.rectangle([mx - 46, 80, mx + 46, 106], fill=(0x22, 0x22, 0x22, 255))
    for cx in (mx - 28, mx + 28):
        g.ellipse([cx - 10, 83, cx + 10, 103], fill=(0xe0, 0x20, 0x18, 255))
    g.rectangle([mx - 16, h - 40, mx + 16, h - 1], fill=(0x44, 0x44, 0x44, 255))
    for i in range(8):
        g.rectangle([2 * q + i * q // 8, 0, 2 * q + (i + 1) * q // 8 - 1, h - 1], fill=(255, 255, 255, 255) if i % 2 else (0xd0, 0x18, 0x18, 255))
    g.rectangle([3 * q, 0, w - 1, h - 1], fill=(0x4a, 0x4a, 0x4c, 255))
    im.save(os.path.join(OUT, "props_albedo.png"))


def main():
    src = os.path.join(ROOT, "obj", "rail_junction_layouts.json")
    data = json.load(open(src))
    os.makedirs(OUT, exist_ok=True); os.makedirs(DOCS, exist_ok=True)
    props_atlas()
    Hfull, Cfull, TH, TC = track_tile()
    track = (TH, TC)
    ballast = ballast_tile()
    apath = next((p for p in ASPHALT if os.path.exists(p)), None)
    if apath:
        asphalt = np.asarray(Image.open(apath).convert("RGB").resize((200, 200), Image.BOX), np.float32) / 255  # 2 cm texels
    else:
        asphalt = np.full((4, 4, 3), 0.25, np.float32)
    # the plain track tile and the road asphalt, for the preview's plain track / road beside the junctions
    Image.fromarray((np.clip(Cfull, 0, 1) * 255).astype(np.uint8)).save(os.path.join(DOCS, "track_albedo.jpg"), quality=90)
    Image.fromarray((np.clip((Hfull - F) / -F, 0, 1) * 255).astype(np.uint8)).save(os.path.join(DOCS, "track_height.png"))
    if apath: Image.open(apath).convert("RGB").resize((512, 512), Image.BOX).save(os.path.join(DOCS, "asphalt_albedo.jpg"), quality=88)
    index, total = [], {"rgba8": 0, "astc4": 0, "astc6": 0}
    print(f"{'layout':14s} {'size m':>13s} {'texels':>11s} {'RGBA8':>8s} {'ASTC4x4':>8s} {'ASTC6x6':>8s}  (with mips)")
    for L in data["layouts"]:
        X, Z, H, col, proj, routes = bake_layout(L, track, ballast, asphalt)
        H, col = finish_layout(L, X, Z, H, col, proj, routes, track, asphalt)
        rgb, a, Hn = encode(H, col)
        x0, z0, x1, z1 = L["rect"]; nA, nX = L["tex"]
        Image.fromarray(rgb).save(os.path.join(OUT, f"{L['id']}_albedo.jpg"), quality=92)
        Image.fromarray(a).save(os.path.join(OUT, f"{L['id']}_height.png"))
        # no normal map in assets: JunctionPS takes the normal from the height (as TrackPS); the preview keeps one in docs
        Image.fromarray(normal_map(Hn, (z1 - z0) / nX, (x1 - x0) / nA)).save(os.path.join(DOCS, f"{L['id']}_normal.png"))
        Image.fromarray(rgb).save(os.path.join(DOCS, f"{L['id']}_albedo.jpg"), quality=85)
        Image.fromarray(a).save(os.path.join(DOCS, f"{L['id']}_height.png"), optimize=True)
        n = nA * nX
        mem = {"rgba8": n * 4 * 4 / 3, "astc4": n * 4 / 3, "astc6": n * 16 / 36 * 4 / 3}
        for k in total: total[k] += mem[k]
        holes = float((a == 0).mean())
        print(f"{L['id']:14s} {x1 - x0:6.1f}x{z1 - z0:5.1f} {nA:5d}x{nX:<5d} {mem['rgba8'] / 2**20:7.2f}M {mem['astc4'] / 2**20:7.2f}M {mem['astc6'] / 2**20:7.2f}M  holes {holes:.0%}")
        index.append({k: L[k] for k in ("id", "type", "angle", "radius", "rect", "tex")} | {"holes": round(holes, 3)})
    print(f"{'total':14s} {'':13s} {'':11s} {total['rgba8'] / 2**20:7.2f}M {total['astc4'] / 2**20:7.2f}M {total['astc6'] / 2**20:7.2f}M")
    json.dump({"texel": data["texel"], "pitch": data["pitch"], "formation": F, "layouts": index}, open(os.path.join(DOCS, "layouts.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
