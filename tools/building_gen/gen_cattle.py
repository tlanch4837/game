"""Rigged close-up cattle: the cow sculpt (cow_sculpt.py) meshed, decimated to a low-poly budget, vertex-coloured by breed,
skinned to a 13-bone skeleton and given two looping clips: "walk" (lateral-sequence gait, head bob, tail swish) and
"graze" (head down, chewing, weight shifts). The far LOD is the 12-triangle SPOM volume from industry_spom.py.

    python tools/building_gen/gen_cattle.py   -> assets/industry/cattle/glb/COW_<breed>.glb
Model space: +x forward (nose), +y up, +z the cow's left; origin on the ground under the barrel. 1 unit = 1 m.
"""
import json
import math
import os
import struct
import sys

import numpy as np
import fast_simplification
from skimage.measure import marching_cubes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cow_sculpt import cow_sdf, _smin, _noise3  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
OUT = os.path.join(ROOT, 'assets', 'industry', 'cattle', 'glb')
TARGET_TRIS = 400
A_OFF = 1.0  # sculpt a (0 tail .. 2.6 nose) -> model x = a - A_OFF


def C(hexv):
    return np.array([(hexv >> 16) & 255, (hexv >> 8) & 255, hexv & 255], np.float32) / 255.0


# ------------------------------------------------------------------------------------------------ mesh
def field(P, udder):
    d, part, ud = cow_sdf(P, False, stance=False)
    if udder:
        d = _smin(d, ud, 0.04)
    return d, part, ud


def build_mesh(udder):
    step = 0.018
    a = np.arange(-0.3, 2.85, step, dtype=np.float32)
    h = np.arange(-0.04, 1.85, step, dtype=np.float32)
    s = np.arange(-0.5, 0.5, step, dtype=np.float32)
    AA, HH, SS = np.meshgrid(a, h, s, indexing='ij')
    d, _, _ = field((AA, HH, SS), udder)
    v, f, _, _ = marching_cubes(d, 0.0, spacing=(step, step, step))
    v = v + np.array([a[0], h[0], s[0]], np.float32)
    v = np.ascontiguousarray(v, np.float32)
    f = np.ascontiguousarray(f[:, ::-1], np.int32)  # outward winding
    ratio = 1.0 - TARGET_TRIS / len(f)
    v, f = fast_simplification.simplify(v, f, target_reduction=max(0.0, ratio))
    v = np.maximum(v, [-1e9, 0.0, -1e9])            # hooves flat on the ground
    return v.astype(np.float32), f.astype(np.int32)


def colours(v, breed):
    P = (v[:, 0], v[:, 1], v[:, 2])
    d, part, ud = cow_sdf(P, False, stance=False)
    A, Hh, S = P
    if breed == 'holstein':
        part = np.where(ud <= np.minimum(d, 0) + 0.012, 4, part)
        n = _noise3(P, 5.0, 11) + 0.45 * _noise3(P, 11.0, 12)
        col = np.where(((n > 0.12) & (part != 4))[:, None], C(0x151413), C(0xDAD5CA))
        hd = (part == 1) | (part == 8)
        col[hd] = C(0x151413)
        col[hd & (np.abs(S) < 0.06) & (A > 2.3)] = C(0xDAD5CA)                       # blaze
        col[(part == 2) & (Hh < 0.45)] = C(0xDAD5CA)
        col[part == 4] = C(0xD5A39A)
        col[part == 7] = C(0x1A1816)
    elif breed == 'angus':
        col = np.broadcast_to(C(0x161412), (len(A), 3)).copy() * (1 + 0.25 * np.clip(Hh - 0.9, 0, 1))[:, None]
    else:
        col = np.broadcast_to(C(0x7B3A1F), (len(A), 3)).copy()
        white = (part == 1) | (part == 5) | (part == 8) | (part == 7)
        white |= (Hh < 0.86) & (A > 0.45) & (A < 1.75) & (part == 0)
        white |= (A > 1.75) & (Hh < 1.02) & (part == 0)
        white |= (part == 2) & (Hh < 0.35)
        white |= (A < 0.1) & (Hh > 1.2) & (part == 0)
        col[white] = C(0xE7E0D2)
        col[(part == 1) & (np.abs(S) > 0.12)] = C(0x7B3A1F)
    col[part == 3] = C(0x2A2420)
    col[part == 5] = col[part == 5] * 0.55 + C(0x4A3A36) * 0.45
    col[part == 6] = C(0x0A0808)
    # baked occlusion from the field (creases, belly, between the legs), as on the SPOM LOD
    occ = np.ones(len(A), np.float32)
    nrm = vertex_normals_from_field(P)
    for dl in (0.05, 0.12):
        nd, _, _ = cow_sdf((A + nrm[:, 0] * dl, Hh + nrm[:, 1] * dl, S + nrm[:, 2] * dl), False, stance=False)
        occ *= np.clip(0.5 + 0.5 * nd / dl, 0.5, 1.0)
    occ *= np.clip(0.72 + 0.28 * Hh, 0.72, 1.0)
    return np.clip(col * occ[:, None], 0, 1).astype(np.float32), part


def vertex_normals_from_field(P):
    e = 0.01
    A, Hh, S = P
    g = [cow_sdf((A + e, Hh, S), False, stance=False)[0] - cow_sdf((A - e, Hh, S), False, stance=False)[0],
         cow_sdf((A, Hh + e, S), False, stance=False)[0] - cow_sdf((A, Hh - e, S), False, stance=False)[0],
         cow_sdf((A, Hh, S + e), False, stance=False)[0] - cow_sdf((A, Hh, S - e), False, stance=False)[0]]
    n = np.stack(g, 1)
    return n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-6)


# ------------------------------------------------------------------------------------------------ skeleton (sculpt coords)
BONES = [  # name, parent, rest head position (a, h, s)
    ('root', None, (1.0, 0.0, 0.0)),
    ('pelvis', 'root', (0.35, 1.22, 0.0)),
    ('spine', 'pelvis', (1.0, 1.28, 0.0)),
    ('chest', 'spine', (1.6, 1.22, 0.0)),
    ('neck', 'chest', (1.8, 1.22, 0.0)),
    ('head', 'neck', (2.14, 1.4, 0.0)),
    ('tail', 'pelvis', (0.02, 1.32, 0.0)),
    ('legFL', 'chest', (1.66, 0.98, 0.17)), ('shinFL', 'legFL', (1.68, 0.48, 0.17)),
    ('legFR', 'chest', (1.66, 0.98, -0.17)), ('shinFR', 'legFR', (1.68, 0.48, -0.17)),
    ('legHL', 'pelvis', (0.41, 1.0, 0.17)), ('shinHL', 'legHL', (0.3, 0.52, 0.17)),
    ('legHR', 'pelvis', (0.41, 1.0, -0.17)), ('shinHR', 'legHR', (0.3, 0.52, -0.17)),
]
BI = {b[0]: i for i, b in enumerate(BONES)}


def weights(v, part):
    A, Hh, S = v[:, 0], v[:, 1], v[:, 2]
    n = len(A)
    W = np.zeros((n, len(BONES)), np.float32)
    # trunk: pelvis -> spine -> chest by position along the body
    t = np.clip((A - 0.35) / 0.65, 0, 1)
    u = np.clip((A - 1.0) / 0.6, 0, 1)
    W[:, BI['pelvis']] = 1 - t
    W[:, BI['spine']] = t * (1 - u)
    W[:, BI['chest']] = t * u
    # neck and head
    neckw = np.clip((A - 1.72) / 0.25, 0, 1)
    headw = np.clip((A - 2.08) / 0.12, 0, 1)
    head = (part == 1) | (part == 5) | (part == 6) | (part == 8)
    headw = np.maximum(headw, head.astype(np.float32))
    for k in ('pelvis', 'spine', 'chest'):
        W[:, BI[k]] *= 1 - neckw
    W[:, BI['neck']] = neckw * (1 - headw)
    W[:, BI['head']] = neckw * headw + (head & (neckw < 1)) * 0
    W[head, :] = 0
    W[head, BI['head']] = 1
    # tail
    tail = (A < 0.02) & (Hh < 1.3) | (part == 7)
    W[tail, :] = 0
    W[tail, BI['tail']] = 1
    # legs: below the belly, near a leg axis; thigh/forearm above the joint, shin below, blended across it
    for side, sv in (('L', 0.17), ('R', -0.17)):
        for end, a_up, a_lo, joint_h in (('F', 1.66, 1.68, 0.48), ('H', 0.41, 0.3, 0.52)):
            near = (np.abs(S - sv) < 0.13) & (np.abs(A - (a_up + a_lo) * 0.5) < 0.24) & (Hh < 1.0)
            # blend into the body over the top of the leg
            body_mix = np.clip((Hh - 0.82) / 0.18, 0, 1)
            lo = np.clip((joint_h + 0.06 - Hh) / 0.12, 0, 1)
            lw = (1 - body_mix)
            W[near] *= body_mix[near][:, None]
            W[near, BI['leg' + end + side]] += lw[near] * (1 - lo[near])
            W[near, BI['shin' + end + side]] += lw[near] * lo[near]
    # keep the four largest, normalise
    idx = np.argsort(-W, axis=1)[:, :4]
    w4 = np.take_along_axis(W, idx, 1)
    w4 = w4 / np.maximum(w4.sum(1, keepdims=True), 1e-6)
    return idx.astype(np.uint16), w4.astype(np.float32)


# ------------------------------------------------------------------------------------------------ animation
def qaxis(axis, ang):
    s = math.sin(ang / 2)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(ang / 2)]


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]


Z, X, Y = (0, 0, 1), (1, 0, 0), (0, 1, 0)   # pitch about the side axis (z): + lifts the front of a bone forward


def clip_walk(n=24, T=1.3):
    """lateral-sequence walk: hind left, fore left, hind right, fore right a quarter cycle apart."""
    times = [T * i / n for i in range(n + 1)]
    tracks = {}
    phase = {'HL': 0.0, 'FL': 0.25, 'HR': 0.5, 'FR': 0.75}
    for leg, ph in phase.items():
        up, lo = [], []
        for t in times:
            c = 2 * math.pi * (t / T + ph)
            swing = math.sin(c) * 0.32                         # forward / back about the shoulder or hip
            lift = max(0.0, math.sin(c + math.pi / 2)) * (0.55 if leg[0] == 'F' else -0.5)  # knee folds back / hock forward in the swing
            up.append(qaxis(Z, swing))
            lo.append(qaxis(Z, lift))
        tracks['leg' + leg] = up
        tracks['shin' + leg] = lo
    tracks['neck'] = [qaxis(Z, -0.08 + 0.06 * math.sin(4 * math.pi * t / T)) for t in times]   # head nods twice a stride
    tracks['head'] = [qaxis(Z, 0.05 * math.sin(4 * math.pi * t / T + 1.0)) for t in times]
    tracks['spine'] = [qaxis(Y, 0.035 * math.sin(2 * math.pi * t / T)) for t in times]           # barrel sways
    tracks['pelvis'] = [qaxis(X, 0.03 * math.sin(2 * math.pi * t / T)) for t in times]
    tracks['tail'] = [qmul(qaxis(X, 0.18 * math.sin(2 * math.pi * t / T + 0.6)), qaxis(Z, -0.1)) for t in times]
    return 'walk', times, tracks


def clip_graze(n=36, T=3.6):
    times = [T * i / n for i in range(n + 1)]
    tracks = {}
    tracks['neck'] = [qaxis(Z, -0.95 + 0.04 * math.sin(2 * math.pi * t / T)) for t in times]     # head down to the grass
    tracks['head'] = [qaxis(Z, -0.25 + 0.06 * math.sin(6 * math.pi * t / T)) for t in times]     # chewing, tearing grass
    tracks['tail'] = [qaxis(X, 0.35 * math.sin(2 * math.pi * t / T * 2)) for t in times]         # flicks
    tracks['legFR'] = [qaxis(Z, 0.12 * max(0.0, math.sin(2 * math.pi * t / T))) for t in times]   # a step forward now and then
    tracks['shinFR'] = [qaxis(Z, 0.2 * max(0.0, math.sin(2 * math.pi * t / T))) for t in times]
    return 'graze', times, tracks


# ------------------------------------------------------------------------------------------------ GLB writer (skinned)
def write_glb(path, v, nrm, uv, f, joints, wts, clips, png):
    pos = np.ascontiguousarray(np.stack([v[:, 0] - A_OFF, v[:, 1], v[:, 2]], 1), np.float32)
    rest = {b[0]: np.array([b[2][0] - A_OFF, b[2][1], b[2][2]], np.float32) for b in BONES}
    blobs, views, accs = [], [], []

    def add(arr, target=None, comp=None, typ=None, minmax=False):
        data = arr.tobytes()
        while len(data) % 4:
            data += b'\0'
        off = sum(len(b) for b in blobs)
        blobs.append(data)
        view = {'buffer': 0, 'byteOffset': off, 'byteLength': arr.nbytes}
        if target:
            view['target'] = target
        views.append(view)
        acc = {'bufferView': len(views) - 1, 'componentType': comp, 'count': len(arr), 'type': typ}
        if minmax:
            acc['min'] = arr.min(0).tolist() if arr.ndim > 1 else [float(arr.min())]
            acc['max'] = arr.max(0).tolist() if arr.ndim > 1 else [float(arr.max())]
        accs.append(acc)
        return len(accs) - 1

    FLOAT, U16, U32 = 5126, 5123, 5125
    a_pos = add(pos, 34962, FLOAT, 'VEC3', True)
    a_nrm = add(np.ascontiguousarray(nrm, np.float32), 34962, FLOAT, 'VEC3')
    a_uv = add(np.ascontiguousarray(uv, np.float32), 34962, FLOAT, 'VEC2')
    a_jnt = add(np.ascontiguousarray(joints, np.uint16), 34962, U16, 'VEC4')
    a_wgt = add(np.ascontiguousarray(wts, np.float32), 34962, FLOAT, 'VEC4')
    a_idx = add(np.ascontiguousarray(f.reshape(-1), np.uint32), 34963, U32, 'SCALAR')
    nodes = []
    for name, parent, _ in BONES:
        t = rest[name] - (rest[parent] if parent else 0)
        nodes.append({'name': name, 'translation': [float(x) for x in t], 'children': []})
    for i, (name, parent, _) in enumerate(BONES):
        if parent:
            nodes[BI[parent]]['children'].append(i)
    for n_ in nodes:
        if not n_['children']:
            del n_['children']
    ibm = []
    for name, _, _ in BONES:
        m = np.eye(4, dtype=np.float32)
        m[:3, 3] = -rest[name]
        ibm.append(m.T.reshape(-1))      # column-major
    a_ibm = add(np.ascontiguousarray(np.array(ibm, np.float32)), None, FLOAT, 'MAT4')
    img = png
    while len(img) % 4:
        img += bytes(1)
    blobs.append(img)
    views.append({'buffer': 0, 'byteOffset': sum(len(b) for b in blobs[:-1]), 'byteLength': len(png)})
    img_view = len(views) - 1
    mesh_node = len(nodes)
    nodes.append({'name': 'cow', 'mesh': 0, 'skin': 0})
    anims = []
    for name, times, tracks in clips:
        a_t = add(np.array(times, np.float32), None, FLOAT, 'SCALAR', True)
        samplers, channels = [], []
        for bone, qs in tracks.items():
            a_q = add(np.array(qs, np.float32), None, FLOAT, 'VEC4')
            samplers.append({'input': a_t, 'output': a_q, 'interpolation': 'LINEAR'})
            channels.append({'sampler': len(samplers) - 1, 'target': {'node': BI[bone], 'path': 'rotation'}})
        anims.append({'name': name, 'samplers': samplers, 'channels': channels})
    gl = {
        'asset': {'version': '2.0', 'generator': 'cityengine gen_cattle.py'},
        'scene': 0, 'scenes': [{'nodes': [0, mesh_node]}],
        'nodes': nodes,
        'meshes': [{'name': 'cow', 'primitives': [{'attributes': {'POSITION': a_pos, 'NORMAL': a_nrm, 'TEXCOORD_0': a_uv, 'JOINTS_0': a_jnt, 'WEIGHTS_0': a_wgt},
                                                    'indices': a_idx, 'material': 0}]}],
        'materials': [{'name': 'cow', 'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicFactor': 0.0, 'roughnessFactor': 0.85}}],
        'textures': [{'source': 0, 'sampler': 0}], 'samplers': [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 33071, 'wrapT': 33071}],
        'images': [{'bufferView': img_view, 'mimeType': 'image/png'}],
        'skins': [{'joints': list(range(len(BONES))), 'inverseBindMatrices': a_ibm, 'skeleton': 0}],
        'animations': anims,
        'buffers': [{'byteLength': sum(len(b) for b in blobs)}],
        'bufferViews': views, 'accessors': accs,
    }
    js = json.dumps(gl, separators=(',', ':')).encode()
    while len(js) % 4:
        js += b' '
    bin_ = b''.join(blobs)
    with open(path, 'wb') as fh:
        fh.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)))
        fh.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
        fh.write(struct.pack('<II', len(bin_), 0x004E4942) + bin_)


# the SPOM LOD volumes' side paint doubles as the rig's texture: u along the body, v up, both flanks share it
SPOM = os.path.join(ROOT, 'assets', 'industry', 'spom')
TEXFOR = {'holstein': 'cow_hol', 'angus': 'cow_angus_g', 'hereford': 'cow_here'}
VOL_A0, VOL_L, VOL_H = -0.18, 2.95, 1.75


def texture_png(breed):
    from PIL import Image
    import io
    im = Image.open(os.path.join(SPOM, TEXFOR[breed] + '_a.png')).convert('RGB')
    # fill the empty (black) texels from their neighbours so filtering at the silhouette never pulls in black
    a = np.asarray(im, np.float32)
    empty = a.sum(2) < 6
    for _ in range(6):
        sh = [np.roll(a, k, ax) for ax in (0, 1) for k in (-1, 1)]
        se = [np.roll(empty, k, ax) for ax in (0, 1) for k in (-1, 1)]
        acc = sum(np.where(~e[..., None], s, 0) for s, e in zip(sh, se))
        cnt = sum((~e).astype(np.float32) for e in se)[..., None]
        fill = empty & (cnt[..., 0] > 0)
        a[fill] = (acc / np.maximum(cnt, 1))[fill]
        empty = empty & ~fill
    b = io.BytesIO()
    Image.fromarray(a.clip(0, 255).astype(np.uint8)).save(b, 'PNG', optimize=True)
    return b.getvalue()


def write_rig(base, v, nrm, uv, f, joints, wts, clips, png):
    """the engine's copy (src/city_industry.inl CowRig): <base>.cowbin + <base>.png, little endian.
    'COW1', nv, ni, nb, nclips; bones: parent (int32, -1 root), rest position xyz; vertices: pos3 nrm3 uv2 joints4 weights4
    (float32); indices (uint32); clips: frames, duration, then frames x bones quaternions xyzw (identity where not animated)."""
    pos = np.stack([v[:, 0] - A_OFF, v[:, 1], v[:, 2]], 1)
    with open(base + '.cowbin', 'wb') as fh:
        fh.write(b'COW1' + struct.pack('<4i', len(v), f.size, len(BONES), len(clips)))
        for name, parent, p in BONES:
            fh.write(struct.pack('<i3f', BI[parent] if parent else -1, p[0] - A_OFF, p[1], p[2]))
        vt = np.hstack([pos, nrm, uv, joints.astype(np.float32), wts]).astype(np.float32)
        fh.write(vt.tobytes())
        fh.write(f.astype(np.uint32).reshape(-1).tobytes())
        for name, times, tracks in clips:
            fh.write(struct.pack('<if', len(times), times[-1]))
            q = np.tile(np.array([0, 0, 0, 1], np.float32), (len(times), len(BONES), 1))
            for bone, qs in tracks.items():
                q[:, BI[bone]] = np.array(qs, np.float32)
            fh.write(q.astype(np.float32).tobytes())
    open(base + '.png', 'wb').write(png)


def main():
    os.makedirs(OUT, exist_ok=True)
    meshes = {}
    for breed in ('holstein', 'angus', 'hereford'):
        udder = breed == 'holstein'
        if udder not in meshes:
            meshes[udder] = build_mesh(udder)
        v, f = meshes[udder]
        _, part = cow_sdf((v[:, 0], v[:, 1], v[:, 2]), False, stance=False)[:2]
        nrm = vertex_normals_from_field((v[:, 0], v[:, 1], v[:, 2]))
        joints, wts = weights(v, part)
        uv = np.stack([(v[:, 0] - VOL_A0) / VOL_L, 1.0 - v[:, 1] / VOL_H], 1).astype(np.float32)
        path = os.path.join(OUT, 'COW_%s.glb' % breed)
        clips = [clip_walk(), clip_graze()]
        png = texture_png(breed)
        write_glb(path, v, nrm, uv, f, joints, wts, clips, png)
        write_rig(path[:-4], v, nrm, uv, f, joints, wts, clips, png)
        print(os.path.basename(path), len(f), 'tris', len(v), 'verts', len(BONES), 'bones')


if __name__ == '__main__':
    main()
