"""Rasterises the Optima Engine logo SVG into the PNGs the game draws (the engine has no SVG renderer).

    python tools/branding/make_logo_png.py                 # writes the game's PNGs (below)
    python tools/branding/make_logo_png.py --check <src>   # IoU of the SVG against the source raster (exit 1 below 0.97)

Pure Python: Pillow + numpy only (no cairo, no inkscape). It reads the subset of SVG that tools/branding/trace_logo.py
writes - one <path> of absolute M / L / C / Z commands (nonzero fill, simple subpaths: shapes wind one way, counters the
other), plus <rect>s - flattens the curves, fills by winding at 4x and box-filters down for anti-aliasing.

Outputs (white logo on transparent, straight alpha):
    assets_src/branding/optima_engine_1024.png   1024 px wide  (iPad / phone screens; bundled into the app)
    assets_src/branding/optima_engine_2048.png   2048 px wide  (4K / large screens)
"""
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
BRAND = os.path.normpath(os.path.join(HERE, "..", "..", "assets_src", "branding"))
SS = 4  # supersampling


def parse(svg_text):
    vb = [float(v) for v in re.search(r'viewBox="([^"]+)"', svg_text).group(1).split()]
    subpaths, rects = [], []
    for d in re.findall(r'<path[^>]* d="([^"]+)"', svg_text):
        toks = re.findall(r"[MLCZ]|-?\d*\.?\d+(?:e-?\d+)?", d)
        i, cur, start, pts = 0, None, None, []
        while i < len(toks):
            t = toks[i]
            i += 1
            if t == "M":
                if pts:
                    subpaths.append(pts)
                cur = (float(toks[i]), float(toks[i + 1])); i += 2
                start, pts = cur, [cur]
            elif t == "L":
                cur = (float(toks[i]), float(toks[i + 1])); i += 2
                pts.append(cur)
            elif t == "C":
                c1 = (float(toks[i]), float(toks[i + 1])); c2 = (float(toks[i + 2]), float(toks[i + 3]))
                p = (float(toks[i + 4]), float(toks[i + 5])); i += 6
                n = 24
                for k in range(1, n + 1):
                    s = k / n; u = 1 - s
                    pts.append((u ** 3 * cur[0] + 3 * u * u * s * c1[0] + 3 * u * s * s * c2[0] + s ** 3 * p[0],
                                u ** 3 * cur[1] + 3 * u * u * s * c1[1] + 3 * u * s * s * c2[1] + s ** 3 * p[1]))
                cur = p
            elif t == "Z":
                if pts:
                    subpaths.append(pts)
                pts, cur = [], start
        if pts:
            subpaths.append(pts)
    for m in re.finditer(r"<rect ([^>]+)/>", svg_text):
        a = dict(re.findall(r'(\w+)="([^"]+)"', m.group(1)))
        rects.append(tuple(float(a[k]) for k in ("x", "y", "width", "height")))
    return vb, subpaths, rects


def raster(svg_text, width, offset=(0.0, 0.0), canvas=None):
    """Coverage (0..1 float array). width: output pixels across the viewBox; canvas=(w, h) and offset (px) place the
    viewBox's origin at `offset` on a canvas of that size instead (the IoU check lines it up with the source)."""
    vb, subpaths, rects = parse(svg_text)
    k = width / vb[2]
    W, H = canvas if canvas else (int(round(vb[2] * k)), int(round(vb[3] * k)))
    wind = np.zeros((H * SS, W * SS), np.int16)
    for sp in subpaths:  # nonzero rule over simple subpaths: each adds its winding (+1 / -1 by its signed area)
        m = Image.new("1", (W * SS, H * SS), 0)
        ImageDraw.Draw(m).polygon([((x * k + offset[0]) * SS, (y * k + offset[1]) * SS) for x, y in sp], fill=1)
        a = sum(sp[i][0] * sp[(i + 1) % len(sp)][1] - sp[(i + 1) % len(sp)][0] * sp[i][1] for i in range(len(sp)))
        wind += np.asarray(m, np.int16) * (1 if a > 0 else -1)
    big = wind != 0
    m = Image.new("1", (W * SS, H * SS), 0)
    dr = ImageDraw.Draw(m)
    for x, y, w, h in rects:
        dr.rectangle([(x * k + offset[0]) * SS, (y * k + offset[1]) * SS, ((x + w) * k + offset[0]) * SS - 1, ((y + h) * k + offset[1]) * SS - 1], fill=1)
    big |= np.asarray(m, bool)
    return big.reshape(H, SS, W, SS).mean(axis=(1, 3))


def write_png(cov, path):
    a = (np.clip(cov, 0, 1) * 255 + 0.5).astype(np.uint8)
    rgba = np.dstack([np.full_like(a, 255)] * 3 + [a])
    Image.fromarray(rgba, "RGBA").save(path, optimize=True)


def check(src):
    svg = open(os.path.join(BRAND, "optima_engine.svg")).read()
    m = re.search(r"crop origin (-?[\d.]+) (-?[\d.]+)", svg)
    ox, oy = float(m.group(1)), float(m.group(2))
    vb = parse(svg)[0]
    g = np.asarray(Image.open(src).convert("L"), dtype=np.float32) / 255.0
    H, W = g.shape
    cov = raster(svg, vb[2], offset=(ox, oy), canvas=(W, H))
    a, b = (1 - g) > 0.5, cov > 0.5
    iou = (a & b).sum() / max((a | b).sum(), 1)
    res = {"iou": iou}
    # per part: the dome (above the letters), the word OPTIMA, the ENGINE line
    rows = np.where(a.any(axis=1))[0]
    gaps = [r for r in range(rows[0], rows[-1]) if not a[r].any()]
    bands, start = [], rows[0]
    for r in gaps:
        if r == start:
            start = r + 1
            continue
        if a[start:r].any():
            bands.append((start, r))
        start = r + 1
    bands.append((start, rows[-1] + 1))
    bands = [bd for bd in bands if bd[1] - bd[0] > 3]
    for name, (r0, r1) in zip(("dome", "OPTIMA", "ENGINE + rules"), bands):
        aa, bb = a[r0:r1], b[r0:r1]
        res[name] = (aa & bb).sum() / max((aa | bb).sum(), 1)
    # mean absolute grey error (anti-aliased) over the logo's box
    ys, xs = np.where(a)
    box = (slice(ys.min(), ys.max() + 1), slice(xs.min(), xs.max() + 1))
    res["mean_abs_err"] = float(np.abs((1 - g)[box] - cov[box]).mean())
    print("SVG vs source raster: IoU %.4f (dome %.4f, OPTIMA %.4f, ENGINE %.4f), mean |grey error| %.4f, SVG %d bytes"
          % (res["iou"], res.get("dome", 0), res.get("OPTIMA", 0), res.get("ENGINE + rules", 0), res["mean_abs_err"], len(svg)))
    return 0 if iou >= 0.97 else 1


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--check":
        sys.exit(check(sys.argv[2]))
    svg = open(os.path.join(BRAND, "optima_engine_white.svg")).read()
    for w in (1024, 2048):
        out = os.path.join(BRAND, f"optima_engine_{w}.png")
        write_png(raster(svg, w), out)
        print("wrote", out)


if __name__ == "__main__":
    main()
