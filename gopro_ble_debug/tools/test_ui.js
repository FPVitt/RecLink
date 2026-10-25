// test_ui.js -- headless checks of the web UI's Open GoPro codec, using spec /
// SDK byte examples. Runs under any plain JS shell, e.g. macOS JavaScriptCore:
//   JSC=/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc
//   cd www && $JSC ../tools/test_ui.js
var window = globalThis;
window.addEventListener = () => {};
var document = { querySelector: () => null, querySelectorAll: () => [] };
var performance = { now: () => 0 };
if (typeof TextEncoder === 'undefined') {
  globalThis.TextEncoder = class { encode(s) { const u = unescape(encodeURIComponent(s)); return Uint8Array.from(u, c => c.charCodeAt(0)); } };
  globalThis.TextDecoder = class { decode(b) { let s = ''; for (const x of b) s += String.fromCharCode(x); try { return decodeURIComponent(escape(s)); } catch (e) { return s; } } };
}
load('catalog.js'); load('ops.js'); load('app.js');
// UI hooks the decoders call; not under test here.
renderHw = () => {}; scheduleTables = () => {};

let fails = 0, n = 0;
function eq(name, got, want) {
  n++;
  const g = typeof got === 'string' ? got : JSON.stringify(got), w = typeof want === 'string' ? want : JSON.stringify(want);
  if (g !== w) { fails++; print('FAIL ' + name + '\n   got  ' + g + '\n   want ' + w); } else print('ok   ' + name);
}
const op = name => OPS.find(o => o.n === name);
const H = s => unhex(s);

// --- TLV command builders (SDK / spec wire examples) ---
eq('Set Shutter on', hex(buildOp(op('Set Shutter'), { p0: '1' }).msg), '01 01 01');
eq('Load Preset Group video', hex(buildOp(op('Load Preset Group (Set Mode)'), { p0: '1000' }).msg), '3e 02 03 e8');
eq('Load Preset u32', hex(buildOp(op('Load Preset'), { p0: '65536' }).msg), '40 04 00 01 00 00');
eq('Hilight', hex(buildOp(op('Hilight Moment'), {}).msg), '18');
eq('Keep alive', [buildOp(op('Keep Alive'), { p0: '66' })].map(b => b.ch + ' ' + hex(b.msg))[0], '0074 5b 01 42');
eq('Press Button spec LV', hex(buildOp(op('Press Button'), { p0: '1', p1: '0', enc: 'lv' }).msg), '1b 01 01 01 00');
eq('Press Button SDK single', hex(buildOp(op('Press Button'), { p0: '1', p1: '0', enc: 'single' }).msg), '1b 02 01 00');
eq('Set Submode = spec 03 01 m 01 s', hex(buildOp(op('Set Submode'), { p0: '2', p1: '5', enc: 'lv' }).msg), '03 01 02 01 05');
eq('Reset Video Protune', hex(buildOp(op('Reset Video Protune'), { p0: '1' }).msg), '0a 01 01');
// spec example: "2023-01-31 03:04:05 (utc-02:00) (dst: on)" => 07:E7:01:1F:03:04:05:FF:88:01
eq('Set Local Date Time (spec example)', hex(buildOp(op('Set Local Date Time (tz + DST)'), { p0: '2023-01-31T03:04:05', tz: '-120', dst: true }).msg), '0f 0a 07 e7 01 1f 03 04 05 ff 88 01');
eq('Set Date Time (spec example)', hex(buildOp(op('Set Date Time'), { p0: '2023-01-31T03:04:05' }).msg), '0d 07 07 e7 01 1f 03 04 05');
eq('Register all statuses', hex(buildOp(op('Register for Status Value Updates'), { ids: '' }).msg), '53');
eq('Get settings 2 3 121', hex(buildOp(op('Get Setting Values'), { ids: '2 3 121' }).msg), '12 02 03 79');
eq('Get status 2-byte', hex(buildOp(op('Get Status Values (2-byte IDs)'), { ids: '70 300' }).msg), '16 00 46 01 2c');

// --- protobuf ---
eq('PB RequestSetCameraControlStatus EXTERNAL', hex(buildOp(op('Set Camera Control'), { json: '{"camera_control_status":"CAMERA_EXTERNAL_CONTROL"}' }).msg), 'f1 69 08 02');
eq('PB RequestSetTurboActive false', hex(buildOp(op('Set Turbo Transfer'), { json: '{"active":false}' }).msg), 'f1 6b 08 00');
eq('PB pairing finish', hex(PB.encode('RequestPairingFinish', { result: 'SUCCESS', phoneName: 'RecLink' })), '08 00 12 07 52 65 63 4c 69 6e 6b');
eq('PB preset status request (SDK default)', hex(buildOp(op('Get Available Presets'), { json: JSON.stringify(op('Get Available Presets').def) }).msg), 'f5 72 08 01 08 02 18 01 20 01');
eq('PB required field missing throws', (() => { try { PB.encode('RequestConnect', {}); return 'no'; } catch (e) { return 'threw'; } })(), 'threw');
eq('PB negative int32 varint', hex(PB.varint(-1)), 'ff ff ff ff ff ff ff ff ff 01');
eq('PB decode ResponseGeneric', PB.decode('ResponseGeneric', H('0801')), { result: 'RESULT_SUCCESS' });
const live = { url: 'rtmp://x', encode: true, window_size: 'WINDOW_SIZE_1080', minimum_bitrate: 800, maximum_bitrate: 8000, starting_bitrate: 2000, lens: 'LENS_LINEAR' };
eq('PB roundtrip RequestSetLiveStreamMode', PB.decode('RequestSetLiveStreamMode', PB.encode('RequestSetLiveStreamMode', live)), live);
const scan = { result: 'RESULT_SUCCESS', scan_id: 3, entries: [{ ssid: 'home', signal_strength_bars: 4, signal_frequency_mhz: 2412, scan_entry_flags: 9 }] };
const scanB = PB.encode('ResponseGetApEntries', scan);
eq('PB nested repeated roundtrip (ResponseGetApEntries)', PB.decode('ResponseGetApEntries', scanB), scan);
eq('decode 0092 protobuf by feature/action', decodeMessage('0092', cat(Uint8Array.of(0x02, 0x83), scanB)).title, 'PB 0x02/0x83 ResponseGetApEntries');
eq('decode protobuf on 0073 (F1/E9 ResponseGeneric)', decodeMessage('0073', H('f1e90801')).obj, { result: 'RESULT_SUCCESS' });
eq('generic field lines', hex(PB.fromLines('1:varint=1\n2:string=ab\n3:bool=true\n4:sint=-2')), '08 01 12 02 61 62 18 01 20 03');
eq('generic decode (nested message, string, varints)', PB.generic(H('0801120261621a020801')), { 1: 1, 2: 'ab', 3: { 1: 1 } });

// --- TLV response decoders ---
eq('cmd rsp shutter ok', decodeMessage('0073', H('0100')).title, 'Set Shutter -> Success');
eq('cmd rsp invalid param', decodeMessage('0073', H('4002')).kind, 'bad');
// Get Local Date Time: [10][00][len 0a][07e7 01 1f 03 04 05][ff88][01]
eq('cmd rsp 0x10 with length byte', decodeMessage('0073', H('10000a07e7011f030405ff8801')).obj, { status: 'Success (0)', date: '2023-01-31 03:04:05', utc_offset_min: -120, dst: true });
// Get Hardware Info, SDK layout: [3c][00][04][model u32][len][name]...
const hw = '3c00' + '0400000037' + '0c' + hex(enc8.encode('HERO12 Black'), '') + '0400000000' + '0c' + hex(enc8.encode('H23.01.01.10'), '') +
  '0e' + hex(enc8.encode('C3501324500000'), '') + '0a' + hex(enc8.encode('GP24500000'), '') + '0c' + hex(enc8.encode('2674f7f65f78'), '');
const hwd = decodeMessage('0073', H(hw)).obj;
eq('hw info model/name/fw', [hwd.model_number, hwd.model_name, hwd.firmware_version, hwd.ap_ssid], [55, 'HERO12 Black', 'H23.01.01.10', 'GP24500000']);
// SDK parser: Padding(1) major Padding(1) minor  -> [51][00][01][02][01][00]
eq('OGP version', decodeMessage('0073', H('510001020100')).obj, { status: 'Success (0)', major: 2, minor: 0 });
eq('setting rsp', decodeMessage('0075', H('0200')).title, 'Set Video Resolution -> Success');
eq('keepalive rsp flagged', decodeMessage('0075', H('5b00')).ka, true);
eq('2-byte setting rsp', decodeMessage('0075', H('ff01010000')).obj.setting, 257);
const st = decodeMessage('0077', H('1300' + '0a0101' + '460155' + '1e05' + hex(enc8.encode('GP123'), '') + '2101ff' + '360800000000000003e8'));
eq('status query decode', st.obj, { status: 'Success (0)', '10 Encoding': '1', '70 Internal battery percentage': '85', '30 Access point ssid': '"GP123"', '33 Primary storage': 'Unknown (-1)', '54 Sd card remaining': '1000' });
eq('status table updated', fmtStatus(70, STATE.status[70].raw), '85');
const se = decodeMessage('0077', H('9200' + '020101' + '030108'));
eq('setting push decode', se.obj, { status: 'Success (0)', '2 Video Resolution': '4K (1)', '3 Frames Per Second': '30.0 (8)' });
const cap = decodeMessage('0077', H('3200' + '020101' + '020104' + '030108'));
eq('capabilities grouped', STATE.cap['2'], [1, 4]);
// multi-byte capability values (setting 62 bitrates) must not be cut to their low byte
eq('4-byte capability value kept whole', decodeMessage('0077', H('3200' + '3e040003d090' + '3e0400061a80')).obj['62 Secondary Stream Bit Rate'], '250 Kbps (250000), 400 Kbps (400000)');

// --- unofficial settings merged from camera gpControl dumps ---
eq('unofficial setting named (13 ISO Max)', [OGP.SETTING[13].n, OGP.SETTING[13].u, fmtSetting(13, H('04'))], ['ISO Max', 1, '800 (4)']);
eq('Protune shutter options (145)', fmtSetting(145, H('08')), '1/60 (8)');
eq('wide unofficial setting is int32ub', OGP.SETTING[62].f, 'int32ub');
eq('official setting not overridden', [OGP.SETTING[2].n, OGP.SETTING[2].u], ['Video Resolution', undefined]);
eq('official setting gained option name (176)', OGP.SETTING[176].o['0'] !== undefined, true);
eq('2-byte status push', Object.keys(decodeMessage('0077', H('9600' + '004601' + '50')).obj)[1], '70 Internal battery percentage');

// --- packetization: same algorithm as dbg_ble.c gp_send() ---
function gpPackets(msg) {
  const out = [];
  if (msg.length < 20) { out.push(cat(Uint8Array.of(msg.length), msg)); return out; }
  let off = 0, cnt = 0;
  while (off < msg.length) {
    const hdr = off === 0 ? Uint8Array.of(0x20 | ((msg.length >> 8) & 0x1f), msg.length & 0xff) : Uint8Array.of(0x80 | (cnt++ & 0x0f));
    const take = Math.min(msg.length - off, 20 - hdr.length);
    out.push(cat(hdr, msg.slice(off, off + take))); off += take;
  }
  return out;
}
const big = Uint8Array.from({ length: 700 }, (_, i) => i & 0xff);
const pk = gpPackets(big);
let res = null; for (const p of pk) { const r = depacketize(99, p); if (r.msg) res = r.msg; }
eq('700-byte message: packet count', pk.length, 1 + Math.ceil((700 - 18) / 19));
eq('700-byte message: reassembled', res && hex(res, '') === hex(big, ''), true);
eq('all packets <= 20 bytes', pk.every(p => p.length <= 20), true);
// packet 1 is the start; continuations count 0..15 then wrap, so packet 17 carries counter 0 again
eq('continuation counter wraps', [hex([pk[1][0]]), hex([pk[16][0]]), hex([pk[17][0]])], ['80', '8f', '80']);
eq('short message single packet', hex(gpPackets(H('010101'))[0]), '03 01 01 01');
let r2 = depacketize(5, H('0d1300')); eq('partial', r2.partial, '2/13');
eq('ext16 rx header', hex(depacketize(6, H('4000030a0b0c')).msg), '0a 0b 0c');

print(`\n${n - fails}/${n} passed`);
if (fails) throw new Error(fails + ' failures');
