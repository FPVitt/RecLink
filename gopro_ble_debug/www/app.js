// app.js -- GOPRO-BLE-DEBUG web UI. The firmware is a byte transport; all Open
// GoPro framing, encoding and decoding lives here (catalog.js + ops.js tables).
'use strict';

const { CH, RSP_OF, PACKETIZED, OPS, PB_RX, PB_FEATURES, CMD_RSP } = window.OPSDEF;
const OGP = window.OGP;

// ---------------------------------------------------------------- utilities
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
function h(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k === 'class') e.className = v;
    else if (k === 'html') e.innerHTML = v;
    else e.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids.flat()) if (k != null && k !== false) e.append(k.nodeType ? k : String(k));
  return e;
}
const hex = (u8, sep = ' ') => [...u8].map(b => b.toString(16).padStart(2, '0')).join(sep);
function unhex(s) {
  const t = (s || '').replace(/0x/gi, '').replace(/[\s:,\-]/g, '');
  if (t.length % 2 || /[^0-9a-f]/i.test(t)) throw new Error('bad hex');
  const out = new Uint8Array(t.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(t.substr(i * 2, 2), 16);
  return out;
}
const hx2 = n => '0x' + n.toString(16).toUpperCase().padStart(2, '0');
const utf8 = new TextDecoder('utf-8', { fatal: false });
const enc8 = new TextEncoder();
const printable = u8 => u8.length > 0 && [...u8].every(b => (b >= 0x20 && b < 0x7f) || b === 0);
function beInt(u8) {
  if (u8.length > 6) { let v = 0n; for (const b of u8) v = (v << 8n) | BigInt(b); return v <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(v) : v.toString(); }
  let v = 0; for (const b of u8) v = v * 256 + b; return v;
}
function beBytes(v, n) {
  let b = BigInt.asUintN(n * 8, BigInt(v)); const out = new Uint8Array(n);
  for (let i = n - 1; i >= 0; i--) { out[i] = Number(b & 0xffn); b >>= 8n; }
  return out;
}
const cat = (...a) => { const n = a.reduce((s, x) => s + x.length, 0), o = new Uint8Array(n); let i = 0; for (const x of a) { o.set(x, i); i += x.length; } return o; };
const ts = ms => { const s = ms / 1000; return s.toFixed(3).padStart(9, ' '); };
function toast(msg, bad) {
  const t = h('div', { class: 'toast' + (bad ? ' bad' : '') }, msg);
  $('#toasts').append(t); setTimeout(() => t.remove(), bad ? 6000 : 3000);
}
async function api(path, body) {
  const opt = body === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) };
  const r = await fetch(path, opt);
  const j = await r.json().catch(() => ({}));
  if (!r.ok || j.ok === false) throw new Error(j.error || r.statusText);
  return j;
}
const tryApi = (path, body, okMsg) => api(path, body).then(j => { if (okMsg) toast(okMsg); return j; }).catch(e => { toast(path + ': ' + e.message, true); throw e; });

// NimBLE error codes -> names (ATT = 0x100 + att err, HCI = 0x200 + hci reason).
const ATT_ERR = { 1: 'invalid handle', 2: 'read not permitted', 3: 'write not permitted', 5: 'insufficient authentication', 6: 'request not supported',
  7: 'invalid offset', 8: 'insufficient authorization', 0x0d: 'invalid attribute value length', 0x0e: 'unlikely error', 0x0f: 'insufficient encryption', 0x11: 'insufficient resources' };
const HCI_ERR = { 0x08: 'supervision timeout', 0x13: 'remote user terminated', 0x14: 'remote low resources', 0x15: 'remote power off', 0x16: 'terminated by local host',
  0x3b: 'unacceptable conn params', 0x3d: 'MIC failure', 0x3e: 'failed to establish', 0x06: 'PIN/key missing', 0x05: 'authentication failure' };
function errName(st) {
  if (!st) return 'ok';
  if (st >= 0x100 && st < 0x200) return `ATT ${hx2(st - 0x100)} ${ATT_ERR[st - 0x100] || ''}`.trim();
  if (st >= 0x200 && st < 0x300) return `HCI ${hx2(st - 0x200)} ${HCI_ERR[st - 0x200] || ''}`.trim();
  if (st >= 0x300 && st < 0x400) return `SM ${hx2(st - 0x300)}`;
  const HS = { 13: 'timeout', 7: 'not connected', 14: 'done', 6: 'no memory', 2: 'already', 3: 'invalid args' };
  return `host ${st} ${HS[st] || ''}`.trim();
}

// ---------------------------------------------------------------- GATT map
let GATT = { chars: [], services: [] };
const byHandle = new Map();
function shortUuid(u) {
  const m = /^b5f9([0-9a-f]{4})-aa8d-11e3-9046-0002a5d5c51b$/i.exec(u);
  if (m) return m[1].toLowerCase();
  const s = /^0x([0-9a-f]{4})$/i.exec(u);
  if (s) return s[1].toLowerCase();
  return u;
}
const STD = { '1800': 'GAP', '1801': 'GATT', '180a': 'Device Info', '180f': 'Battery', 'fea6': 'GoPro Control & Query', '0001': 'GoPro WiFi AP', '0090': 'GoPro Camera Mgmt',
  '0080': 'GoPro Internal', '2a00': 'Device Name', '2a01': 'Appearance', '2a04': 'Conn Params', '2a05': 'Service Changed', '2a19': 'Battery Level',
  '2a24': 'Model Number', '2a25': 'Serial Number', '2a26': 'Firmware Rev', '2a27': 'Hardware Rev', '2a28': 'Software Rev', '2a29': 'Manufacturer', '2a50': 'PnP ID' };
const uuidName = s => CH[s] || STD[s] || '';
async function loadGatt() {
  GATT = await api('/api/gatt');
  byHandle.clear();
  for (const c of GATT.chars) { c.short = shortUuid(c.uuid); byHandle.set(c.handle, c); }
  renderGatt();
}
const handleOf = short => { for (const c of GATT.chars) if (c.short === short) return c.handle; return 0; };
const charLabel = hdl => { const c = byHandle.get(hdl); return c ? (uuidName(c.short) || c.short) : 'h' + hdl; };

// ---------------------------------------------------------------- protobuf
const PB = {
  schema: n => OGP.PROTO[n.replace(/^WSDK_/, '')],
  isEnum: t => !!OGP.ENUM[t],
  varint(v) {
    let b = BigInt.asUintN(64, BigInt(v)); const out = [];
    do { let x = Number(b & 0x7fn); b >>= 7n; if (b) x |= 0x80; out.push(x); } while (b);
    return Uint8Array.from(out);
  },
  key: (no, wt) => PB.varint((no << 3) | wt),
  encScalar(t, v) {
    if (t === 'string') { const s = enc8.encode(String(v)); return [2, cat(PB.varint(s.length), s)]; }
    if (t === 'bytes') { const s = typeof v === 'string' ? unhex(v) : Uint8Array.from(v); return [2, cat(PB.varint(s.length), s)]; }
    if (t === 'bool') return [0, PB.varint(v === true || v === 'true' || v === 1 || v === '1' ? 1 : 0)];
    if (PB.isEnum(t)) {
      if (typeof v === 'string' && !/^-?\d+$/.test(v)) {
        const e = Object.entries(OGP.ENUM[t]).find(([, n]) => n === v);
        if (!e) throw new Error(`${t}: unknown value ${v}`);
        v = +e[0];
      }
      return [0, PB.varint(+v)];
    }
    if (/^(u?int|sint)(32|64)$/.test(t)) {
      let n = BigInt(v);
      if (t.startsWith('sint')) n = n >= 0n ? n << 1n : ((-n) << 1n) - 1n;
      return [0, PB.varint(n)];
    }
    if (OGP.PROTO[t]) { const m = PB.encode(t, v); return [2, cat(PB.varint(m.length), m)]; }
    throw new Error('unsupported type ' + t);
  },
  encode(name, obj) {
    const sch = PB.schema(name); if (!sch) throw new Error('no schema for ' + name);
    const parts = [];
    for (const [no, fname, t, lab] of sch) {
      let v = obj[fname];
      if (v === undefined || v === null) { if (lab === 'q') throw new Error(`${name}.${fname} is required`); continue; }
      for (const x of lab === 'r' ? [].concat(v) : [v]) { const [wt, body] = PB.encScalar(t, x); parts.push(PB.key(no, wt), body); }
    }
    return cat(...parts);
  },
  readVarint(u8, i) {
    let v = 0n, s = 0n;
    for (;;) { if (i >= u8.length) throw new Error('truncated varint'); const b = u8[i++]; v |= BigInt(b & 0x7f) << s; s += 7n; if (!(b & 0x80)) break; }
    return [v, i];
  },
  wire(u8) {
    const f = []; let i = 0;
    while (i < u8.length) {
      let k; [k, i] = PB.readVarint(u8, i);
      const no = Number(k >> 3n), wt = Number(k & 7n);
      if (wt === 0) { let v; [v, i] = PB.readVarint(u8, i); f.push({ no, wt, v }); }
      else if (wt === 2) { let n; [n, i] = PB.readVarint(u8, i); n = Number(n); if (i + n > u8.length) throw new Error('truncated field'); f.push({ no, wt, b: u8.slice(i, i + n) }); i += n; }
      else if (wt === 5 || wt === 1) {
        const n = wt === 5 ? 4 : 8;
        if (i + n > u8.length) throw new Error('truncated fixed field');
        f.push({ no, wt, b: u8.slice(i, i + n) }); i += n;
      }
      else throw new Error('bad wire type ' + wt);
    }
    return f;
  },
  num(v, t) {
    if (t === 'int32') v = BigInt.asIntN(32, v);
    else if (t === 'int64') v = BigInt.asIntN(64, v);
    else if (t && t.startsWith('sint')) v = (v >> 1n) ^ -(v & 1n);
    return v <= BigInt(Number.MAX_SAFE_INTEGER) && v >= BigInt(Number.MIN_SAFE_INTEGER) ? Number(v) : v.toString();
  },
  decode(name, u8) {
    const sch = PB.schema(name); if (!sch) return PB.generic(u8);
    const out = {};
    for (const fld of PB.wire(u8)) {
      const d = sch.find(x => x[0] === fld.no);
      if (!d) { (out['#' + fld.no] ??= []).push(fld.wt === 0 ? PB.num(fld.v) : hex(fld.b, '')); continue; }
      const [, fname, t, lab] = d;
      let vals;
      if (fld.wt === 0) vals = [fld.v];
      else if (fld.wt === 2 && (t === 'bool' || PB.isEnum(t) || /int/.test(t))) {   // packed repeated
        vals = []; let i = 0; while (i < fld.b.length) { let v; [v, i] = PB.readVarint(fld.b, i); vals.push(v); }
      } else vals = [fld.b];
      for (const raw of vals) {
        let v;
        if (t === 'string') v = utf8.decode(raw);
        else if (t === 'bytes') v = hex(raw, '');
        else if (t === 'bool') v = raw !== 0n;
        else if (PB.isEnum(t)) v = OGP.ENUM[t][String(PB.num(raw))] || PB.num(raw);
        else if (OGP.PROTO[t]) v = PB.decode(t, raw);
        else v = PB.num(raw, t);
        if (lab === 'r') (out[fname] ??= []).push(v); else out[fname] = v;
      }
    }
    return out;
  },
  generic(u8, depth = 0) {
    const out = {};
    for (const f of PB.wire(u8)) {
      let v;
      if (f.wt === 0) v = PB.num(f.v);
      else if (f.wt === 2) {
        // Without a schema, a length-delimited field is a string, a nested
        // message or raw bytes; text is by far the most common in GoPro messages.
        v = null;
        if (printable(f.b)) v = utf8.decode(f.b);
        else if (depth < 4 && f.b.length) { try { const sub = PB.generic(f.b, depth + 1); if (Object.keys(sub).length) v = sub; } catch (e) { /* not a message */ } }
        if (v === null) v = hex(f.b, '');
      } else v = hex(f.b, '');
      (out[f.no] ??= []).push(v);
    }
    for (const k in out) if (out[k].length === 1) out[k] = out[k][0];
    return out;
  },
  template(name, depth = 0) {
    const sch = PB.schema(name); if (!sch) return {};
    const o = {};
    for (const [, fname, t, lab] of sch) {
      let v;
      if (t === 'string') v = ''; else if (t === 'bytes') v = ''; else if (t === 'bool') v = false;
      else if (PB.isEnum(t)) v = Object.values(OGP.ENUM[t])[0];
      else if (OGP.PROTO[t]) v = depth < 3 ? PB.template(t, depth + 1) : {};
      else v = 0;
      o[fname] = lab === 'r' ? [v] : v;
    }
    return o;
  },
  // "1:varint=5\n2:string=abc\n3:hex=0a0b\n4:bool=1\n5:sint=-3" -> bytes
  fromLines(txt) {
    const parts = [];
    for (const line of txt.split('\n').map(s => s.trim()).filter(Boolean)) {
      const m = /^(\d+)\s*:\s*(varint|int|sint|bool|string|str|hex|bytes|fixed32|fixed64)\s*=\s*(.*)$/i.exec(line);
      if (!m) throw new Error('bad field line: ' + line);
      const no = +m[1], t = m[2].toLowerCase(), v = m[3];
      if (t === 'varint' || t === 'int') parts.push(PB.key(no, 0), PB.varint(BigInt(v)));
      else if (t === 'sint') { const n = BigInt(v); parts.push(PB.key(no, 0), PB.varint(n >= 0n ? n << 1n : ((-n) << 1n) - 1n)); }
      else if (t === 'bool') parts.push(PB.key(no, 0), PB.varint(/^(1|true)$/i.test(v) ? 1 : 0));
      else if (t === 'string' || t === 'str') { const s = enc8.encode(v); parts.push(PB.key(no, 2), PB.varint(s.length), s); }
      else if (t === 'hex' || t === 'bytes') { const s = unhex(v); parts.push(PB.key(no, 2), PB.varint(s.length), s); }
      else if (t === 'fixed32') parts.push(PB.key(no, 5), beBytes(v, 4).reverse());
      else if (t === 'fixed64') parts.push(PB.key(no, 1), beBytes(v, 8).reverse());
    }
    return cat(...parts);
  },
};

// ---------------------------------------------------------------- decoders
const CMD_STATUS = { 0: 'Success', 1: 'Error', 2: 'Invalid Parameter' };
const QID = {
  0x12: ['Get Setting Values', 'setting', 1], 0x13: ['Get Status Values', 'status', 1], 0x15: ['Get Setting Values (2B)', 'setting', 2], 0x16: ['Get Status Values (2B)', 'status', 2],
  0x32: ['Get Setting Capabilities', 'cap', 1], 0x52: ['Register Setting Updates', 'setting', 1], 0x53: ['Register Status Updates', 'status', 1],
  0x55: ['Register Setting Updates (2B)', 'setting', 2], 0x56: ['Register Status Updates (2B)', 'status', 2], 0x62: ['Register Capability Updates', 'cap', 1],
  0x72: ['Unregister Setting Updates', 'setting', 1], 0x73: ['Unregister Status Updates', 'status', 1], 0x75: ['Unregister Setting Updates (2B)', 'setting', 2],
  0x76: ['Unregister Status Updates (2B)', 'status', 2], 0x82: ['Unregister Capability Updates', 'cap', 1],
  0x92: ['Setting Value Push', 'setting', 1], 0x93: ['Status Value Push', 'status', 1], 0x95: ['Setting Value Push (2B)', 'setting', 2], 0x96: ['Status Value Push (2B)', 'status', 2],
  0xA2: ['Setting Capability Push', 'cap', 1],
};
const CMD_NAME = {};
for (const o of OPS) if (o.k === 'cmd' && !(o.id in CMD_NAME)) CMD_NAME[o.id] = o.n;
CMD_NAME[0x03] = 'Set Submode / Pairing Complete (legacy)';
const STRING_STATUS = new Set([29, 30, 122]);

const STATE = { status: {}, setting: {}, cap: {}, hw: null };
function fmtStatus(id, raw) {
  const d = OGP.STATUS[id];
  if (STRING_STATUS.has(id) || (raw.length > 4 && printable(raw))) return JSON.stringify(utf8.decode(raw).replace(/\0+$/, ''));
  if (raw.length === 1 && raw[0] === 0xff && d && d.o && d.o['-1']) return d.o['-1'] + ' (-1)';
  const v = beInt(raw);
  return d && d.o && d.o[v] !== undefined ? `${d.o[v]} (${v})` : String(v);
}
function fmtSetting(id, raw) { return fmtSettingVal(id, beInt(raw)); }
function fmtSettingVal(id, v) {
  const d = OGP.SETTING[id];
  if (d && d.o && d.o[v] !== undefined) return `${Array.isArray(d.o[v]) ? d.o[v][0] : d.o[v]} (${v})`;
  return String(v);
}
function walkTlv(body, w) {
  const out = []; let i = 0;
  while (i + w + 1 <= body.length) {
    const id = w === 2 ? (body[i] << 8) | body[i + 1] : body[i]; i += w;
    const n = body[i++]; if (i + n > body.length) { out.push({ id, err: 'truncated' }); break; }
    out.push({ id, raw: body.slice(i, i + n) }); i += n;
  }
  return out;
}
function decodeLV(body, names, numeric) {
  const o = {}; let i = 0, k = 0;
  while (i < body.length && k < names.length) {
    const n = body[i++]; const v = body.slice(i, i + n); i += n;
    const nm = names[k++]; o[nm] = numeric.includes(nm) ? beInt(v) : (printable(v) ? utf8.decode(v) : hex(v, ''));
  }
  if (i < body.length) o._extra = hex(body.slice(i), '');
  return o;
}
function decodeDt(b, len10) {   // [len][yyyy][mm][dd][hh][mi][ss]([off16][dst])
  let i = 0; if (b.length === (len10 ? 11 : 8)) i = 1;     // spec: leading response_length byte
  const y = (b[i] << 8) | b[i + 1], p = n => String(n).padStart(2, '0');
  const o = { date: `${y}-${p(b[i + 2])}-${p(b[i + 3])} ${p(b[i + 4])}:${p(b[i + 5])}:${p(b[i + 6])}` };
  if (len10) { let off = (b[i + 7] << 8) | b[i + 8]; if (off & 0x8000) off -= 0x10000; o.utc_offset_min = off; o.dst = !!b[i + 9]; }
  return o;
}

// Returns { title, obj, kind } for one reassembled message on characteristic `short`.
function decodeMessage(short, m) {
  if (!m.length) return { title: '(empty)', obj: {} };
  // Protobuf first (spec "Decipher Message Payload Type").
  if (m.length >= 2 && PB_FEATURES.has(m[0])) {
    const key = hex([m[0]], '') + ':' + hex([m[1]], '');
    const name = PB_RX[key];
    if (name || short === '0092') {
      let obj; try { obj = name ? PB.decode(name, m.slice(2)) : PB.generic(m.slice(2)); } catch (e) { obj = { _error: e.message, raw: hex(m.slice(2), '') }; }
      return { title: `PB ${hx2(m[0])}/${hx2(m[1])} ${name ? name.replace(/^WSDK_/, '') : '(unknown message)'}`, obj, kind: 'pb' };
    }
  }
  if (short === '0073') {
    const id = m[0], st = m[1], body = m.slice(2), r = CMD_RSP[id];
    let obj = { status: `${CMD_STATUS[st] || 'Reserved'} (${st})` };
    if (r && st === 0) {
      if (r.lv) Object.assign(obj, decodeLV(body, r.lv, r.num || []));
      if (r.dt) Object.assign(obj, decodeDt(body, r.dt === 10));
      if (id === 0x3C) { STATE.hw = obj; renderHw(); }
    } else if (body.length) obj.payload = hex(body);
    return { title: `${CMD_NAME[id] || 'Command ' + hx2(id)} -> ${CMD_STATUS[st] || st}`, obj, kind: st ? 'bad' : 'ok' };
  }
  if (short === '0075') {
    let id, st;
    if (m[0] === 0xFF && m.length >= 4) { id = (m[1] << 8) | m[2]; st = m[3]; } else { id = m[0]; st = m[1]; }
    const nm = id === 0x5B ? 'Keep Alive' : (OGP.SETTING[id] ? 'Set ' + OGP.SETTING[id].n : 'Setting ' + id);
    return { title: `${nm} -> ${CMD_STATUS[st] || st}`, obj: { setting: id, status: `${CMD_STATUS[st] || 'Reserved'} (${st})` }, kind: st ? 'bad' : 'ok', ka: id === 0x5B };
  }
  if (short === '0077') {
    const q = QID[m[0]]; const st = m[1];
    if (!q) return { title: `Query ${hx2(m[0])} -> ${st}`, obj: { raw: hex(m.slice(2)) } };
    const [qname, kind, w] = q;
    const items = walkTlv(m.slice(2), w), obj = { status: `${CMD_STATUS[st] || st} (${st})` };
    const now = Date.now();
    for (const it of items) {
      if (it.err) { obj['#' + it.id] = it.err; continue; }
      if (kind === 'status') { STATE.status[it.id] = { raw: it.raw, t: now }; obj[`${it.id} ${(OGP.STATUS[it.id] || {}).n || '?'}`] = fmtStatus(it.id, it.raw); }
      else if (kind === 'setting') { STATE.setting[it.id] = { raw: it.raw, t: now }; obj[`${it.id} ${(OGP.SETTING[it.id] || {}).n || '?'}`] = fmtSetting(it.id, it.raw); }
      else {
        if (!STATE.cap._fresh) STATE.cap._fresh = {};
        (STATE.cap._fresh[it.id] ??= []).push(beInt(it.raw));
      }
    }
    if (kind === 'cap' && STATE.cap._fresh) {
      for (const [id, vals] of Object.entries(STATE.cap._fresh)) { STATE.cap[id] = vals; obj[`${id} ${(OGP.SETTING[id] || {}).n || '?'}`] = vals.map(v => fmtSettingVal(+id, v)).join(', '); }
      delete STATE.cap._fresh;
    }
    scheduleTables();
    return { title: `${qname} (${items.length} items) -> ${CMD_STATUS[st] || st}`, obj, kind: st ? 'bad' : 'ok' };
  }
  return { title: `${m.length} B`, obj: printable(m) ? { text: utf8.decode(m), hex: hex(m) } : { hex: hex(m) } };
}

// Open GoPro depacketizer, one per handle.
const REASM = new Map();
function depacketize(hdl, pkt) {
  let r = REASM.get(hdl); if (!r) { r = { need: 0, buf: [] }; REASM.set(hdl, r); }
  const b0 = pkt[0];
  let body;
  if (!(b0 & 0x80)) {
    const typ = (b0 >> 5) & 3;
    if (typ === 0) { r.need = b0 & 0x1f; body = pkt.slice(1); }
    else if (typ === 1) { r.need = ((b0 & 0x1f) << 8) | pkt[1]; body = pkt.slice(2); }
    else if (typ === 2) { r.need = (pkt[1] << 8) | pkt[2]; body = pkt.slice(3); }
    else return { err: 'bad header' };
    r.buf = [...body];
  } else {
    if (!r.need) return { err: 'continuation without start' };
    r.buf.push(...pkt.slice(1));
  }
  if (r.buf.length >= r.need) { const msg = Uint8Array.from(r.buf.slice(0, r.need)); r.need = 0; r.buf = []; return { msg }; }
  return { partial: `${r.buf.length}/${r.need}` };
}

// ---------------------------------------------------------------- log
const LOG = [];            // {t, dir, ch, title, obj, hex, cls, raw, ka}
const LOG_MAX = 3000;
let logDirty = false, pendingRsp = null;
const EV = { SYS: 0, TX: 1, RX: 2, IND: 3, READ: 4, ERR: 5 };
function addLog(e) {
  LOG.push(e); if (LOG.length > LOG_MAX) LOG.splice(0, LOG.length - LOG_MAX);
  logDirty = true;
  if (pendingRsp && !e.raw && e.dir === 'rx' && e.chShort === pendingRsp.ch && !e.ka) {
    pendingRsp.cb(e); pendingRsp = null;
  }
}
function onEvent(ev) {
  const t = ev.t;
  if (ev.y === EV.SYS || ev.y === EV.ERR) {
    addLog({ t, dir: 'sys', title: ev.x, cls: ev.y === EV.ERR ? 'bad' : 'sys' });
    if (/^GATT discovered/.test(ev.x)) loadGatt().catch(() => {});
    if (/^(connected|disconnected)/.test(ev.x)) { REASM.clear(); refreshStatus(); }
    if (/^disconnected/.test(ev.x)) { GATT = { chars: [], services: [] }; byHandle.clear(); renderGatt(); }
    return;
  }
  const u8 = ev.d ? unhex(ev.d) : new Uint8Array();
  const c = byHandle.get(ev.h), short = c ? c.short : '';
  const name = charLabel(ev.h), ka = !!(ev.f & 1);
  if (ev.y === EV.TX) {
    addLog({ t, dir: 'tx', ch: name, chShort: short, title: `packet ${u8.length} B` + (ev.st ? ' FAILED: ' + errName(ev.st) : ''), hex: hex(u8), raw: true, ka, cls: ev.st ? 'bad' : '' });
    if (ev.st) addLog({ t, dir: 'tx', ch: name, chShort: short, title: 'write failed: ' + errName(ev.st), cls: 'bad', ka });
    return;
  }
  if (ev.y === EV.READ) {
    const o = ev.st ? { error: errName(ev.st) } : printable(u8) ? { text: utf8.decode(u8).replace(/\0+$/, ''), hex: hex(u8) } : { hex: hex(u8) };
    addLog({ t, dir: 'rx', ch: name, chShort: short, title: 'read ' + (ev.st ? 'FAILED: ' + errName(ev.st) : `${u8.length} B`), obj: o, hex: hex(u8), cls: ev.st ? 'bad' : 'ok' });
    return;
  }
  // RX / IND
  addLog({ t, dir: 'rx', ch: name, chShort: short, title: `${ev.y === EV.IND ? 'indication' : 'notification'} ${u8.length} B`, hex: hex(u8), raw: true });
  if (PACKETIZED.has(short)) {
    const r = depacketize(ev.h, u8);
    if (r.err) { addLog({ t, dir: 'rx', ch: name, chShort: short, title: 'depacketize: ' + r.err, hex: hex(u8), cls: 'bad' }); return; }
    if (!r.msg) return;
    const d = decodeMessage(short, r.msg);
    addLog({ t, dir: 'rx', ch: name, chShort: short, title: d.title, obj: d.obj, hex: hex(r.msg), cls: d.kind === 'bad' ? 'bad' : d.kind === 'ok' ? 'ok' : '', ka: d.ka });
  } else {
    addLog({ t, dir: 'rx', ch: name, chShort: short, title: `${uuidName(short) || 'h' + ev.h}: ${u8.length} B`, obj: printable(u8) ? { text: utf8.decode(u8) } : { value: beInt(u8) }, hex: hex(u8) });
  }
}

function renderLog() {
  if (!logDirty || $('#tab-log').hidden && $('#tab-commands').hidden) return;
  logDirty = false;
  const raw = $('#f-raw').checked, ka = $('#f-ka').checked, q = $('#f-q').value.toLowerCase();
  const rows = LOG.filter(e => (raw || !e.raw) && (ka || !e.ka) && (!q || (e.title + ' ' + (e.ch || '') + ' ' + (e.hex || '')).toLowerCase().includes(q))).slice(-600);
  const box = $('#log'); const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 30;
  box.replaceChildren(...rows.map(logRow));
  if (atBottom || $('#f-follow').checked) box.scrollTop = box.scrollHeight;
  // mini log in Commands tab
  const mini = $('#cmd-log'); mini.replaceChildren(...LOG.filter(e => !e.raw && !e.ka).slice(-40).map(logRow)); mini.scrollTop = mini.scrollHeight;
}
function logRow(e) {
  const arrow = e.dir === 'tx' ? '→' : e.dir === 'rx' ? '←' : '•';
  const det = e.obj || e.hex ? h('details', {}, h('summary', {}, h('span', { class: 'lt' }, ts(e.t)), h('span', { class: 'ld ' + e.dir }, arrow),
      h('span', { class: 'lc' }, e.ch || ''), h('span', { class: 'lm' }, e.title)),
    e.obj ? h('pre', { class: 'lo' }, JSON.stringify(e.obj, null, 1)) : null,
    e.hex ? h('pre', { class: 'lh' }, e.hex) : null)
    : h('div', { class: 'lrow' }, h('span', { class: 'lt' }, ts(e.t)), h('span', { class: 'ld ' + e.dir }, arrow), h('span', { class: 'lc' }, e.ch || ''), h('span', { class: 'lm' }, e.title));
  det.className += ' le ' + (e.cls || '') + (e.raw ? ' raw' : '');
  return det;
}

// ---------------------------------------------------------------- sending
function localTx(ch, title, msg, obj) {
  addLog({ t: performance.now() + clockSkew, dir: 'tx', ch: uuidName(ch) || ch, chShort: ch, title, obj, hex: hex(msg), cls: 'txm' });
}
let clockSkew = 0;   // device ms - performance.now(), so local TX lines sort in
async function sendMsg(ch, msg, title, opts = {}) {
  const hdl = handleOf(ch);
  if (!hdl) throw new Error(`characteristic GP-${ch} not discovered (connect + discover first)`);
  localTx(ch, title, msg, opts.obj);
  if (opts.onRsp && RSP_OF[ch]) pendingRsp = { ch: RSP_OF[ch], cb: opts.onRsp };
  await api('/api/write', { handle: hdl, hex: hex(msg, ''), gopro: opts.raw ? 0 : 1, rsp: opts.noRsp ? 0 : 1 });
}

function encParam(p, v, form) {
  switch (p.t) {
    case 'u8': return Uint8Array.of(+v & 0xff);
    case 'u16': return beBytes(+v, 2);
    case 'u32': return beBytes(+v, 4);
    case 'i16': return beBytes(+v, 2);
    case 'str': case 'rawstr': return enc8.encode(v);
    case 'hex': return unhex(v);
    case 'dt7': case 'dt10': {
      const d = v ? new Date(v) : new Date();
      const b = [...beBytes(d.getFullYear(), 2), d.getMonth() + 1, d.getDate(), d.getHours(), d.getMinutes(), d.getSeconds()];
      if (p.t === 'dt10') b.push(...beBytes(+(form.tz ?? -d.getTimezoneOffset()), 2), form.dst ? 1 : 0);
      return Uint8Array.from(b);
    }
  }
  throw new Error('param type ' + p.t);
}

// Build the logical message for an op from form values -> {ch, msg, obj}
function buildOp(op, vals) {
  const pv = (op.p || []).map((p, i) => encParam(p, vals['p' + i], vals));
  if (op.k === 'cmd' || op.k === 'setk') {
    let params;
    const encm = vals.enc || 'lv';
    if (!pv.length) params = new Uint8Array();
    else if (pv.length === 1 || encm === 'lv') params = cat(...pv.map(x => cat(Uint8Array.of(x.length), x)));
    else if (encm === 'single') { const all = cat(...pv); params = cat(Uint8Array.of(all.length), all); }
    else params = cat(...pv);
    return { ch: op.k === 'cmd' ? '0072' : '0074', msg: cat(Uint8Array.of(op.id), params) };
  }
  if (op.k === 'query') {
    const ids = (vals.ids || '').split(/[\s,]+/).filter(Boolean).map(Number);
    const w = op.w || 1;
    return { ch: '0076', msg: cat(Uint8Array.of(op.id), ...ids.map(i => w === 2 ? beBytes(i, 2) : Uint8Array.of(i))) };
  }
  if (op.k === 'pb') {
    const f = vals.f ?? op.f, a = vals.a ?? op.a;
    let body, obj;
    if (op.pub === false || !op.req || !PB.schema(op.req)) body = PB.fromLines(vals.fields || '');
    else { obj = JSON.parse(vals.json || '{}'); body = PB.encode(op.req, obj); }
    return { ch: op.c, msg: cat(Uint8Array.of(f, a), body), obj };
  }
  if (op.k === 'write') return { ch: op.c, msg: pv[0], raw: true };
  throw new Error('kind ' + op.k);
}

// ---------------------------------------------------------------- Commands tab
let curOp = null;
function renderOpList() {
  const q = $('#op-q').value.toLowerCase(), list = $('#op-list');
  const groups = {};
  OPS.forEach((o, i) => {
    const tag = o.k === 'pb' ? `${hx2(o.f)}/${hx2(o.a)}` : o.id != null ? hx2(o.id) : 'GP-' + o.c;
    if (q && !(o.n + ' ' + o.g + ' ' + tag + ' ' + (o.req || '')).toLowerCase().includes(q)) return;
    (groups[o.g] ??= []).push(h('button', { class: 'op' + (curOp === o ? ' sel' : '') + (o.danger ? ' danger' : ''), onclick: () => selectOp(o) },
      h('span', {}, o.n), h('code', {}, tag)));
  });
  list.replaceChildren(...Object.entries(groups).map(([g, items]) => h('div', { class: 'opg' }, h('h4', {}, g, h('small', {}, ` ${items.length}`)), ...items)));
}
function paramInput(p, i) {
  const id = 'p' + i;
  if (p.t === 'dt7' || p.t === 'dt10') {
    const now = new Date(Date.now() - new Date().getTimezoneOffset() * 60000).toISOString().slice(0, 19);
    return h('div', { class: 'row' }, h('label', {}, p.n, h('input', { type: 'datetime-local', step: 1, 'data-k': id, value: now })),
      p.t === 'dt10' ? [h('label', {}, 'UTC offset (min)', h('input', { type: 'number', 'data-k': 'tz', value: -new Date().getTimezoneOffset() })),
        h('label', { class: 'chk' }, h('input', { type: 'checkbox', 'data-k': 'dst' }), 'DST')] : null,
      h('small', {}, 'empty = now'));
  }
  if (p.o) {
    return h('label', {}, p.n + ` (${p.t})`, h('select', { 'data-k': id }, ...Object.entries(p.o).map(([v, n]) => h('option', { value: v, selected: +v === p.d }, `${n} (${v})`)),
      h('option', { value: '' }, 'custom…')), h('input', { 'data-k': id + '_c', placeholder: 'custom value', class: 'small' }));
  }
  return h('label', {}, p.n + ` (${p.t})`, h('input', { 'data-k': id, value: p.d ?? '' }));
}
function formVals(root) {
  const v = {};
  for (const e of $$('[data-k]', root)) v[e.dataset.k] = e.type === 'checkbox' ? e.checked : e.value;
  for (const k of Object.keys(v)) if (k.endsWith('_c')) { const b = k.slice(0, -2); if (v[b] === '' && v[k] !== '') v[b] = v[k]; delete v[k]; }
  if (v.f !== undefined) v.f = parseInt(v.f, 16); if (v.a !== undefined) v.a = parseInt(v.a, 16);
  return v;
}
function selectOp(op) {
  curOp = op; renderOpList();
  const box = $('#op-form');
  const meta = op.k === 'pb' ? `Protobuf · feature ${hx2(op.f)} action ${hx2(op.a)} · ${CH[op.c]} (GP-${op.c}) · request ${op.req || '—'} → ${op.rsp || '—'}`
    : op.k === 'query' ? `TLV query ${hx2(op.id)} on Query (GP-0076)` : op.k === 'read' ? `GATT read of ${CH[op.c]} (GP-${op.c})`
    : op.k === 'write' ? `GATT write to ${CH[op.c]} (GP-${op.c}), not packetized` : `TLV command ${hx2(op.id)} on ${op.k === 'setk' ? 'Settings (GP-0074)' : 'Command (GP-0072)'}`;
  const kids = [h('h3', {}, op.n, op.danger ? h('span', { class: 'tag bad' }, 'destructive') : null, op.pub === false ? h('span', { class: 'tag' }, 'schema not public') : null),
    h('p', { class: 'meta' }, meta), op.note ? h('p', { class: 'note' }, op.note) : null];
  (op.p || []).forEach((p, i) => kids.push(paramInput(p, i)));
  if (op.multi) kids.push(h('label', {}, 'Encoding', h('select', { 'data-k': 'enc' }, h('option', { value: 'lv' }, 'per-param length (spec TLV)'),
    h('option', { value: 'single' }, 'one length for all (SDK)'), h('option', { value: 'none' }, 'no lengths'))));
  if (op.k === 'query') {
    const tbl = op.ids === 'status' ? OGP.STATUS : OGP.SETTING;
    kids.push(h('label', {}, `Element IDs (${op.ids}, ${op.w || 1}-byte; empty = ALL)`, h('input', { 'data-k': 'ids', placeholder: 'e.g. 2 3 121  (empty = all)' })),
      h('details', { class: 'ids' }, h('summary', {}, `known ${op.ids} IDs`), h('div', {}, ...Object.entries(tbl).map(([id, d]) => h('button', { class: 'chip', onclick: ev => {
        const inp = $('[data-k=ids]', box); inp.value = (inp.value + ' ' + id).trim(); ev.preventDefault(); preview(); } }, `${id} ${d.n}`)))));
  }
  if (op.k === 'pb') {
    kids.push(h('div', { class: 'row' }, h('label', {}, 'Feature ID', h('input', { 'data-k': 'f', value: op.f.toString(16).padStart(2, '0'), class: 'small' })),
      h('label', {}, 'Action ID', h('input', { 'data-k': 'a', value: op.a.toString(16).padStart(2, '0'), class: 'small' }))));
    if (op.pub === false || !PB.schema(op.req || '')) {
      kids.push(h('label', {}, 'Fields (one per line: num:type=value — types varint sint bool string hex fixed32 fixed64; empty = empty message)',
        h('textarea', { 'data-k': 'fields', rows: 4 }, typeof op.def === 'string' ? op.def : '')));
    } else {
      const tpl = Object.assign(PB.template(op.req), op.def || {});
      if (op.def && Object.keys(op.def).length) for (const k of Object.keys(tpl)) if (!(k in op.def)) delete tpl[k];
      kids.push(h('label', {}, `${op.req} (JSON; enums by name or number; bytes as hex)`, h('textarea', { 'data-k': 'json', rows: 6 }, JSON.stringify(tpl, null, 1))),
        schemaHelp(op.req));
    }
  }
  kids.push(h('div', { class: 'preview' }, h('b', {}, 'Message: '), h('code', { id: 'op-prev' }, '')),
    h('div', { class: 'row' }, h('button', { class: 'pri', onclick: () => runOp(op, box) }, op.k === 'read' ? 'Read' : 'Send'),
      h('label', { class: 'chk' }, h('input', { type: 'checkbox', id: 'op-norsp' }), 'write without response')),
    h('div', { id: 'op-rsp', class: 'rsp' }));
  box.replaceChildren(...kids.filter(Boolean));
  box.oninput = preview; preview();
}
function schemaHelp(name, seen = new Set()) {
  const sch = PB.schema(name); if (!sch || seen.has(name)) return null; seen.add(name);
  const lines = [];
  const walk = (n, ind) => { for (const [no, f, t, lab] of PB.schema(n) || []) {
    lines.push(`${ind}${f} = ${no}  ${lab === 'r' ? 'repeated ' : lab === 'q' ? 'required ' : ''}${t}` + (OGP.ENUM[t] ? '  {' + Object.entries(OGP.ENUM[t]).map(([v, s]) => `${s}=${v}`).join(', ') + '}' : ''));
    if (OGP.PROTO[t] && ind.length < 6) walk(t, ind + '  ');
  } };
  walk(name, '');
  return h('details', { class: 'schema' }, h('summary', {}, 'schema'), h('pre', {}, lines.join('\n')));
}
function preview() {
  const out = $('#op-prev'); if (!out || !curOp) return;
  try { const b = buildOp(curOp, formVals($('#op-form'))); out.textContent = curOp.k === 'read' ? `(read GP-${curOp.c})` : `GP-${b.ch}: ${hex(b.msg)}  (${b.msg.length} B${b.raw ? ', raw' : ', packetized'})`; out.className = ''; }
  catch (e) { out.textContent = e.message; out.className = 'bad'; }
}
async function runOp(op, box) {
  if (op.danger && !confirm(`"${op.n}" is destructive on the camera. Send it?`)) return;
  const rspBox = $('#op-rsp'); rspBox.replaceChildren(h('i', {}, 'waiting…'));
  try {
    if (op.k === 'read') {
      const hdl = handleOf(op.c); if (!hdl) throw new Error(`GP-${op.c} not discovered`);
      pendingRsp = { ch: op.c, cb: e => showRsp(rspBox, e) };
      await api('/api/read', { handle: hdl });
      return;
    }
    const b = buildOp(op, formVals(box));
    await sendMsg(b.ch, b.msg, op.n, { obj: b.obj, raw: b.raw, noRsp: $('#op-norsp').checked, onRsp: e => showRsp(rspBox, e) });
    if (!RSP_OF[b.ch]) rspBox.replaceChildren(h('i', {}, 'sent (no response characteristic)'));
  } catch (e) { rspBox.replaceChildren(h('span', { class: 'bad' }, e.message)); }
}
function showRsp(box, e) {
  box.replaceChildren(h('b', { class: e.cls || '' }, e.title), e.obj ? h('pre', {}, JSON.stringify(e.obj, null, 1)) : null, e.hex ? h('pre', { class: 'lh' }, e.hex) : null);
}

// Raw composer
async function rawSend() {
  try {
    const hdl = +$('#raw-h').value; if (!hdl) throw new Error('pick a characteristic');
    const msg = unhex($('#raw-hex').value);
    const c = byHandle.get(hdl);
    localTx(c ? c.short : 'h' + hdl, 'raw ' + ($('#raw-gp').checked ? 'message' : 'write'), msg);
    if (c && RSP_OF[c.short]) pendingRsp = { ch: RSP_OF[c.short], cb: e => showRsp($('#raw-rsp'), e) };
    await api('/api/write', { handle: hdl, hex: hex(msg, ''), gopro: $('#raw-gp').checked ? 1 : 0, rsp: $('#raw-rsp-w').checked ? 1 : 0 });
  } catch (e) { toast(e.message, true); }
}

// ---------------------------------------------------------------- Settings / Statuses tabs
let tablesTimer = 0;
function scheduleTables() { if (!tablesTimer) tablesTimer = setTimeout(() => { tablesTimer = 0; renderSettings(); renderStatuses(); }, 300); }
function camIdx() {
  const n = STATE.hw && STATE.hw.model_name; if (!n) return -1;
  return OGP.CAMS.findIndex(c => c.toLowerCase() === String(n).toLowerCase());
}
function renderSettings() {
  if ($('#tab-settings').hidden) return;
  const ci = camIdx(), only = $('#s-only').checked, q = $('#s-q').value.toLowerCase();
  const rows = [];
  for (const [id, d] of Object.entries(OGP.SETTING).sort((a, b) => a[0] - b[0])) {
    const cur = STATE.setting[id];
    if (only && ci >= 0 && d.c && !d.c.includes(ci)) continue;
    if (q && !(id + ' ' + d.n).toLowerCase().includes(q)) continue;
    const caps = STATE.cap[id];
    const sel = h('select', {}, ...Object.entries(d.o || {}).map(([v, o]) => {
      const nm = Array.isArray(o) ? o[0] : o, cams = Array.isArray(o) ? o[1] : null;
      const ok = (ci < 0 || !cams || cams.includes(ci)) && (!caps || caps.includes(+v));
      return h('option', { value: v, selected: cur && beInt(cur.raw) === +v, class: ok ? '' : 'dim' }, `${nm} (${v})${ok ? '' : ' ✕'}`);
    }), h('option', { value: '' }, 'custom…'));
    const custom = h('input', { class: 'small', placeholder: 'value' });
    rows.push(h('tr', {}, h('td', {}, id), h('td', {}, d.n, h('small', {}, ' ' + (d.f || 'int8ub')), unofficialTag(d)), h('td', { class: cur ? '' : 'dim' }, cur ? fmtSetting(+id, cur.raw) : '—'),
      h('td', {}, sel, custom, h('button', { onclick: () => setSetting(+id, sel.value !== '' ? sel.value : custom.value, d.f) }, 'Set'),
        h('button', { onclick: () => sendMsg('0076', Uint8Array.of(+id < 256 ? 0x12 : 0x15, ...(+id < 256 ? [+id] : beBytes(+id, 2))), 'Get setting ' + id).catch(e => toast(e.message, true)) }, 'Get'))));
  }
  $('#s-model').textContent = ci >= 0 ? `Camera: ${OGP.CAMS[ci]}` : STATE.hw ? `Camera: ${STATE.hw.model_name} (not in spec DB)` : 'Camera model unknown — run Get Hardware Info to filter options';
  $('#settings-body').replaceChildren(...rows);
}
// Settings/statuses absent from the Open GoPro spec, named from a camera's
// gpControl dump: may be rejected or behave differently on other models/firmware.
const unofficialTag = d => d && d.u ? h('span', { class: 'tag warn', title: `Not in the Open GoPro spec. Name from the ${d.src || 'camera'} gpControl settings dump.` }, 'unofficial') : null;
function setSetting(id, v, fmt) {
  if (v === '' || v == null) return toast('pick a value', true);
  const n = fmt === 'int64ub' ? 8 : fmt === 'int32ub' ? 4 : 1;
  const val = beBytes(BigInt(v), n);
  const msg = id < 256 ? cat(Uint8Array.of(id, n), val) : cat(Uint8Array.of(0xFF), beBytes(id, 2), Uint8Array.of(n), val);
  sendMsg('0074', msg, `Set ${(OGP.SETTING[id] || {}).n || id} = ${v}`).catch(e => toast(e.message, true));
}
function renderStatuses() {
  if ($('#tab-statuses').hidden) return;
  const q = $('#st-q').value.toLowerCase(), now = Date.now();
  const ids = new Set([...Object.keys(OGP.STATUS), ...Object.keys(STATE.status)]);
  const rows = [...ids].map(Number).sort((a, b) => a - b).filter(id => !q || (id + ' ' + ((OGP.STATUS[id] || {}).n || '')).toLowerCase().includes(q)).map(id => {
    const cur = STATE.status[id], d = OGP.STATUS[id] || { n: '(not in catalog)' };
    return h('tr', { class: cur && now - cur.t < 1500 ? 'flash' : '' }, h('td', {}, id), h('td', {}, d.n, unofficialTag(d)), h('td', { class: cur ? '' : 'dim' }, cur ? fmtStatus(id, cur.raw) : '—'),
      h('td', { class: 'mono' }, cur ? hex(cur.raw) : ''), h('td', { class: 'dim' }, cur ? Math.round((now - cur.t) / 1000) + ' s ago' : ''));
  });
  $('#status-body').replaceChildren(...rows);
}

// ---------------------------------------------------------------- GATT tab
const PROPS = [[0x02, 'R'], [0x04, 'Wn'], [0x08, 'W'], [0x10, 'N'], [0x20, 'I'], [0x01, 'B'], [0x40, 'S'], [0x80, 'X']];
function renderGatt() {
  const rows = [];
  GATT.services.forEach((s, si) => {
    const ss = shortUuid(s.uuid);
    rows.push(h('tr', { class: 'svc' }, h('td', { colspan: 6 }, `${uuidName(ss) || 'Service'} `, h('code', {}, s.uuid), ` handles ${s.start}–${s.end}`)));
    for (const c of GATT.chars.filter(c => c.svc === si)) {
      const props = PROPS.filter(([b]) => c.props & b).map(([, n]) => n).join(' ');
      const inp = h('input', { class: 'mono', placeholder: 'hex' });
      const gp = h('input', { type: 'checkbox', checked: PACKETIZED.has(RSP_OF[c.short] || '') || ['0072', '0074', '0076', '0078', '0091'].includes(c.short) });
      rows.push(h('tr', {}, h('td', {}, c.handle), h('td', {}, h('b', {}, uuidName(c.short) || ''), h('br'), h('code', {}, c.uuid)), h('td', {}, props), h('td', {}, c.cccd || '—'),
        h('td', {}, c.props & 0x02 ? h('button', { onclick: () => api('/api/read', { handle: c.handle }).catch(e => toast(e.message, true)) }, 'Read') : null,
          c.cccd && c.props & 0x10 ? h('button', { class: c.sub === 1 ? 'on' : '', onclick: () => tryApi('/api/subscribe', { handle: c.handle, mode: c.sub === 1 ? 0 : 1 }).then(() => setTimeout(loadGatt, 800)) }, c.sub === 1 ? 'Notify ✓' : 'Notify') : null,
          c.cccd && c.props & 0x20 ? h('button', { class: c.sub === 2 ? 'on' : '', onclick: () => tryApi('/api/subscribe', { handle: c.handle, mode: c.sub === 2 ? 0 : 2 }).then(() => setTimeout(loadGatt, 800)) }, c.sub === 2 ? 'Indicate ✓' : 'Indicate') : null),
        h('td', {}, c.props & 0x0c ? [inp, h('label', { class: 'chk', title: 'wrap in Open GoPro packet headers' }, gp, 'GoPro'),
          h('button', { onclick: () => { try { const m = unhex(inp.value); localTx(c.short, 'GATT write', m); api('/api/write', { handle: c.handle, hex: hex(m, ''), gopro: gp.checked ? 1 : 0, rsp: c.props & 0x08 ? 1 : 0 }).catch(e => toast(e.message, true)); } catch (e) { toast(e.message, true); } } }, 'Write')] : null)));
    }
  });
  $('#gatt-body').replaceChildren(...rows);
  $('#gatt-sum').textContent = GATT.chars.length ? `${GATT.services.length} services, ${GATT.chars.length} characteristics` : 'not discovered';
  $('#raw-h').replaceChildren(h('option', { value: '' }, '— characteristic —'), ...GATT.chars.filter(c => c.props & 0x0c).map(c => h('option', { value: c.handle }, `h${c.handle} ${uuidName(c.short) || c.short}`)));
  const cmd = handleOf('0072'); if (cmd) $('#raw-h').value = cmd;
}

// ---------------------------------------------------------------- Connect tab
const MFG_STATUS = ['awake', 'Wi-Fi AP on', 'pairing mode', 'central role', 'new media'];
function decodeAdv(hexs) {
  const b = hexs ? unhex(hexs) : new Uint8Array(); const out = {}; let i = 0;
  while (i < b.length) { const n = b[i]; if (!n) break; const t = b[i + 1], v = b.slice(i + 2, i + 1 + n); i += n + 1;
    if (t === 0xff && v[0] === 0xf2 && v[1] === 0x02) {
      out.schema = v[2]; out.status = MFG_STATUS.filter((_, k) => v[3] & (1 << k)).join(', ') || 'asleep'; out.model = v[4];
      out.pairing = !!(v[3] & 4);
    }
    if (t === 0x16 && v[0] === 0xa6 && v[1] === 0xfe) { out.ap_mac = hex(v.slice(2, 6), ':'); out.serial_tail = utf8.decode(v.slice(6)); }
  }
  return out;
}
async function refreshDevices() {
  const { devices } = await api('/api/devices');
  const only = $('#scan-gp').checked;
  const rows = devices.filter(d => !only || d.gopro).sort((a, b) => b.rssi - a.rssi).map(d => {
    const a = Object.assign(decodeAdv(d.adv), decodeAdv(d.rsp));
    return h('tr', {}, h('td', {}, d.name || h('i', {}, '(no name)'), d.gopro ? h('span', { class: 'tag' }, 'GoPro') : null, d.bonded ? h('span', { class: 'tag ok' }, 'bonded') : null,
      a.pairing ? h('span', { class: 'tag warn' }, 'pairing mode') : null),
      h('td', { class: 'mono' }, d.addr, h('small', {}, d.type ? ' random' : ' public')), h('td', {}, d.rssi + ' dBm'),
      h('td', { class: 'small' }, a.schema != null ? `model ${a.model} · ${a.status}` : '', a.serial_tail ? ` · SN …${a.serial_tail}` : ''),
      h('td', {}, h('button', { class: 'pri', onclick: () => connect(d.addr, d.type) }, 'Connect')));
  });
  $('#dev-body').replaceChildren(...rows);
}
async function connect(addr, type) {
  await tryApi('/api/connect', { addr, type, auto_secure: $('#a-sec').checked, auto_discover: $('#a-disc').checked, auto_subscribe: $('#a-sub').checked }, 'connecting…');
}
let STATUS = null;
async function refreshStatus() {
  try { STATUS = await api('/api/status'); } catch (e) { $('#conn-state').textContent = 'device unreachable'; return; }
  const b = STATUS.ble;
  const pill = $('#pill'); pill.textContent = b.link === 'connected' ? (b.encrypted ? 'connected · encrypted' : 'connected') : b.link; pill.className = 'pill ' + b.link;
  $('#conn-state').replaceChildren(
    kv('Link', b.link + (b.peer ? ` ${b.peer}` : '')), kv('Encrypted', b.encrypted ? `yes (key ${b.key_size})` : 'no'), kv('Bonded', b.bonded ? 'yes' : 'no'),
    kv('MTU', b.mtu || '—'), kv('RSSI', b.link === 'connected' ? b.rssi + ' dBm' : '—'), kv('Packets', `tx ${b.tx} (err ${b.tx_err}) · rx ${b.rx}`),
    kv('Up', b.connected_s ? b.connected_s + ' s' : '—'), kv('Last disconnect', b.last_disc_reason ? errName(b.last_disc_reason) : '—'), kv('Scanning', b.scanning ? 'yes' : 'no'));
  $('#ka').checked = b.keepalive;
  $('#hdr-fw').textContent = `${STATUS.project} ${STATUS.fw} · ${STATUS.running.label} · heap ${(STATUS.heap / 1024).toFixed(0)} KB`;
  renderFw();
}
const kv = (k, v) => h('div', { class: 'kv' }, h('span', {}, k), h('b', {}, String(v)));
function renderHw() {
  const hw = STATE.hw; if (!hw) return;
  $('#hw').replaceChildren(...Object.entries(hw).filter(([k]) => k !== 'status').map(([k, v]) => kv(k.replace(/_/g, ' '), v)));
  scheduleTables();
}

// "GoPro setup" macro: the spec's post-connect sequence.
async function goproSetup() {
  const steps = $('#setup-log'); steps.replaceChildren();
  const say = (t, bad) => steps.append(h('div', { class: bad ? 'bad' : '' }, t));
  const waitRsp = (ch, ms = 3000) => new Promise(res => { const tm = setTimeout(() => { pendingRsp = null; res(null); }, ms); pendingRsp = { ch, cb: e => { clearTimeout(tm); res(e); } }; });
  try {
    if (!handleOf('0072')) throw new Error('GoPro Command characteristic not discovered');
    let ok = false;
    for (let i = 0; i < 10 && !ok; i++) {
      const p = waitRsp('0073'); await sendMsg('0072', Uint8Array.of(0x3C), 'Get Hardware Info'); const r = await p;
      ok = r && r.cls === 'ok'; say(`Get Hardware Info #${i + 1}: ${r ? r.title : 'no response'}`, !ok);
      if (!ok) await new Promise(r2 => setTimeout(r2, 1000));
    }
    if (!ok) throw new Error('camera never reported BLE-ready');
    if ($('#setup-pair').checked && handleOf('0091')) {
      const p = waitRsp('0092'); await sendMsg('0091', cat(Uint8Array.of(0x03, 0x01), PB.encode('RequestPairingFinish', { result: 'SUCCESS', phoneName: 'RecLink' })), 'Set Pairing State');
      const r = await p; say('Set Pairing State: ' + (r ? r.title + ' ' + JSON.stringify(r.obj) : 'no response'));
    }
    for (const [id, n] of [[0x51, 'Get Open GoPro Version'], [0x10, 'Get Local Date Time']]) {
      const p = waitRsp('0073'); await sendMsg('0072', Uint8Array.of(id), n); const r = await p; say(`${n}: ${r ? JSON.stringify(r.obj) : 'no response'}`);
    }
    { const p = waitRsp('0077'); await sendMsg('0076', Uint8Array.of(0xF5, 0x6C), 'Get Camera Capabilities'); const r = await p; say('Camera capabilities: ' + (r ? JSON.stringify(r.obj) : 'no response (older camera)')); }
    for (const [id, n] of [[0x53, 'Register all statuses'], [0x52, 'Register all settings'], [0x62, 'Register all capabilities']]) {
      const p = waitRsp('0077', 5000); await sendMsg('0076', Uint8Array.of(id), n); const r = await p; say(`${n}: ${r ? r.title : 'no response'}`);
    }
    await api('/api/keepalive', { on: true, ms: 3000 }); say('Keep-alive every 3 s: on');
    say('Done — see the Settings and Statuses tabs.');
  } catch (e) { say(e.message, true); }
  refreshStatus();
}

// ---------------------------------------------------------------- Firmware tab
function slotCard(title, p) {
  if (!p) return h('div', { class: 'card' }, h('h4', {}, title), '—');
  return h('div', { class: 'card' }, h('h4', {}, title, ' ', h('code', {}, p.label)),
    p.valid ? [kv('Project', p.project), kv('Version', p.version), kv('Built', p.built), kv('IDF', p.idf), kv('OTA state', p.ota_state || '—')] : h('p', { class: 'dim' }, 'no valid app image'));
}
function renderFw() {
  if (!STATUS) return;
  $('#slots').replaceChildren(slotCard('Running', STATUS.running), slotCard('Other slot', STATUS.other));
  const o = STATUS.other, b = $('#boot-other');
  b.disabled = !(o && o.valid);
  b.textContent = o && o.valid ? `Boot ${o.project} ${o.version} (other slot)` : 'Other slot empty';
}
async function bootOther() {
  const o = STATUS && STATUS.other;
  if (!o || !o.valid) return;
  if (!confirm(`Reboot into "${o.project} ${o.version}" in slot ${o.label}?\n\nThe original firmware's AP is RECLINK-XXXX (not RECLINK-DBG).`)) return;
  const j = await tryApi('/api/boot_other', {});
  toast(`Rebooting into ${j.project} ${j.version}…`);
}
function uploadOta() {
  const f = $('#ota-file').files[0]; if (!f) return toast('choose a .bin', true);
  if (!confirm(`Flash ${f.name} (${(f.size / 1024).toFixed(0)} KB) into the other slot and reboot into it?`)) return;
  const x = new XMLHttpRequest(); const bar = $('#ota-bar');
  x.upload.onprogress = e => { if (e.lengthComputable) bar.value = e.loaded / e.total; };
  x.onload = () => { let j = {}; try { j = JSON.parse(x.responseText); } catch (e) { /* */ }
    if (x.status === 200 && j.ok) toast(`Flashed ${j.project} ${j.version} — rebooting`); else toast('OTA failed: ' + (j.error || x.status), true); };
  x.onerror = () => toast('upload error (device rebooting?)', true);
  x.open('POST', '/api/ota'); x.setRequestHeader('Content-Type', 'application/octet-stream'); x.send(f);
}
async function refreshBonds() {
  const { bonds } = await api('/api/bonds');
  $('#bonds').replaceChildren(...(bonds.length ? bonds.map(b => h('div', { class: 'row' }, h('code', {}, b.addr), h('small', {}, b.type ? 'random' : 'public'),
    h('button', { class: 'danger', onclick: async () => { if (confirm(`Delete bond ${b.addr}?\n\nThe bond store is SHARED with the original RecLink firmware: it will have to re-pair this camera.`)) { await tryApi('/api/bonds/delete', b, 'deleted'); refreshBonds(); } } }, 'Delete')))
    : [h('p', { class: 'dim' }, 'no bonds stored')]));
}
async function refreshSyslog() { $('#syslog').textContent = await (await fetch('/api/log')).text(); $('#syslog').scrollTop = 1e9; }

// ---------------------------------------------------------------- event pump
let nextSeq = 0;
async function pump() {
  for (;;) {
    let got = 0;
    try {
      const r = await fetch(`/api/events?since=${nextSeq}&max=150`, { cache: 'no-store' });
      const j = await r.json();
      if (nextSeq && j.oldest > nextSeq) addLog({ t: 0, dir: 'sys', title: `⚠ ${j.oldest - nextSeq} events lost (UI fell behind the ring buffer)`, cls: 'bad' });
      for (const ev of j.ev) { if (ev.t) clockSkew = ev.t - performance.now(); onEvent(ev); }
      got = j.ev.length; nextSeq = j.next;
      $('#hdr-link').className = 'dot ok';
    } catch (e) { $('#hdr-link').className = 'dot bad'; await new Promise(r => setTimeout(r, 1500)); }
    renderLog();
    if (got < 150) await new Promise(r => setTimeout(r, 200));
  }
}

function exportLog() {
  const blob = new Blob([JSON.stringify(LOG, null, 1)], { type: 'application/json' });
  const a = h('a', { href: URL.createObjectURL(blob), download: `gopro-ble-log-${new Date().toISOString().replace(/[:.]/g, '-')}.json` }); a.click();
}

// ---------------------------------------------------------------- wiring
function showTab(name) {
  for (const t of $$('.tab')) t.hidden = t.id !== 'tab-' + name;
  for (const b of $$('nav button')) b.classList.toggle('sel', b.dataset.tab === name);
  try { localStorage.setItem('tab', name); } catch (e) { /* storage blocked */ }
  logDirty = true; renderLog(); renderSettings(); renderStatuses();
  if (name === 'firmware') { refreshBonds().catch(() => {}); refreshSyslog().catch(() => {}); }
}
window.addEventListener('DOMContentLoaded', () => {
  for (const b of $$('nav button')) b.onclick = () => showTab(b.dataset.tab);
  $('#scan-start').onclick = () => tryApi('/api/scan', { on: 1, secs: +$('#scan-secs').value, active: $('#scan-act').checked ? 1 : 0, clear: 1 });
  $('#scan-stop').onclick = () => tryApi('/api/scan', { on: 0 });
  $('#btn-disc').onclick = () => tryApi('/api/disconnect', {});
  $('#btn-sec').onclick = () => tryApi('/api/secure', {});
  $('#btn-gatt').onclick = () => tryApi('/api/discover', {}, 'discovering…');
  $('#btn-suball').onclick = () => tryApi('/api/subscribe_all', {}).then(() => setTimeout(loadGatt, 1500));
  $('#btn-mtu').onclick = () => tryApi('/api/mtu', {});
  $('#btn-setup').onclick = goproSetup;
  $('#ka').onchange = e => tryApi('/api/keepalive', { on: e.target.checked, ms: 3000 });
  for (const id of ['#a-sec', '#a-disc', '#a-sub']) $(id).onchange = () => tryApi('/api/auto', { secure: $('#a-sec').checked, discover: $('#a-disc').checked, subscribe: $('#a-sub').checked });
  $('#scan-gp').onchange = () => refreshDevices().catch(() => {});
  $('#op-q').oninput = renderOpList;
  $('#raw-send').onclick = rawSend;
  $('#s-only').onchange = renderSettings; $('#s-q').oninput = renderSettings; $('#st-q').oninput = renderStatuses;
  $('#s-getall').onclick = () => sendMsg('0076', Uint8Array.of(0x12), 'Get all settings').catch(e => toast(e.message, true));
  $('#s-reg').onclick = () => sendMsg('0076', Uint8Array.of(0x52), 'Register all settings').catch(e => toast(e.message, true));
  $('#s-caps').onclick = () => sendMsg('0076', Uint8Array.of(0x32), 'Get all setting capabilities').catch(e => toast(e.message, true));
  $('#st-getall').onclick = () => sendMsg('0076', Uint8Array.of(0x13), 'Get all statuses').catch(e => toast(e.message, true));
  $('#st-reg').onclick = () => sendMsg('0076', Uint8Array.of(0x53), 'Register all statuses').catch(e => toast(e.message, true));
  $('#st-unreg').onclick = () => sendMsg('0076', Uint8Array.of(0x73), 'Unregister all statuses').catch(e => toast(e.message, true));
  for (const id of ['#f-raw', '#f-ka', '#f-q']) $(id).oninput = () => { logDirty = true; renderLog(); };
  $('#log-clear').onclick = () => { LOG.length = 0; logDirty = true; renderLog(); };
  $('#log-export').onclick = exportLog;
  $('#boot-other').onclick = bootOther;
  $('#ota-go').onclick = uploadOta;
  $('#reboot').onclick = () => confirm('Reboot the RecLink?') && tryApi('/api/reboot', {}, 'rebooting…');
  $('#bonds-all').onclick = async () => { if (confirm('Delete ALL bonds?\n\nShared with the original firmware: every camera must be re-paired.')) { await tryApi('/api/bonds/delete', { all: 1 }, 'all bonds deleted'); refreshBonds(); } };
  $('#syslog-refresh').onclick = () => refreshSyslog();
  $('#catrev').textContent = OGP.REV;
  renderOpList(); selectOp(OPS[0]);
  let tab = 'connect'; try { tab = localStorage.getItem('tab') || tab; } catch (e) { /* */ }
  if ($('#tab-' + location.hash.slice(1))) tab = location.hash.slice(1);   // deep link: /#commands
  showTab(tab);
  refreshStatus(); loadGatt().catch(() => {});
  setInterval(refreshStatus, 2000);
  setInterval(() => { if (!$('#tab-connect').hidden) refreshDevices().catch(() => {}); }, 1500);
  setInterval(() => { if (!$('#tab-statuses').hidden) renderStatuses(); }, 1000);
  pump();
});
