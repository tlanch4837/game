"""Underground subway station -> GLB, companion to the other gen_station_* generators.

Frame: y = 0 is the top of the rail (gen_track pieces and SUBWAY_car GLBs drop in); the street is at y = DEPTH.
Built as a cut-away: every below-ground surface (hall walls, barrel vault, end walls, running tunnels, stair shaft)
faces INWARD only, so from outside / above (through a translucent ground) you look straight into the station, while
from inside it reads as a closed box. Above ground everything is ordinary outward-facing geometry.

  - two tracks (x = 0 and x = -XT) in a tiled hall under a barrel vault, an island platform between them
  - tiled walls with a coloured frieze, station name panels and posters on the track walls
  - a row of steel columns down the island, LED strips in the vault, benches, bins, arrival boards
  - running tunnels continuing out of both end walls
  - a straight stairway (inclined shaft with landings, handrails, lighting) rising from the island to the street
  - an NYC-style street entrance: open stairwell, green cast-iron railings, globe lamps, SUBWAY plates
Run:  python gen_station_underground.py
Output: assets/rail/station/glb/STATION_underground_subway.glb
"""
import math, os
import numpy as np
from PIL import Image, ImageDraw
import gen_cars as gc
import gen_transit_tex as tx
import gen_station as st
import gen_station_modern as md
import gen_station_elevated as ev
from gen_station import Mesh, quad, box, post, panel_x, panel_z, bench, bench_l, bin_, departure_board, PH, EDGE, _png, _jpg, _noise

NAME = "CENTRAL"
DEPTH = 11.0                     # street above the rail top
FLOOR = -0.55                    # track bed
XT = 2 * EDGE + 7.0              # second track centre line at x = -XT (7 m island)
XR, XL = 2.6, -XT - 2.6          # hall side walls
WALL_H, VAULT = 5.0, 3.0         # wall height above the track bed, vault rise
Z0, Z1 = -50.0, 30.0             # hall ends
XI = -XT / 2                     # island centre
SW = 3.4                         # stair width
ZWELL = 0.0                      # z where the stair shaft opens to the street (set by stairway())
SZ0 = -12.0                      # foot of the stair (on the island), rising toward -z
SLOPE = 0.6                      # rise / run (about 31 degrees)
SZ1 = SZ0 - (DEPTH - PH) / SLOPE # head of the stair at street level
st.SCALE.update({"tile": 1.2, "dark": 4.0})


def textures():
    maps = ev.textures()
    n = 512
    # wall tiles: 1.2 m tile of 150 x 75 mm glazed bricks, a coloured band at a fixed height (v wraps, so drawn by geometry)
    a = np.full((n, n, 3), (236, 236, 230), float) + _noise(n, 3, 21, 1)[..., None]
    rows, cols = 16, 8
    for r in range(rows):
        y = int(r * n / rows); a[y:y + 2] = (170, 170, 165)
        off = (r % 2) * n // cols // 2
        for c in range(cols + 1):
            x = (int(c * n / cols) + off) % n; a[y:y + n // rows, x:x + 2] = (170, 170, 165)
    maps["tile"] = (_jpg(Image.fromarray(a.clip(0, 255).astype(np.uint8))), None)
    a = np.full((64, 64, 3), (26, 26, 28), float) + _noise(64, 4, 22)[..., None]
    maps["dark"] = (_jpg(Image.fromarray(a.clip(0, 255).astype(np.uint8))), None)
    a = np.full((n, n, 3), (0, 92, 170), float) + _noise(n, 3, 23, 1)[..., None]       # frieze band tiles
    for k in range(0, n, n // 8): a[:, k:k + 2] *= 0.7
    a[:2] *= 0.7
    maps["band"] = (_jpg(Image.fromarray(a.clip(0, 255).astype(np.uint8))), None)
    # station name panel: blue roundel bar over white enamel, "CENTRAL"
    im = Image.new("RGB", (1024, 256), (245, 245, 240)); d = ImageDraw.Draw(im)
    d.ellipse([352, 8, 672, 248], outline=(200, 20, 30), width=34)
    d.rectangle([150, 92, 874, 164], fill=(0, 60, 140))
    f = tx.font(60); w = d.textlength(NAME, font=f); d.text(((1024 - w) / 2, 94), NAME, font=f, fill=(255, 255, 255))
    maps["namesign4"] = (_png(im), None)
    # posters: four framed bold colour-block posters in one atlas row
    im = Image.new("RGB", (1024, 256), (30, 30, 30)); d = ImageDraw.Draw(im)
    pal = [((230, 80, 40), (250, 210, 60)), ((30, 140, 200), (240, 240, 240)), ((40, 160, 90), (250, 240, 200)), ((120, 50, 160), (255, 190, 220))]
    for i, (bg, fg) in enumerate(pal):
        x0 = i * 256 + 10; d.rectangle([x0, 10, x0 + 236, 246], fill=bg)
        d.ellipse([x0 + 40, 40, x0 + 196, 160], fill=fg); d.rectangle([x0 + 24, 186, x0 + 212, 206], fill=fg); d.rectangle([x0 + 24, 216, x0 + 150, 230], fill=fg)
    maps["posters"] = (_png(im), None)
    # METRO sign for the entrance totem
    im = Image.new("RGB", (512, 128), (0, 60, 140)); d = ImageDraw.Draw(im)
    d.text((100, 18), "METRO", font=tx.font(84), fill=(255, 255, 255)); d.ellipse([16, 24, 84, 104], fill=(200, 20, 30))
    maps["metro"] = (_png(im), None)
    # NYC set: dark-green cast iron, green globe glass, black SUBWAY plate with route bullets
    a = np.full((64, 64, 3), (24, 64, 44), float) + _noise(64, 4, 24)[..., None]
    maps["nycgreen"] = (_jpg(Image.fromarray(a.clip(0, 255).astype(np.uint8))), None)
    maps["beacon"] = (_jpg(Image.new("RGB", (16, 16), (40, 125, 65))), None)
    im = Image.new("RGB", (1024, 192), (12, 12, 12)); d = ImageDraw.Draw(im)
    d.line([(20, 30), (1004, 30)], fill=(240, 240, 240), width=4)
    d.text((40, 48), "SUBWAY", font=tx.font(96), fill=(250, 250, 250))
    for k, (ch, col) in enumerate((("4", (0, 147, 60)), ("5", (0, 147, 60)), ("6", (0, 147, 60)), ("N", (252, 204, 10)), ("Q", (252, 204, 10)))):
        cx = 560 + k * 92; d.ellipse([cx - 40, 56, cx + 40, 136], fill=col)
        f = tx.font(56); w = d.textlength(ch, font=f)
        d.text((cx - w / 2, 64), ch, font=f, fill=(20, 20, 20) if col[0] > 200 else (255, 255, 255))
    maps["subway"] = (_png(im), None)
    return maps


# ---------------------------------------------------------------- inward-facing shell helpers
def wall_x(m, mat, x, nx, z0, z1, y0, y1):
    quad(m, mat, [(x, y0, z0), (x, y0, z1), (x, y1, z1), (x, y1, z0)], (nx, 0, 0))


def wall_z(m, mat, z, nz, x0, x1, y0, y1):
    quad(m, mat, [(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)], (0, 0, nz))


def vault(m, mat, x0, x1, y0, rise, z0, z1, seg=14):
    """Segmental barrel vault from (x0, y0) to (x1, y0) peaking `rise` above, normals pointing down/in."""
    xc, half = (x0 + x1) / 2, (x1 - x0) / 2
    R = (half * half + rise * rise) / (2 * rise); yc = y0 + rise - R; a0 = math.asin(half / R)
    pts = [(xc + R * math.sin(a), yc + R * math.cos(a)) for a in np.linspace(-a0, a0, seg + 1)]
    for (xa, ya), (xb, yb) in zip(pts[:-1], pts[1:]):
        xm, ym = (xa + xb) / 2, (ya + yb) / 2
        nrm = (xc - xm, yc - ym, 0)
        quad(m, mat, [(xa, ya, z0), (xb, yb, z0), (xb, yb, z1), (xa, ya, z1)], nrm)
    return pts


def tunnel(m, xc, z0, z1, r=2.6):
    """Running tunnel: round bore (inward), track bed floor."""
    seg = 12; pts = [(xc + r * math.cos(a), 1.2 + r * math.sin(a)) for a in np.linspace(math.radians(-25), math.radians(205), seg + 1)]
    for (xa, ya), (xb, yb) in zip(pts[:-1], pts[1:]):
        quad(m, "concrete", [(xa, ya, z0), (xb, yb, z0), (xb, yb, z1), (xa, ya, z1)], (xc - (xa + xb) / 2, 1.2 - (ya + yb) / 2, 0))
    xa, xb = pts[0][0], pts[-1][0]
    quad(m, "dark", [(xb, FLOOR, z0), (xa, FLOOR, z0), (xa, FLOOR, z1), (xb, FLOOR, z1)], (0, 1, 0))
    for x, nx in ((xa, -1), (xb, 1)):
        wall_x(m, "concrete", x, nx, z0, z1, FLOOR, pts[0][1])


# ---------------------------------------------------------------- the hall
def hall(m):
    top = FLOOR + WALL_H
    quad(m, "dark", [(XL, FLOOR, Z0), (XR, FLOOR, Z0), (XR, FLOOR, Z1), (XL, FLOOR, Z1)], (0, 1, 0))     # track bed
    for x, nx in ((XR, -1), (XL, 1)):
        wall_x(m, "concrete", x, nx, Z0, Z1, FLOOR, PH)                                     # below platform level
        wall_x(m, "tile", x, nx, Z0, Z1, PH, top - 0.9)
        wall_x(m, "band", x + nx * 0.005, nx, Z0, Z1, PH + 2.9, PH + 3.2)                   # frieze
        wall_x(m, "band", x, nx, Z0, Z1, top - 0.9, top)
        for z in np.arange(Z0 + 6.0, Z1 - 4.0, 12.0):                                       # name panels + posters
            panel_x(m, "namesign4", x + nx * 0.03, nx, z - 1.5, z + 1.5, PH + 1.0, PH + 2.5) if nx < 0 else \
                panel_x(m, "namesign4", x + nx * 0.03, nx, z - 1.5, z + 1.5, PH + 1.0, PH + 2.5)
            for k, dz in enumerate((3.0, 6.0)):
                u0 = ((int(z) + k) % 4) / 4
                za, zb = z + dz - 1.0, z + dz + 1.0
                zz = (zb, za) if nx < 0 else (za, zb)
                quad(m, "posters", [(x + nx * 0.03, PH + 0.8, zz[0]), (x + nx * 0.03, PH + 0.8, zz[1]), (x + nx * 0.03, PH + 2.6, zz[1]), (x + nx * 0.03, PH + 2.6, zz[0])],
                     (nx, 0, 0), [(u0, 1), (u0 + 0.25, 1), (u0 + 0.25, 0), (u0, 0)])
    vault(m, "white", XL, XR, top, VAULT, Z0, Z1)
    for x in (XL + 3.0, XR - 3.0, XI - 1.2, XI + 1.2):                                      # LED strips
        yv = vault_y(x) - 0.06
        quad(m, "lamp", [(x - 0.1, yv, Z0 + 1.0), (x + 0.1, yv, Z0 + 1.0), (x + 0.1, yv, Z1 - 1.0), (x - 0.1, yv, Z1 - 1.0)], (0, -1, 0))
    # end walls with tunnel mouths, and the running tunnels beyond
    for z, nz in ((Z0, 1), (Z1, -1)):
        for xa, xb in ((XL, -XT - 2.3), (-XT + 2.3, -2.3), (2.3, XR)):
            if xb > xa: wall_z(m, "tile", z, nz, xa, xb, FLOOR, top)
        for xc in (0.0, -XT):
            wall_z(m, "tile", z, nz, xc - 2.3, xc + 2.3, FLOOR + 4.3, top)                  # lintel over each mouth
        wall_z(m, "tile", z, nz, XL, XR, top, top + VAULT)                                  # tympanum (fills the vault end)
        zt = z - nz * 30.0
        for xc in (0.0, -XT): tunnel(m, xc, min(z, zt), max(z, zt))


def vault_y(x):
    top = FLOOR + WALL_H; xc, half = (XL + XR) / 2, (XR - XL) / 2
    R = (half * half + VAULT * VAULT) / (2 * VAULT); yc = top + VAULT - R
    return yc + math.sqrt(max(R * R - (x - xc) ** 2, 0))


def island(m):
    md_island = md.island                                                                   # coping + tactile both edges
    md.Z0, md.Z1 = Z0, Z1
    md_island(m, -EDGE, -(XT - EDGE))
    for z in np.arange(Z0 + 4.0, Z1 - 1.0, 8.0):                                            # steel columns to the vault
        if SZ1 - 1.0 < z < SZ0 + 1.0: continue
        post(m, "steel", XI, z, PH, vault_y(XI), 0.16)
        box(m, "tile", XI - 0.3, XI + 0.3, PH, PH + 1.2, z - 0.3, z + 0.3)                  # tiled column base
    k = 0
    for z in np.arange(Z0 + 8.0, Z1 - 1.0, 8.0):
        if SZ1 - 2.0 < z < SZ0 + 2.0: continue
        if k % 3 == 1: departure_board(m, XI, z)
        else: bench(m, XI + 0.9, z); bench_l(m, XI - 0.9, z)
        k += 1
    for z in (Z0 + 6.0, SZ0 + 5.0, Z1 - 6.0): bin_(m, XI + 1.6, z)


def stairway(m):
    """Straight stair from the island (SZ0, PH) up to the street (SZ1, DEPTH): inward shaft, mid landing, lights."""
    x0, x1 = XI - SW / 2, XI + SW / 2
    mid = (SZ0 + SZ1) / 2; land = 2.0
    ym = PH + (DEPTH - PH) / 2
    za, zb = SZ0, mid + land / 2                                                            # lower flight (ascending toward -z)
    ev.stairs(m, x0, x1, za, zb, PH, ym, "concrete", "paving")
    box(m, "paving", x0, x1, ym - 0.3, ym, mid - land / 2, mid + land / 2, bottom=True)     # landing
    ev.stairs(m, x0, x1, mid - land / 2, SZ1 - land, ym, DEPTH, "concrete", "paving")
    # shaft: inward side walls + sloped ceiling following the stair 3 m above the nosing line; starts where it meets the vault
    def yline(z):
        if z >= mid + land / 2: return PH + (SZ0 - z) / (SZ0 - mid - land / 2) * (ym - PH)
        if z >= mid - land / 2: return ym
        return ym + (mid - land / 2 - z) / (mid - land / 2 - SZ1 + land) * (DEPTH - ym)
    # the shaft ends where its ceiling (3.2 m over the stair line) meets the street; past that the well is open to the sky
    za_, zb_ = mid - land / 2, SZ1 - land
    zcut = za_ + (zb_ - za_) * (DEPTH - 3.2 - yline(za_)) / (yline(zb_) - yline(za_))
    global ZWELL; ZWELL = zcut
    zs = [SZ0, mid + land / 2, mid - land / 2, zcut]
    for zA, zB in zip(zs[:-1], zs[1:]):
        yA, yB = yline(zA), yline(zB)
        hA, hB = 3.2, 3.2
        cA = max(yA + hA, 0); cB = max(yB + hB, 0)
        for x, nx in ((x0 - 0.05, 1), (x1 + 0.05, -1)):
            quad(m, "tile", [(x, yA - 0.4, zA), (x, yB - 0.4, zB), (x, yB + hB, zB), (x, yA + hA, zA)], (nx, 0, 0))
            quad(m, "band", [(x + nx * 0.005, yA + 1.1, zA), (x + nx * 0.005, yB + 1.1, zB), (x + nx * 0.005, yB + 1.35, zB), (x + nx * 0.005, yA + 1.35, zA)], (nx, 0, 0))
        quad(m, "white", [(x0 - 0.05, yA + hA, zA), (x1 + 0.05, yA + hA, zA), (x1 + 0.05, yB + hB, zB), (x0 - 0.05, yB + hB, zB)], (0, -1, 0))
        quad(m, "lamp", [(XI - 0.15, yA + hA - 0.03, zA), (XI + 0.15, yA + hA - 0.03, zA), (XI + 0.15, yB + hB - 0.03, zB), (XI - 0.15, yB + hB - 0.03, zB)], (0, -1, 0))
    # top flight lands at street level inside the pavilion: floor slab around the stair head
    # portal frame where the shaft leaves the hall, with the name panel over it
    for x in (x0 - 0.15, x1 + 0.15): post(m, "steel", x, SZ0, PH, PH + 3.4, 0.08)
    box(m, "steel", x0 - 0.2, x1 + 0.2, PH + 3.3, PH + 3.6, SZ0 - 0.1, SZ0 + 0.1)
    panel_z(m, "metro", SZ0 + 0.12, 1, XI - 1.2, XI + 1.2, PH + 3.62, PH + 4.22)


def entrance(m):
    """NYC-style street entrance: an open stairwell ringed by dark-green cast-iron railings on a granite curb,
    two green globe lamps on posts at the top of the stairs, black SUBWAY plates with route bullets on the railings.
    No sidewalk or road: the entrance drops onto whatever ground the city places around it."""
    G = DEPTH + 0.04                                                                         # just above the ground plane
    x0, x1 = XI - SW / 2 - 0.08, XI + SW / 2 + 0.08                                          # well (stair width)
    ztop = SZ1 - 2.0                                                                         # top step, where you enter
    zback = ZWELL                                                                            # shaft ceiling meets the sidewalk
    # well lining: tiled walls down to where the shaft takes over, and the well's back wall
    for x, nx in ((x0, 1), (x1, -1)):
        quad(m, "tile", [(x, DEPTH - 3.2, zback), (x, G, zback), (x, G, ztop), (x, DEPTH - 0.4, ztop)], (nx, 0, 0))
    quad(m, "tile", [(x0, DEPTH - 3.2, zback), (x1, DEPTH - 3.2, zback), (x1, G, zback), (x0, G, zback)], (0, 0, -1))
    # cast-iron railing: granite coping, posts with ball finials, top + mid rails, balusters, on both sides and the back
    H = 1.05
    def rail(xa, za, xb, zb):
        L = math.hypot(xb - xa, zb - za); n = max(1, int(L / 0.12))
        box(m, "stone", min(xa, xb) - 0.12, max(xa, xb) + 0.12, G, G + 0.25, min(za, zb) - 0.12, max(za, zb) + 0.12)
        for y, t in ((G + H, 0.05), (G + 0.4, 0.03)):
            box(m, "nycgreen", min(xa, xb) - t, max(xa, xb) + t, y - t, y + t, min(za, zb) - t, max(za, zb) + t, bottom=True)
        for k in range(n + 1):
            x, z = xa + (xb - xa) * k / n, za + (zb - za) * k / n
            box(m, "nycgreen", x - 0.012, x + 0.012, G + 0.25, G + H, z - 0.012, z + 0.012)
    for x in (x0 - 0.12, x1 + 0.12): rail(x, ztop, x, zback)
    rail(x0 - 0.12, zback + 0.12, x1 + 0.12, zback + 0.12)
    for x in (x0 - 0.12, x1 + 0.12):                                                          # corner posts
        for z in (ztop, zback + 0.12):
            box(m, "nycgreen", x - 0.07, x + 0.07, G, G + H + 0.15, z - 0.07, z + 0.07)
            box(m, "nycgreen", x - 0.05, x + 0.05, G + H + 0.15, G + H + 0.25, z - 0.05, z + 0.05)
    # SUBWAY plates on both side railings (facing out) and on the back railing (facing the approach)
    zm = (ztop + zback) / 2
    for x, s in ((x0 - 0.12, -1), (x1 + 0.12, 1)):
        box(m, "nycgreen", x - 0.02, x + 0.02, G + 1.12, G + 1.62, zm - 1.35, zm + 1.35)
        panel_x(m, "subway", x + s * 0.03, s, zm - 1.3, zm + 1.3, G + 1.13, G + 1.61)
        panel_x(m, "subway", x - s * 0.03, -s, zm - 1.3, zm + 1.3, G + 1.13, G + 1.61)
    # globe lamps flanking the top of the stairs: fluted post, green glass globe
    for x in (x0 - 0.12, x1 + 0.12):
        z = ztop - 0.25
        tube = lambda a, b, r, rb=None: gc.tube(m, "nycgreen", a, b, r, (0.5, 0.5), 10, rb=rb)
        tube((x, G, z), (x, G + 0.35, z), 0.14, 0.09)
        tube((x, G + 0.35, z), (x, G + 2.3, z), 0.06, 0.05)
        tube((x, G + 2.3, z), (x, G + 2.42, z), 0.11)
        gc.tube(m, "beacon", (x, G + 2.42, z), (x, G + 2.56, z), 0.12, (0.5, 0.5), 12, rb=0.2)
        gc.tube(m, "beacon", (x, G + 2.56, z), (x, G + 2.78, z), 0.2, (0.5, 0.5), 12, rb=0.17)
        gc.tube(m, "beacon", (x, G + 2.78, z), (x, G + 2.92, z), 0.17, (0.5, 0.5), 12, rb=0.06)
        tube((x, G + 2.92, z), (x, G + 3.05, z), 0.03)


def build():
    m = Mesh()
    hall(m)
    island(m)
    stairway(m)
    entrance(m)
    return m


def main():
    os.makedirs(st.OUT, exist_ok=True)
    maps = textures()
    gc.MAT_PBR.update({"concrete": (0, 0.9), "paving": (0, 0.85), "stone": (0, 0.8), "roof": (0.6, 0.45), "tactile": (0, 0.7),
                       "steel": (0.5, 0.4), "wood": (0, 0.7), "board": (0, 0.3), "entrance": (0.3, 0.3), "lamp": (0, 0.5),
                       "curtain": (0.6, 0.08), "white": (0, 0.6), "cladding": (0.5, 0.5), "tile": (0, 0.2), "band": (0, 0.25),
                       "dark": (0, 0.9), "namesign4": (0, 0.3), "posters": (0, 0.4), "metro": (0, 0.4), "nycgreen": (0.4, 0.45), "beacon": (0, 0.1), "subway": (0, 0.4)})
    m = build()
    out = os.path.join(st.OUT, "STATION_underground_subway.glb")
    gc.write_glb(out, m, maps, (1, 1, 1))
    print(f"underground_subway: {m.count()} tris, {os.path.getsize(out) / 1e6:.2f} MB -> {out}  (street at y = +{DEPTH})")


if __name__ == "__main__":
    main()
