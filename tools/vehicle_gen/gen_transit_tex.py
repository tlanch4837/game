"""Procedural textures + height maps for the transit vehicles (gen_transit.py).

Every material gets an albedo and a normal map baked from a hand-drawn height map. The engine rebuilds the height from the
normal map (surface_maps.h) and runs parallax occlusion with sun self-shadowing on it (SurfaceRelief in mesh.hlsl), so panel
seams, rivets, gaskets, louvres, lamp recesses and bellows ribs are relief in the texture instead of triangles.

Window atlas: 4x4 tiles of 512 px, one window bay each (black glazing border = the pillar). Variants mix interiors
(seats, grab poles, seated / standing passengers), tints, frit bands, blinds, curtains, hopper vents, ad wraps, school sashes,
plus a windshield and a rear-window tile. gen_transit picks a tile per pane, so no two vehicles read alike.

Output (also written to assets/vehicles/transit/tex/ for the preview): <mat>_albedo.jpg, <mat>_normal.png, <mat>_height.png
"""
import io, math, os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
import gen_cars as gc

TEX = os.path.join(gc.ROOT, "assets", "vehicles", "transit", "tex")
BODY_H = 4.4      # metres covered by the body texture's height (v = 1 - y / BODY_H)
BODY_PITCH = 3.0  # metres per body texture tile along the vehicle
FLAT = (0.5, 0.012)  # dead-flat clean spot of the body texture (above 4.3 m): roofs, edges, bevels


def font(px, bold=True):
    try: return ImageFont.truetype("arialbd.ttf" if bold else "arial.ttf", px)
    except OSError: return ImageFont.load_default()


class Tex:
    def __init__(self, w, h, bg, hbg=0.5):
        self.w, self.h = w, h
        self.img = Image.new("RGB", (w, h), bg); self.d = ImageDraw.Draw(self.img)
        self.hm = Image.new("L", (w, h), int(hbg * 255)); self.hd = ImageDraw.Draw(self.hm)

    def rect(self, b, fill=None, hv=None, r=0, outline=None, width=1):
        b = [int(round(v)) for v in b]
        if fill is not None or outline is not None:
            self.d.rounded_rectangle(b, r, fill=fill, outline=outline, width=width) if r else self.d.rectangle(b, fill=fill, outline=outline, width=width)
        if hv is not None:
            self.hd.rounded_rectangle(b, r, fill=int(hv * 255)) if r else self.hd.rectangle(b, fill=int(hv * 255))

    def ell(self, b, fill=None, hv=None):
        b = [int(round(v)) for v in b]
        if fill is not None: self.d.ellipse(b, fill=fill)
        if hv is not None: self.hd.ellipse(b, fill=int(hv * 255))

    def bake(self, name, k=6.0, blur=1.2, store=None):
        h = np.asarray(self.hm.filter(ImageFilter.GaussianBlur(blur)), np.float32) / 255.0
        rgb = np.asarray(self.img)
        alb = gc.enc(rgb, "JPEG"); nor = gc.enc(gc.normal_from_height(h, k), "PNG")
        os.makedirs(TEX, exist_ok=True)
        open(os.path.join(TEX, f"{name}_albedo.jpg"), "wb").write(alb)
        open(os.path.join(TEX, f"{name}_normal.png"), "wb").write(nor)
        Image.fromarray((h * 255).astype(np.uint8)).save(os.path.join(TEX, f"{name}_height.png"))
        if store is not None: store[name] = (alb, nor)
        return alb, nor


def noise(w, h, scale, seed, amp):
    rng = np.random.default_rng(seed)
    n = Image.fromarray((rng.random((max(2, h // scale), max(2, w // scale))) * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC)
    return (np.asarray(n, np.float32) / 255.0 - 0.5) * amp


# ---------------------------------------------------------------- body panels
def body(store):
    """Near-white paint carrier (baseColorFactor tints it). One 3 m x 4.4 m tile: vertical panel joint, rivet rows,
    skirt panel line, rub-strip groove, a fuel / service flap; the top 0.15 m is left dead flat for roofs and edges."""
    S = 1024; t = Tex(S, S, (236, 236, 236), 0.6)
    py = lambda y: (1 - y / BODY_H) * S                     # metres above ground -> pixel row
    px = lambda z: z / BODY_PITCH * S
    lo = py(1.15)                                           # relief only below the window line
    t.rect((0, py(0.32), S, py(0.30)), (205, 205, 205), 0.25)          # lower lip
    t.rect((0, py(0.58), S, py(0.52)), (214, 214, 214), 0.35)          # rub-strip groove
    t.rect((0, py(0.82), S, py(0.805)), (200, 200, 200), 0.30)         # skirt panel line
    t.rect((S - 3, lo, S, py(0.30)), (190, 190, 190), 0.25); t.rect((0, lo, 3, py(0.30)), (190, 190, 190), 0.25)  # vertical joint
    for y in (0.84, 1.10):                                 # rivet rows
        for k in range(30):
            x = (k + 0.5) / 30 * S
            t.ell((x - 3.5, py(y) - 3.5, x + 3.5, py(y) + 3.5), (226, 226, 226), 0.78)
    for z in (0.4, 2.6):                                   # rivets along the joints
        pass
    # service flap (fuel / battery hatch) with recessed latch
    x0, x1, y0, y1 = px(1.1), px(1.75), py(1.05), py(0.62)
    t.rect((x0 - 3, y0 - 3, x1 + 3, y1 + 3), (198, 198, 198), 0.33, 10)
    t.rect((x0, y0, x1, y1), (232, 232, 232), 0.6, 8)
    t.rect((x1 - 38, (y0 + y1) / 2 - 7, x1 - 12, (y0 + y1) / 2 + 7), (60, 60, 60), 0.35, 4)
    # subtle orange-peel / waviness so big flat sides catch light like real sheet metal
    a = np.asarray(t.img, np.float32) + noise(S, S, 64, 1, 6)[..., None]
    t.img = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)); t.d = ImageDraw.Draw(t.img)
    t.rect((0, 0, S, py(4.25)), (238, 238, 238), 0.6)       # flat top band
    return t.bake("body", k=5.0, blur=1.4, store=store)


# ---------------------------------------------------------------- window atlas
TILE = 512
WIN = {"clear": 0, "standing": 1, "seated": 2, "tinted": 3, "tinted_heads": 4, "frit": 5, "blind_half": 6, "blind_full": 7,
       "curtain_open": 8, "curtain_half": 9, "adwrap": 10, "hopper": 11, "school": 12, "school_kids": 13, "windshield": 14, "rearwin": 15}
MOQUETTE = [(40, 60, 120), (120, 30, 40), (40, 90, 80), (70, 70, 80), (110, 80, 30)]
SKIN = [(225, 190, 160), (190, 140, 105), (140, 95, 65), (95, 65, 45), (240, 210, 185)]
HAIR = [(30, 25, 20), (80, 55, 30), (160, 120, 70), (200, 190, 170), (20, 20, 20), (120, 40, 25)]
SHIRT = [(40, 50, 90), (150, 40, 40), (220, 220, 210), (60, 110, 60), (30, 30, 30), (200, 150, 40), (90, 60, 120)]


def tile_uv(idx, inset=0.004):
    """uv corners (u0, v0, u1, v1) of atlas tile idx, glTF convention (v down)."""
    c, r = idx % 4, idx // 4
    return (c / 4 + inset, r / 4 + inset, (c + 1) / 4 - inset, (r + 1) / 4 - inset)


def person(d, cx, base, scale, rng, standing=False):
    sk, hr, sh = (SKIN[rng.integers(len(SKIN))], HAIR[rng.integers(len(HAIR))], SHIRT[rng.integers(len(SHIRT))])
    hw = 46 * scale; hh = 58 * scale
    top = base - (300 if standing else 150) * scale
    d.rounded_rectangle([cx - 70 * scale, top + hh * 0.9, cx + 70 * scale, base + 200], int(30 * scale), fill=sh)  # torso
    d.ellipse([cx - hw / 2, top, cx + hw / 2, top + hh], fill=sk)
    d.chord([cx - hw / 2 - 3, top - 4, cx + hw / 2 + 3, top + hh * 0.75], 180, 360, fill=hr)
    if standing:
        d.line([cx + 50 * scale, top + hh, cx + 60 * scale, top - 60 * scale], fill=sk, width=int(14 * scale))  # arm to the rail


def interior(t, X0, Y0, X1, Y1, rng, kind):
    """Draw the saloon seen through one pane into its own image, so nothing spills past the glass, then paste it."""
    X0, Y0, X1, Y1 = int(X0), int(Y0), int(X1), int(Y1)
    sub = Image.new("RGB", (X1 - X0, Y1 - Y0)); _interior(ImageDraw.Draw(sub), 0, 0, X1 - X0, Y1 - Y0, rng, kind)
    t.img.paste(sub, (X0, Y0)); t.d = ImageDraw.Draw(t.img)


def _interior(d, x0, y0, x1, y1, rng, kind):
    W, H = x1 - x0, y1 - y0
    # far side of the saloon: ceiling with a light strip, opposite windows (bright), panels
    d.rectangle([x0, y0, x1, y1], fill=(70, 72, 76))
    d.rectangle([x0, y0, x1, y0 + H * 0.14], fill=(165, 165, 160))
    d.rectangle([x0, y0 + H * 0.07, x1, y0 + H * 0.10], fill=(235, 235, 220))
    d.rectangle([x0, y0 + H * 0.20, x1, y0 + H * 0.62], fill=(150, 175, 195))
    for k in range(3):
        xp = x0 + W * (0.12 + 0.38 * k + rng.uniform(-0.05, 0.05)); d.rectangle([xp, y0 + H * 0.18, xp + W * 0.05, y0 + H * 0.64], fill=(35, 35, 38))
    d.rectangle([x0, y0 + H * 0.62, x1, y1], fill=(90, 90, 92))
    rail = y0 + H * 0.17; d.rectangle([x0, rail, x1, rail + 7], fill=(205, 175, 40))  # ceiling grab rail
    seat_c = MOQUETTE[rng.integers(len(MOQUETTE))]
    if kind not in ("rearwin",):
        n = 3
        for k in range(n):  # seat backs seen side-on, high-backed
            sx = x0 + W * (0.08 + k * 0.33)
            d.rounded_rectangle([sx, y0 + H * 0.58, sx + W * 0.11, y1 + 20], 18, fill=seat_c)
            d.rounded_rectangle([sx + W * 0.02, y0 + H * 0.56, sx + W * 0.10, y0 + H * 0.60], 6, fill=(200, 200, 200))  # grab handle
    for k in range(rng.integers(1, 3)):  # yellow vertical grab poles
        xp = x0 + W * rng.uniform(0.2, 0.9); d.rectangle([xp, rail, xp + 10, y1], fill=(225, 190, 40)); d.rectangle([xp + 2, rail, xp + 4, y1], fill=(250, 235, 140))
    if kind in ("standing",):
        for k in range(rng.integers(1, 3)): person(d, x0 + W * rng.uniform(0.2, 0.8), y0 + H * 0.70, rng.uniform(0.9, 1.05), rng, True)
    if kind in ("standing", "seated", "tinted_heads", "school_kids", "rearwin", "curtain_open"):
        for k in range(3):
            if rng.random() < (0.85 if kind in ("seated", "school_kids") else 0.5):
                person(d, x0 + W * (0.19 + k * 0.33), y0 + H * (0.80 if kind != "school_kids" else 0.84), 0.85 if kind == "school_kids" else 1.0, rng)
        if kind == "rearwin":
            for k in range(4):
                if rng.random() < 0.4: person(d, x0 + W * (0.15 + 0.23 * k), y0 + H * 0.86, 1.0, rng)


def glaze(arr, x0, y0, x1, y1, tint, rng):
    """Glass look over the interior: tint, sky reflection (bright top), and a soft diagonal streak."""
    sub = arr[y0:y1, x0:x1].astype(np.float32); h, w = sub.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w] / np.array([h, w])[:, None, None]
    sub *= tint
    refl = 70 * (1 - yy) ** 2 + 40 * np.exp(-((xx - yy * 0.6 - rng.uniform(0.1, 0.6)) / 0.08) ** 2)
    sub += refl[..., None] * np.array([0.85, 0.95, 1.1])
    arr[y0:y1, x0:x1] = np.clip(sub, 0, 255).astype(np.uint8)


def windows(store):
    S = TILE * 4; t = Tex(S, S, (12, 12, 12), 1.0)
    rng = np.random.default_rng(7)
    names = {v: k for k, v in WIN.items()}
    B, G = 22, 16                               # black ceramic border (= pillar), rubber gasket
    for idx in range(16):
        kind = names[idx]; c, r = idx % 4, idx // 4
        X0, Y0 = c * TILE, r * TILE
        gx0, gy0, gx1, gy1 = X0 + B, Y0 + B, X0 + TILE - B, Y0 + TILE - B
        t.rect((X0, Y0, X0 + TILE - 1, Y0 + TILE - 1), (14, 14, 15), 1.0)
        t.rect((gx0, gy0, gx1, gy1), (28, 28, 30), 0.80, 26)                 # gasket proud of the glass
        px0, py0, px1, py1 = gx0 + G, gy0 + G, gx1 - G, gy1 - G
        t.rect((px0, py0, px1, py1), None, 0.55, 18)                         # glass sits back
        if kind in ("windshield",):
            d = t.d; W, H = px1 - px0, py1 - py0
            d.rectangle([px0, py0, px1, py1], fill=(60, 62, 66))
            d.rectangle([px0, py0 + H * 0.25, px1, py0 + H * 0.65], fill=(120, 135, 150))   # saloon beyond
            d.rectangle([px0, py0 + H * 0.78, px1, py1], fill=(25, 25, 27))                  # dashboard
            d.ellipse([px0 + W * 0.08, py0 + H * 0.60, px0 + W * 0.36, py0 + H * 0.86], outline=(20, 20, 20), width=14)  # wheel
            person(d, px0 + W * 0.22, py0 + H * 0.70, 1.1, rng)
            d.rectangle([px0 + W * 0.55, py0 + H * 0.70, px0 + W * 0.75, py0 + H * 0.78], fill=(50, 120, 160))  # ticket machine screen
        elif kind in ("adwrap",):
            d = t.d
            for k in range(6):
                d.rectangle([px0, py0 + k * (py1 - py0) / 6, px1, py0 + (k + 1) * (py1 - py0) / 6], fill=[(220, 40, 60), (240, 150, 30), (40, 120, 200)][k % 3])
            d.text((px0 + 30, py0 + 120), "SALE", font=font(130), fill=(255, 255, 255))
        else:
            interior(t, px0, py0, px1, py1, rng, kind)
        arr = np.asarray(t.img).copy()
        tint = {"tinted": 0.28, "tinted_heads": 0.32, "adwrap": 0.75}.get(kind, 0.62)
        glaze(arr, px0, py0, px1, py1, tint, rng)
        if kind == "adwrap":  # perforated film: dark dot grid
            yy, xx = np.mgrid[py0:py1, px0:px1]
            dots = ((xx % 10 - 5) ** 2 + (yy % 10 - 5) ** 2) < 9
            arr[py0:py1, px0:px1][dots] = (arr[py0:py1, px0:px1][dots] * 0.35).astype(np.uint8)
        t.img = Image.fromarray(arr); t.d = ImageDraw.Draw(t.img)
        d = t.d; W, H = px1 - px0, py1 - py0
        if kind == "frit":  # ceramic dot band fading down from the top
            for yy in range(py0, py0 + int(H * 0.22), 9):
                rr = 4.5 * (1 - (yy - py0) / (H * 0.22))
                for xx in range(px0, px1, 9): d.ellipse([xx - rr, yy - rr, xx + rr, yy + rr], fill=(10, 10, 10))
        if kind in ("blind_half", "blind_full"):  # roller blind, slatted
            bot = py0 + H * (0.45 if kind == "blind_half" else 0.95)
            for yy in range(py0, int(bot), 14): d.rectangle([px0, yy, px1, yy + 11], fill=(150, 140, 120))
            d.rectangle([px0, bot - 6, px1, bot + 6], fill=(60, 60, 60))
        if kind in ("curtain_open", "curtain_half"):  # pleated curtains gathered at the sides (+ pelmet)
            wcur = W * (0.18 if kind == "curtain_open" else 0.42); col = MOQUETTE[rng.integers(len(MOQUETTE))]
            for xs, xe in ((px0, px0 + wcur), (px1 - wcur, px1)):
                for k, xx in enumerate(np.arange(xs, xe, 18)):
                    sh = 0.75 + 0.25 * math.sin(k * 1.7)
                    d.rectangle([xx, py0, xx + 18, py1], fill=tuple(int(v * sh) for v in col))
                    t.hd.rectangle([xx, py0, xx + 18, py1], fill=int((0.55 + 0.1 * math.sin(k * 1.7)) * 255))
            d.rectangle([px0, py0, px1, py0 + H * 0.08], fill=tuple(int(v * 0.6) for v in col))
        if kind == "hopper":  # top-hinged vent window with its own frame
            yb = py0 + H * 0.30
            t.rect((px0, yb - 9, px1, yb + 9), (30, 30, 32), 0.85)
            t.rect((px0 + 30, py0 + 6, px1 - 30, py0 + 16), (60, 60, 62), 0.75)
        if kind in ("school", "school_kids"):  # split-sash with a lower fixed pane and latches
            ym = py0 + H * 0.48
            t.rect((px0 - 4, ym - 14, px1 + 4, ym + 14), (25, 25, 25), 0.95)
            for xx in (px0 + 40, px1 - 70): t.rect((xx, ym - 22, xx + 30, ym - 14), (170, 170, 170), 1.0)
        if kind == "windshield":
            for xx in (0.30, 0.68):  # parked wipers
                d.line([px0 + W * (xx - 0.25), py1 - 20, px0 + W * xx, py1 - 34], fill=(15, 15, 15), width=10)
    return t.bake("glass", k=7.0, blur=1.6, store=store)


# ---------------------------------------------------------------- glass overlay atlas (InteriorPS)
# The saloon behind the glass is ray-cast by the engine (InteriorPS in mesh.hlsl): no interior is painted here, only what sits ON
# the glass. RGBA, alpha = coverage: 0 = clear glass (the ray-cast interior shows), 1 = opaque (blind, frame, curtain).
OVL = {"clear": 0, "clear_sticker": 1, "tint_light": 2, "tint_dark": 3, "frit": 4, "blind_half": 5, "blind_full": 6,
       "curtain_open": 7, "curtain_half": 8, "adwrap": 9, "hopper": 10, "school": 11, "school_down": 12, "frit_hopper": 13,
       "windshield": 14, "rear": 15}


def overlay(store):
    S = TILE * 4; img = Image.new("RGBA", (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(img)
    rng = np.random.default_rng(11); names = {v: k for k, v in OVL.items()}
    for idx in range(16):
        kind = names[idx]; X0, Y0 = (idx % 4) * TILE, (idx // 4) * TILE
        B = 20 if kind not in ("windshield", "rear") else 14
        px0, py0, px1, py1 = X0 + B, Y0 + B, X0 + TILE - B, Y0 + TILE - B; W_, H_ = px1 - px0, py1 - py0
        d.rectangle([X0, Y0, X0 + TILE - 1, Y0 + TILE - 1], fill=(12, 12, 13, 255))               # black ceramic border = pillar
        d.rounded_rectangle([px0, py0, px1, py1], 16, fill=(0, 0, 0, 0))                          # clear
        d.rounded_rectangle([px0, py0, px1, py1], 16, outline=(30, 30, 32, 255), width=6)          # rubber gasket
        if kind == "tint_light": d.rounded_rectangle([px0 + 6, py0 + 6, px1 - 6, py1 - 6], 12, fill=(8, 12, 14, 90))
        if kind in ("tint_dark", "school_down"): d.rounded_rectangle([px0 + 6, py0 + 6, px1 - 6, py1 - 6], 12, fill=(6, 8, 10, 165))
        if kind == "rear": d.rounded_rectangle([px0 + 6, py0 + 6, px1 - 6, py1 - 6], 12, fill=(6, 8, 10, 70))
        if kind in ("frit", "frit_hopper", "windshield"):  # ceramic dot band fading down from the top
            band = 0.22 if kind != "windshield" else 0.12
            for yy in range(py0, py0 + int(H_ * band), 9):
                rr = 4.6 * (1 - (yy - py0) / (H_ * band))
                for xx in range(px0, px1, 9): d.ellipse([xx - rr, yy - rr, xx + rr, yy + rr], fill=(10, 10, 10, 255))
        if kind == "clear_sticker":  # small route / accessibility decals
            d.rounded_rectangle([px0 + 20, py1 - 90, px0 + 90, py1 - 20], 8, fill=(30, 80, 170, 255))
            d.ellipse([px0 + 38, py1 - 78, px0 + 72, py1 - 44], outline=(255, 255, 255, 255), width=5)
        if kind in ("blind_half", "blind_full"):
            bot = py0 + H_ * (0.42 if kind == "blind_half" else 0.94)
            for yy in range(py0, int(bot), 14): d.rectangle([px0, yy, px1, yy + 11], fill=(158, 148, 128, 255)); d.rectangle([px0, yy + 11, px1, yy + 13], fill=(110, 104, 90, 255))
            d.rectangle([px0, bot - 6, px1, bot + 6], fill=(60, 60, 60, 255))
        if kind in ("curtain_open", "curtain_half"):
            col = [(40, 60, 120), (120, 30, 40), (40, 90, 80), (110, 80, 30)][rng.integers(4)]
            wc = W_ * (0.17 if kind == "curtain_open" else 0.40)
            for xs, xe in ((px0, px0 + wc), (px1 - wc, px1)):
                for k, xx in enumerate(np.arange(xs, xe, 16)):
                    sh = 0.72 + 0.28 * math.sin(k * 1.7); d.rectangle([xx, py0, xx + 16, py1], fill=(*(int(v * sh) for v in col), 255))
            d.rectangle([px0, py0, px1, py0 + H_ * 0.07], fill=(*(int(v * 0.55) for v in col), 255))
        if kind == "adwrap":  # perforated vinyl: graphic with a dot grid you can partly see through
            g = Image.new("RGB", (W_, H_)); gd = ImageDraw.Draw(g)
            for k in range(5): gd.rectangle([0, k * H_ / 5, W_, (k + 1) * H_ / 5], fill=[(220, 40, 60), (240, 150, 30), (40, 120, 200)][k % 3])
            gd.text((30, H_ * 0.35), "SALE", font=font(130), fill=(255, 255, 255))
            a = np.full((H_, W_), 215, np.uint8); yy, xx = np.mgrid[0:H_, 0:W_]; a[((xx % 10 - 5) ** 2 + (yy % 10 - 5) ** 2) < 9] = 40
            img.paste(Image.fromarray(np.dstack([np.asarray(g), a])), (px0, py0)); d = ImageDraw.Draw(img)
        if kind in ("hopper", "frit_hopper"):
            yb = py0 + H_ * 0.30; d.rectangle([px0, yb - 9, px1, yb + 9], fill=(28, 28, 30, 255)); d.rectangle([px0 + 30, py0 + 4, px1 - 30, py0 + 14], fill=(60, 60, 62, 255))
        if kind in ("school", "school_down"):
            ym = py0 + H_ * 0.48; d.rectangle([px0 - 4, ym - 14, px1 + 4, ym + 14], fill=(24, 24, 24, 255))
            for xx in (px0 + 40, px1 - 70): d.rectangle([xx, ym - 22, xx + 30, ym - 14], fill=(170, 170, 170, 255))
        if kind == "windshield":
            for xx in (0.30, 0.70): d.line([px0 + W_ * (xx - 0.26), py1 - 16, px0 + W_ * xx, py1 - 30], fill=(15, 15, 15, 255), width=9)
            d.rectangle([px0 + W_ * 0.05, py0 + H_ * 0.12, px0 + W_ * 0.42, py0 + H_ * 0.22], fill=(40, 40, 42, 200))  # sun blind
    b = io.BytesIO(); img.save(b, "PNG", optimize=True); png = b.getvalue()
    os.makedirs(TEX, exist_ok=True); open(os.path.join(TEX, "interior_albedo.png"), "wb").write(png)
    store["interior"] = (png, None)
    return png


# ---------------------------------------------------------------- front (LED sign + fascias) and rear
def led(t, b, text, rng, col=(255, 170, 30)):
    x0, y0, x1, y1 = b; t.rect(b, (8, 8, 8), 0.35)
    cols, rows = 96, 12
    small = Image.new("L", (cols, rows), 0); ImageDraw.Draw(small).text((1, -1), text, font=font(11), fill=255)
    m = np.asarray(small); dx, dy = (x1 - x0) / cols, (y1 - y0) / rows
    for j in range(rows):
        for i in range(cols):
            on = m[j, i] > 100; c = col if on else (28, 22, 14)
            t.d.ellipse([x0 + i * dx + 1, y0 + j * dy + 1, x0 + (i + 1) * dx - 1, y0 + (j + 1) * dy - 1], fill=c)


def lamp(t, b, rng, kind="head"):
    x0, y0, x1, y1 = b
    t.rect(b, (20, 20, 22), 0.25, 18)                          # recess
    if kind == "head":
        t.rect((x0 + 8, y0 + 8, x1 - 8, y1 - 8), (70, 72, 78), 0.40, 14)
        for k in range(3):                                     # LED projector cups
            cx = x0 + (x1 - x0) * (0.22 + 0.28 * k); cy = (y0 + y1) / 2; r = (y1 - y0) * 0.26
            t.ell((cx - r, cy - r, cx + r, cy + r), (215, 220, 230), 0.55); t.ell((cx - r * 0.45, cy - r * 0.45, cx + r * 0.45, cy + r * 0.45), (255, 255, 250), 0.35)
        t.rect((x0 + 10, y1 - 22, x1 - 10, y1 - 12), (255, 250, 235), 0.5)   # DRL light bar
        t.rect((x1 - 40, y0 + 12, x1 - 14, y1 - 12), (230, 140, 30), 0.45)    # indicator
    else:                                                      # tail cluster: red, amber, reverse
        h = y1 - y0
        t.rect((x0 + 8, y0 + 8, x1 - 8, y0 + h * 0.45), (170, 15, 15), 0.45, 10)
        t.rect((x0 + 8, y0 + h * 0.48, x1 - 8, y0 + h * 0.70), (220, 120, 20), 0.45, 8)
        t.rect((x0 + 8, y0 + h * 0.73, x1 - 8, y1 - 8), (215, 215, 215), 0.45, 8)


def sign(text, route_col=(255, 170, 30)):
    """Destination sign texture for one vehicle: amber dot-matrix, route number left, destination fitted to the rest.
    text = "12 CENTRAL" -> route "12", destination "CENTRAL" (first word is the route when it has a digit)."""
    words = text.split(); route = words[0] if words and any(c.isdigit() for c in words[0]) else ""
    dest = " ".join(words[1:] if route else words)
    cols, rows = 128, 14
    small = Image.new("L", (cols, rows), 0); sd = ImageDraw.Draw(small)
    x = 1
    if route:
        sd.text((1, 0), route, font=font(13), fill=255); x = int(sd.textlength(route, font=font(13))) + 6
    f = 12
    while f > 6 and sd.textlength(dest, font=font(f)) > cols - x - 1: f -= 1   # shrink long destinations to fit
    sd.text((x, (rows - f) // 2 - 1), dest, font=font(f), fill=255)
    m = np.asarray(small) > 100
    S = 8; img = Image.new("RGB", (cols * S, rows * S), (8, 8, 8)); d = ImageDraw.Draw(img)
    for j in range(rows):
        for i in range(cols):
            d.ellipse([i * S + 1, j * S + 1, (i + 1) * S - 1, (j + 1) * S - 1], fill=route_col if m[j, i] else (30, 23, 14))
    return gc.enc(np.asarray(img), "JPEG")


def front(store):
    """1024^2: v 0..0.15 LED destination sign | 0.20..0.55 bus/tram lower fascia | 0.60..1.0 school-bus grille."""
    S = 1024; t = Tex(S, S, (30, 30, 32), 0.5); rng = np.random.default_rng(3)
    t.rect((0, 0, S, 0.15 * S), (8, 8, 8), 0.35)  # (destination sign: its own texture per vehicle, see sign())
    y0, y1 = 0.20 * S, 0.55 * S                                # fascia: black bumper skin with lamps and a grille slot
    t.rect((0, y0, S, y1), (36, 36, 38), 0.55)
    lamp(t, (20, y0 + 30, 300, y0 + 150), rng); lamp(t, (S - 300, y0 + 30, S - 20, y0 + 150), rng)
    for k in range(7): t.rect((360, y0 + 40 + k * 22, S - 360, y0 + 52 + k * 22), (14, 14, 14), 0.30, 5)  # grille slats
    t.rect((S / 2 - 130, y1 - 110, S / 2 + 130, y1 - 50), (235, 235, 230), 0.62, 6)                        # plate
    t.d.text((S / 2 - 105, y1 - 108), "  2041", font=font(44), fill=(20, 20, 20))
    t.rect((0, y1 - 30, S, y1), (24, 24, 24), 0.42)                                                       # bumper lip
    for x in (60, S - 140): t.rect((x, y1 - 95, x + 80, y1 - 55), (225, 225, 220), 0.45, 18)              # fog lamps
    y0, y1 = 0.60 * S, S                                        # school bus: chrome-framed grille, round headlamps
    t.rect((0, y0, S, y1), (195, 195, 190), 0.7)
    t.rect((40, y0 + 30, S - 40, y1 - 30), (20, 20, 20), 0.35, 20)
    for k in range(12): t.rect((70, y0 + 50 + k * 28, S - 70, y0 + 62 + k * 28), (150, 150, 150), 0.62)
    for x in (120, S - 120):
        t.ell((x - 70, y0 + 50, x + 70, y0 + 190), (200, 200, 195), 0.75); t.ell((x - 50, y0 + 70, x + 50, y0 + 170), (250, 250, 240), 0.5)
    return t.bake("front", k=6.0, blur=1.2, store=store)


def rear(store):
    """1024^2: v 0..0.45 lower rear fascia | 0.50..1.0 engine-bay louvre door."""
    S = 1024; t = Tex(S, S, (32, 32, 34), 0.5); rng = np.random.default_rng(4)
    y0, y1 = 0, 0.45 * S
    t.rect((0, y0, S, y1), (38, 38, 40), 0.55)
    lamp(t, (20, y0 + 20, 170, y1 - 40), rng, "tail"); lamp(t, (S - 170, y0 + 20, S - 20, y1 - 40), rng, "tail")
    t.rect((S / 2 - 130, y1 - 140, S / 2 + 130, y1 - 80), (235, 225, 90), 0.62, 6)
    t.d.text((S / 2 - 105, y1 - 138), "  2041", font=font(44), fill=(20, 20, 20))
    t.rect((0, y1 - 30, S, y1), (24, 24, 24), 0.42)
    y0, y1 = 0.50 * S, S
    t.rect((0, y0, S, y1), (60, 60, 62), 0.7)
    for k in range(16):  # louvres: each slat a ramp (bright, raised top edge -> dark recessed slot)
        yy = y0 + 25 + k * 29
        t.rect((40, yy, S - 40, yy + 14), (95, 95, 98), 0.85); t.rect((40, yy + 14, S - 40, yy + 24), (10, 10, 10), 0.2)
    t.rect((S / 2 - 40, y1 - 60, S / 2 + 40, y1 - 40), (150, 150, 150), 0.9, 6)  # latch
    return t.bake("rear", k=6.0, blur=1.2, store=store)


def door(store):
    """512x1024 double glazed leaf door: frames, centre rubber seal, push button, kick panels, step lighting strip."""
    W, H = 512, 1024; t = Tex(W, H, (20, 20, 22), 0.9); rng = np.random.default_rng(5)
    for x0, x1 in ((10, 250), (262, 502)):
        t.rect((x0, 10, x1, H - 10), (34, 34, 36), 0.85, 14)
        gx0, gy0, gx1, gy1 = x0 + 18, 30, x1 - 18, int(H * 0.74)
        t.rect((gx0, gy0, gx1, gy1), None, 0.55, 10)
        interior(t, gx0, gy0, gx1, gy1, rng, "clear")
        arr = np.asarray(t.img).copy(); glaze(arr, gx0, gy0, gx1, gy1, 0.6, rng); t.img = Image.fromarray(arr); t.d = ImageDraw.Draw(t.img)
        t.rect((gx0, int(H * 0.77), gx1, H - 40), (45, 45, 48), 0.75, 8)       # kick panel
        t.rect((gx0, int(H * 0.40), gx1, int(H * 0.40) + 14), (200, 200, 200), 0.95, 6)  # handrail across the glass
    t.rect((250, 0, 262, H), (8, 8, 8), 1.0)                                      # centre seal
    t.ell((200, int(H * 0.50), 236, int(H * 0.50) + 36), (40, 160, 70), 0.95)   # door-open button
    t.rect((10, H - 26, W - 10, H - 14), (255, 240, 200), 0.6)                    # step light
    return t.bake("door", k=6.0, blur=1.3, store=store)


def bellows(store):
    """Concertina rubber: tiled ribs (u along the joint), 8 folds per tile."""
    W, H = 256, 256; t = Tex(W, H, (30, 30, 30), 0.5)
    for k in range(8):
        x = k * W / 8
        for i in range(int(W / 8)):
            s = math.sin(math.pi * i / (W / 8))
            t.d.line([x + i, 0, x + i, H], fill=(int(18 + 40 * s),) * 3); t.hd.line([x + i, 0, x + i, H], fill=int(255 * (0.15 + 0.85 * s)))
    return t.bake("bellows", k=9.0, blur=0.8, store=store)


ADS = ["CITY ZOO", "FRESH BREAD", "SUMMER FEST", "LEARN TO SWIM"]


def ads(store):
    """2x2 atlas of side advert posters (flat: normal map is neutral)."""
    S = 1024; t = Tex(S, S, (255, 255, 255), 0.5)
    bgs = [(30, 120, 70), (200, 120, 30), (180, 30, 90), (30, 90, 170)]
    for k, (txt, bg) in enumerate(zip(ADS, bgs)):
        x0, y0 = (k % 2) * 512, (k // 2) * 512
        t.rect((x0, y0, x0 + 511, y0 + 511), bg)
        t.ell((x0 + 300, y0 + 60, x0 + 520, y0 + 300), tuple(min(255, v + 60) for v in bg))
        t.d.text((x0 + 30, y0 + 330), txt, font=font(64), fill=(255, 255, 255))
        t.d.text((x0 + 30, y0 + 420), "this weekend", font=font(36, False), fill=(240, 240, 240))
        t.rect((x0, y0, x0 + 511, y0 + 511), outline=(250, 250, 250), width=10)
    return t.bake("ad", k=0.0, store=store)


def build_all():
    store = {}
    body(store); overlay(store); front(store); rear(store); door(store); bellows(store); ads(store)
    return store


if __name__ == "__main__":
    build_all(); print("textures ->", TEX)
