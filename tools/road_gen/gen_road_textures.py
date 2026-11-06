"""Procedural, seamlessly tiling road textures (albedo + normal), no image generation.

Writes assets/roads/tex/<name>_albedo.png and <name>_normal.png. Each texture tiles; the real-world size it covers
is in TILE_METERS (the preview and later the engine use it for UVs).
Run: python tools/road_gen/gen_road_textures.py
"""
import os
import numpy as np
from PIL import Image, ImageDraw

N = 1024
OUT = os.path.join(os.path.dirname(__file__), "..", "..", "assets", "roads", "tex")
rng = np.random.default_rng(7)

TILE_METERS = {"asphalt": 4, "asphalt_worn": 4, "gravel": 3, "concrete": 3, "curb": 2, "pavers": 2,
               "grass": 4, "bike_asphalt": 4}


def fractal(beta, seed, n=N):
    """Periodic noise: white noise shaped to a 1/f^beta spectrum, so it tiles exactly. Normalized to 0..1."""
    r = np.random.default_rng(seed)
    f = np.fft.fft2(r.standard_normal((n, n)))
    fy = np.fft.fftfreq(n)[:, None]
    fx = np.fft.fftfreq(n)[None, :]
    k = np.sqrt(fx * fx + fy * fy)
    k[0, 0] = 1
    v = np.real(np.fft.ifft2(f / k ** beta))
    return (v - v.min()) / (v.max() - v.min())


def blur(a, px):
    """Periodic gaussian blur via FFT (keeps tiling)."""
    n = a.shape[0]
    fy = np.fft.fftfreq(n)[:, None]
    fx = np.fft.fftfreq(n)[None, :]
    g = np.exp(-2 * (np.pi * px) ** 2 * (fx * fx + fy * fy))
    return np.real(np.fft.ifft2(np.fft.fft2(a) * g))


def normal_from_height(h, strength):
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * strength
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * strength
    n = np.stack([-dx, -dy, np.ones_like(h)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def save(name, rgb, height, strength):
    os.makedirs(OUT, exist_ok=True)
    Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(os.path.join(OUT, f"{name}_albedo.png"))
    Image.fromarray((normal_from_height(height, strength) * 255).astype(np.uint8)).save(os.path.join(OUT, f"{name}_normal.png"))
    print("wrote", name)


def tint(gray, color):
    return gray[..., None] * np.array(color)[None, None, :]


def stones(count, rmin, rmax, seed):
    """Height map of round stones scattered with wraparound."""
    r = np.random.default_rng(seed)
    img = Image.new("F", (N, N), 0)
    d = ImageDraw.Draw(img)
    vals = []
    for _ in range(count):
        x, y = r.uniform(0, N, 2)
        rad = r.uniform(rmin, rmax)
        v = r.uniform(0.4, 1.0)
        vals.append(v)
        for ox in (-N, 0, N):
            for oy in (-N, 0, N):
                d.ellipse([x + ox - rad, y + oy - rad, x + ox + rad, y + oy + rad], fill=v)
    return blur(np.array(img), 1.2)


def cracks(count, seed, length=400):
    r = np.random.default_rng(seed)
    img = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(img)
    for _ in range(count):
        x, y = r.uniform(0, N, 2)
        ang = r.uniform(0, 2 * np.pi)
        pts = [(x, y)]
        for _ in range(length // 8):
            ang += r.normal(0, 0.35)
            x += 8 * np.cos(ang)
            y += 8 * np.sin(ang)
            pts.append((x, y))
        for ox in (-N, 0, N):
            for oy in (-N, 0, N):
                d.line([(px + ox, py + oy) for px, py in pts], fill=255, width=2)
    return blur(np.array(img, dtype=np.float64) / 255, 0.8)


# ---------------------------------------------------------------- asphalt
def asphalt(seed, base, worn):
    low = fractal(1.6, seed)                     # large blotches (repairs, stains)
    mid = fractal(1.0, seed + 1)
    grain = rng.random((N, N))
    agg = stones(9000, 1.0, 3.2, seed + 2)        # aggregate stones
    g = base + 0.06 * (low - 0.5) + 0.04 * (mid - 0.5) + 0.05 * (grain - 0.5) + 0.10 * agg
    h = 0.6 * agg + 0.25 * grain + 0.15 * mid
    if worn:
        c = cracks(14, seed + 3)
        patch = blur((fractal(1.8, seed + 4) > 0.72).astype(float), 3)  # darker patched areas
        g = g + 0.08 * (fractal(1.2, seed + 5) - 0.5) - 0.06 * patch - 0.10 * c
        h = h - 0.8 * c
    return g, h


g, h = asphalt(10, 0.20, False)
save("asphalt", tint(g, (1.0, 1.0, 1.03)), h, 3.0)
g, h = asphalt(20, 0.27, True)
save("asphalt_worn", tint(g, (1.03, 1.0, 0.97)), h, 3.0)
g, h = asphalt(30, 0.22, False)
save("bike_asphalt", tint(g, (1.9, 0.85, 0.75)), h, 3.0)  # red-tinted surface course

# ---------------------------------------------------------------- gravel
big = stones(5000, 4, 11, 40)
small = stones(16000, 1.5, 4, 41)
dirt = fractal(1.4, 42)
hgt = np.maximum(big, 0.7 * small)
gv = 0.32 + 0.25 * hgt + 0.08 * (dirt - 0.5) + 0.05 * (rng.random((N, N)) - 0.5)
col = tint(gv, (1.05, 0.97, 0.86))
col = col * (1 - 0.25 * (hgt < 0.05)[..., None]) + np.array([0.30, 0.25, 0.19]) * 0.25 * (hgt < 0.05)[..., None]
save("gravel", col, hgt, 5.0)


# ---------------------------------------------------------------- concrete (sidewalk slabs, 1.5 m joints)
def concrete(seed, base, joints_per_tile):
    low = fractal(1.5, seed)
    grain = blur(rng.random((N, N)), 0.6)
    g = base + 0.06 * (low - 0.5) + 0.06 * (grain - 0.5)
    h = 0.4 * grain + 0.2 * low
    if joints_per_tile:
        j = np.zeros((N, N))
        step = N // joints_per_tile
        for k in range(joints_per_tile):
            j[:, k * step:k * step + 3] = 1
            j[k * step:k * step + 3, :] = 1
        j = blur(j, 1.0)
        g = g - 0.18 * j
        h = h - 1.5 * j
    return g, h


g, h = concrete(50, 0.62, 2)
save("concrete", tint(g, (1.0, 0.99, 0.96)), h, 2.0)
g, h = concrete(60, 0.70, 0)
save("curb", tint(g, (1.0, 1.0, 0.98)), h, 1.5)

# ---------------------------------------------------------------- pavers (pedestrian street, running bond 20x10 cm)
img = np.zeros((N, N))
colv = np.zeros((N, N))
bw, bh = N // 10, N // 20  # 2 m tile: 20 cm x 10 cm bricks
r2 = np.random.default_rng(70)
for row in range(20):
    off = (bw // 2) * (row % 2)
    for c in range(11):
        x0 = (c * bw + off) % N
        v = r2.uniform(-0.08, 0.08)
        xs = (np.arange(x0 + 3, x0 + bw - 3)) % N
        img[row * bh + 3:row * bh + bh - 3][:, xs] = 1
        colv[row * bh + 3:row * bh + bh - 3][:, xs] = v
img = blur(img, 1.3)
grain = blur(rng.random((N, N)), 0.7)
g = 0.12 + img * (0.42 + colv + 0.08 * (grain - 0.5)) + 0.05 * (fractal(1.5, 71) - 0.5)
save("pavers", tint(g, (1.15, 0.92, 0.80)), img + 0.2 * grain, 4.0)

# ---------------------------------------------------------------- grass (medians, verges)
low = fractal(1.5, 80)
blades = blur(rng.random((N, N)), 0.5)
g = 0.5 + 0.25 * (low - 0.5) + 0.35 * (blades - 0.5)
col = np.stack([0.25 * g + 0.05 * low, 0.42 * g, 0.16 * g], -1)
save("grass", col, blades, 2.0)

print("tile sizes (m):", TILE_METERS)
