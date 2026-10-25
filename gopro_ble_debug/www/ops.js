// ops.js -- every Open GoPro BLE operation the spec documents, plus the
// SDK-only legacy ones. Sources: gopro/OpenGoPro gh-pages BLE spec (operation
// database + feature pages) and the Python SDK (constants.py / ble_commands.py)
// @ main 0f96357.
//
// k (kind):
//   cmd   TLV command on GP-0072:  [id][params...]
//   setk  TLV on GP-0074 (Keep Alive)
//   set   Change Setting on GP-0074 (handled by the Settings tab too)
//   query TLV query on GP-0076:    [id][element ids...] (empty = all)
//   pb    protobuf: [feature][action][message] on `c`
//   read  plain GATT read of `c`;   write: plain GATT write of `c`
// p: params {n name, t u8|u16|u32|i16|str|hex|dt7|dt10, o {value: label}, d default}
// pub:false = request message schema is NOT published -> generic field editor.
(() => {
'use strict';

const CH = {   // short GoPro UUIDs (GP-XXXX = b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b)
  '0002': 'WiFi AP SSID', '0003': 'WiFi AP Password', '0004': 'WiFi AP Power',
  '0005': 'WiFi AP State', '0006': 'WiFi CSI Password',
  '0072': 'Command', '0073': 'Command Response', '0074': 'Settings',
  '0075': 'Settings Response', '0076': 'Query', '0077': 'Query Response',
  '0078': 'Sensor', '0079': 'Sensor Response',
  '0091': 'Network Mgmt Command', '0092': 'Network Mgmt Response',
  '0081': 'Internal 81', '0082': 'Internal 82', '0083': 'Internal 83', '0084': 'Internal 84',
};
// Response characteristic for each request characteristic.
const RSP_OF = { '0072': '0073', '0074': '0075', '0076': '0077', '0078': '0079', '0091': '0092' };
// Characteristics carrying Open GoPro packetized messages (depacketize on RX).
const PACKETIZED = new Set(['0073', '0075', '0077', '0079', '0092']);

const OFFON = { 0: 'Off', 1: 'On' };
const SUBMODE_ENC = 'Multi-param commands: spec says each param is length-prefixed; the SDK sends one length for all. Pick in "Encoding".';

const OPS = [
  // ---------------- Control (Command GP-0072) ----------------
  { g: 'Control', n: 'Set Shutter', k: 'cmd', id: 0x01, p: [{ n: 'mode', t: 'u8', o: OFFON, d: 1 }] },
  { g: 'Control', n: 'Hilight Moment', k: 'cmd', id: 0x18 },
  { g: 'Control', n: 'Sleep', k: 'cmd', id: 0x05, note: 'Camera stays BLE-connectable while asleep.' },
  { g: 'Control', n: 'Power Down (SDK legacy)', k: 'cmd', id: 0x04 },
  { g: 'Control', n: 'Reboot the Camera', k: 'cmd', id: 0x11, note: 'Approximates a battery pull.' },
  { g: 'Control', n: 'Set AP Control', k: 'cmd', id: 0x17, p: [{ n: 'mode', t: 'u8', o: { 0: 'Disable', 1: 'Enable', 2: 'Bounce' }, d: 1 }],
    note: 'Wait for status 69 (AP Mode) = 1 before connecting over Wi-Fi.' },
  { g: 'Control', n: 'Locate Camera', k: 'cmd', id: 0x16, p: [{ n: 'enable', t: 'u8', o: OFFON, d: 1 }] },
  { g: 'Control', n: 'Press Button', k: 'cmd', id: 0x1B, multi: true, note: SUBMODE_ENC,
    p: [{ n: 'button_id', t: 'u8', o: { 0: 'Shutter', 1: 'Mode' }, d: 0 }, { n: 'press_type', t: 'u8', o: { 0: 'Short', 1: 'Long' }, d: 0 }] },
  { g: 'Control', n: 'Load Preset Group (Set Mode)', k: 'cmd', id: 0x3E,
    p: [{ n: 'group', t: 'u16', o: { 1000: 'Video', 1001: 'Photo', 1002: 'Timelapse' }, d: 1000 }] },
  { g: 'Control', n: 'Load Preset', k: 'cmd', id: 0x40, p: [{ n: 'preset_id', t: 'u32', d: 0 }],
    note: 'Preset IDs come from "Get Available Presets".' },
  { g: 'Control', n: 'Load Legacy Mode', k: 'cmd', id: 0x02, p: [{ n: 'mode', t: 'u8', d: 0 }] },
  { g: 'Control', n: 'Set Submode', k: 'cmd', id: 0x03, multi: true, note: 'Spec lists explicit 1-byte lengths: 03 01 <mode> 01 <submode>.',
    p: [{ n: 'mode', t: 'u8', d: 0 }, { n: 'submode', t: 'u8', d: 0 }] },
  { g: 'Control', n: 'Reset Protune', k: 'cmd', id: 0x3F },
  { g: 'Control', n: 'Reset Video Protune', k: 'cmd', id: 0x0A, p: [{ n: 'value', t: 'u8', d: 1 }] },
  { g: 'Control', n: 'Reset Photo Protune', k: 'cmd', id: 0x0C, p: [{ n: 'value', t: 'u8', d: 1 }] },
  { g: 'Control', n: 'Reset Multishot Protune', k: 'cmd', id: 0x0B, p: [{ n: 'value', t: 'u8', d: 1 }] },
  { g: 'Control', n: 'Delete Last File', k: 'cmd', id: 0x42, danger: true },
  { g: 'Control', n: 'Delete All Files (format SD)', k: 'cmd', id: 0x41, danger: true },
  { g: 'Control', n: 'Set Date Time', k: 'cmd', id: 0x0D, p: [{ n: 'date_time', t: 'dt7' }] },
  { g: 'Control', n: 'Set Local Date Time (tz + DST)', k: 'cmd', id: 0x0F, p: [{ n: 'date_time', t: 'dt10' }] },
  { g: 'Control', n: 'Set Analytics (third-party client)', k: 'cmd', id: 0x50 },
  { g: 'Control', n: 'Set Client Device Type', k: 'cmd', id: 0x1C, multi: true, note: SUBMODE_ENC,
    p: [{ n: 'reserved', t: 'u8', d: 0 }, { n: 'device_type', t: 'u8', o: { 1: 'iOS', 2: 'Android' }, d: 2 },
        { n: 'reserved', t: 'u8', d: 0 }, { n: 'reserved', t: 'u8', d: 0 }, { n: 'reserved', t: 'u8', d: 0 }, { n: 'reserved', t: 'u8', d: 0 }] },
  { g: 'Control', n: 'Keep Alive', k: 'setk', id: 0x5B, p: [{ n: 'keep_alive', t: 'u8', d: 0x42 }],
    note: 'Setting 91 = 0x42 on GP-0074. The Connect tab can send this automatically every 3 s.' },
  { g: 'Control', n: 'Set Camera Control', k: 'pb', c: '0072', f: 0xF1, a: 0x69, req: 'RequestSetCameraControlStatus', rsp: 'ResponseGeneric',
    def: { camera_control_status: 'CAMERA_EXTERNAL_CONTROL' } },
  { g: 'Control', n: 'Set Camera Name', k: 'pb', c: '0072', f: 0xF1, a: 0x62, req: 'RequestSetCameraName', rsp: 'ResponseGeneric', def: { name: 'GoPro' } },
  { g: 'Control', n: 'Set Turbo Transfer', k: 'pb', c: '0072', f: 0xF1, a: 0x6B, req: 'RequestSetTurboActive', rsp: 'ResponseGeneric', def: { active: false } },
  { g: 'Control', n: 'Set In Contextual Menu', k: 'pb', c: '0072', f: 0xF1, a: 0x6A, req: 'RequestSetInContextualMenu', rsp: 'ResponseGeneric', pub: false,
    def: '1:varint=1' },
  { g: 'Control', n: 'Clear New Media Flag', k: 'pb', c: '0072', f: 0xF1, a: 0x7C, req: 'RequestClearNewMediaFlag', rsp: 'ResponseGeneric', pub: false, def: '' },
  { g: 'Control', n: 'Reset Onscreen Shortcuts', k: 'pb', c: '0072', f: 0xF1, a: 0x6F, req: 'RequestShortcutsReset', rsp: 'ResponseGeneric', pub: false, def: '' },
  { g: 'Control', n: 'Set App Power-On Start', k: 'pb', c: '0072', f: 0xF1, a: 0x7D, req: 'RequestSetSystemNotifyEvent', rsp: 'ResponseGeneric', pub: false, def: '1:varint=0' },
  { g: 'Control', n: 'Set Mobile Offload Power-On Start', k: 'pb', c: '0072', f: 0xF1, a: 0x7D, req: 'RequestSetSystemNotifyEvent', rsp: 'ResponseGeneric', pub: false, def: '1:varint=0' },
  { g: 'Control', n: 'Set Mobile Offload Stop', k: 'pb', c: '0072', f: 0xF1, a: 0x7D, req: 'RequestSetSystemNotifyEvent', rsp: 'ResponseGeneric', pub: false, def: '1:varint=0' },
  { g: 'Control', n: 'Set Wakeup Reason', k: 'pb', c: '0072', f: 0xF1, a: 0x7D, req: 'RequestSetSystemNotifyEvent', rsp: 'ResponseGeneric', pub: false, def: '1:varint=0' },
  { g: 'Control', n: 'Set TimeWarp Speed 1x', k: 'pb', c: '0072', f: 0xF1, a: 0x71, req: 'RequestSetTimeWarpSpeed1X', rsp: 'ResponseGeneric', pub: false, def: '1:varint=1' },
  { g: 'Control', n: 'Set TimeWarp Speed Ramp', k: 'pb', c: '0072', f: 0xF1, a: 0x6D, req: 'RequestSetTimeWarpSpeedRamp', rsp: 'ResponseGeneric', pub: false, def: '1:varint=0' },
  { g: 'Control', n: 'Set TimeWarp Speed Ramp Active', k: 'pb', c: '0072', f: 0xF1, a: 0x6C, req: 'RequestSetTimeWarpSpeedRampActive', rsp: 'ResponseGeneric', pub: false, def: '1:varint=1' },

  // ---------------- Query ----------------
  { g: 'Query', n: 'Get Hardware Info', k: 'cmd', id: 0x3C, note: 'Poll until success after connecting = camera BLE ready.' },
  { g: 'Query', n: 'Get Open GoPro Version', k: 'cmd', id: 0x51 },
  { g: 'Query', n: 'Get Date Time', k: 'cmd', id: 0x0E },
  { g: 'Query', n: 'Get Local Date Time (tz + DST)', k: 'cmd', id: 0x10 },
  { g: 'Query', n: 'Get Settings JSON (SDK legacy)', k: 'cmd', id: 0x3B },
  { g: 'Query', n: 'Get Setting Values', k: 'query', id: 0x12, ids: 'setting' },
  { g: 'Query', n: 'Get Status Values', k: 'query', id: 0x13, ids: 'status' },
  { g: 'Query', n: 'Get Setting Capabilities', k: 'query', id: 0x32, ids: 'setting' },
  { g: 'Query', n: 'Register for Setting Value Updates', k: 'query', id: 0x52, ids: 'setting' },
  { g: 'Query', n: 'Register for Status Value Updates', k: 'query', id: 0x53, ids: 'status' },
  { g: 'Query', n: 'Register for Setting Capability Updates', k: 'query', id: 0x62, ids: 'setting' },
  { g: 'Query', n: 'Unregister for Setting Value Updates', k: 'query', id: 0x72, ids: 'setting' },
  { g: 'Query', n: 'Unregister for Status Value Updates', k: 'query', id: 0x73, ids: 'status' },
  { g: 'Query', n: 'Unregister for Setting Capability Updates', k: 'query', id: 0x82, ids: 'setting' },
  { g: 'Query', n: 'Get Setting Values (2-byte IDs)', k: 'query', id: 0x15, ids: 'setting', w: 2 },
  { g: 'Query', n: 'Get Status Values (2-byte IDs)', k: 'query', id: 0x16, ids: 'status', w: 2 },
  { g: 'Query', n: 'Register Setting Updates (2-byte IDs)', k: 'query', id: 0x55, ids: 'setting', w: 2 },
  { g: 'Query', n: 'Register Status Updates (2-byte IDs)', k: 'query', id: 0x56, ids: 'status', w: 2 },
  { g: 'Query', n: 'Unregister Setting Updates (2-byte IDs)', k: 'query', id: 0x75, ids: 'setting', w: 2 },
  { g: 'Query', n: 'Unregister Status Updates (2-byte IDs)', k: 'query', id: 0x76, ids: 'status', w: 2 },
  { g: 'Query', n: 'Get Camera Capabilities', k: 'pb', c: '0076', f: 0xF5, a: 0x6C, req: 'RequestGetCameraCapabilities', rsp: 'ResponseGetCameraCapabilities', def: {},
    note: 'supports_2byte_ids=true -> use the 2-byte query / 0xFF setting forms.' },
  { g: 'Query', n: 'Get Last Captured Media', k: 'pb', c: '0076', f: 0xF5, a: 0x6D, req: 'RequestGetLastCapturedMedia', rsp: 'ResponseLastCapturedMedia', def: {} },
  { g: 'Query', n: 'Get OTA Status / Get CSI Value', k: 'pb', c: '0076', f: 0xF5, a: 0x00, req: '', rsp: '', pub: false, def: '',
    note: 'Documented by name only -- the spec publishes no feature/action IDs for these. Fill in IDs to probe.' },

  // ---------------- Presets ----------------
  { g: 'Presets', n: 'Get Available Presets', k: 'pb', c: '0076', f: 0xF5, a: 0x72, req: 'RequestGetPresetStatus', rsp: 'NotifyPresetStatus',
    def: { register_preset_status: ['REGISTER_PRESET_STATUS_PRESET', 'REGISTER_PRESET_STATUS_PRESET_GROUP_ARRAY'], unregister_preset_status: [], use_constant_setting_ids: true, include_hidden: true } },
  { g: 'Presets', n: 'Update Custom Preset', k: 'pb', c: '0072', f: 0xF1, a: 0x64, req: 'RequestCustomPresetUpdate', rsp: 'ResponseGeneric',
    def: { title_id: 'PRESET_TITLE_USER_DEFINED_CUSTOM_NAME', custom_name: 'RecLink', icon_id: 'PRESET_ICON_VIDEO' } },
  { g: 'Presets', n: 'Set Preset Visibility (V2)', k: 'pb', c: '0072', f: 0xF1, a: 0x63, req: 'RequestPresetSetVisibility', rsp: 'ResponseGeneric', def: { id: 0, visible: true } },
  { g: 'Presets', n: 'Create Custom Preset (legacy / V2 / V3)', k: 'pb', c: '0072', f: 0xF1, a: 0x76, req: 'RequestCreateCustomPreset', rsp: 'ResponseCreateCustomPreset', pub: false, def: '' },
  { g: 'Presets', n: 'Duplicate Active Preset', k: 'pb', c: '0072', f: 0xF1, a: 0x61, req: 'RequestPresetDuplicate', rsp: 'ResponsePresetDuplicate', pub: false, def: '' },
  { g: 'Presets', n: 'Remove Preset', k: 'pb', c: '0072', f: 0xF1, a: 0x6E, req: 'RequestSetPresetRemove', rsp: 'ResponseGeneric', pub: false, def: '1:varint=0', danger: true },
  { g: 'Presets', n: 'Reset Preset (factory)', k: 'pb', c: '0072', f: 0xF1, a: 0x75, req: 'RequestSetPresetFactoryReset', rsp: 'ResponseGeneric', pub: false, def: '1:varint=0', danger: true },
  { g: 'Presets', n: 'Set Preset Order', k: 'pb', c: '0072', f: 0xF1, a: 0x77, req: 'RequestSetPresetOrder', rsp: 'ResponseGeneric', pub: false, def: '' },
  { g: 'Presets', n: 'Update Preset (List) Order', k: 'pb', c: '0072', f: 0xF1, a: 0x60, req: 'RequestSetPresetListOrder', rsp: 'ResponseGeneric', pub: false, def: '' },

  // ---------------- Networking ----------------
  { g: 'Networking', n: 'Read WiFi AP SSID', k: 'read', c: '0002', dec: 'str' },
  { g: 'Networking', n: 'Read WiFi AP Password', k: 'read', c: '0003', dec: 'str' },
  { g: 'Networking', n: 'Read WiFi AP State', k: 'read', c: '0005' },
  { g: 'Networking', n: 'Read WiFi CSI Password', k: 'read', c: '0006', dec: 'str' },
  { g: 'Networking', n: 'Write WiFi AP Power', k: 'write', c: '0004', p: [{ n: 'power', t: 'u8', o: OFFON, d: 1 }] },
  { g: 'Networking', n: 'Set WiFi Config V2: write SSID', k: 'write', c: '0002', p: [{ n: 'ssid', t: 'rawstr', d: '' }] },
  { g: 'Networking', n: 'Set WiFi Config V2: write Password', k: 'write', c: '0003', p: [{ n: 'password', t: 'rawstr', d: '' }] },
  { g: 'Networking', n: 'Scan for Access Points', k: 'pb', c: '0091', f: 0x02, a: 0x02, req: 'RequestStartScan', rsp: 'ResponseStartScanning', def: {},
    note: 'Progress arrives as NotifStartScanning (02/0B); then Get AP Scan Results with the scan_id.' },
  { g: 'Networking', n: 'Get AP Scan Results', k: 'pb', c: '0091', f: 0x02, a: 0x03, req: 'RequestGetApEntries', rsp: 'ResponseGetApEntries', def: { start_index: 0, max_entries: 20, scan_id: 0 } },
  { g: 'Networking', n: 'Connect to Provisioned AP', k: 'pb', c: '0091', f: 0x02, a: 0x04, req: 'RequestConnect', rsp: 'ResponseConnect', def: { ssid: '' } },
  { g: 'Networking', n: 'Connect to a New AP', k: 'pb', c: '0091', f: 0x02, a: 0x05, req: 'RequestConnectNew', rsp: 'ResponseConnectNew', def: { ssid: '', password: '' } },
  { g: 'Networking', n: 'Release Network Ownership', k: 'pb', c: '0072', f: 0xF1, a: 0x78, req: 'RequestReleaseNetwork', rsp: 'ResponseGeneric', def: {} },
  { g: 'Networking', n: 'Set Pairing State (finish pairing)', k: 'pb', c: '0091', f: 0x03, a: 0x01, req: 'RequestPairingFinish', rsp: 'ResponseGeneric',
    def: { result: 'SUCCESS', phoneName: 'RecLink' }, note: 'Clears the pairing screen on the camera after first bond.' },
  { g: 'Networking', n: 'Get Network Capabilities', k: 'pb', c: '0091', f: 0x02, a: 0x01, req: 'RequestGetNetworkCapabilities', rsp: 'ResponseCapabilities', pub: false, def: '' },
  { g: 'Networking', n: 'Delete Stored AP', k: 'pb', c: '0091', f: 0x02, a: 0x06, req: 'RequestDeleteSingleAp', rsp: 'ResponseGeneric', pub: false, def: '1:string=', danger: true },
  { g: 'Networking', n: 'Delete All Stored APs', k: 'pb', c: '0091', f: 0x02, a: 0x07, req: 'RequestDeleteAllAp', rsp: 'ResponseGeneric', pub: false, def: '', danger: true },
  { g: 'Networking', n: 'Connect to MFi AP', k: 'pb', c: '0091', f: 0x02, a: 0x0D, req: 'RequestConnectMFi', rsp: 'ResponseConnectMFi', pub: false, def: '' },
  { g: 'Networking', n: 'Take Network Ownership (undocumented)', k: 'pb', c: '0091', f: 0x02, a: 0x08, req: 'RequestTakeOwnership', rsp: 'ResponseTakeOwnership', pub: false, def: '' },
  { g: 'Networking', n: 'Stop Network Features (undocumented)', k: 'pb', c: '0091', f: 0x02, a: 0x0A, req: 'RequestStopNetworkFeatures', rsp: 'ResponseGeneric', pub: false, def: '' },
  { g: 'Networking', n: 'Select Wireless Band', k: 'pb', c: '0072', f: 0xF1, a: 0x7B, req: 'RequestSetBandSelect', rsp: 'ResponseGeneric', pub: false, def: '1:varint=0' },
  { g: 'Networking', n: 'Start Network OTA Update', k: 'pb', c: '0072', f: 0xF1, a: 0x7A, req: 'RequestSetOTAUpdate', rsp: 'ResponseGeneric', pub: false, def: '', danger: true },
  { g: 'Networking', n: 'Pair Bluetooth Classic (HERO7-era)', k: 'pb', c: '0091', f: 0x03, a: 0x05, req: 'RequestPairBtc', rsp: 'ResponseGeneric', pub: false, def: '' },
  { g: 'Networking', n: 'Connect Bluetooth Classic', k: 'pb', c: '0091', f: 0x03, a: 0x07, req: 'RequestConnectBtc', rsp: 'ResponseGeneric', pub: false, def: '' },
  { g: 'Networking', n: 'Set Resume Sequence (undocumented)', k: 'pb', c: '0091', f: 0x03, a: 0x03, req: 'RequestSetSuspendSequence', rsp: 'ResponseGeneric', pub: false, def: '' },
  { g: 'Networking', n: 'Set Cold Boot Sequence (undocumented)', k: 'pb', c: '0091', f: 0x03, a: 0x04, req: 'RequestSetSuspendSequence', rsp: 'ResponseGeneric', pub: false, def: '' },
  { g: 'Networking', n: 'Ping (deprecated)', k: 'pb', c: '0091', f: 0x02, a: 0x00, req: '', rsp: 'ResponseGeneric', pub: false, def: '' },

  // ---------------- Camera on the Home Network ----------------
  { g: 'COHN', n: 'Get COHN Status', k: 'pb', c: '0076', f: 0xF5, a: 0x6F, req: 'RequestGetCOHNStatus', rsp: 'NotifyCOHNStatus', def: { register_cohn_status: true } },
  { g: 'COHN', n: 'Get COHN Certificate', k: 'pb', c: '0076', f: 0xF5, a: 0x6E, req: 'RequestCOHNCert', rsp: 'ResponseCOHNCert', def: {} },
  { g: 'COHN', n: 'Create COHN Certificate', k: 'pb', c: '0072', f: 0xF1, a: 0x67, req: 'RequestCreateCOHNCert', rsp: 'ResponseGeneric', def: { override: false } },
  { g: 'COHN', n: 'Clear COHN Certificate', k: 'pb', c: '0072', f: 0xF1, a: 0x66, req: 'RequestClearCOHNCert', rsp: 'ResponseGeneric', def: {}, danger: true },
  { g: 'COHN', n: 'Set COHN Setting', k: 'pb', c: '0072', f: 0xF1, a: 0x65, req: 'RequestSetCOHNSetting', rsp: 'ResponseGeneric', def: { cohn_active: true } },

  // ---------------- Live streaming ----------------
  { g: 'Live Streaming', n: 'Get Livestream Status', k: 'pb', c: '0076', f: 0xF5, a: 0x74, req: 'RequestGetLiveStreamStatus', rsp: 'NotifyLiveStreamStatus',
    def: { register_live_stream_status: ['REGISTER_LIVE_STREAM_STATUS_STATUS', 'REGISTER_LIVE_STREAM_STATUS_ERROR', 'REGISTER_LIVE_STREAM_STATUS_MODE', 'REGISTER_LIVE_STREAM_STATUS_BITRATE'] } },
  { g: 'Live Streaming', n: 'Set Livestream Mode', k: 'pb', c: '0072', f: 0xF1, a: 0x79, req: 'RequestSetLiveStreamMode', rsp: 'ResponseGeneric',
    def: { url: 'rtmp://', encode: true, window_size: 'WINDOW_SIZE_1080', minimum_bitrate: 1000, maximum_bitrate: 4000, starting_bitrate: 2000, lens: 'LENS_WIDE' } },
];

// Every protobuf (feature, action) the camera can SEND, -> message name.
// Requests above contribute their response; these add async notifications.
const PB_RX = {
  '02:0b': 'NotifStartScanning', '02:0c': 'NotifProvisioningState',
  'f5:f3': 'NotifyPresetStatus', 'f5:f5': 'NotifyLiveStreamStatus', 'f5:ef': 'NotifyCOHNStatus',
  '02:80': 'ResponseGeneric',
};
for (const o of OPS) {
  if (o.k === 'pb' && o.rsp) {
    const key = o.f.toString(16).padStart(2, '0') + ':' + ((o.a | 0x80) & 0xff).toString(16).padStart(2, '0');
    if (!(key in PB_RX)) PB_RX[key] = o.rsp;
  }
}
// Preset status responses use F2 (request 72 | 0x80), already covered above.
const PB_FEATURES = new Set([0x02, 0x03, 0xF1, 0xF3, 0xF5]);

// TLV command response layouts (after [cmd][status]): length-value fields.
const CMD_RSP = {
  0x3C: { lv: ['model_number', 'model_name', 'deprecated', 'firmware_version', 'serial_number', 'ap_ssid', 'ap_mac_address'],
          num: ['model_number'] },
  0x51: { lv: ['major', 'minor'], num: ['major', 'minor'] },
  0x0E: { dt: 7 }, 0x10: { dt: 10 },
};

window.OPSDEF = { CH, RSP_OF, PACKETIZED, OPS, PB_RX, PB_FEATURES, CMD_RSP };
})();
