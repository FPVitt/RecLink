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

// ---- bands (preset) built dynamically ----
const bandsWrap = $('#bands');
for (let i = 0; i < 3; i++) {
  bandsWrap.insertAdjacentHTML('beforeend',
    `<fieldset><legend>Band ${i}</legend>
     <label>Max us <input type="number" name="preset.bands.${i}.max_us"></label>
     <label>Preset/mode id <input type="number" name="preset.bands.${i}.id"></label></fieldset>`);
}
$('#tokens').innerHTML =
  'Tokens: %st rec &nbsp; %et elapsed &nbsp; %rt time-left &nbsp; %bt cam batt% &nbsp; %bb bars &nbsp; ' +
  '%rs res &nbsp; %fp fps &nbsp; %ar aspect &nbsp; %ln lens &nbsp; %pr profile &nbsp; %bd bit-depth &nbsp; ' +
  '%sd SD &nbsp; %md mode &nbsp; %cr cam RSSI &nbsp; %gs GPS sats &nbsp; %gf GPS fix &nbsp; ' +
  '%fb FC batt &nbsp; %sp speed &nbsp; %cn craft &nbsp; %wr warning &nbsp; + literal text';

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
  // <select> and text: coerce a pure-numeric value to a Number so it lands as
  // a JSON number (the server ignores numeric strings).
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

async function loadConfig() {
  const c = await fetch('/api/config').then(r => r.json());
  fillConfig(c);
}

$('#save').onclick = async () => {
  msg('saving...');
  const r = await fetch('/api/config', {method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify(collectConfig())}).then(r => r.json()).catch(() => ({ok:false}));
  msg(r.ok ? 'saved' : ('error: ' + (r.errors ? r.errors.join(', ') : 'failed')));
  if (r.ok) loadConfig();
};

// ---- actions ----
const post = (u, body) => fetch(u, body ? {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}
                                        : {method:'POST'}).then(r => r.json()).catch(()=>({ok:false}));
$$('[data-pair]').forEach(b => b.onclick = async () => { msg(await post('/api/camera/pair',{brand:b.dataset.pair}).then(r=>r.ok?'pairing... put camera in pairing mode':'failed')); });
$('#camrefresh').onclick = async () => msg((await post('/api/camera/refresh')).ok ? 'refreshing camera...' : 'failed');
$$('[data-preset]').forEach(b => b.onclick = async () => {
  const r = await post('/api/camera/preset', {slot: +b.dataset.preset});
  msg(r.ok ? ('applied band ' + b.dataset.preset) : ('failed: ' + (r.error||'')));
});
$('#camtest').onclick = async () => msg((await post('/api/camera/test')).ok ? 'recording 3s' : 'failed');
$('#camforget').onclick = async () => { if (confirm('Forget the paired camera?')) msg((await post('/api/camera/forget')).ok ? 'forgotten' : 'failed'); };
$('#syncnow').onclick = async () => { const r = await post('/api/clock/sync'); msg(r.ok ? 'sync queued' : ('sync: ' + (r.error||'failed'))); };
$('#identify').onclick = () => post('/api/identify');
$('#reboot').onclick = () => { if (confirm('Reboot the bridge?')) post('/api/reboot'); };
$('#factory').onclick = () => { if (confirm('Erase ALL settings and camera pairing?')) fetch('/api/config?factory=1',{method:'POST'}); };
$('#otago').onclick = () => {
  const f = $('#otafile').files[0]; if (!f) return msg('pick a .bin first');
  if (f.size < 500000) return msg('that file looks too small -- use fc_cam_bridge.bin');
  const bar = $('#otabar');
  bar.style.display = 'block'; bar.value = 0;
  msg('uploading ' + (f.size/1024|0) + ' KB...');
  const x = new XMLHttpRequest();
  x.open('POST', '/api/ota');
  x.upload.onprogress = e => { if (e.lengthComputable) { bar.value = 100*e.loaded/e.total; } };
  x.onload = () => {
    bar.value = 100;
    let r = {}; try { r = JSON.parse(x.responseText); } catch(_){}
    if (r.ok) { msg('flashed OK -- rebooting, reconnect to Wi-Fi in ~10 s'); }
    else { msg('OTA failed: ' + (r.error || ('HTTP ' + x.status))); }
  };
  x.onerror = () => msg('OTA upload error (connection dropped)');
  x.send(f);
};

// ---- live status ----
function row(k, v) { return `<div>${k}</div><div>${v}</div>`; }
function fmtT(s) { s = s|0; return s >= 3600 ? (s/3600|0)+'h'+((s%3600/60)|0)+'m'
                                             : (s/60|0)+':'+String(s%60).padStart(2,'0'); }
async function poll() {
  try {
    const s = await fetch('/api/status').then(r => r.json());
    $('#fw').textContent = 'fw ' + s.fw + '  up ' + s.uptime_s + 's';
    const m = s.msp, c = s.camera, cs = s.cam_state;
    $('#statgrid').innerHTML =
      row('FC', m.connected ? `${m.variant} api ${m.api} ${m.armed?'ARMED':'disarmed'}` : 'offline') +
      row('Canvas', m.canvas) +
      row('GPS', `${m.gps.present?'yes':'no'} fix ${m.gps.fix} sats ${m.gps.sats}${m.gps.good?' good':''}`) +
      row('Camera', `${c.brand||'-'} ${c.link} ${c.name||''} ${c.rssi||''}dBm`) +
      row('Recording', cs.recording + (cs.elapsed_s!=null?' '+fmtT(cs.elapsed_s):'')
            + (cs.hot ? '  ⚠ HOT' : '')) +
      row('Time left', cs.rec_remaining_s.valid ? fmtT(cs.rec_remaining_s.v) : '--') +
      row('Battery', cs.battery_pct.valid ? cs.battery_pct.v + '%' : '--') +
      row('SD', cs.sd_remaining_kb.valid ? (cs.sd_remaining_kb.v/1024/1024).toFixed(1)+' GB'
                : (cs.sd_missing ? 'NO CARD' : cs.sd_full ? 'FULL' : '--')) +
      row('Mode', cs.mode.valid ? cs.mode.v : '--') +
      row('Res', cs.resolution.valid
            ? `${cs.resolution.v}${cs.fps.v||''}${cs.aspect.valid?' '+cs.aspect.v:''}`
            : '--') +
      row('Profile', cs.profile.valid
            ? cs.profile.v + (cs.bit_depth.valid ? ' '+cs.bit_depth.v+'bit' : '')
            : '--');
    $('#aux').textContent = (m.aux||[]).map((v,i)=>`ch${i}:${v}`).join('  ');
    $('#flags').innerHTML = (s.flags||[]).map(f =>
      f === 'WDT_RECOVERED' ? `<span class="ackflag" title="click to acknowledge">${f} ✕</span>` : `<span>${f}</span>`
    ).join('');
    const af = $('#flags').querySelector('.ackflag');
    if (af) af.onclick = async () => { await post('/api/flags/ack'); poll(); };
    if (s.clock) {
      const ls = s.clock.last_sync;
      $('#clocklive').textContent =
        `FC UTC : ${new Date(m.fc_utc*1000).toISOString()}\n` +
        `Cam    : ${s.clock.readable ? (s.clock.cam_utc?new Date(s.clock.cam_utc*1000).toISOString():'?') : 'n/a (blind)'}\n` +
        `delta  : ${s.clock.delta_s}s\n` +
        `last   : ${ls ? ls.age_s+'s ago '+ls.result : 'never'}`;
    }
    $('#camlive').textContent = `${c.brand||'unpaired'}  ${c.link}  ${c.name||''}  ${c.addr||''}  bonded:${c.bonded}`;
  } catch (e) { $('#fw').textContent = 'disconnected'; }
}
async function pollLog() {
  try { $('#log').textContent = await fetch('/api/logs').then(r => r.text()); } catch (e) {}
}

loadConfig();
poll(); setInterval(poll, 1500);
setInterval(() => { if ($('#system').classList.contains('on')) pollLog(); }, 3000);
