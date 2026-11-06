"""Medium density row housing: lots 1x2 .. 1x6 (wiki: 'Medium Density Row Housing', 2-6 tiles) x 5 levels -> GLB.

Reuses the low-density toolkit (textures, Mesh, GLB writer). A row-house lot is one 8 m cell wide, so the building spans the
full lot width with party walls and holds two narrow units side by side; depth grows with the lot (1x2 .. 1x6 cells).
Source textures (Gemini): assets/buildings/row_housing/src_tex/RH_L<level>_wall.jpg and _roof.jpg
Output: assets/buildings/row_housing/glb/RH_1x<N>_L<level>.glb      Units: metres, +Y up, front faces +Z.
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
import gen_low_density as g
from gen_low_density import Mesh, box, deco, gable_roof, write_glb, tile_maps, detail_sheet, CELL, canopy_over, balcony_at
import spom

ROOT = g.ROOT
SRC = os.path.join(ROOT, "assets", "buildings", "row_housing", "src_tex")
OUT = os.path.join(ROOT, "assets", "buildings", "row_housing", "glb")
TEX = os.path.join(ROOT, "assets", "buildings", "row_housing", "tex")
LOTS = [(1, 2), (1, 3), (1, 4), (1, 5), (1, 6)]

LEVELS = {
    1: dict(wall_m=2.4, roof_m=2.0, wall_k=10.0, roof_k=7.0, trim=(176, 160, 138), door=(70, 50, 36), glass=(30, 36, 42)),
    2: dict(wall_m=2.0, roof_m=2.0, wall_k=7.0, roof_k=7.0, trim=(240, 238, 230), door=(150, 60, 50), glass=(34, 46, 58)),
    3: dict(wall_m=2.4, roof_m=2.4, wall_k=9.0, roof_k=6.0, trim=(226, 222, 210), door=(36, 62, 54), glass=(28, 40, 52)),
    4: dict(wall_m=3.0, roof_m=2.4, wall_k=9.0, roof_k=8.0, trim=(70, 52, 42), door=(48, 34, 28), glass=(24, 34, 46)),
    5: dict(wall_m=2.8, roof_m=3.0, wall_k=7.0, roof_k=4.0, trim=(30, 32, 36), door=(170, 120, 70), glass=(46, 70, 88)),
}
STORIES = {1: 2, 2: 2, 3: 3, 4: 3, 5: 3}
SH = {1: 2.6, 2: 2.7, 3: 3.0, 4: 3.3, 5: 3.1}


def flat_roof_block(m, x0, x1, z0, z1, y0, y1, wm, rm, lip=0.15, band=0.35):
    """Walls up to y1 with a wall-material cornice band that overhangs by `lip`; roof material on top."""
    box(m, x0, x1, y0, y1, z0, z1, wm, skip=("top",))
    box(m, x0 - lip, x1 + lip, y1, y1 + band, z0 - lip, z1 + lip, wm, top="roof", roof_m=rm)


def build_row(N, level):
    p = LEVELS[level]; m = Mesh()
    wm, rm = p["wall_m"], p["roof_m"]
    fw = CELL                      # full lot width: party walls, no side yard
    fd = N * CELL - 4.5            # front stoop strip + back yard
    zc = (2.0 - 2.5) / 2.0
    x0, x1 = -fw / 2, fw / 2
    z1, z0 = zc + fd / 2, zc - fd / 2
    st, sh = STORIES[level], SH[level]
    base = 0.35 if level != 4 else 0.9   # brownstone levels sit on a raised stoop
    H = st * sh + base
    unit = fw / 2

    if level in (1, 3):  # flat roof behind a cornice band (tenement / brick row)
        flat_roof_block(m, x0, x1, z0, z1, 0, H, wm, rm)
        top = H + 0.35
    elif level == 2:     # front-gabled units: two gables side by side, ridge along Z
        box(m, x0, x1, 0, H, z0, z1, wm, skip=("top",))
        for u in range(2):
            ux0, ux1 = x0 + u * unit, x0 + (u + 1) * unit
            gable_roof(m, ux0, ux1, H, z0, z1, unit * 0.55, 0.35, wm, rm, "z")
        top = H + unit * 0.55
    elif level == 4:     # brownstone: steep mansard-like roof, ridge along X
        box(m, x0, x1, 0, H, z0, z1, wm, skip=("top",))
        box(m, x0 - 0.2, x1 + 0.2, H, H + 0.3, z0 - 0.3, z1 + 0.3, wm, top="roof", roof_m=rm)  # heavy cornice
        gable_roof(m, x0, x1, H + 0.3, z0, z1, 2.0, 0.0, wm, rm, "x")
        top = H + 2.3
    else:                # modern: flat roof, bay volume on the right unit's upper floors
        box(m, x0, x1, 0, H, z0, z1, wm, top="roof", roof_m=rm)
        bx0, bx1 = x0 + unit + 0.6, x1 - 0.6
        box(m, bx0, bx1, base + sh, H, z1, z1 + 0.9, wm, top="roof", roof_m=rm)  # oriel box (cantilevered)
        top = H

    # stoop (level 4) or step slab (others): one small box per door
    door_xs = [x0 + unit * (u + 0.5) + (-1.0 if u == 0 else 1.0) for u in range(2)]  # doors on the outer sides, windows inside
    for u, dx in enumerate(door_xs):
        if level == 4:
            box(m, dx - 0.8, dx + 0.8, 0, base, z1, z1 + 1.1, wm, top="roof", roof_m=rm)
        else:
            box(m, dx - 0.7, dx + 0.7, 0, 0.2, z1, z1 + 0.6, wm, top="roof", roof_m=rm)
        deco(m, "door", dx, base + 1.1, z1, 1.1, 2.2, +1)
        # windows: ground floor beside the door, upper floors centred on the unit
        for s in range(st):
            y = base + s * sh + sh * 0.55
            wx = x0 + unit * (u + 0.5) + (0.95 if u == 0 else -0.95)  # ground floor: beside the door, never on it
            if s == 0:
                deco(m, "win", wx, y, z1, 1.0, 1.4, +1)
            else:
                deco(m, "win", x0 + unit * (u + 0.5), y, z1 + (0.9 if (level == 5 and u == 1) else 0), 1.3, 1.5, +1)
            deco(m, "win", x0 + unit * (u + 0.5), y, z0, 1.2, 1.4, -1)  # back wall
    # silhouette-POM volumes (see spom.py): a canopy over every door, balconies on the upper floors from level 3
    for dx in door_xs:
        canopy_over(m, dx, base + 2.2, z1)
    if level == 3:
        for u in range(2): balcony_at(m, x0 + unit * (u + 0.5), base + sh, z1)
    elif level == 4:
        for u in range(2): balcony_at(m, x0 + unit * (u + 0.5), base + sh, z1)
    elif level == 5:
        balcony_at(m, x0 + unit * 0.5, base + sh, z1); balcony_at(m, x0 + unit * 0.5, base + 2 * sh, z1)
    return m, (fw, N * CELL, top)


def main():
    os.makedirs(OUT, exist_ok=True); os.makedirs(TEX, exist_ok=True)
    only = [int(a) for a in sys.argv[1:]] or sorted(LEVELS)
    for lv in only:
        p = LEVELS[lv]
        wp, rp = os.path.join(SRC, f"RH_L{lv}_wall.jpg"), os.path.join(SRC, f"RH_L{lv}_roof.jpg")
        if not (os.path.exists(wp) and os.path.exists(rp)):
            print(f"level {lv}: missing source textures, skipped"); continue
        tex = {"wall": tile_maps(wp, p["wall_k"]), "roof": tile_maps(rp, p["roof_k"]), "detail": detail_sheet(p),
               "spom_balcony": spom.relief("balcony", p), "spom_canopy": spom.relief("canopy", p)}
        for name, (a, n) in tex.items():
            open(os.path.join(TEX, f"RH_L{lv}_{name}_albedo." + ("png" if name.startswith("spom_") else "jpg")), "wb").write(a); open(os.path.join(TEX, f"RH_L{lv}_{name}_normal.png"), "wb").write(n)
        for (W, N) in LOTS:
            mesh, dims = build_row(N, lv)
            out = os.path.join(OUT, f"RH_{W}x{N}_L{lv}.glb")
            write_glb(out, mesh, tex)
            print(f"RH_{W}x{N}_L{lv}: {mesh.tri_count():3d} tris, {os.path.getsize(out)/1e6:.2f} MB, height {dims[2]:.1f} m")


if __name__ == "__main__":
    main()
