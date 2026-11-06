#!/usr/bin/env python3
"""
Procedural Low-Poly Tower Crane Generator
Outputs 'crane.obj' and 'crane.mtl' with materials and UV/normals.
Real-world dimensions in meters, Y-up, centered at base origin.
Triangles kept under 5,000.
"""

import math

class MeshBuilder:
    def __init__(self):
        self.vertices = []
        self.normals = []
        self.faces = []  # list of (material_name, [list of (v_idx, vn_idx)])
        self.current_material = "yellow_steel"
        self.spom = []  # silhouette-POM facades (see add_spom_facade)

    def set_material(self, mat_name):
        self.current_material = mat_name

    def add_vertex(self, x, y, z):
        self.vertices.append((float(x), float(y), float(z)))
        return len(self.vertices)

    def add_normal(self, nx, ny, nz):
        length = math.sqrt(nx*nx + ny*ny + nz*nz)
        if length > 1e-6:
            nx /= length
            ny /= length
            nz /= length
        else:
            nx, ny, nz = 0.0, 1.0, 0.0
        self.normals.append((nx, ny, nz))
        return len(self.normals)

    def add_quad(self, v1, v2, v3, v4, n):
        # 1-based indexing for OBJ
        self.faces.append((self.current_material, [(v1, n), (v2, n), (v3, n)]))
        self.faces.append((self.current_material, [(v1, n), (v3, n), (v4, n)]))

    def add_box(self, cx, cy, cz, sx, sy, sz):
        hx, hy, hz = sx * 0.5, sy * 0.5, sz * 0.5
        v = [
            self.add_vertex(cx - hx, cy - hy, cz - hz),  # 0: ---
            self.add_vertex(cx + hx, cy - hy, cz - hz),  # 1: +--
            self.add_vertex(cx + hx, cy + hy, cz - hz),  # 2: ++-
            self.add_vertex(cx - hx, cy + hy, cz - hz),  # 3: -+-
            self.add_vertex(cx - hx, cy - hy, cz + hz),  # 4: --+
            self.add_vertex(cx + hx, cy - hy, cz + hz),  # 5: +-+
            self.add_vertex(cx + hx, cy + hy, cz + hz),  # 6: +++
            self.add_vertex(cx - hx, cy + hy, cz + hz),  # 7: -++
        ]

        # Normals
        n_neg_z = self.add_normal(0, 0, -1)
        n_pos_z = self.add_normal(0, 0, 1)
        n_neg_x = self.add_normal(-1, 0, 0)
        n_pos_x = self.add_normal(1, 0, 0)
        n_neg_y = self.add_normal(0, -1, 0)
        n_pos_y = self.add_normal(0, 1, 0)

        # Faces
        self.add_quad(v[0], v[3], v[2], v[1], n_neg_z)  # Front (-Z)
        self.add_quad(v[5], v[6], v[7], v[4], n_pos_z)  # Back (+Z)
        self.add_quad(v[4], v[7], v[3], v[0], n_neg_x)  # Left (-X)
        self.add_quad(v[1], v[2], v[6], v[5], n_pos_x)  # Right (+X)
        self.add_quad(v[4], v[0], v[1], v[5], n_neg_y)  # Bottom (-Y)
        self.add_quad(v[3], v[7], v[6], v[2], n_pos_y)  # Top (+Y)

    def add_strut(self, p1, p2, thickness):
        """Draws a rectangular strut box connecting two 3D points."""
        x1, y1, z1 = p1
        x2, y2, z2 = p2
        dx, dy, dz = x2 - x1, y2 - y1, z2 - z1
        length = math.sqrt(dx*dx + dy*dy + dz*dz)
        if length < 1e-4:
            return

        # Normalized direction vector
        ax, ay, az = dx / length, dy / length, dz / length

        # Arbitrary perpendicular vector
        if abs(ax) < 0.9:
            rx, ry, rz = 1.0, 0.0, 0.0
        else:
            rx, ry, rz = 0.0, 1.0, 0.0

        # Cross product to get u
        ux = ay * rz - az * ry
        uy = az * rx - ax * rz
        uz = ax * ry - ay * rx
        u_len = math.sqrt(ux*ux + uy*uy + uz*uz)
        ux, uy, uz = ux / u_len, uy / u_len, uz / u_len

        # Cross product to get v
        vx = ay * uz - az * uy
        vy = az * ux - ax * uz
        vz = ax * uy - ay * ux

        ht = thickness * 0.5
        v_offsets = [
            (-ux * ht - vx * ht, -uy * ht - vy * ht, -uz * ht - vz * ht),
            ( ux * ht - vx * ht,  uy * ht - vy * ht,  uz * ht - vz * ht),
            ( ux * ht + vx * ht,  uy * ht + vy * ht,  uz * ht + vz * ht),
            (-ux * ht + vx * ht, -uy * ht + vy * ht, -uz * ht + vz * ht),
        ]

        v_start = [self.add_vertex(x1 + ox, y1 + oy, z1 + oz) for ox, oy, oz in v_offsets]
        v_end = [self.add_vertex(x2 + ox, y2 + oy, z2 + oz) for ox, oy, oz in v_offsets]

        # Normals for the 4 sides
        n1 = self.add_normal(-vx, -vy, -vz)
        n2 = self.add_normal(ux, uy, uz)
        n3 = self.add_normal(vx, vy, vz)
        n4 = self.add_normal(-ux, -uy, -uz)

        self.add_quad(v_start[0], v_end[0], v_end[1], v_start[1], n1)
        self.add_quad(v_start[1], v_end[1], v_end[2], v_start[2], n2)
        self.add_quad(v_start[2], v_end[2], v_end[3], v_start[3], n3)
        self.add_quad(v_start[3], v_end[3], v_end[0], v_start[0], n4)

    def add_cylinder(self, cx, cy, cz, radius, height, axis='y', segments=8):
        """Draws a simple open-ended or capped low-poly cylinder along x, y, or z."""
        angles = [2.0 * math.pi * i / segments for i in range(segments)]
        half_h = height * 0.5

        top_v = []
        bot_v = []
        normals = []

        for a in angles:
            cos_a, sin_a = math.cos(a), math.sin(a)
            if axis == 'y':
                px, pz = radius * cos_a, radius * sin_a
                top_v.append(self.add_vertex(cx + px, cy + half_h, cz + pz))
                bot_v.append(self.add_vertex(cx + px, cy - half_h, cz + pz))
                normals.append(self.add_normal(cos_a, 0.0, sin_a))
            elif axis == 'x':
                py, pz = radius * cos_a, radius * sin_a
                top_v.append(self.add_vertex(cx + half_h, cy + py, cz + pz))
                bot_v.append(self.add_vertex(cx - half_h, cy + py, cz + pz))
                normals.append(self.add_normal(0.0, cos_a, sin_a))
            elif axis == 'z':
                px, py = radius * cos_a, radius * sin_a
                top_v.append(self.add_vertex(cx + px, cy + py, cz + half_h))
                bot_v.append(self.add_vertex(cx + px, cy + py, cz - half_h))
                normals.append(self.add_normal(cos_a, sin_a, 0.0))

        # Quad sides
        for i in range(segments):
            nxt = (i + 1) % segments
            n = normals[i]
            self.add_quad(bot_v[i], top_v[i], top_v[nxt], bot_v[nxt], n)

BAR_PLANE = 0.17    # metres out from a facade's wall where the lattice bars are centred (make_lattice_maps.py PLANE)
BOX_DEPTH = 0.34    # a facade box's depth out from its wall (make_lattice_maps.py BOX_DEPTH)

def add_spom_facade(m, name, tex, center, N, W, H, D):
    """A thin box standing out of a wall plane; its truss silhouette comes from the relief texture (SPOM in mesh.hlsl).
    center = middle of the wall rectangle, N = outward facade normal (axis aligned), W along T = cross(up, N), H along up,
    D = box depth out from the wall. All six faces carry N as their normal, as SpomPS expects."""
    up = (0.0, 1.0, 0.0)
    T = (up[1] * N[2] - up[2] * N[1], up[2] * N[0] - up[0] * N[2], up[0] * N[1] - up[1] * N[0])
    def pt(t, b, n):
        return tuple(center[i] + T[i] * t + up[i] * b + N[i] * n for i in range(3))
    corners = {(i, j, k): pt((i - 0.5) * W, (j - 0.5) * H, k * D) for i in (0, 1) for j in (0, 1) for k in (0, 1)}
    quads = []
    for axis in range(3):
        u_ax, v_ax = (axis + 1) % 3, (axis + 2) % 3
        for side in (0, 1):
            q = []
            for du, dv in ((0, 0), (1, 0), (1, 1), (0, 1)):
                idx = [0, 0, 0]
                idx[axis], idx[u_ax], idx[v_ax] = side, du, dv
                q.append(corners[tuple(idx)])
            out = [0.0, 0.0, 0.0]
            out[axis] = 1.0 if side else -1.0
            outward = tuple(T[i] * out[0] + up[i] * out[1] + N[i] * out[2] for i in range(3))
            quads.append((q, outward))
    tris = []
    for q, outward in quads:
        for a, b, c in ((0, 1, 2), (0, 2, 3)):
            pa, pb, pc = q[a], q[b], q[c]
            e1 = [pb[i] - pa[i] for i in range(3)]
            e2 = [pc[i] - pa[i] for i in range(3)]
            cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            if sum(cr[i] * outward[i] for i in range(3)) < 0:
                pb, pc = pc, pb
            tris.append((pa, pb, pc))
    verts = []
    for tri in tris:
        for p in tri:
            d = [p[i] - center[i] for i in range(3)]
            t = sum(d[i] * T[i] for i in range(3))
            b = d[1]
            z = sum(d[i] * N[i] for i in range(3))
            # engine convention (SpomPS): uv = metres from the volume's lower-left on the wall, colour = (W, H, z out from the wall)
            verts.append((p, (t + W * 0.5, b + H * 0.5), z))
    m.spom.append({"name": name, "tex": tex, "N": N, "verts": verts, "W": W, "H": H, "D": D})


def build_crane_mesh():
    m = MeshBuilder()

    # -------------------------------------------------------------
    # 1. FOUNDATION / OUTRIGGER BASE (Origin Y=0)
    # -------------------------------------------------------------
    m.set_material("dark_steel")
    # Concrete ballast feet pads
    pad_offset = 3.2
    for sx in [-1, 1]:
        for sz in [-1, 1]:
            m.set_material("concrete")
            m.add_box(sx * pad_offset, 0.25, sz * pad_offset, 2.0, 0.5, 2.0)
            m.set_material("dark_steel")
            # Diagonal base braces connecting to mast bottom
            m.add_strut((sx * pad_offset, 0.5, sz * pad_offset), (sx * 1.0, 1.2, sz * 1.0), 0.18)

    # Steel cross base beams
    m.set_material("dark_steel")
    m.add_box(0, 0.6, 0, 8.4, 0.35, 0.5)
    m.add_box(0, 0.6, 0, 0.5, 0.35, 8.4)
    # Central support ring plate
    m.add_box(0, 0.9, 0, 2.8, 0.4, 2.8)

    # -------------------------------------------------------------
    # 2. LATTICE MAST (45 m height, 2.0 m x 2.0 m footprint)
    # -------------------------------------------------------------
    m.set_material("yellow_steel")
    mast_w = 2.0
    half_w = mast_w * 0.5
    mast_h = 44.0
    mast_base_y = 1.1
    # The chords, ties and X braces are no longer geometry: four thin SPOM facades carry them as a
    # round-profile relief (tex/lattice_mast.png), so the whole mast is 4 boxes instead of ~1700 triangles.
    wall = half_w - BAR_PLANE      # the bars' centre plane lies on the old chord line
    for name, N, c in (("spom_mast_pz", (0, 0, 1), (0, 0, wall)), ("spom_mast_nz", (0, 0, -1), (0, 0, -wall)),
                       ("spom_mast_px", (1, 0, 0), (wall, 0, 0)), ("spom_mast_nx", (-1, 0, 0), (-wall, 0, 0))):
        for i in range(11):   # 11 bays of 4 m, each its own short volume: SpomPS searches 64 steps per volume, so short ones stay sharp
            add_spom_facade(m, name, "mast", (c[0], mast_base_y + (i + 0.5) * 4.0, c[2]), N, mast_w, 4.0, BOX_DEPTH)

    # -------------------------------------------------------------
    # 3. SLEWING UNIT & OPERATOR CAB
    # -------------------------------------------------------------
    slew_y = mast_base_y + mast_h  # 45.1 m
    m.set_material("dark_steel")
    # Turntable ring & motor housing
    m.add_cylinder(0, slew_y + 0.35, 0, radius=1.35, height=0.7, axis='y', segments=12)
    m.add_box(0, slew_y + 0.85, 0, 2.4, 0.4, 2.4)

    # Operator Cab (mounted on right/front side)
    cab_y = slew_y + 1.2
    cab_x = 1.6
    cab_z = 1.3
    m.set_material("yellow_steel")
    # Cab main enclosure
    m.add_box(cab_x, cab_y + 1.1, cab_z, 1.8, 2.1, 1.5)
    # Walkway platform and railing
    m.set_material("dark_steel")
    m.add_box(cab_x, cab_y - 0.05, cab_z, 2.3, 0.1, 2.0)
    m.add_box(cab_x, cab_y + 0.5, cab_z + 0.95, 2.3, 1.0, 0.05)
    # Glass panoramic window
    m.set_material("glass")
    m.add_box(cab_x + 0.35, cab_y + 1.25, cab_z, 1.15, 1.2, 1.54)

    # -------------------------------------------------------------
    # 4. A-FRAME / TOWER APEX (Cathead)
    # -------------------------------------------------------------
    apex_base_y = slew_y + 1.05
    apex_top_y = apex_base_y + 7.5
    m.set_material("yellow_steel")
    a_leg_t = 0.12
    # Pyramid struts tapering to crown
    m.add_strut((-1.0, apex_base_y, -0.9), (0.0, apex_top_y, 0.0), a_leg_t)
    m.add_strut((-1.0, apex_base_y,  0.9), (0.0, apex_top_y, 0.0), a_leg_t)
    m.add_strut(( 1.0, apex_base_y, -0.9), (0.0, apex_top_y, 0.0), a_leg_t)
    m.add_strut(( 1.0, apex_base_y,  0.9), (0.0, apex_top_y, 0.0), a_leg_t)
    # Crown block cap
    m.set_material("dark_steel")
    m.add_box(0.0, apex_top_y + 0.15, 0.0, 0.6, 0.3, 0.6)

    # -------------------------------------------------------------
    # 5. COUNTER-JIB & COUNTERWEIGHTS (Extends along -X)
    # -------------------------------------------------------------
    cj_len = 15.0
    cj_y = apex_base_y + 0.3
    m.set_material("yellow_steel")
    # Two side trusses as SPOM facades (tex/lattice_cjib.png) plus a thin deck between them
    for name, N, z in (("spom_cjib_pz", (0, 0, 1), 0.7 - BAR_PLANE), ("spom_cjib_nz", (0, 0, -1), -(0.7 - BAR_PLANE))):
        for i in range(3):
            add_spom_facade(m, name, "cjib", (-(i + 0.5) * 5.0, cj_y + 0.3, z), N, 5.0, 1.2, BOX_DEPTH)
    m.add_box(-cj_len * 0.5, cj_y - 0.25, 0.0, cj_len, 0.1, 1.4)
    # Winch hoist drum machinery
    m.set_material("dark_steel")
    m.add_box(-4.5, cj_y + 0.45, 0.0, 1.8, 0.8, 1.1)
    m.add_cylinder(-4.5, cj_y + 0.45, 0.0, radius=0.35, height=1.2, axis='z', segments=8)

    # Concrete Counterweight blocks stacked at end
    m.set_material("concrete")
    cw_count = 5
    for i in range(cw_count):
        cw_x = -cj_len + 1.2 + (i * 0.75)
        m.add_box(cw_x, cj_y + 0.9, 0.0, 0.65, 1.7, 1.8)

    # -------------------------------------------------------------
    # 6. MAIN WORKING JIB (Extends along +X: triangular lattice truss)
    # -------------------------------------------------------------
    m.set_material("yellow_steel")
    jib_len = 48.0
    jib_h = 1.9
    jib_half_w = 0.9
    jib_base_y = cj_y

    # Two side trusses as SPOM facades (tex/lattice_jib.png), a top and a bottom plate close the box
    for name, N, z in (("spom_jib_pz", (0, 0, 1), jib_half_w - BAR_PLANE), ("spom_jib_nz", (0, 0, -1), -(jib_half_w - BAR_PLANE))):
        for i in range(8):
            add_spom_facade(m, name, "jib", ((i + 0.5) * 6.0, jib_base_y + jib_h * 0.5, z), N, 6.0, jib_h, BOX_DEPTH)
    m.add_box(jib_len * 0.5, jib_base_y + jib_h, 0.0, jib_len, 0.1, jib_half_w * 2.0)
    m.add_box(jib_len * 0.5, jib_base_y, 0.0, jib_len, 0.08, jib_half_w * 2.0)

    # -------------------------------------------------------------
    # 7. PENDANT TENSION CABLES (Stay ropes from A-frame apex)
    # -------------------------------------------------------------
    m.set_material("cable")
    apex_pt = (0.0, apex_top_y + 0.2, 0.0)

    # Counter-jib stays
    m.add_strut(apex_pt, (-cj_len + 1.0, cj_y + 0.4, -0.6), 0.035)
    m.add_strut(apex_pt, (-cj_len + 1.0, cj_y + 0.4,  0.6), 0.035)

    # Working jib dual pendant stays (to ~18m and ~35m points on top chord)
    m.add_strut(apex_pt, (18.0, jib_base_y + jib_h, 0.0), 0.04)
    m.add_strut(apex_pt, (35.0, jib_base_y + jib_h, 0.0), 0.04)

    # -------------------------------------------------------------
    # 8. TROLLEY, HOIST CABLES & HOOK BLOCK
    # -------------------------------------------------------------
    # Position trolley midway along jib
    trolley_x = 24.0
    trolley_y = jib_base_y - 0.15

    # Trolley carriage
    m.set_material("dark_steel")
    m.add_box(trolley_x, trolley_y, 0.0, 1.4, 0.25, 1.3)
    # Trolley guide wheels
    m.add_cylinder(trolley_x - 0.4, trolley_y + 0.12, -jib_half_w, 0.12, 0.1, axis='z', segments=8)
    m.add_cylinder(trolley_x + 0.4, trolley_y + 0.12, -jib_half_w, 0.12, 0.1, axis='z', segments=8)
    m.add_cylinder(trolley_x - 0.4, trolley_y + 0.12,  jib_half_w, 0.12, 0.1, axis='z', segments=8)
    m.add_cylinder(trolley_x + 0.4, trolley_y + 0.12,  jib_half_w, 0.12, 0.1, axis='z', segments=8)

    # Pulley Hook Block hanging below
    block_y = trolley_y - 12.0
    m.set_material("cable")
    cable_r = 0.02
    cable_len = 12.0
    # Two drops of hoist cable
    m.add_cylinder(trolley_x, trolley_y - cable_len * 0.5, -0.22, radius=cable_r, height=cable_len, axis='y', segments=6)
    m.add_cylinder(trolley_x, trolley_y - cable_len * 0.5,  0.22, radius=cable_r, height=cable_len, axis='y', segments=6)

    # Sheave block body
    m.set_material("yellow_steel")
    m.add_box(trolley_x, block_y - 0.45, 0.0, 0.7, 0.9, 0.6)
    # Pulley wheel
    m.set_material("dark_steel")
    m.add_cylinder(trolley_x, block_y - 0.35, 0.0, radius=0.28, height=0.65, axis='z', segments=8)

    # Forged Hook
    m.set_material("dark_steel")
    hook_top_y = block_y - 0.9
    # Hook shank
    m.add_cylinder(trolley_x, hook_top_y - 0.25, 0.0, radius=0.08, height=0.5, axis='y', segments=6)
    # Curved hook bowl
    m.add_box(trolley_x + 0.15, hook_top_y - 0.65, 0.0, 0.5, 0.15, 0.15)
    m.add_box(trolley_x + 0.35, hook_top_y - 0.45, 0.0, 0.15, 0.35, 0.15)

    return m

SPOM_MATERIALS = {  # facade material -> relief set tex/lattice_<key>_albedo.png + _normal.png (made by make_lattice_maps.py)
    "spom_mast_pz": "mast", "spom_mast_nz": "mast", "spom_mast_px": "mast", "spom_mast_nx": "mast",
    "spom_cjib_pz": "cjib", "spom_cjib_nz": "cjib", "spom_jib_pz": "jib", "spom_jib_nz": "jib",
}

def write_mtl(filename="crane.mtl"):
    materials = {
        "yellow_steel": {
            "Kd": (0.92, 0.70, 0.08),  # Construction safety yellow
            "Ks": (0.3, 0.3, 0.3),
            "Ns": 40.0,
            "d": 1.0
        },
        "dark_steel": {
            "Kd": (0.18, 0.20, 0.22),  # Matte industrial dark grey
            "Ks": (0.4, 0.4, 0.4),
            "Ns": 50.0,
            "d": 1.0
        },
        "concrete": {
            "Kd": (0.65, 0.63, 0.60),  # Weathered concrete
            "Ks": (0.05, 0.05, 0.05),
            "Ns": 5.0,
            "d": 1.0
        },
        "glass": {
            "Kd": (0.20, 0.45, 0.60),  # Tinted cab glass
            "Ks": (0.9, 0.9, 0.9),
            "Ns": 90.0,
            "d": 0.45
        },
        "cable": {
            "Kd": (0.12, 0.12, 0.12),  # Steel wire rope
            "Ks": (0.2, 0.2, 0.2),
            "Ns": 20.0,
            "d": 1.0
        }
    }

    with open(filename, "w") as f:
        f.write("# Tower Crane Materials\n")
        for name, props in materials.items():
            f.write(f"\nnewmtl {name}\n")
            f.write(f"Ka 0.2 0.2 0.2\n")
            f.write("Kd 1.000 1.000 1.000\n")  # white so the texture isn't tinted
            f.write(f"Ks {props['Ks'][0]:.3f} {props['Ks'][1]:.3f} {props['Ks'][2]:.3f}\n")
            f.write(f"Ns {props['Ns']:.1f}\n")
            f.write(f"d {props['d']:.2f}\n")
            f.write("illum 2\n")
            if name in TEXTURES:
                f.write(f"map_Kd {TEXTURES[name][0]}\n")
        for fa, key in SPOM_MATERIALS.items():  # OBJ shows these as plain boxes: the lattice needs the engine's SPOM shader (GLB)
            f.write(f"\nnewmtl {fa}\nKa 0.2 0.2 0.2\nKd 1.000 1.000 1.000\nKs 0.1 0.1 0.1\nNs 20.0\nd 1.00\nillum 2\n")
            f.write(f"map_Kd tex/lattice_{key}_albedo.png\n")

def write_obj(mesh, filename="crane.obj", mtl_filename="crane.mtl"):
    with open(filename, "w") as f:
        f.write("# Wavefront OBJ - Procedural Tower Crane\n")
        f.write(f"mtllib {mtl_filename}\n\n")

        for vx, vy, vz in mesh.vertices:
            f.write(f"v {vx:.4f} {vy:.4f} {vz:.4f}\n")

        for nx, ny, nz in mesh.normals:
            f.write(f"vn {nx:.4f} {ny:.4f} {nz:.4f}\n")

        current_mat = None
        for mat, face in mesh.faces:
            if mat != current_mat:
                current_mat = mat
                f.write(f"\nusemtl {current_mat}\n")

            indices_str = " ".join([f"{v}//{vn}" for v, vn in face])
            f.write(f"f {indices_str}\n")

# ---------------------------------------------------------------------------
# UVs (box projection in world space) + textures + GLB export
# ---------------------------------------------------------------------------
TEXTURES = {  # material -> (file, metres per texture repeat)
    "yellow_steel": ("tex/yellow_steel.jpg", 3.0),
    "dark_steel": ("tex/dark_steel.jpg", 3.0),
    "concrete": ("tex/concrete.jpg", 2.0),
    "glass": ("tex/glass.jpg", 2.0),
    "cable": ("tex/cable.jpg", 0.6),
}

def box_uv(mesh, v_idx, n_idx, mat):
    x, y, z = mesh.vertices[v_idx - 1]
    nx, ny, nz = mesh.normals[n_idx - 1]
    ax, ay, az = abs(nx), abs(ny), abs(nz)
    if ay >= ax and ay >= az:
        u, v = x, z
    elif ax >= az:
        u, v = z, y
    else:
        u, v = x, y
    s = TEXTURES[mat][1]
    return (u / s, v / s)

def write_obj_uv(mesh, filename="crane.obj", mtl_filename="crane.mtl"):
    with open(filename, "w") as f:
        f.write("# Wavefront OBJ - Procedural Tower Crane (UV mapped)\n")
        f.write(f"mtllib {mtl_filename}\n\n")
        for vx, vy, vz in mesh.vertices:
            f.write(f"v {vx:.4f} {vy:.4f} {vz:.4f}\n")
        for nx, ny, nz in mesh.normals:
            f.write(f"vn {nx:.4f} {ny:.4f} {nz:.4f}\n")
        uvs, lines, cur = [], [], None
        for mat, face in mesh.faces:
            if mat != cur:
                cur = mat
                lines.append(f"usemtl {mat}")
            ids = []
            for v, vn in face:
                uvs.append(box_uv(mesh, v, vn, mat))
                ids.append(f"{v}/{len(uvs)}/{vn}")
            lines.append("f " + " ".join(ids))
        for u, v in uvs:
            f.write(f"vt {u:.4f} {v:.4f}\n")
        f.write("\n" + "\n".join(lines) + "\n")
        # SPOM facades as plain alpha-cut boxes (normalised UVs), so the OBJ still shows the lattice
        vo, no, to = len(mesh.vertices), len(mesh.normals), len(uvs)
        for fa in mesh.spom:
            f.write(f"\nusemtl {fa['name']}\n")
            nx, ny, nz = fa["N"]
            f.write(f"vn {nx:.4f} {ny:.4f} {nz:.4f}\n")
            no += 1
            for (px, py, pz), (u, v), _z in fa["verts"]:
                f.write(f"v {px:.4f} {py:.4f} {pz:.4f}\nvt {u / fa['W']:.4f} {v / fa['H']:.4f}\n")
            for k in range(0, len(fa["verts"]), 3):
                f.write("f " + " ".join(f"{vo + k + j + 1}/{to + k + j + 1}/{no}" for j in range(3)) + "\n")
            vo += len(fa["verts"])
            to += len(fa["verts"])

def m_spom(mesh):
    return mesh.spom

def write_glb(mesh, filename="crane.glb"):
    import json, struct, os
    base = os.path.dirname(os.path.abspath(filename))
    prims = {}  # mat -> (pos, nrm, uv)
    for mat, face in mesh.faces:
        p, n, t = prims.setdefault(mat, ([], [], []))
        for v, vn in face:
            p.extend(mesh.vertices[v - 1])
            n.extend(mesh.normals[vn - 1])
            t.extend(box_uv(mesh, v, vn, mat))
    bin_data = bytearray()
    views, accs, images, textures, materials, gprims = [], [], [], [], [], []

    def add_view(data):
        while len(bin_data) % 4:
            bin_data.append(0)
        views.append({"buffer": 0, "byteOffset": len(bin_data), "byteLength": len(data)})
        bin_data.extend(data)
        return len(views) - 1

    tex_cache = {}

    def get_tex(rel):
        if rel not in tex_cache:
            with open(os.path.join(base, rel), "rb") as imf:
                iv = add_view(imf.read())
            images.append({"bufferView": iv, "mimeType": "image/png" if rel.endswith(".png") else "image/jpeg"})
            textures.append({"source": len(images) - 1})
            tex_cache[rel] = len(textures) - 1
        return tex_cache[rel]

    # SPOM facades (silhouette POM, SpomPS in mesh.hlsl): one primitive + material each, named "spom_*" so the importer tags them.
    # The shader reads the vertex data raw: NORMAL = the facade's outward normal on every face, TEXCOORD_0 = metres from the
    # volume's lower-left on the wall, COLOR_0 = (width, height, z out from the wall). The relief is two-layer: albedo.a = where
    # solid ends, normal.a = where it starts (tools/building_gen/spom.py has the same encoding).
    merged = {}
    for fa in m_spom(mesh):
        f = merged.setdefault(fa["name"], {"name": fa["name"], "tex": fa["tex"], "N": fa["N"], "verts": []})
        f["verts"].extend((pos, uv, z, fa["W"], fa["H"]) for pos, uv, z in fa["verts"])
    for f in merged.values():
        p, n, t, c = [], [], [], []
        for pos, uv, z, fw, fh in f["verts"]:
            p.extend(pos); n.extend(f["N"]); t.extend(uv); c.extend((fw, fh, z))
        count = len(p) // 3
        pv = add_view(struct.pack(f"<{len(p)}f", *p))
        nv = add_view(struct.pack(f"<{len(n)}f", *n))
        tv = add_view(struct.pack(f"<{len(t)}f", *t))
        cv = add_view(struct.pack(f"<{len(c)}f", *c))
        xs, ys, zs = p[0::3], p[1::3], p[2::3]
        a0 = len(accs)
        accs.append({"bufferView": pv, "componentType": 5126, "count": count, "type": "VEC3",
                     "min": [min(xs), min(ys), min(zs)], "max": [max(xs), max(ys), max(zs)]})
        accs.append({"bufferView": nv, "componentType": 5126, "count": count, "type": "VEC3"})
        accs.append({"bufferView": tv, "componentType": 5126, "count": count, "type": "VEC2"})
        accs.append({"bufferView": cv, "componentType": 5126, "count": count, "type": "VEC3"})
        key = f["tex"]
        materials.append({"name": f["name"],
                          "pbrMetallicRoughness": {"baseColorTexture": {"index": get_tex(f"tex/lattice_{key}_albedo.png")},
                                                   "baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0.0, "roughnessFactor": 0.9},
                          "normalTexture": {"index": get_tex(f"tex/lattice_{key}_normal.png")}})
        gprims.append({"attributes": {"POSITION": a0, "NORMAL": a0 + 1, "TEXCOORD_0": a0 + 2, "COLOR_0": a0 + 3},
                       "material": len(materials) - 1})

    for mat, (p, n, t) in prims.items():
        count = len(p) // 3
        pv = add_view(struct.pack(f"<{len(p)}f", *p))
        nv = add_view(struct.pack(f"<{len(n)}f", *n))
        tv = add_view(struct.pack(f"<{len(t)}f", *t))
        xs, ys, zs = p[0::3], p[1::3], p[2::3]
        a0 = len(accs)
        accs.append({"bufferView": pv, "componentType": 5126, "count": count, "type": "VEC3",
                     "min": [min(xs), min(ys), min(zs)], "max": [max(xs), max(ys), max(zs)]})
        accs.append({"bufferView": nv, "componentType": 5126, "count": count, "type": "VEC3"})
        accs.append({"bufferView": tv, "componentType": 5126, "count": count, "type": "VEC2"})
        m = {"name": mat, "pbrMetallicRoughness": {
            "baseColorTexture": {"index": get_tex(TEXTURES[mat][0])},
            "metallicFactor": 0.3 if "steel" in mat else 0.0,
            "roughnessFactor": 0.35 if mat == "glass" else 0.7}}
        if mat == "glass":
            m["alphaMode"] = "BLEND"
            m["pbrMetallicRoughness"]["baseColorFactor"] = [1, 1, 1, 0.45]
        materials.append(m)
        gprims.append({"attributes": {"POSITION": a0, "NORMAL": a0 + 1, "TEXCOORD_0": a0 + 2},
                       "material": len(materials) - 1})
    while len(bin_data) % 4:
        bin_data.append(0)
    gltf = {"asset": {"version": "2.0", "generator": "generate_crane.py"},
            "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": "crane", "mesh": 0}],
            "meshes": [{"name": "crane", "primitives": gprims}],
            "materials": materials, "textures": textures, "images": images,
            "samplers": [], "accessors": accs, "bufferViews": views,
            "buffers": [{"byteLength": len(bin_data)}]}
    gltf["textures"] = [dict(t, sampler=0) for t in textures]
    gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    total = 12 + 8 + len(js) + 8 + len(bin_data)
    with open(filename, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(bin_data), 0x004E4942) + bytes(bin_data))

if __name__ == "__main__":
    print("Building procedural tower crane...")
    mesh = build_crane_mesh()
    tri_count = len(mesh.faces) + sum(len(fa["verts"]) // 3 for fa in mesh.spom)
    vert_count = len(mesh.vertices)

    print(f"Generated {vert_count} vertices and {tri_count} triangles.")
    assert tri_count < 5000, f"Triangle budget exceeded: {tri_count} >= 5000"

    write_mtl("crane.mtl")
    write_obj_uv(mesh, "crane.obj", "crane.mtl")
    write_glb(mesh, "crane.glb")
    print("Exported crane.obj, crane.mtl and crane.glb successfully.")
