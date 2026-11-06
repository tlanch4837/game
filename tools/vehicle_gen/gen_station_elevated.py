"""Elevated (over-ground) subway station -> GLB, companion to gen_station / gen_station_modern.

Frame: y = 0 is the top of the rail on the viaduct (gen_track pieces and the SUBWAY_car GLBs drop straight in);
the street is at y = -ELEV. Two tracks, x = 0 and x = -XS, each with an outside side platform 80 m long.
  - concrete box-girder viaduct deck on twin round piers with crossheads, every 16 m, beyond the station too
  - side platforms (1.10 m, coping + tactile strip) with steel gable canopies, glass windscreens with a steel rail
    on the outer edge, benches, lamps, name boards, arrival screens
  - fare-control mezzanine hung under the deck (glazed, dark-clad ends), stair heads up to each platform
  - street stairs down both sides of the street, a glass lift shaft on the +x side, entrance globe-lamps
Run:  python gen_station_elevated.py
Output: assets/rail/station/glb/STATION_elevated_subway.glb
"""
import os
import numpy as np
from PIL import Image, ImageDraw
import gen_cars as gc
import gen_transit_tex as tx
import gen_station as st
import gen_station_modern as md
from gen_station import Mesh, quad, box, post, panel_x, panel_z, bench, bench_l, lamp, bin_, departure_board, PH, EDGE, _png

NAME = "ELM STREET"
ELEV = 9.0                       # rail top above the street
STREET = -ELEV
DECK = -0.55                     # deck top (track formation bottom)
XS = 4.0                         # track spacing
PW = 4.6                         # platform width
Z0, Z1 = -50.0, 30.0             # platform ends
V0, V1 = -90.0, 70.0             # viaduct extent
MEZ0, MEZ1 = DECK - 1.6, DECK - 5.6   # mezzanine ceiling / floor
XA, XB = EDGE + PW, -XS - EDGE - PW  # outer platform edges (+x / -x)


def textures():
    maps = md.textures()
    im = Image.new("RGB", (1024, 128), (16, 16, 18)); d = ImageDraw.Draw(im)
    d.ellipse([18, 14, 118, 114], fill=(0, 147, 60)); d.text((50, 22), "G", font=tx.font(74), fill=(255, 255, 255))
    d.text((150, 22), NAME, font=tx.font(78), fill=(250, 250, 250))
    maps["namesign3"] = (_png(im), None)
    return maps


def stairs(m, x0, x1, za, zb, y0, y1, mat="concrete", tread="paving"):
    """Straight flight from (za, y0) up to (zb, y1) across x0..x1: sawtooth treads on solid side stringers."""
    n = max(2, int(round(abs(y1 - y0) / 0.18))); d = 1 if zb > za else -1
    for k in range(n):
        z_a, z_b = za + (zb - za) * k / n, za + (zb - za) * (k + 1) / n; y = y0 + (y1 - y0) * (k + 1) / n
        yp = y0 + (y1 - y0) * k / n
        quad(m, mat, [(x0, yp, z_a), (x1, yp, z_a), (x1, y, z_a), (x0, y, z_a)], (0, 0, -d))   # riser
        quad(m, tread, [(x0, y, z_a), (x1, y, z_a), (x1, y, z_b), (x0, y, z_b)], (0, 1, 0))      # tread
    for x, s in ((x0, -1), (x1, 1)):                                                            # stringers
        quad(m, mat, [(x, y0 - 0.4, za), (x, y1 - 0.4, zb), (x, y1 + 0.15, zb), (x, y0 + 0.15, za)], (s, 0, 0))
        quad(m, mat, [(x, y0 - 0.4, za), (x, y1 - 0.4, zb), (x, y1 + 0.15, zb), (x, y0 + 0.15, za)], (-s, 0, 0))
    quad(m, mat, [(x0, y0 - 0.4, za), (x1, y0 - 0.4, za), (x1, y1 - 0.4, zb), (x0, y1 - 0.4, zb)], (0, -1, -d * 0.5))  # soffit
    for x in (x0 + 0.08, x1 - 0.08):                                                            # handrails
        quad(m, "steel", [(x, y0 + 0.95, za), (x, y1 + 0.95, zb), (x, y1 + 1.0, zb), (x, y0 + 1.0, za)], (1, 0, 0))
        quad(m, "steel", [(x, y0 + 0.95, za), (x, y1 + 0.95, zb), (x, y1 + 1.0, zb), (x, y0 + 1.0, za)], (-1, 0, 0))
        for t_ in (0.0, 0.5, 1.0):
            post(m, "steel", x, za + (zb - za) * t_, y0 + (y1 - y0) * t_, y0 + (y1 - y0) * t_ + 1.0, 0.025)


def viaduct(m):
    x0, x1 = XB - 0.6, XA + 0.6
    xm = (x0 + x1) / 2
    box(m, "white", x0, x1, DECK - 1.2, DECK, V0, V1, bottom=True)                              # deck slab
    for xg in (x0 + 1.2, xm - 1.6, xm + 1.6, x1 - 1.2):                                         # box girders
        box(m, "concrete", xg - 0.9, xg + 0.9, DECK - 2.6, DECK - 1.2, V0, V1, bottom=True)
    for z in np.arange(V0 + 8.0, V1, 16.0):
        if Z0 - 2 < z < Z1 + 2 and abs(z + 10.0) < 14.0: continue                               # mezzanine there: hangers
        box(m, "concrete", x0 + 0.5, x1 - 0.5, DECK - 3.6, DECK - 2.6, z - 1.0, z + 1.0, bottom=True)   # crosshead
        for xp in (x0 + 2.2, x1 - 2.2):
            post(m, "concrete", xp, z, STREET, DECK - 3.6, 0.65)
            box(m, "concrete", xp - 0.9, xp + 0.9, STREET, STREET + 0.4, z - 0.9, z + 0.9)        # footing
    for z in (V0, V1):                                                                           # outer parapets
        pass
    for x, s in ((x0, -1), (x1, 1)):
        for za, zb in ((V0, Z0), (Z1, V1)):                                                       # parapet beyond the platforms
            box(m, "white", x, x - s * 0.25, DECK, DECK + 1.2, za, zb)


def platform(m, xe, xo):
    """Side platform on the deck: edge at xe, back at xo."""
    s = 1 if xo > xe else -1
    box(m, "concrete", xe, xo, DECK, PH - 0.05, Z0, Z1, faces="fbnlr")
    box(m, "stone", xe, xe + s * 0.45, PH - 0.05, PH, Z0, Z1, faces="tfb" + ("l" if s > 0 else "r"))
    box(m, "tactile", xe + s * 0.45, xe + s * 1.05, PH - 0.05, PH, Z0, Z1, faces="t")
    box(m, "paving", xe + s * 1.05, xo, PH - 0.05, PH, Z0, Z1, faces="t")
    # windscreen along the back: concrete upstand, glass, steel top rail and posts
    box(m, "concrete", xo - s * 0.2, xo, PH, PH + 0.6, Z0, Z1)
    for side in (1, -1):
        panel = lambda za, zb: quad(m, "glass", [(xo - s * 0.1 + side * 0.02, PH + 0.6, za), (xo - s * 0.1 + side * 0.02, PH + 0.6, zb),
                                                 (xo - s * 0.1 + side * 0.02, PH + 2.4, zb), (xo - s * 0.1 + side * 0.02, PH + 2.4, za)], (side, 0, 0))
        for za in np.arange(Z0, Z1, 4.0): panel(za, za + 4.0)
    for z in np.arange(Z0, Z1 + 0.1, 4.0): post(m, "steel", xo - s * 0.1, z, PH, PH + 2.5, 0.05)
    box(m, "steel", xo - s * 0.18, xo - s * 0.02, PH + 2.4, PH + 2.5, Z0, Z1)
    for z, d in ((Z0, -1), (Z1, 1)):                                                             # end railings
        x0_, x1_ = sorted((xe + s * 0.3, xo - s * 0.2))
        box(m, "steel", x0_, x1_, PH + 1.05, PH + 1.12, z - 0.03, z + 0.03)
        for x in np.linspace(x0_, x1_, 4): post(m, "steel", x, z, PH, PH + 1.1, 0.03)


def canopy(m, xe, xo, z0, z1, pitch=8.0):
    """Steel gable canopy over a side platform: columns near the back, roof pitched up toward the track (rain to the back)."""
    s = 1 if xo > xe else -1
    xc = xo - s * 1.0; yb, yf = PH + 3.4, PH + 4.2
    xf = xe + s * 0.4
    for z in np.arange(z0 + 2.0, z1, pitch):
        post(m, "steel", xc, z, PH, yb + 0.25, 0.11)
        quad(m, "steel", [(xc, yb - 0.6, z - 0.05), (xc, yb + 0.25, z - 0.05), (xf, yf, z - 0.05), (xf, yf - 0.3, z - 0.05)], (0, 0, -1))
        quad(m, "steel", [(xc, yb - 0.6, z + 0.05), (xc, yb + 0.25, z + 0.05), (xf, yf, z + 0.05), (xf, yf - 0.3, z + 0.05)], (0, 0, 1))
    xb = xo + s * 0.3
    quad(m, "roof", [(xb, yb + 0.25, z0), (xb, yb + 0.25, z1), (xf, yf + 0.05, z1), (xf, yf + 0.05, z0)], (0, 1, 0))
    quad(m, "roof", [(xb, yb + 0.18, z0), (xb, yb + 0.18, z1), (xf, yf - 0.02, z1), (xf, yf - 0.02, z0)], (0, -1, 0))
    box(m, "white", xf - 0.08, xf + 0.08, yf - 0.4, yf + 0.1, z0, z1)                           # fascia (track side)
    box(m, "steel", xb - 0.1, xb + 0.1, yb, yb + 0.3, z0, z1)                                   # gutter
    for z in np.arange(z0 + 6.0, z1, 8.0):                                                      # light boxes
        box(m, "lamp", xe + s * 1.6 - 0.1, xe + s * 1.6 + 0.1, yf - 0.55, yf - 0.45, z - 1.0, z + 1.0, bottom=True, faces="")


def namesign(m, xb, z, s):
    """Name board hung from the canopy, readable from the train (faces the track) and from the platform."""
    x = xb
    box(m, "steel", x - 0.04, x + 0.04, PH + 2.35, PH + 2.95, z - 1.55, z + 1.55)
    panel_x(m, "namesign3", x + 0.07, 1, z - 1.5, z + 1.5, PH + 2.4, PH + 2.9)
    panel_x(m, "namesign3", x - 0.07, -1, z - 1.5, z + 1.5, PH + 2.4, PH + 2.9)
    for dz in (-1.2, 1.2): post(m, "steel", x, z + dz, PH + 2.95, PH + 3.6, 0.02)


def mezzanine(m, z0, z1):
    """Fare-control level hung under the deck: glass sides, dark-clad ends, hanger columns, stair heads to the platforms."""
    x0, x1 = XB - 0.4, XA + 0.4
    box(m, "white", x0, x1, MEZ1 - 0.5, MEZ1, z0, z1, bottom=True)                              # floor slab
    for s, x in ((-1, x0), (1, x1)):
        box(m, "curtain", x - s * 0.05, x, MEZ1, MEZ0, z0, z1, faces="lr")
    for z, d in ((z0, -1), (z1, 1)):
        box(m, "cladding", x0, x1, MEZ1, MEZ0, z - d * 0.05, z, faces="fb")
        panel_z(m, "namesign3", z + d * 0.03, d, -XS / 2 - 3.0, -XS / 2 + 3.0, MEZ0 - 1.0, MEZ0 - 0.25)
    for xp in (x0 + 2.2, x1 - 2.2):                                                              # piers through the hall
        for z in (z0 + 2.0, z1 - 2.0): post(m, "concrete", xp, z, STREET, DECK - 2.6, 0.65)
    box(m, "concrete", x0 + 0.5, x1 - 0.5, MEZ0, DECK - 2.6, z0 + 1.0, z0 + 3.0, bottom=True)
    box(m, "concrete", x0 + 0.5, x1 - 0.5, MEZ0, DECK - 2.6, z1 - 3.0, z1 - 1.0, bottom=True)
    for xp in (x0 + 2.2, x1 - 2.2):
        for z in (z0 + 2.0, z1 - 2.0): box(m, "concrete", xp - 0.9, xp + 0.9, STREET, STREET + 0.4, z - 0.9, z + 0.9)
    # stair heads on each platform: glass enclosure open toward the platform, roof, "exit" frame
    for xe, xo in ((EDGE, XA), (-XS - EDGE, XB)):
        s = 1 if xo > xe else -1; xc = (xe + s * 1.6 + xo - s * 0.4) / 2; w = 1.1
        zs0, zs1 = z0 + 6.0, z0 + 13.0
        for x in (xc - w, xc + w):
            quad(m, "glass", [(x, PH, zs0), (x, PH, zs1), (x, PH + 2.3, zs1), (x, PH + 2.3, zs0)], (1, 0, 0))
            quad(m, "glass", [(x, PH, zs0), (x, PH, zs1), (x, PH + 2.3, zs1), (x, PH + 2.3, zs0)], (-1, 0, 0))
            box(m, "steel", x - 0.05, x + 0.05, PH + 2.3, PH + 2.45, zs0, zs1)
        panel_z(m, "glass", zs0, -1, xc - w, xc + w, PH, PH + 2.3); panel_z(m, "glass", zs0 + 0.03, 1, xc - w, xc + w, PH, PH + 2.3)
        box(m, "steel", xc - w - 0.1, xc + w + 0.1, PH + 2.45, PH + 2.55, zs0 - 0.1, zs1 + 0.1, bottom=True)
        quad(m, "cladding", [(xc - w + 0.05, PH + 0.002, zs0 + 0.05), (xc + w - 0.05, PH + 0.002, zs0 + 0.05), (xc + w - 0.05, PH + 0.002, zs1), (xc - w + 0.05, PH + 0.002, zs1)], (0, 1, 0))
        stairs(m, xc - w + 0.1, xc + w - 0.1, zs1 - 0.5, zs0 + 0.5, MEZ0 + 0.4, PH - 0.4)       # flight seen under the deck


def street_access(m, z0):
    """Street stairs on both sides up to the mezzanine, a glass lift shaft on the +x side, entrance globe lamps."""
    for x, s in ((XA + 0.4, 1), (XB - 0.4, -1)):
        xa, xb = sorted((x + s * 0.3, x + s * 2.6))
        stairs(m, xa, xb, z0 - 14.0, z0 + 2.0, STREET, MEZ1, "white")
        box(m, "white", xa, xb, MEZ1 - 0.5, MEZ1, z0 + 2.0, z0 + 6.0, bottom=True)              # landing into the hall
        for z in (z0 + 2.0, z0 + 6.0): post(m, "white", (xa + xb) / 2, z, STREET, MEZ1 - 0.5, 0.18)
        quad(m, "roof", [(xa - 0.2, MEZ1 + 2.6, z0 - 14.0), (xb + 0.2, MEZ1 + 2.6, z0 - 14.0), (xb + 0.2, MEZ1 + 2.6, z0 + 6.0), (xa - 0.2, MEZ1 + 2.6, z0 + 6.0)], (0, 1, 0))
        quad(m, "roof", [(xa - 0.2, MEZ1 + 2.5, z0 - 14.0), (xb + 0.2, MEZ1 + 2.5, z0 - 14.0), (xb + 0.2, MEZ1 + 2.5, z0 + 6.0), (xa - 0.2, MEZ1 + 2.5, z0 + 6.0)], (0, -1, 0))
        for z in np.linspace(z0 - 4.0, z0 + 6.0, 3): post(m, "steel", xb if s > 0 else xa, z, MEZ1, MEZ1 + 2.55, 0.06)
        for xx in (xa, xb):                                                                       # entrance lamps at the foot
            post(m, "steel", xx, z0 - 14.6, STREET, STREET + 2.6, 0.05)
            box(m, "lamp", xx - 0.18, xx + 0.18, STREET + 2.6, STREET + 2.95, z0 - 14.78, z0 - 14.42, bottom=True)
        panel_z(m, "namesign3", z0 - 14.05, -1, xa, xb, MEZ1 + 1.8, MEZ1 + 2.45)
    # lift shaft from the street to the +x platform
    lx, lz = XA + 2.6, z0 + 10.0
    box(m, "curtain", lx - 1.1, lx + 1.1, STREET, PH + 3.0, lz - 1.1, lz + 1.1)
    box(m, "white", lx - 1.25, lx + 1.25, PH + 3.0, PH + 3.3, lz - 1.25, lz + 1.25, bottom=True)
    for x in (lx - 1.1, lx + 1.1):
        for z in (lz - 1.1, lz + 1.1): post(m, "white", x, z, STREET, PH + 3.0, 0.07)
    box(m, "paving", XA, lx - 1.1, PH - 0.05, PH, lz - 1.0, lz + 1.0, bottom=True)                # bridge to the platform
    panel_x(m, "entrance", lx + 1.13, 1, lz - 0.9, lz + 0.9, STREET, STREET + 2.4)


def build():
    m = Mesh()
    viaduct(m)
    platform(m, EDGE, XA); platform(m, -XS - EDGE, XB)
    canopy(m, EDGE, XA, Z0 + 6.0, Z1 - 6.0); canopy(m, -XS - EDGE, XB, Z0 + 6.0, Z1 - 6.0)
    mz0, mz1 = -24.0, 4.0
    mezzanine(m, mz0, mz1)
    street_access(m, mz0)
    for xe, xo in ((EDGE, XA), (-XS - EDGE, XB)):
        s = 1 if xo > xe else -1; xm = xe + s * 2.6
        for z in (Z0 + 10.0, -2.0, Z1 - 10.0): namesign(m, xm, z, s)
        for z in (Z0 + 3.0, Z1 - 3.0): lamp(m, xo - s * 0.6, z)
        for z in (-36.0, 14.0):
            (bench_l if s > 0 else bench)(m, xo - s * 0.9, z)
            (bench_l if s > 0 else bench)(m, xo - s * 0.9, z + 3.0)
        bin_(m, xo - s * 0.6, 9.0)
        departure_board(m, xm, -30.0)
    return m


def main():
    os.makedirs(st.OUT, exist_ok=True)
    maps = textures()
    gc.MAT_PBR.update({"concrete": (0, 0.9), "paving": (0, 0.85), "stone": (0, 0.8), "roof": (0.6, 0.45), "tactile": (0, 0.7),
                       "steel": (0.5, 0.4), "wood": (0, 0.7), "board": (0, 0.3), "entrance": (0.3, 0.3), "lamp": (0, 0.5), "glass": (0.1, 0.1),
                       "curtain": (0.6, 0.08), "white": (0, 0.6), "cladding": (0.5, 0.5), "timber": (0, 0.7), "namesign3": (0.1, 0.4)})
    m = build()
    out = os.path.join(st.OUT, "STATION_elevated_subway.glb")
    gc.write_glb(out, m, maps, (1, 1, 1))
    print(f"elevated_subway: {m.count()} tris, {os.path.getsize(out) / 1e6:.2f} MB -> {out}")


if __name__ == "__main__":
    main()
