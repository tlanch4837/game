"""Procedural textures + height maps for the passenger train (gen_train.py), drawn with gen_transit_tex's Tex helper.

Same scheme as the buses: every material is an albedo plus a normal map baked from a drawn height map; the engine turns
the normal map back into height and runs parallax occlusion on it, so seams, louvres, springs, bolts and lamp recesses are
relief in the texture, not triangles.

Atlases (u, v with v down):
  body    1024^2, one 3 m x 4.4 m panel tile: vertical panel joints, skirt line, belt groove, cantrail gutter; flat top band
  door    u 0..0.33 coach plug door (2 leaves) | 0.34..0.54 loco cab door | 0.56..1 coach end wall with gangway opening
  front   v 0..0.60 loco nose fascia (buffer beam, lamps, number) | 0.62..0.80 top headlamp pod | 0.82..1 snow plough
  grille  u 0..0.5 side louvre / filter panel | u 0.5..1: v 0..0.5 roof fan, v 0.5..1 equipment box door
  bogie   v 0..0.5 bogie side frame | v 0.5..1: u 0..0.5 rail wheel face, u 0.5..0.95 underframe box; dark corner at (0.98, 0.98)

Output: assets/vehicles/train/tex/<mat>_albedo.jpg, _normal.png, _height.png
"""
import math, os
import numpy as np
from PIL import Image, ImageDraw
import gen_cars as gc
import gen_transit_tex as tx
from gen_transit_tex import Tex, font, noise

TEX = os.path.join(gc.ROOT, "assets", "vehicles", "train", "tex")
BODY_H, BODY_PITCH, FLAT = tx.BODY_H, tx.BODY_PITCH, tx.FLAT

# atlas regions (u0, v0, u1, v1)
COACH_DOOR, CAB_DOOR, GANGWAY = (0.004, 0.004, 0.326, 0.996), (0.344, 0.004, 0.536, 0.996), (0.564, 0.004, 0.996, 0.996)
NOSE, HEADPOD, PLOUGH = (0, 0.004, 1, 0.596), (0.25, 0.624, 0.75, 0.796), (0, 0.824, 1, 0.996)
LOUVRE, FAN, EQUIP = (0.004, 0.004, 0.496, 0.996), (0.504, 0.004, 0.996, 0.496), (0.504, 0.504, 0.996, 0.996)
FRAME, RWHEEL, UNDERBOX = (0.0, 0.004, 1.0, 0.496), (0.0, 0.5, 0.5, 1.0), (0.504, 0.504, 0.946, 0.946)
BDARK = (0.982, 0.982)
FRAME_WB = 0.65  # axles sit at u = 0.5 +- FRAME_WB / 2 of the frame: frame length = wheelbase / FRAME_WB


def _noisy(t, amp, seed, scale=48):
    a = np.asarray(t.img, np.float32) + noise(t.w, t.h, scale, seed, amp)[..., None]
    t.img = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)); t.d = ImageDraw.Draw(t.img)


def bolt(t, x, y, r=5, col=(150, 150, 150)):
    t.ell((x - r, y - r, x + r, y + r), col, 0.85); t.ell((x - r * 0.4, y - r * 0.6, x + r * 0.2, y - r * 0.1), (210, 210, 210))


# ---------------------------------------------------------------- body
def body(store):
    """Near-white carrier, tinted by baseColorFactor. Bodyside of a steel / aluminium extrusion carbody."""
    S = 1024; t = Tex(S, S, (236, 236, 236), 0.6)
    py = lambda y: (1 - y / BODY_H) * S
    t.rect((0, py(1.22), S, py(1.20)), (200, 200, 200), 0.30)          # solebar / skirt joint
    t.rect((0, py(1.62), S, py(1.585)), (212, 212, 212), 0.38)         # belt groove under the windows
    t.rect((0, py(3.42), S, py(3.38)), (205, 205, 205), 0.30)          # cantrail gutter
    t.rect((0, py(3.48), S, py(3.42)), (226, 226, 226), 0.70)          # gutter lip (raised)
    for x0 in (0, S - 3):                                              # vertical panel joint at the tile seam
        t.rect((x0, py(3.38), x0 + 3, py(1.0)), (196, 196, 196), 0.28)
    for y in (1.05, 1.15):                                             # rivet line on the solebar
        for k in range(24):
            x = (k + 0.5) / 24 * S; t.ell((x - 3, py(y) - 3, x + 3, py(y) + 3), (224, 224, 224), 0.78)
    # fluted stainless sheathing (North American coaches): horizontal ribs, each a ramp lit on top and shadowed below
    for y0, y1 in ((1.25, 1.55), (2.90, 3.30)):
        y = y0
        while y < y1:
            t.rect((0, py(y + 0.030), S, py(y + 0.012)), (246, 246, 246), 0.78)
            t.rect((0, py(y + 0.012), S, py(y)), (206, 206, 206), 0.42)
            y += 0.045
    _noisy(t, 5, 21, 64)
    t.rect((0, 0, S, py(4.25)), (238, 238, 238), 0.6)                  # flat top band (roofs, bevels, trim)
    return t.bake("body", k=5.0, blur=1.4, store=store)


# ---------------------------------------------------------------- doors and end walls
def _glass(t, b, rng):
    x0, y0, x1, y1 = [int(v) for v in b]
    t.rect(b, (14, 16, 20), 0.30, 10)
    arr = np.asarray(t.img).copy(); h, w = y1 - y0, x1 - x0
    g = np.linspace(0, 1, h)[:, None] * np.ones((1, w))
    refl = (38 + 30 * (1 - g) + 24 * np.clip(1 - np.abs(np.linspace(-1, 1, w)[None, :] * 1.6 + g - 0.4), 0, 1))
    arr[y0:y1, x0:x1] = np.clip(arr[y0:y1, x0:x1] * 0.5 + refl[..., None] * np.array([0.85, 0.95, 1.1]), 0, 255)
    t.img = Image.fromarray(arr.astype(np.uint8)); t.d = ImageDraw.Draw(t.img)
    t.rect(b, outline=(28, 28, 30), width=8, r=10)


def door(store):
    S = 1024; t = Tex(S, S, (70, 72, 76), 0.6); rng = np.random.default_rng(7)
    # coach plug door, 2 leaves (1.30 m x 2.05 m)
    X0, X1 = 4, 334
    t.rect((X0, 4, X1, S - 4), (40, 40, 42), 0.30)                     # door aperture shadow
    for a, b in ((X0 + 8, (X0 + X1) // 2 - 3), ((X0 + X1) // 2 + 3, X1 - 8)):
        t.rect((a, 14, b, S - 40), (190, 192, 196), 0.75, 8)           # leaf (light grey, not livery tinted)
        _glass(t, (a + 16, 40, b - 16, int(S * 0.52)), rng)
        t.rect((a + 16, int(S * 0.56), b - 16, int(S * 0.58)), (160, 160, 165), 0.55)   # leaf panel line
        t.rect((a + 16, int(S * 0.62), b - 16, S - 70), (182, 184, 188), 0.68, 6)       # pressed lower panel
    t.rect(((X0 + X1) // 2 - 3, 10, (X0 + X1) // 2 + 3, S - 36), (12, 12, 12), 0.25)   # centre seal
    t.ell((X1 - 58, int(S * 0.55), X1 - 26, int(S * 0.55) + 32), (40, 170, 70), 0.95)   # open button
    t.ell((X1 - 52, int(S * 0.55) + 6, X1 - 32, int(S * 0.55) + 26), (120, 240, 140), 0.9)
    t.rect((X0, S - 34, X1, S - 4), (215, 175, 30), 0.45)              # yellow step edge
    for k in range(10): t.rect((X0 + 8 + k * 33, S - 30, X0 + 22 + k * 33, S - 8), (60, 50, 20), 0.3)  # tread slots
    # loco cab door (0.75 m x 2.10 m)
    X0, X1 = 352, 549
    t.rect((X0, 4, X1, S - 4), (36, 36, 38), 0.28)
    t.rect((X0 + 8, 12, X1 - 8, S - 12), (205, 205, 205), 0.75, 6)
    _glass(t, (X0 + 26, 50, X1 - 26, int(S * 0.36)), rng)
    t.rect((X1 - 46, int(S * 0.50), X1 - 22, int(S * 0.50) + 70), (25, 25, 25), 0.35, 4)  # handle recess
    t.rect((X1 - 42, int(S * 0.50) + 14, X1 - 26, int(S * 0.50) + 56), (170, 170, 170), 0.9, 3)
    for y in (0.40, 0.62, 0.84): t.rect((X0 + 8, int(S * y), X0 + 20, int(S * y) + 40), (120, 120, 120), 0.95, 2)  # hinges
    for k in range(3): t.rect((X0 + 30, S - 60 - k * 90, X1 - 30, S - 44 - k * 90), (60, 60, 60), 0.4)  # kick plate ribs
    # coach end wall + gangway (u 0.564..0.996 -> full end of the carbody, v 0 = roof, 1 = solebar)
    X0, X1 = 578, 1020; W = X1 - X0
    t.rect((X0, 4, X1, S - 4), (58, 60, 64), 0.6)
    for k in range(6): t.rect((X0, 60 + k * 160, X1, 64 + k * 160), (48, 48, 52), 0.45)  # end-wall stiffener lines
    gx0, gx1, gy0, gy1 = X0 + W * 0.30, X0 + W * 0.70, S * 0.20, S * 0.86
    t.rect((gx0 - 22, gy0 - 22, gx1 + 22, gy1 + 10), (22, 22, 22), 0.85, 18)    # rubber gangway surround (raised)
    t.rect((gx0, gy0, gx1, gy1), (12, 12, 14), 0.10, 10)                      # opening
    t.rect((gx0 + 18, gy0 + 30, gx1 - 18, gy1 - 30), (110, 112, 118), 0.25, 6) # inner gangway door
    _glass(t, (gx0 + 40, gy0 + 60, gx1 - 40, gy0 + 260), rng)
    for x in (X0 + W * 0.10, X0 + W * 0.90):                                  # tail + marker lamps
        t.ell((x - 26, S * 0.76, x + 26, S * 0.76 + 52), (20, 20, 20), 0.30); t.ell((x - 18, S * 0.76 + 8, x + 18, S * 0.76 + 44), (190, 20, 20), 0.55)
        t.ell((x - 18, S * 0.12, x + 18, S * 0.12 + 36), (20, 20, 20), 0.30); t.ell((x - 12, S * 0.12 + 6, x + 12, S * 0.12 + 30), (235, 235, 220), 0.55)
    for x in (X0 + W * 0.20, X0 + W * 0.80):                                  # vertical grab rails
        t.rect((x - 6, S * 0.35, x + 6, S * 0.80), (200, 200, 200), 0.95, 5)
    for x in (X0 + W * 0.04, X0 + W * 0.96 - 40):                              # UIC cable sockets
        t.rect((x, S * 0.90, x + 40, S * 0.95), (30, 30, 30), 0.7, 6); t.ell((x + 10, S * 0.91, x + 30, S * 0.94), (90, 90, 90), 0.5)
    _noisy(t, 4, 22)
    return t.bake("door", k=6.0, blur=1.3, store=store)


# ---------------------------------------------------------------- loco front
def lamp(t, cx, cy, r, col, hv=0.55):
    t.ell((cx - r - 8, cy - r - 8, cx + r + 8, cy + r + 8), (18, 18, 20), 0.25)
    t.ell((cx - r, cy - r, cx + r, cy + r), col, hv)
    t.ell((cx - r * 0.45, cy - r * 0.45, cx + r * 0.45, cy + r * 0.45), tuple(min(255, c + 30) for c in col), hv - 0.15)


def front(store):
    S = 1024; t = Tex(S, S, (34, 34, 36), 0.5)
    # nose fascia: buffer beam band at the bottom, lamp clusters at the corners, number in the middle
    y0, y1 = 4, int(S * 0.596); H = y1 - y0
    t.rect((0, y0, S, y1), (44, 44, 46), 0.6)
    t.rect((0, y0 + H * 0.62, S, y1), (150, 25, 22), 0.66)                      # red buffer beam
    t.rect((0, y0 + H * 0.60, S, y0 + H * 0.62), (20, 20, 20), 0.35)
    for cx in (130, S - 130):                                                   # lamp clusters
        t.rect((cx - 110, y0 + H * 0.12, cx + 110, y0 + H * 0.50), (16, 16, 18), 0.30, 30)
        lamp(t, cx - 45, y0 + H * 0.31, 38, (230, 232, 240))                     # headlamp
        lamp(t, cx + 55, y0 + H * 0.31, 26, (170, 18, 18))                       # tail lamp
    t.rect((S / 2 - 150, y0 + H * 0.20, S / 2 + 150, y0 + H * 0.42), (230, 230, 225), 0.7, 8)  # number plate
    t.d.text((S / 2 - 120, y0 + H * 0.205), "4012", font=font(96), fill=(25, 25, 25))
    for x in (S / 2 - 260, S / 2 + 200):                                        # horn grilles
        for k in range(5): t.rect((x, y0 + H * 0.18 + k * 14, x + 60, y0 + H * 0.18 + k * 14 + 7), (12, 12, 12), 0.25, 3)
    for x in (S * 0.30, S * 0.42, S * 0.58, S * 0.70):                          # brake / main reservoir hoses
        t.rect((x - 9, y0 + H * 0.66, x + 9, y0 + H * 0.96), (25, 25, 25), 0.8, 8)
        t.rect((x - 14, y0 + H * 0.66, x + 14, y0 + H * 0.70), [(200, 30, 30), (230, 200, 30)][int(x) % 2], 0.9, 4)
    for x in range(40, S, 120): bolt(t, x, y0 + H * 0.97 - 6, 6, (110, 30, 26))
    # top headlamp pod
    y0, y1 = int(S * 0.624), int(S * 0.796)
    t.rect((S * 0.25, y0, S * 0.75, y1), (20, 20, 22), 0.4, 30)
    lamp(t, S / 2, (y0 + y1) / 2, 52, (235, 236, 242), 0.6)
    for x in (S * 0.32, S * 0.68): lamp(t, x, (y0 + y1) / 2, 22, (240, 230, 200), 0.6)
    # plough: angled slats
    y0, y1 = int(S * 0.824), S - 4
    t.rect((0, y0, S, y1), (30, 30, 30), 0.5)
    for k in range(9):
        x = 40 + k * 110; t.rect((x, y0 + 8, x + 70, y1 - 8), (70, 70, 72), 0.85, 6)
    _noisy(t, 4, 23)
    return t.bake("front", k=6.0, blur=1.2, store=store)


# ---------------------------------------------------------------- grilles, roof fan, equipment doors
def grille(store):
    S = 1024; t = Tex(S, S, (60, 62, 66), 0.6)
    # side louvre panel: raised frame, chevron slats over a filter mesh
    X0, X1 = 4, 508
    t.rect((X0, 4, X1, S - 4), (150, 152, 156), 0.85, 6)
    t.rect((X0 + 22, 22, X1 - 22, S - 22), (36, 38, 40), 0.35, 4)
    for k in range(32):
        y = 30 + k * 30; t.rect((X0 + 26, y, X1 - 26, y + 16), (115, 118, 122), 0.80); t.rect((X0 + 26, y + 16, X1 - 26, y + 24), (12, 12, 12), 0.15)
    for x, y in ((X0 + 12, 12), (X1 - 12, 12), (X0 + 12, S - 12), (X1 - 12, S - 12), (X0 + 12, S / 2), (X1 - 12, S / 2)): bolt(t, x, y, 6)
    # roof fan
    cx, cy, r = 768, 256, 220
    t.rect((516, 12, S - 12, 500), (90, 92, 96), 0.75, 10)
    t.ell((cx - r - 14, cy - r - 14, cx + r + 14, cy + r + 14), (40, 40, 42), 0.5)
    t.ell((cx - r, cy - r, cx + r, cy + r), (10, 10, 10), 0.10)
    for k in range(12):                                                          # mesh guard rings + blades
        a = 2 * math.pi * k / 12; t.d.line([cx, cy, cx + r * math.cos(a), cy + r * math.sin(a)], fill=(110, 110, 110), width=7)
        t.hd.line([cx, cy, cx + r * math.cos(a), cy + r * math.sin(a)], fill=200, width=7)
    for rr in (60, 120, 180):
        t.d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=(100, 100, 100), width=5); t.hd.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=200, width=5)
    t.ell((cx - 40, cy - 40, cx + 40, cy + 40), (80, 80, 80), 0.8)
    # equipment door
    t.rect((516, 516, S - 12, S - 12), (78, 80, 84), 0.55, 6)
    t.rect((540, 540, S - 36, S - 36), (96, 98, 102), 0.72, 4)
    for y in (600, 920): t.rect((530, y, 552, y + 50), (60, 60, 60), 0.9, 3)    # hinges
    t.rect((S - 90, 740, S - 60, 800), (30, 30, 30), 0.4, 4); t.rect((S - 84, 750, S - 66, 790), (190, 190, 190), 0.9, 3)  # latch
    t.d.polygon([(770, 600), (720, 690), (820, 690)], fill=(235, 190, 20)); t.d.text((760, 640), "!", font=font(40), fill=(10, 10, 10))
    _noisy(t, 4, 24)
    return t.bake("grille", k=6.0, blur=1.1, store=store)


# ---------------------------------------------------------------- bogie side frame, rail wheel, underframe box
def bogie(store):
    S = 1024; t = Tex(S, S, (16, 16, 17), 0.15)
    # side frame (u 0..1 = frame length, v 0..0.5 = frame height): fishbelly side beam, primary coil springs over the
    # axle boxes, dampers, brake calipers, secondary air spring / coil seat in the middle
    y0, y1 = 4, 508; H = y1 - y0
    fy = lambda f: y0 + H * f
    axles = [S * (0.5 - FRAME_WB / 2), S * (0.5 + FRAME_WB / 2)]
    beam = [(0, fy(0.10)), (S, fy(0.10)), (S, fy(0.34)), (axles[1] + 70, fy(0.34)), (axles[1] - 40, fy(0.62)),
            (axles[0] + 40, fy(0.62)), (axles[0] - 70, fy(0.34)), (0, fy(0.34))]
    t.d.polygon(beam, fill=(58, 60, 62)); t.hd.polygon(beam, fill=int(0.80 * 255))
    t.rect((0, fy(0.10), S, fy(0.14)), (80, 82, 84), 0.9)                          # top flange highlight
    for ax in axles:
        t.rect((ax - 60, fy(0.36), ax + 60, fy(0.82)), (48, 50, 52), 0.75, 10)     # axle box housing
        t.ell((ax - 50, fy(0.52), ax + 50, fy(0.52) + 100), (90, 92, 96), 0.95)    # axle end cover
        for k in range(6):
            a = 2 * math.pi * k / 6; bolt(t, ax + 34 * math.cos(a), fy(0.52) + 50 + 34 * math.sin(a), 5)
        for sx in (-95, 95):                                                       # coil springs
            for k in range(7):
                yy = fy(0.16) + k * 20; t.rect((ax + sx - 26, yy, ax + sx + 26, yy + 11), (120, 60, 30), 0.85, 5)
        t.rect((ax - 135, fy(0.62), ax - 110, fy(0.92)), (200, 170, 30), 0.7, 8)   # yellow vertical damper
        t.rect((ax + 110, fy(0.40), ax + 175, fy(0.62)), (36, 36, 38), 0.6, 6)     # brake caliper
    cx = S / 2                                                                      # secondary suspension
    t.rect((cx - 120, fy(0.0), cx + 120, fy(0.12)), (20, 20, 20), 0.95, 30)        # air spring bellows
    t.rect((cx - 150, fy(0.30), cx + 150, fy(0.40)), (70, 70, 72), 0.85, 6)        # bolster
    t.rect((cx - 12, fy(0.40), cx + 12, fy(0.70)), (30, 30, 30), 0.5)              # traction rod
    for x in range(30, S, 70): bolt(t, x, fy(0.22), 4, (110, 110, 112))
    # rail wheel face: steel tread rim, curved web, hub, axle-end bolts, brake disc ring
    cx, cy, r = 256, 768, 248
    t.ell((cx - r, cy - r, cx + r, cy + r), (150, 150, 155), 0.95)                 # flange/tread rim (bright steel)
    t.ell((cx - r * 0.86, cy - r * 0.86, cx + r * 0.86, cy + r * 0.86), (70, 55, 45), 0.55)  # rusty web
    t.ell((cx - r * 0.66, cy - r * 0.66, cx + r * 0.66, cy + r * 0.66), (45, 45, 47), 0.70)  # brake disc
    for k in range(24):
        a = 2 * math.pi * k / 24; t.d.line([cx + r * 0.42 * math.cos(a), cy + r * 0.42 * math.sin(a), cx + r * 0.64 * math.cos(a), cy + r * 0.64 * math.sin(a)], fill=(25, 25, 25), width=6)
    t.ell((cx - r * 0.36, cy - r * 0.36, cx + r * 0.36, cy + r * 0.36), (85, 85, 88), 0.85)  # hub
    t.ell((cx - r * 0.18, cy - r * 0.18, cx + r * 0.18, cy + r * 0.18), (120, 120, 125), 1.0)
    for k in range(4):
        a = 2 * math.pi * k / 4 + 0.4; bolt(t, cx + r * 0.27 * math.cos(a), cy + r * 0.27 * math.sin(a), 9)
    # underframe equipment box face
    X0, Y0, X1, Y1 = 516, 516, 968, 968
    t.rect((X0, Y0, X1, Y1), (52, 54, 58), 0.55, 6)
    t.rect((X0 + 18, Y0 + 18, X1 - 18, Y1 - 18), (66, 68, 72), 0.72, 4)
    for k in range(5): t.rect((X0 + 30, Y0 + 60 + k * 80, X1 - 30, Y0 + 70 + k * 80), (44, 44, 48), 0.5)  # ribs
    for x in (X0 + 60, X1 - 90): t.rect((x, Y0 + 30, x + 30, Y0 + 60), (170, 170, 170), 0.95, 3)          # latches
    t.rect((X0 + 160, Y1 - 120, X0 + 300, Y1 - 60), (230, 200, 30), 0.75, 4)                               # label
    t.rect((S - 36, S - 36, S, S), (6, 6, 6), 0.15)                                                         # dark corner
    _noisy(t, 4, 25)
    return t.bake("bogie", k=7.0, blur=1.1, store=store)


def build_all():
    old = tx.TEX; tx.TEX = TEX                    # Tex.bake writes to tx.TEX; point it at the train folder
    try:
        store = {}
        body(store); door(store); front(store); grille(store); bogie(store)
        tx.overlay(store); tx.bellows(store)      # glass overlay atlas + gangway rubber, shared with the buses
        return store
    finally:
        tx.TEX = old


if __name__ == "__main__":
    build_all(); print("textures ->", TEX)
