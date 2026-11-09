// Viewer for rail_junction_gen.js: one junction with sliders, or every catalog variant in a grid.
import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {buildJunction, CATALOG, MATS} from './rail_junction_gen.js';

const R = new THREE.WebGLRenderer({antialias: true});
R.setPixelRatio(devicePixelRatio); R.setSize(innerWidth, innerHeight); document.body.appendChild(R.domElement);
const scene = new THREE.Scene(); scene.background = new THREE.Color(0x9fb8c8);
const cam = new THREE.PerspectiveCamera(40, innerWidth / innerHeight, 0.1, 3000);
const oc = new OrbitControls(cam, R.domElement); oc.maxPolarAngle = Math.PI * 0.49;
scene.add(new THREE.HemisphereLight(0xdfeaff, 0x58603f, 1.1));
const sun = new THREE.DirectionalLight(0xfff2d8, 2.2); sun.position.set(-40, 80, 30); scene.add(sun);
const ground = new THREE.Mesh(new THREE.PlaneGeometry(4000, 4000), new THREE.MeshLambertMaterial({color: 0x5d6b41}));
ground.rotation.x = -Math.PI / 2; ground.position.y = -0.02; scene.add(ground);

// ---------------------------------------------------------------- procedural textures (stand-ins for the game's set)
function canvasTex(w, h, draw, repeat = true) {
  const c = document.createElement('canvas'); c.width = w; c.height = h; draw(c.getContext('2d'), w, h);
  const t = new THREE.CanvasTexture(c); t.colorSpace = THREE.SRGBColorSpace; t.anisotropy = 8; t.flipY = false; // v = 0 at the image top, as in D3D
  if (repeat) t.wrapS = t.wrapT = THREE.RepeatWrapping;
  return t;
}
let seed = 7; const rnd = () => (seed = (seed * 16807) % 2147483647) / 2147483647;
function gravel(g, w, h, base) {
  g.fillStyle = base; g.fillRect(0, 0, w, h);
  for (let i = 0; i < w * h / 6; i++) { const l = 120 + rnd() * 110 | 0; g.fillStyle = `rgba(${l},${l - 6},${l - 14},0.55)`; g.fillRect(rnd() * w, rnd() * h, 1 + rnd() * 2.5, 1 + rnd() * 2.5); }
}
const TEX = {
  Ballast: canvasTex(256, 256, (g, w, h) => gravel(g, w, h, '#d8d2c8')),
  Tie: canvasTex(128, 128, (g, w, h) => { // v: one tie per repeat, the tie in the middle 40 %
    gravel(g, w, h, '#7a756c');
    const y0 = h * 0.3, y1 = h * 0.7; g.fillStyle = '#5b4a3a'; g.fillRect(0, y0, w, y1 - y0);
    for (let i = 0; i < 14; i++) { g.strokeStyle = `rgba(30,20,12,${0.25 + rnd() * 0.3})`; g.beginPath(); const y = y0 + rnd() * (y1 - y0); g.moveTo(0, y); g.lineTo(w, y + rnd() * 2 - 1); g.stroke(); }
    g.fillStyle = '#3a2e24'; g.fillRect(0, y0, w, 2); g.fillRect(0, y1 - 2, w, 2);
  }),
  Steel: canvasTex(64, 64, (g, w, h) => { g.fillStyle = '#e8e8ea'; g.fillRect(0, 0, w, h); for (let i = 0; i < 300; i++) { g.fillStyle = `rgba(90,80,70,${rnd() * 0.25})`; g.fillRect(rnd() * w, rnd() * h, 2, 1); } }),
  Prop: canvasTex(512, 256, (g, w, h) => { // atlas: machine | mast card | boom | deck
    const c = w / 4; g.clearRect(0, 0, w, h);
    g.fillStyle = '#9aa092'; g.fillRect(0, 0, c, h); g.strokeStyle = '#5e6359'; g.lineWidth = 3; g.strokeRect(10, 10, c - 20, h / 2 - 20); g.strokeRect(10, h / 2 + 10, c - 20, h / 2 - 20);
    const mx = c + c / 2; g.fillStyle = '#d9d9d9'; g.fillRect(mx - 4, 0, 8, h); // mast pole (v up = image up after flipY)
    g.save(); g.translate(mx, 40); for (const a of [0.6, -0.6]) { g.save(); g.rotate(a); g.fillStyle = '#111'; g.fillRect(-56, -9, 112, 18); g.fillStyle = '#fff'; g.fillRect(-53, -6, 106, 12); g.restore(); } g.restore();
    g.fillStyle = '#222'; g.fillRect(mx - 46, 80, 92, 26); g.fillStyle = '#e02018'; g.beginPath(); g.arc(mx - 28, 93, 10, 0, 7); g.arc(mx + 28, 93, 10, 0, 7); g.fill();
    g.fillStyle = '#444'; g.fillRect(mx - 16, h - 40, 32, 40);
    for (let i = 0; i < 8; i++) { g.fillStyle = i % 2 ? '#fff' : '#d01818'; g.fillRect(2 * c + i * c / 8, 0, c / 8, h); }
    g.fillStyle = '#4a4a4c'; g.fillRect(3 * c, 0, c, h); g.strokeStyle = '#2c2c2e'; g.lineWidth = 4; for (let i = 0; i <= 4; i++) { g.beginPath(); g.moveTo(3 * c + i * c / 4, 0); g.lineTo(3 * c + i * c / 4, h); g.stroke(); }
  }, false),
};
const MAT = {}, WIRE = new THREE.MeshBasicMaterial({color: 0x102030, wireframe: true});
function makeMats(useTex) {
  for (const k of MATS) {
    const vc = k === 'Ballast' || k === 'Steel' || k === 'Paint';
    MAT[k] = new THREE.MeshLambertMaterial({vertexColors: vc, map: useTex && k !== 'Paint' ? TEX[k] : null, color: !useTex && !vc ? (k === 'Tie' ? 0x5b4a3a : 0xbbbbbb) : 0xffffff,
      side: k === 'Prop' ? THREE.DoubleSide : THREE.FrontSide, /* windings are consistent: culling keeps every face */ alphaTest: k === 'Prop' ? 0.5 : 0, transparent: false,
      polygonOffset: k === 'Paint', polygonOffsetFactor: -2});
  }
}
export {scene, cam, oc, R, MAT, WIRE, makeMats, THREE, buildJunction, CATALOG, MATS};

// ---------------------------------------------------------------- scene from a junction mesh
const group = new THREE.Group(); scene.add(group);
const labels = [];
function addJunction(m, x, z, wire) {
  const g = new THREE.Group(); g.position.set(x, 0, z);
  for (const k of MATS) {
    const b = m.by[k]; if (!b.p.length) continue;
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute(b.p, 3));
    geo.setAttribute('normal', new THREE.Float32BufferAttribute(b.n, 3));
    geo.setAttribute('color', new THREE.Float32BufferAttribute(b.c, 3));
    geo.setAttribute('uv', new THREE.Float32BufferAttribute(b.uv, 2));
    g.add(new THREE.Mesh(geo, MAT[k]));
    if (wire) g.add(new THREE.Mesh(geo, WIRE));
  }
  group.add(g);
  return g;
}
function clear() {
  for (const c of [...group.children]) { group.remove(c); c.traverse(o => o.geometry && o.geometry.dispose()); }
  for (const l of labels) l.el.remove(); labels.length = 0;
}
function label(text, pos) { const el = document.createElement('div'); el.className = 'lbl'; el.textContent = text; document.body.appendChild(el); labels.push({el, pos}); }
function placeLabels() {
  for (const l of labels) { const p = l.pos.clone().project(cam); const vis = p.z < 1;
    l.el.style.display = vis ? '' : 'none'; l.el.style.left = (p.x * 0.5 + 0.5) * innerWidth + 'px'; l.el.style.top = (-p.y * 0.5 + 0.5) * innerHeight + 'px'; }
}
const NAMES = {Turnout: 'Turnout', Wye: 'Wye', ThreeWay: '3-way', Diamond: 'Diamond', SingleSlip: 'Single slip', Crossover: 'Crossover', Scissors: 'Scissors crossover', LevelCrossing: 'Level crossing'};
function bounds(m) {
  let x0 = 1e9, x1 = -1e9, z0 = 1e9, z1 = -1e9;
  for (const k of MATS) { const p = m.by[k].p; for (let i = 0; i < p.length; i += 3) { x0 = Math.min(x0, p[i]); x1 = Math.max(x1, p[i]); z0 = Math.min(z0, p[i + 2]); z1 = Math.max(z1, p[i + 2]); } }
  return {x0, x1, z0, z1};
}
// a road strip under a level crossing, for context only (not part of the model)
function contextRoad(m, g) {
  const p = m.params; if (p.type !== 'LevelCrossing') return;
  const a = p.angleDeg * Math.PI / 180, road = new THREE.Mesh(new THREE.PlaneGeometry(90, p.roadWidth), new THREE.MeshLambertMaterial({color: 0x3a3b3e}));
  road.rotation.x = -Math.PI / 2; road.rotation.z = -a; road.position.y = 0.01; g.add(road);
  const ln = new THREE.Mesh(new THREE.PlaneGeometry(90, 0.15), new THREE.MeshBasicMaterial({color: 0xe8c040}));
  ln.rotation.x = -Math.PI / 2; ln.rotation.z = -a; ln.position.y = 0.02; g.add(ln);
}
export {group, labels, addJunction, clear, label, placeLabels, NAMES, bounds, contextRoad};

// ---------------------------------------------------------------- UI
const $ = id => document.getElementById(id);
const SL = ['angle', 'radius', 'spacing', 'road', 'tail', 'gauge'];
const PRESET = {Turnout: 12, Wye: 8, ThreeWay: 12, Diamond: 30, SingleSlip: 15, Crossover: 10, Scissors: 10, LevelCrossing: 60};
function params() {
  return {type: $('type').value, hand: $('hand').value, angleDeg: +$('angle').value, radius: +$('radius').value, spacing: +$('spacing').value,
    roadWidth: +$('road').value, tail: +$('tail').value, gauge: +$('gauge').value, lod: +$('lod').value};
}
function statsText(m) {
  const rows = MATS.filter(k => m.tris(k)).map(k => `${k.padEnd(8)} ${String(m.tris(k)).padStart(5)} tris`);
  return rows.join('\n') + `\n${'total'.padEnd(8)} ${String(m.tris()).padStart(5)} tris, ${m.draws} draws` + (m.params.lod ? '' : `, ${m.frogs} frogs`);
}
let lastKey = '';
function rebuild(fit) {
  for (const s of SL) $(s + 'O').textContent = (+$(s).value).toString();
  makeMats($('tex').checked);
  clear();
  const wire = $('wire').checked;
  if ($('view').value === 'one') {
    const m = buildJunction(params());
    const g = addJunction(m, 0, 0, wire); contextRoad(m, g);
    $('note').textContent = m.params.note ? 'Adjusted: ' + m.params.note : '';
    const l1 = buildJunction({...params(), lod: 1 - +$('lod').value});
    $('stats').textContent = statsText(m) + `\n(LOD ${1 - m.params.lod}: ${l1.tris()} tris)`;
    const b = bounds(m), cx = (b.x0 + b.x1) / 2, cz = (b.z0 + b.z1) / 2, r = Math.max(b.x1 - b.x0, b.z1 - b.z0);
    if (fit) { oc.target.set(cx, 0, cz); cam.position.set(cx - r * 0.35, r * 0.55, cz + r * 0.75); }
  } else {
    $('note').textContent = ''; $('stats').textContent = '';
    let x = 0, z = 0, col = 0, rowH = 0;
    CATALOG.forEach(c => {
      const m = buildJunction({...c, lod: +$('lod').value, hand: c.hand === 'None' ? 'Right' : c.hand}), b = bounds(m);
      const w = b.x1 - b.x0, h = b.z1 - b.z0;
      if (col === 3) { col = 0; x = 0; z += rowH + 22; rowH = 0; }
      const g = addJunction(m, x - b.x0, z - b.z0, wire); contextRoad(m, g);
      label(`${NAMES[c.type]}${c.hand !== 'None' ? ' ' + c.hand.toLowerCase() : ''} ${m.params.angleDeg} deg: ${m.tris()} tris`, new THREE.Vector3(x + w / 2, 6, z + h / 2 - h / 2 - 2));
      x += Math.max(w, 60) + 18; rowH = Math.max(rowH, h); col++;
    });
    if (fit) { oc.target.set(130, 0, 60); cam.position.set(130, 260, 200); }
  }
  oc.update();
}
function table() {
  let h = '<table><tr><th>variant</th><th>deg</th><th>LOD0</th><th>LOD1</th><th>draws</th></tr>';
  for (const c of CATALOG) {
    const p = {...c, hand: c.hand === 'None' ? 'Right' : c.hand}, a = buildJunction(p), b = buildJunction({...p, lod: 1});
    h += `<tr><td>${NAMES[c.type]}${c.hand !== 'None' ? ' ' + c.hand.toLowerCase() : ''}</td><td>${c.angleDeg}</td><td>${a.tris()}</td><td>${b.tris()}</td><td>${a.draws}</td></tr>`;
  }
  $('tbl').innerHTML = h + '</table>';
}
$('type').onchange = () => { $('angle').value = PRESET[$('type').value]; rebuild(true); };
for (const id of ['hand', 'lod', 'wire', 'tex']) $(id).onchange = () => rebuild(false);
$('view').onchange = () => rebuild(true);
for (const s of SL) $(s).oninput = () => rebuild(false);
// URL: ?type=Diamond&angle=30&view=all ... (for captures)
const q = new URLSearchParams(location.search);
for (const [k, v] of q) if ($(k)) { if ($(k).type === 'checkbox') $(k).checked = v === '1'; else $(k).value = v; }
if (q.has('type') && !q.has('angle')) $('angle').value = PRESET[$('type').value];
table(); rebuild(true);
addEventListener('resize', () => { R.setSize(innerWidth, innerHeight); cam.aspect = innerWidth / innerHeight; cam.updateProjectionMatrix(); });
(function loop() { oc.update(); R.render(scene, cam); placeLabels(); requestAnimationFrame(loop); })();
window.__junction = {buildJunction, CATALOG};
