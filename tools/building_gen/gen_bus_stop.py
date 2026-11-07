"""Bus stop -> GLB: a shelter (glass back and ends, flat roof, SPOM bench / ad frame / route case / roof fascia) and a pole stop.
Reuses gen_bus_depot's textures and SPOM helpers so the stops match the depots (same orange transit accent).
No buses, people, trees, lamp posts or road/sidewalk baked in; the pad is only the stop's own concrete slab. Name panel left blank.
Run: python gen_bus_stop.py    Output: assets/buildings/transport/bus_stop/glb (+ preview/, preview.html, catalog.json)
"""
import json, os, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_bus_depot as bd
import gen_low_density as g
from gen_low_density import Mesh

BASE = os.path.join(g.ROOT, "assets", "buildings", "transport", "bus_stop")
OUT = os.path.join(BASE, "glb")
ACC = (210, 120, 20)
WALLC = (186, 184, 176)


def sign_tex():
    """Pole model's detail atlas (one 512 sheet): blue bus plate with white bus pictogram (0,0,256,256), blank white name panel (256,0..), dark metal."""
    im = Image.new("RGB", (512, 512), (60, 64, 68)); d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 255, 255], fill=(20, 80, 150)); d.rectangle([8, 8, 247, 247], outline=(245, 245, 245), width=6)
    d.rounded_rectangle([56, 70, 200, 170], 14, fill=(245, 245, 245))            # bus body
    d.rectangle([68, 82, 188, 120], fill=(20, 80, 150))                            # windows
    for x in (96, 128, 160): d.rectangle([x - 2, 82, x + 2, 120], fill=(245, 245, 245))
    for cx in (88, 168): d.ellipse([cx - 14, 158, cx + 14, 186], fill=(20, 80, 150), outline=(245, 245, 245), width=4)
    d.rectangle([256, 0, 511, 255], fill=(240, 240, 236)); d.rectangle([264, 8, 503, 247], outline=(30, 30, 30), width=4)   # blank name panel
    d.rectangle([0, 256, 255, 511], fill=(*ACC,))                                  # accent
    a = np.asarray(im, np.float32) * (0.96 + 0.08 * np.random.default_rng(3).random((512, 512, 1)))
    return bd.maps(a, np.full((512, 512), 0.5, np.float32), 3.0)


def thin_glass(m, x0, x1, y0, y1, z, uvs):          # two-sided pane on a z plane
    bd.zface(m, x0, x1, y0, y1, z, 1, "detail", uvs); bd.zface(m, x0, x1, y0, y1, z, -1, "detail", uvs)


def thin_glass_x(m, z0, z1, y0, y1, x, uvs):
    bd.xface(m, z0, z1, y0, y1, x, 1, "detail", uvs); bd.xface(m, z0, z1, y0, y1, x, -1, "detail", uvs)


def build_shelter():
    m = Mesh(); vols = []
    W, Z0, Z1 = 4.0, -0.9, 1.1
    bd.mbox(m, -2.4, 2.4, 0.0, 0.15, -1.3, 1.5, wall="drive", top="drive", t=2.0)                 # stop pad
    thin_glass(m, -W / 2, W / 2, 0.15, 2.55, Z0, bd.auv("wide"))                                    # back glazing
    thin_glass_x(m, Z0, Z1, 0.15, 2.55, -W / 2, bd.auv("win"))                                      # end panes
    thin_glass_x(m, Z0, 0.2, 0.15, 2.55, W / 2, bd.auv("win"))                                      # right end half-open (entry side)
    for x in (-W / 2, W / 2): bd.mbox(m, x - 0.06, x + 0.06, 0.15, 2.6, Z0 - 0.06, Z0 + 0.06, wall="wall", top=None)   # posts
    bd.mbox(m, -2.25, 2.25, 2.6, 2.78, -1.05, 1.3, wall="wall", top="roof", bottom=True)           # roof slab (closed underneath)
    def paint(a):
        a(0.55, 3.45, 0.45, 0.53, 0.02, 0.5)                                                         # bench seat
        a(0.55, 3.45, 0.55, 0.98, 0.02, 0.09, "dark")                                                # backrest
        for x in (0.62, 3.3): a(x, x + 0.1, 0.0, 0.45, 0.06, 0.44, "dark")                           # bench legs
        a(0.35, 1.85, 1.05, 2.25, 0.02, 0.10, "dark"); a(0.45, 1.75, 1.15, 2.15, 0.10, 0.12)         # ad panel frame + face
        a(2.4, 3.5, 1.25, 2.15, 0.02, 0.12, "dark"); a(2.46, 3.44, 1.31, 2.09, 0.12, 0.14)           # route-info case
        a(0.0, 4.0, 2.35, 2.45, 0.0, 0.14, "dark")                                                   # header rail
        a(-0.05, 4.05, 2.45, 2.6, 0.0, 0.62, "accent")                                               # accent fascia band
    vols.append(bd.spom_volume(m, "stop", 4.0, 2.7, (-W / 2, 0.15, Z0 + 0.04), paint, WALLC, ACC))
    return m, vols, ["detail", "wall", "roof", "drive"]


def build_pole_mesh():
    m = Mesh(); U_DARK = [(400 / 512, 400 / 512)] * 4
    bd.mbox(m, -0.6, 0.6, 0.0, 0.12, -0.6, 0.6, wall="drive", top="drive", t=2.0)
    x0, x1, y0, y1 = -0.05, 0.05, 0.12, 3.3                                                         # round-ish pole as a square tube, flat dark uv
    bd.zface(m, x0, x1, y0, y1, x1, 1, "detail", U_DARK); bd.zface(m, x0, x1, y0, y1, x0, -1, "detail", U_DARK)
    bd.xface(m, x0, x1, y0, y1, x1, 1, "detail", U_DARK); bd.xface(m, x0, x1, y0, y1, x0, -1, "detail", U_DARK)
    # bus plate (front and back), blank name panel under it
    for f in (1, -1):
        bd.zface(m, -0.32, 0.32, 2.55, 3.3, 0.06 * f, f, "detail", [(0.01, 0.49), (0.49, 0.49), (0.49, 0.01), (0.01, 0.01)])
        bd.zface(m, -0.32, 0.32, 2.1, 2.5, 0.06 * f, f, "detail", [(0.52, 0.49), (0.98, 0.49), (0.98, 0.01), (0.52, 0.01)])
    return m


def main():
    os.makedirs(os.path.join(OUT, "preview"), exist_ok=True)
    catalog = []
    roof = bd.roof_tex()
    jobs = []
    m, vols, _ = build_shelter()
    jobs.append(("BS_1x1_bus_shelter_L1", m, vols, {"wall": bd.wall_tex(WALLC), "roof": roof, "detail": bd.detail_tex(ACC), "drive": bd.wall_tex((150, 150, 146))}, [5.0, 3.0]))
    jobs.append(("BS_1x1_bus_stop_pole_L1", build_pole_mesh(), [], {"detail": sign_tex(), "drive": bd.wall_tex((150, 150, 146))}, [1.2, 1.2]))
    for (name, m, vols, tex, lot_m) in jobs:
        for (kind, maps_, boxes, origin) in vols: tex["spom_" + kind] = maps_
        mats = {"detail": {"rough": 0.3, "metal": 0.2}, "drive": {"rough": 0.9}}
        m.prims = {k: v for k, v in m.prims.items() if v}
        tex = {k: v for k, v in tex.items() if k in m.prims}
        p = os.path.join(OUT, name + ".glb"); g.write_glb(p, m, tex, mats)
        tris = m.tri_count(); st = sum(2 if c[3] == 4 else 1 for k, l in m.prims.items() if k.startswith("spom_") for c in l)
        pm = Mesh(); pm.prims = {k: list(v) for k, v in m.prims.items() if not k.startswith("spom_")}; pm.prims.setdefault("detail", [])
        for (kind, maps_, boxes, origin) in vols:
            for b in boxes: bd.pbox(pm, b, origin)
        pp = {k: v for k, v in tex.items() if k in pm.prims}
        g.write_glb(os.path.join(OUT, "preview", name + "_spom_preview.glb"), pm, pp, mats)
        catalog.append({"file": name + ".glb", "preview": "preview/" + name + "_spom_preview.glb", "name": name, "lot": [1, 1], "lot_m": lot_m,
                        "tris": tris, "spom_tris": st, "materials": len(m.prims), "kb": os.path.getsize(p) // 1024})
        print(f"{name}: {tris} tris ({st} SPOM), {len(m.prims)} materials, {os.path.getsize(p) // 1024} KB")
    json.dump(catalog, open(os.path.join(BASE, "catalog.json"), "w"), indent=1)
    html = bd.PREVIEW.replace("Bus depot", "Bus stop").replace("cam.position.set(30,32,78)", "cam.position.set(6,5,11)").replace("oc.target.set(30,3,-4)", "oc.target.set(4,1.2,0)").replace("x+=e.lot_m[0]+12", "x+=e.lot_m[0]+3")
    open(os.path.join(BASE, "preview.html"), "w", encoding="utf-8").write(html)


if __name__ == "__main__":
    main()
