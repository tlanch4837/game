"""Building windows as 'roomglass' (RoomGlassPS in src/shaders/mesh.hlsl): a parallax room behind every pane under smooth, scratched glass.

A roomglass material carries
  baseColorTexture   a 4x4 atlas of back walls seen straight on: a room's furniture, its curtains or blinds, sometimes a lamp
  normalTexture      a wear mask in R: scratches, drips, grime (brighter = dirtier), tiled in metres
and its vertices carry
  uv                 metres across the window plane (along T = cross(up, N)) and up it
  COLOR_0            (room width m, room height m, seed): rooms repeat along the plane, so one quad can glaze a whole facade

Every building family's panes go through pane() or plane() here, and every writer gives the material roughness 0.05.
"""
import io
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import facade_hd as fh

MAT = {"rough": 0.05, "metal": 0.0}


def _jpg(a, q=88):
    b = io.BytesIO(); Image.fromarray(a).save(b, "JPEG", quality=q); return b.getvalue()


def _png(a):
    b = io.BytesIO(); Image.fromarray(a).save(b, "PNG", optimize=True); return b.getvalue()


def room_atlas(seed, kind="home", cell_aspect=1.0, px=256):
    """16 back walls, 4x4, each px wide: a room seen straight on with what hangs at its window. kind: home / office / lobby."""
    rng = np.random.default_rng(seed)
    H = int(px / cell_aspect)
    atlas = np.zeros((4 * H, 4 * px, 3), np.float32)
    if kind == "lobby":
        for k in range(16):
            im = fh.lobby_field(seed * 31 + k, Wm=8.0, Hm=8.0 / cell_aspect, ppm=px / 8.0).astype(np.float32) / 255.0
            im = np.asarray(Image.fromarray((im * 255).astype(np.uint8)).resize((px, H), Image.LANCZOS), np.float32) / 255.0
            atlas[(k // 4) * H:(k // 4 + 1) * H, (k % 4) * px:(k % 4 + 1) * px] = im
    elif kind == "shop":                                                          # shelves of goods, a counter, bright light
        for k in range(16):
            wall = np.array([(0.92, 0.9, 0.86), (0.8, 0.84, 0.88), (0.9, 0.85, 0.78), (0.95, 0.95, 0.95)][rng.integers(4)], np.float32)
            im = np.ones((H, px, 3), np.float32) * wall * 0.75
            im[int(0.82 * H):] = np.array((0.7, 0.68, 0.64)) * 0.7
            for row in np.linspace(0.18, 0.7, int(rng.integers(3, 6))):
                y = int(row * H); im[y:y + 3] = 0.35
                x = 4
                while x < px - 10:                                                  # goods in blocks, big enough not to shimmer
                    bw = min(int(rng.integers(12, 28)), px - x); hh = int(rng.integers(10, 22)); c = rng.uniform(0.25, 0.95, 3)
                    im[max(0, y - hh):y, x:x + bw] = c * (0.85 + 0.15 * np.linspace(0, 1, bw))[None, :, None]; x += bw + 2
            cx = int(rng.uniform(0.1, 0.6) * px)
            im[int(0.6 * H):int(0.82 * H), cx:cx + int(0.35 * px)] = np.array((0.3, 0.24, 0.2)) * rng.uniform(0.8, 1.4)
            im[:int(0.06 * H)] = (0.95, 0.95, 0.92)
            atlas[(k // 4) * H:(k // 4 + 1) * H, (k % 4) * px:(k % 4 + 1) * px] = im * rng.uniform(0.9, 1.1)
    else:
        kinds = dict(none=0.25, roller=0.4, venetian=0.25, curtains=0.07, net=0.03) if kind == "office" else None
        g = fh.Glazing(rng, None, kinds=kinds, lit=0.0, room_dark=1.6 if kind == "office" else 1.9)
        for k in range(16):
            lit = rng.random() < 0.15
            room = g.room(px, H, lit)
            if kind == "office":                                                  # desks and screens instead of sofas
                room[int(0.62 * H):int(0.7 * H), int(0.1 * px):int(0.9 * px)] = np.array((0.5, 0.45, 0.38)) * 0.6
                for x in np.arange(0.15, 0.85, 0.22): room[int(0.5 * H):int(0.6 * H), int(x * px):int((x + 0.1) * px)] = (0.06, 0.07, 0.09)
            img = g.covering(room, lit)
            atlas[(k // 4) * H:(k // 4 + 1) * H, (k % 4) * px:(k % 4 + 1) * px] = img
    return _jpg(np.clip(atlas * 255, 0, 255).astype(np.uint8))


def wear_mask(seed, px=256):
    """Grey wear for glass, 1.2 m per repeat: soft grime blotches, drip streaks from the top, fine scratches. Returned as the normal slot's
    PNG with the mask in R (G, B carry the same, A opaque)."""
    rng = np.random.default_rng(seed)
    m = np.zeros((px, px), np.float32)
    blot = fh.pnoise(rng, px, 18) * 0.5 + fh.pnoise(rng, px, 5) * 0.2
    m += np.clip(blot * 0.35 + 0.15, 0, 1)
    for _ in range(14):                                                             # drips running down
        x = rng.integers(px); y0 = rng.integers(px // 3); L = rng.integers(px // 6, px // 2)
        w = rng.integers(1, 3)
        prof = np.linspace(0.55, 0.0, L)
        for k in range(L): m[(y0 + k) % px, x:x + w] += prof[k]
    im = Image.fromarray((np.clip(m, 0, 1) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8))
    dr = ImageDraw.Draw(im)
    for _ in range(16):                                                             # scratches: thin, bright, at random angles
        x, y = rng.integers(px), rng.integers(px); a = rng.uniform(0, np.pi); L = rng.uniform(8, 60)
        dr.line([(x, y), (x + np.cos(a) * L, y + np.sin(a) * L)], fill=int(rng.uniform(215, 250)), width=1)
    a = np.asarray(im, np.uint8)
    return _png(np.dstack([a, a, a]))


def maps(seed, kind="home", cell_aspect=1.0):
    return room_atlas(seed, kind, cell_aspect), wear_mask(seed + 7)


def pane(mesh, mat, p0, right, up, w, h, room=None, seed=0.0, origin_uv=(0.0, 0.0)):
    """One roomglass quad: lower-left corner p0, unit vectors right (= cross(up, N)) and up, w x h metres.
    room: (room width, room height) in metres; default the pane's own size, so the pane is one room."""
    p0 = np.array(p0, np.float32); right = np.array(right, np.float32); up = np.array(up, np.float32)
    N = np.cross(right, up); N /= np.linalg.norm(N)
    rw, rh = room if room else (w, h)
    u0, v0 = origin_uv
    pts = [p0, p0 + right * w, p0 + right * w + up * h, p0 + up * h]
    uvs = [(u0, v0), (u0 + w, v0), (u0 + w, v0 + h), (u0, v0 + h)]
    if mat not in mesh.prims: mesh.prims[mat] = []
    mesh.raw_quad(mat, pts, N, uvs, [(rw, rh, seed)] * 4)


def panes_in_cell(mesh, rects, p_ll, right, w, h, n_off, normal, seed=0.0, mat="roomglass"):
    """Roomglass panes over the glass of a decal quad: rects are (x0, x1, y0, y1) fractions of the quad, y measured down from its top.
    p_ll: the quad's lower-left corner; right: unit vector along it (= cross(up, N)); n_off: how far in front of the decal."""
    p_ll = np.array(p_ll, np.float32); right = np.array(right, np.float32); Nv = np.array(normal, np.float32)
    for k, (fx0, fx1, fy0, fy1) in enumerate(rects):
        gw, gh = w * (fx1 - fx0), h * (fy1 - fy0)
        p0 = p_ll + right * w * fx0 + np.array([0, h * (1 - fy1), 0], np.float32) + Nv * n_off
        pane(mesh, mat, p0, right, (0, 1, 0), gw, gh, room=(float(np.clip(gw, 1.2, 4.0)), float(np.clip(gh, 1.2, 3.5))), seed=float((seed + k * 1.7) % 97.0))
