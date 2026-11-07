"""Rigs the low-poly MakeHuman body to the Mixamo skeleton and converts Mixamo FBX clips to glTF.

  python fbx_to_human_glb.py <out_dir> <clips_dir> [mesh.obj]

Outputs (out_dir):
  human_0..11.glb      skinned pedestrian, 12 looks (clothes, hair, shoes), each with the idle clip
  anim_<name>.glb      the same rig + one clip each (the engine reads a clip's tracks by joint name)
Joint names stay "mixamorig:*". Root motion lives on mixamorig:Hips (strip its x/z when playing).
The mesh is posed A -> T first (Mixamo's rest pose is a T), then skinned by distance to the bone segments.
"""
import json, math, os, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbxparse as F

TICKS = 46186158000.0
FPS = 30.0
CLIPS = {  # file stem in Downloads -> output name
    "Walking": "walk", "Walk W_ Briefcase": "walk_briefcase", "Happy Idle": "idle_happy", "Sad Idle": "idle_sad",
    "Standing W_Briefcase Idle": "idle_briefcase", "Standing Up": "stand_up", "Sit To Stand": "sit_to_stand",
    "Male Sitting Pose": "sit",
}
IDLE = "idle_happy"


def qmul(a, b):
    x1, y1, z1, w1 = a; x2, y2, z2, w2 = b
    return np.array([w1*x2 + x1*w2 + y1*z2 - z1*y2, w1*y2 - x1*z2 + y1*w2 + z1*x2,
                     w1*z2 + x1*y2 - y1*x2 + z1*w2, w1*w2 - x1*x2 - y1*y2 - z1*z2])


def qaxis(axis, deg):
    h = math.radians(deg) / 2
    v = np.zeros(4); v[axis] = math.sin(h); v[3] = math.cos(h)
    return v


def euler_xyz(d):  # FBX eEulerXYZ: X applied first -> R = Rz * Ry * Rx
    return qmul(qaxis(2, d[2]), qmul(qaxis(1, d[1]), qaxis(0, d[0])))


def qmat(q):
    x, y, z, w = q
    return np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                     [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                     [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])


def prop(node, name, default):
    p = node.first("Properties70")
    if p:
        for c in p.children:
            if c.props and c.props[0] == name:
                return list(c.props[4:])
    return default


class Rig:
    def __init__(self, fbx_path):
        root, _ = F.load(fbx_path)
        objs = root.first("Objects")
        self.models = {}
        for c in objs.children:
            if c.name == "Model" and c.props[2] in ("LimbNode", "Root"):
                nm = c.props[1].split("\x00")[0]
                self.models[c.props[0]] = dict(id=c.props[0], name=nm, node=c)
        self.parent = {}
        for c in root.first("Connections").children:
            if c.props[0] == "OO" and c.props[1] in self.models and c.props[2] in self.models:
                self.parent[c.props[1]] = c.props[2]
        order = []
        def visit(i):
            order.append(i)
            for k, p in self.parent.items():
                if p == i: visit(k)
        for i in self.models:
            if i not in self.parent: visit(i)
        self.order = order
        self.index = {i: n for n, i in enumerate(order)}
        self.names = [self.models[i]["name"] for i in order]
        self.par = [self.index[self.parent[i]] if i in self.parent else -1 for i in order]
        self.rest_t, self.rest_q = [], []
        for i in order:
            n = self.models[i]["node"]
            t = np.array(prop(n, "Lcl Translation", [0, 0, 0]), float)
            pre = prop(n, "PreRotation", [0, 0, 0]); rot = prop(n, "Lcl Rotation", [0, 0, 0])
            self.rest_t.append(t * 0.01)
            self.rest_q.append(qmul(euler_xyz(pre), euler_xyz(rot)))
            self.models[i]["pre"] = pre

    def scaled(self, s):
        self.rest_t = [t * s for t in self.rest_t]
        self.scale = s

    def world(self):
        W = []
        for j in range(len(self.names)):
            m = np.eye(4); m[:3, :3] = qmat(self.rest_q[j]); m[:3, 3] = self.rest_t[j]
            W.append(W[self.par[j]] @ m if self.par[j] >= 0 else m)
        return W


def read_clip(path, rig):
    root, _ = F.load(path)
    objs = root.first("Objects")
    curves = {c.props[0]: c for c in objs.children if c.name == "AnimationCurve"}
    nodes = {c.props[0]: c for c in objs.children if c.name == "AnimationCurveNode"}
    own = {c.props[0]: c.props[1].split("\x00")[0] for c in objs.children if c.name == "Model"}
    ownnode = {c.props[0]: c for c in objs.children if c.name == "Model"}
    ridx = {n: j for j, n in enumerate(rig.names)}
    chan = {}  # (model id, "T"/"R") -> {axis: (times, values)}
    ncurve = {}
    for c in root.first("Connections").children:
        p = c.props
        if p[0] == "OP" and p[1] in curves and p[2] in nodes:
            ncurve.setdefault(p[2], {})[p[3][-1]] = p[1]
    for c in root.first("Connections").children:
        p = c.props
        if p[0] == "OP" and p[1] in nodes and p[2] in own and own[p[2]] in ridx and p[3] in ("Lcl Translation", "Lcl Rotation"):
            kind = "T" if p[3] == "Lcl Translation" else "R"
            d = {a: float(next((x.props[4] for x in nodes[p[1]].first("Properties70").children if x.props[0] == "d|" + a), 0)) for a in "XYZ"}
            ax = {}
            for a in "XYZ":
                cid = ncurve.get(p[1], {}).get(a)
                if cid is None:
                    ax[a] = ([0.0], [d[a]])
                else:
                    cv = curves[cid]
                    ax[a] = ([t / TICKS for t in cv.first("KeyTime").props[0]], cv.first("KeyValueFloat").props[0])
            chan[(p[2], kind)] = ax
    dur = max([max(ax[a][0]) for ax in chan.values() for a in "XYZ"] + [1 / FPS])
    n = max(2, int(round(dur * FPS)) + 1)
    times = [i / FPS for i in range(n)]
    def samp(ax, t):
        return [float(np.interp(t, ax[a][0], ax[a][1])) for a in "XYZ"]
    tr, rt = {}, {}
    for (mid, kind), ax in chan.items():
        j = ridx[own[mid]]
        if kind == "T":
            tr[j] = np.array([[v * 0.01 * rig.scale for v in samp(ax, t)] for t in times])
        else:
            pre = prop(ownnode[mid], "PreRotation", [0, 0, 0])
            qs, prev = [], None
            for t in times:
                q = qmul(euler_xyz(pre), euler_xyz(samp(ax, t)))
                if prev is not None and np.dot(prev, q) < 0: q = -q
                qs.append(q); prev = q
            rt[j] = np.array(qs)
    return times, tr, rt


# ---------------------------------------------------------------------------------------------------- mesh
def load_obj(path):
    v, f = [], []
    for l in open(path):
        if l.startswith("v "): v.append([float(x) for x in l.split()[1:4]])
        elif l.startswith("f "):
            idx = [int(p.split("/")[0]) - 1 for p in l.split()[1:]]
            for k in range(1, len(idx) - 1): f.append([idx[0], idx[k], idx[k + 1]])
    return np.array(v), np.array(f)


def rot_about(p, pivot, ang):  # rotate in the XY plane
    c, s = math.cos(ang), math.sin(ang)
    d = p - pivot
    return pivot + np.array([c * d[0] - s * d[1], s * d[0] + c * d[1], d[2]])


def seg_dist(p, a, b):
    ab = b - a; t = np.clip(np.dot(p - a, ab) / max(np.dot(ab, ab), 1e-9), 0, 1)
    return np.linalg.norm(p - (a + ab * t)), t


def fit_mesh(rig, v, W):
    """Face +Z, stand on the floor, pose the arms to the T, return (T-pose verts, joint skin weights)."""
    v = v.copy(); v[:, 0] *= -1; v[:, 2] *= -1          # the OBJ faces -Z
    v[:, 1] -= v[:, 1].min()
    pos = [m[:3, 3] for m in W]
    nm = {n: i for i, n in enumerate(rig.names)}
    # arms: A -> T about the shoulder (Arm joint), by the angle the arm axis (shoulder -> hand tip) is below the T axis
    out = v.copy()
    for side in ("Left", "Right"):
        arm = pos[nm["mixamorig:%sArm" % side]]; hand = pos[nm["mixamorig:%sHand" % side]]
        sx = 1 if arm[0] > 0 else -1
        m = v[:, 0] * sx > 0
        tip = v[m][np.argmax(v[m][:, 0] * sx)]
        sh = arm.copy()
        axis_a = tip - sh; axis_t = hand - arm
        ang = math.atan2(axis_t[1], axis_t[0]) - math.atan2(axis_a[1], axis_a[0])
        for i, p in enumerate(v):
            if not m[i]: continue
            w = float(np.clip((p[0] * sx - 0.2) / 0.07, 0, 1)) * (1.0 if p[1] > 0.75 else 0.0)
            out[i] = rot_about(p, sh, ang * w)
    return out


def skin(rig, W, v):
    nm = {n: i for i, n in enumerate(rig.names)}
    pos = [m[:3, 3] for m in W]
    P = "mixamorig:"
    segs = []  # (joint, a, b)
    def S(j, a, b): segs.append((nm[P + j], pos[nm[P + a]], pos[nm[P + b]]))
    S("Hips", "Hips", "Spine"); S("Spine", "Spine", "Spine1"); S("Spine1", "Spine1", "Spine2"); S("Spine2", "Spine2", "Neck")
    S("Neck", "Neck", "Head"); S("Head", "Head", "HeadTop_End")
    for s in ("Left", "Right"):
        S(s + "Shoulder", s + "Shoulder", s + "Arm"); S(s + "Arm", s + "Arm", s + "ForeArm"); S(s + "ForeArm", s + "ForeArm", s + "Hand")
        S(s + "Hand", s + "Hand", s + "HandMiddle3")
        S(s + "UpLeg", s + "UpLeg", s + "Leg"); S(s + "Leg", s + "Leg", s + "Foot"); S(s + "Foot", s + "Foot", s + "ToeBase")
        S(s + "ToeBase", s + "ToeBase", s + "Toe_End")
    J = np.zeros((len(v), 4), int); Wt = np.zeros((len(v), 4))
    for i, p in enumerate(v):
        d = np.array([seg_dist(p, a, b)[0] for _, a, b in segs])
        w = 1.0 / (d + 0.015) ** 4
        top = np.argsort(-w)[:4]
        ws = w[top] / w[top].sum()
        for k, t in enumerate(top): J[i, k] = segs[t][0]; Wt[i, k] = ws[k]
    return J, Wt


def normals(v, f):
    n = np.zeros_like(v)
    for a, b, c in f:
        fn = np.cross(v[b] - v[a], v[c] - v[a]); n[a] += fn; n[b] += fn; n[c] += fn
    l = np.linalg.norm(n, axis=1, keepdims=True); l[l == 0] = 1
    return n / l


def colors(rig, J, Wt, v, look):
    """Vertex-colour clothing for one look: sleeves, shorts / trousers, shoes, hair style, skin tone."""
    skin_c = np.array(look["skin"]); shirt = np.array(look["shirt"]); pants = np.array(look["pants"])
    shoe = np.array(look["shoe"]); hair = np.array(look["hair"])
    group = {}
    for j, n in enumerate(rig.names):
        n = n.replace("mixamorig:", "")
        group[j] = ("head" if n in ("Head", "Neck") else "hand" if "Hand" in n else "foot" if "Foot" in n or "Toe" in n else
                    "leg" if "Leg" in n or n == "Hips" else "arm" if n in ("LeftForeArm", "RightForeArm") else "torso")
    top_y = v[:, 1].max()
    out = np.zeros((len(v), 4)); out[:, 3] = 1
    for i in range(len(v)):
        g = group[J[i, int(np.argmax(Wt[i]))]]
        x, y, z = v[i]
        c = {"head": skin_c, "hand": skin_c, "foot": shoe, "leg": pants, "arm": shirt * 0.95, "torso": shirt}[g]
        if g == "torso" and y > top_y - 0.30 and abs(x) < 0.10:
            c = shirt if look["collar"] else skin_c                     # neckline
        if g == "arm" and look["sleeve"] == "short": c = skin_c      # forearm bare (short sleeves)
        if g == "torso" and look["sleeve"] == "short" and abs(x) > 0.36: c = skin_c  # upper arm: sleeve to mid-arm
        if g == "leg" and look["legs"] == "shorts" and y < 0.50: c = skin_c
        if g == "leg" and look["legs"] == "capri" and y < 0.30: c = skin_c
        if g in ("foot", "leg") and look["shoe_h"] > 0 and y < look["shoe_h"]: c = shoe   # boots rise up the shin
        if g == "foot" and y > 0.07: c = pants if look["legs"] == "long" else skin_c   # trouser cuff / ankle
        if g == "head":
            if y > top_y - 0.11 and look["hair_style"] != "bald": c = hair                       # the cap
            elif look["hair_style"] == "long" and z < 0.0 and y < top_y - 0.11 and y > top_y - 0.42: c = hair  # falls over the back
            elif look["hair_style"] == "bun" and z < -0.02 and y > top_y - 0.22: c = hair
            elif look["hair_style"] == "short" and z < -0.045 and y > top_y - 0.17: c = hair
        out[i, :3] = c
    return out


def looks():
    skins = [[0.86, 0.66, 0.52], [0.72, 0.52, 0.38], [0.55, 0.38, 0.27], [0.36, 0.24, 0.17], [0.92, 0.76, 0.64], [0.80, 0.60, 0.46]]
    shirts = [[0.25, 0.45, 0.75], [0.75, 0.30, 0.25], [0.88, 0.84, 0.74], [0.30, 0.60, 0.40], [0.92, 0.72, 0.20], [0.20, 0.20, 0.24],
              [0.62, 0.35, 0.65], [0.85, 0.50, 0.25], [0.55, 0.62, 0.68], [0.90, 0.90, 0.92], [0.45, 0.28, 0.20], [0.78, 0.25, 0.45]]
    pants = [[0.18, 0.20, 0.28], [0.30, 0.27, 0.22], [0.20, 0.28, 0.40], [0.35, 0.30, 0.25], [0.12, 0.12, 0.14], [0.55, 0.50, 0.40],
             [0.25, 0.30, 0.22], [0.40, 0.18, 0.18], [0.22, 0.22, 0.26], [0.60, 0.58, 0.52], [0.15, 0.22, 0.34], [0.32, 0.32, 0.36]]
    hairs = [[0.10, 0.07, 0.05], [0.30, 0.20, 0.10], [0.62, 0.45, 0.20], [0.70, 0.70, 0.68], [0.45, 0.18, 0.10], [0.05, 0.05, 0.06]]
    styles = ["short", "long", "bald", "bun", "short", "long", "short", "bun", "bald", "short", "long", "short"]
    sleeves = ["short", "long", "long", "short", "long", "long", "short", "long", "short", "long", "long", "short"]
    legs = ["long", "long", "shorts", "long", "capri", "long", "long", "shorts", "long", "long", "capri", "long"]
    shoes = [[0.12, 0.11, 0.11], [0.85, 0.85, 0.85], [0.35, 0.22, 0.12], [0.8, 0.2, 0.2], [0.12, 0.11, 0.11], [0.3, 0.3, 0.34]]
    boots = [0, 0, 0, 0.22, 0, 0, 0.28, 0, 0, 0, 0.22, 0]
    L = []
    for k in range(12):
        L.append(dict(skin=skins[(k * 5) % 6], shirt=shirts[k], pants=pants[(k * 7 + 3) % 12], hair=hairs[(k * 5 + 1) % 6], hair_style=styles[k],
                      sleeve=sleeves[k], legs=legs[k], shoe=shoes[(k * 5 + 2) % 6], shoe_h=boots[k], collar=(k % 3 != 1)))
    return L


# ---------------------------------------------------------------------------------------------------- glTF
def write_glb(path, rig, W, v, f, J, Wt, col, anim=None, name="human"):
    blob = bytearray(); views = []; accs = []
    def add(arr, ctype, atype, target=None, minmax=False):
        while len(blob) % 4: blob.append(0)
        views.append(dict(buffer=0, byteOffset=len(blob), byteLength=arr.nbytes, **({"target": target} if target else {})))
        blob.extend(arr.tobytes())
        a = dict(bufferView=len(views) - 1, componentType=ctype, count=len(arr), type=atype)
        if minmax: a["min"] = arr.min(0).tolist(); a["max"] = arr.max(0).tolist()
        accs.append(a); return len(accs) - 1
    FLOAT, UINT, USHORT = 5126, 5125, 5123
    pos = add(v.astype("<f4"), FLOAT, "VEC3", 34962, True)
    nor = add(normals(v, f).astype("<f4"), FLOAT, "VEC3", 34962)
    cl = add(col.astype("<f4"), FLOAT, "VEC4", 34962)
    jt = add(J.astype("<u2"), USHORT, "VEC4", 34962)
    wt = add(Wt.astype("<f4"), FLOAT, "VEC4", 34962)
    idx = add(f.astype("<u4").reshape(-1), UINT, "SCALAR", 34963)
    ibm = np.array([np.linalg.inv(m).T.reshape(-1) for m in W], dtype="<f4").reshape(-1, 16)  # column-major
    ibm_a = add(ibm, FLOAT, "MAT4")
    nj = len(rig.names)
    nodes = []
    for j in range(nj):
        q = rig.rest_q[j]
        nodes.append(dict(name=rig.names[j], translation=[float(x) for x in rig.rest_t[j]], rotation=[float(x) for x in q],
                          children=[k for k in range(nj) if rig.par[k] == j]))
    for n in nodes:
        if not n["children"]: del n["children"]
    nodes.append(dict(name=name, mesh=0, skin=0))
    gl = dict(asset=dict(version="2.0", generator="cityengine anim_gen"), scene=0,
              scenes=[dict(nodes=[rig.par.index(-1), nj])], nodes=nodes,
              skins=[dict(joints=list(range(nj)), inverseBindMatrices=ibm_a, skeleton=rig.par.index(-1))],
              meshes=[dict(name=name, primitives=[dict(attributes=dict(POSITION=pos, NORMAL=nor, COLOR_0=cl, JOINTS_0=jt, WEIGHTS_0=wt), indices=idx, material=0)])],
              materials=[dict(name="body", pbrMetallicRoughness=dict(baseColorFactor=[1, 1, 1, 1], metallicFactor=0, roughnessFactor=0.9))])
    if anim:
        times, tr, rt = anim
        tin = add(np.array(times, dtype="<f4"), FLOAT, "SCALAR", None, True)
        samplers, channels = [], []
        for j, arr in tr.items():
            samplers.append(dict(input=tin, output=add(arr.astype("<f4"), FLOAT, "VEC3"), interpolation="LINEAR"))
            channels.append(dict(sampler=len(samplers) - 1, target=dict(node=j, path="translation")))
        for j, arr in rt.items():
            samplers.append(dict(input=tin, output=add(arr.astype("<f4"), FLOAT, "VEC4"), interpolation="LINEAR"))
            channels.append(dict(sampler=len(samplers) - 1, target=dict(node=j, path="rotation")))
        gl["animations"] = [dict(name="clip", samplers=samplers, channels=channels)]
    gl["bufferViews"] = views; gl["accessors"] = accs; gl["buffers"] = [dict(byteLength=len(blob))]
    js = json.dumps(gl, separators=(",", ":")).encode()
    while len(js) % 4: js += b" "
    while len(blob) % 4: blob.append(0)
    with open(path, "wb") as fh:
        fh.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob)))
        fh.write(struct.pack("<II", len(js), 0x4E4F534A)); fh.write(js)
        fh.write(struct.pack("<II", len(blob), 0x004E4942)); fh.write(bytes(blob))


def main():
    out, clips = sys.argv[1], sys.argv[2]
    obj = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), "..", "..", "makehuman_1500_opt.obj")
    os.makedirs(out, exist_ok=True)
    first = os.path.join(clips, "Walking.fbx")
    rig = Rig(first)
    v0, f = load_obj(obj)
    W0 = rig.world()
    top = max(m[1, 3] for m in W0) + 0.0
    ymax = max(m[1, 3] for m in W0)   # HeadTop_End: the crown
    s = (v0[:, 1].max() - v0[:, 1].min()) / ymax
    rig.scaled(s)
    W = rig.world()
    v = fit_mesh(rig, v0, W)
    J, Wt = skin(rig, W, v)
    print("rig: %d joints, scale %.3f, mesh %d verts / %d tris" % (len(rig.names), s, len(v), len(f)))
    LOOKS = looks()
    idle = None
    for stem, nm in CLIPS.items():
        p = os.path.join(clips, stem + ".fbx")
        if not os.path.exists(p): print("missing", p); continue
        a = read_clip(p, rig)
        col = colors(rig, J, Wt, v, LOOKS[0])
        write_glb(os.path.join(out, "anim_%s.glb" % nm), rig, W, v, f, J, Wt, col, a)
        print("clip %-16s %5.2fs  %d rot tracks" % (nm, a[0][-1], len(a[2])))
        if nm == IDLE: idle = a
        if nm == "sit_to_stand":  # the same clip run backwards sits down; its first frame held is the seated idle
            times, tr, rt = a
            rev = (times, {j: x[::-1].copy() for j, x in tr.items()}, {j: x[::-1].copy() for j, x in rt.items()})
            write_glb(os.path.join(out, "anim_sit_down.glb"), rig, W, v, f, J, Wt, col, rev)
            hold = ([0.0, 1.0], {j: np.stack([x[0], x[0]]) for j, x in tr.items()}, {j: np.stack([x[0], x[0]]) for j, x in rt.items()})
            write_glb(os.path.join(out, "anim_sit_hold.glb"), rig, W, v, f, J, Wt, col, hold)
            print("clip sit_down (reversed sit_to_stand), sit_hold")
    for k, look in enumerate(LOOKS):
        write_glb(os.path.join(out, "human_%d.glb" % k), rig, W, v, f, J, Wt, colors(rig, J, Wt, v, look), idle)
    print("wrote", out)


if __name__ == "__main__":
    main()
