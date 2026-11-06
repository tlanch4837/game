"""Cities: Skylines II base-game tree set — realistic low-poly, two LODs each.

Species (from the CS2 wiki "Base Game Trees" + palms):
  Alder, Apple, Spruce, Pine, Oak, Birch, LondonPlane, Linden, Hickory, Poplar,
  Chestnut, CoconutPalm, FloridaPalm, RoyalPalm, SylvesterPalm

Per species, one material + one 2048 texture atlas shared by both LODs:
  atlas cell (0,0)=bark, other 3 cells = foliage variants (alpha cutout).
Outputs:
  out/obj/<Name>_LOD0.obj  (~1700 tris, close-up)   <Name>_LOD1.obj (~100 tris, far)
  out/cs2/<Name>/<Name>_BaseColor.png, _NormalMap.png, _MaskMap.png  (+ FBX via blender_export.py)
  out/preview.html
Units: metres, Y up.
"""
import math, random, json, pathlib
from PIL import Image, ImageDraw, ImageFilter

ROOT = pathlib.Path(__file__).parent / "out"
TEX = 2048
CELL = [(0, 0), (.5, 0), (0, .5), (.5, .5)]      # bark, fol1, fol2, fol3

# ------------------------------------------------------------------ vector utils
def sub(a,b): return tuple(x-y for x,y in zip(a,b))
def add(a,b): return tuple(x+y for x,y in zip(a,b))
def mul(a,s): return tuple(x*s for x in a)
def dot(a,b): return sum(x*y for x,y in zip(a,b))
def cross(a,b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def norm(a): l=math.sqrt(dot(a,a)) or 1; return tuple(c/l for c in a)
def lerp(a,b,t): return add(a, mul(sub(b,a), t))
def bez(p0,p1,p2,t): return lerp(lerp(p0,p1,t), lerp(p1,p2,t), t)

class Mesh:
    def __init__(s): s.V, s.T, s.N, s.F = [], [], [], []
    def v(s,p): s.V.append(p); return len(s.V)
    def t(s,p): s.T.append(p); return len(s.T)
    def n(s,p): s.N.append(norm(p)); return len(s.N)
    def tri(s,a,b,c): s.F.append((a,b,c))

# ------------------------------------------------------------------ geometry builders
class Builder:
    def __init__(s, hi, seed):
        s.hi, s.m = hi, Mesh()
        s.r = random.Random(seed)          # shared layout -> identical silhouette in both LODs
        s.r2 = random.Random(seed + 1000)  # extra detail only (hi)

    def tube(s, pts, radii, sides, hi_sides=None, nsub=6, bump=0.0, crownshaft=None):
        """Tapered tube along a polyline; hi subdivides along a smooth curve."""
        m = s.m
        if s.hi:
            sides = hi_sides or max(sides*3, 12)
            P, R = [], []
            for i in range(len(pts)-1):
                for k in range(nsub):
                    t = k/nsub; P.append(lerp(pts[i], pts[i+1], t)); R.append(radii[i]+(radii[i+1]-radii[i])*t)
            P.append(pts[-1]); R.append(radii[-1])
        else:
            P, R = list(pts), list(radii)
        total = sum(math.dist(P[i], P[i+1]) for i in range(len(P)-1)) or 1
        prev_u, acc, rings = None, 0.0, []
        for i, p in enumerate(P):
            axis = norm(sub(P[min(i+1,len(P)-1)], P[max(i-1,0)]))
            ref = prev_u or ((0,0,1) if abs(axis[1]) > .9 else (0,1,0))
            u = norm(cross(axis, cross(ref, axis))) if prev_u else norm(cross(axis, ref))
            prev_u = u; w = cross(axis, u)
            if i: acc += math.dist(P[i-1], p)
            vv = acc/total
            cs = crownshaft and vv >= crownshaft
            ring = []
            for j in range(sides+1):
                a = 2*math.pi*j/sides
                d = add(mul(u, math.cos(a)), mul(w, math.sin(a)))
                b = 1 + (bump*(math.sin(3*a)*.6+math.sin(5*a+1)*.4) if s.hi else 0)
                uu = .475 + .02*(j/sides) if cs else .02 + .42*(j/sides)
                ring.append((m.v(add(p, mul(d, R[i]*b))), m.t((uu, .02 + .46*vv)), m.n(d)))
            rings.append(ring)
        for A, B in zip(rings, rings[1:]):
            for j in range(sides):
                m.tri(A[j], B[j+1], A[j+1]); m.tri(A[j], B[j], B[j+1])

    def _quad(s, c, a, b, size, cell, ncenter, nbias=(0,0,0)):
        m = s.m; cu, cv = CELL[cell]; ids = []
        for x, y in [(-1,-1),(1,-1),(1,1),(-1,1)]:
            p = add(c, add(mul(a, x*size), mul(b, y*size)))
            ids.append((m.v(p), m.t((cu+.01+(x+1)/4*.96, cv+.01+(y+1)/4*.96)), m.n(add(norm(sub(p, ncenter)), nbias))))
        m.tri(ids[0], ids[1], ids[2]); m.tri(ids[0], ids[2], ids[3])

    def card(s, c, size, ncenter, up_bias=.35, sub_n=6):
        def one(c, size, R):
            out = norm(add(sub(c, ncenter), (R.uniform(-.4,.4), up_bias, R.uniform(-.4,.4))))
            t1 = norm(cross(out, (0,1,0)) if abs(out[1]) < .95 else cross(out,(1,0,0))); t2 = cross(out, t1)
            rot = R.uniform(0, 2*math.pi)
            a = add(mul(t1, math.cos(rot)), mul(t2, math.sin(rot))); b = add(mul(t1, -math.sin(rot)), mul(t2, math.cos(rot)))
            s._quad(c, a, b, size, R.choice([1,2,3]), ncenter)
        one(c, size, s.r)
        if s.hi:
            for _ in range(sub_n):
                off = tuple(s.r2.uniform(-.3,.3)*size for _ in range(3))
                one(add(c, off), size*s.r2.uniform(.45,.65), s.r2)

    def frond(s, base, direction, length, width, droop, cell=None, fold=.25):
        """Palm frond strip arching down; hi = more segments + V-fold."""
        m = s.m; cell = cell if cell is not None else s.r.choice([1,2,3])
        cu, cv = CELL[cell]
        d = norm(direction); side = norm(cross(d, (0,1,0)));
        p1 = add(base, mul(d, length*.55)); p2 = add(add(base, mul((d[0],0,d[2]), length*.95)), (0, d[1]*length*.3 - droop, 0))
        segs = 8 if s.hi else 2
        cols = 3 if s.hi else 2     # hi: centre rib + two halves => V fold
        rows = []
        for k in range(segs+1):
            t = k/segs; c = bez(base, p1, p2, t)
            tang = norm(sub(bez(base,p1,p2,min(t+.05,1)), bez(base,p1,p2,max(t-.05,0))))
            up = norm(cross(side, tang)); wv = width*math.sin(math.pi*min(.15+t*.85,1))*(1 if t>0 else .2)
            row = []
            for j in range(cols):
                x = j/(cols-1)*2-1
                p = add(c, mul(side, x*wv))
                if cols == 3 and j != 1: p = add(p, mul(up, -fold*wv))   # fold leaflets down from rib
                nrm = add(up, mul(side, x*.4))
                row.append((m.v(p), m.t((cu+.01+(x+1)/4*.96, cv+.01+t*.48)), m.n(nrm)))
            rows.append(row)
        for A, B in zip(rows, rows[1:]):
            for j in range(cols-1):
                m.tri(A[j], A[j+1], B[j+1]); m.tri(A[j], B[j+1], B[j])

# ------------------------------------------------------------------ species
def broadleaf(b, P):
    r = b.r
    H, trunk_r, cc, (rx, ry) = P["h"], P["tr"], P["crown_y"], P["crown"]
    lean = P.get("lean", 0)
    top = (lean, H*.92, 0)
    mid = (lean*.4 + r.uniform(-.1,.1), H*.45, r.uniform(-.1,.1))
    b.tube([(0,-.2,0), mid, top], [trunk_r, trunk_r*.65, trunk_r*.2], 5, bump=.08)
    base_y = P.get("branch_from", cc - ry*.6)
    for k in range(P.get("branches", 5)):
        a = k*2.4 + r.uniform(0,.5); y = base_y + (cc + ry*.3 - base_y)*k/max(P.get("branches",5)-1,1)
        st = (lean*y/H, y, 0)
        en = (math.cos(a)*rx*.75, y + ry*P.get("rise", .45), math.sin(a)*rx*.75)
        b.tube([st, lerp(st,en,.5), en], [trunk_r*.4, trunk_r*.25, trunk_r*.08], 4, hi_sides=8, nsub=3)
        if b.hi:
            for _ in range(3):
                s0 = lerp(st, en, b.r2.uniform(.45,.85))
                tip = add(s0, (b.r2.uniform(-1,1)*rx*.4, b.r2.uniform(.1,.5)*ry*.5, b.r2.uniform(-1,1)*rx*.4))
                b.tube([s0, tip], [trunk_r*.08, trunk_r*.02], 3, hi_sides=4, nsub=3)
    center = (lean*.8, cc, 0)
    n = P.get("cards", 18)
    for i in range(n):
        th = r.uniform(0, 2*math.pi); ph = math.acos(r.uniform(-.75, 1)); k = r.uniform(.45, .95)
        c = (center[0] + math.sin(ph)*math.cos(th)*rx*k, cc + math.cos(ph)*ry*k, math.sin(ph)*math.sin(th)*rx*k)
        b.card(c, P.get("card", .55)*min(rx, ry)*r.uniform(.85,1.15), center)

def conifer(b, P):
    """Spruce: drooping whorl tiers on a straight stem, cone silhouette."""
    r = b.r; H, tr = P["h"], P["tr"]
    b.tube([(0,-.2,0), (0,H*.5,0), (0,H,0)], [tr, tr*.55, tr*.05], 5, bump=.05)
    tiers = P.get("tiers", 7); y0 = H*P.get("clear", .12)
    for i in range(tiers):
        t = i/(tiers-1); y = y0 + (H*.97-y0)*t; rad = P["base_r"]*(1-t)**1.1 + .4
        for k in range(4):
            a = k*math.pi/2 + i*1.1 + r.uniform(-.3,.3)
            outd = (math.cos(a), 0, math.sin(a)); tang = (-math.sin(a), 0, math.cos(a))
            c = add((0, y - rad*.15, 0), mul(outd, rad*.55))
            slope = norm(add(outd, (0, -.35, 0)))
            ncen = (0, y - rad*.6, 0)
            b._quad(c, slope, tang, rad*.62, r.choice([1,2,3]), ncen, (0,.5,0))
            if b.hi:
                for _ in range(5):
                    a2 = a + b.r2.uniform(-.9,.9); od = (math.cos(a2),0,math.sin(a2))
                    c2 = add((0, y - rad*b.r2.uniform(.05,.3), 0), mul(od, rad*b.r2.uniform(.35,.75)))
                    b._quad(c2, norm(add(od,(0,b.r2.uniform(-.5,-.1),0))), (-math.sin(a2),0,math.cos(a2)),
                            rad*b.r2.uniform(.3,.45), b.r2.choice([1,2,3]), ncen, (0,.5,0))
        if b.hi and i < tiers-1:     # visible branch stubs per tier
            for k in range(4):
                a = k*math.pi/2 + i
                b.tube([(0,y,0), (math.cos(a)*rad*.8, y-rad*.25, math.sin(a)*rad*.8)], [tr*.18, tr*.04], 3, hi_sides=5, nsub=2)

def palm(b, P):
    r = b.r; H, tr = P["h"], P["tr"]; lean = P.get("lean", 0)
    top = (lean, H, 0)
    pts = [(0,-.2,0), (lean*.15, H*.35, 0), (lean*.5, H*.7, 0), top]
    radii = [tr*P.get("flare",1.3), tr, tr*.92, tr*.85]
    if P.get("crownshaft"):
        pts.append(add(top, (lean*.05, H*.12, 0))); radii.append(tr*.95); top = pts[-1]
    b.tube(pts, radii, 6, hi_sides=14, bump=P.get("bump",.04), crownshaft=P.get("crownshaft"))
    n = P.get("fronds", 12)
    for i in range(n):
        a = i*2.39996 + r.uniform(-.2,.2)
        rise = r.uniform(*P.get("rise", (-.3, .6)))
        d = (math.cos(a), rise, math.sin(a))
        L = P["frond_len"]*r.uniform(.85,1.1)
        b.frond(top, d, L, P["frond_w"], P.get("droop", 2.0)*(1-rise*.6), fold=P.get("fold",.25))
    if b.hi and P.get("boots"):     # leaf-base stubs ("boots") under crown
        for i in range(10):
            a = b.r2.uniform(0, 2*math.pi); y = H*b.r2.uniform(.75,.98)
            st = (lean*y/H, y, 0); b.tube([st, add(st,(math.cos(a)*tr*1.8, tr*1.2, math.sin(a)*tr*1.8))], [tr*.25, tr*.05], 3, hi_sides=4, nsub=1)

# name: (builder, params, bark_style, foliage_style)
SPECIES = {
 "Alder":       (broadleaf, dict(h=15, tr=.28, crown_y=10, crown=(3.2, 4.6), cards=18, branches=5), ("rough", (95,88,80)), ("broad", (60,105,45), 1.0)),
 "Apple":       (broadleaf, dict(h=5.5, tr=.18, crown_y=3.8, crown=(2.8, 1.9), cards=18, branches=5, rise=.3, branch_from=1.3), ("rough", (100,82,66)), ("apple", (75,125,48), 1.0)),
 "Oak":         (broadleaf, dict(h=15, tr=.45, crown_y=9.5, crown=(5.5, 4.5), cards=20, branches=6, branch_from=4), ("rough", (88,78,66)), ("oak", (55,95,38), 1.1)),
 "Birch":       (broadleaf, dict(h=15, tr=.2, crown_y=10, crown=(2.6, 4.8), cards=16, branches=5, lean=.4, rise=.6), ("birch", (225,222,212)), ("broad", (100,145,55), .75)),
 "LondonPlane": (broadleaf, dict(h=20, tr=.5, crown_y=13.5, crown=(6.5, 5.5), cards=22, branches=6, branch_from=5.5), ("plane", (150,140,105)), ("plane", (70,115,45), 1.3)),
 "Linden":      (broadleaf, dict(h=18, tr=.4, crown_y=11, crown=(4.5, 6.5), cards=20, branches=6), ("rough", (85,78,70)), ("broad", (70,120,45), 1.05)),
 "Hickory":     (broadleaf, dict(h=19, tr=.38, crown_y=13, crown=(4.2, 5.5), cards=18, branches=5, branch_from=7), ("shaggy", (110,100,88)), ("broad", (75,110,40), .95)),
 "Poplar":      (broadleaf, dict(h=22, tr=.35, crown_y=12.5, crown=(2.2, 9.5), cards=18, branches=6, rise=.8, branch_from=3), ("rough", (105,100,88)), ("poplar", (65,115,50), .8)),
 "Chestnut":    (broadleaf, dict(h=17, tr=.5, crown_y=10.5, crown=(5.5, 5.5), cards=20, branches=6, branch_from=4), ("spiral", (90,75,62)), ("chestnut", (50,95,35), 1.2)),
 "Spruce":      (conifer,   dict(h=20, tr=.35, base_r=4.0, tiers=11, clear=.04), ("rough", (80,62,50)), ("needle", (55,100,58), 1.0)),
 "Pine":        (broadleaf, dict(h=19, tr=.33, crown_y=15.5, crown=(3.8, 2.6), cards=16, branches=5, rise=.35, branch_from=11.5), ("pine", (150,85,50)), ("needle", (60,95,55), 1.0)),
 "CoconutPalm": (palm, dict(h=14, tr=.22, lean=3.0, fronds=12, frond_len=5.0, frond_w=.9, droop=2.4, rise=(-.4,.5)), ("rings", (130,115,95)), ("pinnate", (85,125,40), 1.0)),
 "FloridaPalm": (palm, dict(h=11, tr=.25, fronds=14, frond_len=3.0, frond_w=1.4, droop=.9, rise=(-.2,.9), fold=.1, boots=True, bump=.1), ("boots", (120,105,80)), ("fan", (110,150,85), 1.0)),
 "RoyalPalm":   (palm, dict(h=18, tr=.32, fronds=12, frond_len=4.5, frond_w=.8, droop=2.8, rise=(-.5,.6), crownshaft=.88, flare=1.5), ("smooth", (175,172,165)), ("pinnate", (65,115,45), 1.0)),
 "SylvesterPalm":(palm,dict(h=9, tr=.42, fronds=18, frond_len=4.2, frond_w=.75, droop=1.8, rise=(-.5,.85), boots=True, bump=.12), ("diamond", (115,95,70)), ("pinnate", (95,125,95), 1.0)),
}

# ------------------------------------------------------------------ textures
def draw_bark(img, style, col, R):
    S = img.size[0]//2; px = img.load()
    base = col
    for x in range(S):
        cn = R.uniform(.85, 1.1)
        for y in range(S):
            n = cn + R.uniform(-.07,.07)
            if style == "rings": n *= .8 + .2*abs(math.sin(y*.12))
            if style == "pine" : base = tuple(int(c*(.55+.45*y/S)) for c in col) if y < S*.6 else col  # grey-brown low, orange high (v up = y up)
            px[x, y] = tuple(max(0,min(255,int(c*n))) for c in base) + (255,)
    d = ImageDraw.Draw(img)
    if style in ("rough", "shaggy", "pine", "spiral"):
        for _ in range(260 if style!="shaggy" else 180):
            x = R.randrange(S); y = R.randrange(S); l = R.randint(30, 160); w = R.randint(2, 6 if style=="shaggy" else 4)
            dx = R.randint(-6,6) + (l//3 if style=="spiral" else 0)
            d.line([(x,y),(x+dx,y+l)], fill=tuple(int(c*.45) for c in col)+(255,), width=w)
    if style == "birch":
        for _ in range(140):
            x = R.randrange(S); y = R.randrange(S); l = R.randint(20, 120)
            d.line([(x,y),(min(S-1,x+l),y+R.randint(-2,2))], fill=(35,32,30,255), width=R.randint(2,7))
    if style == "plane":
        for _ in range(90):
            x = R.randrange(S); y = R.randrange(S); rr = R.randint(25,80); c = R.choice([(205,195,150),(120,125,95),(180,170,120)])
            d.ellipse([x-rr,y-rr*1.6,x+rr,y+rr*1.6], fill=c+(255,))
    if style == "diamond":
        for y in range(0, S, 40):
            for x in range(0, S, 40):
                o = 20 if (y//40)%2 else 0
                d.polygon([(x+o,y),(x+o+20,y+20),(x+o,y+40),(x+o-20,y+20)], outline=(60,48,35,255), width=5)
    if style == "boots":
        for y in range(0, S, 55):
            for x in range(0, S, 70):
                o = 35 if (y//55)%2 else 0
                d.polygon([(x+o-30,y),(x+o+30,y),(x+o,y+50)], fill=(140,120,90,255), outline=(70,58,42,255))

def leaf_poly(x, y, L, W, th, kind, R):
    pts = []
    n = 14
    for i in range(n):
        s = i/n*2*math.pi
        rr = 1.0
        if kind in ("oak",):   rr = 1 + .25*math.sin(s*5)
        if kind in ("plane",): rr = 1 + .35*abs(math.sin(s*2.5))
        px_, py_ = math.cos(s)*L/2*rr, math.sin(s)*W/2*rr
        pts.append((x+px_*math.cos(th)-py_*math.sin(th), y+px_*math.sin(th)+py_*math.cos(th)))
    return pts

def draw_foliage(img, ox, oy, cs, style, col, scale, R):
    d = ImageDraw.Draw(img)
    def shade_col(sh, jit=18):
        return tuple(max(0,min(255,int(c*sh)+R.randint(-jit,jit))) for c in col) + (255,)
    if style in ("broad","oak","plane","poplar","chestnut","apple"):
        cnt = int(1100/scale**1.3)
        Lmin, Lmax = {"poplar":(12,22), "plane":(30,50), "chestnut":(34,58), "oak":(24,38)}.get(style, (18,32))
        for _ in range(cnt):
            ang = R.uniform(0,2*math.pi); rr = R.random()**.55*cs*.44
            x = ox+cs/2+math.cos(ang)*rr; y = oy+cs/2+math.sin(ang)*rr
            L = R.uniform(Lmin,Lmax)*scale*cs/512; W = L*(.75 if style in ("plane","poplar") else .45)
            if style == "chestnut":   # palmate: 5 long leaflets
                for k in range(5):
                    th = ang + (k-2)*.45 + R.uniform(-.1,.1)
                    d.polygon(leaf_poly(x+math.cos(th)*L*.4, y+math.sin(th)*L*.4, L, L*.28, th, "broad", R), fill=shade_col(1.2-rr/cs*.6))
                continue
            d.polygon(leaf_poly(x,y,L,W,R.uniform(0,math.pi),style,R), fill=shade_col(1.2-rr/cs*.6 + R.uniform(-.12,.12)))
        if style == "apple":
            for _ in range(70):
                ang = R.uniform(0,2*math.pi); rr = R.random()**.6*cs*.4
                x = ox+cs/2+math.cos(ang)*rr; y = oy+cs/2+math.sin(ang)*rr; s_ = cs*.016
                d.ellipse([x-s_,y-s_,x+s_,y+s_], fill=(R.randint(170,210),R.randint(30,60),30,255))
    elif style == "needle":
        for _ in range(300):
            ang = R.uniform(0,2*math.pi); rr = R.random()**.6*cs*.42
            cx_, cy_ = ox+cs/2+math.cos(ang)*rr, oy+cs/2+math.sin(ang)*rr
            sh = 1.15-rr/cs*.6
            for _ in range(55):                     # radiating needle tuft
                a = R.uniform(0, 2*math.pi); l = R.uniform(.03,.07)*cs
                d.line([(cx_,cy_),(cx_+math.cos(a)*l, cy_+math.sin(a)*l)], fill=shade_col(sh, 12), width=max(2,cs//300))
    elif style == "pinnate":                        # frond: rib along +v (texture y), leaflets both sides
        cx_ = ox+cs/2
        d.line([(cx_,oy),(cx_,oy+cs)], fill=shade_col(.8,5), width=max(3,cs//120))
        for i in range(90):
            t = i/90; y = oy+t*cs; L = cs*.47*math.sin(math.pi*min(.1+t*.9,1))
            for sgn in (-1,1):
                a = .55 + R.uniform(-.08,.08)
                ex, ey = cx_+sgn*L*math.cos(a)*.98, y + L*math.sin(a)*.6
                d.polygon([(cx_,y),(ex,ey),(cx_,y+cs*.012)], fill=shade_col(1-t*.25 + R.uniform(-.08,.08), 10))
    elif style == "fan":                            # costapalmate fan from base (v=0)
        bx, by = ox+cs/2, oy
        for i in range(55):
            a = math.pi*(.08 + .84*i/54) + R.uniform(-.01,.01)
            L = cs*R.uniform(.85,.98)
            tip = (bx+math.cos(a)*L*.5, by+math.sin(a)*L)
            d.polygon([(bx,by),(bx+math.cos(a+.02)*L*.48, by+math.sin(a+.02)*L*.97),tip,(bx+math.cos(a-.02)*L*.48, by+math.sin(a-.02)*L*.97)], fill=shade_col(R.uniform(.85,1.1),8))

def make_textures(name, bark, fol, seed, outdir):
    R = random.Random(seed + 7)
    img = Image.new("RGBA", (TEX, TEX), (0,0,0,0))
    S = TEX//2
    barkimg = Image.new("RGBA", (TEX, TEX), (0,0,0,0)); draw_bark(barkimg, bark[0], bark[1], R)
    barkimg = barkimg.filter(ImageFilter.GaussianBlur(1.2))
    img.paste(barkimg.crop((0,0,S,S)), (0,0))
    ImageDraw.Draw(img).rectangle([int(TEX*.47*1), 0, S-1, S-1], fill=(70,120,55,255))  # crownshaft green (u .47-.5)
    for ci in (1,2,3):
        cu, cv = CELL[ci]
        draw_foliage(img, int(cu*TEX), int(cv*TEX), S, fol[0], fol[1], fol[2], R)
    # OBJ/three use v-up; PIL y-down -> flip so (u,v) lookups land on the right cell
    img = img.transpose(Image.FLIP_TOP_BOTTOM)
    # dilate colour into transparent texels so mip-maps don't get dark halos
    rgb = img.convert("RGB").filter(ImageFilter.MaxFilter(5)); a = img.getchannel("A")
    out = Image.composite(img.convert("RGB"), rgb, a); out.putalpha(a)
    out.save(outdir / f"{name}_BaseColor.png")
    Image.new("RGB", (512,512), (128,128,255)).save(outdir / f"{name}_NormalMap.png")
    # CS2 MaskMap: R metallic, G ambient occlusion, B unused, A smoothness
    Image.new("RGBA", (512,512), (0,255,0,40)).save(outdir / f"{name}_MaskMap.png")

# ------------------------------------------------------------------ export
def write_obj(mesh, path, name):
    with open(path, "w") as f:
        f.write(f"# {name}: {len(mesh.F)} triangles\nmtllib {name}.mtl\no {path.stem}\n")
        for p in mesh.V: f.write("v %.4f %.4f %.4f\n" % p)
        for p in mesh.T: f.write("vt %.4f %.4f\n" % p)
        for p in mesh.N: f.write("vn %.4f %.4f %.4f\n" % p)
        f.write(f"usemtl {name}\n")
        for tri in mesh.F: f.write("f " + " ".join(f"{a}/{b}/{c}" for a,b,c in tri) + "\n")

def main():
    objd = ROOT/"obj"; objd.mkdir(parents=True, exist_ok=True)
    preview = {}
    for idx, (name, (fn, P, bark, fol)) in enumerate(SPECIES.items()):
        cs2 = ROOT/"cs2"/name; cs2.mkdir(parents=True, exist_ok=True)
        seed = 100 + idx*17
        make_textures(name, bark, fol, seed, cs2)
        (objd/f"{name}.mtl").write_text(f"newmtl {name}\nKd 1 1 1\nKs 0 0 0\nmap_Kd ../cs2/{name}/{name}_BaseColor.png\nmap_d ../cs2/{name}/{name}_BaseColor.png\n")
        counts = {}
        for lod, hi in (("LOD0", True), ("LOD1", False)):
            b = Builder(hi, seed); fn(b, P)
            write_obj(b.m, objd/f"{name}_{lod}.obj", name)
            counts[lod] = len(b.m.F)
            preview.setdefault(name, {})[lod] = {"v": b.m.V, "t": b.m.T, "n": b.m.N, "f": b.m.F}
        print(f"{name:14s} LOD0 {counts['LOD0']:5d} tris   LOD1 {counts['LOD1']:4d} tris")
    html = (pathlib.Path(__file__).parent/"cs2_preview_template.html").read_text()
    (ROOT/"preview.html").write_text(html.replace("__DATA__", json.dumps(preview, separators=(",",":"))))

if __name__ == "__main__":
    main()
