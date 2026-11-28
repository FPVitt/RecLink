'use strict';
// Drop the post-flash ?v=<ts> cache-buster from the address bar.
if (location.search) { try { history.replaceState({}, '', location.pathname); } catch (e) {} }
const $ = s => document.querySelector(s);
const $$ = s => [...document.querySelectorAll(s)];
const msg = t => { $('#msg').textContent = t; };
const el = (tag, cls) => { const e = document.createElement(tag); if (cls) e.className = cls; return e; };

// ---- tabs ----
function showTab(name) {
  $$('#tabs button').forEach(x => x.classList.toggle('on', x.dataset.t === name));
  $$('.tab').forEach(x => x.classList.toggle('on', x.id === name));
  if (name === 'rid') ridNearPoll();
  camselUi();
}
$$('#tabs button').forEach(b => b.onclick = () => showTab(b.dataset.t));

// cfg.ui_flags: bit0 advanced, bit1 no pw nag (localStorage = paint cache)
let uiFlags = 0;
const advmode = $('#advmode');
function persistFlags() {
  fetch('/api/config', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ui_flags: uiFlags})
  }).catch(() => {});
}
function applyAdv(on) {
  document.body.classList.toggle('adv', on);
  const cur = $('#tabs button.on');
  if (cur && cur.classList.contains('adv-only') && !on) showTab('status');
}
function setAdv(on, persist) {
  uiFlags = on ? (uiFlags | 1) : (uiFlags & ~1);
  advmode.checked = on;
  localStorage.setItem('advmode', on ? '1' : '0');
  applyAdv(on);
  if (persist) persistFlags();
}
setAdv(localStorage.getItem('advmode') === '1', false);
advmode.onchange = () => setAdv(advmode.checked, true);

// ============ Cam control tab: AUX range bars ============
// name= maps 1:1 to /api/config; the last zone's max_us is fixed at 2100 us
const US_MIN = 900, US_MAX = 2100, US_SPAN = US_MAX - US_MIN;
const clampUs = u => u < US_MIN ? US_MIN : u > US_MAX ? US_MAX : u;
const snapUs  = u => Math.round(clampUs(u) / 25) * 25;
const usPct   = u => (clampUs(u) - US_MIN) / US_SPAN * 100;

(function fillAux() {
  let o = '<option value="0">Disabled</option>';
  for (let i = 1; i <= 14; i++) { o += `<option value="${i}">AUX ${i}</option>`; }
  $$('select[data-auxsel]').forEach(s => { s.innerHTML = o; });
})();

const ZONES_MAX = 6, ZONES_MIN = 2;
let presetN = 3;                     // number of radio-switch zones
const zwrap = $('#zone-ids');
function buildZoneRows() {
  zwrap.innerHTML = '';
  for (let i = 0; i < presetN; i++) {
    zwrap.insertAdjacentHTML('beforeend',
      `<div class="pz" data-z="${i}"><span class="zlab">Zone ${i + 1}</span>` +
      (i < presetN - 1 ? `<input type="hidden" name="preset.bands.${i}.max_us">` : '') +
      `<label class="zid"><span class="zt">Mode ID</span>
         <input type="number" inputmode="numeric" name="preset.bands.${i}.id" min="0" step="1"><select class="zsel" aria-label="Camera preset" hidden></select></label>` +
      `<button type="button" class="ghost zcap" data-z="${i}" title="Use the preset the camera is on now" hidden>Capture</button>` +
      `<button type="button" class="ghost zapply" data-z="${i}" title="Switch the connected camera to this zone now">Apply</button></div>`);
  }
  $('#zone-add').disabled = presetN >= ZONES_MAX;
  $('#zone-del').disabled = presetN <= ZONES_MIN;
}
buildZoneRows();
// GoPro simple zones: the camera's presets by name
let gpPre = [];
function zselRender() {
  const on = gpPre.length > 0 && cfgBrand === 'gopro' && getField('preset.gp_mode') === 1;
  $$('#zone-ids .pz').forEach(r => {
    const inp = r.querySelector('input[type=number]'), sel = r.querySelector('.zsel'), v = +inp.value || 0;
    inp.hidden = on; sel.hidden = !on; if (!on) return;
    const o = gpPre.some(p => p[0] === v) ? gpPre : gpPre.concat([[v, '']]);
    const h = o.map(p => `<option value="${p[0]}">${p[1] || 'Preset ' + p[0]}</option>`).join('');
    if (sel.dataset.sig !== h) { sel.dataset.sig = h; sel.innerHTML = h; }
    sel.value = v;
  });
}
zwrap.addEventListener('change', e => { const s = e.target.closest('.zsel');
  if (s) setNum(s.parentNode.querySelector('input').name, +s.value); });
zwrap.addEventListener('click', async e => {
  const cp = e.target.closest('.zcap');                 // GoPro simple zone: take the camera's current preset id
  if (cp) {
    const id = lastStatus && lastStatus.cam_state && lastStatus.cam_state.preset_id;
    if (id === undefined) return msg('The camera has not reported its preset yet.');
    setNum('preset.bands.' + cp.dataset.z + '.id', id); zselRender();
    const p = gpPre.find(x => x[0] === id);
    return msg('zone ' + (+cp.dataset.z + 1) + ' = ' + (p && p[1] ? p[1] : 'preset ' + id));
  }
  const b = e.target.closest('.zapply'); if (!b) return;
  await autoSave();                                     // Apply uses the saved zone
  const i = +b.dataset.z;
  msg((await post('/api/camera/preset', { slot: i })).ok ? 'zone ' + (i + 1) + ' applied' : 'failed');
});

const numAt =name => {
  const e = document.querySelector(`[name="${CSS.escape(name)}"]`);
  return e ? (e.value === '' ? 0 : Number(e.value)) : 0;
};
function setNum(name, u) {
  const e = document.querySelector(`[name="${CSS.escape(name)}"]`);
  if (!e || String(e.value) === String(u)) return;
  e.value = u;
  e.dispatchEvent(new Event('input', { bubbles: true }));
}

const BARS = {};
let presetLiveUs = null;

function barVals(fn) {
  if (fn === 'record')  return [snapUs(numAt('record.switch.threshold_us') || 1700)];
  if (fn === 'hilight') return [1700];
  const e = [];                                   // the editable edges: one fewer than zones
  for (let k = 0; k < presetN - 1; k++) e.push(snapUs(numAt('preset.bands.' + k + '.max_us') || (900 + (k + 1) * 1200 / presetN)));
  return e;
}
function setBarVal(fn, i, u) {
  u = snapUs(u);
  if (fn === 'record') { setNum('record.switch.threshold_us', u); return; }
  if (fn === 'preset') {
    const v = barVals('preset');                  // keep the edges ascending, >= 25 us apart
    const lo = i ? v[i - 1] + 25 : 925, hi = i < v.length - 1 ? v[i + 1] - 25 : 2075;
    u = Math.max(lo, Math.min(hi, u));
    setNum('preset.bands.' + i + '.max_us', snapUs(u));
  }
}
function tickHTML() {
  return [1000, 1500, 2000].map(u =>
    `<i class="tick" style="left:${usPct(u)}%"></i>` +
    `<i class="tlab" style="left:${usPct(u)}%">${u}</i>`).join('');
}
function buildBar(host) {
  const fn = host.dataset.fn;
  host.innerHTML = '<i class="track"></i>' +
    (fn === 'preset' ? Array.from({ length: presetN }, (_, k) => `<i class="seg z${k}"></i>`).join('')
                     : '<i class="fill"></i>') +
    tickHTML() + '<i class="cursor" hidden><i class="cval"></i></i>';
  const b = {
    host, fn,
    fill: host.querySelector('.fill'),
    segs: [...host.querySelectorAll('.seg')],
    cursor: host.querySelector('.cursor'),
    cval: host.querySelector('.cval'),
    handles: []
  };
  const nh = fn === 'record' ? 1 : fn === 'preset' ? presetN - 1 : 0;
  for (let i = 0; i < nh; i++) {
    const h = el('i', 'handle');
    h.tabIndex = 0;
    h.setAttribute('role', 'slider');
    h.setAttribute('aria-valuemin', US_MIN);
    h.setAttribute('aria-valuemax', US_MAX);
    host.appendChild(h);
    attachDrag(b, h, i);
    b.handles.push(h);
  }
  BARS[fn] = b;
  layoutBar(fn);
}
function layoutBar(fn) {
  const b = BARS[fn]; if (!b) return;
  const v = barVals(fn);
  if (fn === 'preset') {
    const e = [0, ...v.map(usPct), 100];           // zone k spans e[k]..e[k+1]
    b.segs.forEach((sg, k) => {
      sg.style.cssText = k === b.segs.length - 1 ? `left:${e[k]}%;right:0` : `left:${e[k]}%;width:${e[k + 1] - e[k]}%`;
    });
    b.handles.forEach((h, k) => {
      h.style.left = e[k + 1] + '%';
      h.setAttribute('aria-valuenow', v[k]);
    });
  } else {
    const p = usPct(fn === 'hilight' ? 1700 : v[0]);
    b.fill.style.cssText = `left:${p}%;right:0`;
    if (b.handles[0]) {
      b.handles[0].style.left = p + '%';
      b.handles[0].setAttribute('aria-valuenow', v[0]);
    }
  }
}
function attachDrag(b, h, i) {
  const usFromX = cx => {
    const r = b.host.querySelector('.track').getBoundingClientRect();
    return US_MIN + (cx - r.left) / r.width * US_SPAN;
  };
  h.addEventListener('pointerdown', e => {
    if (h.getAttribute('aria-disabled') === 'true') return;
    h.setPointerCapture(e.pointerId);
    h.classList.add('on');
    e.preventDefault();
  });
  h.addEventListener('pointermove', e => {
    if (!h.hasPointerCapture(e.pointerId)) return;
    setBarVal(b.fn, i, usFromX(e.clientX));
    layoutBar(b.fn);
    if (b.fn === 'preset') paintZones();
  });
  const up = e => { h.classList.remove('on'); try { h.releasePointerCapture(e.pointerId); } catch (_) {} };
  h.addEventListener('pointerup', up);
  h.addEventListener('pointercancel', up);
  h.addEventListener('keydown', e => {
    const d = /Left|Down/.test(e.key) ? -25 : /Right|Up/.test(e.key) ? 25 : 0;
    if (!d) return;
    e.preventDefault();
    setBarVal(b.fn, i, barVals(b.fn)[i] + d);
    layoutBar(b.fn);
    if (b.fn === 'preset') paintZones();
  });
}
function setBarEnabled(fn, on) {
  const b = BARS[fn]; if (!b) return;
  b.host.classList.toggle('off', !on);
  b.handles.forEach(h => {
    h.setAttribute('aria-disabled', on ? 'false' : 'true');
    h.tabIndex = on ? 0 : -1;
  });
}
function paintZones(liveUs) {
  const b = BARS.preset; if (!b) return;
  if (liveUs !== undefined) presetLiveUs = liveUs;
  const v = barVals('preset');
  const z = presetLiveUs == null ? -1
          : (k => k < 0 ? presetN - 1 : k)(v.findIndex(x => presetLiveUs <= x));
  b.segs.forEach((s, k) => s.classList.toggle('live', k === z));
  $$('#zone-ids .pz').forEach((r, k) => r.classList.toggle('live', k === z));
}
function layoutAllBars() { Object.keys(BARS).forEach(layoutBar); paintZones(); }

function updateCursors(m) {
  const aux = (m && m.connected && Array.isArray(m.aux)) ? m.aux : null;
  [['record', 'record.switch.channel'],
   ['preset', 'preset.channel'],
   ['hilight', 'hilight.channel']].forEach(pair => {
    const b = BARS[pair[0]]; if (!b) return;
    const ch = numAt(pair[1]);
    const u = (aux && ch && !b.host.classList.contains('off') && aux.length > 3 + ch)
              ? aux[3 + ch] : null;
    if (u == null) {
      if (!b.cursor.hidden) b.cursor.hidden = true;
      if (pair[0] === 'preset') paintZones(null);
      return;
    }
    const left = usPct(u) + '%';
    if (b.cursor.hidden) b.cursor.hidden = false;
    if (b.cursor.style.left !== left) { b.cursor.style.left = left; b.cval.textContent = u + ' µs'; }
    if (pair[0] === 'preset') paintZones(u);
  });
}
$$('.rangebar').forEach(buildBar);

// ---- add / remove zones: rebuilds rows, bar and tabs, keeping values ----
function setZoneCount(n) {
  const snap = [];
  for (let i = 0; i < presetN; i++) snap.push({ max: numAt('preset.bands.' + i + '.max_us'), id: numAt('preset.bands.' + i + '.id') });
  const old = presetN;
  presetN = n;
  buildZoneRows();
  snap.forEach((z, i) => {
    if (i >= n) return;
    if (i < n - 1) setField('preset.bands.' + i + '.max_us', z.max);
    setField('preset.bands.' + i + '.id', z.id);
  });
  if (n > old) {                                              // the old top zone now needs an edge
    const last = old >= 2 ? snap[old - 2].max : 900;
    const mid = snapUs(last + (2100 - last) / 2);
    if (mid - last >= 150 && 2100 - mid >= 150 && n === old + 1) {
      setField('preset.bands.' + (old - 1) + '.max_us', mid);
    } else {                                                  // the top would get cramped: spread all edges evenly
      for (let k = 0; k < n - 1; k++) setField('preset.bands.' + k + '.max_us', snapUs(900 + (k + 1) * 1200 / n));
    }
    setField('preset.bands.' + (n - 1) + '.id', n - 1);
  }
  while (gpz.length < n) gpz.push({});
  gpz.length = n;
  if (gpSel >= n) gpSel = n - 1;
  buildBar($('#rb-preset'));
  applyGates();
  gpRender(); camEdited();
}
$('#zone-add').onclick = () => { if (presetN < ZONES_MAX) { setZoneCount(presetN + 1); gpSelectZone(presetN - 1, true); } };
$('#zone-del').onclick = () => {
  if (presetN <= ZONES_MIN) return;
  if (Object.keys(gpz[presetN - 1] || {}).length && !confirm('Remove zone ' + presetN + ' and its camera settings?')) return;
  setZoneCount(presetN - 1);
};
['input', 'change'].forEach(ev => $('#cam-control').addEventListener(ev, e => {
  const n = (e.target && e.target.name) || '';
  if (n === 'record.switch.threshold_us') layoutBar('record');
  else if (/^preset\.bands\.\d+\.max_us$/.test(n)) { layoutBar('preset'); paintZones(); }
  if (/\.channel$/.test(n) && lastStatus) updateCursors(lastStatus.msp);
}));

// ---- "Set AUX": the channel that moves most past a threshold wins ----
const AUX_MOVE_US = 200;      // minimum swing to count as a deliberate move
const AUX_TIMEOUT_MS = 15000;
const auxDetect = {};          // select name -> {btn, sel, baseline, startedAt}

function auxDetectStop(name, note) {
  const d = auxDetect[name];
  if (!d) return;
  d.btn.textContent = 'Set AUX';
  d.btn.classList.remove('active');
  delete auxDetect[name];
  if (note) msg(note);
}
function auxDetectStart(btn, sel) {
  // gate() can't reach other buttons: check the select itself.
  if (sel.disabled) { msg('enable this section first'); return; }
  const name = sel.name;
  if (auxDetect[name]) { auxDetectStop(name, 'AUX detect cancelled'); return; }
  const m = lastStatus && lastStatus.msp;
  if (!m || !m.connected || !Array.isArray(m.aux) || m.aux.length < 5) {
    msg('no live AUX data — connect the FC first');
    return;
  }
  auxDetect[name] = { btn, sel, baseline: m.aux.slice(), startedAt: Date.now() };
  // Polls are 1.5 s apart and can miss a tap: ask for a hold.
  btn.textContent = 'hold switch/button ~1s';
  btn.classList.add('active');
}
function auxDetectCheck(m) {
  const names = Object.keys(auxDetect);
  if (!names.length) return;
  const aux = (m && m.connected && Array.isArray(m.aux)) ? m.aux : null;
  names.forEach(name => {
    const d = auxDetect[name];
    if (Date.now() - d.startedAt > AUX_TIMEOUT_MS) {
      auxDetectStop(name, 'no movement seen — try again');
      return;
    }
    if (!aux) return;
    let bestIdx = -1, bestDelta = AUX_MOVE_US;
    for (let i = 4; i < aux.length && i < d.baseline.length; i++) {
      const delta = Math.abs(aux[i] - d.baseline[i]);
      if (delta >= bestDelta) { bestDelta = delta; bestIdx = i; }
    }
    if (bestIdx < 0) return;
    const ch = bestIdx - 3;   // aux[3+ch] <-> AUX channel `ch` (spec 8.2 mapping)
    d.sel.value = ch;
    d.sel.dispatchEvent(new Event('input', { bubbles: true }));
    d.sel.dispatchEvent(new Event('change', { bubbles: true }));
    auxDetectStop(name, 'AUX ' + ch + ' set');
  });
}
$$('.auxdetect').forEach(btn => {
  const sel = document.querySelector(`[name="${CSS.escape(btn.dataset.auxFor)}"]`);
  if (sel) { btn.onclick = () => auxDetectStart(btn, sel); }
});

// ==== OSD tab: drag-and-drop line editor + live preview ====
const TOKLIST = [
  ['st','REC state'],['et','elapsed'],['rt','time left'],['bt','batt %'],['bi','batt icon'],['cm','cam model'],
  ['rs','resolution'],['fp','fps'],['ar','aspect'],['ln','lens'],['pr','profile'],['bd','bit depth'],
  ['sd','SD left'],['sg','SD GB left'],['md','mode'],['pn','preset'],['cr','cam RSSI'],
  ['wr','warning'],['rp','Remote ID warning'],
  ['ix','ISO max'],['in','ISO min'],['sh','shutter'],['wb','white bal'],['co','colour'],['sa','sharpness'],['ev','EV comp'],['pt','Protune']
];
const TOKS = {}; TOKLIST.forEach(t => TOKS[t[0]] = t[1]);
const PGRP = [['Recording', 'st et rt wr'], ['Camera', 'bt bi sd sg cm md pn cr'], ['Video', 'rs fp ar ln pr bd'],
  ['Protune (GoPro)', 'ix in sh wb co sa ev pt'], ['Remote ID', 'rp']];
// Tokens only one brand can fill (GoPro settings); none is DJI-only.
const GOPRO_ONLY = new Set(['pn', 'ar', 'ln', 'pr', 'bd', 'ix', 'in', 'sh', 'wb', 'co', 'sa', 'ev', 'pt']);
const DJI_ONLY = new Set();
const tokOk = t => cfgBrand === 'dji' ? !GOPRO_ONLY.has(t) : cfgBrand === 'gopro' ? !DJI_ONLY.has(t) : true;
const tokWhy = () => cfgBrand === 'dji' ? 'GoPro only' : 'DJI only';

let model = [[], [], [], []];
let focusLine = 0;
let lastStatus = null;

// A line is a list of items: {t: token} or {lit: text}; every space is an item of its own.
function parseTmpl(str) {
  str = str || ''; const out = []; let lit = '';
  const flush = () => { lit.split(/( )/).forEach(x => x && out.push({lit: x})); lit = ''; };
  for (let p = 0; p < str.length; p++) {
    if (str[p] === '%') {
      const pr = str.substr(p + 1, 2);
      if (pr.length === 2 && TOKS[pr]) { flush(); out.push({t: pr}); p += 2; continue; }
    }
    lit += str[p];
  }
  flush();
  return out;
}
const serialTmpl = toks => toks.map(x => x.t ? '%' + x.t : x.lit).join('');

function commit() {
  for (let i = 0; i < 4; i++) $(`[name="osd.lines.${i}"]`).value = serialTmpl(model[i]);
  camEdited();
  renderPreview();
}
function renderChips(i) {
  const w = $('#chips-' + i); w.textContent = '';
  if (!model[i].length) { const e = el('span', 'empty'); e.textContent = 'Drop tokens here'; w.appendChild(e); }
  model[i].forEach((x, j) => {
    const bad = x.t && !tokOk(x.t), c = el('span', 'chip' + (x.t ? '' : x.lit === ' ' ? ' sp' : ' lit') + (bad ? ' bad' : ''));
    c.tabIndex = 0;
    c.title = bad ? tokWhy() + ': shows nothing with this camera. Remove it.' : x.t ? '%' + x.t : '';
    if (x.t) { const b = el('b'); b.textContent = TOKS[x.t]; c.appendChild(b); }
    else if (x.lit === ' ') { c.textContent = '␣'; c.title = 'space'; }
    else {
      const inp = el('input'); inp.value = x.lit; inp.size = Math.max(2, x.lit.length); inp.placeholder = 'text';
      inp.oninput = () => { x.lit = inp.value; inp.size = Math.max(2, inp.value.length); commit(); };
      c.appendChild(inp);
    }
    const b = el('button', 'x'); b.type = 'button'; b.textContent = '✕'; b.title = 'Remove';
    b.onclick = ev => { ev.stopPropagation(); model[i].splice(j, 1); renderChips(i); commit(); };
    c.appendChild(b);
    c.onkeydown = ev => {                                 // keyboard: arrows move, Delete removes
      if (ev.target !== c) return;
      const k = ev.key, a = model[i], n = j + (k === 'ArrowLeft' ? -1 : k === 'ArrowRight' ? 1 : 0);
      if (k === 'Delete' || k === 'Backspace') a.splice(j, 1);
      else if (n !== j && n >= 0 && n < a.length) { a[j] = a[n]; a[n] = x; }
      else return;
      ev.preventDefault(); renderChips(i); commit();
      const cs = $$('#chips-' + i + ' .chip'); (cs[Math.min(n === j ? j : n, cs.length - 1)] || {focus() {}}).focus();
    };
    w.appendChild(c);
  });
}
function setFocusLine(i) {
  focusLine = i;
  $$('.oline').forEach(o => o.classList.toggle('foc', +o.dataset.i === i));
  renderPreview();
}
const palItem = d => d.t ? {t: d.t} : {lit: d.k === 'sp' ? ' ' : ''};
let dragDone = 0;
function buildPalette() {
  const pal = $('#palette'), sd = sampleData(), EX = {bi: '▮', wr: 'LOW BATT', rp: '!RID1', st: 'REC'};
  const add = (box, txt, ex, set) => {
    const b = el('button', 'pchip'); b.type = 'button';
    const s = el('span'); s.textContent = txt; b.appendChild(s);
    if (ex) { const it = el('i'); it.textContent = ex; b.appendChild(it); }
    Object.assign(b.dataset, set); if (set.t) b.title = '%' + set.t;
    b.onclick = () => {                                   // tap: add to the selected line
      if (Date.now() - dragDone < 300 || b.disabled) return;
      const x = palItem(b.dataset); model[focusLine].push(x); renderChips(focusLine); commit();
      if (x.lit === '') $$('#chips-' + focusLine + ' .lit input').pop().focus();
    };
    box.appendChild(b);
  };
  PGRP.concat([['Text', '']]).forEach(([g, ts]) => {
    const d = el('div', 'pgrp'), h = el('b'); h.textContent = g; d.appendChild(h);
    ts.split(' ').filter(Boolean).forEach(t => add(d, TOKS[t], EX[t] || tokVal(t, sd), {t}));
    if (!ts) { add(d, 'space', '␣', {k: 'sp'}); add(d, 'text', 'abc', {k: 'tx'}); }
    pal.appendChild(d);
  });
}
$$('.oline').forEach(o => o.addEventListener('click', () => setFocusLine(+o.dataset.i)));
$('#osdsim').onclick = renderPreview;
$('#osdlines').onclick = e => { const l = e.target.closest('.osdline'); if (l && l.dataset.i) setFocusLine(+l.dataset.i); };

// Drag: palette items and line chips, with mouse or touch (hold ~0.3 s on touch, so the page still scrolls).
// While dragging, the preview shows the line as it would be after the drop; dropping a chip off the lines removes it.
let pd = null, drag = null, osdD;
const caret = el('div', 'dcaret');
function dropAt(x, y) {
  const t = document.elementFromPoint(x, y), o = t && t.closest('.oline'), pl = t && t.closest('.osdline[data-i]');
  if (pl) {                                              // on the preview: at the character it was dropped on
    const i = +pl.dataset.i, r = pl.getBoundingClientRect(), cw = r.width / 16, col = (x - r.left) / cw;
    const a = model[i].filter((_, k) => !drag.src || drag.src[0] !== i || drag.src[1] !== k);
    let at = a.length, c = 0;
    for (let k = 0; k < a.length; k++) {
      const w = expandTmpl(serialTmpl([a[k]]), osdD).length;
      if (col < c + w / 2) { at = k; break; }
      c += w;
    }
    Object.assign(caret.style, {left: r.left + Math.min(c, 16) * cw - 1 + 'px', top: r.top + 'px', height: r.height + 'px'});
    return [i, at];
  }
  if (!o) return null;
  const i = +o.dataset.i, cs = $$('#chips-' + i + ' .chip:not(.drg)');
  let at = cs.length, r;
  for (let k = 0; k < cs.length; k++) {
    r = cs[k].getBoundingClientRect();
    if (y < r.top || (y <= r.bottom && x < r.left + r.width / 2)) { at = k; break; }
  }
  const box = $('#chips-' + i).getBoundingClientRect(), c = cs[at] || cs[at - 1];
  r = c ? c.getBoundingClientRect() : box;
  Object.assign(caret.style, {left: (c ? (cs[at] ? r.left - 4 : r.right + 2) : box.left + 4) + 'px', top: r.top + 'px', height: (c ? r.height : 36) + 'px'});
  return [i, at];
}
function dragMove(e) {
  const g = drag.ghost, x = e.clientX, y = e.clientY;
  g.style.transform = `translate(${x - drag.dx}px,${y - drag.dy}px)`;
  if (y < $('#tabs').offsetHeight + 24) scrollBy(0, -10); else if (y > innerHeight - 70) scrollBy(0, 10);
  const at = dropAt(x, y), tmp = model.map(a => a.slice());
  if (drag.src) tmp[drag.src[0]].splice(drag.src[1], 1);
  if (at) tmp[at[0]].splice(at[1], 0, drag.item);
  drag.tmp = tmp; drag.at = at;
  caret.hidden = !at;
  g.classList.toggle('del', !at && !!drag.src);
  $$('.oline').forEach(o => o.classList.toggle('hov', !!at && +o.dataset.i === at[0]));
  renderPreview();
}
function dragBegin() {
  const c = pd.c, r = c.getBoundingClientRect(), g = c.cloneNode(true);
  clearTimeout(pd.timer);
  drag = pd; pd = null;
  drag.dx = drag.x - r.left; drag.dy = drag.y - r.top;
  g.className += ' dghost'; g.style.width = r.width + 'px';
  document.body.append(g, caret); drag.ghost = g;
  if (drag.src) c.classList.add('drg');
  document.body.classList.add('dragging');
  if (navigator.vibrate && drag.touch) navigator.vibrate(10);
  dragMove({clientX: drag.x, clientY: drag.y});
}
function dragEnd(ok) {
  const d = drag; drag = null;
  d.ghost.remove(); caret.remove();
  document.body.classList.remove('dragging');
  $$('.oline.hov').forEach(o => o.classList.remove('hov'));
  dragDone = Date.now();
  const ch = ok && (d.at || d.src) && d.tmp.map(serialTmpl).join('\n') !== model.map(serialTmpl).join('\n');
  if (ch) model = d.tmp;
  if (ok && d.at) focusLine = d.at[0];
  for (let i = 0; i < 4; i++) renderChips(i);
  if (ch) commit(); setFocusLine(focusLine);
}
document.addEventListener('pointerdown', e => {
  const c = e.target.closest('#osd .pchip:not(:disabled),#osd .chip');
  if (!c || e.button > 0 || drag || e.target.closest('input,.x')) return;
  const w = c.parentNode, src = c.classList.contains('chip') ? [+w.id.slice(6), [...w.querySelectorAll('.chip')].indexOf(c)] : null;
  pd = {c, src, item: src ? model[src[0]][src[1]] : palItem(c.dataset), x: e.clientX, y: e.clientY, touch: e.pointerType === 'touch'};
  if (pd.touch) pd.timer = setTimeout(dragBegin, 300);
});
document.addEventListener('pointermove', e => {
  if (drag) return dragMove(e);
  if (!pd) return;
  if (Math.hypot(e.clientX - pd.x, e.clientY - pd.y) > (pd.touch ? 8 : 5)) {
    if (pd.touch) { clearTimeout(pd.timer); pd = null; } else dragBegin();   // touch moved early: it is a scroll
  }
});
document.addEventListener('pointerup', () => { if (drag) dragEnd(1); else if (pd) { clearTimeout(pd.timer); pd = null; } });
document.addEventListener('pointercancel', () => { if (drag) dragEnd(0); else if (pd) { clearTimeout(pd.timer); pd = null; } });
document.addEventListener('touchmove', e => { if (drag) e.preventDefault(); }, {passive: false});
document.addEventListener('contextmenu', e => { if (pd || drag) e.preventDefault(); });
document.addEventListener('keydown', e => { if (drag && e.key === 'Escape') dragEnd(0); });

function osdLoad() {
  for (let i = 0; i < 4; i++) {
    model[i] = parseTmpl($(`[name="osd.lines.${i}"]`).value || ''); renderChips(i);
  }
  setFocusLine(0);
}

// ---- token expander (mirrors osd_format.c) ----
const mmss = s => Math.floor(s / 60) + ':' + String(Math.floor(s % 60)).padStart(2, '0');
const remainStr = s => s >= 3600 ? Math.floor(s / 3600) + 'h' : Math.floor(s / 60) + 'm';

function activeWarning(d) {
  if (!d.link) return 'CAM LOST';
  if (d.sd_missing) return 'NO SD';
  if (d.sd_full) return 'SD FULL';
  if (d.rec_fail) return 'REC FAIL';
  if (d.battery.valid && d.battery.v <= d.warn_batt_pct) return 'LOW BATT';
  if (d.rec_remaining.valid && d.rec_remaining.v <= d.warn_sd_low_min * 60) return 'SD LOW';
  if (d.hot) return 'HOT';
  if (d.clock_fail) return 'CLK FAIL';
  return '';
}
const pv = x => x && x.valid ? x.v : '';
// mirrors osd_format.c osd_cam_model_short()
const camShort = m => { m = m || ''; let x;
  if ((x = /^HERO(\d{1,3})/.exec(m))) return 'GP' + x[1] + (/Mini/.test(m) ? 'M' : '');
  if ((x = /^Osmo Action (\d{1,2})/.exec(m))) return 'A' + x[1] + (/Pro/.test(m) ? 'P' : '');
  if (/^Osmo 360/.test(m)) return 'O360';
  return /^Osmo/.test(m) ? 'DJI' : ''; };
// Protune tokens: cam_state key + sample value
const PROT = {ix: ['iso_max', '1600'], in: ['iso_min', '100'], sh: ['shutter', '1/60'], wb: ['wb', '5500K'],
  co: ['color', 'Flat'], sa: ['sharpness', 'Medium'], ev: ['ev', '0.0'], pt: ['protune', 'ON'], pn: ['preset_name', 'Standard']};
function tokVal(pair, d) {
  switch (pair) {
    case 'st': if (!d.link) return ''; if (d.pre_record) return 'PRE';
      if (d.recording === 'true') return 'REC'; if (d.recording === 'false') return 'IDLE'; return '';
    case 'et': return d.rec_elapsed.valid ? mmss(d.rec_elapsed.v) : '';
    case 'rt': return d.rec_remaining.valid ? remainStr(d.rec_remaining.v) : '';
    case 'bt': return d.battery.valid ? d.battery.v + '%' : '';
    case 'bi':
      // BF battery glyph 0x90..0x96 (osd_format.c), drawn from U+E090..E096
      if (!d.battery.valid) return '';
      return String.fromCharCode(0xE090 + Math.max(0, Math.min(6, Math.floor((100 - d.battery.v) * 7 / 100))));
    case 'rs': return d.resolution.valid ? d.resolution.v : '';
    case 'fp': return d.fps.valid ? d.fps.v : '';
    case 'ln': return d.lens.valid ? d.lens.v : '';
    case 'sd': if (d.sd_missing) return 'NO SD'; if (d.sd_full) return 'FULL';
      if (d.rec_remaining.valid) return remainStr(d.rec_remaining.v);
      if (d.sd_remaining_kb.valid) return Math.floor(d.sd_remaining_kb.v / (1024 * 1024)) + 'G'; return '';
    case 'sg': return d.sd_remaining_kb.valid
      ? (d.sd_remaining_kb.v / 1024 / 1024).toFixed(1) + 'G' : '';
    case 'md': return d.mode.valid ? d.mode.v : '';
    case 'wr': { const w = activeWarning(d); return (w && !(d.warn_blink && d.blinkOn)) ? w : ''; }
    case 'rp': return '';                    // the alert text replaces the whole line (simulated below)
    case 'ar': return d.aspect.valid ? d.aspect.v : '';
    case 'pr': return d.profile.valid ? d.profile.v : '';
    case 'bd': return d.bit_depth.valid ? d.bit_depth.v + 'b' : '';
    case 'cr': return d.link ? String(d.rssi) : '';
    case 'cm': return camShort(d.model);
    default: return PROT[pair] ? pv(d[PROT[pair][0]]) : null;
  }
}
function expandTmpl(tmpl, d) {
  let out = '';
  for (let p = 0; p < tmpl.length; p++) {
    const c = tmpl[p];
    if (c !== '%') { out += c; continue; }
    p++; if (p >= tmpl.length) break;
    const a = tmpl[p], b = (p + 1 < tmpl.length) ? tmpl[p + 1] : '';
    const r = tokVal(a + b, d);
    if (r === null) { out += '%' + a; } else { out += r; p++; }
  }
  return out;
}
function withForm(d) {
  d.warn_batt_pct = +$('[name="osd.warn.batt_pct"]').value || 0;
  d.warn_sd_low_min = +$('[name="osd.warn.sd_low_min"]').value || 0;
  d.warn_blink = $('[name="osd.warn.blink"]').checked;
  d.blinkOn = false;
  d.units = +$('[name="osd.units"]').value || 0;
  d.digital = +$('[name="osd.type"]').value === 1;
  return d;
}
// mirror osd_format.c digital filter: fold lowercase, keep only 0x20..0x5F
function digitalize(s) {
  let o = '';
  for (const ch of s) {
    if (ch >= '\uE090' && ch <= '\uE096') { o += ch; continue; }   // the %bi battery glyph is exempt from the filter
    let c = ch.charCodeAt(0);
    if (c >= 97 && c <= 122) c -= 32;
    o += (c < 0x20 || c > 0x5F) ? ' ' : String.fromCharCode(c);
  }
  return o;
}
function sampleData() {
  return withForm({
    link: true, searching: false, pre_record: false, recording: 'true',
    rec_elapsed: {v: 83, valid: true}, rec_remaining: {v: 2700, valid: true},
    battery: {v: 78, valid: true},
    resolution: {v: '5.3K', valid: true}, fps: {v: '30', valid: true}, aspect: {v: '16:9', valid: true},
    lens: {v: 'Wide', valid: true}, profile: {v: 'HDR', valid: true}, bit_depth: {v: 10, valid: true},
    mode: {v: 'Video', valid: true}, model: 'HERO11 Black',
    ...Object.fromEntries(Object.values(PROT).map(([k, v]) => [k, {v, valid: true}])),
    sd_missing: false, sd_full: false, sd_remaining_kb: {v: 60 * 1024 * 1024, valid: true},
    rssi: -52, gps_present: true, gps_fix: 3, gps_sats: 11, gps_spd: 850,
    fc_batt_state: 0, craft_name: 'QUAD', hot: false, rec_fail: false, clock_fail: false
  });
}
function liveData(s) {
  const cs = s.cam_state, m = s.msp || {};
  const g = m.gps || {};
  return withForm({
    link: true, searching: false, pre_record: false, recording: cs.recording,
    rec_elapsed: {v: cs.elapsed_s || 0, valid: cs.elapsed_s != null},
    rec_remaining: cs.rec_remaining_s, battery: cs.battery_pct,
    resolution: cs.resolution, fps: cs.fps, aspect: cs.aspect,
    lens: {v: 'Wide', valid: true},        // not in /api/status -- sample fallback
    profile: cs.profile, bit_depth: cs.bit_depth, mode: cs.mode,
    ...cs,
    sd_missing: cs.sd_missing, sd_full: cs.sd_full, sd_remaining_kb: cs.sd_remaining_kb,
    rssi: s.camera.rssi,
    gps_present: !!g.present, gps_fix: g.fix || 0, gps_sats: g.sats || 0,
    gps_spd: 0,                            // not in /api/status -- sample fallback
    fc_batt_state: 0,                      // not in /api/status -- sample fallback
    craft_name: '',                        // not in /api/status -- sample fallback
    hot: cs.hot, rec_fail: false, clock_fail: false
  });
}
function renderPreview() {
  const scr = $('#osdscreen'); if (!scr) return;
  const box = $('#osdlines');

  const connected = !!(lastStatus && lastStatus.camera && lastStatus.camera.link === 'READY');
  const sim = $('#osdsim');
  sim.disabled = connected;
  sim.parentNode.classList.toggle('muted', connected);

  let d;
  if (connected) {
    d = liveData(lastStatus);
  } else {
    d = sampleData();
    if (!sim.checked) { d.link = false; d.searching = false; }
  }

  // the lines as the camera fills them (while dragging: as they would be after the drop); counted even when the link is down
  const dc = osdD = d.link ? d : sampleData(), tm = i => drag ? serialTmpl(drag.tmp[i]) : $(`[name="osd.lines.${i}"]`).value || '';
  const full = [0, 1, 2, 3].map(i => { const t = expandTmpl(tm(i), dc); return dc.digital ? digitalize(t) : t; });   // mirror the firmware digital filter
  full.forEach((t, i) => { const c = $$('.olh .cnt')[i]; c.textContent = t.length + '/16'; c.classList.toggle('over', t.length > 16);
    c.title = t.length > 16 ? 'Too long: the OSD cuts it after 16 characters' : ''; });
  let lines = d.link ? full.map(t => t.slice(0, 16)) : [d.searching ? 'CAM ?' : 'CAM LOST', '', '', ''];

  // simulate the Remote ID messages on the line that holds %rp (script in ridDemo)
  const ri = sim.checked ? ridRpLine() : -1, lab = [];
  ridNol();
  if (ri >= 0) {
    if (REDUCED.matches) [1, 2, 3].forEach(k => { lines.splice(ri + k, 0, ridDemo(k << 3)); lab[ri + k] = RTAG[k]; });
    else { const d = ridDemo(ridPh); if (d) { lines[ri] = d; lab[ri] = RTAG[ridPh >> 3]; } }
  }
  while (box.children.length < lines.length) box.appendChild(el('div', 'osdline'));
  while (box.children.length > lines.length) box.lastChild.remove();
  lines.forEach((t, i) => {
    const c = box.children[i];
    if (c.dataset.raw !== t) {
      c.dataset.raw = t; c.textContent = '';
      t.split(/([\uE090-\uE096])/).forEach(x => {          // the battery glyph is drawn as a battery (--f = charge)
        if (/^[\uE090-\uE096]$/.test(x)) { const b = el('i', 'bat'); b.style.setProperty('--f', (6 - (x.charCodeAt(0) - 0xE090)) / 6 * 100 + '%'); c.appendChild(b); }
        else if (x) c.appendChild(document.createTextNode(x));
      });
    }
    if (lab[i]) c.dataset.l = lab[i]; else delete c.dataset.l;
    const li = lines.length === 4 && d.link ? i : -1;    // which editor line this is (not while Remote ID lines are inserted)
    if (li >= 0) c.dataset.i = li; else delete c.dataset.i;
    c.classList.toggle('foc', li >= 0 && li === (drag && drag.at ? drag.at[0] : focusLine));
  });
}
['input', 'change'].forEach(ev => document.addEventListener(ev, e => {
  if (e.target.closest && e.target.closest('#osd')) renderPreview();
}));
buildPalette();
const navH = () => document.body.style.setProperty('--navh', $('#tabs').offsetHeight + 'px');   // the preview sticks below the tab bar
addEventListener('resize', navH); navH();

// ============ GoPro zone settings builder ============
// preset.bands[N].gp = {settingId: value} (gp_preset.h)
const GP_SET = {
  2: ["Resolution", [[1,"4K"],[4,"2.7K"],[6,"2.7K 4:3"],[7,"1440"],[9,"1080"],[12,"720"],[18,"4K 4:3"],[21,"5.6K"],[24,"5K"],[25,"5K 4:3"],[26,"5.3K 8:7"],[27,"5.3K 4:3"],[28,"4K 8:7"],[31,"8K"],[35,"5.3K 21:9"],[36,"4K 21:9"],[37,"4K 1:1"],[38,"900"],[39,"4K SPH"],[40,"8K 4:3"],[44,"1440 4:3"],[100,"5.3K"],[107,"5.3K 8:7 V2"],[108,"4K 8:7 V2"],[109,"4K 9:16 V2"],[110,"1080 9:16 V2"],[111,"2.7K 4:3 V2"],[112,"4K 4:3 V2"],[113,"5.3K 4:3 V2"]]],
  108: ["Aspect ratio", [[0,"4:3"],[1,"16:9"],[3,"8:7"],[4,"9:16"],[5,"21:9"],[6,"1:1"]], ["HERO12 Black", "HERO13 Black", "MAX 2", "Mission 1 Pro", "Mission 1", "LIT HERO"]],
  3: ["Frame rate (fps)", [[0,"240"],[1,"120"],[2,"100"],[3,"90"],[5,"60"],[6,"50"],[8,"30"],[9,"25"],[10,"24"],[13,"200"],[15,"400"],[16,"360"],[17,"300"],[18,"480"],[19,"960"],[20,"800"]]],
  121: ["Lens", [[0,"Wide"],[2,"Narrow"],[3,"Superview"],[4,"Linear"],[7,"Max SuperView"],[8,"Linear + Horizon Leveling"],[9,"HyperView"],[10,"Linear + Horizon Lock"],[11,"Max HyperView"],[12,"Ultra SuperView"],[13,"Ultra Wide"],[14,"Ultra Linear"],[104,"Ultra HyperView"]]],
  135: ["HyperSmooth", [[0,"Off"],[1,"Low"],[2,"High"],[3,"Boost"],[4,"Auto Boost"],[100,"Standard"]], ["HERO9 Black", "HERO10 Black", "HERO11 Black", "HERO11 Black Mini", "HERO12 Black", "HERO13 Black", "MAX 2", "Mission 1 Pro", "Mission 1"]],
  183: ["Bit depth", [[0,"8-Bit"],[2,"10-Bit"]], ["HERO12 Black", "HERO13 Black", "MAX 2", "Mission 1 Pro", "Mission 1"]],
  184: ["Color profile", [[0,"Standard"],[1,"HDR"],[2,"Log"],[101,"HLG HDR"]], ["HERO12 Black", "HERO13 Black", "MAX 2"]],
  114: ["Protune", [[0,"OFF"],[1,"ON"]]],
  13: ["ISO max", [[0,"6400"],[1,"1600"],[2,"400"],[3,"3200"],[4,"800"],[7,"200"],[8,"100"],[9,"Auto"]]],
  102: ["ISO min", [[0,"6400"],[1,"1600"],[2,"400"],[3,"3200"],[4,"800"],[7,"200"],[8,"100"],[9,"Auto"]]],
  145: ["Shutter angle", [[0,"Auto"],[3,"1/24"],[4,"1/25"],[5,"1/30"],[6,"1/48"],[7,"1/50"],[8,"1/60"],[11,"1/96"],[12,"1/100"],[13,"1/120"],[16,"1/192"],[17,"1/200"],[18,"1/240"],[21,"1/400"],[22,"1/480"],[23,"1/960"],[24,"1/1920"],[25,"1/384"],[28,"1/800"],[29,"1/1600"],[30,"1/3200"],[31,"1/3840"]]],
  115: ["White balance", [[0,"Auto"],[2,"5500K"],[3,"6500K"],[4,"Native"],[5,"4000K"],[7,"6000K"],[8,"2300K"],[9,"2800K"],[10,"3200K"],[11,"4500K"],[12,"5000K"]]],
  116: ["Color", [[1,"Flat"],[2,"Natural"],[100,"Vibrant"]]],
  117: ["Sharpness", [[0,"High"],[1,"Medium"],[2,"Low"]]],
  118: ["EV comp", [[0,"+2.0"],[1,"+1.5"],[2,"+1.0"],[3,"+0.5"],[4,"0.0"],[5,"-0.5"],[6,"-1.0"],[7,"-1.5"],[8,"-2.0"]]],
  174: ["10-bit", [[0,"Off"],[1,"On"]]],
};
// GP_SET[id] = [label, options, models that have it (none = all)]
const GP_GROUPS = [['Video', [184,183,108,2,3,121,135]],
                   ['Protune', [114,13,102,145,115,116,117,118,174]]];
let gpz = [{}, {}, {}], gpSel = 0, gpModel = '';
try { gpModel = localStorage.getItem('gpmodel') || ''; } catch (e) {}
const gpSupported = id => { const m = GP_SET[id][2]; return !m || !gpModel || m.includes(gpModel); };

// ---- shutter angle ----
// 145 is stored raw (1/60...), shown as an angle from the frame rate.
const GP_FPS = {0:240,1:120,2:100,3:90,5:60,6:50,8:30,9:25,10:24,13:200,15:400,16:360,17:300,18:480,19:960,20:800};
const GP_ANGLES = [360, 180, 90, 45, 22.5, 11.25, 5.625, 2.8125];
const gpDenom = v => { const o = GP_SET[145][1].find(x => x[0] === v), m = o && /^1\/(\d+)$/.exec(o[1]);
                       return m ? +m[1] : 0; };                                  // 0 = Auto / unknown
const gpAngleOf = (v, fps) => { const d = gpDenom(v); return d ? Math.round(360 * fps / d * 10) / 10 : 0; };
const gpShutterFor = (angle, fps) => { const d = Math.round(fps * 360 / angle);
  const o = GP_SET[145][1].find(x => gpDenom(x[0]) === d); return o ? o[0] : undefined; };
const fmtAngle = a => String(a) + '\u00b0';
// Frame rate for the angles: the zone's own, else the camera's.
function gpFpsNum(z) {
  if (z[3] !== undefined) return GP_FPS[z[3]] || 0;
  const gs = lastStatus && lastStatus.cam_state && lastStatus.cam_state.gp_set;
  return gs && gs[3] !== undefined ? GP_FPS[gs[3]] || 0 : 0;
}
// ---- capabilities (what the camera offers right now) ----
let gpCaps = null, gpCapsGen = -1, gpCamReady = false, gpSyncing = false;
const gpCapsOn = () => !!(gpCamReady && gpCaps && gpCaps[2]);
async function gpLoadCaps() {
  try {
    const r = await fetch('/api/camera/caps').then(x => x.json());
    gpCaps = r && r.caps && Object.keys(r.caps).length ? r.caps : null;
    gpPre = (r && r.presets) || []; zselRender();
  } catch (e) { return; }
  gpSyncing = false;
  gpRender();
}

// ---- option order: sorted for reading ----
const GP_ORD = {                       // explicit order by option value
  121: [0, 4, 8, 10, 2, 3, 7, 12, 9, 11, 104, 13, 14],   // Wide, Linear(+HL), Narrow, Superview..., HyperView..., Ultra
  135: [0, 1, 2, 3, 4, 100],
  184: [0, 1, 2, 101],
  108: [1, 0, 3, 5, 6, 4],
  116: [100, 2, 1],                    // Vibrant, Natural, Flat
  117: [2, 1, 0],                      // Low, Medium, High
};
const GP_RES_SIZE = ['8K', '5.6K', '5.3K', '5K', '4K', '2.7K', '1440', '1080', '900', '720'];   // large -> small
const GP_RES_ASP = ['', '21:9', '4:3', '8:7', '1:1', '9:16', 'SPH'];
function gpRank(id, o) {
  const v = o[0], n = o[1];
  if (GP_ORD[id]) { const i = GP_ORD[id].indexOf(v); return i < 0 ? 900 + v : i; }
  switch (+id) {
    case 2: {                          // 5.3K ... 720; plain 16:9 before 4:3 / 8:7 / ...; V2 after
      const m = /^(\S+)\s*(.*)$/.exec(n) || [0, n, ''], rest = m[2].replace(/\s*V2/, '');
      const si = GP_RES_SIZE.indexOf(m[1]), ai = GP_RES_ASP.indexOf(rest);
      return (si < 0 ? 99 : si) * 100 + (ai < 0 ? 9 : ai) * 10 + (/V2/.test(m[2]) ? 5 : 0) + v / 1000;
    }
    case 3: case 118: return parseFloat(n);                    // fps and EV: numeric
    case 13: case 102: return n === 'Auto' ? -1 : parseInt(n, 10);
    case 115: return n === 'Auto' ? -2 : n === 'Native' ? -1 : parseInt(n, 10);   // Kelvin ascending
    case 145: return gpDenom(v);                               // Auto (0) first, then 1/24 ... 1/3840
    default: return v;
  }
}
// Options to offer: the camera's list if known, else all; sorted.
function gpOpts(id) {
  const all = GP_SET[id][1];
  let o = all.slice();
  if (gpCapsOn()) { const c = gpCaps[id] || []; o = o.filter(x => c.includes(x[0])); }   // unnamed values are never offered
  o.sort((a, b) => gpRank(id, a) - gpRank(id, b) || a[0] - b[0]);
  if (+id !== 2) return o;
  const sh = o.map(x => gpResName(x[1], 1));                   // short unless two clash
  return o.map((x, i) => [x[0], sh.indexOf(sh[i]) !== sh.lastIndexOf(sh[i]) ? gpResName(x[1]) : sh[i]]);
}
// No "V2"; size only when the Aspect row sets the aspect
const gpResName = (n, short) => (n = n.replace(' V2', ''), short && gpCapsOn() && gpCaps[108] ? n.split(' ')[0] : n);
const gpKnown = (id, v) => GP_SET[id][1].some(x => x[0] === v);
const gpShown = (id, v) => v !== undefined ? gpKnown(id, v) : gpCapsOn() ? gpOpts(id).length > 0 : gpSupported(id);

function gpFillShutter(sel, z) {
  const fps = gpFpsNum(z), v = z[145], allowed = gpOpts(145);
  let opts;
  if (!fps) {
    opts = allowed.map(o => [o[0], o[1]]);                      // no frame rate known: plain fractions
  } else {
    opts = [];
    if (allowed.some(o => o[0] === 0)) opts.push([0, 'Auto']);
    GP_ANGLES.forEach(a => { const o = gpShutterFor(a, fps);
      if (o !== undefined && allowed.some(x => x[0] === o)) opts.push([o, fmtAngle(a) + ' (1/' + gpDenom(o) + ')']); });
  }
  if (v !== undefined && !opts.some(o => o[0] === v)) {        // stored value the list lacks
    const a = fps ? gpAngleOf(v, fps) : 0;
    opts.push([v, (a ? fmtAngle(a) + ' (1/' + gpDenom(v) + ')' : v === 0 ? 'Auto' : '1/' + gpDenom(v))
                  + (gpCapsOn() ? ' (not available now)' : '')]);
  }
  return opts;
}

function gpBuild() {
  $('#gp-rows').innerHTML = GP_GROUPS.map(([g, ids]) =>
    `<h4>${g}</h4>` + ids.map(id =>
      `<div class="gpr" data-id="${id}"><input type="checkbox" aria-label="Push ${GP_SET[id][0]}">` +
      `<span>${GP_SET[id][0]}</span><select></select></div>`).join('')).join('');
}
function gpGate() {
  const on = gpCamReady && !!getField('preset.enabled');
  gate($('#gp-builder'), on, 'connect the camera to edit zone settings');
  $$('#gp-builder button').forEach(b => { b.disabled = !on; });
}
function gpRender() {
  if (!$('#gp-rows').firstChild) gpBuild();
  const z = gpz[gpSel], capsOn = gpCapsOn();
  $$('#gp-rows .gpr').forEach(r => {
    const id = r.dataset.id, sel = r.querySelector('select');
    if (z[id] !== undefined && !gpKnown(id, z[id])) delete z[id];   // an unnamed value (Color in Log): never shown or pushed
    const v = z[id];
    let opts = id === '145' ? gpFillShutter(sel, z) : gpOpts(id).map(o => [o[0], o[1]]);
    if (id !== '145' && v !== undefined && !opts.some(o => o[0] === v)) {
      const base = GP_SET[id][1].find(x => x[0] === v);        // stored value the camera does not offer now
      opts.push([v, (base ? gpResName(base[1]) : 'option ' + v) + (capsOn ? ' (not available now)' : '')]);
    }
    const sig = JSON.stringify(opts);
    if (sel.dataset.sig !== sig) {                              // rebuild only on change: keeps an open list open
      sel.dataset.sig = sig;
      sel.innerHTML = opts.map(o => `<option value="${o[0]}">${o[1]}</option>`).join('');
    }
    if (v !== undefined) sel.value = v;
    r.classList.toggle('on', v !== undefined);
    r.classList.toggle('bad', v !== undefined && capsOn && !(gpCaps[id] || []).includes(v));
    r.querySelector('input').checked = v !== undefined;
    r.hidden = !gpShown(id, v);
    r.title = v !== undefined && !capsOn && !gpSupported(id) ? gpModel + ' does not list this setting' : '';
    if (id === '145' && z[3] === undefined) {
      r.title = (r.title ? r.title + '. ' : '') + 'Angles use the camera’s current frame rate; tick Frame rate in this zone to fix it.';
    }
  });
  gpGate();
  $('#gp-sync').classList.toggle('on', gpSyncing);             // a spinner: swapping the text reflowed the view
  $('#gp-model').textContent = !gpCamReady ? 'Connect the camera to edit zone settings.'
    : capsOn ? 'Showing what ' + (gpModel || 'the camera') + ' offers right now' +
        ' — edits are also sent to the camera so dependent lists stay correct.'
    : gpModel ? 'Showing the settings ' + gpModel + ' supports. Connect the camera (in a video preset) to filter the options too.'
    : 'Connect the camera to hide settings and options it does not support.';
  const zt = $('#gp-zt');
  while (zt.children.length < presetN) {
    const i = zt.children.length, b = el('button', 'ghost');
    b.type = 'button'; b.onclick = () => gpSelectZone(i);
    zt.appendChild(b);
  }
  while (zt.children.length > presetN) zt.lastChild.remove();
  [...zt.children].forEach((b, i) => {
    b.classList.toggle('on', i === gpSel);
    b.textContent = 'Zone ' + (i + 1) + (Object.keys(gpz[i] || {}).length ? ' \u00b7 ' + Object.keys(gpz[i]).length : '');
  });
}
// After a live edit, ticked settings follow what the camera changes itself (a few s)
let gpFollow = null;
function gpFollowCam(gs) {
  if (!gpFollow || !gs || !gpCamReady) return;
  if (Date.now() > gpFollow.until) { gpFollow = null; return; }
  const z = gpz[gpFollow.zone]; let ch = 0;
  for (const id in z) if (!gpFollow.ids.includes(id) && gs[id] !== undefined && gs[id] !== gpFollow.base[id] && gs[id] !== z[id] && gpKnown(id, gs[id])) { z[id] = gs[id]; ch++; }
  if (ch) { gpRender(); camEdited(); }
}
async function gpPushLive(ids, gapMs) {
  const z = gpz[gpSel];
  const gs = lastStatus && lastStatus.cam_state && lastStatus.cam_state.gp_set;
  gpFollow = ids.length < 4 && gs ? { zone: gpSel, ids: ids.map(String), base: { ...gs }, until: Date.now() + 9000 } : null;
  gpSyncing = true; gpRender();
  for (const id of ids) {
    if (z[id] === undefined) continue;
    const r = await post('/api/camera/set', { id: +id, val: z[id] });
    if (!r.ok) { gpSyncing = false; gpRender(); msg('camera did not take ' + GP_SET[id][0] + (r.error ? ': ' + r.error : '')); return; }
    if (gapMs) await new Promise(res => setTimeout(res, gapMs));   // let the camera finish one setting (resolution) before the next
  }
  setTimeout(() => { if (gpSyncing) { gpSyncing = false; gpRender(); } }, 6000);   // no change came back
}
function gpEdit(row, on) {
  const id = row.dataset.id, sel = row.querySelector('select'), z = gpz[gpSel];
  const angle = (id === '3' && z[145] !== undefined && gpFpsNum(z)) ? gpAngleOf(z[145], gpFpsNum(z)) : 0;
  if (on) z[id] = +sel.value; else delete z[id];
  const push = on ? [id] : [];
  if (angle && gpFpsNum(z)) {                                   // frame rate changed: keep the same angle
    const nv = gpShutterFor(angle, gpFpsNum(z));
    if (nv !== undefined && nv !== z[145]) { z[145] = nv; if (on) push.push('145'); }
  }
  gpRender(); camEdited();
  if (gpCamReady && push.length) gpPushLive(push);
}
$('#gp-rows').addEventListener('change', e => {
  const row = e.target.closest('.gpr'); if (!row) return;
  gpEdit(row, e.target.type === 'checkbox' ? e.target.checked : true);
});
// Select a zone; it is applied to the camera unless `quiet`.
function gpSelectZone(i, quiet) {
  gpSel = i; gpRender();
  const z = gpz[i], ids = GP_GROUPS.flatMap(g => g[1]).filter(id => z[id] !== undefined);
  if (!quiet && gpCamReady && ids.length) {
    msg('applying zone ' + (i + 1) + ' to the camera');
    gpPushLive(ids, 350);
  }
}
$('#gp-clear').onclick = () => { gpz[gpSel] = {}; gpRender(); camEdited(); };
$('#gp-capture').onclick = () => {
  const gs = lastStatus && lastStatus.cam_state && lastStatus.cam_state.gp_set;
  const ids = gs ? Object.keys(gs).filter(k => GP_SET[k] && gpKnown(k, gs[k]) && (!gpCapsOn() || (gpCaps[k] || []).includes(gs[k]))) : [];
  if (!ids.length) { msg('camera has not reported its settings yet'); return; }
  gpz[gpSel] = {}; ids.forEach(k => { gpz[gpSel][k] = gs[k]; });
  gpRender(); camEdited();
  msg('captured ' + ids.length + ' settings into zone ' + (gpSel + 1));
};
function gpApplyText(za) {
  if (!za || !za.zone) return '\u2014';
  const n = { none: 'nothing to push', waiting: 'waiting for camera', applying: 'applying\u2026',
              ok: 'applied', partial: 'partly applied', failed: 'not applied' }[za.state] || za.state;
  return 'Zone ' + za.zone + ': ' + n +
    (za.fail && za.fail.length ? ' (not accepted: ' + za.fail.map(i => GP_SET[i] ? GP_SET[i][0] : i).join(', ') + ')' : '');
}

// ---- config load/save ----
function setField(name, val) {
  const el = document.querySelector(`[name="${CSS.escape(name)}"]`);
  if (!el) return;
  if (el.type === 'checkbox') el.checked = !!val;
  else el.value = val;
}
function getField(name) {
  const el = document.querySelector(`[name="${CSS.escape(name)}"]`);
  if (!el) return undefined;
  if (el.dataset.m !== undefined) return Number(el.dataset.m);   // Remote ID distances: metres, whatever is shown
  if (el.type === 'checkbox') return el.checked;
  if (el.type === 'number') return el.value === '' ? 0 : Number(el.value);
  const v = el.value;
  if (el.tagName === 'SELECT' && /^-?\d+$/.test(v)) return Number(v);
  return v;
}

function fillConfig(c) {
  const walk = (obj, prefix) => {
    for (const k in obj) {
      const v = obj[k], name = prefix ? prefix + '.' + k : k;
      if (Array.isArray(v)) {
        v.forEach((item, i) => {
          if (item && typeof item === 'object') walk(item, name + '.' + i);
          else setField(name + '.' + i, item);
        });
      } else if (v && typeof v === 'object') walk(v, name);
      else setField(name, v);
    }
  };
  walk(c, '');
}

function collectConfig(auto) {      // auto: only the Cam control and OSD tabs
  const c = auto ? { preset:{bands:[]}, osd:{lines:[]} } : { record:{switch:{}}, preset:{bands:[]}, osd:{lines:[],warn:{}},
              clock:{}, gps_push:{}, hilight:{}, msp:{}, wifi:{} };
  const set = (path, val) => {
    const p = path.split('.'); let o = c;
    for (let i = 0; i < p.length - 1; i++) {
      const key = p[i], next = p[i+1];
      if (/^\d+$/.test(next)) { o[key] = o[key] || []; }
      else { o[key] = o[key] || {}; }
      o = o[key];
    }
    o[p[p.length-1]] = val;
  };
  $$(auto ? AUTO_F : 'input[name],select[name]').forEach(el => {
    let name = el.name, val = getField(name);
    if (name.startsWith('osd.lines.')) { c.osd.lines[+name.split('.')[2]] = val; return; }
    set(name, val);
  });
  c.preset.n_zones = presetN;
  for (let i = 0; i < presetN; i++) { c.preset.bands[i] = c.preset.bands[i] || {}; c.preset.bands[i].gp = gpz[i] || {}; }
  if (auto) return c;
  c.msp.baud = Number(c.msp.baud);
  c.rid.ignore = ridIgnore();            // replaces the whole stored list; blanks/duplicates dropped
  return c;
}

let cfgBrand = '';            // paired camera brand, drives field gating
let gateBrand = null;         // brand the OSD palette was last gated for
let rebootBaseline = '';      // msp.baud + swap_pins at last load, to detect reboot-needed

// ---- per-camera settings ----
let viewCam = '', camStored = true, camSig = '', camAuto = false, camConn = '';   // viewCam: 12 hex digits of the camera on screen ('' = none chosen)
const camHex = a => (a || '').replace(/:/g, '').toLowerCase();
const camList = () => (lastStatus && lastStatus.camera && lastStatus.camera.cameras) || [];
const camAct = () => camList().find(c => c.active);
const viewIsActive = () => !camAct() || camHex(camAct().addr) === viewCam;
const camQ = () => viewCam && camList().length ? '?cam=' + viewCam : '';
function camselUi() {
  const L = camList(), t = $('#tabs .on'), s = $('#camsel-s');
  const inTab = !!(t && ['cam-control', 'osd', 'rid', 'clock'].includes(t.dataset.t));
  const none = !!lastStatus && !L.length && inTab;
  $('#nocam').hidden = !none;                         // nothing paired: only the pairing message
  $$('.tab').forEach(x => x.classList.toggle('nocam-hide', none && x.classList.contains('on')));
  $('#camsel').hidden = !(L.length > 0 && inTab);
  const sig = L.map(c => camHex(c.addr) + c.name + (c.connected ? 1 : c.active ? 2 : 0)).join();
  if (sig !== camSig) {                               // rebuild only on a change: it would close an open list
    camSig = sig; s.textContent = '';
    const o0 = el('option'); o0.value = ''; o0.textContent = 'Select a camera…'; s.appendChild(o0);
    L.forEach(c => { const o = el('option'); o.value = camHex(c.addr);
      o.textContent = c.name + (c.connected ? ' (connected)' : c.active ? ' (active)' : ''); s.appendChild(o); });
  }
  if (viewCam && !L.some(c => camHex(c.addr) === viewCam)) { viewCam = ''; camAuto = false; }      // the chosen camera is gone: choose again
  s.value = viewCam;
  if (L.length && !viewCam && !camAuto) {              // nothing chosen yet: the only camera, else the connected one, else the last one chosen
    let m = ''; try { m = localStorage.getItem('camsel') || ''; } catch (e) {}
    const p = L.length === 1 ? L[0] : L.find(c => c.connected) || L.find(c => camHex(c.addr) === m);
    if (p) { camAuto = true; loadConfig(camHex(p.addr)); }     // else try again on the next poll (a camera may connect)
  }
  const cc = (L.find(c => c.connected) || {}).addr;   // a camera that has just connected takes the screen, unless edits are pending
  const ch = cc ? camHex(cc) : '';
  if (ch !== camConn) {
    camConn = ch;
    if (ch && ch !== viewCam && !dirtyCam) { camAuto = true; loadConfig(ch); }
  }
  const lock = L.length > 0 && !viewCam;              // no camera chosen: the camera pages are grayed out
  ['cam-control', 'osd', 'rid', 'clock'].forEach(id => { const x = $('#' + id); x.inert = lock; x.classList.toggle('camlock', lock); });
  $('#camsel-n').textContent = lock ? ''
    : !viewIsActive() ? 'Not connected: applies when it connects. Zone camera settings need it connected.'
    : camStored ? '' : 'No saved settings yet: starts as a copy of these.';
  $('#syncnow').disabled = !viewIsActive();
}
$('#nocam-go').onclick = () => showTab('camera');
$('#camsel-s').onchange = e => {
  if (dirtyCam && !confirm('Discard unsaved Remote ID / Clock changes?')) { e.target.value = viewCam; return; }
  camAuto = true;                                     // the user chose (even "Select a camera"): stop choosing for them
  loadConfig(e.target.value);
};

async function loadConfig(cam) {
  await autoSave();                                   // before the fields are refilled (and maybe for another camera)
  const keep = document.body.classList.contains('dirty') && !dirtyCam
    ? $$('input[name],select[name]').filter(x => !x.closest(AUTO + ',#rid,#clock')).map(x => [x, x.type === 'checkbox' ? x.checked : x.value]) : null;
  if (cam != null) { viewCam = cam; try { localStorage.setItem('camsel', cam); } catch (e) {} }
  const r = await fetch('/api/config' + camQ());
  if (r.status === 404) { viewCam = ''; return loadConfig(); }       // that camera was forgotten
  const c = await r.json();
  camStored = r.headers.get('X-Cam-Stored') !== '0';
  const nz = (c.preset && c.preset.n_zones) || 3;
  if (nz !== presetN) { presetN = nz; buildZoneRows(); buildBar($('#rb-preset')); }
  $$('[name^="rid.ignore."]').forEach(e => { e.value = ''; });   // the list may have shrunk
  fillConfig(c);
  ridDistLoad();
  gpz = Array.from({ length: presetN }, (_, i) => Object.assign({}, (((c.preset || {}).bands || [])[i] || {}).gp || {}));
  if (gpSel >= presetN) gpSel = 0;
  gpRender();
  osdLoad();
  if (c.ui_flags != null) { uiFlags = c.ui_flags | 0; setAdv(!!(uiFlags & 1), false); }
  const vc = camList().find(x => camHex(x.addr) === viewCam);
  cfgBrand = vc ? vc.brand : (c.camera && c.camera.brand) || '';
  camselUi();
  rebootBaseline = getField('msp.baud') + '/' + getField('msp.swap_pins');
  applyGates();
  ridUi();
  setDirty(false);
  if (keep && keep.length) { keep.forEach(([x, v]) => { if (x.type === 'checkbox') x.checked = v; else x.value = v; }); setDirty(true, keep[0][0]); }   // unsaved Network / System edits survive a camera switch
}

// ---- field gating ----
function gate(el, on, reason) {
  if (!el) return;
  el.querySelectorAll ? el.querySelectorAll('input,select').forEach(i => i.disabled = !on)
                      : (el.disabled = !on);
  const lg = el.tagName === 'FIELDSET' ? el.querySelector('legend') : el;
  if (el.tagName === 'FIELDSET') el.classList.toggle('disabled', !on);
  if (reason && !on && lg && lg.dataset) lg.title = reason;
}
function applyGates() {
  const mode = Number(getField('record.mode'));
  const recSw = mode === 2 || mode === 3 || mode === 4;
  gate($('#rec-switch'), recSw, 'only used with a switch trigger');
  setBarEnabled('record', recSw);

  const pOn = getField('preset.enabled');
  gate($('#preset-setup'), pOn, 'turn on “Enable zone control” first');
  setBarEnabled('preset', pOn);
  gpGate();

  const isGP = cfgBrand === 'gopro', isDJI = cfgBrand === 'dji';
  const gpSimple = isGP && getField('preset.gp_mode') === 1;      // GoPro "simple" zones switch the camera's own preset by id
  const idZones = isDJI || gpSimple;                     // zones that are just an id (DJI mode / GoPro preset), no settings builder
  $('#gp-mode-row').hidden = !isGP;
  $('#gp-builder').hidden = idZones;
  $$('#zone-ids .zid').forEach(e => { e.hidden = !idZones; });
  $$('#zone-ids .zt').forEach(e => { e.textContent = gpSimple ? 'Preset' : 'Mode ID'; });
  $$('#zone-ids .zcap').forEach(e => { e.hidden = !gpSimple; });
  zselRender();
  zwrap.hidden = !idZones;
  $('#dji-zone-note').hidden = !isDJI;
  $('#gp-simple-note').hidden = !gpSimple;
  $('#hilight-card').hidden = isDJI;                     // a DJI has no highlight tag
  gate($('#lbl-check'),  isGP  || !cfgBrand, 'GoPro only');
  gate($('#lbl-resync'), isDJI || !cfgBrand, 'DJI only');
  const gp = !!isDJI;
  $$('[name^="gps_push."]').forEach(i => i.disabled = !gp);
  if (gateBrand !== cfgBrand) {                          // OSD tokens the other brand cannot fill: grayed out, flagged where already used
    gateBrand = cfgBrand;
    $$('#palette .pchip[data-t]').forEach(b => { const ok = tokOk(b.dataset.t); b.disabled = !ok; b.title = ok ? '%' + b.dataset.t : tokWhy() + ' (this camera never reports it)'; });
    for (let i = 0; i < 4; i++) renderChips(i);
  }

  layoutAllBars();
  if (lastStatus) updateCursors(lastStatus.msp);
}
$$('[name="record.mode"],[name="preset.enabled"],[name="preset.gp_mode"]').forEach(el =>
  el.addEventListener('change', applyGates));

// ---- dirty tracking ----
// Cam control and OSD save themselves (0.7 s after the last edit); the Save button is for the other tabs.
// dirtyCam: unsaved edits in the other per-camera tabs (Remote ID, Clock), which belong to the camera on screen.
const AUTO = '#cam-control,#osd', AUTO_F = '#cam-control [name],#osd [name]';
let autoT = 0, autoQ = '', dirtyCam = false;
function setDirty(d, el) { document.body.classList.toggle('dirty', d); dirtyCam = d && (dirtyCam || !el || !!el.closest('#rid,#clock')); }
function camEdited() { clearTimeout(autoT); autoQ = camQ(); autoT = setTimeout(autoSave, 700); }
async function autoSave(keep) {
  if (!autoT) return;
  clearTimeout(autoT); autoT = 0;
  const r = await fetch('/api/config' + autoQ, { method: 'POST', keepalive: !!keep, headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(collectConfig(1)) }).then(x => x.json()).catch(() => ({ ok: false }));
  if (!r.ok) msg('not saved: ' + (r.errors ? r.errors.join(', ') : 'the bridge did not answer'));
}
document.addEventListener('input', e => { const t = e.target; if (t.name) t.closest(AUTO) ? camEdited() : setDirty(true, t); });
window.addEventListener('beforeunload', e => {
  autoSave(1);
  if (document.body.classList.contains('dirty')) { e.preventDefault(); e.returnValue = ''; }
});

$('#save').onclick = async () => {
  const pw = getField('wifi.password');
  if (pw && pw.length > 0 && pw.length < 8) return msg('Wi-Fi password must be at least 8 characters (or blank to keep).');
  const re = ridCheck();
  if (re) { showTab('rid'); re.el.focus(); return msg('Not saved: ' + re.t); }
  msg('saving…');
  const r = await fetch('/api/config' + camQ(), {method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify(collectConfig())}).then(r => r.json()).catch(() => ({ok:false}));
  if (!r.ok) { msg('save failed: ' + (r.errors ? r.errors.join(', ') : 'server rejected it')); return; }
  const changedReboot = (getField('msp.baud') + '/' + getField('msp.swap_pins')) !== rebootBaseline;
  await loadConfig();
  if (r.reboot_required) { ridRebootSaved = true; ridStatus(lastStatus); }
  if (r.reboot_required) { msg('Saved. Remote ID starts after a reboot.'); $('#rebootnow').hidden = false; }
  else if (changedReboot) { msg('Saved. Baud / pin-swap changes need a reboot to take effect.'); $('#rebootnow').hidden = false; }
  else { msg('Saved.'); }
};
$('#rebootnow').onclick = () => { if (confirm('Reboot the bridge now?')) post('/api/reboot'); };

// ---- password reveal ----
$$('.reveal').forEach(b => b.onclick = () => {
  const t = b.dataset.for;
  const el = /^[.#]/.test(t) ? document.querySelector(t)
                             : document.querySelector(`[name="${CSS.escape(t)}"]`);
  const show = el.type === 'password';
  el.type = show ? 'text' : 'password';
  b.textContent = show ? 'hide' : 'show';
});

// ---- actions ----
const post = (u, body) => fetch(u, body ? {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}
                                        : {method:'POST'}).then(r => r.json()).catch(()=>({ok:false}));

// ---- default-password modal ----
let setupDismissed = false;   // per-session: don't re-pop after a plain dismiss
function setupClose() {
  $('#setup').hidden = true;
  setupDismissed = true;
  if ($('#setup-nomore').checked && !(uiFlags & 2)) { uiFlags |= 2; persistFlags(); }
}
$('#setup-no').onclick = setupClose;
$('#setup').onclick = e => { if (e.target.id === 'setup') setupClose(); };
$('#setup-yes').onclick = () => {
  $('#setup-ask').hidden = true;
  $('#setup-form').hidden = false;
  $('#setup-pw').focus();
};
$('#setup-save').onclick = async () => {
  const pw = $('#setup-pw').value;
  if (pw.length < 8) { $('#setup-err').textContent = 'At least 8 characters.'; return; }
  $('#setup-err').textContent = '';
  $('#setup-save').disabled = true;
  const r = await post('/api/config', { wifi: { password: pw } });
  if (!r.ok) {
    $('#setup-save').disabled = false;
    $('#setup-err').textContent = 'Could not save: ' + (r.errors ? r.errors.join(', ') : 'server rejected it');
    return;
  }
  await post('/api/reboot');
  $('#setup .modal-card').innerHTML =
    '<h3>Rebooting…</h3><p>Reconnect to the RecLink Wi-Fi with the new password in ~10 s.</p>';
};

// ---- generic confirm modal (#ask) ----
let askCb = null;
function ask(h, body, yesLabel, cb, danger) {
  $('#ask-h').textContent = h;
  $('#ask-b').textContent = body;
  const y = $('#ask-yes');
  y.textContent = yesLabel;
  y.className = danger ? 'danger' : '';
  askCb = cb;
  $('#ask').hidden = false;
}
function askClose() { $('#ask').hidden = true; askCb = null; }
$('#ask-cancel').onclick = askClose;
$('#ask').onclick = e => { if (e.target.id === 'ask') askClose(); };
$('#ask-yes').onclick = () => { const cb = askCb; askClose(); if (cb) cb(); };

// Pairing modal, GoPro and DJI
let pairPhase = '';       // '' | 'discover' | 'bonding'
let pairStart = 0, pairPicked = '', pairBrand = 'gopro';
const pairNm = () => pairBrand === 'dji' ? 'DJI camera' : 'GoPro';
const pairSteps = () => pairBrand === 'dji'
  ? '<li>Switch the <b>Osmo</b> on and wake its screen. Keep it within a metre of the RecLink.</li>'
  + '<li>Close the DJI app on any phone: the camera takes one connection.</li>'
  + '<li>Press <b>Scan now</b>, tap your camera in the list, then <b>confirm the request on the camera&rsquo;s screen</b>.</li>'
  : '<li>On the camera: swipe down &rarr; <b>Preferences</b> &rarr; <b>Connections</b>.</li>'
  + '<li><b>Connect Device</b> &rarr; <b>The Remote</b>.</li>'
  + '<li>When the camera shows it&rsquo;s pairing, press <b>Scan now</b>.</li>';
const pairCardHTML = $('#pair .modal-card').innerHTML;   // restored on reopen (success panel replaces it)
function startPair(brand) {
  pairBrand = brand === 'dji' ? 'dji' : 'gopro';
  if (!$('#pair-ask')) $('#pair .modal-card').innerHTML = pairCardHTML;
  $('#pair-cancel').onclick = pairClose;
  $('#pair-go').onclick = pairGo;
  $('#pair-title').textContent = 'Pair a ' + (pairBrand === 'dji' ? 'DJI camera' : 'GoPro');
  $('#pair-steps').innerHTML = pairSteps();
  $('#pair-qr').hidden = pairBrand === 'dji';
  pairPhase = '';
  pairPicked = '';
  $('#pair-msg').textContent = '';
  $('#pair-list').textContent = '';
  $('#pair-ask').hidden = false;
  $('#pair').hidden = false;
}
$$('[data-pair]').forEach(b => b.onclick = () => {
  const cam = lastStatus && lastStatus.camera;
  const count = cam ? (cam.count | 0) : 0;
  if (count >= 4) return;
  if (count >= 1) {
    ask('Pair another camera?',
        'Your other camera' + (count > 1 ? 's stay' : ' stays') + ' paired. '
        + 'The current camera disconnects while pairing.',
        'Continue', () => startPair(b.dataset.pair));
  } else {
    startPair(b.dataset.pair);
  }
});
function pairClose() { $('#pair').hidden = true; pairPhase = ''; }
$('#pair-cancel').onclick = pairClose;
$('#pair').onclick = e => { if (e.target.id === 'pair') pairClose(); };
async function pairGo() {
  const r = await post('/api/camera/pair', {brand: pairBrand});
  if (!r.ok) { $('#pair-msg').textContent = 'Pair request failed.'; return; }
  pairPicked = '';
  $('#pair-list').textContent = '';
  pairPhase = 'discover';
  pairStart = Date.now();
  $('#pair-msg').textContent = pairBrand === 'dji' ? 'Searching for DJI cameras…' : 'Searching for GoPros in pairing mode…';
}
$('#pair-go').onclick = pairGo;
function pairPick(addr, name) {
  pairPicked = name || addr;
  pairPhase = 'bonding';
  pairStart = Date.now();
  $('#pair-list').textContent = '';
  $('#pair-msg').textContent = 'Pairing with ' + pairPicked + '…';
  post('/api/camera/pair', {brand: pairBrand, addr});
}
function renderFound(list) {
  const w = $('#pair-list');
  const keep = new Set((list || []).map(f => f.addr));
  [...w.children].forEach(b => { if (!keep.has(b.dataset.addr)) b.remove(); });
  (list || []).forEach(f => {
    let b = w.querySelector('[data-addr="' + f.addr + '"]');
    if (!b) {
      b = el('button'); b.type = 'button'; b.dataset.addr = f.addr;
      b.onclick = () => pairPick(f.addr, f.name);
      b.appendChild(el('b')); b.appendChild(el('small'));
      w.appendChild(b);
    }
    const nm = f.name || pairNm(), sig = f.addr + '  ·  ' + f.rssi + ' dBm';
    if (b.firstChild.textContent !== nm) b.firstChild.textContent = nm;
    if (b.lastChild.textContent !== sig) b.lastChild.textContent = sig;
  });
}
$('#camrefresh').onclick = async () => msg((await post('/api/camera/refresh')).ok ? 'refreshing camera…' : 'failed');
let testWake = 0;                                                 // a test record is waiting for an asleep camera to wake
async function camTest() {
  const r = await post('/api/camera/test');
  testWake = r.ok && r.waking ? Date.now() : 0;
  msg(!r.ok ? 'failed' : r.waking ? 'waking the camera\u2026' : 'recording 3 s\u2026');
}
$('#camtest').onclick = $('#rectest').onclick = camTest;

// ---- Binding tab: paired-camera list ----
const brandTag = b => b === 'gopro' ? 'GoPro' : b === 'dji' ? 'DJI' : (b || 'Camera');
const camLabel = c => c.name || (brandTag(c.brand) + ' ' + (c.addr || '').slice(-8));
const setText = (e, t) => { if (e && e.textContent !== t) e.textContent = t; };

function askForget(cam) {
  ask('Forget ' + camLabel(cam) + '?',
      'Erases the pairing for this camera only — your other cameras stay paired. Re-pair to reconnect it.',
      'Forget', async () => {
        msg('forgetting…');
        const r = await post('/api/camera/forget', {addr: cam.addr});
        msg(r.ok ? 'camera forgotten' : 'could not forget camera');
        poll();
      }, true);
}
// ---- camera names ----
let nickAddr = '', nickQueue = null, knownCams = null;
function nickOpen(cam, first) {
  nickAddr = cam.addr;
  $('#nick-h').textContent = first ? 'Name your camera' : 'Rename camera';
  $('#nick-b').textContent = first ? 'Give ' + (cam.model || 'the new camera') + ' a name. It is used in the camera selector and the Binding tab.' : '';
  $('#nick-in').value = cam.name;
  $('#nick').hidden = false;
  $('#nick-in').select();
}
const nickClose = () => { $('#nick').hidden = true; };
async function nickSave() {
  const r = await post('/api/camera/name', {addr: nickAddr, name: $('#nick-in').value});
  nickClose(); msg(r.ok ? 'name saved' : 'could not save the name'); poll();
}
$('#nick-save').onclick = nickSave;
$('#nick-skip').onclick = nickClose;
$('#nick-in').onkeydown = e => { if (e.key === 'Enter') nickSave(); };
function nickWatch(cams) {                  // a camera that was not there before = just paired: ask for a name
  if (knownCams) cams.forEach(c => { if (!knownCams.has(c.addr)) nickQueue = c; });
  knownCams = new Set(cams.map(c => c.addr));
  if (nickQueue && $('#pair').hidden && $('#nick').hidden) {   // wait for the GoPro pairing panel to finish
    const c = cams.find(x => x.addr === nickQueue.addr); nickQueue = null;
    if (c) nickOpen(c, true);
  }
}
$('#forgetall').onclick = () => ask('Forget all cameras?',
  'Erases every pairing. The bridge has no camera until you pair one again.',
  'Forget all', async () => {
    msg('forgetting…');
    const r = await post('/api/camera/forget');
    msg(r.ok ? 'all cameras forgotten' : 'failed');
    poll();
  }, true);

function renderCams(c) {
  const cams = (c && c.cameras) || [];
  const count = c ? (c.count | 0) : cams.length;
  $$('#pairrow [data-pair]').forEach(b => b.disabled = count >= 4);
  $('#pairfull').hidden = count < 4;
  const up = cams.some(x => x.connected);
  $('#camtest').disabled = $('#rectest').disabled = !up && !(c && c.asleep);   // an asleep camera is woken first
  if (testWake && (up || Date.now() - testWake > 30000)) { const go = up; testWake = 0; if (go) setTimeout(camTest, 1500); }
  { const on = cams.some(x => x.connected && camHex(x.addr) === viewCam);      // the camera on screen is the connected one
    $$('.zapply,.zcap').forEach(b => { b.disabled = !on; }); }
  $('#forgetall').hidden = count < 2;

  const wrap = $('#camlist');
  if (!cams.length) {
    if (!wrap.querySelector('.camempty'))
      wrap.innerHTML = '<p class="muted camempty">No cameras paired yet.</p>';
    return;
  }
  const gone = wrap.querySelector('.camempty'); if (gone) gone.remove();
  const keep = new Set(cams.map(x => x.addr));
  [...wrap.children].forEach(r => { if (!keep.has(r.dataset.addr)) r.remove(); });

  cams.forEach((cam, idx) => {
    let r = wrap.querySelector('[data-addr="' + cam.addr + '"]');
    if (!r) {
      r = el('div', 'camrow');
      r.dataset.addr = cam.addr;
      const opts = '<option value="5">Fast (0.5 s)</option>'
                 + '<option value="10">Normal (1 s)</option>'
                 + '<option value="15">Slow (1.5 s)</option>';
      r.innerHTML = '<div class="camtop-wrap"><div class="caminfo"><div class="camtop">'
        + '<b class="camname"></b><span class="tag"></span></div>'
        + '<div class="cammeta mono"></div>'
        + '<label class="camwake" title="When RecLink powers up, switch this GoPro on if it is asleep. One camera at a time; switched off later, it stays off until a recording starts.">'
        + '<input type="checkbox" class="wakechk">Wake at power-up</label></div>'
        + '<div class="camright"><span class="badge"></span>'
        + '<button type="button" class="ghost rename">Rename</button>'
        + '<button type="button" class="forget">Forget</button></div></div>'
        + '<div class="camadv adv-only"><span>Telemetry poll</span>'
        + '<select class="pollsel" aria-label="Telemetry poll interval">' + opts + '</select></div>';
      r.querySelector('.wakechk').onchange = async e => {
        const res = await post('/api/camera/poll', { addr: r.dataset.addr, wake: e.target.checked });
        if (!res.ok) msg('could not save');
        poll();
      };
      const ps = r.querySelector('.pollsel');
      ps.onchange = async () => {
        const res = await post('/api/camera/poll', { addr: r.dataset.addr, poll_ds: +ps.value });
        msg(res.ok ? 'poll interval saved' : 'could not save');
      };
    }
    r.querySelector('.forget').onclick = () => askForget(cam);
    r.querySelector('.rename').onclick = () => nickOpen(cam, false);
    setText(r.querySelector('.camname'), camLabel(cam));
    setText(r.querySelector('.tag'), brandTag(cam.brand));
    setText(r.querySelector('.cammeta'), (cam.model && cam.model !== cam.name ? cam.model + ' · ' : '') + cam.addr);
    const badge = r.querySelector('.badge');
    let txt = 'Standby', cls = 'badge';
    if (cam.connected) { txt = 'Connected · ' + c.rssi + ' dBm'; cls = 'badge ok'; }
    else if (cam.asleep) { txt = 'Asleep'; cls = 'badge warn'; }
    else if (cam.active) { txt = 'Connecting…'; cls = 'badge warn'; }
    setText(badge, txt);
    if (badge.className !== cls) badge.className = cls;
    // Telemetry poll: GoPro only (DJI pushes status on its own schedule).
    r.querySelector('.camadv.adv-only').hidden = r.querySelector('.camwake').hidden = cam.brand !== 'gopro';
    r.querySelector('.wakechk').checked = !!cam.wake;
    const ps = r.querySelector('.pollsel');
    if (document.activeElement !== ps && cam.poll_ds) ps.value = cam.poll_ds;
    // Insert only when out of place -- re-parenting a node closes an open
    // <select> dropdown inside it (that's why it kept snapping shut).
    if (wrap.children[idx] !== r) wrap.insertBefore(r, wrap.children[idx] || null);
  });
}
$('#syncnow').onclick = async () => {
  msg('syncing…');
  const r = await post('/api/clock/sync');
  if (r.ok) msg('sync queued' + (r.last_delta_ms != null ? ' (previous delta ' + r.last_delta_ms + ' ms)' : ''));
  else msg('sync not done: ' + (r.error || 'failed'));
};
$('#identify').onclick = () => { post('/api/identify'); msg('LED blinking for 3 s'); };
$('#reboot').onclick = () => { if (confirm('Reboot the bridge? The portal drops for ~10 s.')) { post('/api/reboot'); msg('rebooting…'); } };
$('#factory').onclick = () => {
  if (!confirm('Factory reset: erase ALL settings AND camera pairing, then reboot. Continue?')) return;
  if (!confirm('Really erase everything? This cannot be undone.')) return;
  fetch('/api/config?factory=1', {method:'POST'});
  msg('erasing and rebooting…');
};

$('#mspscan').onclick = async () => {
  try {
    const d = await fetch('/api/msp/scan').then(r => r.json());
    $('#scanout').textContent = (d.aux || []).map(a => `AUX${a.aux}: ${a.us} µs`).join('\n') || 'no AUX channels reported (FC offline?)';
  } catch (e) { $('#scanout').textContent = 'scan failed (FC offline?)'; }
};

// ---- firmware flash modal ----
$('#flash-cancel').onclick = () => { $('#flash').hidden = true; };
$('#otago').onclick = () => {
  const f = $('#otafile').files[0];
  if (!f) return msg('choose a .bin file first');
  if (f.size < 500000) return msg('that file looks too small — use build/fc_cam_bridge.bin');
  const erase = $('#otaerase').checked;
  $('#flash-info').innerHTML = 'Flash <b>' + (f.size / 1024 | 0) + ' KB</b> now?'
    + (erase ? ' <span class="warn-inline">All settings and camera pairing will be erased.</span>' : '')
    + ' The bridge reboots when done.';
  $('#flash-ask').hidden = false;
  $('#flash-progress').hidden = true;
  $('#flash-bar').value = 0;
  $('#flash').hidden = false;
};
$('#flash-go').onclick = () => {
  const f = $('#otafile').files[0];
  if (!f) { $('#flash').hidden = true; return; }
  const erase = $('#otaerase').checked;
  $('#flash-ask').hidden = true;
  $('#flash-progress').hidden = false;
  $('#flash-msg').textContent = 'Uploading… 0 %';
  const x = new XMLHttpRequest();
  x.open('POST', '/api/ota' + (erase ? '?erase=1' : ''));
  x.upload.onprogress = e => {
    if (!e.lengthComputable) return;
    const p = Math.round(100 * e.loaded / e.total);
    $('#flash-bar').value = p;
    $('#flash-msg').textContent = p < 100 ? ('Uploading… ' + p + ' %') : 'Writing to flash…';
  };
  x.onload = () => {
    let r = {}; try { r = JSON.parse(x.responseText); } catch (_) {}
    if (r.ok) {
      $('#flash-bar').value = 100;
      $('#flash .modal-card').innerHTML = '<h3>Flashed</h3><p id="flash-done">'
        + (r.erased ? 'Settings erased. ' : '')
        + 'Rebooting on the new firmware…</p>';
      // After the reboot, hard-reload to replace cached files.
      const reloadFresh = () => location.replace(
        location.pathname + '?v=' + Date.now());
      let tries = 0;
      const waitForDevice = () => {
        tries++;
        fetch('/api/status?_=' + Date.now(), { cache: 'no-store' })
          .then(res => { if (res.ok) { reloadFresh(); } else { throw 0; } })
          .catch(() => {
            const p = $('#flash-done');
            if (tries > 30) {
              if (p) p.textContent = r.erased
                ? 'Rebooted with default settings — reconnect to the RecLink Wi-Fi (password: reclinkfpv), then reload this page.'
                : 'Still rebooting — reconnect to the RecLink Wi-Fi and reload this page.';
              return;
            }
            if (p) p.textContent = 'Rebooting on the new firmware… (' + tries + ')';
            setTimeout(waitForDevice, 2000);
          });
      };
      setTimeout(waitForDevice, 5000);
    } else {
      $('#flash-progress').hidden = true;
      $('#flash-ask').hidden = false;
      $('#flash-info').innerHTML = '<span class="warn-inline">Flash failed: '
        + (r.error || ('HTTP ' + x.status)) + '</span> — current firmware kept.';
    }
  };
  x.onerror = () => {
    $('#flash-progress').hidden = true;
    $('#flash-ask').hidden = false;
    $('#flash-info').innerHTML = '<span class="warn-inline">Upload dropped</span> — current firmware kept.';
  };
  x.send(f);
};

// ---- live status ----
const D = [['prof','Profile'],['isomax','ISO max'],['isomin','ISO min'],['shut','Shutter angle'],['ptune','Protune'],
           ['wb','White balance'],['color','Color'],['sharp','Sharpness'],['ev','EV comp'],['p10','Protune 10-bit'],['zap','Zone settings']];
function buildGrid(el, list) {
  el.innerHTML = list.map(r =>
    `<div class="k">${r[1]}</div><div class="v" id="s-${r[0]}">—</div>`).join('');
}
$('#stat-detail').innerHTML = D.map(r =>
  `<div class="kv" data-h hidden><span>${r[1]}</span><span id="s-${r[0]}"></span></div>`).join('');
function put(id, text, cls) {
  const el = $('#s-' + id); if (!el) return;
  if (el.textContent !== text) el.textContent = text;
  el.className = 'v' + (cls ? ' ' + cls : '');
  const q = el.parentNode; if (q.dataset.h !== undefined) q.hidden = text === '\u2014';   // status tab: hide what has no value
}
function fmtT(s) { s = s|0; return s >= 3600 ? (s/3600|0)+'h'+((s%3600/60)|0)+'m'
                                             : (s/60|0)+':'+String(s%60).padStart(2,'0'); }
function fmtUp(s) { s = s|0; const d = s/86400|0, h = (s%86400)/3600|0, m = (s%3600)/60|0;
  return d ? d+'d '+h+'h' : h ? h+'h '+m+'m' : m+'m'; }

let lastFlags = '', lastCounters = '', lastDbg = '', lastRecDbg = '', lastBleDbg = '';
function setOffline(off) {
  document.body.classList.toggle('offline', off);
  $('#conn').hidden = !off;
  if (off) { $('#fw').textContent = 'offline'; updateCursors(null); document.body.classList.remove('rec', 'camup'); }
}

async function poll() {
  let s;
  try {
    s = await fetch('/api/status').then(r => { if (!r.ok) throw 0; return r.json(); });
  } catch (e) { setOffline(true); return; }
  setOffline(false);
  lastStatus = s;
  $('#wifi-trig').hidden = !!s.reclink_board;

  $('#fw').textContent = 'fw ' + s.fw + ' · up ' + fmtUp(s.uptime_s);
  const m = s.msp, c = s.camera, cs = s.cam_state;
  const fcUp = m && m.connected;
  const paired = !!(c && c.brand);
  if (c && c.cameras) nickWatch(c.cameras);

  // GoPro pairing modal
  if (pairPhase && !$('#pair').hidden) {
    const link = c ? c.link : '';
    if (pairPhase === 'discover') {
      renderFound(c && c.found);
      const n = (c && c.found) ? c.found.length : 0;
      $('#pair-msg').textContent = n ? 'Tap the camera to pair:'
        : pairBrand === 'dji' ? 'Searching for DJI cameras…' : 'Searching for GoPros in pairing mode…';
      if (!n && Date.now() - pairStart > 50000) {
        pairPhase = '';
        $('#pair-msg').textContent = pairBrand === 'dji'
          ? 'No DJI camera found. Make sure it is on and awake, then press Scan now again.'
          : 'No GoPro found. Make sure it is in remote-pairing mode, then press Scan now again.';
      }
    } else if (pairPhase === 'bonding') {
      if (link === 'READY') {
        pairPhase = '';
        const card = $('#pair .modal-card');
        card.textContent = '';
        const d = el('div', 'pair-done');
        const ck = el('div', 'check'); ck.textContent = '✓';
        const p = el('p'); p.textContent = (pairPicked || 'Camera') + ' paired and connected';
        d.appendChild(ck); d.appendChild(p);
        card.appendChild(d);
        setTimeout(() => { $('#pair').hidden = true; }, 2600);
      } else if (Date.now() - pairStart > 60000) {
        pairPhase = '';
        $('#pair-msg').textContent = 'Could not pair with ' + pairPicked + '. Try again.';
      } else {
        $('#pair-msg').textContent = pairBrand === 'dji' && link === 'SYNCING'
          ? 'Look at ' + pairPicked + ' and confirm the pairing request on its screen…'
          : 'Pairing with ' + pairPicked + '… (' + (link ? link.toLowerCase() : 'connecting') + ')';
      }
    }
  }

  // verdict banner + fact tiles
  const ready = !!(c && c.link === 'READY'), isRec = !!(cs && cs.recording === 'true');
  const camNm = paired ? (c.name || c.brand.toUpperCase()) : '';
  const sleepers = ((c && c.cameras) || []).filter(x => x.asleep).map(camLabel).join(', ');   // which camera is switched off
  const iss = [];                                                  // [text, 'bad'|'warn', acknowledgeable]
  if (!fcUp) iss.push(['No FC link', 'warn']);
  if (!paired) iss.push(['No camera paired', 'warn']);
  else if (!ready) iss.push(c.asleep ? [(sleepers || 'Camera') + ' asleep', 'warn'] : [c.link === 'LOST' ? 'Camera lost' : 'No camera', c.link === 'LOST' ? 'bad' : 'warn']);
  if (ready) {
    if (cs.sd_missing) iss.push(['No SD card', 'bad']); else if (cs.sd_full) iss.push(['SD card full', 'bad']);
    if (cs.hot) iss.push(['Camera hot', 'bad']);
    if (cs.battery_pct.valid && cs.battery_pct.v <= 15) iss.push(['Camera battery ' + cs.battery_pct.v + '%', 'warn']);
  }
  if (fcUp && m.gps.present && !m.gps.good) iss.push(['GPS: no fix', 'warn']);
  const names = {
    CFG_RESET:'Settings were reset to defaults', REPAIR_NEEDED:'Camera re-pair needed',
    OSD_UNSUPPORTED:'FC too old for custom OSD', CLOCK_SYNC_FAIL:'Clock sync failed',
    RID_FAULT:'Remote ID receiver stopped (RID RX FAIL) \u2014 reboot to restore', WDT_RECOVERED:'Recovered from a watchdog reset'
  };
  for (const f of s.flags || []) if (f !== 'FC_OFFLINE') {
    iss.push([names[f] || f, f === 'REPAIR_NEEDED' || f === 'RID_FAULT' ? 'bad' : 'warn', f === 'WDT_RECOVERED']);
  }
  const shoot = ready && cs.resolution.valid ? cs.resolution.v + (cs.fps.v || '') : '';
  const left = ready && cs.rec_remaining_s.valid ? fmtT(cs.rec_remaining_s.v) + ' left' : '';
  const info = [camNm, shoot, left].filter(Boolean).join(' \u00b7 ');
  $('#hero').className = 'hero ' + (isRec ? 'rec' : iss.some(x => x[1] === 'bad') ? 'bad' : iss.length ? 'warn' : 'ok');
  $('#hero-t').textContent = isRec ? 'REC ' + (cs.elapsed_s != null ? fmtT(cs.elapsed_s) : '') : iss.length ? 'Not ready' : 'Ready';
  $('#hero-s').textContent = isRec || !iss.length ? info : '';
  const fj = JSON.stringify(iss);
  if (fj !== lastFlags) {
    lastFlags = fj;
    $('#flags').innerHTML = iss.map(f => f[2] ? `<span class="ackflag" title="click to acknowledge">${f[0]} \u2715</span>`
      : `<span class="${f[1]}">${f[0]}</span>`).join('');
    const af = $('#flags').querySelector('.ackflag');
    if (af) af.onclick = async () => { await post('/api/flags/ack'); poll(); };
  }
  const tile = (id, v, cls, s1, s2) => {
    $('#t-' + id).className = 'tile ' + cls;
    $('#t-' + id + '-v').textContent = v; $('#t-' + id + '-s').textContent = s1;
    const g = $('#t-' + id + '-g'); if (g) g.textContent = s2 || '';
  };
  tile('ac', !fcUp ? 'No FC' : m.armed ? 'ARMED' : 'Disarmed', !fcUp ? 'warn' : m.armed ? 'bad' : 'ok',
       fcUp ? m.variant + ' \u00b7 MSP ' + m.api + ' \u00b7 OSD ' + m.canvas : 'Waiting for the flight controller',
       !fcUp ? '' : m.gps.present ? 'GPS ' + (m.gps.good ? 'fix' : 'no fix') + ' \u00b7 ' + m.gps.sats + ' sats' : 'No GPS on the FC');
  const asleep = paired && !ready && !!c.asleep;                  // switched off, still advertising (GoPro)
  const scan = paired && c.link === 'SCAN' && !asleep;            // hunting: no brand or model yet
  tile('cam', asleep ? 'Asleep' : scan ? 'Scanning' : paired ? camNm : 'None', ready ? 'ok' : paired ? (c.link === 'LOST' && !asleep ? 'bad' : 'warn') : '',
       asleep ? (sleepers ? sleepers + ' \u00b7 ' : '') + 'switched off' : scan ? '' : paired ? c.brand.toUpperCase() + ' \u00b7 ' + (ready ? 'connected \u00b7 ' + c.rssi + ' dBm' : c.link.toLowerCase())
              : 'Pair one on the Binding tab');
  { const sg = $('#sig'); sg.hidden = !ready; const rs = c ? c.rssi : 0; sg.className = 'sig s' + (rs > -60 ? 4 : rs > -70 ? 3 : rs > -80 ? 2 : 1); }
  $('#c-cam').hidden = !paired;

  put('batt', ready && cs.battery_pct.valid ? cs.battery_pct.v + '%' : '\u2014', ready && cs.battery_pct.valid && cs.battery_pct.v <= 15 ? 'bad' : '');
  { const bi = $('#m-bat'), bp = ready && cs.battery_pct.valid ? cs.battery_pct.v : 0;
    bi.style.width = bp + '%'; bi.className = bp <= 15 ? 'low' : ''; }
  put('sd', !ready ? '\u2014' : cs.sd_missing ? 'NO CARD' : cs.sd_full ? 'FULL'
      : cs.sd_remaining_kb.valid ? (cs.sd_remaining_kb.v/1024/1024).toFixed(1) + ' GB' : '\u2014',
      ready && (cs.sd_missing || cs.sd_full) ? 'bad' : '');
  put('tleft', ready && cs.rec_remaining_s.valid ? fmtT(cs.rec_remaining_s.v) : '\u2014');
  put('res', ready && cs.resolution.valid
      ? (cs.resolution.v + (cs.fps.v || '') + (cs.aspect.valid ? ' ' + cs.aspect.v : '')) : '\u2014');
  put('prof', ready && cs.profile.valid
      ? (cs.profile.v + (cs.bit_depth.valid ? ' ' + cs.bit_depth.v + 'bit' : '')) : '\u2014');
  document.body.classList.toggle('rec', isRec);
  document.body.classList.toggle('camup', ready);

  for (const [id, k] of [['isomax','iso_max'],['isomin','iso_min'],['ptune','protune'],
                         ['wb','wb'],['color','color'],['sharp','sharpness'],['ev','ev'],['p10','tune_10bit']]) {
    put(id, ready && cs[k] && cs[k].valid ? cs[k].v : '\u2014');
  }
  { // shutter as an angle where the frame rate is known: "180° (1/60)"
    const sh = cs && cs.shutter, fpsN = cs && cs.fps && cs.fps.valid ? +cs.fps.v : 0;
    const m = sh && sh.valid ? /^1\/(\d+)$/.exec(sh.v) : null;
    put('shut', !ready || !sh || !sh.valid ? '\u2014'
        : m && fpsN ? fmtAngle(Math.round(360 * fpsN / +m[1] * 10) / 10) + ' (' + sh.v + ')' : sh.v); }

  { const rdy = !!(c && c.link === 'READY') && viewIsActive();                 // capabilities only mean something while connected
    if (rdy !== gpCamReady) { gpCamReady = rdy; gpRender(); }
    gpFollowCam(cs && cs.gp_set);
    if (cs && cs.caps_gen !== undefined && cs.caps_gen !== gpCapsGen) { gpCapsGen = cs.caps_gen; gpLoadCaps(); } }
  if (cs && cs.model && cs.model !== gpModel) {        // learn the model once the camera reports it
    gpModel = cs.model;
    try { localStorage.setItem('gpmodel', gpModel); } catch (e) {}
    gpRender();
  }
  { const za = cs && cs.zone_apply, t = paired ? gpApplyText(za) : '\u2014';
    put('zap', t, za && (za.state === 'partial' || za.state === 'failed') ? 'warn' : za && za.state === 'ok' ? 'ok' : '');
    const gs = $('#gp-state'); if (gs.textContent !== t) gs.textContent = paired ? 'Last apply \u2014 ' + t : ''; }
  $('#stat-more').hidden = !$('#stat-detail .kv:not([hidden])');

  $('#aux').textContent = (fcUp && m.aux && m.aux.length)
    ? m.aux.map((v, i) => 'ch' + (i + 1) + ':' + v).join('  ') : '—';
  updateCursors(m);
  auxDetectCheck(m);

  // AP still on the factory password: nag to change it (unless suppressed).
  if (s.wifi && s.wifi.default_pw && !s.wifi.pw_nag_off && !setupDismissed && $('#setup').hidden) {
    $('#setup').hidden = false;
  }

  const cj = JSON.stringify(s.errors || []);
  if (cj !== lastCounters) {
    lastCounters = cj;
    $('#counters').textContent = (s.errors || []).map(e =>
      /^[a-z_]+:/.test(e) ? e.replace(':', ' = ') : 'last disconnect: ' + e).join('\n');
  }

  const g = (m && m.debug) || {};
  const dj = JSON.stringify(g);
  if (dj !== lastDbg) {
    lastDbg = dj;
    const armFlags = (g.arm_disable_flags | 0);
    // Boot-timing lines: historical ("since this boot"), so shown even if the
    // FC is offline right now -- that's exactly when they're most useful.
    const bootLines = [
      'MSP link up at  : ' + (g.link_up_first_ms ? (g.link_up_first_ms / 1000).toFixed(2) + ' s after boot' : 'not yet this boot'),
      'OSD first sent  : ' + (g.osd_first_ms ? (g.osd_first_ms / 1000).toFixed(2) + ' s after boot' : 'not yet this boot'),
    ];
    $('#mspdbg').textContent = fcUp ? bootLines.concat([
      '',
      'RC channel poll : ' + (g.rc_hz || 0).toFixed(1) + ' Hz'
        + (g.rc_period_ms ? '  (' + g.rc_period_ms + ' ms, nominal ' + (g.rc_nominal_hz||10) + ' Hz)' : '  (no data)'),
      'RC data age     : ' + (g.rc_age_ms >= 0 ? g.rc_age_ms + ' ms' : '—'),
      'RC channels seen: ' + (g.rc_channels || 0),
      'MSP_STATUS poll : ' + (g.status_hz || 0).toFixed(1) + ' Hz'
        + (g.status_period_ms ? '  (' + g.status_period_ms + ' ms, nominal ' + (g.status_nominal_hz||20) + ' Hz)' : '  (no data)'),
      '',
      'Armed (raw bit) : ' + (g.armed_raw ? 'ARMED' : 'disarmed'),
      'BOXARM bit index: ' + (g.arm_bit_resolved ? g.arm_bit : 'not resolved (waiting for MSP_BOXIDS)'),
      'Arming-disable  : ' + (armFlags ? '0x' + armFlags.toString(16).toUpperCase() : '0 (armable)')
    ]).join('\n') : bootLines.concat(['', 'FC offline — no MSP data']).join('\n');
  }

  const bl = s.ble || {};
  const blj = JSON.stringify(bl);
  if (blj !== lastBleDbg) {
    lastBleDbg = blj;
    const rdy = bl.link === 'READY';
    $('#bledbg').textContent = [
      'Link state      : ' + (bl.link || '—') + '  (' + (bl.state_age_s || 0) + ' s)',
      'Scan            : ' + (bl.scan || '—'),
      'Signal          : ' + (rdy ? bl.rssi + ' dBm' : '—'),
      'Last data       : ' + (rdy ? bl.last_rx_ms + ' ms ago' : '—'),
      'ATT MTU         : ' + (rdy && bl.mtu ? bl.mtu : '—'),
      'Poll interval   : ' + (rdy && bl.poll_ms ? (bl.poll_ms / 1000) + ' s' : '—'),
      'Bonds cfg/stack : ' + (bl.cfg_bonds || 0) + ' / ' + (bl.stack_bonds || 0),
      'Notify drops    : ' + (bl.notify_drops || 0),
      'Re-pair needed  : ' + (bl.repair_needed ? 'YES' : 'no')
    ].join('\n');
  }

  const rd = s.record || {};
  const rj = JSON.stringify(rd);
  if (rj !== lastRecDbg) {
    lastRecDbg = rj;
    const modeNames = ['off', 'on arm', 'on switch', 'armed OR switch', 'armed AND switch'];
    const recTriggerNames = ['switch', 'button'];
    $('#recdbg').textContent = fcUp ? [
      'Record mode      : ' + (modeNames[rd.mode] || rd.mode),
      'Trigger type     : ' + (recTriggerNames[rd.sw_mode] || rd.sw_mode),
      'AUX vs threshold : ' + (rd.raw_active ? 'above' : 'below'),
      'Switch/button    : ' + (rd.sw_active ? 'on' : 'off')
        + (recTriggerNames[rd.sw_mode] === 'button' ? ' (latched — press again to flip)' : ''),
      'Desired recording: ' + (rd.desired ? 'YES' : 'no'),
      'Last cmd to cam  : ' + (rd.last_cmd || '—'),
      'Camera reports   : ' + (cs && cs.recording ? cs.recording : 'unknown'),
      'Trigger phase    : ' + (rd.phase || '—')
    ].join('\n') : 'FC offline — no MSP data';
  }

  if (s.clock) {
    const ls = s.clock.last_sync;
    $('#clocklive').textContent =
      'FC UTC : ' + (m && m.fc_utc ? new Date(m.fc_utc*1000).toISOString() : '—') + '\n' +
      'Camera : ' + (s.clock.readable ? (s.clock.cam_utc ? new Date(s.clock.cam_utc*1000).toISOString() : '?')
                                      : 'n/a (DJI is write-only)') + '\n' +
      'Delta  : ' + s.clock.delta_s + ' s\n' +
      'Last   : ' + (ls ? ls.age_s + ' s ago · ' + ls.result : 'never');
  }
  renderPreview();
  renderCams(c);
  try { ridStatus(s); ridNearPoll(); camselUi(); } catch (e) {}
}

async function pollLog() {
  try { $('#log').textContent = await fetch('/api/logs').then(r => r.text()); } catch (e) {}
}

// ============ Remote ID tab ============
// Distance inputs show ft when imperial; metres live in dataset.m.
const FT = 3.28084, MI = 1609.344, DMIN = 20, DMAX = 5000;
const imp = () => +getField('osd.units') === 1;
const DIST = ['notice_m', 'warn_m', 'alert_m'].map(k => $(`[name="rid.warn.${k}"]`));
const DNAME = ['NOTICE', 'WARN', 'ALERT'];
let ridRebootSaved = false, ridBusy = false;

function ridDistLoad() { DIST.forEach(e => { e.dataset.m = e.value; }); ridDistShow(); }
function ridDistShow() {
  const f = imp(), lo = f ? Math.ceil(DMIN * FT) : DMIN, hi = f ? Math.floor(DMAX * FT) : DMAX;
  DIST.forEach((e, i) => {
    const m = e.dataset.m;
    e.value = m === '' || !f ? m : Math.round(m * FT);
    e.min = lo; e.max = hi;
    e.previousElementSibling.textContent = DNAME[i] + ' when closer than — ' + (f ? 'ft' : 'm') + ' (' + lo + '–' + hi + ')';
  });
}
function ridOsdDist(m) {
  if (imp()) { const ft = Math.round(m * FT); return ft < 1000 ? ft + 'FT' : (m / MI).toFixed(1) + 'MI'; }
  return m < 1000 ? m + 'M' : (m / 1000).toFixed(1) + 'KM';
}
function ridText(n, m, v, on) {      // n: aircraft id 1-9; v: 0 level, 1 above, 2 below; on: the leading ! is lit
  return (on ? '!' : ' ') + 'RID' + n + ' ' + ridOsdDist(m) + ' ' + ['↗', '↗', 'NE', '045'][+getField('rid.warn.bearing')] + ['', '+', '-'][v | 0];
}

const ridIgn = () => $$('[name^="rid.ignore."]');
function ridIgnore() {
  const o = [];
  ridIgn().forEach(e => { const v = e.value.trim(); if (v && !o.includes(v)) o.push(v); });
  return o;
}
// mirror of cfg_validate(); returns the first {el,t}
function ridCheck() {
  $$('#rid .rerr').forEach(p => { p.textContent = ''; });
  $$('#rid input.bad').forEach(e => e.classList.remove('bad'));
  const errs = [], bad = (el, t) => { el.classList.add('bad'); errs.push({el, t}); };
  const int = (n, lo, hi, w) => {
    const e = $(`[name="${n}"]`);
    if (!/^\d+$/.test(e.value) || e.value < lo || e.value > hi) bad(e, w + ' ' + lo + '–' + hi + '.');
  };
  int('rid.min_sats', 5, 12, 'Satellites:');
  int('rid.stale_s', +getField('rid.ble_duty') ? 2 : 10, 30, 'Expiry s:');
  const m = DIST.map(e => +e.dataset.m);
  DIST.forEach((e, i) => { if (!(m[i] >= DMIN && m[i] <= DMAX)) bad(e, DNAME[i] + ' distance ' + DMIN + '–' + DMAX + ' m.'); });
  if (!errs.some(x => DIST.includes(x.el))) {
    if (m[0] < m[1]) bad(DIST[0], 'NOTICE must be ≥ WARN.');
    if (m[1] < m[2]) bad(DIST[1], 'WARN must be ≥ ALERT.');
  }
  ridIgn().forEach(e => { if (/[^\x20-\x7e]/.test(e.value)) bad(e, 'ID: printable ASCII only.'); });
  errs.forEach(x => { const p = x.el.closest('.card').querySelector('.rerr'); p.textContent += (p.textContent ? ' ' : '') + x.t; });
  return errs[0];
}

function ridUi() {
  const on = getField('rid.enabled'), w = on && getField('rid.warn.enabled');
  gate($('#rid-recv'), on, 'turn on the receiver first');
  gate($('#rid-warnsw'), on);
  gate($('#rid-wopt'), w, 'turn on warnings first');
  gate($('#rid-mine'), on);
  const lo = +getField('rid.ble_duty') ? 2 : 10;
  $('#rid-stale-l').textContent = 'Position expires after — s (' + lo + '–30)';
  $('#rid-prev').textContent = [0, 1, 2].map(i => ridText(i + 1, DIST[i].dataset.m | 0, i, 1).padEnd(18) + DNAME[i] +
    [' (! steady)', ' (! slow blink)', ' (! fast blink, whole line)'][i]).concat('RID RX FAIL'.padEnd(18) + 'receiver failed').join('\n');
  ridNol(); ridCheck();
}
document.addEventListener('input', e => {
  const t = e.target;
  if (t.dataset && t.dataset.m !== undefined) t.dataset.m = t.value === '' || !imp() ? t.value : Math.round(t.value / FT);
  if (t.name && (t.name.startsWith('rid.') || t.name === 'osd.units')) ridUi();
});
document.addEventListener('change', e => {
  const t = e.target, n = t.name || '';
  if (n === 'osd.units') ridDistShow();
  if (n === 'rid.ble_duty' && !+t.value && +getField('rid.stale_s') < 10) { setField('rid.stale_s', 10); setDirty(true, t); }
  if (n === 'osd.units' || n.startsWith('rid.')) ridUi();
});

// ---- OSD preview: the RID message replaces the %rp line ----
const RID_TICK = 250, REDUCED = matchMedia('(prefers-reduced-motion: reduce)'), RTAG = ['', 'RID notice', 'RID warn', 'RID alert', 'RID alert'];
let ridPh = 0;
// 2 s steps: normal, NOTICE, WARN (alternating with normal), two ALERTs; the ! blinks (slow WARN, fast ALERT)
function ridDemo(ph) {
  const k = ph >> 3, m = DIST.map(e => e.dataset.m | 0);
  if (!k) return null;
  return ridText(k, [0, m[0] * .9, m[1] * .8, m[2] * .7, m[2] * .4][k] | 0, k % 3,
                 !getField('rid.warn.blink') || k === 1 || (k === 2 ? (ph >> 1 & 1) === 0 : (ph & 1) === 0));
}
const ridRpLine = () => [0, 1, 2, 3].findIndex(i => ($(`[name="osd.lines.${i}"]`).value || '').includes('%rp'));
function ridNol() {
  const none = ridRpLine() < 0;
  $('#osd-nol').hidden = !none;
  $('#rid-nol').hidden = !(none && getField('rid.enabled'));
}
setInterval(() => {
  if (document.hidden || REDUCED.matches || !$('#osd').classList.contains('on') || ridRpLine() < 0) return;
  ridPh = (ridPh + 1) % 40;
  renderPreview();
}, RID_TICK);

// ---- /api/status.rid ----
const fmtDist = m => imp() ? (m * FT < 1000 ? Math.round(m * FT) + ' ft' : (m / MI).toFixed(1) + ' mi')
                           : (m < 1000 ? m + ' m' : (m / 1000).toFixed(1) + ' km');
buildGrid($('#rid-health'), [['rh-state','Remote ID'],['rh-hdg','Heading'],['rh-ble','Bluetooth'],['rh-wifi','Wi-Fi'],['rh-fault','Faults']]);
function ridTestText(t) {
  if (!t || t.state === 'idle') return 'none since boot';
  const n = t.ble_frames + ' BLE' + (t.wifi ? ', ' + t.wifi_frames + ' Wi-Fi (' + t.beacon + ' beacon, ' + t.nan + ' NAN)' : '') + ' frames, ' + t.aircraft + ' aircraft';
  return t.state === 'run' ? 'running, ' + t.secs_left + ' s left, ' + n : (t.aborted ? 'stopped early (armed), ' : '') + n;
}
function ridStatus(s) {
  const r = s && s.rid; if (!r) return;
  if (!r.enabled || r.available === true || r.reboot_required === false) ridRebootSaved = false;
  const need = ridRebootSaved || r.reboot_required === true || (r.enabled && r.inactive_reason === 'reboot required');
  $('#rid-reboot').hidden = !need;
  if (need) $('#rebootnow').hidden = false;
  const f = r.fault || {}, b = r.ble || {}, w = r.wifi || {}, why = r.inactive_reason;
  put('rh-state', !r.enabled ? 'off' : r.active ? 'active' : 'inactive: ' + why,
      r.active ? 'ok' : why === 'memory allocation failed' ? 'bad' : r.enabled ? 'warn' : '');
  put('rh-hdg', r.hdg === 'mag' ? 'FC compass' : r.hdg === 'gps' ? 'GPS-aligned' : 'none');
  put('rh-ble', 'scan ' + b.scan + ', ' + b.rx + ' frames, ' + b.drops + ' dropped');
  put('rh-wifi', w.state + ', ' + w.rx + ' frames, ' + w.drops + ' dropped, ' + w.frames_30s + '/30 s');
  const fl = [f.subsystem && 'task stopped', f.ble && 'Bluetooth scan not running', f.wifi && 'Wi-Fi reception not running'].filter(Boolean).join(', ');
  put('rh-fault', fl ? fl + ' (OSD: RID RX FAIL)' : 'none', fl ? 'bad' : r.enabled ? 'ok' : '');
  ridTestPoll(r.test);
  setText($('#rid-teststat'), r.enabled ? 'Last test: ' + ridTestText(r.test) : 'Turn on and Save to test.');
  $('#rid-testbtn').disabled = !r.enabled || r.available === false || !!(r.test && r.test.state === 'run') || !!(s.msp && s.msp.armed);
}
$('#rid-reboot-btn').onclick = () => {
  if (confirm((document.body.classList.contains('dirty') ? 'Unsaved edits are lost. ' : '') + 'Reboot now? The portal drops for ~10 s.')) { post('/api/reboot'); msg('rebooting…'); }
};

// ---- reception test ----
$('#rid-testbtn').onclick = () => {
  if (document.body.classList.contains('dirty')) return msg('Save first: the test uses saved settings.');
  const w = +getField('rid.mode') !== 0;
  ask('Test reception for 30 s?', w
    ? 'Wi-Fi is included: the bridge\u2019s Wi-Fi drops for ~30 s and your phone disconnects. Reconnect after ~40 s and reopen this page.'
    : 'Bluetooth only: this page stays connected.', w ? 'Start (drops Wi-Fi)' : 'Start', ridTest);
};
async function ridTest() {
  msg('starting…');
  let x, r = {};
  try {
    x = await fetch('/api/rid/test', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({secs: 30})});
    r = await x.json().catch(() => ({}));
  } catch (e) { return msg('No answer. If it started, reconnect in ~40 s.'); }
  if (r.reboot_required) { ridRebootSaved = true; ridStatus(lastStatus); }
  msg(x.status === 409 ? 'Not while armed.' : x.status === 503 ? 'Busy: try again.'
    : x.status === 412 ? (r.reboot_required ? 'Reboot first.' : 'Turn on and Save first.')
    : !r.ok ? 'Not started.' : r.started === false ? 'Already running.'
    : r.wifi ? 'Wi-Fi dropping: reconnect in ~' + (r.secs + 10) + ' s, reopen this page.' : 'Test running\u2026');
}

// ---- test results: GET /api/rid/test (all fields optional) ----
let ridTShown = false, ridTBusy = false, ridT404 = false;
const CMP = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
const SRC = {ble: 'BLE', wifi: 'Wi-Fi', both: 'BLE + Wi-Fi'};
const VT = {above: '+ above', below: '- below', level: 'level'};
const rngBrg = a => fmtDist(a.dist_m) + ' · ' + String(a.brg).padStart(3, '0') + '° ' + CMP[Math.round(a.brg / 45) % 8] +
  (VT[a.vert] ? ' · ' + VT[a.vert] + (a.dv_m == null ? '' : ' ' + (a.dv_m > 0 ? '+' : '') + (imp() ? Math.round(a.dv_m * FT) + ' ft' : a.dv_m + ' m')) : '');
function ridTestList(d) {
  const box = $('#rid-tlist'), ac = d.ac || [];
  box.textContent = '';
  setText($('#rid-tempty'), ac.length ? (ac.some(a => a.dist_m != null) ? '' : 'Range/bearing need a GPS fix.')
    : d.state === 'run' ? 'No aircraft heard yet.' : 'No aircraft heard in this test.');
  ac.forEach(a => {
    const c = el('div', 'tac'), h = el('b', 'mono'), g = el('div');
    h.textContent = (a.n ? 'RID' + a.n + ' · ' : '') + a.id; g.textContent = [SRC[a.src] || a.src, a.age_ms / 1000 + ' s ago', a.dist_m != null && a.brg != null && rngBrg(a)].filter(Boolean).join(' · ');
    c.appendChild(h); c.appendChild(g); box.appendChild(c);
  });
}
async function ridTestPoll(t) {
  if (ridT404 || ridTBusy || document.hidden || !t || !$('#rid').classList.contains('on')) return;
  if (t.state === 'idle') { ridTShown = false; $('#rid-tlist').textContent = ''; setText($('#rid-tempty'), ''); return; }
  if (t.state === 'run') ridTShown = false; else if (ridTShown) return;
  ridTBusy = true;
  try {
    const r = await fetch('/api/rid/test');
    if (r.status === 404) ridT404 = true;
    else if (r.ok) { const d = await r.json(); ridTestList(d); ridTShown = d.state === 'done'; }   // 503: retry next tick
  } catch (e) {} finally { ridTBusy = false; }
}

// ---- Nearby now: GET /api/rid while this tab shows ----
const ridRows = new Map();
function ridMkRow() {
  const r = el('div', 'ridrow'), L = el('div', 'rl'), R = el('div', 'rr');
  const f = r._f = {id: el('div', 'mono'), a: el('div'), b: el('div', 'muted'), lv: el('b'), btn: el('button', 'ghost')};
  f.btn.type = 'button'; f.btn.textContent = 'Ignore';
  f.btn.onclick = () => ask('Ignore this aircraft?', '“' + r._id + '” is added to your own-aircraft list: no more warnings for it. Only do this for your own aircraft.', 'Add to my aircraft', () => ridIgnoreAdd(r._id), true);
  [f.id, f.a, f.b].forEach(x => L.appendChild(x)); R.appendChild(f.lv); R.appendChild(f.btn);
  r.appendChild(L); r.appendChild(R); return r;
}
function ridFill(r, a) {
  const f = r._f; r._id = a.id;
  setText(f.id, (a.n ? 'RID' + a.n + ' · ' : '') + a.id);
  setText(f.a, a.dist_m == null || a.brg == null ? 'range not confirmed' : rngBrg(a));
  setText(f.b, (SRC[a.src] || a.src) + ' \u00b7 ' + a.age_ms / 1000 + ' s ago');
  setText(f.lv, a.ignored ? 'IGNORED' : a.level ? a.level.toUpperCase() : '—');
  f.btn.hidden = !!a.ignored;
  f.btn.disabled = !/^[\x20-\x7e]{1,20}$/.test(a.id) || /^(BLE|WIFI|HEX):/.test(a.id) || ridIgnore().length > 3;
}
function ridList(d) {
  const box = $('#rid-list'), rs = (lastStatus && lastStatus.rid) || {}, seen = new Set();
  const ac = d.ac || [];
  ac.forEach((a, i) => {
    seen.add(a.id);
    let r = ridRows.get(a.id); if (!r) { r = ridMkRow(); ridRows.set(a.id, r); }
    ridFill(r, a);
    if (box.children[i] !== r) box.insertBefore(r, box.children[i] || null);
  });
  ridRows.forEach((r, id) => { if (!seen.has(id)) { r.remove(); ridRows.delete(id); } });
  setText($('#rid-nearmsg'), !d.active ? (rs.enabled ? 'Inactive (' + rs.inactive_reason + '): nothing listed.' : 'Remote ID is off.')
    : ac.length ? ac.length + ' heard' : 'None heard. Not the same as clear.');
}
async function ridNearPoll() {
  if (ridBusy || document.hidden || !$('#rid').classList.contains('on')) return;
  ridBusy = true;
  try {
    const r = await fetch('/api/rid');
    if (r.status !== 503) { if (!r.ok) throw 0; ridList(await r.json()); }   // 503 = busy: retry quietly
  } catch (e) { setText($('#rid-nearmsg'), 'Offline: last list shown.'); }
  finally { ridBusy = false; }
}
function ridIgnoreAdd(id) {
  const free = ridIgn().find(e => !e.value.trim());
  if (!free) return msg('My aircraft list is full.');
  free.value = id; setDirty(true, free); ridUi(); msg('Added. Save to apply.');
}

loadConfig().catch(() => msg('could not load config — is the bridge up?'));
poll(); setInterval(poll, 1500);
setInterval(() => {
  if (document.body.classList.contains('adv') && $('#system').classList.contains('on')) pollLog();
}, 3000);
