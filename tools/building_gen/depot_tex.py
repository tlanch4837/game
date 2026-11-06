"""Relief textures for the rail depot's SPOM slabs (SlabPS in src/shaders/mesh.hlsl).

Each texture covers one face: u across one repeat ("period", metres along the face), v the face's full extent (wall height, or
roof depth), top row = top of a wall / the b=0 edge of a roof. RGB is the albedo, A the relief height out of the face as a fraction
of the slab depth (kSlabDepth, 0.8 m). Ambient occlusion from the height field is baked into the albedo.

    python tools/building_gen/depot_tex.py      -> assets/rail/depot/tex/*.png
"""
import os
import numpy as np
from PIL import Image

PX = 32  # pixels per metre
OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'assets', 'rail', 'depot', 'tex')
rng = np.random.default_rng(7)


def hexc(c):
    return np.array([(c >> 16) & 255, (c >> 8) & 255, c & 255], np.float32) / 255.0


class Face:
    def __init__(self, w, h):  # metres
        self.W, self.H = int(round(w * PX)), int(round(h * PX))
        self.col = np.zeros((self.H, self.W, 3), np.float32)
        self.hgt = np.zeros((self.H, self.W), np.float32)

    def rect(self, x0, y0, x1, y1, c, h, noise=0.0, top=False):
        """metres; y measured up from the bottom (walls) unless top=True (from the top row: roofs)."""
        if top:
            r0, r1 = int(round(y0 * PX)), int(round(y1 * PX))
        else:
            r0, r1 = self.H - int(round(y1 * PX)), self.H - int(round(y0 * PX))
        c0, c1 = int(round(x0 * PX)), int(round(x1 * PX))
        r0, r1, c0, c1 = max(r0, 0), min(r1, self.H), max(c0, 0), min(c1, self.W)
        if r1 <= r0 or c1 <= c0:
            return
        col = hexc(c) if isinstance(c, int) else np.array(c, np.float32)
        blk = np.broadcast_to(col, (r1 - r0, c1 - c0, 3)).copy()
        if noise:
            blk *= 1.0 + noise * rng.standard_normal((r1 - r0, c1 - c0, 1)).astype(np.float32)
        self.col[r0:r1, c0:c1] = blk
        self.hgt[r0:r1, c0:c1] = h

    def grain(self, amount):
        self.col *= 1.0 + amount * rng.standard_normal((self.H, self.W, 1)).astype(np.float32)

    def save(self, name):
        h = self.hgt
        # baked ambient occlusion: how far a texel sits under its blurred neighbourhood, wrapped across u
        acc, n = np.zeros_like(h), 0
        for r in (2, 5, 10):
            for dy, dx in ((r, 0), (-r, 0), (0, r), (0, -r), (r, r), (-r, -r), (r, -r), (-r, r)):
                acc += np.roll(np.roll(h, dy, 0), dx, 1)
                n += 1
        ao = np.clip(1.0 - np.maximum(acc / n - h, 0) * 1.6, 0.45, 1.0)
        rgb = np.clip(self.col * ao[..., None], 0, 1)
        img = np.dstack([rgb, np.clip(h, 0, 1)[..., None]])
        os.makedirs(OUT, exist_ok=True)
        Image.fromarray((img * 255 + 0.5).astype(np.uint8), 'RGBA').save(os.path.join(OUT, name + '.png'))
        print(name, self.W, 'x', self.H)


WALL, BRICK, MORTAR, PIL, CORNICE, GLASS, FRAME = 0xC9C4B8, 0x8F4F3A, 0x6E6A62, 0xA9A397, 0x8A8E93, 0x23313D, 0x5C6268


def brick_band(f, y0, y1, h=0.36):
    f.rect(0, y0, f.W / PX, y1, MORTAR, h - 0.05)
    bw, bh = 0.5, 0.2
    row = 0
    y = y0
    while y < y1 - 0.01:
        off = (row % 2) * bw * 0.5
        x = -off
        while x < f.W / PX:
            tint = 1.0 + 0.12 * rng.standard_normal()
            f.rect(x + 0.02, y + 0.02, x + bw - 0.02, min(y + bh - 0.02, y1), tuple(hexc(BRICK) * tint), h, 0.04)
            x += bw
        y += bh
        row += 1


def window(f, x0, y0, x1, y1, bars=1.0):
    f.rect(x0 - 0.12, y0 - 0.12, x1 + 0.12, y1 + 0.12, FRAME, 0.32)
    f.rect(x0, y0, x1, y1, GLASS, 0.04)
    # sky reflection: lighter towards the top of each pane
    r0, r1 = f.H - int(round(y1 * PX)), f.H - int(round(y0 * PX))
    c0, c1 = int(round(x0 * PX)), int(round(x1 * PX))
    g = np.linspace(1.6, 0.8, r1 - r0, dtype=np.float32)[:, None, None]
    f.col[r0:r1, c0:c1] *= g
    x = x0 + bars
    while x < x1 - 0.05:
        f.rect(x - 0.04, y0, x + 0.04, y1, FRAME, 0.22)
        x += bars
    f.rect(x0, (y0 + y1) / 2 - 0.04, x1, (y0 + y1) / 2 + 0.04, FRAME, 0.22)
    f.rect(x0 - 0.2, y0 - 0.2, x1 + 0.2, y0 - 0.08, 0xB9B4AA, 0.48)  # sill


def cornice(f, H):
    f.rect(0, H - 0.6, f.W / PX, H, CORNICE, 0.55, 0.03)
    f.rect(0, H - 0.68, f.W / PX, H - 0.6, 0x6F7378, 0.42)


# ---- shed side wall: one 12 m bay, 10 m high (pilaster at the bay edge, a long window band, brick plinth)
f = Face(12, 10)
f.rect(0, 0, 12, 10, WALL, 0.22, 0.02)
for x in np.arange(0, 12, 1.0):  # cladding panel joints
    f.rect(x - 0.015, 1.2, x + 0.015, 9.4, 0xB3AEA2, 0.18)
brick_band(f, 0, 1.2)
f.rect(0, 1.2, 12, 1.32, 0xB9B4AA, 0.42)  # plinth cap
window(f, 1.6, 4.3, 10.4, 6.7, 1.1)
for x0 in (0.0, 11.6):  # pilasters (half on each bay edge so the repeat closes)
    f.rect(x0, 0, x0 + 0.4, 9.4, PIL, 0.62, 0.02)
f.rect(0.55, 0.0, 0.67, 9.3, 0x7A7F85, 0.75)  # downpipe
for y in np.arange(0.5, 9.3, 1.6):
    f.rect(0.5, y, 0.72, y + 0.06, 0x6A6F75, 0.8)
f.rect(2.0, 7.4, 4.4, 8.4, 0x2E3238, 0.3)  # louvre vent
for y in np.arange(7.5, 8.35, 0.12):
    f.rect(2.05, y, 4.35, y + 0.05, 0x8A9096, 0.36)
cornice(f, 10)
f.grain(0.03)
f.save('depot_wall')

# ---- shed end wall: the whole 32 m gable end, 10 m high, five track portals (5 m apart, 4.4 m wide, 6.6 m high)
f = Face(32, 10)
f.rect(0, 0, 32, 10, WALL, 0.22, 0.02)
brick_band(f, 0, 1.2)
for c in (6, 11, 16, 21, 26):
    a, b = c - 2.2, c + 2.2
    f.rect(a - 0.35, 0, b + 0.35, 6.95, 0x9DA1A6, 0.5)            # steel portal frame
    f.rect(a, 0, b, 6.6, 0x101214, 0.0)                              # open door: the dark shed beyond
    for k in range(6):                                               # rolled-up door slats in the head
        f.rect(a, 6.0 + k * 0.1, b, 6.05 + k * 0.1, 0x3A3F45, 0.12)
    for e in (a - 0.3, b + 0.05):                                    # red/white door guides
        for k in range(14):
            f.rect(e, k * 0.47, e + 0.25, k * 0.47 + 0.47, 0xD23B2B if k % 2 else 0xEEEEEE, 0.62)
    f.rect(c - 0.45, 7.25, c + 0.45, 7.65, 0xF2F2EE, 0.4)            # road number plate
    f.rect(c - 0.3, 7.33, c + 0.3, 7.57, 0x1F4F8F, 0.42)
f.rect(0, 6.95, 32, 7.15, 0xD8A928, 0.6)                             # yellow clearance strip
f.rect(1.0, 7.6, 3.2, 9.0, 0x2E3238, 0.3)                            # vents
f.rect(28.8, 7.6, 31.0, 9.0, 0x2E3238, 0.3)
cornice(f, 10)
f.grain(0.03)
f.save('depot_end')

# ---- shed roof: one 19 m bay along the shed, the full 34.5 m across; parapets on both long edges, a skylight either side
f = Face(19, 34.5)
f.rect(0, 0, 19, 34.5, 0x8C9094, 0.04, 0.03, top=True)
for x in np.arange(0, 19, 1.0):  # membrane seams
    f.rect(x - 0.02, 0, x + 0.02, 34.5, 0x7E8287, 0.06, top=True)
for y0 in (0.0, 33.9):
    f.rect(0, y0, 19, y0 + 0.6, 0x6C7075, 0.95, top=True)
    f.rect(0, y0 + (0.55 if y0 == 0 else -0.05), 19, y0 + (0.65 if y0 == 0 else 0.05), 0x55595E, 0.7, top=True)
for yc in (10.25, 24.25):
    f.rect(3.0, yc - 4.0, 16.0, yc + 4.0, 0xB7BBBF, 0.42, top=True)    # upstand
    f.rect(3.4, yc - 3.6, 15.6, yc + 3.6, GLASS, 0.3, top=True)          # glazing
    r0, r1 = int((yc - 3.6) * PX), int((yc + 3.6) * PX)
    f.col[r0:r1, int(3.4 * PX):int(15.6 * PX)] *= np.linspace(0.8, 1.5, int(15.6 * PX) - int(3.4 * PX), dtype=np.float32)[None, :, None]
    for x in np.arange(4.9, 15.5, 1.5):
        f.rect(x - 0.05, yc - 3.6, x + 0.05, yc + 3.6, 0xA0A5AA, 0.38, top=True)
    f.rect(3.4, yc - 0.05, 15.6, yc + 0.05, 0xA0A5AA, 0.38, top=True)
f.rect(17.0, 16.25, 18.4, 18.25, 0x9AA0A6, 0.95, 0.02, top=True)      # roof plant
f.rect(17.2, 16.45, 18.2, 18.05, 0x4A4F55, 1.0, top=True)
for y in (3.0, 31.0):
    f.rect(17.5, y, 18.1, y + 0.6, 0x6A6F75, 0.7, top=True)           # vent pipes
f.grain(0.03)
f.save('depot_roof')

# ---- office wall: one 4 m bay, 7 m high
f = Face(4, 7)
f.rect(0, 0, 4, 7, 0xD6D0C2, 0.22, 0.02)
brick_band(f, 0, 0.8)
window(f, 0.8, 1.6, 3.2, 3.0, 0.8)
window(f, 0.8, 4.0, 3.2, 5.6, 0.8)
f.rect(0, 0, 0.18, 6.4, PIL, 0.5)
f.rect(3.82, 0, 4.0, 6.4, PIL, 0.5)
cornice(f, 7)
f.grain(0.03)
f.save('office_wall')

# ---- office roof: 6 m bay along, 12.5 m across, gravel with parapets and a rooftop unit
f = Face(6, 12.5)
f.rect(0, 0, 6, 12.5, 0x77736B, 0.05, 0.12, top=True)
for y0 in (0.0, 12.0):
    f.rect(0, y0, 6, y0 + 0.5, 0x6C7075, 0.9, top=True)
f.rect(1.8, 4.6, 4.2, 7.4, 0x9AA0A6, 1.0, 0.02, top=True)
f.rect(2.2, 5.0, 3.8, 7.0, 0x5A5F66, 0.95, top=True)
f.grain(0.02)
f.save('office_roof')
