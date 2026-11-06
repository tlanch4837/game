"""Low-poly but realistic-looking tree: geometry trunk/branches + alpha leaf cards.

Outputs lowpoly_tree.obj/.mtl, leaves.png (RGBA), bark.png, preview.html.
Leaf-card normals are pointed outward from the crown centre (standard game trick)
so the canopy shades like one soft volume instead of flat planes.
"""
import math, random, json, pathlib, sys
from PIL import Image, ImageDraw, ImageFilter

random.seed(11)
HI = '--hi' in sys.argv          # high-detail close-up version, same layout
rng2 = random.Random(99)         # extra detail uses its own RNG so the shared layout stays identical
SFX = '_hi' if HI else ''
OUT = pathlib.Path(__file__).parent
V, VT, VN, F = [], [], [], []          # F: (mat, [(v,vt,vn)*3])

def v(p): V.append(p); return len(V)
def vt(p): VT.append(p); return len(VT)
def vn(p):
    l = math.sqrt(sum(c*c for c in p)) or 1; VN.append(tuple(c/l for c in p)); return len(VN)
def sub(a,b): return tuple(x-y for x,y in zip(a,b))
def add(a,b): return tuple(x+y for x,y in zip(a,b))
def mul(a,s): return tuple(x*s for x in a)
def cross(a,b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def norm(a): l=math.sqrt(sum(c*c for c in a)) or 1; return tuple(c/l for c in a)

# ---------- wood: tapered 5-sided tubes, smooth normals, open ends -------------
def tube(p0, p1, r0, r1, sides=5, vrep=2.0, flare=0.0, hi_sides=None, hi_segs=6):
    axis = norm(sub(p1,p0))
    ref = (0,0,1) if abs(axis[1]) > .9 else (0,1,0)
    u = norm(cross(axis, ref)); w = cross(axis, u)
    segs = hi_segs if HI else 1
    if HI: sides = hi_sides or max(sides*3, 12)
    bumps = [1+.08*math.sin(3*2*math.pi*i/sides)+.05*math.sin(5*2*math.pi*i/sides+1) for i in range(sides)]  # same on every tube -> no seams
    rings = []
    for k in range(segs + 1):
        t = k/segs
        p = add(p0, mul(sub(p1,p0), t)); r = r0 + (r1-r0)*t
        if HI:
            r *= 1 + flare*(1-t)**4                                 # root flare
            e = math.sin(math.pi*t); p = add(p, add(mul(u, .015*e*math.sin(t*7+r0*40)), mul(w, .015*e*math.cos(t*5))))
        ring = []
        for i in range(sides + 1):
            a = 2*math.pi*i/sides
            d = add(mul(u, math.cos(a)), mul(w, math.sin(a)))
            rr = r * (bumps[i % sides] if HI else 1)
            ring.append((v(add(p, mul(d, rr))), vt((i/sides, vrep*t)), vn(d)))
        rings.append(ring)
    for k in range(segs):
        A, B = rings[k], rings[k+1]
        for i in range(sides):
            F.append(("bark", [A[i], B[i+1], A[i+1]]))
            F.append(("bark", [A[i], B[i], B[i+1]]))

base, top = (0,0,0), (0.06,2.3,0.02)
tube((0,-0.05,0), (0.03,1.2,0.01), 0.17, 0.11, vrep=3, flare=0.8)      # lower trunk
tube((0.03,1.2,0.01), top, 0.11, 0.05, vrep=2)              # upper trunk
branches = []
for k in range(5):
    a = k*2.4 + 0.6; h = 1.15 + 0.18*k
    st = (0.03, h, 0.01)
    en = (math.cos(a)*0.7, h + 0.5, math.sin(a)*0.7)
    tube(st, en, 0.055, 0.02, sides=4, vrep=1.5)
    branches.append(en)
    if HI:                                            # twigs off each branch
        for _ in range(3):
            f = rng2.uniform(.45, .85); s0 = add(st, mul(sub(en, st), f))
            tip = add(s0, (rng2.uniform(-.35,.35), rng2.uniform(.1,.35), rng2.uniform(-.35,.35)))
            tube(s0, tip, 0.018, 0.006, sides=3, vrep=.6, hi_sides=4, hi_segs=3)

# ---------- leaf cards ----------------------------------------------------------
crown_c = (0.03, 1.95, 0.0)
def card(center, size, atlas_cell):
    _card(center, size, atlas_cell, random)
    if HI:                                            # denser crown: smaller cards around each one
        for _ in range(6):
            off = (rng2.uniform(-.3,.3)*size, rng2.uniform(-.25,.25)*size, rng2.uniform(-.3,.3)*size)
            _card(add(center, off), size*rng2.uniform(.45,.65), rng2.choice(cells), rng2)

def _card(center, size, atlas_cell, R):
    random = R
    # random orientation, but face roughly outward and somewhat up
    out = norm(add(sub(center, crown_c), (random.uniform(-.4,.4), .35, random.uniform(-.4,.4))))
    t1 = norm(cross(out, (0,1,0)) if abs(out[1]) < .95 else cross(out,(1,0,0)))
    t2 = cross(out, t1)
    rot = random.uniform(0, 2*math.pi)
    a = add(mul(t1, math.cos(rot)), mul(t2, math.sin(rot)))
    b = add(mul(t1, -math.sin(rot)), mul(t2, math.cos(rot)))
    cu, cv = atlas_cell
    corners = [(-1,-1),(1,-1),(1,1),(-1,1)]
    ids = []
    for (x, y) in corners:
        p = add(center, add(mul(a, x*size), mul(b, y*size)))
        n = norm(sub(p, crown_c))          # spherical crown normal
        ids.append((v(p), vt((cu + (x+1)/4, cv + (y+1)/4)), vn(n)))
    F.append(("leaves", [ids[0], ids[1], ids[2]]))
    F.append(("leaves", [ids[0], ids[2], ids[3]]))

cells = [(0,0),(.5,0),(0,.5),(.5,.5)]
for en in branches:                                   # clusters at branch tips
    for _ in range(2):
        card(add(en, (random.uniform(-.2,.2), random.uniform(-.05,.3), random.uniform(-.2,.2))),
             random.uniform(.55,.7), random.choice(cells))
for _ in range(8):                                    # fill + top of crown
    a = random.uniform(0, 2*math.pi); r = random.uniform(.1, .6)
    card((math.cos(a)*r, random.uniform(1.75, 2.6), math.sin(a)*r), random.uniform(.6,.75), random.choice(cells))

# ---------- textures ------------------------------------------------------------
def leaf_atlas(S=2048 if HI else 1024):
    k = S/1024
    img = Image.new("RGBA", (S,S), (0,0,0,0))
    d = ImageDraw.Draw(img)
    for cx in range(2):
        for cy in range(2):
            ox, oy, cs = cx*S//2, cy*S//2, S//2
            for _ in range(1100):
                # leaves denser toward the cluster centre, sparse ragged edge
                ang = random.uniform(0, 2*math.pi); rr = (random.random()**0.55) * cs*0.44
                x = ox + cs/2 + math.cos(ang)*rr; y = oy + cs/2 + math.sin(ang)*rr
                L = random.uniform(18, 32)*k; W = L*0.45; th = random.uniform(0, math.pi)
                shade = 1 - rr/(cs*0.5)*0.45 + random.uniform(-.12,.12)
                g = (int(70*shade+random.randint(-8,20)), int(135*shade+random.randint(-15,20)), int(45*shade), 255)
                pts = []
                for i in range(24 if HI else 10):
                    s = i/9*math.pi*2
                    px, py = math.cos(s)*L/2, math.sin(s)*W/2
                    pts.append((x+px*math.cos(th)-py*math.sin(th), y+px*math.sin(th)+py*math.cos(th)))
                d.polygon(pts, fill=g)
    return img.filter(ImageFilter.SMOOTH)

def bark(S=256):
    img = Image.new("RGB", (S,S)); px = img.load()
    for x in range(S):
        col = random.uniform(.8, 1.1)
        for y in range(S):
            n = col * (0.85 + 0.15*math.sin((y*0.15 + x*0.05)*256/S)) + random.uniform(-.06,.06)
            px[x,y] = (int(120*n), int(98*n), int(78*n))
    for _ in range(int(40*S/256)):                                  # vertical cracks
        x = random.randrange(S); y = random.randrange(S); l = random.randint(20,80)*S//256
        for i in range(l): px[(x+i//12)%S, (y+i)%S] = (40,30,24)
    return img.filter(ImageFilter.GaussianBlur(0.7))

leaf_atlas().save(OUT/f"leaves{SFX}.png"); bark(1024 if HI else 256).save(OUT/f"bark{SFX}.png")

# ---------- OBJ / MTL -----------------------------------------------------------
(OUT/f"lowpoly_tree{SFX}.mtl").write_text(
    f"newmtl bark\nKd 1 1 1\nKs 0 0 0\nmap_Kd bark{SFX}.png\n\n"
    f"newmtl leaves\nKd 1 1 1\nKs 0 0 0\nmap_Kd leaves{SFX}.png\nmap_d leaves{SFX}.png\n")
with open(OUT/f"lowpoly_tree{SFX}.obj","w") as f:
    f.write(f"# realistic low-poly tree: {len(F)} triangles\nmtllib lowpoly_tree{SFX}.mtl\no tree\n")
    for p in V: f.write("v %.4f %.4f %.4f\n" % p)
    for p in VT: f.write("vt %.4f %.4f\n" % p)
    for p in VN: f.write("vn %.4f %.4f %.4f\n" % p)
    cur = None
    for m, tri in F:
        if m != cur: f.write(f"usemtl {m}\n"); cur = m
        f.write("f " + " ".join(f"{a}/{b}/{c}" for a,b,c in tri) + "\n")

# ---------- preview -------------------------------------------------------------
data = json.dumps({"v":V,"t":VT,"n":VN,"f":F})
html = """<!DOCTYPE html><html><head><meta charset=utf-8><title>Low-poly tree</title>
<style>body{margin:0}#i{position:fixed;top:8px;left:10px;font:13px sans-serif}</style></head><body><div id=i></div>
<script type=importmap>{"imports":{"three":"https://unpkg.com/three@0.160.0/build/three.module.js","three/addons/":"https://unpkg.com/three@0.160.0/examples/jsm/"}}</script>
<script type=module>
import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
const D=__DATA__; document.getElementById('i').textContent=D.f.length+' triangles';
const r=new THREE.WebGLRenderer({antialias:true});r.setSize(innerWidth,innerHeight);r.shadowMap.enabled=true;
r.shadowMap.type=THREE.PCFSoftShadowMap;r.outputColorSpace=THREE.SRGBColorSpace;document.body.appendChild(r.domElement);
const s=new THREE.Scene();s.background=new THREE.Color(0xbcd6e8);s.fog=new THREE.Fog(0xbcd6e8,8,25);
const c=new THREE.PerspectiveCamera(38,innerWidth/innerHeight,.1,100);c.position.set(4,2.2,4.5);
const o=new OrbitControls(c,r.domElement);o.target.set(0,1.6,0);o.autoRotate=true;o.autoRotateSpeed=1;o.update();
s.add(new THREE.HemisphereLight(0xe6f2ff,0x4a4030,1.2));
const L=new THREE.DirectionalLight(0xfff0d0,2.6);L.position.set(4,7,3);L.castShadow=true;L.shadow.mapSize.set(2048,2048);s.add(L);
const tl=new THREE.TextureLoader();const tex={bark:tl.load('bark__SFX__.png'),leaves:tl.load('leaves__SFX__.png')};
for(const k in tex){tex[k].colorSpace=THREE.SRGBColorSpace;tex[k].wrapS=tex[k].wrapT=THREE.RepeatWrapping;}
for(const name of ['bark','leaves']){
  const p=[],uv=[],nn=[];
  for(const [m,tri] of D.f) if(m===name) for(const [a,b,cc] of tri){p.push(...D.v[a-1]);uv.push(...D.t[b-1]);nn.push(...D.n[cc-1]);}
  const g=new THREE.BufferGeometry();g.setAttribute('position',new THREE.Float32BufferAttribute(p,3));
  g.setAttribute('uv',new THREE.Float32BufferAttribute(uv,2));g.setAttribute('normal',new THREE.Float32BufferAttribute(nn,3));
  const mat=new THREE.MeshStandardMaterial({map:tex[name],roughness:.85,side:name==='leaves'?THREE.DoubleSide:THREE.FrontSide,alphaTest:.5});
  const me=new THREE.Mesh(g,mat);me.castShadow=me.receiveShadow=true;s.add(me);}
const gr=new THREE.Mesh(new THREE.CircleGeometry(12,48),new THREE.MeshStandardMaterial({color:0x6f8f4a,roughness:1}));
gr.rotation.x=-Math.PI/2;gr.receiveShadow=true;s.add(gr);
addEventListener('resize',()=>{c.aspect=innerWidth/innerHeight;c.updateProjectionMatrix();r.setSize(innerWidth,innerHeight)});
(function a(){requestAnimationFrame(a);o.update();r.render(s,c)})();
</script></body></html>"""
(OUT/f"preview{SFX}.html").write_text(html.replace("__DATA__", data).replace("__SFX__", SFX))
print(len(F), "triangles,", len(V), "verts")
