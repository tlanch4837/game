"""Deathcare buildings -> GLB: two cemeteries and two crematoriums (CS2 has the Cemetery and the Crematorium; each in a small and a large size).
Built from the crematorium_graveyard.html mock-up but low poly: halls and chapels are boxes with gable roofs and atlas windows, facades get SPOM relief
(piers, cornice, plinth), the grave rows are SPOM relief SHEETS (a whole row of headstones, crosses and arches is one standing volume, one on each
face so it reads from both sides), paths, plots and the plinth are painted into one ground texture. No trees, benches, lamp posts or cars baked in.
Frame: lot centred on the origin, front (street / entrance) at +z, ground at y = 0.
Run: python gen_deathcare.py   Output: assets/buildings/deathcare/glb + preview.html + catalog.json
"""
import json, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_low_density as g
from gen_low_density import Mesh, CELL
import gen_bus_depot as b
import gen_harbor as hb
import spom

BASE = os.path.join(g.ROOT, "assets", "buildings", "deathcare")
OUT = os.path.join(BASE, "glb")


# ---------------------------------------------------------------- textures
def ground_tex(W, D, paths, plots, plinth=None, lawn=(88, 122, 70)):
    """Whole-lot ground at 10 px/m: lawn, gravel paths, grave plots (slightly raised soil rectangles), optional paved plinth, kerb."""
    px = 10
    w, d = int(W * px), int(D * px)
    im = Image.new("RGB", (w, d), lawn); dr = ImageDraw.Draw(im)
    X = lambda x: (x + W / 2) * px; Z = lambda z: (z + D / 2) * px
    for (x0, x1, z0, z1) in plots:                                              # soil beds, each row a strip with a darker edge
        dr.rectangle([X(x0), Z(z0), X(x1), Z(z1)], fill=(96, 104, 70), outline=(70, 90, 56), width=2)
    if plinth:
        dr.rectangle([X(plinth[0]), Z(plinth[2]), X(plinth[1]), Z(plinth[3])], fill=(176, 172, 164))
        for x in np.arange(plinth[0], plinth[1], 4.0): dr.line([X(x), Z(plinth[2]), X(x), Z(plinth[3])], fill=(150, 148, 142), width=2)
        for z in np.arange(plinth[2], plinth[3], 4.0): dr.line([X(plinth[0]), Z(z), X(plinth[1]), Z(z)], fill=(150, 148, 142), width=2)
    for (x0, x1, z0, z1) in paths:                                              # gravel paths
        dr.rectangle([X(x0), Z(z0), X(x1), Z(z1)], fill=(184, 174, 152))
    dr.rectangle([0, 0, w - 1, d - 1], outline=(130, 128, 120), width=5)         # kerb
    arr = np.asarray(im.filter(ImageFilter.GaussianBlur(0.7)), np.float32) + b.noise(max(w, d), 5, 1.6, 41)[:d, :w, None]
    return b.maps(arr, np.asarray(im.convert("L"), np.float32) / 255, 2.0)


def stone_wall_tex(col):
    """Ashlar stone 3 m tile: courses 0.4 m high with staggered joints, soft variation per block."""
    n = 512
    r = np.random.default_rng(7)
    a = np.zeros((n, n, 3), np.float32); h = np.full((n, n), 0.6, np.float32)
    rows = 8
    for ry in range(rows):
        y0 = ry * n // rows
        bw = 96 + int(r.integers(0, 3)) * 24
        x = -int(r.integers(0, bw))
        while x < n:
            v = 0.9 + 0.16 * r.random()
            xa, xb = max(x, 0), min(x + bw, n)
            if xb > xa:
                a[y0:y0 + n // rows, xa:xb] = np.array(col, np.float32) * v
                h[y0:y0 + 3, xa:xb] = 0.15; h[y0:y0 + n // rows, xa:xa + 3] = 0.15
            x += bw
    a *= (0.95 + 0.1 * r.random((n, n, 1))).astype(np.float32)
    return b.maps(a, h, 4.0)


# ---------------------------------------------------------------- geometry helpers
def new_mesh():
    m = Mesh()
    return m


def gable(m, x0, x1, y, z0, z1, rise, over=0.5, wall="wall"):
    """Gable roof over [x0,x1]x[z0,z1] at eave height y, ridge along x; gable ends in the wall material."""
    zm = (z0 + z1) / 2
    e0, e1 = z0 - over, z1 + over
    for (za, zb, fa, fb) in ((e1, zm, 1, 0), (e0, zm, 0, 1)):
        m.quad("roof", (x0 - over, y - 0.15, za), (x1 + over, y - 0.15, za), (x1 + over, y + rise, zb), (x0 - over, y + rise, zb),
               [(0, 0), ((x1 - x0) / 3, 0), ((x1 - x0) / 3, 1.5), (0, 1.5)]) if za > zb else \
        m.quad("roof", (x1 + over, y - 0.15, za), (x0 - over, y - 0.15, za), (x0 - over, y + rise, zb), (x1 + over, y + rise, zb),
               [(0, 0), ((x1 - x0) / 3, 0), ((x1 - x0) / 3, 1.5), (0, 1.5)])
    for xe, a, c in ((x1, (x1, y, z1), (x1, y, z0)), (x0, (x0, y, z0), (x0, y, z1))):
        m.tri(wall, a, c, (xe, y + rise - 0.15, zm), [(a[2] / 3, -y / 3), (c[2] / 3, -y / 3), (zm / 3, -(y + rise) / 3)])


def hall(m, x0, x1, z0, z1, h, rise, front_windows, door_x=None, door_w=3.4, step=3.6, wall="wall"):
    """A gable hall: four wall faces (the +z face gets a door and glazing from the atlas), gable roof, no bottom."""
    b.mbox(m, x0, x1, 0.0, h, z0, z1, wall=wall, skip=("+z",), top=None)
    b.zface(m, x0, x1, 0.0, h, z1, 1, wall, b.wuv(x0, x1, 0.0, h))
    if door_x is not None:
        b.zface(m, door_x - door_w / 2, door_x + door_w / 2, 0.0, 3.4, z1 + 0.03, 1, "detail", b.auv("door"))
    if front_windows:
        for (a, c) in front_windows:
            b.windows_z(m, a, c, 1.2, h - 1.0 if h < 7 else 5.2, z1, 1, step=step, kind="win" if h >= 7 else "wide")
    for xs, f in ((x1, 1), (x0, -1)):
        b.windows_x(m, z0 + 2.0, z1 - 2.0, 1.4, 4.2, xs, f, 4.0, "wide")
    gable(m, x0, x1, h, z0, z1, rise, wall=wall)


def facade(m, kind, W, H, origin, h_total, accent, wallc, piers, band_y):
    """SPOM relief on a +z facade: piers at the given x (metres from the left edge), plinth, cornice, accent band."""
    def paint(a):
        for xc in piers: a(xc - 0.25, xc + 0.25, 0.0, h_total - 0.3, 0.0, 0.34)
        a(0.0, W, 0.0, 0.7, 0.0, 0.5, "dark")
        a(0.0, W, band_y, band_y + 0.35, 0.0, 0.55, "accent")
        a(0.0, W, h_total - 0.3, h_total + 0.1, 0.0, 0.6)
        a(0.0, W, h_total + 0.1, h_total + 0.28, 0.0, 0.42)
    return b.spom_volume(m, kind, W, H, origin, paint, wallc, accent)


STONE_COLS = {"stone": (0.6, 0.6, 0.58), "stone2": (0.8, 0.79, 0.75), "stone3": (0.46, 0.47, 0.48), "plinth": (0.34, 0.34, 0.33)}


def grave_row_sheet(kind, L, seed):
    """One row of headstones as relief: plinths, slabs with arched tops, crosses and obelisks. Painted in x from the row's left end (mirror False)."""
    sh = hb.Sheet(kind, False, 0.0, 0.0, L, 1.5, axis="z")
    sh.col.update(STONE_COLS)
    r = np.random.default_rng(seed)
    pitch = 1.3
    n = int(L // pitch)
    x0 = (L - n * pitch) / 2
    for i in range(n):
        xc = x0 + (i + 0.5) * pitch
        tag = ("stone", "stone2", "stone3")[int(r.integers(0, 3))]
        sh.add(xc - 0.36, xc + 0.36, 0.0, 0.12, 0.0, 0.34, "plinth")
        form = int(r.integers(0, 4))
        if form == 0:        # arched slab
            sh.add(xc - 0.28, xc + 0.28, 0.12, 0.86, 0.0, 0.2, tag)
            for k, (w, y0, y1) in enumerate(((0.24, 0.86, 0.92), (0.17, 0.92, 0.97), (0.08, 0.97, 1.0))): sh.add(xc - w, xc + w, y0, y1, 0.0, 0.2, tag)
        elif form == 1:      # cross
            sh.add(xc - 0.06, xc + 0.06, 0.12, 1.15, 0.0, 0.16, tag); sh.add(xc - 0.28, xc + 0.28, 0.72, 0.84, 0.0, 0.16, tag)
        elif form == 2:      # plain slab, taller
            sh.add(xc - 0.26, xc + 0.26, 0.12, 1.0, 0.0, 0.18, tag)
        else:                # obelisk
            sh.add(xc - 0.14, xc + 0.14, 0.12, 0.9, 0.0, 0.18, tag); sh.add(xc - 0.09, xc + 0.09, 0.9, 1.1, 0.0, 0.14, tag)
            sh.add(xc - 0.04, xc + 0.04, 1.1, 1.22, 0.0, 0.1, tag)
    return sh


def place_rows(m, tex, prev, rows, L, x_left, seed0):
    """rows: z of each grave row. One relief kind per row (so no two rows repeat), each emitted front and back."""
    for i, zrow in enumerate(rows):
        sh = grave_row_sheet("grave%d_%d" % (seed0, i), L, seed0 * 31 + i)
        for mirror, xo in ((False, x_left), (True, x_left + L)):
            snap = sh.place(zrow, xo, mirror)
            sh.emit(m, 0.0)
            prev.append((sh, 0.0, snap))
        tex["spom_" + sh.kind] = sh.L.encode()
        tex["sprev_" + sh.kind] = (sh.preview(), hb.flat_normal())


# ---------------------------------------------------------------- models
def cemetery(small):
    cells = (4, 4) if small else (6, 6)
    W, D = cells[0] * CELL, cells[1] * CELL
    m = new_mesh(); prev = []; tex = {}
    wallc = (196, 190, 176)
    accent = (70, 90, 120)
    vols = []
    hx, hz = W / 2 - 1.5, D / 2 - 1.5
    # chapel at the back: stone box, gable roof, glazed front, blank sign
    cw, cd, ch = (10.0, 7.0, 4.6) if small else (14.0, 9.0, 5.6)
    cz1 = -hz + cd
    hall(m, -cw / 2, cw / 2, -hz, cz1, ch, 2.8 if small else 3.4, [(-cw / 2 + 0.6, -2.4), (2.4, cw / 2 - 0.6)], door_x=0.0, step=3.2)
    vols.append(facade(m, "chapel", cw, ch + 0.3, (-cw / 2, 0.0, cz1), ch, accent, wallc, [0.25, cw - 0.25, cw / 2 - 2.4, cw / 2 + 2.4], ch - 0.7))
    if not small:                         # bell tower on the chapel and a mausoleum on the left
        b.mbox(m, cw / 2 - 3.0, cw / 2, ch, ch + 5.5, -hz + 1.0, -hz + 4.0, top="roof", skip=("-y",))
        b.mbox(m, cw / 2 - 2.6, cw / 2 - 0.4, ch + 5.5, ch + 6.3, -hz + 1.4, -hz + 3.6, top="roof")
        mx0, mx1, mz0, mz1, mh = -hx, -hx + 11.0, -hz, -hz + 8.0, 5.0
        b.mbox(m, mx0, mx1, 0.0, mh, mz0, mz1, skip=("+z",), top="roof")
        b.zface(m, mx0, mx1, 0.0, mh, mz1, 1, "wall", b.wuv(mx0, mx1, 0.0, mh))
        b.zface(m, mx0 + 4.0, mx1 - 4.0, 0.0, 3.2, mz1 + 0.03, 1, "detail", b.auv("door"))
        vols.append(facade(m, "mausoleum", mx1 - mx0, mh + 0.3, (mx0, 0.0, mz1), mh, accent, wallc, [0.4, 3.6, 7.4, 10.6], mh - 0.8))
    # entrance gate piers with a blank arch sign between them
    for sx in (-3.2, 3.2): b.mbox(m, sx - 0.5, sx + 0.5, 0.0, 3.4, hz - 0.5, hz + 0.5, top="roof")
    b.zface(m, -2.7, 2.7, 2.6, 3.2, hz + 0.52, 1, "detail", [(236 / 512, 240 / 512)] * 4)
    # grave rows
    paths, plots = [(-1.2, 1.2, -hz + cd, D / 2), (-hx, hx, -hz + cd + 1.5, -hz + cd + 3.0)], []
    if small:
        rows = [-2.0, 2.5, 7.0]
        place_rows(m, tex, prev, rows, 24.0, -12.0, 1)
        for z in rows: plots.append((-12.0, 12.0, z - 2.2, z + 0.2))
        paths = [(-1.2, 1.2, -hz + cd, D / 2)]
    else:
        rows = [-3.0, 3.0, 9.0, 15.0]
        place_rows(m, tex, prev, rows, 18.0, -21.0, 2)
        place_rows(m, tex, prev, rows, 18.0, 3.0, 3)
        for z in rows:
            plots += [(-21.0, -3.0, z - 2.4, z + 0.2), (3.0, 21.0, z - 2.4, z + 0.2)]
        paths = [(-1.2, 1.2, -hz + cd, D / 2), (-hx, hx, -hz + cd + 0.5, -hz + cd + 2.0)]
    m.quad("base", (-W / 2, 0.0, D / 2), (W / 2, 0.0, D / 2), (W / 2, 0.0, -D / 2), (-W / 2, 0.0, -D / 2), [(0, 1), (1, 1), (1, 0), (0, 0)])
    tex.update({"base": ground_tex(W, D, paths, plots), "wall": stone_wall_tex(wallc), "roof": b.roof_tex(), "detail": b.detail_tex(accent)})
    return cells, m, tex, vols, prev


def crematorium(small):
    cells = (4, 4) if small else (5, 5)
    W, D = cells[0] * CELL, cells[1] * CELL
    m = new_mesh(); prev = []; tex = {}
    wallc = (170, 120, 96)                           # warm brick
    accent = (60, 64, 72)
    vols = []
    hx, hz = W / 2 - 1.5, D / 2 - 1.5
    hw, hd, hh = (18.0, 10.0, 6.4) if small else (24.0, 12.0, 7.6)
    hz1 = -hz + hd + (0 if small else 2.0)
    # main chapel hall, centred, entrance on +z
    hall(m, -hw / 2, hw / 2, hz1 - hd, hz1, hh, 3.2 if small else 4.0, [(-hw / 2 + 0.6, -2.2), (2.2, hw / 2 - 0.6)], door_x=0.0, door_w=3.6, step=3.4)
    piers = [0.25, hw - 0.25, hw / 2 - 2.2, hw / 2 + 2.2] + ([hw / 4, 3 * hw / 4] if not small else [])
    vols.append(facade(m, "hall", hw, hh + 0.3, (-hw / 2, 0.0, hz1), hh, accent, wallc, piers, hh - 0.8))
    # covered porch / porte-cochere in front of the door: flat slab on slim columns
    pw, pd = (8.0, 4.5) if small else (12.0, 5.5)
    b.mbox(m, -pw / 2, pw / 2, 4.2, 4.6, hz1, hz1 + pd, top="roof", bottom=True)
    for sx in (-pw / 2 + 0.4, pw / 2 - 0.4):
        for sz in (hz1 + pd - 0.4,): b.mbox(m, sx - 0.2, sx + 0.2, 0.0, 4.2, sz - 0.2, sz + 0.2, top=None)
    b.zface(m, -2.5, 2.5, 4.6, 5.5, hz1 + 0.04, 1, "detail", [(236 / 512, 240 / 512)] * 4)   # blank sign above the porch
    # cremator wing at the back-right: low block with roll-up service door, plus the chimney
    wx0, wx1 = hw / 2 - 1.0, hw / 2 + (9.0 if small else 11.0)
    wz0, wz1 = -hz, -hz + (9.0 if small else 11.0)
    b.mbox(m, wx0, wx1, 0.0, 5.0, wz0, wz1, skip=("+z",), top="roof")
    b.zface(m, wx0, wx1, 0.0, 5.0, wz1, 1, "wall", b.wuv(wx0, wx1, 0.0, 5.0))
    b.zface(m, wx0 + 1.2, wx0 + 5.2, 0.0, 3.6, wz1 + 0.03, 1, "detail", b.auv("garage"))
    b.mbox(m, wx0 + 5.8, wx1 - 1.0, 4.4, 5.6, wz0 + 1.0, wz0 + 3.5, top="roof")                   # roof plant
    cx, cz = wx1 - 2.0, wz0 + 2.0                                                                 # chimney: tapered square stack = three stacked boxes
    ch = 17.0 if small else 22.0
    b.mbox(m, cx - 1.1, cx + 1.1, 5.0, 5.0 + ch * 0.45, cz - 1.1, cz + 1.1, top=None)
    b.mbox(m, cx - 0.9, cx + 0.9, 5.0 + ch * 0.45, 5.0 + ch * 0.8, cz - 0.9, cz + 0.9, top=None)
    b.mbox(m, cx - 0.75, cx + 0.75, 5.0 + ch * 0.8, 5.0 + ch, cz - 0.75, cz + 0.75, top=None)
    b.mbox(m, cx - 1.0, cx + 1.0, 5.0 + ch, 5.0 + ch + 0.6, cz - 1.0, cz + 1.0, wall="detail", top="detail")
    if not small:                                                                                 # columbarium wall on the left
        kx0, kx1, kz0, kz1 = -hw / 2 - 6.0, -hw / 2 - 3.5, -hz + 1.0, -hz + 15.0
        b.mbox(m, kx0, kx1, 0.0, 4.0, kz0, kz1, top="roof")
        b.mbox(m, kx0 - 0.3, kx1 + 0.3, 4.0, 4.3, kz0 - 0.3, kz1 + 0.3, top="roof")
    plinth = (-hx, hx, -hz, hz)
    paths = [(-1.4, 1.4, hz1, D / 2)]
    plots = [(-hx + 1.0, -hw / 2 - 1.0, hz1 + 1.0, hz - 1.0)] if small else [(-hx + 1.0, -hw / 2 - 1.0, hz1 + 3.0, hz - 1.0)]
    m.quad("base", (-W / 2, 0.0, D / 2), (W / 2, 0.0, D / 2), (W / 2, 0.0, -D / 2), (-W / 2, 0.0, -D / 2), [(0, 1), (1, 1), (1, 0), (0, 0)])
    tex.update({"base": ground_tex(W, D, paths, plots, plinth=plinth), "wall": b.wall_tex(wallc), "roof": b.roof_tex(), "detail": b.detail_tex(accent)})
    return cells, m, tex, vols, prev


MODELS = [
    ("CM_4x4_small_cemetery_L1", lambda: cemetery(True)),
    ("CM_6x6_cemetery_L2", lambda: cemetery(False)),
    ("CR_4x4_crematorium_L1", lambda: crematorium(True)),
    ("CR_5x5_large_crematorium_L2", lambda: crematorium(False)),
]


def _adapt(f):
    cells, c = f()
    return cells, c.m, c.tex, [], c.prev


def main():
    import gen_deathcare_campus as cp
    os.makedirs(OUT, exist_ok=True)
    catalog = []
    entries = list(MODELS) + [(n, (lambda f=f: _adapt(f))) for (n, f) in cp.MODELS]
    for (name, fn) in entries:
        cells, m, tex, vols, prev = fn()
        for (kind, maps_, boxes, origin) in vols: tex["spom_" + kind] = maps_
        mats = {"detail": {"rough": 0.35, "metal": 0.1}, "base": {"rough": 0.95}}
        for k in list(tex):
            if k.startswith("sprev_"): mats[k] = {"rough": 0.6, "blend": True, "alpha": 1.0}
        m.prims = {k: v for k, v in m.prims.items() if v}
        pm_tex = dict(tex)
        tex = {k: v for k, v in tex.items() if k in m.prims}
        p = os.path.join(OUT, name + ".glb")
        g.write_glb(p, m, tex, mats)
        tris = m.tri_count()
        spom_tris = sum(2 if c[3] == 4 else 1 for k, l in m.prims.items() if k.startswith("spom_") for c in l)
        pm = Mesh(); pm.prims = {k: list(v) for k, v in m.prims.items() if not k.startswith("spom_")}
        pm.prims.setdefault("detail", [])
        for (kind, maps_, boxes, origin) in vols:
            for bx in boxes: b.pbox(pm, bx, origin)
        for item in prev:
            sh, y0, snap = item[:3]
            sh.place(*snap)
            if len(item) == 4:
                tmp = Mesh(); sh.emit_preview(tmp, y0); cp.merge(pm, tmp, *item[3])
            else:
                sh.emit_preview(pm, y0)
        os.makedirs(os.path.join(OUT, "preview"), exist_ok=True)
        g.write_glb(os.path.join(OUT, "preview", name + "_spom_preview.glb"), pm, {k: v for k, v in pm_tex.items() if k in pm.prims}, mats)
        W, D = cells[0] * CELL, cells[1] * CELL
        catalog.append({"file": name + ".glb", "preview": "preview/" + name + "_spom_preview.glb", "name": name, "lot": list(cells), "lot_m": [W, D],
                        "tris": tris, "spom_tris": spom_tris, "materials": len(m.prims), "kb": os.path.getsize(p) // 1024})
        print(f"{name}: {tris} tris ({spom_tris} SPOM relief, {tris - spom_tris} real), {len(m.prims)} materials, {os.path.getsize(p) // 1024} KB")
    json.dump(catalog, open(os.path.join(BASE, "catalog.json"), "w"), indent=1)
    open(os.path.join(BASE, "preview.html"), "w", encoding="utf-8").write(b.PREVIEW.replace("<title>Bus depot</title>", "<title>Deathcare</title>").replace('g.scene.traverse(o=>{if(o.isMesh)o.castShadow=o.receiveShadow=true})', 'g.scene.traverse(o=>{if(o.isMesh){o.castShadow=o.receiveShadow=true;if(o.material.transparent){o.castShadow=false;o.receiveShadow=false}}})'))
    camp = b.PREVIEW.replace("<title>Bus depot</title>", "<title>Deathcare campus lots</title>").replace("for(const e of cat){", "for(const e of cat.filter(e=>/_L3/.test(e.name))){").replace("cam.position.set(30,32,78)", "cam.position.set(80,150,170)").replace("oc.target.set(30,3,-4)", "oc.target.set(80,0,0)").replace('g.scene.traverse(o=>{if(o.isMesh)o.castShadow=o.receiveShadow=true})', 'g.scene.traverse(o=>{if(o.isMesh){o.castShadow=o.receiveShadow=true;if(o.material.transparent){o.castShadow=false;o.receiveShadow=false}}})')
    open(os.path.join(BASE, "campus_preview.html"), "w", encoding="utf-8").write(camp)


if __name__ == "__main__":
    main()
