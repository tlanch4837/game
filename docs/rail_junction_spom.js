// SPOM plane preview: the junction box and plain track boxes ray-marched against their height maps (a GLSL port of
// TrackPS in src/shaders/mesh.hlsl: march, bisect, discard on a miss, normal from the height, sun shadow).
import * as THREE from 'three';

export const RAIL_TOP = 0.5, DEPTH = 0.55; // the box: rail head down to the formation (track base frame of the meshes)

const VS = `
out vec3 vLocal;
void main() { vLocal = position; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`;

const FS = `
precision highp float;
in vec3 vLocal;
uniform mat4 projectionMatrix;
out vec4 fragColor;
uniform sampler2D hTex, cTex;
uniform int uMode;          // 0: one picture over the rect, 1: the plain track tile (4.4 m across, repeats every pitch)
uniform vec2 uO, uF;        // box frame: origin and forward (xz, in the group's frame)
uniform vec4 uRect;         // along a0, across b0, along a1, across b1
uniform vec4 uTex;          // mode 0: texture rect (x0, z0, x1, z1) of the canonical layout
uniform float uMirror, uYT, uYB, uSteps;
uniform vec3 uCamL, uSun, uOrigin;
vec2 UV(vec2 ab) {
  if (uMode == 1) return vec2((ab.y + 2.2) / 4.4, ab.x / 0.6);
  return vec2((ab.y * uMirror - uTex.y) / (uTex.w - uTex.y), (ab.x - uTex.x) / (uTex.z - uTex.x));
}
float Hgt(vec2 ab, out vec2 uv) {
  uv = UV(ab);
  float h = textureLod(hTex, uv, 0.0).r;              // SampleLevel, as TrackPS: no derivatives inside the march
  if (h < 0.5 / 255.0) return -1e9;                 // a hole: no surface
  return uYB + (uYT - uYB) * h;
}
float H(vec3 p) { vec2 uv; return Hgt(p.xz, uv); } // p = (a, y, b)
vec3 toFrame(vec3 l) { vec2 d = l.xz - uO; return vec3(dot(d, uF), l.y, dot(d, vec2(-uF.y, uF.x))); }
vec3 dirFrame(vec3 d) { return vec3(dot(d.xz, uF), d.y, dot(d.xz, vec2(-uF.y, uF.x))); }
vec3 dirLocal(vec3 f) { vec2 R = vec2(-uF.y, uF.x); vec2 xz = uF * f.x + R * f.z; return vec3(xz.x, f.y, xz.y); }
void main() {
  vec3 p0 = toFrame(vLocal), d = normalize(dirFrame(vLocal - uCamL));
  vec2 uvIn = UV(p0.xz), gx = dFdx(uvIn), gy = dFdy(uvIn);  // footprint for the albedo's filtering
  d = vec3(abs(d.x) < 1e-5 ? 1e-5 : d.x, abs(d.y) < 1e-5 ? -1e-5 : d.y, abs(d.z) < 1e-5 ? 1e-5 : d.z);
  vec3 lo = vec3(uRect.x, uYB, uRect.y), hi = vec3(uRect.z, uYT, uRect.w);
  vec3 t1 = (lo - p0) / d, t2 = (hi - p0) / d, tf = max(t1, t2);
  float tExit = max(min(min(tf.x, tf.y), tf.z), 0.0);
  float t = 0.0; bool hit = false;
  if (uSteps > 0.5) {
    float dt = tExit / uSteps, tl = 0.0, th = 0.0;
    for (int k = 0; k <= 64; ++k) {                 // fixed count, no break: ANGLE / D3D compile it reliably
      if (!hit && float(k) <= uSteps) {
        th = dt * float(k);
        vec3 q = p0 + d * th;
        if (q.y <= H(q)) hit = true; else tl = th;
      }
    }
    if (!hit) discard;
    for (int r = 0; r < 6; ++r) { float tm = 0.5 * (tl + th); vec3 q = p0 + d * tm; if (q.y <= H(q)) th = tm; else tl = tm; }
    t = th;
  } else if (H(p0) < -1e8) discard;
  vec3 q = p0 + d * t;
  vec2 uv; float h0 = Hgt(q.xz, uv);
  vec3 alb = textureGrad(cTex, uv, gx, gy).rgb;
  vec3 nf = vec3(0.0, 1.0, 0.0);
  if (uSteps > 0.5) {
    float e = 0.02;
    float hx = H(q + vec3(e, 0, 0)) - H(q - vec3(e, 0, 0)), hz = H(q + vec3(0, 0, e)) - H(q - vec3(0, 0, e));
    if (abs(hx) < 1.0 && abs(hz) < 1.0) nf = normalize(vec3(-hx / (2.0 * e), 1.0, -hz / (2.0 * e)));
    if (q.y < h0 - 0.003) { // came in through a side (an if, not ?: : ANGLE's D3D back end fails on that form)
      if (abs(d.x) > abs(d.z)) nf = vec3(d.x > 0.0 ? -1.0 : 1.0, 0.0, 0.0);
      else nf = vec3(0.0, 0.0, d.z > 0.0 ? -1.0 : 1.0);
    }
  }
  vec3 n = normalize(dirLocal(nf));
  vec3 lt = dirFrame(uSun);
  float shade = 1.0;
  if (uSteps > 0.5 && lt.y > 0.02) for (int s = 1; s <= 6; ++s) { vec3 sp = q + lt * (float(s) * 0.06 + 0.004); if (sp.y <= H(sp)) shade = 0.45; }
  float diff = max(dot(n, normalize(uSun)), 0.0);
  vec3 c = alb * (0.42 + 0.75 * diff * shade);
  // world hit -> depth, so the plane sorts like real relief against the props and the road
  vec2 R = vec2(-uF.y, uF.x);
  vec2 xz = uO + uF * q.x + R * q.z;
  vec4 clip = projectionMatrix * viewMatrix * vec4(vec3(xz.x, q.y, xz.y) + uOrigin, 1.0);
  gl_FragDepth = clamp(clip.z / clip.w * 0.5 + 0.5, 0.0, 1.0);
  fragColor = vec4(c, 1.0);
}`;

// A box (top and four sides, no bottom) over the frame rect [a0..a1] x [b0..b1], rail head to formation.
export function spomBox(o, f, rect, uniforms) {
  const [a0, b0, a1, b1] = rect, R = {x: -f.z, z: f.x};
  const P = (a, b, y) => [o.x + f.x * a + R.x * b, y, o.z + f.z * a + R.z * b];
  const yT = RAIL_TOP, yB = RAIL_TOP - DEPTH;
  const c = [[a0, b0], [a1, b0], [a1, b1], [a0, b1]], pos = [];
  const quad = (A, B, C, D) => pos.push(...A, ...B, ...C, ...A, ...C, ...D);
  quad(P(a0, b0, yT), P(a0, b1, yT), P(a1, b1, yT), P(a1, b0, yT));
  for (let k = 0; k < 4; k++) { const [pa, pb] = [c[k], c[(k + 1) % 4]]; quad(P(pa[0], pa[1], yT), P(pb[0], pb[1], yT), P(pb[0], pb[1], yB), P(pa[0], pa[1], yB)); }
  const g = new THREE.BufferGeometry(); g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
  const m = new THREE.ShaderMaterial({glslVersion: THREE.GLSL3, vertexShader: VS, fragmentShader: FS, side: THREE.DoubleSide,
    uniforms: {hTex: {value: null}, cTex: {value: null}, uMode: {value: 0}, uO: {value: new THREE.Vector2(o.x, o.z)}, uF: {value: new THREE.Vector2(f.x, f.z)},
      uRect: {value: new THREE.Vector4(a0, b0, a1, b1)}, uTex: {value: new THREE.Vector4()}, uMirror: {value: 1}, uYT: {value: yT}, uYB: {value: yB},
      uSteps: {value: 48}, uCamL: {value: new THREE.Vector3()}, uSun: {value: new THREE.Vector3(-0.4, 0.8, 0.3).normalize()}, uOrigin: {value: new THREE.Vector3()}, ...uniforms}});
  const mesh = new THREE.Mesh(g, m);
  mesh.onBeforeRender = (r, s, cam) => { m.uniforms.uCamL.value.copy(cam.position).sub(m.uniforms.uOrigin.value); };
  return mesh;
}

// ---------------------------------------------------------------------------------------------------- the page
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {buildJunction, junctionRoutes, adjustParams, DEFAULTS} from './rail_junction_gen.js';

const R = new THREE.WebGLRenderer({antialias: true});
R.setPixelRatio(devicePixelRatio); R.setSize(innerWidth, innerHeight); document.body.appendChild(R.domElement);
const scene = new THREE.Scene(); scene.background = new THREE.Color(0x9fb8c8);
const cam = new THREE.PerspectiveCamera(40, innerWidth / innerHeight, 0.05, 3000);
const oc = new OrbitControls(cam, R.domElement); oc.maxPolarAngle = Math.PI * 0.495;
scene.add(new THREE.HemisphereLight(0xdfeaff, 0x58603f, 1.1));
const sun = new THREE.DirectionalLight(0xfff2d8, 2.2); sun.position.set(-40, 80, 30); scene.add(sun);
const ground = new THREE.Mesh(new THREE.PlaneGeometry(4000, 4000), new THREE.MeshLambertMaterial({color: 0x5d6b41}));
ground.rotation.x = -Math.PI / 2; ground.position.y = -0.22; scene.add(ground); // terrain: the node level (track base - 0.22)
const group = new THREE.Group(); scene.add(group);
const $ = id => document.getElementById(id);
const tl = new THREE.TextureLoader(), texCache = {};
function tex(name, repeat, srgb) {
  const k = name + repeat;
  if (!texCache[k]) { const t = tl.load('rail_junction_tex/' + name); t.flipY = false; t.anisotropy = 8;
    t.wrapS = THREE.ClampToEdgeWrapping; t.wrapT = repeat ? THREE.RepeatWrapping : THREE.ClampToEdgeWrapping;
    if (srgb) t.colorSpace = THREE.NoColorSpace; texCache[k] = t; }
  return texCache[k];
}
const TYPES = {'Turnout': 'Turnout', 'Wye': 'Wye', '3-way': 'ThreeWay', 'Diamond': 'Diamond', 'Single slip': 'SingleSlip', 'Crossover': 'Crossover', 'Scissors crossover': 'Scissors', 'Level crossing': 'LevelCrossing'};
let LAYOUTS = [];
const steps = () => +$('steps').value;
const spoms = [];

// Open ends of a layout's routes: where plain track carries on (not ending on another route).
function openEnds(routes, virt) {
  const ends = [];
  const near = (p, k) => routes.some((r, j) => j !== k && !virt[j] && r.some((q, i) => i && segDist(p, r[i - 1].c, q.c) < 0.6));
  routes.forEach((r, k) => { if (virt[k]) return;
    const a = r[0], b = r[r.length - 1];
    if (!near(a.c, k)) ends.push({p: a.c, f: {x: -a.f.x, z: -a.f.z}});
    if (!near(b.c, k)) ends.push({p: b.c, f: b.f}); });
  return ends;
}
function segDist(p, a, b) { const abx = b.x - a.x, abz = b.z - a.z, L2 = abx * abx + abz * abz || 1;
  const t = Math.max(0, Math.min(1, ((p.x - a.x) * abx + (p.z - a.z) * abz) / L2)); return Math.hypot(a.x + abx * t - p.x, a.z + abz * t - p.z); }

// props atlas (same cells as rail_junction_view.js: machine | mast card | boom | deck)
function atlas() {
  const c = document.createElement('canvas'); c.width = 512; c.height = 256; const g = c.getContext('2d'), w = 512, h = 256, q = w / 4;
  g.fillStyle = '#9aa092'; g.fillRect(0, 0, q, h); g.strokeStyle = '#5e6359'; g.lineWidth = 3; g.strokeRect(10, 10, q - 20, h / 2 - 20); g.strokeRect(10, h / 2 + 10, q - 20, h / 2 - 20);
  const mx = q + q / 2; g.fillStyle = '#d9d9d9'; g.fillRect(mx - 4, 0, 8, h);
  g.save(); g.translate(mx, 40); for (const a of [0.6, -0.6]) { g.save(); g.rotate(a); g.fillStyle = '#111'; g.fillRect(-56, -9, 112, 18); g.fillStyle = '#fff'; g.fillRect(-53, -6, 106, 12); g.restore(); } g.restore();
  g.fillStyle = '#222'; g.fillRect(mx - 46, 80, 92, 26); g.fillStyle = '#e02018'; g.beginPath(); g.arc(mx - 28, 93, 10, 0, 7); g.arc(mx + 28, 93, 10, 0, 7); g.fill();
  g.fillStyle = '#444'; g.fillRect(mx - 16, h - 40, 32, 40);
  for (let i = 0; i < 8; i++) { g.fillStyle = i % 2 ? '#fff' : '#d01818'; g.fillRect(2 * q + i * q / 8, 0, q / 8, h); }
  g.fillStyle = '#4a4a4c'; g.fillRect(3 * q, 0, q, h);
  const t = new THREE.CanvasTexture(c); t.flipY = false; t.colorSpace = THREE.SRGBColorSpace; return t;
}
const PROP = new THREE.MeshLambertMaterial({map: atlas(), side: THREE.DoubleSide, alphaTest: 0.5});
const PAINT = new THREE.MeshLambertMaterial({color: 0xeeeeea, polygonOffset: true, polygonOffsetFactor: -2});
let ROAD = null;
function roadMat() { if (!ROAD) { const t = tl.load('rail_junction_tex/asphalt_albedo.jpg'); t.wrapS = t.wrapT = THREE.RepeatWrapping; t.colorSpace = THREE.SRGBColorSpace; t.anisotropy = 8;
  ROAD = new THREE.MeshLambertMaterial({map: t, side: THREE.DoubleSide}); } return ROAD; }
function meshOf(b, mat) {
  const g = new THREE.BufferGeometry();
  g.setAttribute('position', new THREE.Float32BufferAttribute(b.p, 3)); g.setAttribute('normal', new THREE.Float32BufferAttribute(b.n, 3));
  g.setAttribute('uv', new THREE.Float32BufferAttribute(b.uv, 2)); return new THREE.Mesh(g, mat);
}
// The road through a level crossing (context, the road network's own mesh in the game): asphalt at world uv / 4, the
// same as the crossing's ramps, so the seam can be judged; sidewalks one step up.
function contextRoad(p) {
  const a = p.angleDeg * Math.PI / 180, r = {x: Math.cos(a), z: Math.sin(a)}, n = {x: -r.z, z: r.x}, half = p.roadWidth / 2, L = 60;
  const strip = (e0, e1, y, mat, uvScale) => { const pos = [], uv = [];
    const P = (s, e) => [r.x * s + n.x * e, y, r.z * s + n.z * e];
    for (const [s0, s1] of [[-L, L]]) { const q = [P(s0, e0), P(s1, e0), P(s1, e1), P(s0, e1)];
      for (const i of [0, 1, 2, 0, 2, 3]) { pos.push(...q[i]); uv.push(q[i][0] / uvScale, q[i][2] / uvScale); } }
    const g = new THREE.BufferGeometry(); g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3)); g.setAttribute('uv', new THREE.Float32BufferAttribute(uv, 2));
    g.computeVertexNormals(); const m = new THREE.Mesh(g, mat); m.renderOrder = -1; group.add(m); };
  strip(-half, half, -0.10, roadMat(), 4);
  const side = new THREE.MeshLambertMaterial({color: 0xb9b6ae, side: THREE.DoubleSide});
  strip(half, half + p.sidewalk, 0.05, side, 2); strip(-half - p.sidewalk, -half, 0.05, side, 2);
}

function rebuild(fit) {
  for (const c of [...group.children]) { group.remove(c); c.geometry && c.geometry.dispose(); }
  spoms.length = 0;
  const L = LAYOUTS[+$('layout').value]; if (!L) return;
  const type = TYPES[L.type], handed = !['Wye', 'ThreeWay', 'LevelCrossing'].includes(type);
  const mirror = handed && $('hand').value === 'Left' ? -1 : 1;
  const [x0, z0, x1, z1] = L.rect;
  const box = spomBox({x: 0, z: 0}, {x: 1, z: 0}, [x0, mirror > 0 ? z0 : -z1, x1, mirror > 0 ? z1 : -z0], {});
  Object.assign(box.material.uniforms, {hTex: {value: tex(L.id + '_height.png', false, true)}, cTex: {value: tex(L.id + '_albedo.jpg', false, true)},
    uMode: {value: 0}, uTex: {value: new THREE.Vector4(x0, z0, x1, z1)}, uMirror: {value: mirror}});
  group.add(box); spoms.push(box);
  if ($('wire').checked) { const w = new THREE.Mesh(box.geometry, new THREE.MeshBasicMaterial({color: 0x102030, wireframe: true})); group.add(w); }
  const p = adjustParams({...DEFAULTS, type, hand: mirror < 0 ? 'Left' : 'Right', angleDeg: L.angle, radius: L.radius, gate: +$('gate').value, spom: true, lod: 0});
  const {routes, virt} = junctionRoutes(p);
  let plainTris = 0;
  if ($('plain').checked) for (const e of openEnds(routes, virt)) {
    const t = spomBox(e.p, e.f, [0, -2.2, 18, 2.2], {});
    Object.assign(t.material.uniforms, {hTex: {value: tex('track_height.png', true, true)}, cTex: {value: tex('track_albedo.jpg', true, true)}, uMode: {value: 1}});
    group.add(t); spoms.push(t); plainTris += 10;
  }
  const m = buildJunction(p);
  if (m.by.Prop.p.length) group.add(meshOf(m.by.Prop, PROP));
  if (m.by.Paint.p.length) group.add(meshOf(m.by.Paint, PAINT));
  if (m.by.Road.p.length) group.add(meshOf(m.by.Road, roadMat()));
  if (type === 'LevelCrossing') contextRoad(p);
  for (const s of spoms) s.material.uniforms.uSteps.value = steps();
  const props = m.tris('Prop') + m.tris('Paint') + m.tris('Road'), [nA, nX] = L.tex, n = nA * nX;
  $('stats').textContent = `${L.id}${mirror < 0 ? ' (mirrored)' : ''}: ${L.angle} deg, R ${L.radius} m\n` +
    `plane ${(x1 - x0).toFixed(1)} x ${(z1 - z0).toFixed(1)} m, 10 tris\nprops ${props} tris (props / paint / ramps)\n` +
    `texture ${nA} x ${nX}, ${(n / 1e6).toFixed(2)} Mtexel, holes ${Math.round(L.holes * 100)} %\n` +
    `RGBA8 ${(n * 4 * 4 / 3 / 2 ** 20).toFixed(2)} MB, ASTC 6x6 ${(n * 16 / 36 * 4 / 3 / 2 ** 20).toFixed(2)} MB (mips)\nplain track beside: ${plainTris} tris`;
  if (fit) { const cx = (x0 + x1) / 2, r = Math.max(x1 - x0, z1 - z0) + 20; oc.target.set(cx, 0, 0); cam.position.set(cx - r * 0.3, r * 0.5, r * 0.7); }
  oc.update();
}
fetch('rail_junction_tex/layouts.json').then(r => r.json()).then(d => {
  LAYOUTS = d.layouts;
  $('layout').innerHTML = LAYOUTS.map((l, i) => `<option value="${i}">${l.id} (${l.type} ${l.angle} deg)</option>`).join('');
  const q = new URLSearchParams(location.search);
  for (const [k, v] of q) if ($(k)) { if ($(k).type === 'checkbox') $(k).checked = v === '1'; else $(k).value = v; }
  if (q.has('id')) $('layout').value = LAYOUTS.findIndex(l => l.id === q.get('id'));
  rebuild(true);
});
for (const id of ['hand', 'gate', 'plain', 'wire']) $(id).oninput = () => rebuild(false);
$('layout').onchange = () => rebuild(true);
$('steps').onchange = () => { for (const s of spoms) s.material.uniforms.uSteps.value = steps(); };
addEventListener('resize', () => { R.setSize(innerWidth, innerHeight); cam.aspect = innerWidth / innerHeight; cam.updateProjectionMatrix(); });
(function loop() { oc.update(); R.render(scene, cam); requestAnimationFrame(loop); })();
window.__spom = {cam, oc, rebuild, group, R, scene};
