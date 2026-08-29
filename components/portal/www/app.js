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
$('#tokens').textContent = 'Tokens: %st %et %rt %bt %bb %rs %fp %ln %sd %md %cn %wr';

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
  return el.value;
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
$('#camtest').onclick = async () => msg((await post('/api/camera/test')).ok ? 'recording 3s' : 'failed');
$('#camforget').onclick = async () => { if (confirm('Forget the paired camera?')) msg((await post('/api/camera/forget')).ok ? 'forgotten' : 'failed'); };
$('#syncnow').onclick = async () => { const r = await post('/api/clock/sync'); msg(r.ok ? 'sync queued' : ('sync: ' + (r.error||'failed'))); };
$('#identify').onclick = () => post('/api/identify');
$('#reboot').onclick = () => { if (confirm('Reboot the bridge?')) post('/api/reboot'); };
$('#factory').onclick = () => { if (confirm('Erase ALL settings and camera pairing?')) fetch('/api/config?factory=1',{method:'POST'}); };
$('#otago').onclick = async () => {
  const f = $('#otafile').files[0]; if (!f) return msg('pick a .bin first');
  msg('uploading ' + (f.size/1024|0) + ' KB...');
  const r = await fetch('/api/ota', {method:'POST', body:f}).then(r=>r.json()).catch(()=>({ok:false}));
  msg(r.ok ? 'flashed, rebooting' : 'OTA failed');
};

// ---- live status ----
function row(k, v) { return `<div>${k}</div><div>${v}</div>`; }
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
      row('Recording', cs.recording + (cs.elapsed_s!=null?' '+cs.elapsed_s+'s':'')) +
      row('Battery', cs.battery_pct.valid ? cs.battery_pct.v + '%' : '--') +
      row('SD', cs.sd_remaining_kb.valid ? (cs.sd_remaining_kb.v/1024/1024).toFixed(1)+' GB'
                : (cs.sd_missing ? 'NO CARD' : cs.sd_full ? 'FULL' : '--')) +
      row('Mode', cs.mode.valid ? cs.mode.v : '--') +
      row('Res', cs.resolution.valid
            ? `${cs.resolution.v}${cs.fps.v||''}${cs.aspect.valid?' '+cs.aspect.v:''}`
            : '--');
    $('#aux').textContent = (m.aux||[]).map((v,i)=>`ch${i}:${v}`).join('  ');
    $('#flags').innerHTML = (s.flags||[]).map(f => `<span>${f}</span>`).join('');
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
