"""Bakes the in-game GUI's signed-distance-field font.

Input:  DejaVu Sans and DejaVu Sans Bold (Bitstream Vera / DejaVu licence: free to use, embed and redistribute with the
        licence notice; see assets/ui/LICENSE_DejaVu.txt). The copies shipped inside matplotlib are used by default.
Output: assets/ui/ui_font_sdf.png   one atlas, both weights, ASCII 32..126, single channel distance in all four channels
        src/gui/ui_font_metrics.h  generated metrics (advance, bearings, atlas rects, ascent, cap height, kerning)

Each glyph is rendered at 4x the bake size, the signed distance to the outline is measured at the atlas resolution
(exact Euclidean distance to the nearest boundary pixel of the opposite side, numpy, a few seconds), and mapped to
0..255 with 0.5 at the edge and +-SPREAD atlas pixels at 1 / 0. The runtime needs neither Pillow nor numpy.

    python tools/bake_ui_font.py [--font-dir <folder with DejaVuSans.ttf>]
"""
import argparse
import os
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

BAKE = 48        # em size in atlas pixels
SUPER = 4        # supersampling of the source raster
SPREAD = 6       # distance range in atlas pixels on each side of the edge
ATLAS_W = 512
FIRST, LAST = 32, 126

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_font_dir():
    try:
        import matplotlib
        return os.path.join(os.path.dirname(matplotlib.__file__), 'mpl-data', 'fonts', 'ttf')
    except ImportError:
        return None


def boundary(mask):
    """Pixels of `mask` that have a 4-neighbour outside it."""
    p = np.pad(mask, 1, constant_values=False)
    nb_out = ~p[:-2, 1:-1] | ~p[2:, 1:-1] | ~p[1:-1, :-2] | ~p[1:-1, 2:]
    return mask & nb_out


def sdf_glyph(font, ch):
    """Returns (sdf uint8 array, plane box in em units relative to pen/baseline, advance in em)."""
    hi = BAKE * SUPER
    adv = font.getlength(ch) / hi
    x0, y0, x1, y1 = font.getbbox(ch, anchor='ls')
    if x1 <= x0 or y1 <= y0:  # space
        return None, None, adv
    pad = SPREAD * SUPER + SUPER
    # Align the raster so that output pixels are whole SUPER x SUPER blocks.
    left = x0 - pad
    top = y0 - pad
    w = ((x1 - x0 + 2 * pad) + SUPER - 1) // SUPER * SUPER
    h = ((y1 - y0 + 2 * pad) + SUPER - 1) // SUPER * SUPER
    img = Image.new('L', (w, h), 0)
    ImageDraw.Draw(img).text((-left, -top), ch, font=font, anchor='ls', fill=255)
    mask = np.asarray(img) >= 128
    ow, oh = w // SUPER, h // SUPER
    # Sample positions: centres of the SUPER x SUPER blocks, in hi-res pixel coordinates.
    sy, sx = np.mgrid[0:oh, 0:ow]
    samples = np.stack([(sy * SUPER + (SUPER - 1) / 2.0).ravel(), (sx * SUPER + (SUPER - 1) / 2.0).ravel()], axis=1)
    cover = np.asarray(img, dtype=np.float32).reshape(oh, SUPER, ow, SUPER).mean(axis=(1, 3)).ravel() / 255.0
    inside_sample = cover >= 0.5
    b_in = np.argwhere(boundary(mask)).astype(np.float32)
    b_out = np.argwhere(boundary(~mask)).astype(np.float32)

    def nearest(points):
        best = np.full(len(samples), 1e9, dtype=np.float32)
        if len(points) == 0:
            return best
        for start in range(0, len(points), 2048):
            q = points[start:start + 2048]
            d2 = ((samples[:, None, :] - q[None, :, :]) ** 2).sum(axis=2)
            best = np.minimum(best, d2.min(axis=1))
        return np.sqrt(best)

    d_to_in, d_to_out = nearest(b_in), nearest(b_out)
    # Inside: distance to the nearest outside pixel; outside: to the nearest inside pixel. Half a pixel puts the edge
    # between the two pixel centres.
    signed = np.where(inside_sample, d_to_out - 0.5, -(d_to_in - 0.5)) / SUPER  # atlas pixels
    value = np.clip(0.5 + signed / (2.0 * SPREAD), 0.0, 1.0)
    sdf = (value * 255.0 + 0.5).astype(np.uint8).reshape(oh, ow)
    plane = (left / hi, top / hi, (left + w) / hi, (top + h) / hi)
    return sdf, plane, adv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--font-dir', default=default_font_dir())
    args = ap.parse_args()
    if not args.font_dir or not os.path.exists(os.path.join(args.font_dir, 'DejaVuSans.ttf')):
        sys.exit('DejaVuSans.ttf not found; pass --font-dir')
    faces = [('Regular', 'DejaVuSans.ttf'), ('Bold', 'DejaVuSans-Bold.ttf')]

    glyphs = []  # (face, code, sdf, plane, adv)
    face_info = []
    for fi, (name, file) in enumerate(faces):
        font = ImageFont.truetype(os.path.join(args.font_dir, file), BAKE * SUPER)
        ascent, descent = font.getmetrics()
        cap = -font.getbbox('H', anchor='ls')[1]
        xh = -font.getbbox('x', anchor='ls')[1]
        hi = float(BAKE * SUPER)
        kerning = []
        chars = [chr(c) for c in range(FIRST, LAST + 1)]
        base = {c: font.getlength(c) for c in chars}
        for a in chars:
            for b in chars:
                k = font.getlength(a + b) - base[a] - base[b]
                if abs(k) / hi >= 0.01:
                    kerning.append((ord(a), ord(b), k / hi))
        face_info.append(dict(name=name, ascent=ascent / hi, descent=descent / hi, cap=cap / hi, xheight=xh / hi, kerning=kerning))
        for c in range(FIRST, LAST + 1):
            sdf, plane, adv = sdf_glyph(font, chr(c))
            glyphs.append((fi, c, sdf, plane, adv))

    # Shelf packing, tallest first, 1 px gap.
    order = sorted([i for i, g in enumerate(glyphs) if g[2] is not None], key=lambda i: -glyphs[i][2].shape[0])
    x = y = shelf = 0
    place = {}
    for i in order:
        h, w = glyphs[i][2].shape
        if x + w > ATLAS_W:
            x, y, shelf = 0, y + shelf + 1, 0
        place[i] = (x, y)
        x += w + 1
        shelf = max(shelf, h)
    height = 1
    while height < y + shelf:
        height *= 2
    atlas = np.zeros((height, ATLAS_W), dtype=np.uint8)
    for i, (px, py) in place.items():
        s = glyphs[i][2]
        atlas[py:py + s.shape[0], px:px + s.shape[1]] = s

    out_png = os.path.join(ROOT, 'assets', 'ui', 'ui_font_sdf.png')
    os.makedirs(os.path.dirname(out_png), exist_ok=True)
    rgba = np.repeat(atlas[:, :, None], 4, axis=2)
    Image.fromarray(rgba, 'RGBA').save(out_png, optimize=True)
    lic = os.path.join(args.font_dir, 'LICENSE_DEJAVU')
    if os.path.exists(lic):
        shutil.copyfile(lic, os.path.join(ROOT, 'assets', 'ui', 'LICENSE_DejaVu.txt'))

    lines = []
    w = lines.append
    w('// Generated by tools/bake_ui_font.py - do not edit. Metrics of assets/ui/ui_font_sdf.png.')
    w('// Font: DejaVu Sans and DejaVu Sans Bold (Bitstream Vera / DejaVu licence, assets/ui/LICENSE_DejaVu.txt).')
    w('// All lengths are in em (multiply by the font size in pixels); y grows downwards from the baseline.')
    w('#pragma once')
    w('#include <cstdint>')
    w('')
    w('namespace tbd {')
    w('namespace gui {')
    w('namespace font_data {')
    w('')
    w(f'constexpr int kAtlasWidth = {ATLAS_W}, kAtlasHeight = {height};')
    w(f'constexpr float kBakeSize = {BAKE}.0f;   // em size in atlas pixels')
    w(f'constexpr float kSpread = {SPREAD}.0f;     // atlas pixels from the edge to value 0 or 1')
    w(f'constexpr int kFirst = {FIRST}, kLast = {LAST}, kFaces = {len(faces)};')
    w('')
    w('struct Glyph {')
    w('    float advance;            // em')
    w('    float x0, y0, x1, y1;     // quad relative to the pen on the baseline, em (0 size: nothing to draw)')
    w('    uint16_t u0, v0, u1, v1;  // atlas pixels')
    w('};')
    w('struct Kern {')
    w('    uint8_t a, b;')
    w('    float em;')
    w('};')
    w('struct Face {')
    w('    float ascent, descent, capHeight, xHeight; // em')
    w('    const Glyph* glyphs;                       // kLast - kFirst + 1')
    w('    const Kern* kerning;                       // sorted by (a, b)')
    w('    int kerningCount;')
    w('};')
    w('')
    for fi, info in enumerate(face_info):
        w(f'inline const Glyph kGlyphs{info["name"]}[] = {{')
        for key, (gfi, c, sdf, plane, adv) in enumerate(glyphs):
            if gfi != fi:
                continue
            if sdf is None:
                w(f'    {{{adv:.5f}f, 0, 0, 0, 0, 0, 0, 0, 0}}, // {c}')
            else:
                px, py = place[key]
                h, wd = sdf.shape
                ch = chr(c).replace('\\', 'backslash')
                w(f'    {{{adv:.5f}f, {plane[0]:.5f}f, {plane[1]:.5f}f, {plane[2]:.5f}f, {plane[3]:.5f}f, {px}, {py}, {px + wd}, {py + h}}}, // {ch}')
        w('};')
        kern = sorted(info['kerning'])
        w(f'inline const Kern kKerning{info["name"]}[] = {{')
        if not kern:
            w('    {0, 0, 0.0f},')
        for a, b, k in kern:
            w(f'    {{{a}, {b}, {k:.5f}f}},')
        w('};')
        w('')
    w('inline const Face kFaceList[kFaces] = {')
    for info in face_info:
        n = len(info['kerning'])
        w(f'    {{{info["ascent"]:.5f}f, {info["descent"]:.5f}f, {info["cap"]:.5f}f, {info["xheight"]:.5f}f, kGlyphs{info["name"]}, kKerning{info["name"]}, {n}}},')
    w('};')
    w('')
    w('} // namespace font_data')
    w('} // namespace gui')
    w('} // namespace tbd')
    out_h = os.path.join(ROOT, 'src', 'gui', 'ui_font_metrics.h')
    with open(out_h, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    total_k = sum(len(i['kerning']) for i in face_info)
    print(f'atlas {ATLAS_W}x{height}, {len(place)} glyphs, {total_k} kerning pairs -> {out_png}, {out_h}')


if __name__ == '__main__':
    main()
