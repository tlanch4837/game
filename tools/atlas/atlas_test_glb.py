"""Writes a test model for the building atlas branch (docs/building_atlas.md): boxes side by side, textures embedded.

    python tools/atlas/atlas_test_glb.py <out.glb> <out.tbdscene>

  x = -10  "wall_ref"       the residential L2 wall through the ordinary material path (its own albedo and normal)
  x =   0  "atlas" cell 5   the same wall from the bld_residential atlas page            -> should match the box to its left
  x = +10  "atlas" cell 11  the L4 wall, tinted (0.75, 0.85, 1.0)
  x = +20  "atlas" cell 1   the L1 roof on all faces, uv scaled 3x: tiling across many seams
Every face's uv runs in tiles (metres / 4), unwrapped, as a generator must write it.
"""
import json
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def box(x0, y0, z0, sx, sy, sz, uv_scale, color):
    """24 vertices: position, normal, uv in tiles (4 m), rgba. Returns (verts, indices)."""
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)), ((0, 0, 1), (1, 0, 0), (0, 1, 0)),
             ((0, 0, -1), (-1, 0, 0), (0, 1, 0)), ((0, 1, 0), (1, 0, 0), (0, 0, -1))]
    c = (x0 + sx / 2, y0 + sy / 2, z0 + sz / 2)
    half = (sx / 2, sy / 2, sz / 2)
    verts, idx = [], []
    for n, u, v in faces:
        ext = lambda d: abs(d[0]) * half[0] + abs(d[1]) * half[1] + abs(d[2]) * half[2]
        cu, cv, cn = ext(u), ext(v), ext(n)
        base = len(verts)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = tuple(c[k] + n[k] * cn + u[k] * su * cu + v[k] * sv * cv for k in range(3))
            tu = (su * cu + cu) / 4.0 * uv_scale
            tv = (cv - sv * cv) / 4.0 * uv_scale  # glTF v runs down the image
            verts.append((p, n, (tu, tv), color))
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return verts, idx


def write(out_glb, prims, images, materials):
    bin_ = bytearray()
    views, accessors = [], []

    def view(data, target=None):
        while len(bin_) % 4:
            bin_.append(0)
        views.append({"buffer": 0, "byteOffset": len(bin_), "byteLength": len(data), **({"target": target} if target else {})})
        bin_.extend(data)
        return len(views) - 1

    def acc(data, comp, count, typ, mn=None, mx=None, target=34962):
        a = {"bufferView": view(data, target), "componentType": comp, "count": count, "type": typ}
        if mn is not None:
            a["min"], a["max"] = mn, mx
        accessors.append(a)
        return len(accessors) - 1

    gl_prims = []
    for verts, idx, mat in prims:
        pos = b"".join(struct.pack("<3f", *v[0]) for v in verts)
        nrm = b"".join(struct.pack("<3f", *v[1]) for v in verts)
        uv = b"".join(struct.pack("<2f", *v[2]) for v in verts)
        col = b"".join(struct.pack("<4f", *v[3]) for v in verts)
        mn = [min(v[0][k] for v in verts) for k in range(3)]
        mx = [max(v[0][k] for v in verts) for k in range(3)]
        gl_prims.append({"attributes": {"POSITION": acc(pos, 5126, len(verts), "VEC3", mn, mx), "NORMAL": acc(nrm, 5126, len(verts), "VEC3"),
                                        "TEXCOORD_0": acc(uv, 5126, len(verts), "VEC2"), "COLOR_0": acc(col, 5126, len(verts), "VEC4")},
                         "indices": acc(struct.pack(f"<{len(idx)}I", *idx), 5125, len(idx), "SCALAR", target=34963), "material": mat})
    gl_images = []
    for path in images:
        data = open(path, "rb").read()
        gl_images.append({"bufferView": view(data), "mimeType": "image/png" if path.endswith(".png") else "image/jpeg"})
    while len(bin_) % 4:
        bin_.append(0)
    doc = {"asset": {"version": "2.0", "generator": "tools/atlas/atlas_test_glb.py"}, "scene": 0, "scenes": [{"nodes": [0]}],
           "nodes": [{"mesh": 0}], "meshes": [{"primitives": gl_prims}], "materials": materials,
           "textures": [{"source": i} for i in range(len(images))], "images": gl_images,
           "buffers": [{"byteLength": len(bin_)}], "bufferViews": views, "accessors": accessors}
    js = json.dumps(doc).encode()
    js += b" " * ((4 - len(js) % 4) % 4)
    with open(out_glb, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(bin_), 0x004E4942) + bin_)


def main():
    out_glb, out_scene = sys.argv[1], sys.argv[2]
    table = json.load(open(os.path.join(ROOT, "assets/buildings/atlas/building_atlas.json")))
    cell = lambda name: table["cells"][name]["alpha"]
    tex = os.path.join(ROOT, "assets/buildings/low_density/tex")
    atlas = os.path.join(ROOT, "assets/buildings/atlas")
    images = [f"{tex}/L2_wall_albedo.jpg", f"{tex}/L2_wall_normal.png", f"{atlas}/bld_residential_albedo.png", f"{atlas}/bld_residential_normal.png"]
    materials = [{"name": "wall_ref", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "roughnessFactor": 0.85, "metallicFactor": 0.0},
                  "normalTexture": {"index": 1}},
                 {"name": "atlas_bld_residential", "pbrMetallicRoughness": {"baseColorTexture": {"index": 2}, "roughnessFactor": 0.85, "metallicFactor": 0.0},
                  "normalTexture": {"index": 3}}]
    prims = []
    for x, mat, uvs, color in [(-10, 0, 1, (1, 1, 1, 1)), (0, 1, 1, (1, 1, 1, cell("LD_L2_wall"))),
                               (10, 1, 1, (0.75, 0.85, 1.0, cell("LD_L4_wall"))), (20, 1, 3, (1, 1, 1, cell("LD_L1_roof")))]:
        v, i = box(x - 4, 0, -4, 8, 7, 8, uvs, color)
        prims.append((v, i, mat))
    write(out_glb, prims, images, materials)
    with open(out_scene, "w", newline="\n") as f:
        f.write("TBDSCENE 1\nT|0|1|0|0\n")
        f.write(f"E|atlas_test|Imported|0 0 0|0 0 0 1|1 1 1|1 1 1|{os.path.abspath(out_glb)}\n")
    print("wrote", out_glb, os.path.getsize(out_glb), "bytes and", out_scene)


if __name__ == "__main__":
    main()
