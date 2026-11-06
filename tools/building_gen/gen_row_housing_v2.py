"""Medium density row housing, v2: lots 1x2 .. 1x6, five levels, composed from wings like the low-density v2 houses.

A lot is one 8 m cell wide, so the building spans it edge to edge with party walls and holds two narrow units. Structure per level:
 1 tenement   flat roof behind a cornice, stoops, canopies, rear wings on deeper lots
 2 gabled     two front-gabled units, each with its own posted porch, chimneys
 3 brick row  three storeys, tall bays and balconies, deep cornice
 4 brownstone raised stoops, mansard roof with dormers, balconies, canopies
 5 modern     stepped flat volumes, cantilever box, stacked balconies
Run: python gen_row_housing_v2.py [levels...]     Output: assets/buildings/row_housing/glb/RH_1x<N>_L<level>.glb (replaces v1)
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
import gen_low_density as g
import gen_low_density_v2 as v2
import spom
from gen_low_density import box, deco, write_glb, tile_maps, detail_sheet, canopy_over, balcony_at, CELL
from gen_low_density_v2 import House, roof_poly, wall_poly, gable, flat_slab, posts, chimney

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
    5: dict(wall_m=2.8, roof_m=3.0, wall_k=7.0, roof_k=4.0, trim=(150, 150, 150), door=(170, 120, 70), glass=(46, 70, 88)),
}
v2.SH.update({1: 2.6, 2: 2.7, 3: 3.0, 4: 3.3, 5: 3.1})


def door_at(h, dx, z, y0, w=1.1, ht=2.2):
    deco(h.m, "door", dx, y0 + ht / 2, z, w, ht, +1, "x")
    h.reserved.append(("front", dx - w / 2 - 0.3, dx + w / 2 + 0.3, 0, y0 + ht + 0.3))


def stoop(h, dx, z1, height, depth=1.2):
    m = h.m
    box(m, dx - 0.85, dx + 0.85, 0, height, z1, z1 + depth, h.wm, top="roof", roof_m=h.rm * 0.6)
    if height > 0.5:                                                    # a lower step in front
        box(m, dx - 0.85, dx + 0.85, 0, height * 0.5, z1 + depth, z1 + depth + 0.45, h.wm, top="roof", roof_m=h.rm * 0.6)


def tall_bay(h, cx, z1, floors, w=2.2, d=0.7):
    """A bay running up several floors, with its own little roof and a wide window per floor."""
    m, sh = h.m, h.sh
    Hb = floors * sh
    box(m, cx - w / 2, cx + w / 2, 0.15, Hb, z1, z1 + d, h.wm, skip=("top", "back"))
    roof_poly(m, [(cx - w / 2 - 0.15, Hb, z1 + d + 0.15), (cx + w / 2 + 0.15, Hb, z1 + d + 0.15),
                  (cx + w / 2 + 0.15, Hb + 0.4, z1), (cx - w / 2 - 0.15, Hb + 0.4, z1)], h.rm)
    for f in range(floors):
        deco(m, "wide", cx, f * sh + 1.6, z1 + d, w - 0.3, 1.5, +1, "x")
    h.reserved.append(("front", cx - w / 2 - 0.2, cx + w / 2 + 0.2, 0, Hb))


def build(N, level):
    p = LEVELS[level]; h = House(p, level); m = h.m
    sh = h.sh; wm, rm = h.wm, h.rm
    z1 = N * CELL / 2 - 2.5                 # building front (2.5 m of front yard)
    z0 = -N * CELL / 2 + 2.0                # building back
    cx = [-2.0, 2.0]                        # unit centres
    flip = 1 if N % 2 == 0 else -1          # alternate lots mirror their details
    deep = N >= 3

    if level == 1:                                                    # tenement
        H, _ = h.wing(-4, 4, z0, z1, 2, None, name="main")
        flat_slab(m, -4, 4, z0, z1, H, 0.38, 0.18, wm, rm); h.top = H + 0.4
        for dx in (-3.0, 3.0):
            stoop(h, dx, z1, 0.45); door_at(h, dx, z1, 0.45); canopy_over(m, dx, 0.45 + 2.2, z1)
        if deep:                                                      # two lower rear wings
            for u in range(2):
                h.wing(cx[u] - 1.5, cx[u] + 1.5, z0 - 1.5, z0 + 1.0, 1, "gable", rise=0.9, over=0.3, axis="z", name="rear")
        h.windows(sides=("front", "back"), only=("main", "rear"))
        for u in range(2): chimney(m, cx[u] + 0.8 * flip, z1 - 2.2, H + 0.4, H + 1.7, wm, rm)

    elif level == 2:                                                  # two front-gabled units with posted porches
        Hs = []
        for u in range(2):
            x0, x1 = -4 + 4 * u, 4 * u
            H, rise = h.wing(x0, x1, z0, z1, 2, "gable", rise=2.2, over=0.35, axis="z", name="main")
            Hs.append((H, rise))
            h.porch(x0 + 0.35, x1 - 0.35, z1, 1.7, sh + 0.05, shed_rise=0.45, post_step=3.3)
            door_at(h, x0 + 2.0 + (-0.5 if u == 0 else 0.5), z1, 0.12)
            chimney(m, x0 + 2.0 + 0.9 * flip, (z0 + z1) / 2, 4.4, H + rise + 0.8, wm, rm)
        if deep:
            for u in range(2): h.wing(cx[u] - 1.3, cx[u] + 1.3, z0 - 1.5, z0 + 1.0, 1, "gable", rise=1.0, over=0.3, axis="z", name="rear")
        h.windows(sides=("front", "back"), only=("main", "rear"), spacing=3.4)

    elif level == 3:                                                  # three-storey brick: tall bay, balcony, deep cornice
        H, _ = h.wing(-4, 4, z0, z1, 3, None, name="main")
        flat_slab(m, -4, 4, z0, z1, H, 0.5, 0.25, wm, rm); h.top = H + 0.5
        door_at(h, -3.2 * flip, z1, 0.4); stoop(h, -3.2 * flip, z1, 0.4); canopy_over(m, -3.2 * flip, 0.4 + 2.2, z1)
        door_at(h, 1.0 * flip, z1, 0.4); stoop(h, 1.0 * flip, z1, 0.4); canopy_over(m, 1.0 * flip, 0.4 + 2.2, z1)
        tall_bay(h, -1.2 * flip, z1, 2)
        bc = 2.4 * flip
        balcony_at(m, bc, sh + 0.15, z1); h.reserved.append(("front", bc - 1.6, bc + 1.6, sh, 2 * sh))
        if deep:
            for u in range(2): h.wing(cx[u] - 1.5, cx[u] + 1.5, z0 - 1.5, z0 + 1.0, 2, "gable", rise=1.1, over=0.3, axis="z", name="rear")
        h.windows(sides=("front", "back"), only=("main", "rear"))
        for u in range(2): chimney(m, cx[u], z0 + 1.0, H + 0.5, H + 1.8, wm, rm)

    elif level == 4:                                                  # brownstone: raised stoops, mansard with dormers
        base = 0.9
        H, rise = h.wing(-4, 4, z0, z1, 3, "gable", rise=2.3, over=0.3, axis="x", name="main")
        box(m, -4.2, 4.2, H, H + 0.3, z0 - 0.2, z1 + 0.3, wm, top="roof", roof_m=rm)
        for dx in (-3.0, 3.0):
            stoop(h, dx, z1, base, 1.4); door_at(h, dx, z1, base); canopy_over(m, dx, base + 2.2, z1)
        for u in range(2): balcony_at(m, cx[u], sh + 0.15 + 0.4, z1)
        for u in range(2): h.reserved.append(("front", cx[u] - 1.6, cx[u] + 1.6, sh + 0.2, 2 * sh))
        for u in range(2): h.dormer(cx[u], h.wings[0], H + 0.3, rise, w=1.6)
        if deep:
            for u in range(2): h.wing(cx[u] - 1.5, cx[u] + 1.5, z0 - 1.5, z0 + 1.0, 2, "gable", rise=1.2, over=0.3, axis="z", name="rear")
        h.windows(sides=("front", "back"), only=("main", "rear"), sill=1.35)
        for u in range(2): chimney(m, cx[u] + 1.1, z0 + 1.2, H, H + rise + 1.6, wm, rm)

    else:                                                             # modern: stepped volumes, cantilever, stacked balconies
        HA, _ = h.wing(-4, 0.0, z0, z1, 3, "flat", over=0.3, name="A")
        HB, _ = h.wing(0.0, 4, z0, z1 - 0.6, 2, "flat", over=0.3, name="B")
        yb = HB + 0.22                                               # a roof-terrace screen wall on B
        box(m, 0.0, 4, yb, yb + 1.1, z0, z1 - 0.6, wm, skip=("top",))
        box(m, 0.4, 3.6, sh + 0.2, 2 * sh - 0.05, z1 - 0.6, z1 + 1.0, wm, top="roof", roof_m=rm, skip=("back",))   # cantilever box over the entrance
        deco(m, "wide", 2.0, sh + 1.5, z1 + 1.0, 2.6, 1.5, +1, "x")
        for dx in (-3.0, 3.0):
            door_at(h, dx, z1 - (0.6 if dx > 0 else 0.0), 0.12); canopy_over(m, dx, 2.4, z1 - (0.6 if dx > 0 else 0.0))
        balcony_at(m, -2.0, sh + 0.15, z1); balcony_at(m, -2.0, 2 * sh + 0.15, z1)
        h.reserved += [("front", -3.5, -0.5, sh, 3 * sh), ("front", 0.4, 3.6, sh, 2 * sh)]
        h.top = max(h.top, HA + 0.3)
        if deep: h.wing(-3.0, 3.0, z0 - 1.5, z0 + 1.0, 1, "flat", over=0.2, name="rear")
        h.windows(sides=("front", "back"), only=("A", "B", "rear"), kind="wide", ww=1.8, wh=1.6, spacing=4.0)
    return h.m, (8.0, N * CELL, h.top)


def main():
    os.makedirs(OUT, exist_ok=True); os.makedirs(TEX, exist_ok=True)
    only = [int(a) for a in sys.argv[1:]] or sorted(LEVELS)
    for lv in only:
        p = LEVELS[lv]
        wp, rp = os.path.join(SRC, f"RH_L{lv}_wall.jpg"), os.path.join(SRC, f"RH_L{lv}_roof.jpg")
        tex = {"wall": tile_maps(wp, p["wall_k"]), "roof": tile_maps(rp, p["roof_k"]), "detail": detail_sheet(p),
               "spom_balcony": spom.relief("balcony", p), "spom_canopy": spom.relief("canopy", p)}
        for (W, N) in LOTS:
            mesh, dims = build(N, lv)
            out = os.path.join(OUT, f"RH_{W}x{N}_L{lv}.glb")
            write_glb(out, mesh, tex)
            print(f"RH_{W}x{N}_L{lv}: {mesh.tri_count():3d} tris, {os.path.getsize(out)/1e6:.2f} MB, height {dims[2]:.1f} m")


if __name__ == "__main__":
    main()
