'use strict';
const $ = s => document.querySelector(s);
const $$ = s => [...document.querySelectorAll(s)];
const msg = t => { $('#msg').textContent = t; };
const el = (tag, cls) => { const e = document.createElement(tag); if (cls) e.className = cls; return e; };

// ---- tabs ----
function showTab(name) {
  $$('#tabs button').forEach(x => x.classList.toggle('on', x.dataset.t === name));
  $$('.tab').forEach(x => x.classList.toggle('on', x.id === name));
}
$$('#tabs button').forEach(b => b.onclick = () => showTab(b.dataset.t));

// ---- advanced mode (browser-local; gates .adv-only elements) ----
const advmode = $('#advmode');
function applyAdv(on) {
  document.body.classList.toggle('adv', on);
  // if the active tab just got hidden, fall back to Status
  const cur = $('#tabs button.on');
  if (cur && cur.classList.contains('adv-only') && !on) showTab('status');
}
advmode.checked = localStorage.getItem('advmode') === '1';
applyAdv(advmode.checked);
advmode.onchange = () => {
  localStorage.setItem('advmode', advmode.checked ? '1' : '0');
  applyAdv(advmode.checked);
};

// ============ Cam control tab: Betaflight-style AUX range bars =============
// name= attributes still map 1:1 to /api/config: record.switch.threshold_us,
// preset.bands.N.max_us / .id, record.switch.channel, preset.channel,
// hilight.channel. Zone 3's upper edge is fixed at 2100 us in firmware, so
// preset.bands.2.max_us is deliberately NOT a control (server keeps its value).
const US_MIN = 900, US_MAX = 2100, US_SPAN = US_MAX - US_MIN;
const clampUs = u => u < US_MIN ? US_MIN : u > US_MAX ? US_MAX : u;
const snapUs  = u => Math.round(clampUs(u) / 25) * 25;
const usPct   = u => (clampUs(u) - US_MIN) / US_SPAN * 100;

(function fillAux() {
  let o = '<option value="0">Disabled</option>';
  for (let i = 1; i <= 14; i++) { o += `<option value="${i}">AUX ${i}</option>`; }
  $$('select[data-auxsel]').forEach(s => { s.innerHTML = o; });
})();

const zwrap = $('#zone-ids');
for (let i = 0; i < 3; i++) {
  zwrap.insertAdjacentHTML('beforeend',
    `<div class="pz" data-z="${i}"><span class="zlab">Zone ${i + 1}</span>
       <label>up to &micro;s ${i < 2
         ? `<input type="number" inputmode="numeric" name="preset.bands.${i}.max_us" min="900" max="2100" step="25">`
         : `<span class="ro">2100 (fixed)</span>`}</label>
       <label>Preset / mode ID
         <input type="number" inputmode="numeric" name="preset.bands.${i}.id" min="0" step="1"></label></div>`);
}

const numAt = name => {
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
  return [snapUs(numAt('preset.bands.0.max_us') || 1300),
          snapUs(numAt('preset.bands.1.max_us') || 1700)];
}
function setBarVal(fn, i, u) {
  u = snapUs(u);
  if (fn === 'record') { setNum('record.switch.threshold_us', u); return; }
  if (fn === 'preset') {
    const v = barVals('preset');
    if (i === 0) u = Math.min(u, v[1] - 25);
    else         u = Math.max(u, v[0] + 25);
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
    (fn === 'preset' ? '<i class="seg z0"></i><i class="seg z1"></i><i class="seg z2"></i>'
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
  const nh = fn === 'record' ? 1 : fn === 'preset' ? 2 : 0;
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
    const p0 = usPct(v[0]), p1 = usPct(v[1]);
    b.segs[0].style.cssText = `left:0;width:${p0}%`;
    b.segs[1].style.cssText = `left:${p0}%;width:${p1 - p0}%`;
    b.segs[2].style.cssText = `left:${p1}%;right:0`;
    b.handles[0].style.left = p0 + '%';
    b.handles[1].style.left = p1 + '%';
    b.handles[0].setAttribute('aria-valuenow', v[0]);
    b.handles[1].setAttribute('aria-valuenow', v[1]);
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
          : presetLiveUs <= v[0] ? 0 : presetLiveUs <= v[1] ? 1 : 2;
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
['input', 'change'].forEach(ev => $('#cam-control').addEventListener(ev, e => {
  const n = (e.target && e.target.name) || '';
  if (n === 'record.switch.threshold_us') layoutBar('record');
  else if (/^preset\.bands\.[01]\.max_us$/.test(n)) { layoutBar('preset'); paintZones(); }
  if (/\.channel$/.test(n) && lastStatus) updateCursors(lastStatus.msp);
}));

// ================= OSD tab: chip line editor + live preview =================
const TOKLIST = [
  ['st','REC state'],['et','elapsed'],['rt','time left'],['bt','batt %'],['bb','batt bar'],
  ['rs','resolution'],['fp','fps'],['ar','aspect'],['ln','lens'],['pr','profile'],['bd','bit depth'],
  ['sd','SD left'],['md','mode'],['cr','cam RSSI'],['gs','GPS sats'],['gf','GPS fix'],['fb','FC batt'],
  ['sp','speed'],['cn','craft name'],['wr','warning']
];
const TOKS = {}; TOKLIST.forEach(t => TOKS[t[0]] = t[1]);
$('#tokens').innerHTML = TOKLIST.map(t => `<b>%${t[0]}</b> ${t[1]}`).join('<br>')
  + '<br><span class="muted">plus any literal text you type</span>';

const model = [[], [], [], []];
let focusLine = 0;
let lastStatus = null;

function parseTmpl(str) {
  str = str || ''; const out = []; let lit = '';
  for (let p = 0; p < str.length; p++) {
    if (str[p] === '%') {
      const pr = str.substr(p + 1, 2);
      if (pr.length === 2 && TOKS[pr]) { if (lit) { out.push({lit}); lit = ''; } out.push({t: pr}); p += 2; continue; }
    }
    lit += str[p];
  }
  if (lit) out.push({lit});
  return out;
}
const serialTmpl = toks => toks.map(x => x.t ? '%' + x.t : x.lit).join('');

function commitLine(i) {
  const s = serialTmpl(model[i]);
  $(`[name="osd.lines.${i}"]`).value = s;
  $('#raw-' + i).value = s;
  setDirty(true);
  renderPreview();
}
function moveChip(i, j, d) {
  const a = model[i], n = j + d;
  if (n < 0 || n >= a.length) return;
  const t = a[j]; a[j] = a[n]; a[n] = t;
  renderChips(i); commitLine(i);
}
function renderChips(i) {
  const w = $('#chips-' + i); w.textContent = '';
  if (!model[i].length) { const e = el('span', 'empty'); e.textContent = '(empty line)'; w.appendChild(e); }
  model[i].forEach((x, j) => {
    const c = el('span', 'chip' + (x.t ? '' : ' lit'));
    if (x.t) {
      const b = el('b'); b.textContent = TOKS[x.t]; const s = el('i'); s.textContent = '%' + x.t;
      c.appendChild(b); c.appendChild(s);
    } else {
      const inp = el('input'); inp.value = x.lit; inp.size = Math.max(2, x.lit.length);
      inp.oninput = () => { model[i][j].lit = inp.value; inp.size = Math.max(2, inp.value.length); commitLine(i); };
      c.appendChild(inp);
    }
    const mk = (txt, fn) => { const b = el('button', 'x'); b.type = 'button'; b.textContent = txt;
      b.onclick = ev => { ev.stopPropagation(); fn(); }; return b; };
    c.appendChild(mk('◀', () => moveChip(i, j, -1)));
    c.appendChild(mk('▶', () => moveChip(i, j, 1)));
    c.appendChild(mk('✕', () => { model[i].splice(j, 1); renderChips(i); commitLine(i); }));
    w.appendChild(c);
  });
}
function setFocusLine(i) {
  focusLine = i;
  $$('.oline').forEach(o => o.classList.toggle('foc', +o.dataset.i === i));
  $('#palhint').textContent = 'Tap a chip to add it to Line ' + (i + 1);
}
(function buildPalette() {
  const pal = $('#palette');
  TOKLIST.forEach(pair => {
    const b = el('button', 'pchip'); b.type = 'button';
    const s = el('span'); s.textContent = pair[1]; const it = el('i'); it.textContent = '%' + pair[0];
    b.appendChild(s); b.appendChild(it);
    b.onclick = () => { model[focusLine].push({t: pair[0]}); renderChips(focusLine); commitLine(focusLine); };
    pal.appendChild(b);
  });
  const tb = el('button', 'pchip'); tb.type = 'button'; tb.textContent = '＋ text';
  tb.onclick = () => {
    model[focusLine].push({lit: ' '}); renderChips(focusLine); commitLine(focusLine);
    const ins = $('#chips-' + focusLine).querySelectorAll('.chip.lit input');
    if (ins.length) ins[ins.length - 1].focus();
  };
  pal.appendChild(tb);
})();
$$('.oline').forEach(o => o.addEventListener('click', () => setFocusLine(+o.dataset.i)));
$$('.rawtog').forEach(b => b.onclick = e => {
  e.stopPropagation();
  const r = b.closest('.oline').querySelector('.rawrow'); r.hidden = !r.hidden;
});
$$('#olines .rawrow input').forEach(inp => {
  const i = +inp.closest('.oline').dataset.i;
  inp.oninput = () => {
    model[i] = parseTmpl(inp.value);
    $(`[name="osd.lines.${i}"]`).value = inp.value;
    renderChips(i); setDirty(true); renderPreview();
  };
});
$('#osdsim').onclick = renderPreview;

function osdLoad() {
  for (let i = 0; i < 4; i++) {
    const v = $(`[name="osd.lines.${i}"]`).value || '';
    model[i] = parseTmpl(v); $('#raw-' + i).value = v; renderChips(i);
  }
  setFocusLine(0);
  renderPreview();
}

// ---- faithful token expander (mirrors components/osd/osd_format.c) ----
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
function tokVal(pair, d) {
  switch (pair) {
    case 'st': if (!d.link) return ''; if (d.pre_record) return 'PRE';
      if (d.recording === 'true') return 'REC'; if (d.recording === 'false') return 'IDLE'; return '';
    case 'et': return d.rec_elapsed.valid ? mmss(d.rec_elapsed.v) : '';
    case 'rt': return d.rec_remaining.valid ? remainStr(d.rec_remaining.v) : '';
    case 'bt': return d.battery.valid ? d.battery.v + '%' : '';
    case 'bb': { if (!d.battery.valid) return ''; let b = Math.floor((d.battery.v + 12) / 25);
      if (b > 4) b = 4; return '█'.repeat(b) + '░'.repeat(4 - b); }
    case 'rs': return d.resolution.valid ? d.resolution.v : '';
    case 'fp': return d.fps.valid ? d.fps.v : '';
    case 'ln': return d.lens.valid ? d.lens.v : '';
    case 'sd': if (d.sd_missing) return 'NO SD'; if (d.sd_full) return 'FULL';
      if (d.rec_remaining.valid) return remainStr(d.rec_remaining.v);
      if (d.sd_remaining_kb.valid) return Math.floor(d.sd_remaining_kb.v / (1024 * 1024)) + 'G'; return '';
    case 'md': return d.mode.valid ? d.mode.v : '';
    case 'cn': return d.craft_name || '';
    case 'wr': { const w = activeWarning(d); return (w && !(d.warn_blink && d.blinkOn)) ? w : ''; }
    case 'ar': return d.aspect.valid ? d.aspect.v : '';
    case 'pr': return d.profile.valid ? d.profile.v : '';
    case 'bd': return d.bit_depth.valid ? d.bit_depth.v + 'b' : '';
    case 'cr': return d.link ? String(d.rssi) : '';
    case 'gs': return d.gps_present ? String(d.gps_sats) : '';
    case 'gf': return d.gps_present ? (d.gps_fix >= 3 ? '3D' : d.gps_fix === 2 ? '2D' : 'NO') : '';
    case 'fb': return ['OK', 'LOW', 'CRIT', ''][d.fc_batt_state & 3];
    case 'sp': if (!d.gps_present || d.gps_fix < 2) return '';
      return d.units ? Math.floor((d.gps_spd * 224 + 5000) / 10000) + 'MPH'
                     : Math.floor((d.gps_spd * 36 + 500) / 1000) + 'KMH';
    default: return null;
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
  return d;
}
function sampleData() {
  return withForm({
    link: true, searching: false, pre_record: false, recording: 'true',
    rec_elapsed: {v: 83, valid: true}, rec_remaining: {v: 2700, valid: true},
    battery: {v: 78, valid: true},
    resolution: {v: '5.3K', valid: true}, fps: {v: '30', valid: true}, aspect: {v: '16:9', valid: true},
    lens: {v: 'Wide', valid: true}, profile: {v: 'HDR', valid: true}, bit_depth: {v: 10, valid: true},
    mode: {v: 'Video', valid: true},
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
  while (box.children.length < 4) box.appendChild(el('div', 'osdline'));

  const connected = !!(lastStatus && lastStatus.camera && lastStatus.camera.link === 'READY');
  const sim = $('#osdsim');
  sim.disabled = connected;
  sim.parentNode.classList.toggle('muted', connected);

  let d, srcNote;
  if (connected) {
    d = liveData(lastStatus);
    srcNote = 'Live data from the camera. Lens, craft name, FC battery and speed are not in the status feed yet — shown from sample data.';
  } else {
    d = sampleData();
    if (!sim.checked) { d.link = false; d.searching = false; }
    srcNote = sim.checked
      ? 'Sample data — camera not connected (simulating a connected camera).'
      : 'Sample data — camera not connected. On the real OSD only “CAM LOST” shows until the link is ready.';
  }

  let lines;
  if (!d.link) { lines = [d.searching ? 'CAM ?' : 'CAM LOST', '', '', '']; }
  else { lines = [0, 1, 2, 3].map(i => expandTmpl($(`[name="osd.lines.${i}"]`).value || '', d)); }

  const clipped = [];
  lines = lines.map((t, i) => { if (t.length > 16) { clipped.push({n: i + 1, full: t}); return t.slice(0, 16); } return t; });

  for (let i = 0; i < 4; i++) {
    const txt = lines[i] || "";
    if (box.children[i].textContent !== txt) box.children[i].textContent = txt;
  }

  const clipEl = $('#osdclip');
  clipEl.textContent = clipped.length
    ? clipped.map(c => 'Line ' + c.n + ' is ' + c.full.length + ' chars — clipped to “' + c.full.slice(0, 16) + '”').join('   ')
    : '';
  $('#osdnote').textContent = srcNote
    + (d.link && d.warn_blink && activeWarning(d) ? '  The warning blinks on the real OSD.' : '');
}
['input', 'change'].forEach(ev => document.addEventListener(ev, e => {
  if (e.target.closest && e.target.closest('#osd')) renderPreview();
}));

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

function collectConfig() {
  const c = { record:{switch:{}}, preset:{bands:[{},{},{}]}, osd:{lines:[],warn:{}},
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
  $$('input[name],select[name]').forEach(el => {
    let name = el.name, val = getField(name);
    if (name.startsWith('osd.lines.')) { c.osd.lines[+name.split('.')[2]] = val; return; }
    set(name, val);
  });
  c.msp.baud = Number(c.msp.baud);
  return c;
}

let cfgBrand = '';            // paired camera brand, drives field gating
let rebootBaseline = '';      // msp.baud + swap_pins at last load, to detect reboot-needed

async function loadConfig() {
  const c = await fetch('/api/config').then(r => r.json());
  fillConfig(c);
  osdLoad();
  cfgBrand = (c.camera && c.camera.brand) || '';
  rebootBaseline = getField('msp.baud') + '/' + getField('msp.swap_pins');
  applyGates();
  setDirty(false);
}

// ---- field gating (state the control doesn't apply to) ----
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
  const recSw = mode === 2 || mode === 3;
  gate($('#rec-switch'), recSw, 'only used with a switch trigger');
  setBarEnabled('record', recSw);

  const pOn = getField('preset.enabled');
  gate($('#preset-setup'), pOn, 'turn on “Enable zone control” first');
  setBarEnabled('preset', pOn);

  const isGP = cfgBrand === 'gopro', isDJI = cfgBrand === 'dji';
  gate($('#lbl-check'),  isGP  || !cfgBrand, 'GoPro only');
  gate($('#lbl-resync'), isDJI || !cfgBrand, 'DJI only');
  const gp = !!isDJI;
  $$('[name^="gps_push."]').forEach(i => i.disabled = !gp);

  layoutAllBars();
  if (lastStatus) updateCursors(lastStatus.msp);
}
$$('[name="record.mode"],[name="preset.enabled"]').forEach(el =>
  el.addEventListener('change', applyGates));

// ---- dirty tracking ----
function setDirty(d) { document.body.classList.toggle('dirty', d); }
document.addEventListener('input', e => { if (e.target.name) setDirty(true); });
window.addEventListener('beforeunload', e => {
  if (document.body.classList.contains('dirty')) { e.preventDefault(); e.returnValue = ''; }
});

$('#save').onclick = async () => {
  const pw = getField('wifi.password');
  if (pw && pw.length > 0 && pw.length < 8) return msg('Wi-Fi password must be at least 8 characters (or blank to keep).');
  msg('saving…');
  const r = await fetch('/api/config', {method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify(collectConfig())}).then(r => r.json()).catch(() => ({ok:false}));
  if (!r.ok) { msg('save failed: ' + (r.errors ? r.errors.join(', ') : 'server rejected it')); return; }
  const changedReboot = (getField('msp.baud') + '/' + getField('msp.swap_pins')) !== rebootBaseline;
  await loadConfig();
  if (changedReboot) { msg('Saved. Baud / pin-swap changes need a reboot to take effect.'); $('#rebootnow').hidden = false; }
  else { msg('Saved.'); }
};
$('#rebootnow').onclick = () => { if (confirm('Reboot the bridge now?')) post('/api/reboot'); };

// ---- password reveal ----
$$('.reveal').forEach(b => b.onclick = () => {
  const el = document.querySelector(`[name="${CSS.escape(b.dataset.for)}"]`);
  const show = el.type === 'password';
  el.type = show ? 'text' : 'password';
  b.textContent = show ? 'hide' : 'show';
});

// ---- actions ----
const post = (u, body) => fetch(u, body ? {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}
                                        : {method:'POST'}).then(r => r.json()).catch(()=>({ok:false}));

$$('[data-pair]').forEach(b => b.onclick = async () => {
  const r = await post('/api/camera/pair', {brand:b.dataset.pair});
  msg(r.ok ? ('pairing… put the ' + b.dataset.pair.toUpperCase() + ' into pairing mode now') : 'pair request failed');
});
$('#camrefresh').onclick = async () => msg((await post('/api/camera/refresh')).ok ? 'refreshing camera…' : 'failed');
$$('[data-preset]').forEach(b => b.onclick = async () => {
  const r = await post('/api/camera/preset', {slot: +b.dataset.preset});
  msg(r.ok ? ('applied zone ' + (+b.dataset.preset + 1)) : ('failed: ' + (r.error || '')));
});
$('#camtest').onclick = async () => msg((await post('/api/camera/test')).ok ? 'recording 3 s…' : 'failed');
$('#camforget').onclick = async () => {
  if (!confirm('Forget the paired camera? The pairing is erased and the camera must be re-paired manually.')) return;
  msg((await post('/api/camera/forget')).ok ? 'camera forgotten' : 'failed');
};
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

$('#otago').onclick = () => {
  const f = $('#otafile').files[0];
  if (!f) return msg('choose a .bin file first');
  if (f.size < 500000) return msg('that file looks too small — use build/fc_cam_bridge.bin');
  if (!confirm('Flash ' + (f.size/1024|0) + ' KB now? The bridge reboots when done.')) return;
  msg('uploading 0 %…');
  const x = new XMLHttpRequest();
  x.open('POST', '/api/ota');
  let last = 0;
  x.upload.onprogress = e => {
    if (!e.lengthComputable) return;
    const p = Math.round(100 * e.loaded / e.total);
    if (p !== last) { last = p; msg('uploading ' + p + ' %…'); }
  };
  x.onload = () => {
    let r = {}; try { r = JSON.parse(x.responseText); } catch (_) {}
    msg(r.ok ? 'flashed OK — rebooting, reconnect to Wi-Fi in ~10 s'
             : 'OTA failed: ' + (r.error || ('HTTP ' + x.status)) + ' — current firmware kept');
  };
  x.onerror = () => msg('OTA upload error (connection dropped) — current firmware kept');
  x.send(f);
};

// ---- live status ----
const P = [['armed','Aircraft'],['camlink','Camera link'],['rec','Recording'],['warn','Warnings']];
const D = [['fc','Flight controller'],['canvas','OSD grid (cols×rows)'],['gps','GPS'],['sig','Camera signal'],
           ['tleft','Time left'],['batt','Camera battery'],['sd','SD card'],['mode','Mode'],
           ['res','Resolution'],['prof','Profile']];
function buildGrid(el, list) {
  el.innerHTML = list.map(r =>
    `<div class="k">${r[1]}</div><div class="v" id="s-${r[0]}">—</div>`).join('');
}
buildGrid($('#stat-primary'), P);
buildGrid($('#stat-detail'), D);
function put(id, text, cls) {
  const el = $('#s-' + id); if (!el) return;
  if (el.textContent !== text) el.textContent = text;
  el.className = 'v' + (cls ? ' ' + cls : '');
}
function fmtT(s) { s = s|0; return s >= 3600 ? (s/3600|0)+'h'+((s%3600/60)|0)+'m'
                                             : (s/60|0)+':'+String(s%60).padStart(2,'0'); }
function fmtUp(s) { s = s|0; const d = s/86400|0, h = (s%86400)/3600|0, m = (s%3600)/60|0;
  return d ? d+'d '+h+'h' : h ? h+'h '+m+'m' : m+'m'; }

let lastFlags = '', lastCounters = '', lastDbg = '';
function setOffline(off) {
  document.body.classList.toggle('offline', off);
  $('#conn').hidden = !off;
  if (off) { $('#fw').textContent = 'offline'; updateCursors(null); }
}

async function poll() {
  let s;
  try {
    s = await fetch('/api/status').then(r => { if (!r.ok) throw 0; return r.json(); });
  } catch (e) { setOffline(true); return; }
  setOffline(false);
  lastStatus = s;

  $('#fw').textContent = 'fw ' + s.fw + ' · up ' + fmtUp(s.uptime_s);
  const m = s.msp, c = s.camera, cs = s.cam_state;
  const fcUp = m && m.connected;
  const paired = !!(c && c.brand);

  // primary
  if (!fcUp) put('armed', 'FC offline', 'warn');
  else put('armed', m.armed ? 'ARMED' : 'disarmed', m.armed ? 'bad' : 'ok');

  put('camlink', paired ? (c.brand.toUpperCase() + ' · ' + c.link + (c.name ? ' · ' + c.name : ''))
                        : 'no camera paired', paired && c.link === 'READY' ? 'ok'
                        : paired && c.link === 'LOST' ? 'bad' : '');

  const recTxt = !paired ? '—'
    : cs.recording === 'true' ? 'REC ' + (cs.elapsed_s != null ? fmtT(cs.elapsed_s) : '') + (cs.hot ? ' ⚠ HOT' : '')
    : cs.recording === 'false' ? 'stopped' : 'unknown';
  put('rec', recTxt, cs && cs.recording === 'true' ? (cs.hot ? 'bad' : 'ok') : '');

  const w = [];
  if (paired) {
    if (cs.sd_missing) w.push('no SD card');
    else if (cs.sd_full) w.push('SD full');
    if (cs.hot) w.push('camera hot');
    if (cs.battery_pct.valid && cs.battery_pct.v <= 15) w.push('camera battery ' + cs.battery_pct.v + '%');
  }
  if (fcUp && m.gps.present && !m.gps.good) w.push('GPS no fix');
  put('warn', w.length ? w.join(', ') : 'none', w.length ? 'warn' : 'ok');

  // detail
  put('fc', fcUp ? (m.variant + ' · MSP ' + m.api) : 'offline', fcUp ? '' : 'warn');
  put('canvas', fcUp ? m.canvas : '—');
  put('gps', !fcUp ? '—' : !m.gps.present ? 'no GPS on FC'
      : ('fix ' + m.gps.fix + ' · ' + m.gps.sats + ' sats' + (m.gps.good ? ' · good' : '')),
      fcUp && m.gps.present ? (m.gps.good ? 'ok' : 'warn') : '');
  put('sig', paired && c.link === 'READY' ? (c.rssi + ' dBm') : '—');
  put('tleft', paired && cs.rec_remaining_s.valid ? fmtT(cs.rec_remaining_s.v) : '—');
  put('batt', paired && cs.battery_pct.valid ? cs.battery_pct.v + '%' : '—');
  put('sd', !paired ? '—' : cs.sd_missing ? 'NO CARD' : cs.sd_full ? 'FULL'
      : cs.sd_remaining_kb.valid ? (cs.sd_remaining_kb.v/1024/1024).toFixed(1) + ' GB' : '—',
      paired && (cs.sd_missing || cs.sd_full) ? 'bad' : '');
  put('mode', paired && cs.mode.valid ? cs.mode.v : '—');
  put('res', paired && cs.resolution.valid
      ? (cs.resolution.v + (cs.fps.v || '') + (cs.aspect.valid ? ' ' + cs.aspect.v : '')) : '—');
  put('prof', paired && cs.profile.valid
      ? (cs.profile.v + (cs.bit_depth.valid ? ' ' + cs.bit_depth.v + 'bit' : '')) : '—');

  $('#aux').textContent = (fcUp && m.aux && m.aux.length)
    ? m.aux.map((v, i) => 'ch' + (i + 1) + ':' + v).join('  ') : '—';
  updateCursors(m);

  const fj = JSON.stringify(s.flags || []);
  if (fj !== lastFlags) {
    lastFlags = fj;
    const names = {
      CFG_RESET:'Settings were reset to defaults', REPAIR_NEEDED:'Camera re-pair needed',
      OSD_UNSUPPORTED:'FC too old for custom OSD', FC_OFFLINE:'No FC link',
      CLOCK_SYNC_FAIL:'Clock sync failed', WDT_RECOVERED:'Recovered from a watchdog reset'
    };
    $('#flags').innerHTML = (s.flags || []).map(f =>
      f === 'WDT_RECOVERED'
        ? `<span class="ackflag" title="click to acknowledge">${names[f]} ✕</span>`
        : `<span>${names[f] || f}</span>`).join('');
    const af = $('#flags').querySelector('.ackflag');
    if (af) af.onclick = async () => { await post('/api/flags/ack'); poll(); };
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
    $('#mspdbg').textContent = fcUp ? [
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

  $('#camlive').textContent = paired
    ? (c.brand.toUpperCase() + '  ' + c.link + '  ' + (c.name || '') + '  ' + (c.addr || '') + '  bonded:' + c.bonded)
    : 'no camera paired';
}

async function pollLog() {
  try { $('#log').textContent = await fetch('/api/logs').then(r => r.text()); } catch (e) {}
}

loadConfig().catch(() => msg('could not load config — is the bridge up?'));
poll(); setInterval(poll, 1500);
setInterval(() => { if ($('#system').classList.contains('on')) pollLog(); }, 3000);
