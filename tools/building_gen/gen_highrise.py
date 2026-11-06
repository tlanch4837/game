"""High density housing (large apartment buildings): every plot 2x2 .. 6x6 (25 sizes) x 5 levels = 125 buildings.

A tower is a handful of boxes. Its windows, balconies, piers and spandrels are baked into ONE procedural facade tile (9 m x 9 m = 3 floors x
3 bays of 3 m) drawn over a Gemini wall texture, with a height map that becomes the normal map: so a 20-storey tower stays a few dozen
triangles and every storey line stays aligned (tower sizes are multiples of 3 m and storeys are 3 m). A 4.5 m podium (its own plain
material) carries the lobby, storefront glazing, a garage door, and a roof-terrace railing made of silhouette-POM volumes.

  1 slab      concrete housing slab, punched windows and recessed loggias
  2 brick     L-shaped brick block, tall windows with lintels and sills
  3 glass     curtain-wall tower with a crown and mast
  4 stepped   granite tower in three setback tiers with a spire
  5 twin      modern balcony towers (two on wide plots), skybridge, roof gardens

Run: python gen_highrise.py [levels...]      Output: assets/buildings/high_density/glb/HD_<W>x<D>_L<level>.glb
"""
import os, sys, math
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import gen_low_density as g
import gen_apartments as ap
import spom
from gen_low_density import Mesh, box, deco, write_glb, tile_maps, detail_sheet, canopy_over, seamless, height_from_albedo, normal_from_height, jpg, png, CELL
from gen_low_density_v2 import flat_slab, posts, drive_tex

ROOT = g.ROOT
LD_SRC = os.path.join(ROOT, "assets", "buildings", "low_density", "src_tex")
RH_SRC = os.path.join(ROOT, "assets", "buildings", "row_housing", "src_tex")
MD_SRC = os.path.join(ROOT, "assets", "buildings", "medium_apartments", "src_tex")
OUT = os.path.join(ROOT, "assets", "buildings", "high_density", "glb")
TEX = os.path.join(ROOT, "assets", "buildings", "high_density", "tex")
PLOTS = [(w, d) for w in range(2, 7) for d in range(2, 7)]
FLOOR, TILE, PODIUM = 3.0, 18.0, 4.5
FACE_RNG = [None]   # set per building: shifts each face's tile by whole bays and storeys
BASES = {1: os.path.join(MD_SRC, "MD_L1_wall.jpg"), 2: os.path.join(RH_SRC, "RH_L3_wall.jpg"), 3: os.path.join(MD_SRC, "MD_L5_wall.jpg"),
         4: os.path.join(MD_SRC, "MD_L4_wall.jpg"), 5: os.path.join(LD_SRC, "L5_wall.jpg")}
ROOFS = {1: os.path.join(RH_SRC, "RH_L1_roof.jpg"), 2: os.path.join(RH_SRC, "RH_L3_roof.jpg"), 3: os.path.join(RH_SRC, "RH_L3_roof.jpg"),
         4: os.path.join(RH_SRC, "RH_L4_roof.jpg"), 5: os.path.join(RH_SRC, "RH_L5_roof.jpg")}
PAL = {
    1: dict(trim=(205, 200, 190), door=(60, 72, 84), glass=(44, 58, 72), base_m=2.4),
    2: dict(trim=(226, 218, 200), door=(40, 70, 60), glass=(36, 50, 64), base_m=2.4),
    3: dict(trim=(180, 188, 196), door=(70, 80, 90), glass=(52, 84, 108), base_m=2.8),
    4: dict(trim=(214, 206, 188), door=(52, 40, 32), glass=(30, 42, 56), base_m=1.8),
    5: dict(trim=(236, 236, 232), door=(170, 120, 70), glass=(60, 96, 124), base_m=3.0),
}
FLOORS = {1: 8, 2: 9, 3: 12, 4: 13, 5: 14}


# ---------------------------------------------------------------- the facade tile
def facade_tile(level):
    """18 m x 18 m: six storeys of six 3 m bays, every window its own (see facade_hd.py). Returns (albedo jpeg, normal jpeg)."""
    import facade_hd as fh
    half = seamless(Image.open(BASES[level]).convert("RGB").resize((fh.PX // 2, fh.PX // 2), Image.LANCZOS))   # the wall keeps its 9 m scale
    base = np.tile(half, (2, 2, 1))
    hgt = np.clip(height_from_albedo(base) * 0.3 + 0.43, 0, 1)
    alb, h = fh.build_tile(level, base, hgt, PAL[level]["trim"], PAL[level]["glass"], seed=1000 + level)
    return jpg(alb, 90), jpg(normal_from_height(h, 7.0), 92)


# ---------------------------------------------------------------- geometry
def fbox(m, x0, x1, y0, y1, z0, z1, yref, roof_top=True):
    """A tower box: four sides in the facade material, uv aligned to 3 m bays and to the storey lines at yref; a roof on top."""
    r = FACE_RNG[0]
    vo = float(r.integers(6)) * FLOOR if r is not None else 0.0
    def side(a, b, c, d, u):
        uo = float(r.integers(6)) * 3.0 if r is not None else 0.0
        uv = [((u(q) + uo) / TILE, (-(q[1] - yref) + vo) / TILE) for q in (a, b, c, d)]
        m.quad("wall", a, b, c, d, uv)
    side((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), lambda q: q[0] - x0)
    side((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), lambda q: x1 - q[0])
    side((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), lambda q: z1 - q[2])
    side((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0), lambda q: q[2] - z0)
    if roof_top:
        pts = [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]
        m.quad("roof", *pts, [(q[0] / 2.5, q[2] / 2.5) for q in pts])


def pbox(m, x0, x1, y0, y1, z0, z1, tile=3.0, top=True):
    """A podium box in the plain 'base' material."""
    def side(a, b, c, d, ax, rev):
        m.quad("base", a, b, c, d, [((q[ax] if not rev else -q[ax]) / tile, -q[1] / tile) for q in (a, b, c, d)])
    side((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), 0, False)
    side((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), 0, True)
    side((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), 2, True)
    side((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0), 2, False)


def pb(m, x0, x1, y0, y1, z0, z1, *_unused, top="roof", roof_m=2.5, skip=()):
    """A plain box (podium material) with a roof-material top unless skipped."""
    pbox(m, x0, x1, y0, y1, z0, z1)
    if "top" not in skip and top:
        pts = [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]
        m.quad("roof", *pts, [(q[0] / roof_m, q[2] / roof_m) for q in pts])


def pslab(m, x0, x1, z0, z1, y, thick, over, *_unused):
    """A flat roof slab with an overhang, in the podium material, roof texture on top."""
    pb(m, x0 - over, x1 + over, y, y + thick, z0 - over, z1 + over)


EXTRA_TEX = {}
FLAT_NORMAL = jpg(np.full((8, 8, 3), (128, 128, 255), np.uint8), 95)
L1_CLADS = [os.path.join(MD_SRC, "MD_L1_wall.jpg"), os.path.join(MD_SRC, "MD_L5_wall.jpg"), os.path.join(MD_SRC, "MD_L4_wall.jpg"),
            os.path.join(LD_SRC, "L5_wall.jpg"), os.path.join(RH_SRC, "RH_L3_wall.jpg")]


def _clad_box(m, x0, x1, y0, y1, z0, z1):
    for a, b, c, d in (((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)), ((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)),
                       ((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)), ((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0))):
        m.quad("clad", a, b, c, d, [(0, 0), (1, 0), (1, 1), (0, 1)])


def snap(v, lo=9.0): return max(lo, math.floor(v / 3.0 + 1e-6) * 3.0)


def crown(m, x0, x1, y, z0, z1, pal_level, mast=False):
    """A tower top: a parapet slab, a plant room, optionally a mast."""
    pslab(m, x0, x1, z0, z1, y, 0.5, 0.25, g.LEVELS[1]["wall_m"], 2.5)
    cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
    pb(m, cx - 2.2, cx + 2.2, y + 0.5, y + 3.0, cz - 1.8, cz + 1.8, 2.4, top="roof", roof_m=2.5)
    if mast: pb(m, cx - 0.12, cx + 0.12, y + 3.0, y + 3.0 + 7.0, cz - 0.12, cz + 0.12, 2.4, skip=("top",))
    return y + 3.0 + (7.0 if mast else 0.0)


def build(W, D, level):
    m = Mesh(); pal = PAL[level]; EXTRA_TEX.clear()
    FACE_RNG[0] = np.random.default_rng(W * 131 + D * 17 + level * 7)
    lw, ld = W * CELL, D * CELL
    side = 1 if (W + D) % 2 == 0 else -1
    xl, xr = -lw / 2 + 1.0, lw / 2 - 1.0
    if side > 0: xr -= ap.DRIVE_W + 0.8
    else: xl += ap.DRIVE_W + 0.8
    zf, zb = ld / 2 - 3.2, -ld / 2 + 1.5
    zone_w, zone_d = xr - xl, zf - zb
    floors = FLOORS[level] + int((W * D - 4) * 0.3)
    # podium: the whole zone, one storey and a half of lobby and storefront, a terrace on top
    pbox(m, xl, xr, 0, PODIUM, zb, zf)
    pslab(m, xl, xr, zb, zf, PODIUM, 0.22, 0.12, 3.0, 2.5)
    yref = PODIUM + 0.22                                    # storey lines start on the terrace
    ecx = (xl + xr) / 2
    ty = PODIUM + 0.22
    top = ty
    tz1 = zf - 4.2                                          # the tower stands back, leaving a terrace in front

    if level == 1:                                          # slab with character: its own design and SPOM walls (l1_hd.py)
        import l1_hd, facade_hd
        r1 = np.random.default_rng(W * 977 + D * 131 + 5)
        dz = l1_hd.design(r1, L1_CLADS)
        tw = snap(min(zone_w, float(r1.choice([18.0, 21.0, 24.0, 27.0])))); td = snap(min(zone_d - 4.2, float(r1.choice([9.0, 12.0]))), 9.0)
        floors = max(5, floors + int(r1.integers(-3, 5)))
        x0, x1 = ecx - tw / 2, ecx + tw / 2; z1 = tz1 - 1.2; z0 = z1 - td
        H = ty + floors * FLOOR
        fbox(m, x0, x1, ty, H, z0, z1, yref, roof_top=False)
        gl, _ = facade_hd.glass_field(W * 31 + D, pal["glass"], head=2.3)
        EXTRA_TEX["wall"] = (jpg(gl, 86), FLAT_NORMAL)
        nmat = l1_hd.dress_tower(m, dz, r1, x0, x1, z0, z1, ty, floors, EXTRA_TEX, seed=W * 1000 + D * 10)
        t = l1_hd.T_WALL; cc = np.clip(dz["paint"] * 0.92 * 255, 0, 255).astype(np.uint8)
        EXTRA_TEX["clad"] = (jpg(np.tile(cc, (64, 64, 1)), 90), FLAT_NORMAL)
        for (cx_, cz_) in ((x0 - t, z1), (x1, z1), (x0 - t, z0 - t), (x1, z0 - t)):        # corner columns close the wall's thickness
            m.prims.setdefault("clad", []); _clad_box(m, cx_, cx_ + t, ty, H, cz_, cz_ + t)
        top = crown(m, x0 - 0.4, x1 + 0.4, H, z0 - 0.4, z1 + 0.4, 1)
        print(f"   L1 {W}x{D}: {l1_hd.describe(dz)}; {floors} floors, {nmat} strip textures")
    elif level == 2:                                        # L-shape
        tw = snap(min(zone_w, 24.0)); wing = 9.0
        x0, x1 = ecx - tw / 2, ecx + tw / 2; z1 = tz1; z0 = z1 - 9.0
        H = ty + floors * FLOOR
        fbox(m, x0, x1, ty, H, z0, z1, yref, roof_top=False); crown(m, x0, x1, H, z0, z1, 2)
        wz0 = max(zb + 0.5, z0 - snap(min(zone_d - 13.2, 18.0), 9.0)) if zone_d - 13.2 >= 9 else None
        if wz0 is not None and z0 - wz0 >= 9:
            wx0, wx1 = (x0, x0 + wing) if side > 0 else (x1 - wing, x1)
            fbox(m, wx0, wx1, ty, H - FLOOR * 2, wz0, z0, yref, roof_top=False); pslab(m, wx0, wx1, wz0, z0, H - FLOOR * 2, 0.5, 0.25, 2.4, 2.5)
        # brick towers carry corner pilasters: two slim full-height boxes
        for cx_ in (x0, x1 - 0.6):
            pb(m, cx_ - 0.0, cx_ + 0.6, ty, H + 0.9, z1, z1 + 0.35, 2.4, skip=("top",))
        top = H + 3.0
    elif level == 3:                                        # glass tower with a crown and mast
        s = snap(min(zone_w * 0.8, zone_d - 4.2, 24.0)); s = max(9.0, s)
        x0, x1 = ecx - s / 2, ecx + s / 2; z1 = tz1; z0 = z1 - s
        H = ty + floors * FLOOR
        fbox(m, x0, x1, ty, H, z0, z1, yref, roof_top=False); top = crown(m, x0 + 0.0, x1, H, z0, z1, 3, mast=True)
    elif level == 4:                                        # three setback tiers and a spire
        s = snap(min(zone_w * 0.9, zone_d - 4.2, 27.0)); s = max(12.0, s)
        y = ty; cx0 = ecx; zc = tz1 - s / 2
        tiers = [(s, floors), (s - 6.0, max(3, floors - 4)), (max(6.0, s - 12.0), max(2, floors - 8))]
        for i, (ts, nf) in enumerate(tiers):
            if ts < 6.0: break
            x0, x1, z0, z1 = cx0 - ts / 2, cx0 + ts / 2, zc - ts / 2, zc + ts / 2
            yt = y + nf * FLOOR
            fbox(m, x0, x1, y, yt, z0, z1, yref, roof_top=False)
            pslab(m, x0, x1, z0, z1, yt, 0.4, 0.3, 1.8, 2.5)
            y = yt + 0.4
        top = y
        cs = (tiers[-1][0] if len(tiers) else 6.0)
        pb(m, cx0 - 1.2, cx0 + 1.2, y, y + 4.0, zc - 1.2, zc + 1.2, 1.8, top="roof", roof_m=2.5)
        pb(m, cx0 - 0.15, cx0 + 0.15, y + 4.0, y + 12.0, zc - 0.15, zc + 0.15, 1.8, skip=("top",)); top = y + 12.0
    else:                                                   # twin balcony towers (one on narrow plots), skybridge, roof gardens
        n = 2 if zone_w >= 24 else 1
        ts = snap(min((zone_w - (6.0 if n == 2 else 0)) / n * 0.9, zone_d - 4.2, 18.0)); ts = max(9.0, ts)
        centers = [ecx] if n == 1 else [ecx - zone_w / 4, ecx + zone_w / 4]
        H = ty + floors * FLOOR
        for cx_ in centers:
            x0, x1, z1 = cx_ - ts / 2, cx_ + ts / 2, tz1; z0 = z1 - ts
            fbox(m, x0, x1, ty, H, z0, z1, yref, roof_top=False)
            pslab(m, x0, x1, z0, z1, H, 0.4, 0.3, 3.0, 3.0)                              # roof garden slab (sedum roof texture)
            pb(m, x0 + 0.5, x1 - 0.5, H + 0.4, H + 1.0, z0 + 0.5, z1 - 0.5, 3.0, top="roof", roof_m=3.0)   # planting bed
            top = max(top, H + 1.0)
        if n == 2:
            bx0, bx1 = centers[0] + ts / 2, centers[1] - ts / 2
            by = ty + int(floors * 0.6) * FLOOR
            fbox(m, bx0, bx1, by, by + FLOOR, tz1 - ts * 0.6, tz1 - ts * 0.3, yref)           # skybridge between the towers
    # podium: lobby, storefront glazing, a garage door on the driveway side, canopy over the entrance, roof-terrace railings
    deco(m, "door", ecx, 1.4, zf, 2.0, 2.6, +1, "x")
    canopy_over(m, ecx, 2.8, zf)
    pb(m, ecx - 1.6, ecx + 1.6, 0, 0.16, zf, zf + 0.6, 3.0, top="roof", roof_m=2.0)
    nb = max(2, int((zone_w - 3.0) // 4.2))
    for i in range(nb):
        c = xl + (i + 0.5) * zone_w / nb
        if abs(c - ecx) > 2.6: deco(m, "wide", c, 1.9, zf, 3.0, 2.2, +1, "x")
    gx = xr if side > 0 else xl                                # the garage door faces the driveway
    gcz = zb + zone_d * 0.45
    deco(m, "garage", gcz, 1.4, gx, 3.2, 2.6, +1 if side > 0 else -1, "z")
    nrail = int(math.ceil((zone_w - 0.4) / 3.0))
    for i in range(nrail):
        c = xl + 0.2 + (i + 0.5) * (zone_w - 0.4) / nrail
        spom.spom_box(m, "rail", np.array([c - 1.5, ty, zf - 0.3 - 1.0], np.float32), np.array([0, 0, 1], np.float32))
    # ground: the road system's own paving for the driveway, apron and a walk to the entrance
    class _H: pass
    h = _H(); h.m = m; h.level = level
    ap.add_ground(h, lw, ld, side, ecx, zf + 0.6)
    return m, (lw, ld, top)


def main():
    os.makedirs(OUT, exist_ok=True); os.makedirs(TEX, exist_ok=True)
    only = [int(a) for a in sys.argv[1:] if a.isdigit()] or [1, 2, 3, 4, 5]
    plots = [tuple(map(int, a.split('x'))) for a in sys.argv[1:] if 'x' in a] or PLOTS
    for lv in only:
        p = dict(g.LEVELS[1]); p.update(PAL[lv])
        base_tex = tile_maps(BASES[lv], 7.0); roof_tex = tile_maps(ROOFS[lv], 5.0)
        wall_tex = facade_tile(lv)
        open(os.path.join(TEX, f"HD_L{lv}_facade_albedo.jpg"), "wb").write(wall_tex[0]); open(os.path.join(TEX, f"HD_L{lv}_facade_normal.png"), "wb").write(wall_tex[1])
        ap.DRIVE_W = ap.DRIVE_W
        tex = {"wall": wall_tex, "base": base_tex, "roof": roof_tex, "detail": detail_sheet(p),
               "spom_balcony": spom.relief("balcony", p), "spom_canopy": spom.relief("canopy", p), "spom_rail": spom.relief("rail", p), "drive": drive_tex(lv)}
        for (W, D) in plots:
            mesh, dims = build(W, D, lv)
            out = os.path.join(OUT, f"HD_{W}x{D}_L{lv}.glb")
            t2 = dict(tex); t2.update(EXTRA_TEX)
            write_glb(out, mesh, t2)
            print(f"HD_{W}x{D}_L{lv}: {mesh.tri_count():4d} tris, {os.path.getsize(out)/1e6:.2f} MB, height {dims[2]:.0f} m")


if __name__ == "__main__":
    main()
