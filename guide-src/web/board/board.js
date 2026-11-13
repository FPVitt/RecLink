/* RecLink board explorer: a scroll-driven, exploded 3D view built from the board's own layer artwork.
   Textures are the real top/inner/bottom layers; parts are the board's component bodies. */
import * as THREE from './three.module.min.js';

const root = document.getElementById('exp');
const canvas = document.getElementById('bc');
if (root && canvas) start();

function start() {
  const stage = root.querySelector('.exp-sticky');
  const steps = [...root.querySelectorAll('.exp-step')];
  const bar = root.querySelector('.exp-bar i');
  let renderer;
  try {
    renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true, powerPreference: 'low-power' });
  } catch (e) { root.classList.add('nogl'); return; }
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
  renderer.toneMappingExposure = 1.05;

  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(26, 1, 0.5, 200);
  scene.add(new THREE.HemisphereLight(0xffffff, 0x2a3a30, 1.05));
  const sun = new THREE.DirectionalLight(0xffffff, 2.1); sun.position.set(6, 12, 8); scene.add(sun);
  const rim = new THREE.DirectionalLight(0xdfeeff, 2.4); rim.position.set(-3, -12, 6); scene.add(rim);

  const base = new URL('.', import.meta.url).href;
  const J = (n) => fetch(base + n).then((r) => r.json());
  const loader = new THREE.TextureLoader();
  const tex = (n) => new Promise((res, rej) => loader.load(base + n, (t) => {
    t.colorSpace = THREE.SRGBColorSpace; t.anisotropy = 8; res(t);
  }, undefined, rej));

  const LY = [0.5, 0.17, -0.17, -0.5];            // assembled layer heights (mm)
  const names = ['top', 'inner1', 'inner2', 'bottom'];
  const layers = [];                              // {plane, y0}
  const slabs = [];
  const top = new THREE.Group(), bottomParts = new THREE.Group();
  scene.add(top, bottomParts);
  const board = new THREE.Group(); scene.add(board);
  const fr4 = new THREE.MeshStandardMaterial({ color: 0x8c7840, roughness: 0.7, metalness: 0, transparent: true });

  Promise.all([J('tex.json'), J('outline.json'), J('parts.json'), ...names.map((n) => tex(`tex-${n}.webp`))]).then(([tj, oj, pj, ...tx]) => {
    // dielectric slabs between the copper layers
    const shape = new THREE.Shape(oj.outline.map(([x, y]) => new THREE.Vector2(x, y)));
    const th = 0.3;
    
    for (let i = 0; i < 3; i++) {
      const g = new THREE.ExtrudeGeometry(shape, { depth: th, bevelEnabled: false });
      g.rotateX(-Math.PI / 2);                    // shape y -> -z, extrusion -> +y
      g.translate(0, -th / 2, 0);
      const m = new THREE.Mesh(g, fr4); m.userData.ro = 2 * i + 1; board.add(m); slabs.push(m);
    }
    // layer artwork
    names.forEach((n, i) => {
      const geo = new THREE.PlaneGeometry(tj.w, tj.h); geo.rotateX(-Math.PI / 2);
      const outer = i === 0 || i === 3;
      const mat = new THREE.MeshStandardMaterial({
        map: tx[i], transparent: true, alphaTest: outer ? 0.5 : 0, depthWrite: outer,
        side: THREE.DoubleSide, roughness: outer ? 0.42 : 0.55, metalness: outer ? 0.05 : 0.35,
      });
      const p = new THREE.Mesh(geo, mat); p.userData.ro = 2 * i; board.add(p); layers.push({ p, y0: LY[i] });
    });
    // components
    const addParts = (list, grp) => list.forEach((m) => {
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', new THREE.Float32BufferAttribute(m.p, 3));
      g.setIndex(m.i);
      grp.add(new THREE.Mesh(g, new THREE.MeshStandardMaterial({ color: m.c, flatShading: true, roughness: 0.45, metalness: 0.25, side: THREE.DoubleSide })));
    });
    addParts(pj.top, top); addParts(pj.bottom, bottomParts);
    root.classList.add('ready');
    resize(); frame(true);
  }).catch(() => root.classList.add('nogl'));

  // ---- timeline ----
  const K = [ // p, azimuth, polar, componentsLift, layerSpread, distance, shiftY
    [0.00, 35, 54, 0, 0, 25, 0],
    [0.10, 48, 50, 0, 0, 25, 0],
    [0.28, 62, 48, 1, 0, 27, 0.4],
    [0.50, 112, 68, 1, 1, 50, 1.6],
    [0.70, 168, 72, 1, 1, 48, 1.6],
    [0.86, 212, 114, 0.25, 0.55, 36, 0],
    [1.00, 240, 134, 0, 0, 23, 0],
  ];
  const ease = (t) => t * t * (3 - 2 * t);
  function sample(p) {
    let i = 0; while (i < K.length - 2 && p > K[i + 1][0]) i++;
    const a = K[i], b = K[i + 1], t = ease(Math.min(1, Math.max(0, (p - a[0]) / (b[0] - a[0]))));
    return a.map((v, k) => v + (b[k] - v) * t);
  }
  const cur = { az: 35, pol: 54, e1: 0, e2: 0, d: 25, sy: 0 };
  const off = { az: 0, pol: 0 };
  let p = 0, dirty = true, visible = false, raf = 0;

  function progress() {
    const r = root.getBoundingClientRect();
    const span = Math.max(1, r.height - stage.clientHeight);
    return Math.min(1, Math.max(0, -r.top / span));
  }
  function apply(snap) {
    const k = sample(p);
    const f = snap ? 1 : 0.14;
    cur.az += (k[1] - cur.az) * f; cur.pol += (k[2] - cur.pol) * f;
    cur.e1 += (k[3] - cur.e1) * f; cur.e2 += (k[4] - cur.e2) * f;
    cur.d += (k[5] - cur.d) * f; cur.sy += (k[6] - cur.sy) * f;
    const spread = 1 + cur.e2 * 13;
    layers.forEach((l) => { l.p.position.y = l.y0 * spread + (l.y0 > 0 ? 0.004 : -0.004); });
    slabs.forEach((s, i) => { s.position.y = ((LY[i] + LY[i + 1]) / 2) * spread; });
    // the dielectric turns glassy as the stack opens, so the inner copper reads clearly
    const so = 1 - 0.88 * cur.e2;
    fr4.opacity = so; fr4.depthWrite = so > 0.5;
    // part meshes already carry their own height above the board surface (y = 0.5 at the top face)
    top.position.y = LY[0] * (spread - 1) + cur.e1 * 3.2;
    bottomParts.position.y = LY[3] * (spread - 1) - cur.e1 * 3.2;
    const az = (cur.az + off.az) * Math.PI / 180;
    // see-through sheets don't write depth, so draw them far-to-near from wherever the camera is
    const fromBelow = cur.pol + off.pol > 90;
    [...layers.map((l) => l.p), ...slabs].forEach((m) => { m.renderOrder = fromBelow ? m.userData.ro : 6 - m.userData.ro; });
    const pol = Math.min(176, Math.max(4, cur.pol + off.pol)) * Math.PI / 180;
    const d = cur.d * (camera.aspect < 1 ? 2.2 : 1.32);
    camera.position.set(d * Math.sin(pol) * Math.sin(az), d * Math.cos(pol), d * Math.sin(pol) * Math.cos(az));
    camera.lookAt(0, cur.sy, 0);
    renderer.render(scene, camera);
    const settled = Math.abs(k[1] - cur.az) + Math.abs(k[2] - cur.pol) + Math.abs(k[3] - cur.e1) + Math.abs(k[4] - cur.e2) + Math.abs(k[5] - cur.d) < 0.02;
    return !settled;
  }
  function frame(snap) {
    raf = 0;
    p = progress();
    const moving = apply(snap || matchMedia('(prefers-reduced-motion: reduce)').matches);
    steps.forEach((s) => s.classList.toggle('on', p >= +s.dataset.from && p < +s.dataset.to));
    if (bar) bar.style.transform = `scaleX(${p})`;
    root.classList.toggle('started', p > 0.02);
    if ((moving || dirty) && visible) { dirty = false; raf = requestAnimationFrame(() => frame(false)); }
  }
  function kick() { dirty = true; if (!raf && visible) raf = requestAnimationFrame(() => frame(false)); }
  function resize() {
    const w = stage.clientWidth, h = stage.clientHeight;
    renderer.setPixelRatio(Math.min(2, devicePixelRatio || 1));
    renderer.setSize(w, h, false);
    camera.aspect = w / h; camera.updateProjectionMatrix(); kick();
  }
  if (location.search.includes('render')) {   // used to produce the still images on the page
    window.__rl = (az, pol, e1, e2, d) => { Object.assign(cur, { az, pol, e1, e2, d, sy: 0 }); K.length = 0; K.push([0, az, pol, e1, e2, d, 0], [1, az, pol, e1, e2, d, 0]); kick(); };
  }
  addEventListener('scroll', kick, { passive: true });
  new ResizeObserver(resize).observe(stage);
  new IntersectionObserver((e) => { visible = e[0].isIntersecting; if (visible) kick(); }, { rootMargin: '100px' }).observe(root);

  // drag to look around (vertical swipes still scroll the page on touch screens)
  let down = null;
  canvas.addEventListener('pointerdown', (e) => { down = { x: e.clientX, y: e.clientY, az: off.az, pol: off.pol }; canvas.setPointerCapture(e.pointerId); });
  canvas.addEventListener('pointermove', (e) => {
    if (!down) return;
    off.az = down.az - (e.clientX - down.x) * 0.45;
    if (e.pointerType === 'mouse') off.pol = down.pol - (e.clientY - down.y) * 0.3;
    kick();
  });
  const up = () => { down = null; };
  canvas.addEventListener('pointerup', up); canvas.addEventListener('pointercancel', up);
  canvas.addEventListener('dblclick', () => { off.az = 0; off.pol = 0; kick(); });
}
