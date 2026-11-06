"""Railway track: a detailed 3D track model baked into a silhouette-POM height field, carried by a 10-triangle box.

Detail model (analytic, evaluated per texel, tileable along the track every sleeper pitch):
  UIC60 rails (crowned head with gauge-corner radius, web hidden under the head, sloped foot) at 1435 mm gauge,
  rubber rail pads, concrete monoblock sleepers (chamfered, tapered centre, lifting inserts), cast shoulder housings and
  e-type spring clips on both sides of each rail foot, and individually placed ballast stones on a 1:1.5 shoulder over
  the formation.
The top-down height of all that (one solid column per texel, so a height field) plus its colour and normal is baked into a
tile 4.4 m across x 0.60 m along. The mesh is a plain box per track piece (top, two sides, two ends); the shader marches the
eye ray through the box against the height field and discards where it misses, so the rails, sleepers and ballast shoulder
make the outline, not the box. Rail cross-section stays readable at grazing angles because the box sides march too.

Heights: y = 0 is the top of the rail (the train GLBs stand on y = 0); the box bottom is FORMATION = -0.55 m.
Output: assets/rail/track/tex/track_{albedo.jpg, normal.png, height.png (8-bit), height16.png}, assets/rail/track/glb/TRACK_*.glb
Run:  python gen_track.py
"""
import io, json, math, os, struct
import numpy as np
from PIL import Image
import gen_cars as gc

ROOT = os.path.join(gc.ROOT, "assets", "rail", "track")
TEXD, GLBD = os.path.join(ROOT, "tex"), os.path.join(ROOT, "glb")
WIDTH, PITCH, FORMATION = 4.4, 0.60, -0.55          # tile across (m), sleeper pitch along (m), box bottom (m)
NU, NV = 1792, 256                                  # texels across / along (~2.4 mm)
GAUGE, HEAD_W, RAIL_H = 1.435, 0.072, 0.172
RAIL_X = GAUGE / 2 + HEAD_W / 2                    # rail centre lines
SLEEPER_TOP, CRIB, SLEEPER_HALF, SLEEPER_W = -0.188, -0.232, 1.30, 0.25


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t)


def bake():
    rng = np.random.default_rng(1435)
    x = (np.arange(NU) + 0.5) / NU * WIDTH - WIDTH / 2         # across, metres
    z = (np.arange(NV) + 0.5) / NV * PITCH                      # along, metres; sleeper centred at PITCH / 2
    X, Z = np.meshgrid(x, z)                                    # [NV, NU]
    ax, dz = np.abs(X), np.abs(Z - PITCH / 2)
    H = np.full(X.shape, FORMATION, np.float32)
    col = np.zeros(X.shape + (3,), np.float32); col[:] = (0.36, 0.31, 0.25)        # formation: dirt
    mat = np.zeros(X.shape, np.int8)                                                # 0 dirt 1 ballast 2 sleeper 3 pad 4 clip 5 rail
    def put(mask, h, c, m):
        nonlocal H
        w = mask & (h > H); H = np.where(w, h, H); col[w] = c if np.ndim(c) == 1 else c[w]; mat[w] = m
    # ---- ballast bed: crib level between sleepers, shoulder 1:1.5 down to the formation
    shoulder = 1.65
    bed = np.where(ax < shoulder, CRIB, CRIB - (ax - shoulder) / 1.5)
    put(bed > FORMATION, bed, np.array([0.42, 0.41, 0.40]), 1)
    # ---- ballast stones: angular-ish ellipsoids on the bed, wrapped along the track so the tile repeats
    n = 2600
    sx, sz = rng.uniform(-2.2, 2.2, n), rng.uniform(0, PITCH, n)
    rad = rng.uniform(0.018, 0.034, n); asp = rng.uniform(0.7, 1.3, n); rot = rng.uniform(0, math.pi, n)
    tone = rng.uniform(0.30, 0.62, n); warm = rng.uniform(-0.03, 0.04, n)
    for i in range(n):
        base = CRIB if abs(sx[i]) < shoulder else CRIB - (abs(sx[i]) - shoulder) / 1.5
        if base < FORMATION + 0.01: continue
        r = rad[i] * 1.6; i0, i1 = np.searchsorted(x, sx[i] - r), np.searchsorted(x, sx[i] + r)
        if i1 <= i0: continue
        for off in (-PITCH, 0.0, PITCH):
            zc = sz[i] + off
            if zc + r < 0 or zc - r > PITCH: continue
            j0, j1 = np.searchsorted(z, zc - r), np.searchsorted(z, zc + r)
            if j1 <= j0: continue
            px, pz = X[j0:j1, i0:i1] - sx[i], Z[j0:j1, i0:i1] - zc
            c, s = math.cos(rot[i]), math.sin(rot[i])
            u, v = (px * c + pz * s) / (rad[i] * asp[i]), (-px * s + pz * c) / (rad[i] / asp[i])
            d2 = u * u + v * v
            facet = 1 - 0.18 * np.abs(np.sin(3 * np.arctan2(v, u) + rot[i] * 7))     # flat faces, sharp-ish edges
            h = base - rad[i] * 0.35 + rad[i] * 0.85 * np.sqrt(np.clip(1 - d2, 0, 1)) * facet
            m = d2 < 1
            sub = H[j0:j1, i0:i1]; w = m & (h > sub)
            sub[w] = h[w]
            shade = tone[i] * (0.85 + 0.15 * np.sqrt(np.clip(1 - d2, 0, 1)))
            col[j0:j1, i0:i1][w] = np.stack([shade + warm[i], shade + warm[i] * 0.5, shade - warm[i] * 0.3], -1)[w]
            mat[j0:j1, i0:i1][w] = 1
    # ---- concrete sleeper: chamfered block, centre section 12 mm lower, lifting inserts, weathering toward the ends
    chamfer = 0.02
    edge = np.minimum(SLEEPER_W / 2 - dz, SLEEPER_HALF - ax)
    hs = SLEEPER_TOP - 0.012 * (1 - smooth(0.35, 0.55, ax)) - np.clip(chamfer - edge, 0, None)
    stain = 0.04 * np.sin(X * 23.0) * np.cos(Z * 31.0)
    concrete = np.stack([0.66 + stain, 0.65 + stain, 0.62 + stain], -1) * (1 - 0.12 * smooth(1.0, 1.3, ax))[..., None]
    put(edge > 0, hs, concrete, 2)
    for xc in (-0.42, 0.42):                                    # lifting inserts (recessed rings)
        rr = np.hypot(X - xc, Z - PITCH / 2); ring = (rr < 0.018)
        H = np.where(ring & (mat == 2), H - 0.006, H); col[ring & (mat == 2)] = (0.35, 0.34, 0.33)
    # ---- per rail: pad, shoulder housings + clips, then the rail itself
    for s in (-1, 1):
        xr = s * RAIL_X; dx = X - xr; adx = np.abs(dx)
        on_sl = dz < SLEEPER_W / 2 - 0.02
        put(on_sl & (adx < 0.090), np.full(X.shape, SLEEPER_TOP + 0.010, np.float32), np.array([0.07, 0.07, 0.07]), 3)   # rail pad
        for side in (-1, 1):                                    # cast shoulders and e-clips either side of the foot
            cx = xr + side * 0.112
            box = on_sl & (np.abs(X - cx) < 0.030) & (dz < 0.055)
            put(box, np.full(X.shape, SLEEPER_TOP + 0.045, np.float32), np.array([0.30, 0.29, 0.28]), 4)
            # e-clip: a bent bar seen from above = toe on the rail foot, loop round the shoulder, leg into the housing
            t = np.linspace(0, 1, 60)
            path = np.stack([cx - side * (0.075 - 0.10 * t + 0.045 * np.sin(math.pi * t)) * 1.0,
                             PITCH / 2 + 0.040 * np.cos(math.pi * t)], -1)
            hgt = SLEEPER_TOP + 0.030 + 0.030 * np.sin(math.pi * t)
            for (px, pz), hh in zip(path, hgt):
                m = (X - px) ** 2 + (Z - pz) ** 2 < 0.0095 ** 2
                bump = hh + np.sqrt(np.clip(0.0095 ** 2 - (X - px) ** 2 - (Z - pz) ** 2, 0, None))
                put(m, bump.astype(np.float32), np.array([0.13, 0.13, 0.14]), 4)
        # rail foot (150 mm, sloped top), head (72 mm, crowned with gauge-corner radius); web is under the head
        foot = adx < 0.075
        hf = -RAIL_H + 0.012 + 0.010 * (1 - adx / 0.075)
        put(foot, hf.astype(np.float32), np.array([0.33, 0.21, 0.13]), 5)
        head = adx < HEAD_W / 2
        crown = -0.0035 * (dx / (HEAD_W / 2)) ** 2 - 0.010 * smooth(0.024, 0.036, adx)
        shiny = np.clip(1 - adx / 0.024, 0, 1)                  # wheel running band: bright steel, rusty flanks
        rc = np.stack([0.30 + 0.48 * shiny, 0.22 + 0.52 * shiny, 0.16 + 0.58 * shiny], -1)
        put(head, crown.astype(np.float32), rc, 5)
    # ---- grime: oil drip line in the four-foot, dust over everything low
    four_foot = np.exp(-(X / 0.30) ** 2) * 0.25
    col *= (1 - four_foot * (mat <= 2))[..., None]
    col = np.clip(col, 0, 1)
    return X, Z, H, col, mat


def write_maps(H, col):
    os.makedirs(TEXD, exist_ok=True)
    rgb = (col * 255).astype(np.uint8)
    Image.fromarray(rgb).save(os.path.join(TEXD, "track_albedo.jpg"), quality=92)
    hn = (H - FORMATION) / -FORMATION                            # 0 at the formation, 1 at the rail head
    Image.fromarray((np.clip(hn, 0, 1) * 255).astype(np.uint8)).save(os.path.join(TEXD, "track_height.png"))
    Image.fromarray((np.clip(hn, 0, 1) * 65535).astype(np.uint16)).save(os.path.join(TEXD, "track_height16.png"))
    # normal in the tile frame: x across (u), y along (v), z up; metres per texel from the tile size
    gx = np.gradient(H, axis=1) / (WIDTH / NU); gz = np.gradient(H, axis=0) / (PITCH / NV)
    n = np.stack([-gx, -gz, np.ones_like(H)], -1); n /= np.linalg.norm(n, axis=-1, keepdims=True)
    Image.fromarray(((n * 0.5 + 0.5) * 255).astype(np.uint8)).save(os.path.join(TEXD, "track_normal.png"))


def track_mesh(length):
    """Box per piece: top at the rail head, sides down to the formation, ends. uv = (across 0..1, along in sleeper pitches)."""
    hw, y0, y1, L = WIDTH / 2, FORMATION, 0.0, length
    V = PITCH
    quads = [  # positions, uvs, normal
        ([(-hw, y1, 0), (hw, y1, 0), (hw, y1, L), (-hw, y1, L)], [(0, 0), (1, 0), (1, L / V), (0, L / V)], (0, 1, 0)),
        ([(hw, y0, 0), (hw, y0, L), (hw, y1, L), (hw, y1, 0)], [(1, 0), (1, L / V), (1, L / V), (1, 0)], (1, 0, 0)),
        ([(-hw, y0, L), (-hw, y0, 0), (-hw, y1, 0), (-hw, y1, L)], [(0, L / V), (0, 0), (0, 0), (0, L / V)], (-1, 0, 0)),
        ([(hw, y0, L), (-hw, y0, L), (-hw, y1, L), (hw, y1, L)], [(1, L / V), (0, L / V), (0, L / V), (1, L / V)], (0, 0, 1)),
        ([(-hw, y0, 0), (hw, y0, 0), (hw, y1, 0), (-hw, y1, 0)], [(0, 0), (1, 0), (1, 0), (0, 0)], (0, 0, -1)),
    ]
    pos, nrm, uv, idx = [], [], [], []
    for P, U, N in quads:
        b = len(pos); pos += P; uv += U; nrm += [N] * 4
        a, c = np.array(P[1]) - np.array(P[0]), np.array(P[2]) - np.array(P[0])
        tri = [(0, 1, 2), (0, 2, 3)] if np.dot(np.cross(a, c), N) > 0 else [(0, 2, 1), (0, 3, 2)]
        for t in tri: idx += [b + k for k in t]
    return np.array(pos, np.float32), np.array(nrm, np.float32), np.array(uv, np.float32), np.array(idx, np.uint16)


def write_glb(path, length):
    pos, nrm, uv, idx = track_mesh(length)
    files = {k: open(os.path.join(TEXD, f"track_{k}"), "rb").read() for k in ("albedo.jpg", "normal.png", "height.png")}
    bin_ = bytearray(); views = []
    def view(data, target=None):
        while len(bin_) % 4: bin_.append(0)
        v = {"buffer": 0, "byteOffset": len(bin_), "byteLength": len(data)}; bin_.extend(data)
        if target: v["target"] = target
        views.append(v); return len(views) - 1
    accs = [{"bufferView": view(pos.tobytes(), 34962), "componentType": 5126, "count": len(pos), "type": "VEC3", "min": pos.min(0).tolist(), "max": pos.max(0).tolist()},
            {"bufferView": view(nrm.tobytes(), 34962), "componentType": 5126, "count": len(nrm), "type": "VEC3"},
            {"bufferView": view(uv.tobytes(), 34962), "componentType": 5126, "count": len(uv), "type": "VEC2"},
            {"bufferView": view(idx.tobytes(), 34963), "componentType": 5123, "count": len(idx), "type": "SCALAR"}]
    images = [{"bufferView": view(files["albedo.jpg"]), "mimeType": "image/jpeg"}, {"bufferView": view(files["normal.png"]), "mimeType": "image/png"},
              {"bufferView": view(files["height.png"]), "mimeType": "image/png"}]
    g = {"asset": {"version": "2.0", "generator": "cityengine gen_track"}, "scene": 0, "scenes": [{"nodes": [0]}],
         "nodes": [{"mesh": 0, "name": os.path.splitext(os.path.basename(path))[0]}],
         "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
         "materials": [{"name": "track_spom", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.85},
                        "normalTexture": {"index": 1}, "occlusionTexture": {"index": 2},
                        "extras": {"spomHeightField": {"heightTexture": 2, "bottom": FORMATION, "top": 0.0, "tileAcross": WIDTH, "tileAlong": PITCH}}}],
         "textures": [{"source": 0}, {"source": 1}, {"source": 2}], "images": images, "accessors": accs, "bufferViews": views,
         "buffers": [{"byteLength": len(bin_)}]}
    j = json.dumps(g, separators=(",", ":")).encode(); j += b" " * (-len(j) % 4)
    while len(bin_) % 4: bin_.append(0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 28 + len(j) + len(bin_)))
        f.write(struct.pack("<II", len(j), 0x4E4F534A)); f.write(j)
        f.write(struct.pack("<II", len(bin_), 0x004E4942)); f.write(bytes(bin_))


def main():
    X, Z, H, col, mat = bake()
    write_maps(H, col)
    os.makedirs(GLBD, exist_ok=True)
    for L in (6.0, 30.0, 60.0):   # whole sleeper pitches so pieces butt without a seam
        write_glb(os.path.join(GLBD, f"TRACK_straight_{int(L)}m.glb"), L)
    print(f"track tile {WIDTH} x {PITCH} m, {NU}x{NV} texels, heights {H.min():.3f}..{H.max():.3f} m; meshes 10 tris each -> {GLBD}")


if __name__ == "__main__":
    main()
