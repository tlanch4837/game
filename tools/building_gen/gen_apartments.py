"""Medium density housing (small apartment buildings): every plot 2x2 .. 6x6 (25 sizes) x 5 levels = 125 houses.

Built from wings with the low-density toolkit (gen_low_density_v2.House): a main block, a projecting entrance tower, optional corner
towers, rooftop structures, balconies (silhouette POM) on the bay grid, and a rear block joined by a link on deep plots. A side driveway
(the road system's own materials) runs from the lot's front edge to the back, flared at the apron; a walk leads to the entrance.

  1 walk-up     concrete block, flat roof behind a parapet, roof bulkhead
  2 gabled      stucco, pitched roof, gabled entrance tower, dormers, balconies
  3 brick       4-5 storeys, deep cornice, tower, balconies on alternate bays
  4 granite     mansard roof with dormers, corner towers, balconies
  5 modern      panel cladding, stepped top floor, stacked balconies, roof pergola

Run: python gen_apartments.py [levels...]      Output: assets/buildings/medium_apartments/glb/MD_<W>x<D>_L<level>.glb
Source textures: assets/buildings/medium_apartments/src_tex/MD_L<n>_wall.jpg (Gemini); roofs reuse the row-housing roof set.
"""
import os, sys, math
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
import gen_low_density as g
import gen_low_density_v2 as v2
import spom
from gen_low_density import box, deco, write_glb, tile_maps, detail_sheet, canopy_over, balcony_at, CELL
from gen_low_density_v2 import House, roof_poly, wall_poly, gable, hip, flat_slab, posts, chimney, drive_tex, DRIVE_MAT

ROOT = g.ROOT
SRC = os.path.join(ROOT, "assets", "buildings", "medium_apartments", "src_tex")
RH_SRC = os.path.join(ROOT, "assets", "buildings", "row_housing", "src_tex")
OUT = os.path.join(ROOT, "assets", "buildings", "medium_apartments", "glb")
TEX = os.path.join(ROOT, "assets", "buildings", "medium_apartments", "tex")
PLOTS = [(w, d) for w in range(2, 7) for d in range(2, 7)]
LEVELS = {
    1: dict(wall_m=2.0, roof_m=2.0, wall_k=9.0, roof_k=7.0, trim=(215, 210, 200), door=(60, 72, 84), glass=(30, 38, 46)),
    2: dict(wall_m=2.4, roof_m=2.0, wall_k=6.0, roof_k=7.0, trim=(245, 243, 236), door=(100, 64, 44), glass=(34, 46, 58)),
    3: dict(wall_m=2.2, roof_m=2.4, wall_k=9.0, roof_k=6.0, trim=(232, 226, 212), door=(40, 70, 60), glass=(28, 40, 52)),
    4: dict(wall_m=1.8, roof_m=2.4, wall_k=9.0, roof_k=8.0, trim=(90, 84, 76), door=(52, 40, 32), glass=(24, 34, 46)),
    5: dict(wall_m=2.8, roof_m=3.0, wall_k=5.0, roof_k=4.0, trim=(60, 64, 70), door=(170, 120, 70), glass=(46, 70, 88)),
}
v2.SH.update({1: 3.0, 2: 3.0, 3: 3.1, 4: 3.2, 5: 3.2})
DRIVE_W = 3.2


def bays(x0, x1, target=3.8):
    n = max(2, int(round((x1 - x0) / target)))
    return [x0 + (i + 0.5) * (x1 - x0) / n for i in range(n)], (x1 - x0) / n


def block_windows(h, x0, x1, z0, z1, floors, entrance=None, front=True, back=True, sides=True, ww=1.3, wh=1.5):
    """A regular window grid on a block's walls; the entrance tower's span is left to the tower itself."""
    m, sh = h.m, h.sh
    cs, bw = bays(x0, x1)
    for i, c in enumerate(cs):
        for f in range(floors):
            y = f * sh + 1.7
            if front and not (entrance is not None and abs(c - entrance) < 2.0): deco(m, "win", c, y, z1, ww, wh, +1, "x")
            if back: deco(m, "win", c, y, z0, ww, wh, -1, "x")
    if sides:
        n = max(1, int((z1 - z0) // 4.2))
        for i in range(n):
            c = z0 + (i + 0.5) * (z1 - z0) / n
            for f in range(floors):
                y = f * sh + 1.7
                deco(m, "win", c, y, x0, 1.2, wh, -1, "z"); deco(m, "win", c, y, x1, 1.2, wh, +1, "z")
    return cs


def rooftop(h, x0, x1, z0, z1, H, level):
    """Roof clutter that makes a flat roof read as a roof: a stair bulkhead and a plant box."""
    m, wm, rm = h.m, h.wm, h.rm
    cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
    box(m, cx - 1.3, cx + 1.3, H, H + 2.5, cz - 1.0, cz + 1.0, wm, top="roof", roof_m=rm)
    box(m, x0 + 1.2, x0 + 3.4, H, H + 1.2, z0 + 1.2, z0 + 3.0, wm * 0.8, top="roof", roof_m=rm)
    h.top = max(h.top, H + 2.5)


def entrance(h, cx, z1, floors, level, roof):
    """A full-height entrance tower projecting 1.4 m, its own roof, a canopy over double doors, and steps."""
    m, sh, wm, rm = h.m, h.sh, h.wm, h.rm
    ex0, ex1 = cx - 1.9, cx + 1.9
    H, rise = h.wing(ex0, ex1, z1 - 1.5, z1 + 1.4, floors, roof, rise=(1.9 * 0.85 if roof == "gable" else None), over=0.4, axis="z", name="tower")
    if roof == "flat":
        box(m, ex0 - 0.1, ex1 + 0.1, H + 0.22, H + 0.85, z1 - 1.5, z1 + 1.5, wm, top="roof", roof_m=rm, skip=("back",))   # a raised parapet band
        h.top = max(h.top, H + 0.9)
    zf = z1 + 1.4
    deco(m, "door", cx, 1.2, zf, 1.5, 2.3, +1, "x")
    canopy_over(m, cx, 2.5, zf)
    box(m, cx - 1.4, cx + 1.4, 0, 0.16, zf, zf + 0.5, wm * 0.5, top="roof", roof_m=rm * 0.5)
    box(m, cx - 1.1, cx + 1.1, 0, 0.3, zf + 0.5, zf + 0.9, wm * 0.5, top="roof", roof_m=rm * 0.5) if False else None
    for f in range(1, floors):                                           # a tall stair window per floor above the door
        deco(m, "win", cx, f * sh + 1.7, zf, 1.0, 1.6, +1, "x")
    return H, zf


def balconies(h, cs, z1, floors, level, entrance_x, which):
    sh = h.sh
    for i, c in enumerate(cs):
        if abs(c - entrance_x) < 2.6: continue
        if not which(i): continue
        for f in range(1, floors):
            if level in (2, 4) and f > 1: break
            if level == 3 and f == floors - 1 and (i % 4 == 1): continue
            balcony_at(h.m, c, f * sh + 0.15, z1)


def add_ground(h, lw, ld, side, ecx, ez):
    m = h.m; tile = DRIVE_MAT[h.level][1]; y = 0.03
    def ground(x0, x1, za, zb, yy, flare=0.0):
        if zb - za < 0.05: return
        pts = [(x0, yy, za), (x0 - flare, yy, zb), (x1 + flare, yy, zb), (x1, yy, za)]
        m.quad("drive", *pts, [(q[0] / tile, q[2] / tile) for q in pts])
    xa = lw / 2 - 0.5 - DRIVE_W
    x0, x1 = (xa, xa + DRIVE_W) if side > 0 else (-xa - DRIVE_W, -xa)
    zE = ld / 2
    ground(x0, x1, -ld / 2 + 1.0, zE - 1.8, y)
    ground(x0, x1, zE - 1.8, zE, y, flare=0.7)
    ground(ecx - 0.9, ecx + 0.9, ez, zE, y + 0.005)


def build(W, D, level):
    p = LEVELS[level]; h = House(p, level); m = h.m
    sh, wm, rm = h.sh, h.wm, h.rm
    lw, ld = W * CELL, D * CELL
    side = 1 if (W + D) % 2 == 0 else -1
    xl, xr = -lw / 2 + 1.5, lw / 2 - 1.5
    if side > 0: xr -= DRIVE_W + 1.0
    else: xl += DRIVE_W + 1.0
    zf, zb = ld / 2 - 3.4, -ld / 2 + 2.0
    deep = D >= 5
    md = 9.5 if deep else min(9.2 + 1.7 * (D - 2), zf - zb)
    floors = {1: 3, 2: 3, 3: 4, 4: 4, 5: 4}[level] + (1 if (W * D >= 20 and level in (3, 5)) else 0)
    ecx = (xl + xr) / 2
    z1, z0 = zf, zf - md

    def main_block(x0, x1, za, zb_):
        if level == 1:
            H, _ = h.wing(x0, x1, za, zb_, floors, None, name="main"); flat_slab(m, x0, x1, za, zb_, H, 0.3, 0.14, wm, rm); h.top = max(h.top, H + 0.3); return H, 0.3
        if level == 2:
            return h.wing(x0, x1, za, zb_, floors, "gable", rise=min((zb_ - za) * 0.5 * 0.55, 3.8), over=0.6, axis="x", name="main")
        if level == 3:
            H, _ = h.wing(x0, x1, za, zb_, floors, None, name="main"); flat_slab(m, x0, x1, za, zb_, H, 0.5, 0.32, wm, rm); h.top = max(h.top, H + 0.5); return H, 0.5
        if level == 4:
            H, rise = h.wing(x0, x1, za, zb_, floors, "gable", rise=2.6, over=0.35, axis="x", name="main")
            box(m, x0 - 0.2, x1 + 0.2, H, H + 0.3, za - 0.2, zb_ + 0.3, wm, top="roof", roof_m=rm); return H + 0.3, rise
        H, _ = h.wing(x0, x1, za, zb_, floors - 1, "flat", over=0.45, name="main")
        return H, 0.22

    H, rise = main_block(xl, xr, z0, z1)
    main = h.wings[0]
    cs, bw = bays(xl, xr)
    # the rear block and its link on deep plots
    if deep:
        rz1 = z0 - 3.0; rz0 = max(zb, rz1 - 9.5)
        if rz1 - rz0 > 4.0:
            main_block(xl, xr, rz0, rz1)
            lx = ecx
            h.wing(lx - 1.6, lx + 1.6, rz1 - 0.2, z0 + 0.2, floors - 1, "flat", over=0.1, name="link")
            block_windows(h, xl, xr, rz0, rz1, floors, front=False, back=True, sides=True)
    block_windows(h, xl, xr, z0, z1, floors, entrance=ecx, back=True)

    eH, ezf = entrance(h, ecx, z1, floors, level, "gable" if level == 2 else "flat")

    if level == 1:
        rooftop(h, xl, xr, z0, z1, H + 0.3, level)
        balconies(h, cs, z1, floors, level, ecx, lambda i: False)
    elif level == 2:
        for c in cs:
            if abs(c - ecx) > 2.8 and floors >= 3: h.dormer(c, main, H, rise, w=1.5) if (cs.index(c) % 2 == 0) else None
        balconies(h, cs, z1, floors, level, ecx, lambda i: i % 2 == 1)
        chimney(m, xl + 2.0, (z0 + z1) / 2, H, H + rise + 1.0, wm, rm)
    elif level == 3:
        rooftop(h, xl, xr, z0, z1, H + 0.5, level)
        balconies(h, cs, z1, floors, level, ecx, lambda i: i % 2 == 1)
    elif level == 4:
        for c in cs:
            if abs(c - ecx) > 2.4: h.dormer(c, main, H, rise, w=1.5)
        for tx in (xl, xr - 3.4):                                           # corner towers with pyramid roofs
            h.wing(tx, tx + 3.4, z1 - 1.8, z1 + 0.7, floors, "hip", rise=2.2, over=0.4, name="corner")
        balconies(h, cs, z1, floors, level, ecx, lambda i: i % 2 == 0 and 0 < i < len(cs) - 1)
        chimney(m, xr - 2.0, (z0 + z1) / 2, H, H + rise + 1.2, wm, rm)
    else:
        yb = H + 0.22
        box(m, xl + 1.6, xr - 1.6, yb, yb + sh, z0 + 1.3, z1 - 1.3, wm, skip=("top",))                  # the stepped-back top floor
        flat_slab(m, xl + 1.6, xr - 1.6, z0 + 1.3, z1 - 1.3, yb + sh, 0.22, 0.45, wm, rm)
        for c in cs[1:-1]:
            deco(m, "wide", c, yb + 1.7, z1 - 1.3, min(bw - 0.6, 2.6), 1.8, +1, "x")
        h.top = max(h.top, yb + sh + 0.25)
        px = xr - 5.0                                                          # a rooftop pergola on the setback
        posts(m, [(px, z0 + 2.0), (px + 3.2, z0 + 2.0), (px, z0 + 4.4), (px + 3.2, z0 + 4.4)], yb + sh + 0.22, yb + sh + 2.6, wm, s=0.14)
        flat_slab(m, px - 0.2, px + 3.4, z0 + 1.8, z0 + 4.6, yb + sh + 2.6, 0.12, 0.0, wm, rm)
        balconies(h, cs, z1, floors - 1, level, ecx, lambda i: i % 2 == 1)
    add_ground(h, lw, ld, side, ecx, ezf)
    return h.m, (lw, ld, h.top)


def wall_path(lv):
    a = os.path.join(SRC, f"MD_L{lv}_wall.jpg")
    return a if os.path.exists(a) else os.path.join(RH_SRC, f"RH_L{lv}_wall.jpg")   # stand-in until the Gemini wall exists


def main():
    os.makedirs(OUT, exist_ok=True); os.makedirs(TEX, exist_ok=True)
    only = [int(a) for a in sys.argv[1:]] or sorted(LEVELS)
    for lv in only:
        p = LEVELS[lv]
        tex = {"wall": tile_maps(wall_path(lv), p["wall_k"]), "roof": tile_maps(os.path.join(RH_SRC, f"RH_L{lv}_roof.jpg"), p["roof_k"]),
               "detail": detail_sheet(p), "spom_balcony": spom.relief("balcony", p), "spom_canopy": spom.relief("canopy", p),
               "spom_rail": spom.relief("rail", p), "drive": drive_tex(lv)}
        for (W, D) in PLOTS:
            mesh, dims = build(W, D, lv)
            out = os.path.join(OUT, f"MD_{W}x{D}_L{lv}.glb")
            write_glb(out, mesh, tex)
            print(f"MD_{W}x{D}_L{lv}: {mesh.tri_count():4d} tris, {os.path.getsize(out)/1e6:.2f} MB, height {dims[2]:.1f} m")


if __name__ == "__main__":
    main()
