"""Tiling ground textures for the specialized-industry work areas (city_industry.inl draws the drawn area with these instead of
flat coloured cells). Each tile covers TILE metres and wraps seamlessly (periodic noise built in the frequency domain).

    python tools/building_gen/industry_ground.py   -> assets/industry/ground/<name>.png
"""
import os
import numpy as np
from PIL import Image

N = 512
TILE = 8.0  # metres per tile (city_industry.inl kGroundTile)
OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'assets', 'industry', 'ground')
rng = np.random.default_rng(21)
yy, xx = np.mgrid[0:N, 0:N] / N  # 0..1 across the tile


def pnoise(scale, seed=None):
    """periodic smooth noise, feature size ~ scale (fraction of the tile), mean 0 std 1."""
    r = np.random.default_rng(seed) if seed is not None else rng
    f = np.fft.fftfreq(N)[:, None] ** 2 + np.fft.fftfreq(N)[None, :] ** 2
    spec = np.exp(-f * (scale * N) ** 2 * 2.0) * (r.normal(size=(N, N)) + 1j * r.normal(size=(N, N)))
    n = np.real(np.fft.ifft2(spec))
    return (n - n.mean()) / (n.std() + 1e-9)


def C(h):
    return np.array([(h >> 16) & 255, (h >> 8) & 255, h & 255], np.float32) / 255


def mix(a, b, t):
    t = np.clip(t, 0, 1)[..., None]
    return a * (1 - t) + b * t


def speckle(img, amount, scale=0.004):
    return img * (1 + amount * pnoise(scale))[..., None]


def save(name, img):
    os.makedirs(OUT, exist_ok=True)
    Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB').save(os.path.join(OUT, name + '.png'))
    print(name)


# grain: ripe wheat in drill rows along u (rows every 0.25 m), stubble and soil showing between, wind-flattened patches
rows = 0.5 + 0.5 * np.cos(yy * 2 * np.pi * (TILE / 0.25))
img = mix(C(0x5E4A33), C(0xC9A65A), 0.25 + 0.75 * rows ** 0.6)
img = mix(img, C(0xD9BE74), 0.35 * (pnoise(0.03) > 0.8))
img *= (1 + 0.08 * pnoise(0.12))[..., None]
img = mix(img, C(0xA88848), 0.25 * np.clip(pnoise(0.08), 0, 1))
save('grain', speckle(img, 0.12))

# pasture: short grazed grass, clover patches, trodden dirt paths and dung-dark spots
img = mix(C(0x5C7A3A), C(0x7D9A4C), 0.5 + 0.35 * pnoise(0.05))
img = mix(img, C(0x4E6A32), 0.5 * np.clip(pnoise(0.015), 0, 1))
img = mix(img, C(0x7A6A4A), np.clip(pnoise(0.09, 4) * 1.6 - 1.8, 0, 0.85))
img = mix(img, C(0x3A3426), (pnoise(0.006, 5) > 2.6) * 0.8)
save('pasture', speckle(img, 0.14))

# forest floor: needle litter, moss, bare soil, twigs
img = mix(C(0x5A4430), C(0x7A5E3E), 0.5 + 0.3 * pnoise(0.02))
img = mix(img, C(0x4E5A30), np.clip(pnoise(0.07, 7) - 0.4, 0, 1) * 0.8)
tw = np.abs(np.sin((xx * 37 + yy * 13 + pnoise(0.05, 8) * 0.3) * 2 * np.pi)) < 0.02
img = mix(img, C(0x9A8466), tw * 0.6)
save('forest', speckle(img, 0.2))

# oil lease: compacted caliche with tyre tracks and oily stains
img = mix(C(0xBFA77F), C(0xD8C49A), 0.5 + 0.3 * pnoise(0.04))
tracks = (np.abs(np.sin(yy * 2 * np.pi * 2 + pnoise(0.2, 9) * 0.4)) > 0.96)
img = mix(img, C(0x9A8662), tracks * 0.6)
img = mix(img, C(0x3A342C), np.clip(pnoise(0.05, 10) * 1.5 - 2.4, 0, 0.7))
save('oil', speckle(img, 0.1))

# ore pit: broken rock and fines, iron-red banding, scattered cobbles
img = mix(C(0x6E4A3A), C(0x9A6E52), 0.5 + 0.35 * pnoise(0.03))
img = mix(img, C(0x8A8278), np.clip(pnoise(0.1, 11), 0, 1) * 0.5)
cob = pnoise(0.006, 12) > 1.6
img = mix(img, C(0x55453C), cob * 0.7)
img = mix(img, C(0xB8957A), (pnoise(0.006, 13) > 2.0) * 0.6)
save('ore', speckle(img, 0.18))

# landfill working face: mixed refuse (bags, paper, plastic, scrap), dark matrix
img = mix(C(0x4A4440), C(0x6A625A), 0.5 + 0.3 * pnoise(0.02))
for c_, th, s in ((0xE8E6E0, 1.7, 14), (0x3A5A8A, 2.0, 15), (0x2A2A2A, 1.4, 16), (0xC8A050, 2.1, 17), (0x8A3A30, 2.2, 18), (0x5A8A4A, 2.2, 19)):
    img = mix(img, C(c_), (pnoise(0.008, s) > th) * 0.85)
save('refuse', speckle(img, 0.15))

# daily cover soil: dozed earth with blade marks
img = mix(C(0x6E5638), C(0x8A6E48), 0.5 + 0.3 * pnoise(0.03))
img *= (1 + 0.06 * np.sin(yy * 2 * np.pi * 10 + pnoise(0.1, 20)))[..., None]
save('cover', speckle(img, 0.12))

# HDPE liner: black geomembrane in 7 m panels with welded seams, dust in the creases
img = np.broadcast_to(C(0x1C1D1F), (N, N, 3)).copy() * (1 + 0.05 * pnoise(0.05))[..., None]
seam = (np.abs(((xx * TILE) % 7.0) - 0.1) < 0.06) | (np.abs(yy - 0.5) < 0.004)
img = mix(img, C(0x3A3B3D), seam * 0.9)
img = mix(img, C(0x6A6050), np.clip(pnoise(0.04, 21) - 1.2, 0, 1) * 0.5)
save('liner', img)

# capped and grassed: rougher than pasture, with gas-well paths
img = mix(C(0x5E7238), C(0x7A8A48), 0.5 + 0.3 * pnoise(0.05, 22))
img = mix(img, C(0x8A7A52), np.clip(pnoise(0.12, 23) - 1.0, 0, 0.6))
save('cap', speckle(img, 0.14))
