# GOPRO-BLE-DEBUG

A standalone debug firmware for the RecLink board. It talks to a GoPro over
BLE and lets you send **every Open GoPro BLE command** — TLV commands,
settings, queries and protobuf operations, plus raw GATT reads and writes — from
a Wi-Fi web page. Everything the camera sends back is shown decoded.

It installs through the **original RecLink firmware's OTA page** and puts the
original back just as easily. It never touches the original's settings.

```
Phone / laptop ──Wi-Fi──▶ RecLink (ESP32-C3) ──BLE──▶ GoPro
  http://10.0.0.1          gopro_ble_debug.bin
```

## Install (from the original firmware)

1. Build it, or use a prebuilt `gopro_ble_debug.bin`:
   ```sh
   cd gopro_ble_debug
   idf.py set-target esp32c3      # RecLink board (esp32s3 = dev board)
   idf.py build                   # -> build/gopro_ble_debug.bin
   ```
2. Open the original RecLink portal (`RECLINK-XXXX` → http://10.0.0.1). The FC
   must be **disarmed**. Upload `gopro_ble_debug.bin` with **Firmware update**,
   and leave "erase settings" **off**.
3. The board reboots into the debug firmware. Join Wi-Fi **`RECLINK-DBG-XXXX`**
   (password `reclinkfpv`) and open **http://10.0.0.1**.

## Going back to the original firmware — any time

OTA always writes the *spare* slot, so the original firmware is still in flash
while the debug image runs. You have three ways back:

| How | When |
|---|---|
| **Firmware → "Boot fc_cam_bridge … (other slot)"** | Instant, no upload. This is the normal way. |
| **Firmware → Upload `fc_cam_bridge.bin`** | If the other slot was overwritten or you want a different build. |
| **Automatic** | The image marks itself valid only once its Wi-Fi + web server are up. If it never gets that far, the bootloader rolls back to the original on the next reset. After that, **3 crash resets in a row** (each within 60 s of boot) also boot the original. |

USB flashing (`idf.py flash` from the repo root) remains the last resort.

To return to debug later, upload `gopro_ble_debug.bin` from the original portal again.

### What is shared, and what isn't

| | Debug firmware |
|---|---|
| Partition table | The original's `../partitions.csv`, by reference (byte-identical) |
| Original settings (NVS `cfg`) | **Never read or written.** NVS is never erased, even if it fails to open. |
| BLE bonds (NVS `nimble_bond`) | **Shared**, with the same NimBLE store config, address and name ("RecLink"). A camera paired in either firmware reconnects in the other without re-pairing. |
| Wi-Fi AP | Own SSID `RECLINK-DBG-XXXX`, same 10.0.0.1 address |

Deleting a bond in the debug UI deletes it for the original firmware too. The UI warns before doing it.

## Using it

1. **Connect** — Scan (GoPro filter on by default) → Connect. On connect it
   pairs/encrypts, discovers the whole GATT table and subscribes to every
   notifiable characteristic. You can turn each of those steps off.
   *First pairing:* put the camera in pairing mode first (Connections → Connect
   Device). A camera already bonded to RecLink just reconnects.
2. **Run setup** does the spec's post-connect sequence: poll *Get Hardware
   Info* until the camera is BLE-ready, optionally *Set Pairing State*, read
   the version, clock and capabilities, register for all status, setting and
   capability pushes, and start keep-alive (every 3 s).
3. **Commands** — about 100 operations grouped as in the spec (Control, Query,
   Presets, Networking, COHN, Live Streaming), each with parameter fields, a
   live hex preview and the decoded response.
   - TLV commands: multi-parameter commands (Press Button, Set Submode, Client
     Device Type) let you choose the encoding. The spec describes per-parameter
     length bytes, while the Python SDK sends one length for all parameters.
   - Protobuf: edit the request as JSON. Enums can be given by name, and the
     schema and enum values are shown.
   - Operations whose `.proto` is **not public** (TimeWarp, offload, preset
     create/remove, BT classic, …) come with the right feature/action IDs and
     a field editor (`1:varint=1`, `2:string=abc`, …). Their responses are
     decoded generically.
   - **Raw message** sends any bytes to any handle, packetized or raw.
4. **Settings** — all 59 spec settings with the current value, every option
   (filtered to your camera model from *Get Hardware Info*, ✕ = unsupported or
   not in the current capabilities) and a Set button. IDs ≥ 256 use the 2-byte
   `0xFF` form.
5. **Statuses** — every status ID, decoded and live (pushes highlight).
6. **GATT** — the full table: read, notify/indicate toggles, and writes to any
   characteristic.
7. **Log** — every message decoded. Raw packets, keep-alive traffic and a text
   filter are optional, and the log exports as JSON.
8. **Firmware** — slot info, boot other slot, OTA upload, bond list, device log.

## How it works

The firmware is a byte transport. The browser does all Open GoPro framing,
encoding and decoding.

- `main/dbg_ble.c` — NimBLE central. One GATT worker runs exactly one ATT
  procedure at a time (discovery, writes, reads, CCCDs, keep-alive), because
  ATT allows a single outstanding request. It has full service, characteristic
  and descriptor discovery (real CCCD handles) and an Open GoPro TX packetizer
  (20-byte packets with general, extended-13 and continuation headers, up to
  8191-byte messages).
- `main/evlog.c` — ring of every TX/RX packet and link event, polled by the UI
  via `GET /api/events?since=N`. If the UI falls behind, it reports the gap.
- `main/web.c` — HTTP API + gzip-embedded UI. `main/wifi_ap.c` — AP + captive DNS.
- `www/catalog.js` — **generated** decode tables. `www/ops.js` — the operation
  catalog. `www/app.js` — UI, protobuf codec, depacketizer, decoders.

### HTTP API

| Method | Path | Body / notes |
|---|---|---|
| GET | `/api/status` | firmware, both OTA slots, BLE link state |
| POST | `/api/scan` | `{on, secs, active, clear}` |
| GET | `/api/devices` | scan results with raw adv / scan-response |
| POST | `/api/connect` | `{addr:"AA:BB:..", type, auto_secure, auto_discover, auto_subscribe}` |
| POST | `/api/disconnect` `/api/secure` `/api/discover` `/api/mtu` `/api/subscribe_all` | |
| GET | `/api/gatt` | discovered services + characteristics (handles, props, CCCD) |
| POST | `/api/write` | `{handle, hex, gopro:1\|0, rsp:1\|0}` — `gopro` = add Open GoPro packet headers |
| POST | `/api/read` | `{handle}` → result arrives as an event |
| POST | `/api/subscribe` | `{handle, mode:0\|1\|2}` off / notify / indicate |
| POST | `/api/keepalive` | `{on, ms}` |
| GET | `/api/events?since=N&max=M` | event stream (`y`: 0 sys, 1 tx, 2 rx, 3 ind, 4 read, 5 err) |
| GET | `/api/log` | ESP log ring |
| GET/POST | `/api/bonds`, `/api/bonds/delete` | `{addr, type}` or `{all:1}` |
| POST | `/api/ota` | raw `.bin` body (same contract as the original's) |
| POST | `/api/boot_other` `/api/reboot` | |

## Developing the UI without hardware

```sh
python3 tools/mock_server.py --port 8080      # fake RecLink + GoPro, open http://localhost:8080
# headless end-to-end check (20 assertions):
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new \
    --virtual-time-budget=60000 --dump-dom http://localhost:8080/selftest.html | grep -A30 selftest-result
# codec unit tests (47 checks against spec / SDK byte examples), any JS shell:
cd www && /System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc ../tools/test_ui.js
```

### Regenerating the decode tables

`www/catalog.js` comes from gopro/OpenGoPro (main `0f96357` + the gh-pages BLE spec):

```sh
python3 tools/gen_catalog.py \
  --proto OpenGoPro/protobuf \
  --statuses OpenGoPro/demos/python/sdk_wireless_camera_control/open_gopro/models/constants/statuses.py \
  --settings OpenGoPro/demos/python/sdk_wireless_camera_control/open_gopro/models/constants/settings.py \
  --settings-db tools/spec_settings_db.json \
  --extra-status '123=Active Preset Is Custom' --extra-status '28=Pairing State (Legacy)' \
  -o www/catalog.js
```

`tools/spec_settings_db.json` holds the settings and operations database
embedded in the OpenGoPro spec site, extracted from its JS bundle.

## Known limits

- The spec documents *Get OTA Status* and *Get CSI Value* by name only, with no
  IDs. They are listed with editable feature/action fields so you can probe for
  them.
- Request schemas for the non-public protobuf operations are unknown. Field
  numbers in their default templates are guesses.
- Wi-Fi and BLE share the C3 radio. Scans run at 50% duty so the AP stays usable.
