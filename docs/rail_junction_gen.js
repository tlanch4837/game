// Port of src/city/rail_junction_mesh.h (keep in step): builds a junction model as per-material vertex arrays.
// Frame: x along the main line, z across (Right = +z), y up from the track base.
export const K = {
  gauge: 1.435, minRadius: 45, turnoutMin: 4, turnoutMax: 35, diamondMin: 10, slipMax: 20, levelMin: 30,
  spacingMin: 3.8, spacingMax: 12,
  bedTop: 0.25, bedTopHalf: 1.6, bedFootHalf: 2.2, tieTop: 0.33, tieHalfLen: 1.3, tieWidth: 0.26, tieStep: 0.65,
  railTop: 0.5, railHead: 0.075, longTie: 2.4, checkGap: 0.11,
  cellMachine: 0, cellMast: 0.25, cellBoom: 0.5, cellDeck: 0.75,
};
export const MATS = ['Ballast', 'Tie', 'Steel', 'Prop', 'Paint'];
const rad = d => d * 0.0174532925, deg = r => r * 57.2957795;
const v = (x, z) => ({x, z});
const add = (a, b) => v(a.x + b.x, a.z + b.z), sub = (a, b) => v(a.x - b.x, a.z - b.z), mul = (a, s) => v(a.x * s, a.z * s);
const dot = (a, b) => a.x * b.x + a.z * b.z, cross = (a, b) => a.x * b.z - a.z * b.x, len = a => Math.hypot(a.x, a.z);
const norm = a => { const l = len(a); return l > 1e-6 ? mul(a, 1 / l) : v(1, 0); };
const leftOf = d => v(d.z, -d.x), rightOf = d => v(-d.z, d.x), lerp = (a, b, t) => add(a, mul(sub(b, a), t));
const clamp = (x, a, b) => Math.min(b, Math.max(a, x));
export const turnoutLead = (a, R) => R * Math.sin(rad(a));
export const turnoutOffset = (a, R) => R * (1 - Math.cos(rad(a)));
export const crossoverMaxAngle = (sp, R) => deg(Math.acos(clamp(1 - sp / (2 * R), -1, 1)));
export function crossoverRun(sp, a, R) {
  a = Math.min(a, crossoverMaxAngle(sp, R));
  const o = turnoutOffset(a, R), D = Math.max(0, sp - 2 * o) / Math.max(1e-3, Math.sin(rad(a)));
  return 2 * turnoutLead(a, R) + D * Math.cos(rad(a));
}

// ---------------------------------------------------------------- mesh output (mesh::Quad: a b c, a c d; normal to hint)
function newMesh() { const m = {by: {}}; for (const k of MATS) m.by[k] = {p: [], n: [], c: [], uv: []}; return m; }
function tri(o, a, b, c, n, col, uv) {
  for (const [p, i] of [[a, 0], [b, 2], [c, 4]]) { o.p.push(p[0], p[1], p[2]); o.n.push(n[0], n[1], n[2]); o.c.push(col[0], col[1], col[2]); o.uv.push(uv[i], uv[i + 1]); }
}
function quad(o, a, b, c, d, col, uv, hint = [0, 1, 0]) {
  { // wind counter-clockwise seen from the hint side (as Q in rail_junction_mesh.h)
    const x1 = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], x2 = [d[0] - a[0], d[1] - a[1], d[2] - a[2]];
    const cn = [x1[1] * x2[2] - x1[2] * x2[1], x1[2] * x2[0] - x1[0] * x2[2], x1[0] * x2[1] - x1[1] * x2[0]];
    if (cn[0] * hint[0] + cn[1] * hint[1] + cn[2] * hint[2] < 0) { [b, d] = [d, b]; uv = [uv[0], uv[1], uv[6], uv[7], uv[4], uv[5], uv[2], uv[3]]; }
  }
  const e1 = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], e2 = [d[0] - a[0], d[1] - a[1], d[2] - a[2]];
  let n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]];
  let l = Math.hypot(...n);
  if (l < 1e-9) { const f1 = [c[0] - b[0], c[1] - b[1], c[2] - b[2]], f2 = [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
    n = [f1[1] * f2[2] - f1[2] * f2[1], f1[2] * f2[0] - f1[0] * f2[2], f1[0] * f2[1] - f1[1] * f2[0]]; l = Math.hypot(...n) || 1; }
  n = n.map(x => x / l);
  if (n[0] * hint[0] + n[1] * hint[1] + n[2] * hint[2] < 0) n = n.map(x => -x);
  tri(o, a, b, c, n, col, [uv[0], uv[1], uv[2], uv[3], uv[4], uv[5]]);
  tri(o, a, c, d, n, col, [uv[0], uv[1], uv[4], uv[5], uv[6], uv[7]]);
}
const P3 = (p, y) => [p.x, y, p.z];
const COL = {ballast: [0.47, 0.45, 0.42], tie: [0.36, 0.30, 0.25], steel: [0.72, 0.73, 0.76], railSide: [0.42, 0.33, 0.27], prop: [1, 1, 1], paint: [0.93, 0.93, 0.90]};

// ---------------------------------------------------------------- routes
function straight(r, a, f, L) { const s0 = r.length ? r[r.length - 1].s : 0; if (!r.length) r.push({c: a, f, s: 0}); r.push({c: add(a, mul(f, L)), f, s: s0 + L}); }
function arc(r, a, f, R, angleDeg, side, lod) {
  if (!r.length) r.push({c: a, f, s: 0});
  const last = r[r.length - 1]; a = last.c; f = last.f;
  const s0 = last.s, ang = rad(angleDeg), step = lod ? 6 : 3, maxLen = lod ? 24 : 12;
  const n = Math.max(1, Math.max(Math.ceil(angleDeg / step), Math.ceil(R * ang / maxLen)));
  const nrm = mul(rightOf(f), side);
  for (let i = 1; i <= n; i++) { const t = ang * i / n;
    r.push({c: add(add(a, mul(f, R * Math.sin(t))), mul(nrm, R * (1 - Math.cos(t)))), f: norm(add(mul(f, Math.cos(t)), mul(nrm, Math.sin(t)))), s: s0 + R * t}); }
}
function tail(r, L) { if (L > 0.01) { const b = r[r.length - 1]; straight(r, b.c, b.f, L); } }
const offset = (p, d) => add(p.c, mul(rightOf(p.f), d));
function distTo(r, q) { // {d, f}: beside the route only (not beyond its ends)
  let best = 1e30, bf = v(1, 0);
  for (let i = 1; i < r.length; i++) { const a = r[i - 1].c, ab = sub(r[i].c, a), L2 = dot(ab, ab), tu = L2 > 1e-9 ? dot(sub(q, a), ab) / L2 : 0;
    if ((i === 1 && tu < 0) || (i + 1 === r.length && tu > 1)) continue;
    const t = clamp(tu, 0, 1), d = len(sub(add(a, mul(ab, t)), q)); if (d < best) { best = d; bf = norm(ab); } }
  return {d: best, f: bf};
}
function crossAt(r, q, n, lim) {
  let best = 1e30;
  for (let i = 1; i < r.length; i++) { const a = r[i - 1].c, e = sub(r[i].c, a), den = cross(n, e); if (Math.abs(den) < 1e-6) continue;
    const w = sub(a, q), uu = cross(w, e) / den, vv = cross(w, n) / den; if (vv < 0 || vv > 1 || Math.abs(uu) > lim) continue;
    if (Math.abs(uu) < Math.abs(best)) best = uu; }
  return best > 1e29 ? null : best;
}
export const G = {v, add, sub, mul, dot, cross, len, norm, leftOf, rightOf, lerp, clamp, rad, deg, newMesh, quad, P3, COL, straight, arc, tail, offset, distTo, crossAt};

// ---------------------------------------------------------------- parts
function bed(m, r, lod) {
  const o = m.by.Ballast, uvq = (...p) => p.flatMap(q => [q[0] / 4, q[2] / 4]);
  for (let i = 1; i < r.length; i++) { const a = r[i - 1], b = r[i];
    const la = P3(offset(a, -K.bedTopHalf), K.bedTop), ra = P3(offset(a, K.bedTopHalf), K.bedTop), lb = P3(offset(b, -K.bedTopHalf), K.bedTop), rb = P3(offset(b, K.bedTopHalf), K.bedTop);
    quad(o, la, ra, rb, lb, COL.ballast, uvq(la, ra, rb, lb));
    if (lod) continue;
    const lfa = P3(offset(a, -K.bedFootHalf), 0), lfb = P3(offset(b, -K.bedFootHalf), 0), rfa = P3(offset(a, K.bedFootHalf), 0), rfb = P3(offset(b, K.bedFootHalf), 0);
    quad(o, lfa, la, lb, lfb, COL.ballast, uvq(lfa, la, lb, lfb));
    quad(o, ra, rfa, rfb, rb, COL.ballast, uvq(ra, rfa, rfb, rb)); }
}
function railStrip(m, pts, fs, lod, top = K.railTop) {
  const o = m.by.Steel; let s = 0;
  for (let i = 1; i < pts.length; i++) { const l = len(sub(pts[i], pts[i - 1])), u0 = s / 2, u1 = (s + l) / 2; s += l;
    const na = mul(rightOf(fs[i - 1]), K.railHead * 0.5), nb = mul(rightOf(fs[i]), K.railHead * 0.5);
    const la = P3(sub(pts[i - 1], na), top), ra = P3(add(pts[i - 1], na), top), lb = P3(sub(pts[i], nb), top), rb = P3(add(pts[i], nb), top);
    quad(o, la, ra, rb, lb, COL.steel, [u0, 0, u0, 1, u1, 1, u1, 0]);
    if (lod) continue;
    const uvS = [u0, 0, u1, 0, u1, 1, u0, 1];
    const la0 = P3(sub(pts[i - 1], na), K.tieTop), lb0 = P3(sub(pts[i], nb), K.tieTop), ra0 = P3(add(pts[i - 1], na), K.tieTop), rb0 = P3(add(pts[i], nb), K.tieTop);
    const oL = leftOf(fs[i - 1]), oR = rightOf(fs[i - 1]);
    quad(o, la, lb, lb0, la0, COL.railSide, uvS, [oL.x, 0, oL.z]);
    quad(o, rb, ra, ra0, rb0, COL.railSide, uvS, [oR.x, 0, oR.z]); }
}
function railLine(r, gauge, side) { return {pts: r.map(p => offset(p, side * (gauge + K.railHead) * 0.5)), fs: r.map(p => p.f)}; }
function rails(m, r, gauge, lod) { for (const s of [-1, 1]) { const L = railLine(r, gauge, s); railStrip(m, L.pts, L.fs, lod); } }
function box(o, c, f, l, w, y0, h, col, u0) {
  const x = mul(f, l * 0.5), z = mul(rightOf(f), w * 0.5), q = [sub(sub(c, x), z), sub(add(c, x), z), add(add(c, x), z), add(sub(c, x), z)], y1 = y0 + h;
  quad(o, P3(q[0], y1), P3(q[1], y1), P3(q[2], y1), P3(q[3], y1), col, [u0, 0, u0 + 0.25, 0, u0 + 0.25, 0.5, u0, 0.5]);
  for (let k = 0; k < 4; k++) { const a = q[k], b = q[(k + 1) % 4], mid = sub(mul(add(a, b), 0.5), c);
    quad(o, P3(a, y1), P3(b, y1), P3(b, y0), P3(a, y0), col, [u0, 0.5, u0 + 0.25, 0.5, u0 + 0.25, 1, u0, 1], [mid.x, 0, mid.z]); }
}
function card(o, a, b, y0, y1, u0) { const h = rightOf(norm(sub(b, a)));
  quad(o, P3(a, y1), P3(b, y1), P3(b, y0), P3(a, y0), COL.prop, [u0, 0, u0 + 0.25, 0, u0 + 0.25, 1, u0, 1], [h.x, 0, h.z]); }
function routeAt(r, s) { let i = 1; while (i + 1 < r.length && r[i].s < s) i++;
  const a = r[i - 1], b = r[i], t = b.s > a.s ? clamp((s - a.s) / (b.s - a.s), 0, 1) : 0; return {c: lerp(a.c, b.c, t), f: norm(lerp(a.f, b.f, t))}; }
function tieRun(o, r, s0, s1, half) {
  const cut = [s0]; for (const p of r) if (p.s > s0 + 0.01 && p.s < s1 - 0.01) cut.push(p.s); cut.push(s1);
  for (let i = 1; i < cut.length; i++) { const A = routeAt(r, cut[i - 1]), B = routeAt(r, cut[i]), va = cut[i - 1] / K.tieStep, vb = cut[i] / K.tieStep;
    quad(o, P3(sub(A.c, mul(rightOf(A.f), half)), K.tieTop), P3(add(A.c, mul(rightOf(A.f), half)), K.tieTop), P3(add(B.c, mul(rightOf(B.f), half)), K.tieTop),
      P3(sub(B.c, mul(rightOf(B.f), half)), K.tieTop), COL.tie, [0, va, 1, va, 1, vb, 0, vb]); }
}
function ties(m, routes, virt, gauge, skip) {
  const o = m.by.Tie, halfStd = gauge * 0.5 + (K.tieHalfLen - K.gauge * 0.5);
  routes.forEach((r, k) => {
    const L = r[r.length - 1].s; let runA = -1, runB = -1;
    const flush = () => { if (runA >= 0) tieRun(o, r, runA, runB, halfStd); runA = -1; };
    for (let s = K.tieStep * 0.5; s < L; s += K.tieStep) {
      const {c, f} = routeAt(r, s), n = rightOf(f);
      let covered = skip(c);
      for (let j = 0; j < k && !covered; j++) { const D = distTo(routes[j], c); covered = D.d < (Math.abs(dot(D.f, f)) < 0.5 ? halfStd : K.longTie); }
      if (covered) { flush(); continue; }
      let lo = virt[k] ? 1e9 : -halfStd, hi = virt[k] ? -1e9 : halfStd, longTie = false;
      for (let j = k + 1; j < routes.length; j++) { if (virt[j]) continue; const u = crossAt(routes[j], c, n, K.longTie);
        if (u !== null) { lo = Math.min(lo, u - halfStd); hi = Math.max(hi, u + halfStd); longTie = true; } }
      if (hi < lo) { flush(); continue; }
      if (!longTie) { if (runA < 0) runA = Math.max(0, s - K.tieStep * 0.5); runB = Math.min(L, s + K.tieStep * 0.5); continue; }
      flush();
      const w = mul(f, K.tieWidth * 0.5), ul = (hi - lo) / (2 * halfStd);
      quad(o, P3(sub(add(c, mul(n, lo)), w), K.tieTop), P3(sub(add(c, mul(n, hi)), w), K.tieTop), P3(add(add(c, mul(n, hi)), w), K.tieTop), P3(add(add(c, mul(n, lo)), w), K.tieTop),
        COL.tie, [0, 0.3, ul, 0.3, ul, 0.7, 0, 0.7]);
    }
    flush();
  });
}
function frogs(m, routes, virt, gauge) {
  const hits = [];
  for (let i = 0; i < routes.length; i++) for (let j = i + 1; j < routes.length; j++) { if (virt[i] || virt[j]) continue;
    for (const si of [-1, 1]) for (const sj of [-1, 1]) { const A = railLine(routes[i], gauge, si), B = railLine(routes[j], gauge, sj);
      for (let x = 1; x < A.pts.length; x++) for (let y = 1; y < B.pts.length; y++) {
        const r = sub(A.pts[x], A.pts[x - 1]), q = sub(B.pts[y], B.pts[y - 1]), w = sub(B.pts[y - 1], A.pts[x - 1]), den = cross(r, q);
        if (Math.abs(den) < 1e-6) continue; const u = cross(w, q) / den, vv = cross(w, r) / den;
        if (u < 0 || u >= 1 || vv < 0 || vv >= 1) continue; const P = add(A.pts[x - 1], mul(r, u));
        if (len(sub(P, A.pts[0])) < 1 || len(sub(P, B.pts[0])) < 1) continue;
        hits.push({p: P, fa: norm(r), fb: norm(q), ra: i, rb: j, sa: si, sb: sj}); } } }
  const checks = [];
  for (const h of hits) {
    box(m.by.Steel, h.p, norm(add(h.fa, h.fb)), 1.4, 0.22, K.tieTop, K.railTop - K.tieTop, COL.steel, 0);
    for (let w = 0; w < 2; w++) { const ri = w ? h.rb : h.ra, side = -(w ? h.sb : h.sa), f = distTo(routes[ri], h.p).f;
      const c = add(h.p, mul(rightOf(f), side * (gauge + K.railHead))), key = ri * 2 + (side > 0 ? 1 : 0);
      if (checks.some(ck => ck.k === key && len(sub(ck.c, c)) < 1.5)) continue; checks.push({k: key, c});
      const inn = mul(rightOf(f), -side * K.checkGap);
      railStrip(m, [sub(add(c, inn), mul(f, 1.6)), add(add(c, inn), mul(f, 1.6))], [f, f], 0, K.railTop - 0.02); }
  }
  return hits.length;
}
function pointMachine(m, p, f, side) { box(m.by.Prop, add(add(p, mul(f, 0.9)), mul(rightOf(f), side * 1.9)), f, 1.0, 0.45, 0.12, 0.4, [0.55, 0.57, 0.52], K.cellMachine); }
function levelCrossingProps(m, r, w, lod) {
  const prop = m.by.Prop, paint = m.by.Paint, nr = rightOf(r), half = w * 0.5, deckHalf = K.bedTopHalf + 0.2, sinA = Math.max(0.2, Math.abs(cross(v(1, 0), r)));
  const edgeX = (e, z) => (e - z * nr.z) / nr.x, yD = K.railTop + 0.005;
  if (Math.abs(nr.x) > 1e-3) quad(prop, P3(v(edgeX(-half, -deckHalf), -deckHalf), yD), P3(v(edgeX(half, -deckHalf), -deckHalf), yD), P3(v(edgeX(half, deckHalf), deckHalf), yD),
    P3(v(edgeX(-half, deckHalf), deckHalf), yD), [0.34, 0.34, 0.35], [K.cellDeck, 0, K.cellDeck + 0.25, 0, K.cellDeck + 0.25, 1, K.cellDeck, 1]);
  const clear = deckHalf / sinA, z8 = [0, 0, 0, 0, 0, 0, 0, 0], yP = 0.02;
  for (const sg of [1, -1]) {
    const d = mul(r, sg), right = rightOf(d), back = mul(d, -1);
    const sc = mul(back, clear + 3.5), a = add(sc, mul(right, 0.2)), b = add(sc, mul(right, half - 0.2)), bw = mul(d, 0.4);
    quad(paint, P3(a, yP), P3(b, yP), P3(add(b, bw), yP), P3(add(a, bw), yP), COL.paint, z8);
    if (!lod) { const xc = add(mul(back, clear + 12), mul(right, half * 0.5));
      for (const k of [-1, 1]) { const u = mul(norm(add(d, mul(right, k))), 2.6), t = mul(rightOf(norm(u)), 0.2);
        quad(paint, P3(sub(sub(xc, u), t), yP), P3(sub(add(xc, u), t), yP), P3(add(add(xc, u), t), yP), P3(add(sub(xc, u), t), yP), COL.paint, z8); } }
    const mast = add(mul(back, clear + 2.5), mul(right, half + 0.7)), ax = mul(d, 0.45), az = mul(right, 0.45);
    card(prop, sub(mast, az), add(mast, az), 0, 4.2, K.cellMast);
    if (!lod) card(prop, sub(mast, ax), add(mast, ax), 0, 4.2, K.cellMast);
    card(prop, add(mast, mul(right, -0.3)), add(mast, mul(right, -0.3 - half)), 1.0, 1.18, K.cellBoom);
  }
  if (!lod) box(prop, v(half / sinA + 4, -(K.bedFootHalf + 1.2)), v(1, 0), 1.4, 1.0, 0, 1.6, [0.62, 0.63, 0.60], K.cellMachine);
}
export function adjustParams(p) {
  const q = {...p}; q.radius = Math.max(q.radius, K.minRadius); q.gauge = clamp(q.gauge, 0.6, 1.7);
  if (q.type === 'Diamond') q.angleDeg = clamp(q.angleDeg, K.diamondMin, 90);
  else if (q.type === 'SingleSlip') q.angleDeg = clamp(q.angleDeg, K.diamondMin, K.slipMax);
  else if (q.type === 'LevelCrossing') { q.angleDeg = clamp(q.angleDeg, K.levelMin, 90); q.roadWidth = clamp(q.roadWidth, 4, 30); }
  else if (q.type === 'Crossover' || q.type === 'Scissors') { q.spacing = clamp(q.spacing, K.spacingMin, K.spacingMax);
    q.angleDeg = clamp(q.angleDeg, K.turnoutMin, Math.min(K.turnoutMax, crossoverMaxAngle(q.spacing, q.radius))); }
  else q.angleDeg = clamp(q.angleDeg, K.turnoutMin, K.turnoutMax);
  const notes = [];
  if (q.angleDeg !== p.angleDeg) notes.push('angle limited to ' + q.angleDeg.toFixed(1) + ' deg');
  if (q.radius !== p.radius) notes.push('radius at least 45 m');
  if (q.spacing !== p.spacing && (p.type === 'Crossover' || p.type === 'Scissors')) notes.push('spacing 3.8 to 12 m');
  q.note = notes.join(', ');
  return q;
}
export function junctionRoutes(p) {
  const R = [], virt = [], s = p.hand === 'Left' ? -1 : 1, a = p.angleDeg, Rr = p.radius, O = v(0, 0), X = v(1, 0);
  const put = (r, isV = false) => { R.push(r); virt.push(isV); };
  const stem = () => { const r = []; straight(r, v(-p.stem, 0), X, p.stem); put(r); };
  const lead = turnoutLead(a, Rr);
  const t = p.type;
  if (t === 'Turnout') { stem(); const st = [], dv = []; straight(st, O, X, lead + p.tail); put(st); arc(dv, O, X, Rr, a, s, p.lod); tail(dv, p.tail); put(dv); }
  else if (t === 'Wye' || t === 'ThreeWay') { stem(); const st = [], l = [], r = []; straight(st, O, X, lead + p.tail); put(st, t === 'Wye');
    arc(l, O, X, Rr, a, -1, p.lod); tail(l, p.tail); put(l); arc(r, O, X, Rr, a, 1, p.lod); tail(r, p.tail); put(r); }
  else if (t === 'Diamond' || t === 'SingleSlip') {
    const th = rad(a), T = t === 'SingleSlip' ? Rr * Math.tan(th * 0.5) : 0, H = Math.max(8 + 1.6 / Math.sin(th), T + 5), b = v(Math.cos(th), s * Math.sin(th));
    const A = [], B = []; straight(A, v(-H, 0), X, 2 * H); put(A); straight(B, mul(b, -H), b, 2 * H); put(B);
    if (t === 'SingleSlip') { const sl = []; arc(sl, v(-T, 0), X, Rr, a, s, p.lod); put(sl); } }
  else if (t === 'Crossover' || t === 'Scissors') {
    const run = crossoverRun(p.spacing, a, Rr), o = turnoutOffset(a, Rr), D = Math.max(0, p.spacing - 2 * o) / Math.sin(rad(a));
    const A = [], B = [], x = []; straight(A, v(-p.stem, 0), X, run + 2 * p.stem); put(A); straight(B, v(-p.stem, s * p.spacing), X, run + 2 * p.stem); put(B);
    arc(x, O, X, Rr, a, s, p.lod); tail(x, D); arc(x, O, X, Rr, a, -s, p.lod); put(x);
    if (t === 'Scissors') { const y = []; arc(y, v(0, s * p.spacing), X, Rr, a, -s, p.lod); tail(y, D); arc(y, O, X, Rr, a, s, p.lod); put(y); } }
  else if (t === 'LevelCrossing') { const th = rad(a), H = (p.roadWidth * 0.5) / Math.sin(th) + 1.8 / Math.tan(th) + 10, A = []; straight(A, v(-H, 0), X, 2 * H); put(A); }
  return {routes: R, virt};
}
export const DEFAULTS = {type: 'Turnout', hand: 'Right', angleDeg: 12, radius: 150, gauge: K.gauge, stem: 4, tail: 6, spacing: 5, roadWidth: 9, lod: 0};
export function buildJunction(want) {
  const p = adjustParams({...DEFAULTS, ...want}), m = newMesh();
  m.params = p;
  const {routes, virt} = junctionRoutes(p);
  routes.forEach((r, k) => { if (!virt[k]) bed(m, r, p.lod); });
  const level = p.type === 'LevelCrossing', road = v(Math.cos(rad(p.angleDeg)), Math.sin(rad(p.angleDeg))), roadHalf = p.roadWidth * 0.5 + 0.3;
  const under = q => level && Math.abs(dot(q, rightOf(road))) < roadHalf;
  if (!p.lod) ties(m, routes, virt, p.gauge, under);
  routes.forEach((r, k) => { if (!virt[k]) rails(m, r, p.gauge, p.lod); });
  m.frogs = (!p.lod && !level) ? frogs(m, routes, virt, p.gauge) : 0;
  const s = p.hand === 'Left' ? -1 : 1, O = v(0, 0), X = v(1, 0), nX = v(-1, 0);
  if (!p.lod) {
    if (p.type === 'Turnout') pointMachine(m, O, X, -s);
    else if (p.type === 'Wye') pointMachine(m, O, X, -1);
    else if (p.type === 'ThreeWay') { pointMachine(m, O, X, -1); pointMachine(m, O, X, 1); }
    else if (p.type === 'SingleSlip') pointMachine(m, v(-p.radius * Math.tan(rad(p.angleDeg) * 0.5), 0), X, -s);
    else if (p.type === 'Crossover' || p.type === 'Scissors') { const run = crossoverRun(p.spacing, p.angleDeg, p.radius);
      pointMachine(m, O, X, -s); pointMachine(m, v(run, s * p.spacing), nX, -s);
      if (p.type === 'Scissors') { pointMachine(m, v(0, s * p.spacing), X, s); pointMachine(m, v(run, 0), nX, s); } }
  }
  if (level) levelCrossingProps(m, road, p.roadWidth, p.lod);
  m.routes = routes; m.virt = virt;
  m.tris = k => k ? m.by[k].p.length / 9 : MATS.reduce((n, q) => n + m.by[q].p.length / 9, 0);
  m.draws = MATS.filter(q => m.by[q].p.length).length;
  return m;
}
// rail_junction_mesh.h JunctionCatalog (LOD 0 rows; LOD 1 alongside)
export const CATALOG = [
  {type: 'Turnout', hand: 'Left', angleDeg: 12}, {type: 'Turnout', hand: 'Right', angleDeg: 12}, {type: 'Wye', hand: 'None', angleDeg: 8},
  {type: 'ThreeWay', hand: 'None', angleDeg: 12}, {type: 'Diamond', hand: 'Right', angleDeg: 30}, {type: 'Diamond', hand: 'Right', angleDeg: 90},
  {type: 'SingleSlip', hand: 'Right', angleDeg: 15}, {type: 'Crossover', hand: 'Right', angleDeg: 10}, {type: 'Crossover', hand: 'Left', angleDeg: 10},
  {type: 'Scissors', hand: 'Right', angleDeg: 10}, {type: 'LevelCrossing', hand: 'None', angleDeg: 90}, {type: 'LevelCrossing', hand: 'None', angleDeg: 45},
];
