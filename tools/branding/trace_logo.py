"""Vectorises the Optima Engine logo (a black-on-white raster) into a small, clean SVG.

    python tools/branding/trace_logo.py <logo.png|webp> [out_dir]

Writes optima_engine.svg (black) and optima_engine_white.svg (white, for black screens) into out_dir (default
assets_src/branding). Needs numpy, Pillow, OpenCV (cv2) and SciPy - present on the dev machine; the game's build never runs
this (tools/branding/make_logo_png.py rasterises the committed SVG with Pillow + numpy only).

How:
  * the dome (the big arch with the two scalloped arches cut out of it) is traced: the image is upsampled 4x and
    thresholded so the outline is sub-pixel, split at its three sharp tips, smoothed and fitted with as few cubic Beziers
    as a 0.9 px tolerance allows (Schneider, Graphics Gems 1990);
  * the letters are geometric, so they are BUILT, not traced: each is a few primitives (stems and bars as rectangles,
    diagonals as polygons, the O and the G's bowl as ellipses / elliptic arcs of 4-segment cubics, the P's bowl a D) whose
    parameters are fitted to the raster by maximising the letter's IoU (Nelder-Mead). Straight stems stay straight;
  * the two thin rules beside ENGINE are rectangles.
One <path> (nonzero fill: shapes wind one way, counters the other) plus two <rect>s.
"""
import math
import os
import sys

import cv2
import numpy as np
from PIL import Image, ImageDraw
from scipy.optimize import minimize

UP = 4  # upsampling for the dome's sub-pixel outline
KAPPA = 0.5522847498

# ------------------------------------------------------------------ shapes: a subpath is a list of ('M'|'L', p) / ('C', c1, c2, p)


def poly(pts):
    return [("M", tuple(pts[0]))] + [("L", tuple(p)) for p in pts[1:]]


def rect(x0, y0, x1, y1):
    return poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])


def arc_cmds(cx, cy, rx, ry, a0, a1):
    """Cubics along an ellipse from angle a0 to a1 (radians, y down), at most 90 degrees each."""
    n = max(1, int(math.ceil(abs(a1 - a0) / (math.pi / 2) - 1e-9)))
    out = []
    da = (a1 - a0) / n
    k = 4.0 / 3.0 * math.tan(da / 4)
    for i in range(n):
        t0, t1 = a0 + da * i, a0 + da * (i + 1)
        p0 = (cx + rx * math.cos(t0), cy + ry * math.sin(t0))
        p3 = (cx + rx * math.cos(t1), cy + ry * math.sin(t1))
        c1 = (p0[0] - k * rx * math.sin(t0), p0[1] + k * ry * math.cos(t0))
        c2 = (p3[0] + k * rx * math.sin(t1), p3[1] - k * ry * math.cos(t1))
        out.append(("C", c1, c2, p3))
    return out


def ellipse(cx, cy, rx, ry):
    return [("M", (cx + rx, cy))] + arc_cmds(cx, cy, rx, ry, 0, 2 * math.pi)


def flatten(sp, n=16):
    pts, cur = [], None
    for c in sp:
        if c[0] in "ML":
            cur = c[1]
            pts.append(cur)
        else:
            c1, c2, p = c[1], c[2], c[3]
            for k in range(1, n + 1):
                s = k / n
                u = 1 - s
                pts.append((u ** 3 * cur[0] + 3 * u * u * s * c1[0] + 3 * u * s * s * c2[0] + s ** 3 * p[0],
                            u ** 3 * cur[1] + 3 * u * u * s * c1[1] + 3 * u * s * s * c2[1] + s ** 3 * p[1]))
            cur = p
    return pts


def area(pts):
    a = 0.0
    for i in range(len(pts)):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % len(pts)]
        a += x0 * y1 - x1 * y0
    return 0.5 * a


def reverse(sp):
    pts = []  # rebuild the command list backwards
    cur = sp[0][1]
    segs = []
    for c in sp[1:]:
        segs.append((cur, c))
        cur = c[-1]
    if cur != sp[0][1]:
        segs.append((cur, ("L", sp[0][1])))
    out = [("M", sp[0][1])]
    for start, c in reversed(segs):
        if c[0] == "L":
            out.append(("L", start))
        else:
            out.append(("C", c[2], c[1], start))
    return out


def orient(sp, hole):
    """Shapes wind with positive area (y down: clockwise on screen), counters negative: the nonzero rule then unites the
    shapes of a letter and cuts its counters."""
    a = area(flatten(sp))
    return reverse(sp) if (a < 0) != hole else sp


# ------------------------------------------------------------------ the letters (source pixels)


def L_O(x0, y0, x1, y1, sx, sy):
    cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    return [orient(ellipse(cx, cy, rx, ry), False), orient(ellipse(cx, cy, rx - sx, ry - sy), True)]


def L_P(x0, y0, x1, y1, s, bh, sh):
    # the stem, and the bowl: a D (straight top and bottom, a half-ellipse on the right) with its counter
    r = bh / 2
    def d(xl, yt, xr, yb):
        ry = (yb - yt) / 2
        rxx = min(ry * 1.25, (xr - xl) * 0.9)
        cmds = [("M", (xl, yt)), ("L", (xr - rxx, yt))]
        cmds += arc_cmds(xr - rxx, yt + ry, rxx, ry, -math.pi / 2, math.pi / 2)
        cmds += [("L", (xl, yb))]
        return cmds
    return [orient(rect(x0, y0, x0 + s, y1), False), orient(d(x0 + s * 0.5, y0, x1, y0 + bh), False),
            orient(d(x0 + s, y0 + sh, x1 - s, y0 + bh - sh), True)]


def L_T(x0, y0, x1, y1, s, sh):
    cx = (x0 + x1) / 2
    return [orient(rect(x0, y0, x1, y0 + sh), False), orient(rect(cx - s / 2, y0, cx + s / 2, y1), False)]


def L_I(x0, y0, x1, y1):
    return [orient(rect(x0, y0, x1, y1), False)]


def L_M(x0, y0, x1, y1, s, dw, vy, vt, bw):
    cx = (x0 + x1) / 2
    left = poly([(x0, y0), (x0 + dw, y0), (cx, vy - vt), (cx, vy), (cx - bw, vy)])
    right = poly([(x1, y0), (cx + bw, vy), (cx, vy), (cx, vy - vt), (x1 - dw, y0)])
    return [orient(rect(x0, y0, x0 + s, y1), False), orient(rect(x1 - s, y0, x1, y1), False), orient(left, False), orient(right, False)]


def L_A(x0, y0, x1, y1, f, w, ah):
    cx = (x0 + x1) / 2
    return [orient(poly([(x0, y1), (cx - f, y0), (cx + f, y0), (x1, y1), (x1 - w, y1), (cx, y0 + ah), (x0 + w, y1)]), False)]


def L_E(x0, y0, x1, y1, s, sh, wm, wb, my):
    return [orient(rect(x0, y0, x0 + s, y1), False), orient(rect(x0, y0, x1, y0 + sh), False),
            orient(rect(x0, my - sh / 2, x0 + wm, my + sh / 2), False), orient(rect(x0, y1 - sh, x0 + wb, y1), False)]


def L_N(x0, y0, x1, y1, s, dw):
    diag = poly([(x0, y0), (x0 + dw, y0), (x1, y1), (x1 - dw, y1)])
    return [orient(rect(x0, y0, x0 + s, y1), False), orient(rect(x1 - s, y0, x1, y1), False), orient(diag, False)]


def L_G(x0, y0, x1, y1, sx, sy, a1, bx, by, sh):
    # a C (elliptic ring from the upper right, round the left, to the right at the bar), a bar in from the right, and the
    # right side's short stem from the bar down to the ring
    cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    a2 = -2 * math.pi + 0.25  # anticlockwise on screen, round the top and left to just below the right-middle (the stem covers the join)
    ring = [("M", (cx + rx * math.cos(a1), cy + ry * math.sin(a1)))]
    ring += arc_cmds(cx, cy, rx, ry, a1, a2)
    ring += [("L", (cx + (rx - sx) * math.cos(a2), cy + (ry - sy) * math.sin(a2)))]
    ring += arc_cmds(cx, cy, rx - sx, ry - sy, a2, a1)
    bar = rect(bx, by, x1, by + sh)
    stem = rect(x1 - sx, by, x1, cy + ry * math.sin(0.5))
    return [orient(ring, False), orient(bar, False), orient(stem, False)]


# ------------------------------------------------------------------ rasterising a set of subpaths (nonzero, simple subpaths)


def raster_subpaths(subpaths, W, H, ox, oy, ss=4):
    acc = np.zeros((H * ss, W * ss), np.int16)
    for sp in subpaths:
        pts = flatten(sp)
        m = Image.new("1", (W * ss, H * ss), 0)
        ImageDraw.Draw(m).polygon([((x - ox) * ss, (y - oy) * ss) for x, y in pts], fill=1)
        acc += np.asarray(m, np.int16) * (1 if area(pts) > 0 else -1)
    return (acc != 0).reshape(H, ss, W, ss).mean(axis=(1, 3))


def fit_letter(fn, init, cov_src, box):
    """Nelder-Mead on 1 - IoU of the letter against the source coverage inside box (x0, y0, x1, y1), padded."""
    bx0, by0, bx1, by1 = box[0] - 4, box[1] - 4, box[2] + 4, box[3] + 4
    target = cov_src[by0:by1, bx0:bx1]
    W, H = bx1 - bx0, by1 - by0

    def cost(p):
        try:
            c = raster_subpaths(fn(*p), W, H, bx0, by0, 2)
        except (ValueError, ZeroDivisionError):
            return 2.0
        inter = np.minimum(c, target).sum()
        union = np.maximum(c, target).sum()
        return 1.0 - inter / max(union, 1e-9)

    p0 = np.array(init, float)
    best = minimize(cost, p0, method="Nelder-Mead", options={"xatol": 0.02, "fatol": 1e-5, "maxiter": 2500, "maxfev": 4000,
                                                               "initial_simplex": np.vstack([p0] + [p0 + np.eye(len(p0))[i] * max(0.6, abs(p0[i]) * 0.03) for i in range(len(p0))])})
    best = minimize(cost, best.x, method="Nelder-Mead", options={"xatol": 0.01, "fatol": 1e-6, "maxiter": 2000})
    return best.x, 1.0 - best.fun


# ------------------------------------------------------------------ the dome (traced)


def bez(ctrl, t):
    t = np.asarray(t)[:, None]
    u = 1 - t
    return u ** 3 * ctrl[0] + 3 * u * u * t * ctrl[1] + 3 * u * t * t * ctrl[2] + t ** 3 * ctrl[3]


def bez_d1(ctrl, t):
    t = np.asarray(t)[:, None]
    u = 1 - t
    return 3 * u * u * (ctrl[1] - ctrl[0]) + 6 * u * t * (ctrl[2] - ctrl[1]) + 3 * t * t * (ctrl[3] - ctrl[2])


def bez_d2(ctrl, t):
    t = np.asarray(t)[:, None]
    u = 1 - t
    return 6 * u * (ctrl[2] - 2 * ctrl[1] + ctrl[0]) + 6 * t * (ctrl[3] - 2 * ctrl[2] + ctrl[1])


def unit(v):
    n = np.linalg.norm(v)
    return v / n if n > 1e-12 else v


def generate(p, u, t1, t2):
    A1 = t1[None, :] * (3 * (1 - u) ** 2 * u)[:, None]
    A2 = t2[None, :] * (3 * (1 - u) * u ** 2)[:, None]
    C = np.array([[np.sum(A1 * A1), np.sum(A1 * A2)], [np.sum(A1 * A2), np.sum(A2 * A2)]])
    b0 = (1 - u) ** 3 + 3 * (1 - u) ** 2 * u
    b3 = 3 * (1 - u) * u ** 2 + u ** 3
    tmp = p - (b0[:, None] * p[0] + b3[:, None] * p[-1])
    X = np.array([np.sum(A1 * tmp), np.sum(A2 * tmp)])
    det = C[0, 0] * C[1, 1] - C[0, 1] * C[1, 0]
    seg = np.linalg.norm(p[-1] - p[0])
    a1 = a2 = seg / 3
    if abs(det) > 1e-12:
        a1 = (X[0] * C[1, 1] - X[1] * C[0, 1]) / det
        a2 = (C[0, 0] * X[1] - C[1, 0] * X[0]) / det
    if a1 < 1e-6 * seg or a2 < 1e-6 * seg:
        a1 = a2 = seg / 3
    return np.array([p[0], p[0] + t1 * a1, p[-1] + t2 * a2, p[-1]])


def fit_cubic(p, t1, t2, err, out):
    if len(p) == 2:
        dist = np.linalg.norm(p[1] - p[0]) / 3
        out.append(np.array([p[0], p[0] + t1 * dist, p[1] + t2 * dist, p[1]]))
        return
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
    u = d / d[-1]
    ctrl = generate(p, u, t1, t2)
    for it in range(25):
        dist = np.linalg.norm(bez(ctrl, u) - p, axis=1)
        if dist.max() < err:
            out.append(ctrl)
            return
        if dist.max() > err * 4:
            break
        q = bez(ctrl, u) - p
        d1, d2 = bez_d1(ctrl, u), bez_d2(ctrl, u)
        den = np.sum(d1 * d1, axis=1) + np.sum(q * d2, axis=1)
        u = np.clip(np.where(np.abs(den) > 1e-12, u - np.sum(q * d1, axis=1) / np.where(np.abs(den) > 1e-12, den, 1), u), 0, 1)
        ctrl = generate(p, u, t1, t2)
    dist = np.linalg.norm(bez(ctrl, u) - p, axis=1)
    split = min(max(int(np.argmax(dist)), 1), len(p) - 2)
    tc = unit(p[split - 1] - p[split + 1])
    fit_cubic(p[: split + 1], t1, tc, err, out)
    fit_cubic(p[split:], -tc, t2, err, out)


def trace_dome(c, err=0.9):
    """c: the closed outline (N x 2). Splits at the three sharpest turns (the tips), smooths, fits."""
    n = len(c)
    k = 8 * UP
    a = c[(np.arange(n) - k) % n] - c
    b = c[(np.arange(n) + k) % n] - c
    ang = np.degrees(np.arccos(np.clip(np.sum(a * b, 1) / (np.linalg.norm(a, axis=1) * np.linalg.norm(b, axis=1) + 1e-9), -1, 1)))
    tips = []
    for i in np.argsort(ang):
        if all(min(abs(i - t), n - abs(i - t)) > 4 * k for t in tips):
            tips.append(int(i))
        if len(tips) == 3:
            break
    tips.sort()
    sigma = 1.5 * UP
    r = int(3 * sigma)
    w = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    w /= w.sum()
    out = []
    for j, t0 in enumerate(tips):
        t1 = tips[(j + 1) % 3]
        idx = np.arange(t0, t1 + 1) if t1 > t0 else np.r_[np.arange(t0, n), np.arange(0, t1 + 1)]
        p = c[idx]
        # smooth inside the span (the ends stay put at the tips)
        pad = np.r_[2 * p[0] - p[r:0:-1], p, 2 * p[-1] - p[-2:-r - 2:-1]]  # odd reflection: the ends keep their tangents
        s = np.c_[np.convolve(pad[:, 0], w, "valid"), np.convolve(pad[:, 1], w, "valid")]
        s[0], s[-1] = p[0], p[-1]
        kk = min(6, len(s) - 1)
        fit_cubic(s, unit(s[kk] - s[0]), unit(s[-1 - kk] - s[-1]), err, out)
    cmds = [("M", tuple(out[0][0]))] + [("C", tuple(q[1]), tuple(q[2]), tuple(q[3])) for q in out]
    return orient(cmds, False)


# ------------------------------------------------------------------ output


def fmt(v):
    s = f"{v:.1f}"
    s = s[:-2] if s.endswith(".0") else s
    return "0" if s == "-0" else s


def to_d(sp, ox, oy):
    out = []
    for c in sp:
        if c[0] == "M":
            out.append(f"M{fmt(c[1][0] - ox)} {fmt(c[1][1] - oy)}")
        elif c[0] == "L":
            out.append(f"L{fmt(c[1][0] - ox)} {fmt(c[1][1] - oy)}")
        else:
            out.append("C" + " ".join(f"{fmt(q[0] - ox)} {fmt(q[1] - oy)}" for q in c[1:]))
    return "".join(out) + "Z"


def main():
    src = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets_src", "branding")
    g = np.asarray(Image.open(src).convert("L"), dtype=np.float32)
    H, W = g.shape
    cov = np.clip((255.0 - g) / 255.0, 0, 1)
    ink = (g < 128).astype(np.uint8)
    n, lab, st, _ = cv2.connectedComponentsWithStats(ink, 8)
    comps = sorted([tuple(st[i]) for i in range(1, n) if st[i][4] > 100], key=lambda s: (s[1] > 700, s[1] > 620, s[0]))
    dome = max(comps, key=lambda s: s[4])
    rules = [s for s in comps if s[2] > 20 * s[3]]
    letters = [s for s in comps if s is not dome and s not in rules]
    big = [s for s in letters if s[3] > 60]
    small = [s for s in letters if s[3] <= 60]
    big.sort(key=lambda s: s[0]); small.sort(key=lambda s: s[0])
    assert len(big) == 6 and len(small) == 6, (len(big), len(small))
    # the logo's box and margin: the viewBox
    x0 = min(s[0] for s in comps); y0 = min(s[1] for s in comps)
    x1 = max(s[0] + s[2] for s in comps); y1 = max(s[1] + s[3] for s in comps)
    ox, oy = x0 - 8, y0 - 8
    vw, vh = x1 - x0 + 16, y1 - y0 + 16
    # the dome, traced at 4x
    bigimg = cv2.resize(g, (W * UP, H * UP), interpolation=cv2.INTER_CUBIC)
    mask = ((bigimg < 128) & (cv2.resize((lab == [i for i in range(1, n) if tuple(st[i]) == dome][0]).astype(np.uint8), (W * UP, H * UP), interpolation=cv2.INTER_NEAREST) > 0)).astype(np.uint8)
    cs, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    c = max(cs, key=cv2.contourArea)[:, 0, :].astype(np.float64)
    shapes = [trace_dome((c + 0.5) / UP)]
    # the letters, built and fitted
    report = []

    def box(s):
        return (s[0], s[1], s[0] + s[2], s[1] + s[3])

    def go(name, fn, s, extra):
        bx = box(s)
        init = [bx[0], bx[1], bx[2], bx[3]] + extra
        p, iou = fit_letter(fn, init, cov, bx)
        report.append((name, iou, len(p)))
        return fn(*p)

    O, P, T, I, M, A = big
    E1, N1, G, I2, N2, E2 = small
    ss, se = 14.0, 7.0
    shapes += go("O", L_O, O, [ss, ss * 0.85])
    shapes += go("P", L_P, P, [ss, 50.0, ss * 0.85])
    shapes += go("T", L_T, T, [ss, ss * 0.9])
    shapes += go("I", L_I, I, [])
    shapes += go("M", L_M, M, [ss, ss * 1.5, M[1] + 64.0, 18.0, 8.0])
    shapes += go("A", L_A, A, [9.0, ss * 1.2, 18.0])
    for nm, s in (("E", E1), ("E", E2)):
        shapes += go(nm, L_E, s, [se, se * 0.85, s[2] * 0.9, s[2] * 1.0, s[1] + s[3] / 2])
    for s in (N1, N2):
        shapes += go("N", L_N, s, [se, se * 1.3])
    shapes += go("G", L_G, G, [se, se * 0.9, -0.6, G[0] + G[2] * 0.55, G[1] + G[3] * 0.5, se * 0.85])
    shapes += go("I", L_I, I2, [])
    body = "".join(to_d(sp, ox, oy) for sp in shapes)
    rect_s = "".join(f'<rect x="{fmt(s[0] - ox)}" y="{fmt(s[1] - oy)}" width="{fmt(s[2])}" height="{fmt(s[3])}"/>' for s in rules)
    os.makedirs(out_dir, exist_ok=True)
    for name, colour in (("optima_engine.svg", "#000"), ("optima_engine_white.svg", "#fff")):
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {vw} {vh}" width="{vw}" height="{vh}">'
               f'<!-- vectorised from a {W}x{H} raster (tools/branding/trace_logo.py), crop origin {ox} {oy} -->'
               f'<title>Optima Engine</title><g fill="{colour}"><path d="{body}"/>{rect_s}</g></svg>\n')
        with open(os.path.join(out_dir, name), "w", newline="\n") as f:
            f.write(svg)
    for nm, iou, k in report:
        print(f"  {nm}: IoU {iou:.4f} ({k} parameters)")
    print(f"{len(shapes)} subpaths, {len(rules)} rules, viewBox {vw} x {vh}, crop origin ({ox}, {oy}) px")


if __name__ == "__main__":
    main()
