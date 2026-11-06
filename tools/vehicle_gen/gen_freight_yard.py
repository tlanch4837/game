"""Intermodal freight terminal -> GLB: low poly geometry, the detail lives in procedural textures.

Frame: y = 0 is the top of the rail; the ground is at y = -0.55 (track formation bottom). Two loading tracks along z:
x = 0 and x = +XT2. Freight cars from gen_freight drop straight in.

  - two rail-mounted gantry cranes (RMG) on runway rails at x = -3.2 / +31: A-frame legs on wheel bogies with cable
    reels, twin box girders with catwalks and handrails, a trolley with machinery house and driver's cab, hoist ropes
    and a spreader lifting a 40 ft box over the train
  - a container stack yard under the cranes (seeded random 20/40 ft boxes, 1-4 high, eight liveries from one atlas)
  - a truck lane with markings, two terminal tractors with chassis, a reach stacker working the stack ends
  - a transload shed with a loading dock beside track 1 (at the far +z end), corrugated walls, roll-up doors, dock bumpers
  - a two-storey site office (stacked cabins with an outside stair), floodlight masts
Textures (one per material, 512-1024 px) carry the plate seams, bolt rows, grime, corrugation, door gear, liveries,
cracked asphalt and hazard striping.
Run:  python gen_freight_yard.py
Output: assets/rail/station/glb/STATION_freight_yard.glb
"""
import math, os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import gen_cars as gc
import gen_transit_tex as tx
import gen_station as st
from gen_station import Mesh, quad, box, post, panel_x, panel_z, _png, _jpg, _noise

GROUND, APRON = -0.55, -0.08          # surrounding ground / yard pavement top
XT2 = 4.5                             # second loading track
RUN0, RUN1 = -56.0, 14.0              # crane runway (z)
XL_, XR_ = -3.2, 31.0                 # crane rails (x)
GH = 19.0                             # girder underside
CL, CW, CH = 12.19, 2.44, 2.59        # 40 ft container
NAMES = ["OCEANIX", "MERIDIAN", "NORDLINE", "KESTREL", "AZURA", "TRITON", "HALCYON", "PELICAN"]
LIVERY = [(176, 40, 32), (24, 70, 140), (40, 110, 70), (210, 120, 20), (90, 90, 96), (200, 200, 196), (110, 40, 80), (20, 120, 150)]
st.SCALE.update({"crane": 2.0, "hazard": 1.0, "grating": 1.0, "asphalt": 8.0, "corrugated": 3.0, "paint": 1.0, "rubber": 1.0,
                 "cabin": 3.0, "rust": 2.0})


# ---------------------------------------------------------------- textures
def _img(a): return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))


def textures():
    M = {}
    rng = np.random.default_rng(77)
    # container atlas: 8 rows of 128 px; cols 0..767 = side wall (12 m), 768..1023 = door end
    W, R = 1024, 128
    atlas = np.zeros((W, W, 3))
    xs = np.arange(768)
    for i, col in enumerate(LIVERY):
        y0 = i * R; c = np.array(col, float)
        side = np.ones((R, 768, 3)) * c
        corr = 0.80 + 0.20 * (0.5 + 0.5 * np.sin(xs / 768 * 12.19 / 0.28 * 2 * math.pi))     # 280 mm corrugation pitch
        side *= corr[None, :, None]
        side[:9] *= 0.55; side[-11:] *= 0.5                                                    # top / bottom rails
        side[:, :10] *= 0.5; side[:, -10:] *= 0.5                                              # corner posts
        grime = _noise(128, 18, 100 + i, 3)[:R, :, None] if False else 0
        side += _noise(768, 10, 100 + i, 2)[:R, :, None]
        streak = np.linspace(0, 1, R)[:, None, None] * rng.uniform(0, 25, (1, 768, 1)) * (rng.random((1, 768, 1)) < 0.03)
        side -= np.repeat(np.repeat(streak, 1, 0), 1, 1)                                         # rust streaks
        im = _img(side); d = ImageDraw.Draw(im)
        txt = (255, 255, 255) if sum(col) < 450 else (30, 30, 30)
        f = tx.font(54); w = d.textlength(NAMES[i], font=f); d.text(((768 - w) / 2, 30), NAMES[i], font=f, fill=txt)
        d.text((560, 14), f"{NAMES[i][:3]}U {rng.integers(100000, 999999)} {rng.integers(0, 9)}", font=tx.font(14), fill=txt)
        d.text((560, 30), "45G1", font=tx.font(14), fill=txt)
        atlas[y0:y0 + R, :768] = np.asarray(im, float)
        # door end: two doors, four lock bars, hinges, cam keepers, plate
        end = np.ones((R, 256, 3)) * c * 0.92 + _noise(256, 8, 200 + i, 1.5)[:R, :, None]
        ex = np.arange(256); end *= (0.85 + 0.15 * (0.5 + 0.5 * np.sin(ex / 256 * 2.44 / 0.16 * 2 * math.pi)))[None, :, None]
        im = _img(end); d = ImageDraw.Draw(im)
        d.rectangle([0, 0, 255, 8], fill=tuple(int(v * 0.5) for v in col)); d.rectangle([0, R - 10, 255, R - 1], fill=tuple(int(v * 0.5) for v in col))
        d.rectangle([0, 0, 8, R], fill=tuple(int(v * 0.5) for v in col)); d.rectangle([247, 0, 255, R], fill=tuple(int(v * 0.5) for v in col))
        d.line([(128, 8), (128, R - 10)], fill=(25, 25, 25), width=3)
        for x in (40, 92, 164, 216):
            d.line([(x, 10), (x, R - 12)], fill=(70, 70, 72), width=4)
            for y in (14, R - 16): d.rectangle([x - 5, y - 4, x + 5, y + 4], fill=(50, 50, 50))
            d.rectangle([x - 3, 58, x + 9, 64], fill=(60, 60, 60))                           # handles
        for y in (22, 52, 82, 108):
            for x in (10, 240): d.rectangle([x, y - 5, x + 6, y + 5], fill=(40, 40, 40))     # hinges
        d.text((150, 18), NAMES[i][:4] + "U", font=tx.font(13), fill=txt); d.rectangle([24, 90, 74, 110], fill=(200, 200, 190))
        atlas[y0:y0 + R, 768:] = np.asarray(im, float)
    M["boxes"] = _img(atlas)
    # crane steel: 2 m tile, yellow plate panels, weld seams, bolt rows, grime toward the bottom
    n = 512; a = np.ones((n, n, 3)) * np.array((228, 170, 30), float) + _noise(n, 7, 1, 2)[..., None]
    a *= (0.92 + 0.08 * np.linspace(1, 0, n))[:, None, None]
    for k in (0, n // 2):
        a[k:k + 3] *= 0.6; a[:, k:k + 3] *= 0.6
        for b in range(8, n, 22):
            a[k + 8:k + 12, b:b + 4] *= 0.55; a[b:b + 4, k + 8:k + 12] *= 0.55
    for _ in range(40):                                                                       # chips and drips
        x, y = rng.integers(0, n, 2); a[y:y + rng.integers(4, 40), x:x + 2] *= 0.75
    M["crane"] = _img(a)
    im = Image.new("RGB", (256, 256), (230, 180, 20)); d = ImageDraw.Draw(im)
    for k in range(-256, 512, 64): d.polygon([(k, 0), (k + 32, 0), (k + 288, 256), (k + 256, 256)], fill=(20, 20, 20))
    M["hazard"] = im
    a = np.full((256, 256, 3), 42.0); g = np.arange(256)
    a[(g % 16) < 3] = 85; a[:, (g % 16) < 3] = 85                                               # grating bars
    M["grating"] = _img(a + _noise(256, 4, 3)[..., None])
    # cab / office windows
    im = Image.new("RGB", (512, 256), (235, 235, 230)); d = ImageDraw.Draw(im)
    for x0 in (24, 268):
        d.rectangle([x0, 30, x0 + 220, 200], fill=(40, 60, 75)); d.rectangle([x0 + 8, 38, x0 + 212, 70], fill=(120, 150, 170))
    M["cabin"] = im
    # asphalt: 8 m tile, aggregate noise, patches, crack lines, oil stains
    n = 1024; a = np.full((n, n, 3), 74.0) + _noise(n, 9, 4, 0.6)[..., None] + _noise(n, 6, 5, 18)[..., None]
    im = _img(a); d = ImageDraw.Draw(im)
    for _ in range(14):
        x, y = rng.integers(0, n, 2); pts = [(x, y)]
        for _k in range(8): x += rng.integers(-40, 40); y += rng.integers(-40, 40); pts.append((x, y))
        d.line(pts, fill=(40, 40, 40), width=2)
    for _ in range(10):
        x, y, r = *rng.integers(0, n, 2), rng.integers(20, 70); d.ellipse([x - r, y - r // 2, x + r, y + r // 2], fill=(66, 66, 67))
    for _ in range(4):
        x, y = rng.integers(0, n - 200, 2); d.rectangle([x, y, x + rng.integers(80, 200), y + rng.integers(60, 160)], fill=(79, 79, 79))
    M["asphalt"] = im.filter(ImageFilter.GaussianBlur(0.6))
    # corrugated cladding (warehouse): 3 m tile, trapezoidal ribs every 0.25 m, rain staining
    n = 512; xx = np.arange(n); rib = ((xx % 43) < 12).astype(float)
    a = np.ones((n, n, 3)) * np.array((168, 176, 180), float) * (0.82 + 0.18 * rib)[None, :, None] + _noise(n, 5, 6, 2)[..., None]
    a *= (1 - 0.18 * np.linspace(0, 1, n) ** 3)[:, None, None]
    M["corrugated"] = _img(a)
    # roll-up door: horizontal slats, bottom bar, number plate
    im = Image.new("RGB", (256, 256), (150, 156, 160)); d = ImageDraw.Draw(im)
    for y in range(0, 256, 10): d.line([(0, y), (255, y)], fill=(110, 116, 120), width=2)
    d.rectangle([0, 0, 255, 14], fill=(60, 60, 64)); d.rectangle([0, 244, 255, 255], fill=(40, 40, 40)); d.rectangle([108, 20, 148, 44], fill=(240, 200, 40))
    M["rollup"] = im
    M["paint"] = Image.new("RGB", (8, 8), (235, 235, 225))
    M["yellowpaint"] = Image.new("RGB", (8, 8), (235, 190, 30))
    M["rubber"] = _img(np.full((64, 64, 3), 26.0) + _noise(64, 4, 7)[..., None])
    a = np.ones((256, 256, 3)) * np.array((120, 60, 34), float) + _noise(256, 16, 8, 1.5)[..., None]
    M["rust"] = _img(a)
    M["flood"] = Image.new("RGB", (8, 8), (255, 248, 220))
    # tractor / reach-stacker cab body (red), with a dark window band
    im = Image.new("RGB", (256, 256), (190, 30, 30)); d = ImageDraw.Draw(im); d.rectangle([0, 40, 255, 130], fill=(35, 45, 55))
    M["cab"] = im
    maps = {}
    for k, v in M.items():
        maps[k] = (_png(v) if k in ("hazard", "boxes", "cabin", "rollup") else _jpg(v), None)
    maps["lamp"] = maps["flood"]
    maps["steel"] = (_jpg(_img(np.full((64, 64, 3), 70.0) + _noise(64, 5, 9)[..., None])), None)
    return maps


# ---------------------------------------------------------------- container with atlas UVs
def container(m, x, y, z, cell, length=CL, rot=0):
    """Box along z (rot=0) or x (rot=1) with the livery row `cell` of the atlas; 20 ft boxes use half the side strip."""
    L2, W2 = length / 2, CW / 2
    v0, v1 = (cell * 128 + 1) / 1024, ((cell + 1) * 128 - 1) / 1024
    us = 767 / 1024 * (length / CL); ue0, ue1 = 769 / 1024, 1023 / 1024
    if rot: (ax0, ax1), (az0, az1) = (x - L2, x + L2), (z - W2, z + W2)
    else: (ax0, ax1), (az0, az1) = (x - W2, x + W2), (z - L2, z + L2)
    y0, y1 = y, y + CH
    S = [(0, v1), (us, v1), (us, v0), (0, v0)]; E = [(ue0, v1), (ue1, v1), (ue1, v0), (ue0, v0)]
    if not rot:
        m.quad("boxes", [(ax1, y0, az1), (ax1, y0, az0), (ax1, y1, az0), (ax1, y1, az1)], S, np.array([1, 0, 0]))
        m.quad("boxes", [(ax0, y0, az0), (ax0, y0, az1), (ax0, y1, az1), (ax0, y1, az0)], S, np.array([-1, 0, 0]))
        m.quad("boxes", [(ax0, y0, az1), (ax1, y0, az1), (ax1, y1, az1), (ax0, y1, az1)], E, np.array([0, 0, 1]))
        m.quad("boxes", [(ax1, y0, az0), (ax0, y0, az0), (ax0, y1, az0), (ax1, y1, az0)], E, np.array([0, 0, -1]))
        m.quad("boxes", [(ax0, y1, az0), (ax0, y1, az1), (ax1, y1, az1), (ax1, y1, az0)], [(0, v0), (us, v0), (us, v0 + 0.01), (0, v0 + 0.01)], np.array([0, 1, 0]))
    else:
        m.quad("boxes", [(ax0, y0, az1), (ax1, y0, az1), (ax1, y1, az1), (ax0, y1, az1)], S, np.array([0, 0, 1]))
        m.quad("boxes", [(ax1, y0, az0), (ax0, y0, az0), (ax0, y1, az0), (ax1, y1, az0)], S, np.array([0, 0, -1]))
        m.quad("boxes", [(ax1, y0, az1), (ax1, y0, az0), (ax1, y1, az0), (ax1, y1, az1)], E, np.array([1, 0, 0]))
        m.quad("boxes", [(ax0, y0, az0), (ax0, y0, az1), (ax0, y1, az1), (ax0, y1, az0)], E, np.array([-1, 0, 0]))
        m.quad("boxes", [(ax0, y1, az0), (ax1, y1, az0), (ax1, y1, az1), (ax0, y1, az1)], [(0, v0), (us, v0), (us, v0 + 0.01), (0, v0 + 0.01)], np.array([0, 1, 0]))


# ---------------------------------------------------------------- yard surface
def pavement(m):
    def slab(x0, x1, z0, z1):
        box(m, "asphalt", x0, x1, GROUND, APRON, z0, z1)
    slab(XT2 + 2.3, 40.0, -62.0, 22.0)                       # stack yard + truck lane (+x of track 2)
    slab(-14.0, -2.3, -62.0, 60.0)                            # -x side (crane rail, shed apron)
    slab(XT2 + 2.3, 40.0, 22.0, 60.0)
    slab(-2.3 + 0.0, 2.3 - 0.0, 60.0, 60.0)                   # (no-op placeholder keeps the tracks clear)
    for x in (XL_, XR_):                                      # crane runway rails with a hazard strip
        box(m, "steel", x - 0.06, x + 0.06, APRON, APRON + 0.12, RUN0 - 4, RUN1 + 4)
        for s in (-1, 1):
            quad(m, "hazard", [(x + s * 0.3, APRON + 0.004, RUN0 - 4), (x + s * 0.6, APRON + 0.004, RUN0 - 4), (x + s * 0.6, APRON + 0.004, RUN1 + 4), (x + s * 0.3, APRON + 0.004, RUN1 + 4)], (0, 1, 0))
        for z in (RUN0 - 4.5, RUN1 + 4.5):                    # end stops
            box(m, "hazard", x - 0.6, x + 0.6, APRON, APRON + 1.2, z - 0.4, z + 0.4)
    for x in (8.0, 11.2):                                     # truck lane lines
        for z in np.arange(-60.0, 20.0, 6.0):
            quad(m, "paint", [(x - 0.08, APRON + 0.004, z), (x + 0.08, APRON + 0.004, z), (x + 0.08, APRON + 0.004, z + 3.0), (x - 0.08, APRON + 0.004, z + 3.0)], (0, 1, 0))
    for z in np.arange(-58.0, 14.0, 12.8):                    # stack bay ground marks
        quad(m, "yellowpaint", [(12.0, APRON + 0.004, z - 0.06), (30.0, APRON + 0.004, z - 0.06), (30.0, APRON + 0.004, z + 0.06), (12.0, APRON + 0.004, z + 0.06)], (0, 1, 0))


def stacks(m, rng):
    rows = [12.6 + k * (CW + 0.35) for k in range(6)]
    for x in rows:
        z = -58.0 + 0.3
        while z < 12.0:
            forty = rng.random() < 0.7
            L = CL if forty else 6.06
            if z + L > 13.0: break
            h = int(rng.choice([0, 1, 2, 3, 4], p=[0.08, 0.17, 0.3, 0.3, 0.15]))
            for k in range(h):
                container(m, x, APRON + k * CH, z + L / 2, int(rng.integers(0, 8)), L)
            z += L + 0.4


# ---------------------------------------------------------------- gantry crane
def gantry(m, zc, xt, hook_y, load_cell):
    """Rail-mounted gantry crane: legs at XL_/XR_ (+-6 m along z), twin box girders, trolley, cab, spreader."""
    top = GH
    for x in (XL_, XR_):
        # sill beam with wheel bogies and hazard ends
        box(m, "crane", x - 0.7, x + 0.7, APRON + 0.9, APRON + 2.0, zc - 8.0, zc + 8.0)
        for z in (zc - 7.2, zc - 5.0, zc + 5.0, zc + 7.2):
            box(m, "steel", x - 0.5, x + 0.5, APRON + 0.12, APRON + 0.9, z - 0.8, z + 0.8)
            gc.tube(m, "steel", (x - 0.55, APRON + 0.48, z), (x + 0.55, APRON + 0.48, z), 0.36, (0.5, 0.5), 10)
        for z in (zc - 8.0, zc + 8.0): st.panel_z(m, "hazard", z + (0.005 if z > zc else -0.005), 1 if z > zc else -1, x - 0.7, x + 0.7, APRON + 0.9, APRON + 2.0)
        # A-frame: two legs per side leaning in toward the portal beam
        for zl in (zc - 6.0, zc + 6.0):
            zt = zc + (zl - zc) * 0.55
            for dx in (-0.45, 0.45):
                P0, P1 = (x + dx, APRON + 2.0, zl), (x + dx, top, zt)
                quad(m, "crane", [(P0[0], P0[1], P0[2] - 0.45), (P0[0], P0[1], P0[2] + 0.45), (P1[0], P1[1], P1[2] + 0.45), (P1[0], P1[1], P1[2] - 0.45)], (np.sign(dx), 0, 0))
            for dz in (-0.45, 0.45):
                quad(m, "crane", [(x - 0.45, APRON + 2.0, zl + dz), (x + 0.45, APRON + 2.0, zl + dz), (x + 0.45, top, zt + dz), (x - 0.45, top, zt + dz)], (0, 0.2, np.sign(dz)))
        box(m, "crane", x - 0.5, x + 0.5, top - 1.2, top, zc - 4.0, zc + 4.0)                      # portal beam
        box(m, "crane", x - 0.35, x + 0.35, APRON + 7.5, APRON + 8.3, zc - 4.6, zc + 4.6)          # tie beam
        # ladder + rest platforms on the outer leg
        lx = x + (0.47 if x > 0 else -0.47)
        for y in np.arange(APRON + 2.4, top - 1.2, 0.3): box(m, "steel", lx - 0.02, lx + 0.02, y, y + 0.03, zc + 6.0 * 0.8 - 0.25, zc + 6.0 * 0.8 + 0.25)
        for y in (APRON + 8.3, APRON + 14.0):
            box(m, "grating", x - 1.2, x + 1.2, y - 0.05, y, zc + 3.3, zc + 5.6, bottom=True)
            box(m, "yellowpaint", x - 1.2, x + 1.2, y + 1.0, y + 1.05, zc + 5.55, zc + 5.6)
    # cable reel on the +x leg
    gc.tube(m, "steel", (XR_ + 0.75, APRON + 3.5, zc - 2.0), (XR_ + 1.35, APRON + 3.5, zc - 2.0), 1.6, (0.5, 0.5), 16)
    gc.tube(m, "rubber", (XR_ + 0.80, APRON + 3.5, zc - 2.0), (XR_ + 1.30, APRON + 3.5, zc - 2.0), 1.3, (0.5, 0.5), 16)
    # twin box girders, cantilevered past the legs, with catwalks + handrails
    gx0, gx1 = XL_ - 6.0, XR_ + 6.0
    for s in (-1, 1):
        zg = zc + s * 2.0
        box(m, "crane", gx0, gx1, top, top + 2.2, zg - 0.5, zg + 0.5, bottom=True)
        box(m, "steel", gx0, gx1, top + 2.2, top + 2.32, zg - 0.08, zg + 0.08)                       # trolley rail
        zw = zc + s * 2.95
        box(m, "grating", gx0, gx1, top + 1.0, top + 1.05, zw - 0.45, zw + 0.45, bottom=True)          # outer catwalk
        box(m, "yellowpaint", gx0, gx1, top + 2.05, top + 2.1, zw + s * 0.42 - 0.02, zw + s * 0.42 + 0.02)
        box(m, "yellowpaint", gx0, gx1, top + 1.55, top + 1.58, zw + s * 0.42 - 0.015, zw + s * 0.42 + 0.015)
        for x in np.arange(gx0, gx1 + 0.1, 2.5): box(m, "yellowpaint", x - 0.02, x + 0.02, top + 1.05, top + 2.1, zw + s * 0.42 - 0.02, zw + s * 0.42 + 0.02)
        for x in np.arange(gx0 + 1.0, gx1, 4.0): box(m, "crane", x - 0.08, x + 0.08, top + 0.3, top + 1.0, zg + s * 0.5, zw - s * 0.2)   # brackets
    for x in (gx0, gx1): box(m, "crane", x - 0.3, x + 0.3, top, top + 2.2, zc - 1.5, zc + 1.5)        # end ties
    for x in np.arange(gx0 + 4.0, gx1, 8.0): box(m, "crane", x - 0.2, x + 0.2, top + 1.6, top + 2.1, zc - 1.5, zc + 1.5)
    # trolley: frame, machinery house, hoist drums, cab hanging below on the +z side
    ty = top + 2.32
    box(m, "crane", xt - 2.6, xt + 2.6, ty, ty + 0.8, zc - 2.6, zc + 2.6, bottom=True)
    box(m, "corrugated", xt - 2.2, xt + 1.4, ty + 0.8, ty + 3.2, zc - 2.2, zc + 2.2)
    box(m, "crane", xt - 2.35, xt + 1.55, ty + 3.2, ty + 3.35, zc - 2.35, zc + 2.35)
    st.panel_x(m, "rollup", xt + 1.42, 1, zc - 0.8, zc + 0.8, ty + 0.85, ty + 2.6)
    gc.tube(m, "steel", (xt + 1.7, ty + 1.2, zc - 1.8), (xt + 1.7, ty + 1.2, zc + 1.8), 0.55, (0.5, 0.5), 12)   # hoist drum
    for s in (-1, 1):
        for x in (xt - 2.0, xt + 2.0): box(m, "steel", x - 0.35, x + 0.35, ty - 0.25, ty, zc + s * 2.0 - 0.25, zc + s * 2.0 + 0.25)  # wheels
    cz = zc + 1.0
    box(m, "crane", xt - 0.3, xt + 0.3, top - 1.0, top, cz + 1.4, cz + 1.9)                           # cab hanger
    box(m, "paint", xt - 1.2, xt + 1.2, top - 3.4, top - 1.0, cz + 1.0, cz + 3.0, bottom=True)         # cab
    for s, x in ((1, xt + 1.21), (-1, xt - 1.21)): st.panel_x(m, "cabin", x, s, cz + 1.05, cz + 2.95, top - 3.3, top - 1.2)
    st.panel_z(m, "cabin", cz + 3.01, 1, xt - 1.15, xt + 1.15, top - 3.3, top - 1.2)
    # hoist ropes, headblock, spreader, load
    sy = hook_y + CH
    for dx in (-1.2, 1.2):
        for dz in (-0.5, 0.5):
            gc.tube(m, "steel", (xt + dx * 0.6, ty, zc + dz), (xt + dx, sy + 1.0, zc + dz * 2), 0.025, (0.5, 0.5), 4, caps=False)
    box(m, "crane", xt - 1.4, xt + 1.4, sy + 0.6, sy + 1.1, zc - 1.2, zc + 1.2, bottom=True)          # headblock
    box(m, "crane", xt - CW / 2 - 0.05, xt + CW / 2 + 0.05, sy, sy + 0.45, zc - CL / 2, zc + CL / 2, bottom=True)  # spreader beam
    for dz in (-CL / 2 + 0.3, CL / 2 - 0.3): box(m, "hazard", xt - CW / 2 - 0.08, xt + CW / 2 + 0.08, sy + 0.1, sy + 0.5, dz + zc - 0.25, dz + zc + 0.25)
    for dx in (-1.0, 1.0):
        for dz in (-CL / 2 + 0.2, CL / 2 - 0.2): box(m, "steel", xt + dx - 0.1, xt + dx + 0.1, sy - 0.25, sy, zc + dz - 0.1, zc + dz + 0.1)  # flippers
    container(m, xt, hook_y, zc, load_cell)
    for x in (gx0 + 0.3, gx1 - 0.3): box(m, "flood", x - 0.3, x + 0.3, top - 0.3, top, zc - 0.8, zc + 0.8, bottom=True)  # boom lights


# ---------------------------------------------------------------- vehicles
def tractor(m, x, z, cell=None, d=1):
    """Terminal tractor + skeletal chassis (+ optional 40 ft box), pointing +z (d=1) or -z."""
    zc = z
    box(m, "cab", x - 1.25, x + 1.25, 0.55 + APRON, 2.9 + APRON, zc + d * 4.2 - 1.0, zc + d * 4.2 + 1.0)     # cab
    box(m, "steel", x - 1.25, x + 1.25, 0.5 + APRON, 1.1 + APRON, zc + d * 2.5, zc + d * 5.6)              # tractor frame
    for wz in (zc + d * 4.8, zc + d * 2.9, zc - d * 3.8, zc - d * 5.0):
        for s in (-1, 1): gc.tube(m, "rubber", (x + s * 0.95, 0.5 + APRON, wz), (x + s * 1.3, 0.5 + APRON, wz), 0.5, (0.5, 0.5), 10)
    box(m, "steel", x - 0.5, x + 0.5, 0.9 + APRON, 1.25 + APRON, zc - 6.2, zc + 3.2)                     # chassis spine
    for zz in (zc - 6.0, zc + 3.0): box(m, "steel", x - 1.2, x + 1.2, 1.0 + APRON, 1.3 + APRON, zz - 0.15, zz + 0.15)
    if cell is not None: container(m, x, 1.3 + APRON, zc - 1.5, cell)


def reach_stacker(m, x, z, cell):
    """Reach stacker pointing -x at a stack, boom raised with a box on the spreader."""
    box(m, "yellowpaint", x - 3.0, x + 4.5, APRON + 0.8, APRON + 2.2, z - 1.9, z + 1.9)                  # chassis
    for wx in (x - 2.2, x + 3.4):
        for s in (-1, 1): gc.tube(m, "rubber", (wx, APRON + 0.85, z + s * 1.6), (wx, APRON + 0.85, z + s * 2.1), 0.85, (0.5, 0.5), 12)
    box(m, "cab", x + 0.2, x + 2.0, APRON + 2.2, APRON + 4.4, z + 0.4, z + 1.9)                         # cab (offset)
    box(m, "rust", x + 3.0, x + 4.7, APRON + 2.2, APRON + 3.6, z - 1.6, z + 1.6)                        # counterweight
    bx0, by0, bx1, by1 = x + 2.6, APRON + 2.6, x - 5.4, APRON + 11.5
    for s in (-1, 1):
        zz = z + s * 0.45
        quad(m, "yellowpaint", [(bx0, by0, zz), (bx1, by1, zz), (bx1, by1 + 0.9, zz), (bx0, by0 + 0.9, zz)], (0, 0, s))
    quad(m, "yellowpaint", [(bx0, by0 + 0.9, z - 0.45), (bx1, by1 + 0.9, z - 0.45), (bx1, by1 + 0.9, z + 0.45), (bx0, by0 + 0.9, z + 0.45)], (0.7, 0.7, 0))
    quad(m, "yellowpaint", [(bx0, by0, z - 0.45), (bx0, by0, z + 0.45), (bx1, by1, z + 0.45), (bx1, by1, z - 0.45)], (-0.7, -0.7, 0))
    gc.tube(m, "steel", (x + 0.0, APRON + 2.2, z), (x - 1.8, APRON + 6.4, z), 0.22, (0.5, 0.5), 8)     # lift cylinder
    sy = by1 - 2.2 - CH
    box(m, "yellowpaint", bx1 - CL / 2 + 0.2, bx1 + CL / 2 - 0.2, sy + CH, sy + CH + 0.4, z - 1.0, z + 1.0)  # spreader (along x)
    box(m, "steel", bx1 - 0.3, bx1 + 0.3, sy + CH + 0.4, by1, z - 0.3, z + 0.3)
    container(m, bx1, sy, z, cell, rot=1)


# ---------------------------------------------------------------- buildings
def shed(m):
    """Transload shed beside track 1 at the +z end: dock platform at car-floor height, corrugated walls, roll-up doors."""
    x0, x1, z0, z1, H = -24.0, -6.4, 22.0, 58.0, 9.0
    dock = 1.15
    box(m, "white" if False else "paint", -6.4, -3.95, GROUND, dock, z0, z1)                             # dock edge slab
    box(m, "corrugated", x0, x1, GROUND, H, z0, z1)
    for s in (-1, 1):                                                                                     # low-pitch roof
        zr = z0 if s < 0 else z1
    quad(m, "rust", [(x0 - 0.5, H, z0 - 0.5), (x1 + 2.6, H - 0.4, z0 - 0.5), (x1 + 2.6, H - 0.4, z1 + 0.5), (x0 - 0.5, H, z1 + 0.5)], (0, 1, 0))
    quad(m, "rust", [(x0 - 0.5, H - 0.1, z0 - 0.5), (x1 + 2.6, H - 0.5, z0 - 0.5), (x1 + 2.6, H - 0.5, z1 + 0.5), (x0 - 0.5, H - 0.1, z1 + 0.5)], (0, -1, 0))
    for z in np.arange(z0 + 3.5, z1 - 2.0, 6.0):                                                          # rail-side doors + bumpers
        st.panel_x(m, "rollup", x1 + 0.02, 1, z - 1.6, z + 1.6, dock, dock + 3.6)
        for dz in (-1.9, 1.9): box(m, "rubber", -4.05, -3.95, dock - 0.6, dock - 0.05, z + dz - 0.2, z + dz + 0.2)
        box(m, "hazard", -3.96, -3.94, dock - 0.04, dock, z - 1.6, z + 1.6)
    for z in np.arange(z0 + 4.0, z1 - 2.0, 7.0):                                                          # truck doors on the far side
        st.panel_x(m, "rollup", x0 - 0.02, -1, z - 1.8, z + 1.8, GROUND, GROUND + 4.2)
    for z in np.arange(z0 + 1.0, z1, 4.0): box(m, "steel", x1 + 2.3, x1 + 2.42, dock, H - 0.4, z - 0.06, z + 0.06)  # canopy posts
    st.panel_z(m, "cabin", z0 - 0.02, -1, x0 + 2.0, x0 + 8.0, 4.0, 6.5)


def office(m, x, z):
    """Two stacked site cabins with an outside stair and landing."""
    for k in range(2):
        y = APRON + k * 2.9
        box(m, "paint", x - 1.5, x + 1.5, y, y + 2.8, z - 4.5, z + 4.5)
        st.panel_x(m, "cabin", x - 1.52, -1, z - 3.8, z + 3.8, y + 0.6, y + 2.4)
        box(m, "rust", x - 1.55, x + 1.55, y + 2.8, y + 2.9, z - 4.55, z + 4.55)
    box(m, "grating", x + 1.5, x + 2.8, APRON + 2.8, APRON + 2.9, z - 4.5, z - 1.0, bottom=True)
    box(m, "yellowpaint", x + 2.75, x + 2.8, APRON + 2.9, APRON + 3.95, z - 4.5, z - 1.0)
    for k in range(16):
        y = APRON + 2.8 * k / 16
        box(m, "grating", x + 1.6, x + 2.8, y, y + 0.05, z - 1.0 + 0.25 * k, z - 0.75 + 0.25 * k, bottom=True)


def mast(m, x, z):
    gc.tube(m, "steel", (x, APRON, z), (x, APRON + 26.0, z), 0.35, (0.5, 0.5), 8, rb=0.18)
    box(m, "steel", x - 1.6, x + 1.6, APRON + 26.0, APRON + 26.2, z - 0.6, z + 0.6)
    for dx in (-1.2, -0.4, 0.4, 1.2):
        box(m, "steel", x + dx - 0.3, x + dx + 0.3, APRON + 26.2, APRON + 26.9, z - 0.35, z + 0.35)
        quad(m, "flood", [(x + dx - 0.25, APRON + 26.25, z - 0.36), (x + dx + 0.25, APRON + 26.25, z - 0.36), (x + dx + 0.25, APRON + 26.85, z - 0.36), (x + dx - 0.25, APRON + 26.85, z - 0.36)], (0, -0.3, -1))


def build():
    m = Mesh(); rng = np.random.default_rng(2026)
    pavement(m)
    stacks(m, rng)
    gantry(m, -38.0, 0.0, 6.2, 3)           # crane 1 lifting a box off / onto track 1
    gantry(m, -4.0, 18.0, 12.0, 5)          # crane 2 parked over the stack, box raised
    tractor(m, 9.6, -20.0, 1, 1); tractor(m, 9.6, 2.0, None, -1)
    reach_stacker(m, 34.5, -12.0, 6)
    shed(m)
    office(m, 36.0, 30.0)
    for x, z in ((-8.0, -60.0), (-8.0, 0.0), (36.0, -40.0), (36.0, 8.0), (36.0, 48.0)): mast(m, x, z)
    return m


def main():
    os.makedirs(st.OUT, exist_ok=True)
    maps = textures()
    gc.MAT_PBR.update({"boxes": (0.35, 0.55), "crane": (0.3, 0.5), "hazard": (0.1, 0.6), "grating": (0.6, 0.5), "asphalt": (0, 0.95),
                       "corrugated": (0.6, 0.45), "rollup": (0.5, 0.5), "paint": (0, 0.7), "yellowpaint": (0.1, 0.5), "rubber": (0, 0.9),
                       "rust": (0.4, 0.7), "flood": (0, 0.3), "lamp": (0, 0.3), "cab": (0.2, 0.4), "cabin": (0.1, 0.3), "steel": (0.7, 0.4)})
    m = build()
    out = os.path.join(st.OUT, "STATION_freight_yard.glb")
    gc.write_glb(out, m, maps, (1, 1, 1))
    print(f"freight_yard: {m.count()} tris, {os.path.getsize(out) / 1e6:.2f} MB -> {out}")


if __name__ == "__main__":
    main()
