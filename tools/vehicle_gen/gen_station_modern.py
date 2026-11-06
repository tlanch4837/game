"""Modern two-track passenger station -> GLB, companion to gen_station (same platform heights, helpers and track
conventions; different architecture).

Layout (y = 0 is the top of the rail; ground at gen_station.GROUND):
  track 1 centre line x = 0, track 2 centre line x = -XT (XT = 11.4 m).
  +x  side platform for track 1 with a glass pavilion under a thick white roof slab that cantilevers over the platform
      on slender columns (timber soffit), a dark-clad ticket hall block, a vertical totem sign, and steps + ramp to the street.
  between the tracks: an 8 m island platform under a butterfly (V) roof on a single row of white Y-columns.
  A glazed footbridge with glass lift/stair towers links the side platform to the island.
Run:  python gen_station_modern.py
Output: assets/rail/station/glb/STATION_modern_2track.glb
"""
import os
import numpy as np
from PIL import Image, ImageDraw
import gen_cars as gc
import gen_transit_tex as tx
import gen_station as st
from gen_station import (Mesh, quad, box, post, panel_x, panel_z, platform, bench, bench_l, lamp, bin_, departure_board,
                         PH, EDGE, Z0, Z1, GROUND, _noise, _jpg, _png)

NAME = "RIVERSIDE"
XT = 2 * EDGE + 8.0
st.SCALE.update({"curtain": 3.0, "white": 4.0, "cladding": 3.0, "timber": 2.0})


def textures():
    maps = st.textures()
    n = 512; T = {}
    yy = np.linspace(0, 1, n)[:, None]; xx = np.linspace(0, 1, n)[None, :]
    g = np.zeros((n, n, 3))
    g[..., 0] = 95 + 50 * yy + 10 * xx; g[..., 1] = 120 + 45 * yy + 10 * xx; g[..., 2] = 140 + 40 * yy
    g += _noise(n, 3, 11, 6)[..., None]
    for x in (0, n // 2): g[:, x:x + 8] = (45, 48, 52)                     # mullions every 1.5 m
    g[:8] = (45, 48, 52); g[n * 2 // 3:n * 2 // 3 + 6] = (45, 48, 52)      # transoms
    T["curtain"] = g
    a = np.full((n, n, 3), (226, 224, 218), float) + _noise(n, 5, 12, 2)[..., None]
    a[:, :2] *= 0.85; a[:2] *= 0.85
    T["white"] = a
    a = np.full((n, n, 3), (62, 64, 68), float) + _noise(n, 4, 13, 2)[..., None]
    for k in range(0, n, n // 2): a[:, k:k + 4] *= 0.6
    a[:4] *= 0.6
    T["cladding"] = a
    a = np.full((n, n, 3), (176, 128, 80), float) + _noise(n, 9, 14, 0.5)[..., None] * np.array([1, 0.8, 0.6])
    for k in range(0, n, n // 8): a[:, k:k + 5] *= 0.55
    T["timber"] = a
    for k, v in T.items(): maps[k] = (_jpg(Image.fromarray(v.clip(0, 255).astype(np.uint8))), None)
    im = Image.new("RGB", (1024, 128), (34, 36, 40)); d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 22, 127], fill=(0, 150, 140))
    d.text((60, 22), NAME, font=tx.font(78), fill=(250, 250, 250))
    maps["namesign2"] = (_png(im), None)
    return maps


def namesign(m, x, z, y=2.6):
    post(m, "white", x, z - 1.4, PH, PH + y + 0.5, 0.05); post(m, "white", x, z + 1.4, PH, PH + y + 0.5, 0.05)
    box(m, "cladding", x - 0.04, x + 0.04, PH + y - 0.02, PH + y + 0.52, z - 1.55, z + 1.55)
    panel_x(m, "namesign2", x + 0.07, 1, z - 1.5, z + 1.5, PH + y + 0.02, PH + y + 0.48)
    panel_x(m, "namesign2", x - 0.07, -1, z - 1.5, z + 1.5, PH + y + 0.02, PH + y + 0.48)


def island(m, xa, xb):
    """Island platform between the tracks, edges at x = xa (> xb): coping + tactile strip on both edges."""
    box(m, "concrete", xb, xa, -0.6, PH - 0.05, Z0, Z1, faces="fbnlr")
    box(m, "stone", xa - 0.45, xa, PH - 0.05, PH, Z0, Z1, faces="tfbr")
    box(m, "stone", xb, xb + 0.45, PH - 0.05, PH, Z0, Z1, faces="tfbl")
    box(m, "tactile", xa - 1.05, xa - 0.45, PH - 0.05, PH, Z0, Z1, faces="t")
    box(m, "tactile", xb + 0.45, xb + 1.05, PH - 0.05, PH, Z0, Z1, faces="t")
    box(m, "paving", xb + 1.05, xa - 1.05, PH - 0.05, PH, Z0, Z1, faces="t")
    for zc, d in ((Z0, -1), (Z1, 1)):                                       # end railings
        ze = zc - d * 0.2
        for x in np.linspace(xb + 0.3, xa - 0.3, 6): post(m, "white", x, ze, PH, PH + 1.1, 0.03)
        box(m, "white", xb + 0.3, xa - 0.3, PH + 1.05, PH + 1.12, ze - 0.03, ze + 0.03)


def butterfly(m, xc, half, z0, z1, h, pitch=10.0):
    """Island canopy: one row of white Y-columns, V-shaped roof with a centre gutter, timber soffit, light strips."""
    yg, ye = PH + h, PH + h + 0.9
    for z in np.arange(z0 + 3.0, z1 - 1.0, pitch):
        box(m, "white", xc - 0.18, xc + 0.18, PH, yg - 0.9, z - 0.18, z + 0.18)
        for s in (-1, 1):                                                   # Y arms
            P = [(xc, yg - 0.9, z - 0.12), (xc, yg - 0.9, z + 0.12), (xc + s * 2.6, yg + 0.33, z + 0.12), (xc + s * 2.6, yg + 0.33, z - 0.12)]
            quad(m, "white", P, (0, 1, 0)); quad(m, "white", P[::-1], (0, -1, 0))
            for zz, d in ((z - 0.12, -1), (z + 0.12, 1)):
                quad(m, "white", [(xc, yg - 0.9, zz), (xc + s * 2.6, yg + 0.33, zz), (xc + s * 2.6, yg + 0.10, zz), (xc + s * 0.25, yg - 0.9, zz)], (0, 0, d))
    for s in (-1, 1):
        xo = xc + s * half
        quad(m, "roof", [(xc, yg, z0), (xc, yg, z1), (xo, ye, z1), (xo, ye, z0)], (0, 1, 0))
        quad(m, "timber", [(xc, yg - 0.12, z0), (xc, yg - 0.12, z1), (xo, ye - 0.12, z1), (xo, ye - 0.12, z0)], (0, -1, 0))
        box(m, "white", xo - 0.06, xo + 0.06, ye - 0.35, ye + 0.05, z0, z1)
        for z, d in ((z0, -1), (z1, 1)):
            quad(m, "white", [(xc, yg - 0.12, z), (xc, yg, z), (xo, ye, z), (xo, ye - 0.12, z)], (0, 0, d))
        for z in np.arange(z0 + 8.0, z1 - 1.0, 10.0):                        # LED strips under the soffit
            xa_, xb_ = xc + s * 0.8, xc + s * (half - 0.6)
            ya_, yb_ = yg - 0.12 + 0.9 * 0.8 / half - 0.04, yg - 0.12 + 0.9 * (half - 0.6) / half - 0.04
            quad(m, "lamp", [(xa_, ya_, z - 0.06), (xb_, yb_, z - 0.06), (xb_, yb_, z + 0.06), (xa_, ya_, z + 0.06)], (0, -1, 0))
    box(m, "steel", xc - 0.25, xc + 0.25, yg - 0.15, yg + 0.03, z0, z1)        # gutter


def pavilion(m, x0, x1, z0, z1):
    H = PH + 6.0
    box(m, "white", x0 - 0.1, x1 + 0.1, GROUND, PH + 0.3, z0, z1)                                  # plinth
    box(m, "curtain", x0, x1, PH + 0.3, H, z0, z1, faces="lrfb")                                   # glazed hall
    xo = EDGE + 1.6                                                                                 # roof slab
    box(m, "white", xo, x1 + 2.0, H, H + 0.7, z0 - 2.0, z1 + 2.0, faces="tfblr")
    quad(m, "timber", [(xo, H, z0 - 2.0), (x1 + 2.0, H, z0 - 2.0), (x1 + 2.0, H, z1 + 2.0), (xo, H, z1 + 2.0)], (0, -1, 0))
    for z in np.arange(z0 + 2.0, z1, 8.0): post(m, "white", EDGE + 3.4, z, PH, H, 0.12)
    for zc in (z0 + 8.0, z1 - 8.0):                                                                 # platform doors
        panel_x(m, "entrance", x0 - 0.03, -1, zc - 1.5, zc + 1.5, PH + 0.3, PH + 3.2)
    # dark-clad ticket hall / office block at the +z end
    bz0, bz1, bh = z1 + 2.0, z1 + 11.0, PH + 10.5
    box(m, "cladding", x0 + 1.0, x1, GROUND, bh, bz0, bz1)
    box(m, "white", x0 + 0.8, x1 + 0.2, bh, bh + 0.4, bz0 - 0.2, bz1 + 0.2, faces="tfblr")
    for y0 in (PH + 4.0, PH + 7.4):                                                                 # ribbon windows
        quad(m, "curtain", [(x0 + 0.97, y0, bz0 + 0.8), (x0 + 0.97, y0, bz1 - 0.8), (x0 + 0.97, y0 + 2.2, bz1 - 0.8), (x0 + 0.97, y0 + 2.2, bz0 + 0.8)], (-1, 0, 0))
        quad(m, "curtain", [(x1 + 0.03, y0, bz0 + 0.8), (x1 + 0.03, y0, bz1 - 0.8), (x1 + 0.03, y0 + 2.2, bz1 - 0.8), (x1 + 0.03, y0 + 2.2, bz0 + 0.8)], (1, 0, 0))
        quad(m, "curtain", [(x0 + 1.8, y0, bz1 + 0.03), (x1 - 0.8, y0, bz1 + 0.03), (x1 - 0.8, y0 + 2.2, bz1 + 0.03), (x0 + 1.8, y0 + 2.2, bz1 + 0.03)], (0, 0, 1))
    # totem sign at the street corner, lettering running bottom -> top on both faces
    tx_, tz = x1 + 4.5, z0 - 1.0
    box(m, "cladding", tx_ - 0.5, tx_ + 0.5, GROUND, GROUND + 9.0, tz - 0.5, tz + 0.5)
    for s in (-1, 1):
        xf = tx_ + s * 0.53; za, zb = (tz + 0.45, tz - 0.45) if s > 0 else (tz - 0.45, tz + 0.45)
        quad(m, "namesign2", [(xf, GROUND + 1.0, za), (xf, GROUND + 1.0, zb), (xf, GROUND + 8.5, zb), (xf, GROUND + 8.5, za)],
             (s, 0, 0), [(0, 0), (0, 1), (1, 1), (1, 0)])
    # street side: entrance doors, name band on the slab edge, broad steps, and a ramp
    zc = (z0 + z1) / 2
    for dz in (-3.5, 0.0, 3.5): panel_x(m, "entrance", x1 + 0.03, 1, zc + dz - 1.5, zc + dz + 1.5, PH + 0.3, PH + 3.2)
    panel_x(m, "namesign2", x1 + 2.03, 1, zc - 6.0, zc + 6.0, H + 0.08, H + 0.62)
    for k in range(1, 6):
        box(m, "white", x1, x1 + 2.0 + k * 0.35, GROUND, PH + 0.3 - k * (PH + 0.3 - GROUND) / 6, zc - 7.0, zc + 2.0)
    box(m, "white", x1, x1 + 2.0, GROUND, PH + 0.3, zc - 7.0, z1)
    quad(m, "paving", [(x1 + 2.0, PH + 0.3, zc + 2.0), (x1 + 4.0, PH + 0.3, zc + 2.0), (x1 + 4.0, GROUND, z1), (x1 + 2.0, GROUND, z1)], (0, 1, 0.3))


def glass_bridge(m, zc, xa, xb, w=3.2):
    """Footbridge: glass lift/stair towers with white corner posts, glazed span on a white box girder."""
    yd = PH + 6.6
    for xc in (xa, xb):
        box(m, "curtain", xc - 1.8, xc + 1.8, PH, yd + 3.0, zc - 3.0, zc + 3.0, faces="fblr")
        box(m, "white", xc - 2.0, xc + 2.0, yd + 3.0, yd + 3.4, zc - 3.2, zc + 3.2, bottom=True)
        for x in (xc - 1.8, xc + 1.8):
            for z in (zc - 3.0, zc + 3.0): post(m, "white", x, z, PH, yd + 3.0, 0.08)
        for s in (-1, 1): panel_z(m, "entrance", zc + s * 3.03, s, xc - 1.2, xc + 1.2, PH, PH + 3.0)
    x0, x1 = min(xa, xb) + 1.8, max(xa, xb) - 1.8
    box(m, "white", x0, x1, yd - 0.9, yd, zc - w / 2, zc + w / 2, bottom=True)
    for s in (-1, 1):
        box(m, "curtain", x0, x1, yd, yd + 2.6, zc + s * w / 2 - 0.02, zc + s * w / 2 + 0.02, faces="fb")
    box(m, "white", x0, x1, yd + 2.6, yd + 2.9, zc - w / 2 - 0.15, zc + w / 2 + 0.15, bottom=True)


def build():
    m = Mesh()
    XS = 8.0
    platform(m, 1, EDGE + XS)
    island(m, -EDGE, -(XT - EDGE))
    pavilion(m, EDGE + XS, EDGE + XS + 12.0, -28.0, 6.0)
    xi = -XT / 2
    butterfly(m, xi, 3.9, -55.0, 30.0, 3.6)
    for k, z in enumerate(np.arange(-47.0, 30.0, 10.0)):                 # island furniture between the columns
        if k % 3 == 1: departure_board(m, xi, z)
        else: bench(m, xi + 0.9, z); bench_l(m, xi - 0.9, z)
    for z in (-58.0, 36.0): namesign(m, xi, z)
    for z in (-50.0, -20.0, 20.0): namesign(m, EDGE + 2.6, z, y=3.1)
    for z in (-56.0, -38.0, 14.0, 26.0, 36.0): lamp(m, EDGE + 4.0, z)
    for z in (-34.0, 12.0): bin_(m, EDGE + 5.4, z)
    for z in (-22.0, -10.0): bench_l(m, EDGE + 4.6, z)
    glass_bridge(m, -44.0, EDGE + 4.6, xi)
    return m


def main():
    os.makedirs(st.OUT, exist_ok=True)
    maps = textures()
    gc.MAT_PBR.update({"concrete": (0, 0.9), "paving": (0, 0.85), "stone": (0, 0.8), "roof": (0.6, 0.45), "tactile": (0, 0.7),
                       "steel": (0.5, 0.4), "wood": (0, 0.7), "board": (0, 0.3), "entrance": (0.3, 0.3), "lamp": (0, 0.5),
                       "curtain": (0.6, 0.08), "white": (0, 0.6), "cladding": (0.5, 0.5), "timber": (0, 0.7), "namesign2": (0.1, 0.4)})
    m = build()
    out = os.path.join(st.OUT, "STATION_modern_2track.glb")
    gc.write_glb(out, m, maps, (1, 1, 1))
    print(f"modern_2track: {m.count()} tris, {os.path.getsize(out) / 1e6:.2f} MB -> {out}")


if __name__ == "__main__":
    main()
