"""High-density facade tiles: 18 m x 18 m (6 storeys x 6 bays of 3 m), every window drawn on its own.

Each window is a small scene, not a sticker: a room behind the glass (back wall, side wall, floor, ceiling, furniture, sometimes a lamp on),
a covering chosen per window (curtains, roller blind, venetian blind, net curtain, nothing), the sky and the street reflected over it in
Fresnel, a shadow from the reveal, a bevelled frame, a sill and the dirt that runs off it. Air conditioners, flower boxes, balcony clutter
and rain streaks on the wall finish it. Everything that crosses the tile edge wraps, so the tile is seamless; noise is made periodic by
filtering white noise in the frequency domain.
"""
import math
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

TILE_M = 18.0
PX = 2048
S = PX / TILE_M          # pixels per metre

WALL_TONES = [(0.93, 0.90, 0.84), (0.95, 0.94, 0.92), (0.80, 0.82, 0.78), (0.88, 0.80, 0.70), (0.72, 0.78, 0.84), (0.90, 0.86, 0.74),
              (0.65, 0.60, 0.55), (0.85, 0.75, 0.75)]
FABRICS = [(0.82, 0.78, 0.70), (0.92, 0.90, 0.86), (0.55, 0.20, 0.18), (0.30, 0.38, 0.55), (0.62, 0.66, 0.48), (0.78, 0.62, 0.36),
           (0.42, 0.42, 0.44), (0.88, 0.80, 0.62), (0.70, 0.52, 0.58), (0.22, 0.30, 0.28), (0.95, 0.95, 0.93), (0.60, 0.48, 0.38)]
BLINDS = [(0.93, 0.92, 0.88), (0.85, 0.83, 0.78), (0.72, 0.72, 0.70), (0.30, 0.30, 0.32), (0.88, 0.84, 0.74), (0.62, 0.58, 0.52)]
FURN = [(0.10, 0.08, 0.07), (0.18, 0.14, 0.10), (0.25, 0.22, 0.20), (0.12, 0.13, 0.16), (0.30, 0.20, 0.12), (0.35, 0.33, 0.30)]


# ---------------------------------------------------------------- periodic noise
def pnoise(rng, size, sx, sy=None):
    """Seamless noise, roughly unit variance: white noise low-passed with a Gaussian of sx (and sy) pixels, wrapping at the edges."""
    sy = sx if sy is None else sy
    w = rng.standard_normal((size, size))
    fy = np.fft.fftfreq(size)[:, None]; fx = np.fft.fftfreq(size)[None, :]
    filt = np.exp(-2 * (math.pi ** 2) * ((fx * sx) ** 2 + (fy * sy) ** 2))
    n = np.real(np.fft.ifft2(np.fft.fft2(w) * filt))
    return (n - n.mean()) / (n.std() + 1e-9)


def smooth(t): t = np.clip(t, 0, 1); return t * t * (3 - 2 * t)


class Canvas:
    """Albedo (float 0..1, sRGB) and height (0..1); every rectangle wraps around the tile."""
    def __init__(self, base, height):
        self.a = base.astype(np.float32); self.h = height.astype(np.float32)

    def view(self, x0, y0, x1, y1):
        ys = np.arange(int(round(y0)), int(round(y1))) % PX; xs = np.arange(int(round(x0)), int(round(x1))) % PX
        return np.ix_(ys, xs)

    def fill(self, x0, y0, x1, y1, col=None, h=None, mul=None):
        ix = self.view(x0, y0, x1, y1)
        if col is not None: self.a[ix] = col
        if mul is not None: self.a[ix] *= mul
        if h is not None: self.h[ix] = h

    def put(self, x0, y0, rgb, hgt=None, alpha=None):
        hh, ww = rgb.shape[:2]; ix = self.view(x0, y0, x0 + ww, y0 + hh)
        if alpha is None: self.a[ix] = rgb
        else: self.a[ix] = self.a[ix] * (1 - alpha[..., None]) + rgb * alpha[..., None]
        if hgt is not None:
            if alpha is None: self.h[ix] = hgt
            else: self.h[ix] = self.h[ix] * (1 - alpha) + hgt * alpha


def bevel_box(cv, x0, y0, x1, y1, col, h, lip=None):
    """A solid profile (frame member, sill, slab edge): lit top and left edges, shaded bottom and right."""
    col = np.array(col, np.float32)
    cv.fill(x0, y0, x1, y1, col=col, h=h)
    e = max(1, int(lip if lip is not None else min(x1 - x0, y1 - y0) * 0.22))
    cv.fill(x0, y0, x1, y0 + e, mul=1.18); cv.fill(x0, y0, x0 + e, y1, mul=1.08)
    cv.fill(x0, y1 - e, x1, y1, mul=0.72); cv.fill(x1 - e, y0, x1, y1, mul=0.82)


# ---------------------------------------------------------------- one window
class Glazing:
    def __init__(self, rng, refl, tint=(1.0, 1.0, 1.0), F=0.42, lit=0.08, kinds=None, room_dark=1.0):
        self.rng = rng; self.refl = refl; self.tint = np.array(tint, np.float32); self.F = F; self.lit = lit; self.room_dark = room_dark
        self.kinds = kinds or dict(none=0.3, curtains=0.32, roller=0.2, venetian=0.1, net=0.08)

    # -- the room behind the pane, seen straight on
    def room(self, w, h, lit):
        r = self.rng
        v = np.linspace(0, 1, h, dtype=np.float32)[:, None]; u = np.linspace(0, 1, w, dtype=np.float32)[None, :]
        wall = np.array(WALL_TONES[r.integers(len(WALL_TONES))], np.float32)
        k = (0.5 if lit else 0.17) * self.room_dark * r.uniform(0.75, 1.2)
        img = np.ones((h, w, 3), np.float32) * wall * k
        img *= (0.75 + 0.25 * (1 - np.abs(u - r.uniform(0.3, 0.7)) * 1.4))[..., None]       # light falls off away from the window
        side = r.uniform(0.08, 0.3); left = r.random() < 0.5                                 # a side wall in perspective
        sm = (u < side) if left else (u > 1 - side)
        img[np.broadcast_to(sm, (h, w))] *= 0.62
        cy = r.uniform(0.08, 0.16); fy = r.uniform(0.72, 0.84)                                # ceiling and floor
        img[np.broadcast_to(v < cy, (h, w))] *= 0.72
        floor = np.array([(0.32, 0.22, 0.14), (0.45, 0.40, 0.36), (0.25, 0.25, 0.27), (0.50, 0.36, 0.22)][r.integers(4)], np.float32) * k * 1.4
        img[np.broadcast_to(v > fy, (h, w))] = floor
        if lit:                                                                              # a lamp: warm pool
            lx, ly = r.uniform(0.2, 0.8), r.uniform(0.15, 0.45)
            d = np.sqrt((u - lx) ** 2 * (w / h) ** 2 + (v - ly) ** 2)
            img += (np.array((1.0, 0.72, 0.4)) * 0.55 * np.exp(-(d / 0.35) ** 2)[..., None]).astype(np.float32)
            img *= np.array((1.08, 1.0, 0.86), np.float32)
        if r.random() < 0.5:                                                                 # a picture on the back wall
            px0, pw = r.uniform(0.2, 0.6), r.uniform(0.12, 0.25); py0, ph = r.uniform(0.25, 0.4), r.uniform(0.12, 0.2)
            img[int(py0 * h):int((py0 + ph) * h), int(px0 * w):int((px0 + pw) * w)] = np.array(FABRICS[r.integers(len(FABRICS))]) * k * 0.9
        for _ in range(r.integers(1, 4)):                                                    # furniture against the floor line
            fw_, fh_ = r.uniform(0.15, 0.5), r.uniform(0.12, 0.45)
            fx0 = r.uniform(-0.1, 1.0 - fw_ * 0.5)
            c = np.array(FURN[r.integers(len(FURN))], np.float32) * (2.2 if lit else 1.0) * self.room_dark
            y1_ = int(min(h, (fy + r.uniform(0.0, 0.08)) * h)); y0_ = max(0, int(y1_ - fh_ * h))
            img[y0_:y1_, max(0, int(fx0 * w)):min(w, int((fx0 + fw_) * w))] = c
        if r.random() < 0.3:                                                                 # a pendant lamp
            lx = r.uniform(0.3, 0.7)
            img[:int(r.uniform(0.15, 0.3) * h), int(lx * w):int(lx * w) + max(1, w // 120)] *= 0.4
        return img

    # -- what hangs in front of the room
    def covering(self, img, lit):
        r = self.rng; h, w = img.shape[:2]
        v = np.linspace(0, 1, h, dtype=np.float32)[:, None]; u = np.linspace(0, 1, w, dtype=np.float32)[None, :]
        names = list(self.kinds); p = np.array([self.kinds[n] for n in names]); kind = names[r.choice(len(names), p=p / p.sum())]
        light = 0.9 if lit else 0.7
        if kind == "curtains":
            c = np.array(FABRICS[r.integers(len(FABRICS))], np.float32) * light
            period = r.uniform(0.035, 0.07); ph = r.uniform(0, 6.28)
            fold = 0.72 + 0.28 * np.sin(u / period * 6.28 + ph) ** 2
            cl = r.uniform(0.05, 0.45) if r.random() < 0.9 else 0.5; cr = r.uniform(0.05, 0.45) if r.random() < 0.9 else 0.5
            if r.random() < 0.15: cl = cr = 0.5                                              # drawn shut
            m = (u < cl) | (u > 1 - cr)
            cur = np.broadcast_to(c * fold[..., None], (h, w, 3))
            mm = np.broadcast_to(m, (h, w))
            img[mm] = cur[mm]
            if r.random() < 0.4:                                                             # a net behind the drapes
                img[:] = img * 0.55 + np.array((0.88, 0.88, 0.86)) * light * 0.45 * (0.85 + 0.15 * np.sin(u / 0.012) ** 2)[..., None]
        elif kind == "roller":
            c = np.array(BLINDS[r.integers(len(BLINDS))], np.float32) * light
            down = r.choice([r.uniform(0.15, 0.6), r.uniform(0.6, 0.97), 1.0])
            m = np.broadcast_to(v < down, (h, w))
            shade = (0.92 + 0.08 * v)[..., None] * np.ones((1, w, 1))
            img[m] = (c * shade)[m]
            b = int(down * h)
            if b < h: img[max(0, b - max(2, h // 60)):b] = c * 0.55
        elif kind == "venetian":
            c = np.array(BLINDS[r.integers(len(BLINDS))], np.float32) * light
            down = r.uniform(0.3, 1.0); slat = max(4, int(h * r.uniform(0.022, 0.035)))
            openness = r.uniform(0.0, 0.6)
            rows = (np.arange(h) % slat) / slat
            sl = np.where(rows < 0.5, 1.0, 0.78)[:, None, None] * c
            m = (np.arange(h) < down * h)[:, None]
            gap = (rows > 1 - openness * 0.6)[:, None, None]
            mix = np.where(gap, img * 0.7 + sl * 0.3, sl)
            img[:] = np.where(m[..., None], mix, img)
        elif kind == "net":
            c = np.array((0.90, 0.90, 0.87), np.float32) * light
            fold = 0.82 + 0.18 * np.sin(u / r.uniform(0.01, 0.02) * 6.28) ** 2
            a = r.uniform(0.55, 0.8)
            img[:] = img * (1 - a) + (c * fold[..., None]) * a
        return img

    # -- the pane: room and covering under the reflection
    def pane(self, cv, x0, y0, x1, y1, room=True, lit=None, F=None, bright=1.0, inside=None):
        r = self.rng; w, h = int(round(x1 - x0)), int(round(y1 - y0))
        if w < 3 or h < 3: return
        lit = (r.random() < self.lit) if lit is None else lit
        if inside is not None: inside = np.ascontiguousarray(inside[:h, :w]); h, w = inside.shape[:2]
        elif room: inside = self.covering(self.room(w, h, lit), lit)
        else: inside = np.zeros((h, w, 3), np.float32) + 0.03
        dx, dy = r.uniform(-0.25, 0.25) * S, r.uniform(-0.15, 0.15) * S                    # each pane is bowed a little: its reflection slips
        refl = self.refl[cv.view(x0 + dx, y0 + dy, x0 + dx + w, y0 + dy + h)].copy()
        v = np.linspace(0, 1, h, dtype=np.float32)[:, None, None]
        refl *= r.uniform(0.85, 1.12) * (1.08 - 0.16 * v) * bright                          # each pane sits a hair off plane
        if r.random() < 0.08: refl *= np.array(r.uniform(0.92, 1.08, 3), np.float32)       # a replaced pane, slightly different glass
        Fp = (self.F if F is None else F) * r.uniform(0.85, 1.12) * (0.8 if lit else 1.0)
        glass = (inside * (1 - Fp) + refl * Fp) * self.tint
        glass *= (0.55 + 0.45 * smooth(v / 0.14))                                           # the reveal's shadow on the top of the glass
        glass *= (0.75 + 0.25 * smooth(np.linspace(0, 1, w, dtype=np.float32)[None, :, None] / 0.06))
        cv.put(int(round(x0)), int(round(y0)), np.clip(glass, 0, 1), np.full((h, w), 0.22, np.float32))

    # -- a full window: panes split by mullions and a transom, a frame, a sill
    def window(self, cv, x0, y0, x1, y1, trim, mull=1, transom=None, fw=0.06, sill=True, sill_col=None, open_p=0.07, F=None):
        r = self.rng; f = max(2, int(fw * S))
        lit = r.random() < self.lit
        cv.fill(x0 - f * 0.6, y0 - f * 0.6, x1 + f * 0.6, y1 + f * 0.6, mul=0.55)           # the reveal: a dark line around the opening
        xs = [x0 + (x1 - x0) * i / (mull + 1) for i in range(mull + 2)]
        ys = [y0, y0 + (y1 - y0) * transom, y1] if transom else [y0, y1]
        W_, H_ = int(round(x1 - x0)) + 2, int(round(y1 - y0)) + 2                           # one room behind the whole window
        full = self.covering(self.room(W_, H_, lit), lit)
        for i in range(len(xs) - 1):
            for j in range(len(ys) - 1):
                ox, oy = int(round(xs[i] - x0)), int(round(ys[j] - y0))
                self.pane(cv, xs[i], ys[j], xs[i + 1], ys[j + 1], lit=lit, F=F, inside=full[oy:, ox:])
        trim = np.array(trim, np.float32)
        for xx in xs[1:-1]: bevel_box(cv, xx - f * 0.5, y0, xx + f * 0.5, y1, trim, 0.7)
        for yy in ys[1:-1]: bevel_box(cv, x0, yy - f * 0.5, x1, yy + f * 0.5, trim, 0.7)
        if r.random() < open_p:                                                             # a tilted-open sash: a dark wedge at its top
            i = r.integers(len(xs) - 1); jj = len(ys) - 2
            sx0, sx1, sy0 = xs[i], xs[i + 1], ys[jj]
            cv.fill(sx0 + f, sy0, sx1 - f, sy0 + (ys[jj + 1] - sy0) * 0.12, col=np.array((0.04, 0.04, 0.05)))
            bevel_box(cv, sx0 + f, sy0 + (ys[jj + 1] - sy0) * 0.12, sx1 - f, sy0 + (ys[jj + 1] - sy0) * 0.12 + f * 0.7, trim, 0.7)
        bevel_box(cv, x0 - f, y0 - f, x1 + f, y0, trim, 0.75); bevel_box(cv, x0 - f, y1, x1 + f, y1 + f, trim, 0.75)
        bevel_box(cv, x0 - f, y0, x0, y1, trim, 0.75); bevel_box(cv, x1, y0, x1 + f, y1, trim, 0.75)
        if sill:
            sc = np.array(sill_col if sill_col is not None else trim, np.float32) * 0.95
            sh = max(3, int(0.06 * S)); ov = int(0.06 * S)
            cv.fill(x0 - f - ov, y1 + f + sh, x1 + f + ov, y1 + f + sh * 2.2, mul=0.5)      # its shadow on the wall
            bevel_box(cv, x0 - f - ov, y1 + f, x1 + f + ov, y1 + f + sh, sc, 0.92, lip=max(1, sh // 3))
            for sx in (x0 - f - ov + 2, x1 + f + ov - 6) if r.random() < 0.7 else ():          # drips off the sill ends
                L = int(r.uniform(0.4, 1.6) * S); w_ = max(3, int(r.uniform(0.04, 0.12) * S))
                prof = (1 - np.linspace(0, 1, L, dtype=np.float32)) ** 1.5
                ix = cv.view(sx - w_ // 2, y1 + f + sh * 2, sx + w_ // 2 + 1, y1 + f + sh * 2 + L)
                across = np.hanning(ix[1].shape[1] + 2)[1:-1].astype(np.float32)
                cv.a[ix] *= (1 - 0.28 * prof[:, None] * across[None, :])[..., None]
            under = cv.view(x0, y1 + f + sh * 2, x1, y1 + f + sh * 2 + int(0.5 * S))       # a soft grey wash under the whole sill
            fade = (1 - np.linspace(0, 1, under[0].shape[0], dtype=np.float32)) ** 2
            cv.a[under] *= (1 - 0.12 * fade)[:, None, None]


# ---------------------------------------------------------------- props on the wall
def ac_unit(cv, rng, cx, top):
    w, h = 0.8 * S, 0.55 * S; x0 = cx - w / 2
    cv.fill(x0 + 0.06 * S, top + 0.05 * S, x0 + w + 0.12 * S, top + h + 0.14 * S, mul=0.55)  # its shadow
    c = np.array((0.86, 0.86, 0.83), np.float32) * rng.uniform(0.85, 1.0)
    bevel_box(cv, x0, top, x0 + w, top + h, c, 0.95)
    gx, gy, gr = x0 + w * 0.62, top + h * 0.5, h * 0.36                                      # the fan grille
    ix = cv.view(gx - gr, gy - gr, gx + gr, gy + gr)
    yy, xx = np.mgrid[0:ix[0].shape[0], 0:ix[1].shape[1]].astype(np.float32); yy -= gr; xx -= gr
    d = np.sqrt(xx ** 2 + yy ** 2) / gr
    m = d < 1
    ring = np.where((np.sin(d * 30) > 0.3) & m, 0.45, 1.0)
    cv.a[ix] *= np.where(m, ring * 0.7, 1.0)[..., None]
    for k in range(5):
        cv.fill(x0 + w * 0.08, top + h * (0.2 + k * 0.14), x0 + w * 0.36, top + h * (0.2 + k * 0.14) + 2, mul=0.6)
    cv.fill(x0 + 0.3 * S, top - 0.6 * S, x0 + 0.33 * S, top, col=np.array((0.85, 0.85, 0.82)))  # the pipe up to the window


def flower_box(cv, rng, x0, x1, top):
    h = 0.22 * S
    bevel_box(cv, x0, top, x1, top + h, np.array((0.45, 0.28, 0.18)) * rng.uniform(0.8, 1.2), 0.95)
    n = int((x1 - x0) / 6)
    for _ in range(n):
        x = rng.uniform(x0, x1); r_ = rng.uniform(0.05, 0.13) * S; y = top - rng.uniform(0, 0.12) * S
        g = np.array([(0.18, 0.32, 0.12), (0.25, 0.40, 0.15), (0.70, 0.15, 0.18), (0.85, 0.75, 0.25), (0.75, 0.40, 0.60)][rng.integers(5)], np.float32)
        blob(cv, x, y, r_, g * rng.uniform(0.8, 1.1), 0.9)


def blob(cv, cx, cy, r, col, h):
    ix = cv.view(cx - r, cy - r, cx + r, cy + r)
    yy, xx = np.mgrid[0:ix[0].shape[0], 0:ix[1].shape[1]].astype(np.float32)
    d = np.sqrt((xx - r) ** 2 + (yy - r) ** 2) / max(r, 1)
    a = smooth((1 - d) * 4)
    shade = (0.7 + 0.3 * smooth(1 - (yy / (2 * r))))[..., None]
    cv.a[ix] = cv.a[ix] * (1 - a[..., None]) + col * shade * a[..., None]
    cv.h[ix] = np.maximum(cv.h[ix], h * a)


def balcony_stuff(cv, rng, x0, x1, floor_y):
    """Chairs, a table, plants, a drying rack, a bike: dark shapes standing on a balcony floor, seen through or over the railing."""
    for _ in range(rng.integers(0, 4)):
        kind = rng.choice(5, p=[0.36, 0.24, 0.15, 0.19, 0.06]); cx = rng.uniform(x0 + 0.75 * S, x1 - 0.75 * S)
        if kind == 0:                                                                        # a potted plant
            pot = np.array((0.55, 0.30, 0.20)) * rng.uniform(0.7, 1.1)
            cv.fill(cx - 0.12 * S, floor_y - 0.3 * S, cx + 0.12 * S, floor_y, col=pot)
            for _k in range(4):
                blob(cv, cx + rng.uniform(-0.18, 0.18) * S, floor_y - rng.uniform(0.4, 0.9) * S, rng.uniform(0.12, 0.25) * S,
                     np.array((0.16, 0.30, 0.12)) * rng.uniform(0.7, 1.3), 0.8)
        elif kind == 1:                                                                      # a chair
            c = np.array(FURN[rng.integers(len(FURN))]) * 1.5
            cv.fill(cx - 0.22 * S, floor_y - 0.9 * S, cx - 0.17 * S, floor_y, col=c); cv.fill(cx + 0.17 * S, floor_y - 0.45 * S, cx + 0.22 * S, floor_y, col=c)
            cv.fill(cx - 0.22 * S, floor_y - 0.48 * S, cx + 0.22 * S, floor_y - 0.42 * S, col=c)
        elif kind == 2:                                                                      # a table
            c = np.array((0.55, 0.55, 0.55)) * rng.uniform(0.5, 1.2)
            cv.fill(cx - 0.35 * S, floor_y - 0.75 * S, cx + 0.35 * S, floor_y - 0.7 * S, col=c); cv.fill(cx - 0.03 * S, floor_y - 0.7 * S, cx + 0.03 * S, floor_y, col=c)
        elif kind == 3:                                                                      # washing on a rack
            for k in range(rng.integers(2, 5)):
                c = np.array(FABRICS[rng.integers(len(FABRICS))]) * 0.95
                xx = cx + (k - 2) * 0.16 * S
                cv.fill(xx, floor_y - 1.1 * S, xx + 0.13 * S, floor_y - rng.uniform(0.6, 0.85) * S, col=c)
            cv.fill(cx - 0.4 * S, floor_y - 1.12 * S, cx + 0.45 * S, floor_y - 1.09 * S, col=np.array((0.8, 0.8, 0.8)))
        else:                                                                                # a bicycle
            c = np.array((0.08, 0.08, 0.09))
            for wx in (-0.35, 0.35):
                ix = cv.view(cx + wx * S - 0.3 * S, floor_y - 0.6 * S, cx + wx * S + 0.3 * S, floor_y)
                yy, xx = np.mgrid[0:ix[0].shape[0], 0:ix[1].shape[1]].astype(np.float32)
                d = np.sqrt((xx - 0.3 * S) ** 2 + (yy - 0.3 * S) ** 2) / (0.3 * S)
                ring = (np.abs(d - 0.9) < 0.1)[..., None]
                cv.a[ix] = np.where(ring, c, cv.a[ix])
            cv.fill(cx - 0.35 * S, floor_y - 0.5 * S, cx + 0.35 * S, floor_y - 0.46 * S, col=c)


# ---------------------------------------------------------------- the reflection layer and the wall
def reflection_layer(rng, kind):
    """What the glass reflects, as one image under the whole tile: sky and clouds above, the city across the street low in each storey."""
    v = (np.arange(PX, dtype=np.float32) / PX)[:, None]
    cloud = pnoise(rng, PX, 70, 40) * 0.6 + pnoise(rng, PX, 18, 12) * 0.25
    zen, hor = np.array((0.42, 0.56, 0.74), np.float32), np.array((0.80, 0.86, 0.92), np.float32)
    storey = (0.5 + 0.5 * pnoise(rng, PX, 260, 140) * 0.6).clip(0, 1).astype(np.float32)        # sky brightness drifts slowly over the facade
    sky = zen[None, None] * (1 - storey[..., None]) + hor[None, None] * storey[..., None]
    sky = np.broadcast_to(sky, (PX, PX, 3)).copy()
    sky += (smooth(cloud * 0.8 + 0.2) * 0.22)[..., None]
    # buildings across the street: a ragged skyline per storey band, darker and warmer than the sky
    city = np.zeros((PX, PX), np.float32)
    cols = np.cumsum(rng.uniform(0.6, 3.5, 200) * S).astype(int); cols = cols[cols < PX]
    edges = np.concatenate([[0], cols, [PX]])
    for fl in range(6):
        y0 = int(fl * 3.0 * S)
        if rng.random() < (0.5 if kind == "tower" else 0.25): continue                       # many storeys see only sky
        for a, b in zip(edges[:-1], edges[1:]):
            if rng.random() < 0.35: continue
            hgt = rng.uniform(0.1, 0.45) * 3.0 * S
            city[y0 + int(3.0 * S - hgt):y0 + int(3.0 * S), a:b] = rng.uniform(0.3, 0.8)
    city = np.asarray(Image.fromarray((city * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(9)), np.float32) / 255
    city *= smooth(pnoise(rng, PX, 60) * 0.5 + 0.6)                                           # broken up, never a clean silhouette
    wincity = (np.sin(np.arange(PX) / S * 6.28 / 0.9)[None, :] > 0.4) & (np.sin(np.arange(PX)[:, None] / S * 6.28 / 1.1) > 0.3)
    citycol = np.array((0.30, 0.29, 0.28), np.float32) * (1 + 0.25 * pnoise(rng, PX, 30)[..., None]) * np.where(wincity, 0.6, 1.0)[..., None]
    k = 0.5 if kind == "tower" else 0.45
    out = sky * (1 - city[..., None] * k) + citycol * city[..., None] * k
    return np.clip(out, 0, 1.4).astype(np.float32)


def wall_weathering(cv, rng, strength=1.0):
    streak = pnoise(rng, PX, 5, 160)                                                          # long vertical rain streaks
    stain = pnoise(rng, PX, 90)
    blotch = pnoise(rng, PX, 22)
    k = 1 - strength * (0.05 * smooth(streak * 0.5 - 0.2) + 0.06 * smooth(stain * 0.5 + 0.3) + 0.03 * blotch)
    cv.a *= k[..., None]
    tint = 1 + 0.035 * pnoise(rng, PX, 200)[..., None] * np.array((1.0, 0.6, -0.4), np.float32)
    cv.a *= tint


def floor_lines(cv, col, h=0.82, thick=0.16, mul=None):
    for fl in range(6):
        y = (fl + 1) * 3.0 * S - thick * S
        if mul is None: bevel_box(cv, 0, y, PX, y + thick * S, col, h)
        else: cv.fill(0, y, PX, y + thick * S, mul=mul, h=h)


# ---------------------------------------------------------------- the five facades
def build_tile(level, base_rgb, base_h, trim, glass_rgb, seed):
    rng = np.random.default_rng(seed)
    cv = Canvas(base_rgb / 255.0, base_h)
    wall_weathering(cv, rng, 1.0 if level in (1, 2) else 0.7)
    trim = np.array(trim, np.float32) / 255.0
    gtint = np.array(glass_rgb, np.float32) / np.max(glass_rgb)
    tint = 0.78 + 0.22 * gtint

    if level == 1:                                                                            # concrete slab: punched windows, stacked loggias
        refl = reflection_layer(rng, "street"); gz = Glazing(rng, refl, tint, F=0.38, lit=0.07)
        floor_lines(cv, trim * 0.92, thick=0.18)
        loggia_cols = set(rng.choice(6, size=2, replace=False).tolist())
        for fl in range(6):
            fy = fl * 3.0
            for b in range(6):
                bx = b * 3.0
                if b in loggia_cols:                                                          # a recessed loggia: deep shade, door and window, a railing
                    lx0, lx1, ly0, ly1 = (bx + 0.25) * S, (bx + 2.75) * S, (fy + 0.25) * S, (fy + 2.82) * S
                    cv.fill(lx0, ly0, lx1, ly1, col=np.array((0.33, 0.32, 0.30)) * (0.9 + 0.2 * rng.random()), h=0.05)
                    cv.fill(lx0, ly0, lx1, ly0 + 0.5 * S, mul=0.55)
                    gz.window(cv, lx0 + 0.15 * S, ly0 + 0.4 * S, lx0 + 0.95 * S, ly1 - 0.05 * S, trim, mull=0, sill=False, open_p=0.05)
                    gz.window(cv, lx0 + 1.2 * S, ly0 + 0.4 * S, lx1 - 0.15 * S, ly0 + 1.75 * S, trim, mull=1, sill=False)
                    balcony_stuff(cv, rng, lx0, lx1, ly1)
                    rail_top = ly1 - 1.0 * S
                    if rng.random() < 0.5:                                                    # solid parapet
                        ix = cv.view(lx0 - 0.1 * S, rail_top, lx1 + 0.1 * S, ly1 + 0.06 * S)
                        tex = base_rgb[ix].astype(np.float32) / 255.0
                        cv.a[ix] = tex / tex.mean() * trim * rng.uniform(0.85, 1.0); cv.h[ix] = 0.9
                        cv.fill(lx0 - 0.1 * S, ly1 - 0.05 * S, lx1 + 0.1 * S, ly1 + 0.06 * S, mul=0.75)
                        cv.fill(lx0 - 0.1 * S, rail_top, lx1 + 0.1 * S, rail_top + 0.06 * S, mul=1.15)
                    else:                                                                     # bars
                        for k in range(int((lx1 - lx0) / (0.12 * S))):
                            xx = lx0 + k * 0.12 * S
                            cv.fill(xx, rail_top, xx + 0.025 * S, ly1, col=np.array((0.18, 0.18, 0.2)), h=0.9)
                        bevel_box(cv, lx0, rail_top, lx1, rail_top + 0.06 * S, np.array((0.2, 0.2, 0.22)), 0.95)
                    if rng.random() < 0.06:                                                   # a satellite dish
                        blob(cv, lx1 - 0.35 * S, rail_top - 0.2 * S, 0.25 * S, np.array((0.88, 0.88, 0.86)), 0.95)
                else:
                    wide = rng.random() < 0.3
                    x0_, x1_ = (bx + (0.55 if wide else 0.8)) * S, (bx + (2.45 if wide else 2.2)) * S
                    gz.window(cv, x0_, (fy + 0.85) * S, x1_, (fy + 2.15) * S, trim, mull=2 if wide else 1, transom=0.28 if rng.random() < 0.5 else None)
                    if rng.random() < 0.12: ac_unit(cv, rng, (bx + 1.5) * S + rng.uniform(-0.4, 0.4) * S, (fy + 2.45) * S)

    elif level == 2:                                                                          # brick: tall sash windows, lintels, sills, flower boxes
        refl = reflection_layer(rng, "street"); gz = Glazing(rng, refl, tint, F=0.36, lit=0.08)
        for fl in range(6):
            fy = fl * 3.0
            cv.fill(0, (fy + 2.86) * S, PX, (fy + 3.0) * S, col=trim * 0.88, h=0.8); cv.fill(0, (fy + 2.86) * S, PX, (fy + 2.89) * S, mul=1.2)
            for b in range(6):
                bx = b * 3.0
                x0_, x1_, y0_, y1_ = (bx + 0.9) * S, (bx + 2.1) * S, (fy + 0.5) * S, (fy + 2.4) * S
                bevel_box(cv, x0_ - 0.18 * S, y0_ - 0.3 * S, x1_ + 0.18 * S, y0_ - 0.06 * S, trim, 0.85)   # stone lintel
                for k in range(1, 6): cv.fill(x0_ - 0.18 * S + k * (x1_ - x0_ + 0.36 * S) / 6, y0_ - 0.3 * S, x0_ - 0.18 * S + k * (x1_ - x0_ + 0.36 * S) / 6 + 2, y0_ - 0.06 * S, mul=0.7)
                frame = np.array((0.94, 0.93, 0.9), np.float32) if rng.random() < 0.85 else np.array((0.22, 0.28, 0.24), np.float32)
                gz.window(cv, x0_, y0_, x1_, y1_, frame, mull=1, transom=0.5, fw=0.055, sill_col=trim)
                if rng.random() < 0.14: flower_box(cv, rng, x0_ - 0.05 * S, x1_ + 0.05 * S, y1_ + 0.14 * S)
                elif rng.random() < 0.07: ac_unit(cv, rng, (bx + 1.5) * S, y1_ + 0.25 * S)

    elif level == 3:                                                                          # curtain wall: continuous glass, spandrels, mullions
        refl = reflection_layer(rng, "tower")
        gz = Glazing(rng, refl, tint * np.array((0.85, 0.97, 1.0)), F=0.48, lit=0.07, room_dark=1.1,
                     kinds=dict(none=0.25, roller=0.4, venetian=0.2, curtains=0.1, net=0.05))
        mul_c = np.array((0.55, 0.58, 0.6), np.float32)
        for fl in range(6):
            fy = fl * 3.0
            for b in range(6):
                bx = b * 3.0
                gz.pane(cv, bx * S, (fy + 0.0) * S, (bx + 3.0) * S, (fy + 2.15) * S)
                gz.pane(cv, bx * S, (fy + 2.15) * S, (bx + 3.0) * S, (fy + 3.0) * S, room=False, F=0.85, bright=0.75)  # spandrel: dark backed glass
                if rng.random() < 0.08:                                                       # an opening vent
                    cv.fill((bx + 2.2) * S, (fy + 0.05) * S, (bx + 2.95) * S, (fy + 0.25) * S, col=np.array((0.05, 0.05, 0.06)))
            bevel_box(cv, 0, (fy + 2.12) * S, PX, (fy + 2.18) * S, mul_c, 0.7)
            bevel_box(cv, 0, (fy + 2.97) * S, PX, (fy + 3.03) * S, mul_c * 0.9, 0.7)
        for b in range(6):
            bevel_box(cv, b * 3.0 * S - 0.05 * S, 0, b * 3.0 * S + 0.05 * S, PX, mul_c * 1.1, 0.85, lip=2)
            bevel_box(cv, (b * 3.0 + 1.5) * S - 0.025 * S, 0, (b * 3.0 + 1.5) * S + 0.025 * S, PX, mul_c, 0.75, lip=1)

    elif level == 4:                                                                          # granite: piers, arched windows, panels below
        refl = reflection_layer(rng, "street"); gz = Glazing(rng, refl, tint, F=0.4, lit=0.08)
        for fl in range(6):
            fy = fl * 3.0
            cv.fill(0, (fy + 2.88) * S, PX, (fy + 3.0) * S, col=trim * 0.9, h=0.82); cv.fill(0, (fy + 2.88) * S, PX, (fy + 2.91) * S, mul=1.15)
            for b in range(6):
                bx = b * 3.0
                bevel_box(cv, bx * S, fy * S, (bx + 0.45) * S, (fy + 2.88) * S, trim, 0.88)
                x0_, x1_ = (bx + 0.8) * S, (bx + 2.2) * S
                # the arched head: a half-disc fanlight with radial bars
                r_ = (x1_ - x0_) / 2; cx, cy = (x0_ + x1_) / 2, (fy + 1.0) * S
                ix = cv.view(x0_, cy - r_, x1_, cy)
                yy, xx = np.mgrid[0:ix[0].shape[0], 0:ix[1].shape[1]].astype(np.float32)
                d = np.sqrt((xx - r_) ** 2 + (yy - r_) ** 2) / r_
                ang = np.arctan2(r_ - yy, xx - r_)
                inside = d < 1
                fan = refl[ix] * 0.9 * tint
                bars = (np.abs(np.sin(ang * 3)) < 0.06) | (np.abs(d - 0.45) < 0.035) | (np.abs(d - 1) < 0.07)
                col = np.where(bars[..., None], trim, fan * 0.8 + 0.03)
                cv.a[ix] = np.where(inside[..., None], col, cv.a[ix]); cv.h[ix] = np.where(inside, np.where(bars, 0.7, 0.22), cv.h[ix])
                kd = np.sqrt((xx - r_) ** 2 + (yy - r_) ** 2) / r_
                key = (kd > 1) & (kd < 1.22)
                cv.a[ix] = np.where(key[..., None], trim * 1.05, cv.a[ix])
                gz.window(cv, x0_, cy, x1_, (fy + 2.35) * S, trim * 0.7 + 0.1, mull=1, transom=None, fw=0.05, sill_col=trim)
                bevel_box(cv, x0_, (fy + 2.5) * S, x1_, (fy + 2.82) * S, trim * 0.92, 0.8)
                cv.fill(x0_ + 0.06 * S, (fy + 2.56) * S, x1_ - 0.06 * S, (fy + 2.76) * S, mul=0.86, h=0.6)

    else:                                                                                     # balcony tower: floor-to-ceiling glass, slabs, glass railings
        refl = reflection_layer(rng, "tower"); gz = Glazing(rng, refl, tint, F=0.5, lit=0.08, kinds=dict(none=0.35, curtains=0.3, net=0.2, roller=0.15))
        for fl in range(6):
            fy = fl * 3.0
            for b in range(6):
                bx = b * 3.0
                x0_, x1_ = (bx + 0.2) * S, (bx + 2.8) * S
                gz.window(cv, x0_, (fy + 0.12) * S, x1_, (fy + 2.55) * S, np.array((0.25, 0.26, 0.28)), mull=2 if rng.random() < 0.5 else 1,
                          fw=0.045, sill=False, open_p=0.0)
                balcony_stuff(cv, rng, x0_, x1_, (fy + 2.55) * S)
            # the slab edge, its shadow on the glass below, and a glass balustrade
            slab = (fy + 2.55) * S
            bevel_box(cv, 0, slab, PX, slab + 0.26 * S, trim, 1.0)
            nxt = (fy + 2.81) * S
            sh = cv.view(0, nxt, PX, nxt + 0.45 * S)
            cv.a[sh] *= (0.5 + 0.5 * smooth(np.linspace(0, 1, sh[0].shape[0], dtype=np.float32)))[:, None, None]
            rt = slab - 1.05 * S
            rail = cv.view(0, rt, PX, slab)
            rr = np.clip(cv.a[rail] * np.array((0.82, 0.92, 0.9)) + refl[rail] * 0.12, 0, 1)
            cv.a[rail] = rr
            bevel_box(cv, 0, rt - 0.05 * S, PX, rt, np.array((0.78, 0.8, 0.82)), 0.95, lip=2)
            for b in range(12): cv.fill(b * 1.5 * S, rt, b * 1.5 * S + 0.03 * S, slab, col=np.array((0.6, 0.62, 0.64)), h=0.9)

    alb = np.clip(cv.a * 255, 0, 255).astype(np.uint8)
    h = np.asarray(Image.fromarray((np.clip(cv.h, 0, 1) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.2)), np.float32) / 255.0
    return alb, h


def glass_field(seed, glass_rgb, head=2.25, kind="street", F=0.3, kinds=None, room_dark=1.0, lit=0.08):
    """What lies behind L1's SPOM wall: every 3 m x 3 m cell of the 18 m tile is one flat's window, room and covering under the sky's
    reflection, darkened under the head of the opening (head: metres above the storey floor) where the reveal shades it."""
    rng = np.random.default_rng(seed)
    cv = Canvas(np.zeros((PX, PX, 3), np.float32), np.zeros((PX, PX), np.float32))
    gtint = np.array(glass_rgb, np.float32) / np.max(glass_rgb)
    gz = Glazing(rng, reflection_layer(rng, kind), 0.8 + 0.2 * gtint, F=F, lit=lit, kinds=kinds, room_dark=room_dark)
    for fl in range(6):
        for b in range(6):
            gz.pane(cv, b * 3.0 * S, fl * 3.0 * S, (b + 1) * 3.0 * S, (fl + 1) * 3.0 * S)
    rows = (np.arange(PX) / S) % 3.0                                   # metres down from the storey's top
    head_from_top = 3.0 - head
    k = 0.5 + 0.5 * smooth((rows - head_from_top) / 0.35)
    cv.a *= k[:, None, None]
    alb = np.clip(cv.a * 255, 0, 255).astype(np.uint8)
    return alb, np.full((PX, PX), 0.5, np.float32)


def lobby_field(seed, Wm=12.0, Hm=4.5, ppm=64):
    """A lit lobby or shop seen through the podium glazing: stone floor with its reflection, ceiling with downlights, a reception desk,
    sofas, plants, pendant lights, art and columns. Tiles every Wm metres along the podium."""
    if Hm > 4.6:                                                                     # a taller podium: the ground floor at its true scale,
        ground = lobby_field(seed, Wm, 4.5, ppm)                                     # a mezzanine slab, and lit upper floors above it
        up = lobby_field(seed + 17, Wm, Hm - 4.5, ppm) if Hm - 4.5 > 3.0 else np.repeat(ground[:int(0.1 * ground.shape[0])], int((Hm - 4.5) / 0.45), 0)
        up = up[:int(round((Hm - 4.5) * ppm))]
        slab = np.full((int(0.35 * ppm), ground.shape[1], 3), 70, np.uint8)
        out = np.concatenate([up, slab, ground[int(0.35 * ppm):]], 0)
        return out
    rng = np.random.default_rng(seed)
    w, h = int(Wm * ppm), int(Hm * ppm)
    v = (np.arange(h, dtype=np.float32) / h)[:, None]
    wall = np.array([(0.62, 0.48, 0.34), (0.78, 0.74, 0.68), (0.85, 0.83, 0.78), (0.40, 0.34, 0.30), (0.70, 0.66, 0.60)][rng.integers(5)], np.float32)
    img = np.ones((h, w, 3), np.float32) * wall * 0.62
    if rng.random() < 0.5:                                                           # wood or stone panelling joints
        for x in range(0, w, int(rng.uniform(0.6, 1.2) * ppm)): img[:, x:x + 2] *= 0.7
    ceil_y = int(0.1 * h); floor_y = int(0.8 * h)
    img[:ceil_y] = np.array((0.82, 0.8, 0.76)) * 0.7
    floor = np.array([(0.85, 0.82, 0.76), (0.35, 0.33, 0.32), (0.60, 0.52, 0.42)][rng.integers(3)], np.float32)
    img[floor_y:] = floor * 0.6
    refl = img[floor_y - (h - floor_y):floor_y][::-1] * 0.35                         # the polished floor mirrors the room
    img[floor_y:] += refl[:h - floor_y]
    for x in np.arange(0.5, Wm, rng.uniform(1.2, 1.8)):                              # downlights and their pools on the wall
        cx = int(x * ppm)
        img[max(0, ceil_y - 3):ceil_y, max(0, cx - 4):cx + 4] = (1.0, 0.96, 0.85)
        yy, xx = np.mgrid[ceil_y:floor_y, max(0, cx - 40):min(w, cx + 40)]
        d = ((xx - cx) / 40.0) ** 2 + ((yy - ceil_y) / (0.5 * h)) ** 2
        img[ceil_y:floor_y, max(0, cx - 40):min(w, cx + 40)] += (np.array((1.0, 0.85, 0.6)) * 0.22 * np.exp(-d * 2.5)[..., None])
    # furniture: a reception desk, sofas, plants, art, a column or two
    def rect(x0, x1, y0, y1, c): img[int(y0 * h):int(y1 * h), max(0, int(x0 * ppm)):min(w, int(x1 * ppm))] = c
    dx = rng.uniform(1.5, Wm - 5)
    rect(dx, dx + 3.2, 0.62, 0.8, np.array((0.25, 0.2, 0.17)) * rng.uniform(0.8, 1.5)); rect(dx, dx + 3.2, 0.6, 0.63, (0.9, 0.88, 0.82))
    rect(dx + 0.2, dx + 3.0, 0.75, 0.77, (1.0, 0.9, 0.65))                           # its lit kick plate
    for _ in range(rng.integers(1, 3)):
        sx = rng.uniform(0.3, Wm - 2.5); c = np.array([(0.55, 0.5, 0.45), (0.2, 0.25, 0.3), (0.6, 0.35, 0.25), (0.75, 0.72, 0.65)][rng.integers(4)])
        rect(sx, sx + 2.2, 0.68, 0.8, c * 0.7); rect(sx, sx + 2.2, 0.6, 0.7, c * 0.55)
    for _ in range(rng.integers(2, 5)):
        px = rng.uniform(0.3, Wm - 0.6)
        rect(px, px + 0.45, 0.68, 0.8, (0.2, 0.2, 0.2))
        for _k in range(8):
            cx, cy, r = (px + 0.22 + rng.uniform(-0.4, 0.4)) * ppm, rng.uniform(0.38, 0.68) * h, rng.uniform(0.15, 0.3) * ppm
            yy, xx = np.ogrid[:h, :w]; m = (xx - cx) ** 2 + (yy - cy) ** 2 < r * r
            img[m] = np.array((0.15, 0.32, 0.12)) * rng.uniform(0.7, 1.3)
    for _ in range(rng.integers(1, 3)):
        ax = rng.uniform(0.5, Wm - 2); aw = rng.uniform(1.0, 2.0)
        rect(ax, ax + aw, 0.25, 0.48, np.array(FABRICS[rng.integers(len(FABRICS))]) * 0.8)
    for _ in range(rng.integers(1, 4)):                                              # pendant lamps
        lx = int(rng.uniform(0.5, Wm - 0.5) * ppm)
        img[ceil_y:int(0.3 * h), lx:lx + 2] = 0.2
        yy, xx = np.ogrid[:h, :w]; m = ((xx - lx) / 22.0) ** 2 + ((yy - 0.32 * h) / 12.0) ** 2 < 1
        img[m] = (1.0, 0.9, 0.7)
    if rng.random() < 0.6:
        cx = rng.uniform(1, Wm - 1); rect(cx, cx + 0.5, 0.1, 0.8, wall * 0.9 + 0.08)
    img *= rng.uniform(0.95, 1.15)
    return np.clip(img * 255, 0, 255).astype(np.uint8)
