'use strict';
const $ = s => document.querySelector(s);
const $$ = s => [...document.querySelectorAll(s)];
const msg = t => { $('#msg').textContent = t; };

// ---- tabs ----
$$('#tabs button').forEach(b => b.onclick = () => {
  $$('#tabs button').forEach(x => x.classList.remove('on'));
  $$('.tab').forEach(x => x.classList.remove('on'));
  b.classList.add('on');
  $('#' + b.dataset.t).classList.add('on');
});

// ---- preset zones (name= stays preset.bands.N.*) ----
const bandsWrap = $('#bands');
for (let i = 0; i < 3; i++) {
  bandsWrap.insertAdjacentHTML('beforeend',
    `<fieldset class="pz"><legend>Zone ${i + 1}</legend>
     <label>Knob position up to &mdash; &micro;s
       <input type="number" inputmode="numeric" name="preset.bands.${i}.max_us" min="900" max="2100" step="25"></label>
     <label>Preset / mode ID to apply
       <input type="number" inputmode="numeric" name="preset.bands.${i}.id" min="0" step="1"></label></fieldset>`);
}

$('#tokens').innerHTML = [
  ['%st','recording state'],['%et','elapsed time'],['%rt','time left'],['%bt','camera battery %'],
  ['%bb','battery bars'],['%rs','resolution'],['%fp','fps'],['%ar','aspect ratio'],['%ln','lens'],
  ['%pr','profile'],['%bd','bit depth'],['%sd','SD remaining'],['%md','mode'],['%cr','camera RSSI'],
  ['%gs','GPS sats'],['%gf','GPS fix'],['%fb','FC battery'],['%sp','speed'],['%cn','craft name'],
  ['%wr','active warning']
].map(t => `<b>${t[0]}</b> ${t[1]}`).join('<br>') + '<br><span class="muted">plus any literal text</span>';

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
  gate($('#rec-switch'), mode === 2 || mode === 3, 'only used with a switch trigger');

  const pOn = getField('preset.enabled');
  $$('#preset input:not([name="preset.enabled"]),#preset .pz input').forEach(i => i.disabled = !pOn);

  const isGP = cfgBrand === 'gopro', isDJI = cfgBrand === 'dji';
  gate($('#lbl-check'),  isGP  || !cfgBrand, 'GoPro only');
  gate($('#lbl-resync'), isDJI || !cfgBrand, 'DJI only');
  const gp = !!isDJI;
  $$('[name^="gps_push."]').forEach(i => i.disabled = !gp);
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

let lastFlags = '', lastCounters = '';
function setOffline(off) {
  document.body.classList.toggle('offline', off);
  $('#conn').hidden = !off;
  if (off) $('#fw').textContent = 'offline';
}

async function poll() {
  let s;
  try {
    s = await fetch('/api/status').then(r => { if (!r.ok) throw 0; return r.json(); });
  } catch (e) { setOffline(true); return; }
  setOffline(false);

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

  if (s.clock) {
    const ls = s.clock.last_sync;
    $('#clocklive').textContent =
      'FC UTC : ' + (m && m.fc_utc ? new Date(m.fc_utc*1000).toISOString() : '—') + '\n' +
      'Camera : ' + (s.clock.readable ? (s.clock.cam_utc ? new Date(s.clock.cam_utc*1000).toISOString() : '?')
                                      : 'n/a (DJI is write-only)') + '\n' +
      'Delta  : ' + s.clock.delta_s + ' s\n' +
      'Last   : ' + (ls ? ls.age_s + ' s ago · ' + ls.result : 'never');
  }
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
