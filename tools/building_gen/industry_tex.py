"""Relief textures for the specialized-industry hubs' SPOM slabs (SlabPS in src/shaders/mesh.hlsl, drawn by city_industry.inl).

Same format as depot_tex.py: u = one repeat along the face ("period" metres), v = the face's full extent, top row = top of a wall.
RGB albedo with baked AO, A = relief height out of the face (fraction of kSlabDepth, 0.8 m).
Slots per kind (city_industry.inl kIndSlab): 0 main wall, 1 second wall / end wall, 2 small building wall, 3 flat roof.

    python tools/building_gen/industry_tex.py      -> assets/industry/tex/<kind>_<slot>.png
"""
import os
import numpy as np
from PIL import Image

PX = 32
OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'assets', 'industry', 'tex')
rng = np.random.default_rng(11)


def hexc(c):
    return np.array([(c >> 16) & 255, (c >> 8) & 255, c & 255], np.float32) / 255.0


class Face:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.W, self.H = int(round(w * PX)), int(round(h * PX))
        self.col = np.zeros((self.H, self.W, 3), np.float32)
        self.hgt = np.zeros((self.H, self.W), np.float32)

    def rect(self, x0, y0, x1, y1, c, h, noise=0.0, top=False):
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

    def vgrad(self, x0, y0, x1, y1, a, b):
        r0, r1 = self.H - int(round(y1 * PX)), self.H - int(round(y0 * PX))
        c0, c1 = int(round(x0 * PX)), int(round(x1 * PX))
        if r1 > r0 and c1 > c0:
            self.col[r0:r1, c0:c1] *= np.linspace(a, b, r1 - r0, dtype=np.float32)[:, None, None]

    def ribs(self, x0, y0, x1, y1, c, base, step, rib=0.1, dh=0.12, top=False):
        """corrugated / standing-seam: vertical ribs every `step` m."""
        self.rect(x0, y0, x1, y1, c, base, 0.02, top)
        x = x0
        cc = hexc(c) * 1.08
        while x < x1 - 1e-3:
            self.rect(x, y0, min(x + rib, x1), y1, tuple(cc), base + dh, 0, top)
            x += step

    def grain(self, a):
        self.col *= 1.0 + a * rng.standard_normal((self.H, self.W, 1)).astype(np.float32)

    def dirt(self, amount=0.25, height=1.2):
        """grime rising from the ground."""
        rows = int(height * PX)
        g = 1.0 - amount * np.linspace(0, 1, rows, dtype=np.float32) ** 2
        self.col[self.H - rows:] *= g[:, None, None]

    def weather(self, streaks=0.18, fade=0.1, roof=False):
        """real-world wear: rain streaks running down from ledges, a sun-faded top, low-frequency blotches, desaturation."""
        H, W = self.H, self.W
        if not roof:
            # streaks: start under texels that stand proud (sills, ledges) and run down, fading
            ledge = (self.hgt[:-1] - self.hgt[1:]) > 0.12
            s = np.zeros((H, W), np.float32)
            src = np.zeros(W, np.float32)
            for r in range(1, H):
                src = np.maximum(src * 0.985, ledge[r - 1].astype(np.float32) * rng.uniform(0.4, 1.0, W).astype(np.float32))
                s[r] = src
            k = np.ones(5, np.float32) / 5
            s = np.apply_along_axis(lambda row: np.convolve(row, k, 'same'), 1, s)
            self.col *= (1.0 - streaks * s)[..., None]
            self.col *= np.linspace(1.0 + fade, 1.0, H, dtype=np.float32)[:, None, None]  # top bleached by the sun
        # blotches: coarse noise upsampled
        cx, cy = max(2, W // 24), max(2, H // 24)
        n = rng.standard_normal((cy, cx)).astype(np.float32)
        n = np.kron(n, np.ones((H // cy + 1, W // cx + 1), np.float32))[:H, :W]
        k = np.ones(9, np.float32) / 9
        n = np.apply_along_axis(lambda row: np.convolve(row, k, 'same'), 1, n)
        n = np.apply_along_axis(lambda col: np.convolve(col, k, 'same'), 0, n)
        self.col *= (1.0 + 0.06 * n)[..., None]
        # desaturate toward grey: paint and cladding are never pure
        g = self.col.mean(axis=2, keepdims=True)
        self.col = g + (self.col - g) * 0.78

    def save(self, name):
        if not getattr(self, 'weathered', False):
            self.weather(roof=name.endswith('_3'))
        h = self.hgt
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


GLASS, FRAME, WHITE, CONC, STEEL = 0x24323E, 0x5C6268, 0xEDEAE2, 0xB8B4AA, 0x7D8389


def window(f, x0, y0, x1, y1, frame=FRAME, bars=0.0, sill=0xB9B4AA, trim=0.1):
    f.rect(x0 - trim, y0 - trim, x1 + trim, y1 + trim, frame, 0.36)
    f.rect(x0, y0, x1, y1, GLASS, 0.04)
    f.vgrad(x0, y0, x1, y1, 1.7, 0.8)
    if bars:
        x = x0 + bars
        while x < x1 - 0.05:
            f.rect(x - 0.035, y0, x + 0.035, y1, frame, 0.22)
            x += bars
        f.rect(x0, (y0 + y1) / 2 - 0.035, x1, (y0 + y1) / 2 + 0.035, frame, 0.22)
    if sill is not None:
        f.rect(x0 - 0.18, y0 - 0.2, x1 + 0.18, y0 - 0.06, sill, 0.5)


def door(f, x0, x1, y1, c, h=0.08, frame=FRAME):
    f.rect(x0 - 0.1, 0, x1 + 0.1, y1 + 0.1, frame, 0.4)
    f.rect(x0, 0, x1, y1, c, h, 0.02)


def boards(f, x0, y0, x1, y1, c, step=0.35, base=0.3):
    """board and batten siding."""
    f.rect(x0, y0, x1, y1, c, base, 0.05)
    x = x0
    while x < x1:
        f.rect(x, y0, x + 0.07, y1, tuple(hexc(c) * 0.86), base + 0.12)
        x += step


def lap(f, x0, y0, x1, y1, c, step=0.2, base=0.28):
    """horizontal lap siding: each course a step deeper at its bottom edge."""
    y = y0
    while y < y1:
        f.rect(x0, y, x1, min(y + step, y1), c, base, 0.015)
        f.rect(x0, y, x1, y + 0.035, tuple(hexc(c) * 0.78), base + 0.1)
        y += step


def roller(f, x0, x1, y1, c=0xA7ABAF):
    f.rect(x0 - 0.18, 0, x1 + 0.18, y1 + 0.18, 0x4A4F55, 0.5)
    y = 0
    while y < y1:
        f.rect(x0, y, x1, y + 0.16, c, 0.16, 0.02)
        f.rect(x0, y, x1, y + 0.03, tuple(hexc(c) * 0.7), 0.1)
        y += 0.16


def louvre(f, x0, y0, x1, y1):
    f.rect(x0 - 0.08, y0 - 0.08, x1 + 0.08, y1 + 0.08, FRAME, 0.45)
    f.rect(x0, y0, x1, y1, 0x22262A, 0.1)
    y = y0
    while y < y1:
        f.rect(x0, y, x1, y + 0.05, 0x8A9096, 0.32)
        y += 0.13


def flat_roof(f, gravel=0x7A766E, parapet=0x6C7075, units=((0.5, 0.5),)):
    """across the whole extent: gravel, parapets on both long edges, rooftop units at (u, v) fractions."""
    W, H = f.w, f.h
    f.rect(0, 0, W, H, gravel, 0.05, 0.12, top=True)
    for y0 in (0.0, H - 0.45):
        f.rect(0, y0, W, y0 + 0.45, parapet, 0.9, 0.02, top=True)
        f.rect(0, y0 + 0.18, W, y0 + 0.27, tuple(hexc(parapet) * 0.8), 0.95, top=True)
    for (u, v) in units:
        cx, cy = u * W, v * H
        f.rect(cx - 1.1, cy - 0.8, cx + 1.1, cy + 0.8, 0x9AA0A6, 1.0, 0.02, top=True)
        f.rect(cx - 0.9, cy - 0.6, cx - 0.1, cy + 0.6, 0x50555B, 0.98, top=True)
        f.rect(cx + 0.15, cy - 0.5, cx + 0.85, cy + 0.5, 0x3E4247, 1.0, top=True)


def seam_roof(f, c, step=0.6, rib=0.06, pitch_edge=0x55595E, skylight=None):
    W, H = f.w, f.h
    f.ribs(0, 0, W, H, c, 0.04, step, rib, 0.1, top=True)
    for y0 in (0.0, H - 0.35):
        f.rect(0, y0, W, y0 + 0.35, pitch_edge, 0.55, top=True)
    if skylight:
        a, b = skylight
        f.rect(a, 0.35, b, H - 0.35, 0xD9DDE0, 0.2, top=True)
        f.col[int(0.35 * PX):int((H - 0.35) * PX), int(a * PX):int(b * PX)] *= 0.9


# ================================================================================================= grain farm
RED, REDD, TRIM = 0x9A3328, 0x6E2219, 0xEFEBE2
# 0: barn long wall, 7 m period, 7 m high: board and batten, a white-trimmed window, stall door every bay
f = Face(7, 7)
boards(f, 0, 0.6, 7, 7, RED)
f.rect(0, 0, 7, 0.6, CONC, 0.5, 0.05)
f.rect(0, 6.6, 7, 7.0, TRIM, 0.62)                        # eave fascia
f.rect(0, 0.6, 0.22, 6.6, TRIM, 0.55), f.rect(6.78, 0.6, 7, 6.6, TRIM, 0.55)  # corner boards (bay edges)
window(f, 1.2, 3.9, 2.6, 5.3, TRIM, 0.7, TRIM)
door(f, 3.9, 6.0, 2.6, REDD, 0.12, TRIM)
f.rect(3.9, 0, 6.0, 2.6, REDD, 0.12)
for d in (0, 1):                                          # the white X on the stall door
    for k in range(24):
        t = k / 23
        x = 3.9 + t * 2.1
        y = (t if d == 0 else 1 - t) * 2.6
        f.rect(x - 0.06, y - 0.06, x + 0.06, y + 0.06, TRIM, 0.3)
f.dirt(0.3, 1.0)
f.grain(0.03)
f.save('grain_0')
# 1: barn end, 10 m across, 7 m high: the big sliding doors with X bracing and a hay loft door under the eave
f = Face(10, 7)
boards(f, 0, 0.6, 10, 7, RED)
f.rect(0, 0, 10, 0.6, CONC, 0.5, 0.05)
f.rect(0, 6.6, 10, 7.0, TRIM, 0.62)
f.rect(2.4, 4.75, 7.6, 4.95, 0x4A4F55, 0.7)               # door track
for a, b in ((2.6, 5.0), (5.0, 7.4)):
    f.rect(a, 0, b, 4.6, REDD, 0.22, 0.03)
    f.rect(a, 0, a + 0.15, 4.6, TRIM, 0.4), f.rect(b - 0.15, 0, b, 4.6, TRIM, 0.4)
    f.rect(a, 4.45, b, 4.6, TRIM, 0.4), f.rect(a, 0, b, 0.15, TRIM, 0.4)
    for d in (0, 1):
        for k in range(36):
            t = k / 35
            f.rect(a + t * (b - a) - 0.07, (t if d == 0 else 1 - t) * 4.5 - 0.07, a + t * (b - a) + 0.07, (t if d == 0 else 1 - t) * 4.5 + 0.07, TRIM, 0.36)
f.rect(4.2, 5.2, 5.8, 6.5, 0x2A1A14, 0.06)                # hay loft door, open
f.rect(4.05, 5.05, 5.95, 5.2, TRIM, 0.45), f.rect(4.05, 5.2, 4.2, 6.5, TRIM, 0.45), f.rect(5.8, 5.2, 5.95, 6.5, TRIM, 0.45)
f.rect(4.9, 6.45, 5.1, 6.6, 0x4A4F55, 0.85)               # hay hook
f.dirt(0.3, 1.0)
f.grain(0.03)
f.save('grain_1')
# 2: farmhouse, 3.6 m period, 5.2 m high: cream lap siding, two storeys of windows with shutters, porch door
f = Face(3.6, 5.2)
lap(f, 0, 0.5, 3.6, 5.2, 0xEBDFC6)
f.rect(0, 0, 3.6, 0.5, 0x8C8578, 0.45, 0.06)
for y in (1.1, 3.3):
    window(f, 1.2, y, 2.4, y + 1.35, 0xF4F1EA, 0.6, 0xF4F1EA, 0.09)
    f.rect(0.82, y, 1.08, y + 1.35, 0x2F4A3A, 0.4), f.rect(2.52, y, 2.78, y + 1.35, 0x2F4A3A, 0.4)
f.rect(0, 5.0, 3.6, 5.2, 0xF4F1EA, 0.55)
f.grain(0.02)
f.save('grain_2')
# 3: grain dryer / scale house flat roof (6 m period, 6 m extent)
f = Face(6, 6)
seam_roof(f, 0x9AA0A4, 0.5)
f.grain(0.02)
f.save('grain_3')

# ================================================================================================= livestock farm
# 0: free-stall barn side, 4 m period, 5 m high: kick wall, open curtain band (dark inside, steel posts), white fascia
f = Face(4, 5)
f.rect(0, 0, 4, 1.2, CONC, 0.55, 0.05)
f.rect(0, 1.2, 4, 4.5, 0x1C1A18, 0.0)                    # the dark barn beyond the open side
f.vgrad(0, 1.2, 4, 4.5, 1.6, 0.7)
for y in (1.6, 2.4):                                      # steel gates / feed rails
    f.rect(0, y, 4, y + 0.07, 0x9AA0A6, 0.35)
f.rect(0, 0, 0.24, 4.5, 0xA7ABAF, 0.6)                    # post at the bay edge
f.rect(0, 3.55, 4, 4.5, 0xE6E2D8, 0.18, 0.03)             # curtain rolled up under the eave
for x in np.arange(0.1, 4, 0.25):
    f.rect(x, 3.55, x + 0.05, 4.5, 0xC9C5BA, 0.2)
f.rect(0, 4.5, 4, 5.0, TRIM, 0.62)
f.dirt(0.35, 1.2)
f.grain(0.03)
f.save('livestock_0')
# 1: barn end, 8.8 m across, 5 m high: red board and batten, a drive-through feed door, two windows
f = Face(8.8, 5)
boards(f, 0, 0.6, 8.8, 5, 0x8F3125)
f.rect(0, 0, 8.8, 0.6, CONC, 0.5, 0.05)
f.rect(0, 4.6, 8.8, 5.0, TRIM, 0.62)
roller(f, 3.0, 5.8, 3.8, 0xE3DFD5)
window(f, 0.9, 2.2, 2.0, 3.2, TRIM, 0.55, TRIM)
window(f, 6.8, 2.2, 7.9, 3.2, TRIM, 0.55, TRIM)
f.dirt(0.35, 1.2)
f.grain(0.03)
f.save('livestock_1')
# 2: farmhouse, white lap siding, 3.4 m period, 4.6 m high
f = Face(3.4, 4.6)
lap(f, 0, 0.5, 3.4, 4.6, 0xF1EFE9)
f.rect(0, 0, 3.4, 0.5, 0x7F7A70, 0.45, 0.06)
for y in (1.0, 2.95):
    window(f, 1.1, y, 2.3, y + 1.2, 0x3C4650, 0.6, 0xE9E6DE, 0.08)
f.rect(0, 4.4, 3.4, 4.6, 0xF4F1EA, 0.55)
f.grain(0.02)
f.save('livestock_2')
# 3: milk house flat roof
f = Face(5, 5)
seam_roof(f, 0x8E979C, 0.5)
f.grain(0.02)
f.save('livestock_3')

# ================================================================================================= forestry / sawmill
# 0: sawmill shed side, 6 m period, 8 m high: green-grey corrugated cladding, a band of translucent panels, roller door
f = Face(6, 8)
f.ribs(0, 1.0, 6, 8, 0x55685A, 0.22, 0.2, 0.08, 0.1)
f.rect(0, 0, 6, 1.0, CONC, 0.5, 0.05)
f.rect(0, 6.0, 6, 6.9, 0xCFD8CC, 0.2, 0.02)               # translucent light band
f.rect(0, 7.6, 6, 8.0, 0x3C4740, 0.65)
f.rect(0, 0, 0.3, 7.6, 0x6B7A6F, 0.6)                     # portal frame column
roller(f, 1.4, 4.9, 4.6)
f.rect(1.2, 4.95, 5.1, 5.2, 0xD8A928, 0.7)                # door head hazard strip
f.dirt(0.35, 1.5)
f.grain(0.03)
f.save('timber_0')
# 1: sawmill end, 14 m across, 8 m high: the log infeed opening (dark, chains), a man door, vents
f = Face(14, 8)
f.ribs(0, 1.0, 14, 8, 0x55685A, 0.22, 0.2, 0.08, 0.1)
f.rect(0, 0, 14, 1.0, CONC, 0.5, 0.05)
f.rect(4.0, 0.8, 10.0, 5.6, 0x8A7C66, 0.5)                # timber frame round the infeed
f.rect(4.4, 1.0, 9.6, 5.3, 0x121212, 0.0)
for x in np.arange(4.8, 9.4, 0.9):                        # hanging chain curtain
    f.rect(x, 2.4, x + 0.05, 5.3, 0x5A5F64, 0.15)
door(f, 11.0, 12.0, 2.2, 0x4C5A50)
louvre(f, 1.2, 5.6, 2.8, 6.8)
louvre(f, 11.2, 5.6, 12.8, 6.8)
f.rect(0, 7.6, 14, 8.0, 0x3C4740, 0.65)
f.dirt(0.35, 1.5)
f.grain(0.03)
f.save('timber_1')
# 2: log office, 3 m period, 4.4 m high: stained horizontal logs, a window, chinking
f = Face(3, 4.4)
y = 0.5
while y < 4.2:
    f.rect(0, y, 3, y + 0.3, 0x7A5634, 0.42, 0.06)
    f.rect(0, y, 3, y + 0.05, 0xC9BEA4, 0.22)
    y += 0.3
f.rect(0, 0, 3, 0.5, 0x7B7468, 0.5, 0.06)
window(f, 0.8, 1.5, 2.2, 2.9, 0x3B2A1B, 0.7, 0x3B2A1B, 0.12)
f.rect(0, 4.2, 3, 4.4, 0x3B2A1B, 0.6)
f.grain(0.03)
f.save('timber_2')
# 3: sawmill roof, 6 m period, 14 m across: standing seam with a ridge vent strip and a translucent sheet each bay
f = Face(6, 14)
seam_roof(f, 0x6E7A72, 0.6, skylight=(2.4, 3.6))
f.rect(0, 6.6, 6, 7.4, 0x3C4740, 0.75, top=True)          # ridge vent
f.grain(0.02)
f.save('timber_3')

# ================================================================================================= oil drilling
# 0: control building, 4 m period, 5.2 m high: white insulated panels, a ribbon window, blast door
f = Face(4, 5.2)
f.rect(0, 0, 4, 5.2, 0xE4E3DE, 0.24, 0.02)
for y in np.arange(0.6, 5.0, 1.0):
    f.rect(0, y, 4, y + 0.03, 0xB4B3AE, 0.18)
f.rect(0, 0, 4, 0.5, 0x8E8A80, 0.45, 0.05)
window(f, 0.4, 2.3, 3.6, 3.4, 0x5C6268, 1.6, None)
f.rect(0, 4.6, 4, 5.2, 0xC7462F, 0.5)                     # red fascia band (company colour stays blank otherwise)
f.rect(0, 0, 0.12, 4.6, 0xBEBCB5, 0.5)
f.grain(0.02)
f.save('oil_0')
# 1: compressor house, 6 m period, 5.6 m high: grey corrugated, louvres, roller door
f = Face(6, 5.6)
f.ribs(0, 0.5, 6, 5.6, 0x8C9196, 0.22, 0.2, 0.08, 0.1)
f.rect(0, 0, 6, 0.5, CONC, 0.5, 0.05)
roller(f, 0.6, 3.6, 3.8)
louvre(f, 4.2, 2.6, 5.5, 4.4)
louvre(f, 4.2, 0.8, 5.5, 2.2)
f.rect(0, 5.2, 6, 5.6, 0x55595E, 0.65)
f.dirt(0.35, 1.2)
f.grain(0.03)
f.save('oil_1')
# 2: tank farm control kiosk / lab, 3 m period, 3.4 m high
f = Face(3, 3.4)
f.ribs(0, 0.3, 3, 3.4, 0xD9D6CC, 0.24, 0.3, 0.06, 0.08)
f.rect(0, 0, 3, 0.3, CONC, 0.45)
door(f, 0.4, 1.3, 2.2, 0x4D5A66)
window(f, 1.8, 1.4, 2.6, 2.2, FRAME, 0, None)
f.rect(0, 3.1, 3, 3.4, 0x55595E, 0.6)
f.grain(0.02)
f.save('oil_2')
# 3: flat roof with HVAC
f = Face(6, 9)
flat_roof(f, 0x837E75, 0x6C7075, ((0.5, 0.5),))
f.grain(0.02)
f.save('oil_3')

# ================================================================================================= ore mining
# 0: processing plant, 8 m period, 12.8 m high: precast concrete panels, a high clerestory, louvre bank, conveyor port
f = Face(8, 12.8)
f.rect(0, 0, 8, 12.8, 0xB9B3A6, 0.24, 0.03)
for x in (0, 4):
    f.rect(x, 0.6, x + 0.04, 12.2, 0x8E897E, 0.16)
for y in (4.2, 8.4):
    f.rect(0, y, 8, y + 0.05, 0x8E897E, 0.16)
f.rect(0, 0, 8, 0.6, 0x7D786E, 0.5, 0.05)
window(f, 0.8, 9.3, 7.2, 11.0, FRAME, 1.2, None)
louvre(f, 1.0, 5.0, 3.2, 7.4)
roller(f, 4.6, 7.4, 4.0, 0x9FA3A6)
f.rect(0, 0, 0.35, 12.2, 0xA39D90, 0.6)                   # pilaster
f.rect(0, 12.2, 8, 12.8, 0x6F7378, 0.7)
f.dirt(0.4, 2.0)
f.grain(0.03)
f.save('ore_0')
# 1: hoist house, 3.2 m period, 6.6 m high: red brick, tall arched-head windows, corbelled cornice
f = Face(3.2, 6.6)
f.rect(0, 0, 3.2, 6.6, 0x6E6A62, 0.26)
row, y = 0, 0.6
while y < 6.0:
    off = (row % 2) * 0.12
    x = -off
    while x < 3.2:
        t = 1 + 0.12 * rng.standard_normal()
        f.rect(x + 0.015, y + 0.015, x + 0.24 - 0.015, y + 0.085, tuple(hexc(0x8C4632) * t), 0.32)
        x += 0.24
    y += 0.1
    row += 1
f.rect(0, 0, 3.2, 0.6, 0x8A857B, 0.5, 0.05)
window(f, 0.9, 1.4, 2.3, 4.6, 0x2F3438, 0.47, 0xA59F92, 0.14)
f.rect(0.76, 4.6, 2.44, 4.95, 0x9E9A8E, 0.5)              # stone lintel / arch head
f.rect(0, 6.0, 3.2, 6.6, 0x7E3B2A, 0.55)
f.rect(0, 6.3, 3.2, 6.45, 0x9E9A8E, 0.65)
f.rect(0, 0.6, 0.25, 6.0, 0x7E3B2A, 0.45)                 # brick pier
f.grain(0.03)
f.save('ore_1')
# 2: mine office, 3.6 m period, 4 m high: tan block, windows
f = Face(3.6, 4)
f.rect(0, 0, 3.6, 4, 0xC8B79A, 0.24, 0.03)
for y in np.arange(0.4, 3.6, 0.2):
    f.rect(0, y, 3.6, y + 0.02, 0xA89A80, 0.2)
window(f, 0.9, 1.3, 2.7, 2.7, FRAME, 0.9)
f.rect(0, 3.6, 3.6, 4, 0x6F7378, 0.6)
f.grain(0.02)
f.save('ore_2')
# 3: plant roof
f = Face(8, 9)
flat_roof(f, 0x77736B, 0x6C7075, ((0.3, 0.5), (0.75, 0.4)))
f.grain(0.02)
f.save('ore_3')

# ================================================================================================= landfill
# 0: site office (modular cabins), 3 m period, 3.6 m high
f = Face(3, 3.6)
f.ribs(0, 0.4, 3, 3.6, 0xE7E5DE, 0.22, 0.25, 0.05, 0.08)
f.rect(0, 0, 3, 0.4, 0x55595E, 0.5)
window(f, 0.6, 1.4, 2.4, 2.5, FRAME, 0.9, None)
f.rect(0, 3.3, 3, 3.6, 0x3F7A4A, 0.6)
f.rect(2.94, 0.4, 3, 3.3, 0x9EA2A5, 0.3)                  # module joint
f.grain(0.02)
f.save('landfill_0')
# 1: gatehouse / weighbridge kiosk, 2.4 m period, 2.8 m high: sliding hatch window
f = Face(2.4, 2.8)
f.ribs(0, 0.3, 2.4, 2.8, 0xE7E5DE, 0.22, 0.25, 0.05, 0.08)
f.rect(0, 0, 2.4, 0.3, 0x55595E, 0.5)
window(f, 0.3, 1.1, 2.1, 2.2, FRAME, 0.9, 0x6C7075)
f.rect(0, 2.55, 2.4, 2.8, 0x3F7A4A, 0.6)
f.grain(0.02)
f.save('landfill_1')
# 2: gas engine container, 6 m period, 3.2 m high: green corrugated with louvre doors
f = Face(6, 3.2)
f.ribs(0, 0, 6, 3.2, 0x3F6B48, 0.22, 0.28, 0.12, 0.12)
louvre(f, 0.5, 0.4, 2.3, 2.6)
louvre(f, 3.7, 0.4, 5.5, 2.6)
f.rect(0, 0, 6, 0.12, 0x2E3238, 0.5), f.rect(0, 3.05, 6, 3.2, 0x2E3238, 0.6)
f.grain(0.03)
f.save('landfill_2')
# 3: office roof
f = Face(6, 5.6)
flat_roof(f, 0x6E6A62, 0x55595E, ((0.5, 0.5),))
f.grain(0.02)
f.save('landfill_3')
