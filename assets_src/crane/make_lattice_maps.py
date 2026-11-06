#!/usr/bin/env python3
"""Paints the truss relief maps for the crane's silhouette-POM (SPOM) facades (SpomPS in src/shaders/mesh.hlsl).

Two-layer relief, the same encoding tools/building_gen/spom.py uses:
    albedo.a = where solid ENDS   (metres out from the wall / 1.2)      albedo.rgb = Gemini's yellow steel, shaded by the bar's roundness
    normal.a = where solid STARTS (metres out from the wall / 1.2)      normal.rgb = a gentle slope from the end layer
Every bar is a free-standing round tube: radius r about the plane z = PLANE, so start = PLANE - r*p and end = PLANE + r*p with
p = sqrt(1 - (d/r)^2). Seen up close the lattice reads as round pipe, with a real see-through silhouette.
Needs Pillow + numpy. Writes tex/lattice_<name>_albedo.png and tex/lattice_<name>_normal.png.
"""
import os
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DEPTH = 1.2        # kSpomDepth in mesh.hlsl
PLANE = 0.17       # metres out from the wall: the centre plane every bar is centred on
BOX_DEPTH = 0.34   # the facade box's depth: holds PLANE + the thickest bar (0.14)
STEEL = np.asarray(Image.open(os.path.join(HERE, "tex", "yellow_steel.jpg")).convert("RGB"), dtype=np.float32) / 255.0
STEEL_M = 3.0      # metres per steel texture repeat


def seg_dist(X, Y, a, b):
    ax, ay = a; bx, by = b
    dx, dy = bx - ax, by - ay
    t = np.clip(((X - ax) * dx + (Y - ay) * dy) / (dx * dx + dy * dy + 1e-12), 0.0, 1.0)
    return np.hypot(X - (ax + t * dx), Y - (ay + t * dy))


def paint(name, W, H, ppm, struts):
    """struts: list of ((x0,y0),(x1,y1),radius) in metres, origin lower-left of the facade."""
    pw, ph = int(round(W * ppm)), int(round(H * ppm))
    xs = (np.arange(pw) + 0.5) / ppm
    ys = H - (np.arange(ph) + 0.5) / ppm            # row 0 = top, matching uv.y = 1 - y/H
    X, Y = np.meshgrid(xs, ys)
    start = np.full((ph, pw), 9.0, np.float32)
    end = np.zeros((ph, pw), np.float32)
    shade = np.zeros((ph, pw), np.float32)
    for a, b, r in struts:
        d = seg_dist(X, Y, a, b)
        p = np.sqrt(np.clip(1.0 - (d / r) ** 2, 0.0, 1.0))
        on = p > 0.0
        start = np.where(on, np.minimum(start, PLANE - r * p), start)
        end = np.where(on, np.maximum(end, PLANE + r * p), end)
        shade = np.maximum(shade, p)
    start[start > 8.0] = 0.0                         # empty texels: end = start = 0
    # yellow steel tiled in metres, darker toward each bar's edge (a cheap roundness / AO hint)
    sh, sw, _ = STEEL.shape
    iu = ((X / STEEL_M) % 1.0 * sw).astype(int) % sw
    iv = ((Y / STEEL_M) % 1.0 * sh).astype(int) % sh
    rgb = STEEL[iv, iu] * (0.6 + 0.4 * shade)[..., None]
    # 8-bit alpha is 4.7 mm per step at kSpomDepth 1.2 m: dither before rounding so the round profile does not terrace
    rng = np.random.default_rng(7)
    def q8(v, solid):
        out = np.clip(np.floor(v / DEPTH * 255.0 + rng.random(v.shape)), 0, 255).astype(np.uint8)
        return np.where(solid, out, 0).astype(np.uint8)
    solid = shade > 0.0
    a_end = q8(end, solid)
    a_start = q8(start, solid)
    alb = np.dstack([np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8), a_end])
    e = end / DEPTH
    dx = (np.roll(e, -1, 1) - np.roll(e, 1, 1)) * 0.5
    dy = (np.roll(e, -1, 0) - np.roll(e, 1, 0)) * 0.5
    nrm = np.stack([-dx * 2.0, dy * 2.0, np.ones_like(e)], -1)
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
    nor = np.dstack([((nrm * 0.5 + 0.5) * 255).astype(np.uint8), a_start])
    Image.fromarray(alb, "RGBA").save(os.path.join(HERE, "tex", f"lattice_{name}_albedo.png"), optimize=True)
    Image.fromarray(nor, "RGBA").save(os.path.join(HERE, "tex", f"lattice_{name}_normal.png"), optimize=True)
    print(name, f"{pw}x{ph}", "end alpha max", int(a_end.max()), "start alpha max", int(a_start.max()))


def mast():
    W, H, bays = 2.0, 4.0, 1            # ONE 4 m bay: the facade is 11 of these, all sharing this texture
    s = []
    cr, br = 0.14, 0.09                              # chord / brace radius
    s.append(((0.16, -1.0), (0.16, H + 1.0), cr))
    s.append(((W - 0.16, -1.0), (W - 0.16, H + 1.0), cr))
    for i in range(bays + 1):
        y = i * H / bays
        s.append(((0.16, y), (W - 0.16, y), br))
    for i in range(bays):
        y0, y1 = i * H / bays, (i + 1) * H / bays
        s.append(((0.16, y0), (W - 0.16, y1), br))
        s.append(((0.16, y1), (W - 0.16, y0), br))
    paint("mast", W, H, 384, s)


def warren(W, H, bays, cr, br, name, ppm):
    s = [((-1, H - 0.12), (W + 1, H - 0.12), cr), ((-1, 0.12), (W + 1, 0.12), cr)]
    step = W / bays
    for i in range(bays + 1):
        s.append(((i * step, 0.12), (i * step, H - 0.12), br))
    for i in range(bays):
        a, b = i * step, (i + 1) * step
        if i % 2 == 0:
            s.append(((a, 0.12), (b, H - 0.12), br))
        else:
            s.append(((a, H - 0.12), (b, 0.12), br))
    paint(name, W, H, ppm, s)


if __name__ == "__main__":
    mast()
    warren(6.0, 1.9, 2, 0.11, 0.075, "jib", 300)
    warren(5.0, 1.2, 2, 0.10, 0.07, "cjib", 300)
