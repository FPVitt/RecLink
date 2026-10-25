# Open GoPro BLE — implementation reference for RecLink

Everything learned while building `gopro_ble_debug`: protocol framing, every
known command, setting and status, protobuf messages, and the traps we hit on
the ESP32-C3. It's written for engineers adding GoPro features to RecLink.

> **Generated:** the tables in the appendices are generated from the same
> catalog the debug firmware ships (`www/catalog.js`, `www/ops.js`). To refresh
> them, run `python3 tools/gen_reference.py`. The prose sections are
> hand-written; edit them in `docs/reference_head.md`.

## How sure is each fact?

Every claim is tagged with where it comes from. Trust them in this order:

| Tag | Source | Confidence |
|---|---|---|
| **[HW]** | Observed on the real RecLink ESP32-C3 board (boot logs, crashes, heap figures) | Highest |
| **[CAM]** | Observed from a real camera, e.g. the capability response captured on 2026-10-25 | High |
| **[SPEC]** | Open GoPro BLE spec: the gh-pages operations/settings DB plus `protobuf/*.proto` @ `gopro/OpenGoPro` main `0f96357` | High, but the spec is loose in places (see §4.3) |
| **[SDK]** | GoPro's Python SDK (`open_gopro`) @ `0f96357`, i.e. what GoPro's own client actually sends | High for wire bytes |
| **[GPC]** | Cameras' own `/gp/gpControl` settings dumps (HERO11, HERO10, HERO9) from KonradIT/goprowifihack, Apache-2.0 | Good for names; **unofficial**. The camera may reject writes. |
| **[INF]** | Our inference | **Verify on a camera before relying on it** |

---

## 1. Read this first: the ten things most likely to bite you

1. **Serialize every ATT request.** ATT allows one outstanding request per link.
   Wait for each write response (or read, or CCCD write) before issuing the next.
   The original firmware fires 16+ writes back-to-back and reports that the
   camera "does not appear to honour" push registration. Dropped or rejected
   writes are the likely cause **[INF]**. `gopro_ble_debug` runs a single GATT
   worker task that does exactly one procedure at a time (`main/dbg_ble.c`).
2. **Re-subscribe CCCDs on every connection.** The camera does not persist
   them **[SPEC]**. Find the CCCD handles by discovering descriptors. Don't
   assume `value_handle + 1`, as the original does (`ble_central.c:230`).
3. **Wait for BLE readiness after connecting.** Poll *Get Hardware Info*
   (`0x3C`) until it returns status 0 before sending anything else **[SPEC]**.
4. **Send a keep-alive every 3 s.** Write `03 5B 01 42` to GP-0074 (Settings),
   which is setting 91 = `0x42` **[SPEC][SDK]**. Without it the camera sleeps.
5. **Packetize everything you send to GP-0072/0074/0076/0091** with Open GoPro
   headers, as 20-byte packets, and reassemble everything you receive, per
   characteristic (§3).
6. **Check protobuf before TLV when decoding.** The spec's rule: if byte 0 is a
   feature ID and byte 1 is a known action ID for it, the message is protobuf.
   Otherwise it's TLV (§4.1).
7. **Value widths vary, and values are big-endian.** Some statuses arrive as
   8-byte fields (54 SD remaining, 35 remaining video). Setting 62's capability
   options are 4-byte values **[CAM]**. Never assume 1 byte. `-1` is sent as `0xFF`.
8. **The camera keeps separate Protune values for each capture mode,** under
   different setting IDs (ISO, shutter). Setting the wrong ID silently changes
   another mode (§6.2).
9. **Capabilities depend on the current mode.** While in a video preset, the
   photo and multishot settings report only their current value **[CAM]**.
   Query capabilities (`0x32`) after loading a preset, not once at boot.
10. **On the ESP32-C3, budget stack and heap deliberately.** Wi-Fi start-up needs
    about 6 KB of stack, and a log hook adds its cost to *every* task. Our first
    build crash-looped at boot and silently rolled back on OTA **[HW]** (§9).

---

## 2. BLE link

### 2.1 Advertising and discovery **[SPEC]**
- Filter on the 16-bit service UUID **`0xFEA6`**, which appears in the
  incomplete UUID list. The local name is `GoPro XXXX` and arrives in the
  **scan response**, so use an active scan.
- Manufacturer data has company ID `0x02F2`, so the bytes read `F2 02`, followed by:
  - schema version (2 or 3)
  - camera status bits: 0 awake, 1 Wi-Fi AP on, **2 pairing mode**, 3 central role, 4 new media
  - model ID
  - capability bits
- Scan-response service data (`A6 FE …`) carries the AP MAC (4 bytes) and the
  serial-number tail as ASCII.
- A camera stays connectable over BLE for 8 h after going to sleep, and
  connecting wakes it up.

### 2.2 Pairing and bonding
- The **first pairing requires the camera to be in pairing mode** (camera UI →
  Connections → Connect Device). After that, the bond lets it reconnect without
  pairing mode **[SPEC]**.
- RecLink uses LE Secure Connections, Just Works (no MITM, no I/O), bonding, and
  distributes ENC and ID keys (`ble_central.c`) **[HW]**.
- After the first bond, send **Set Pairing State** (protobuf `03/01`,
  `RequestPairingFinish{result: SUCCESS, phoneName}` on GP-0091). It clears the
  camera's pairing screen **[SPEC][SDK]**.
- If the camera forgot the bond (e.g. its connections were reset), NimBLE raises
  `REPEAT_PAIRING`. Delete the stale bond and retry. Encryption fails with an SM
  error until the user re-pairs.
- **Bond storage is shared between RecLink images.** Both firmwares use the
  NimBLE `nimble_bond` NVS namespace with the same configuration (max bonds 4,
  SC, NVS persist), the same public address and the same GAP name ("RecLink").
  Keep that configuration identical in any image that must keep the camera
  bonded **[HW]**.

### 2.3 GATT table

GP-XXXX is shorthand for `b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b`. NimBLE's
128-bit UUID literals are **little-endian**: the XXXX bytes sit at index 12–13.

| Service | Characteristic | Use | Properties |
|---|---|---|---|
| GP-0001 WiFi AP | GP-0002 SSID / GP-0003 Password | Read (needs encryption); writable for "Set WiFi Config V2" | R/W |
| | GP-0004 AP Power | Write | W |
| | GP-0005 AP State | Read / Indicate | R/I |
| | GP-0006 CSI Password | Read | R |
| FEA6 Control & Query | **GP-0072 Command → GP-0073 Response** | TLV commands + protobuf (F1) | W / N |
| | **GP-0074 Settings → GP-0075 Response** | set setting, keep-alive | W / N |
| | **GP-0076 Query → GP-0077 Response** | queries, pushes, protobuf (F5) | W / N |
| | GP-0078 Sensor → GP-0079 Response | not documented | |
| GP-0090 Camera Mgmt | **GP-0091 Net Mgmt Command → GP-0092 Response** | protobuf features 02/03 | W / N |

The camera has more characteristics, including an internal GP-0080 service (81–84).
Discover everything and subscribe to every characteristic with notify or
indicate set **[SPEC]**.

### 2.4 Link parameters
- RecLink requests an MTU of 247, so notifications can carry up to 244 bytes.
  **Send** 20-byte packets anyway, as the SDK does; the framing in §3 assumes it.
- Connection interval 30–50 ms, supervision timeout 4 s (`dbg_ble.c`,
  `ble_central.c`).

---

## 3. Packet framing **[SPEC]**

Every message on GP-0072/73/74/75/76/77/91/92 is wrapped in packet headers.
Header byte 0 bit 7 is 0 for a start packet and 1 for a continuation.

| Header | Bytes | Layout | Max message |
|---|---|---|---|
| General | 1 | `000LLLLL` | 31 (but keep the packet ≤ 20 B, so payload ≤ 19 B) |
| Extended-13 | 2 | `001LLLLL LLLLLLLL` | 8191 |
| Extended-16 (**RX only**) | 3 | `010xxxxx LLLLLLLL LLLLLLLL` | 65535 |
| Continuation | 1 | `1xxxCCCC` (4-bit counter, wraps 0xF → 0x0) | — |

**TX recipe** (`gp_send()` in `main/dbg_ble.c`):
- payload ≤ 19 B → `[len][payload]` in one packet.
- Otherwise:
  1. `[0x20 | len>>8][len & 0xFF]` + 18 bytes,
  2. then `[0x80 | n++ & 0xF]` + 19 bytes, repeated until done.

**RX recipe:** keep one reassembly buffer **per characteristic**. A
continuation without a start means a lost packet, so drop it and resync on the
next start packet. Size the buffer for get-all responses: `0x13` get-all
statuses exceeds 512 B, and preset status protobufs can be several KB.

> ⚠️ The original `gopro_pack()` (`gopro_tlv.c`) only sends general headers
> (≤ 31 B). Anything longer, such as a register-for-many-IDs query, needs the
> extended-13 path.

---

## 4. Message payloads

### 4.1 Telling protobuf from TLV **[SPEC]**
1. If `msg[0]` ∈ {`0x02`, `0x03`, `0xF1`, `0xF3`, `0xF5`} **and** `(msg[0], msg[1])`
   is a known protobuf response or notification (Appendix C), decode
   `msg[2:]` as that protobuf message.
2. Otherwise it's TLV, parsed by characteristic: Command table on 0073,
   Settings on 0075, Query on 0077.

GP-0092 (network management) only carries protobuf.

### 4.2 TLV commands (GP-0072 → GP-0073)
- Request: `[cmd id][params…]`. Response: `[cmd id][status][payload…]`.
- Status: `0` success, `1` error, `2` invalid parameter.
- Response payloads are length-value sequences. Layouts we decode:
  - **0x3C Get Hardware Info**: LV fields `model_number` (4 B, big-endian),
    `model_name`, `deprecated`, `firmware_version`, `serial_number`, `ap_ssid`,
    `ap_mac_address` **[SPEC][SDK]**.
  - **0x51 Open GoPro version**: LV `major`, LV `minor`.
  - **0x0E / 0x10 Get (Local) Date Time**: `[response_length][yyyy BE][mm][dd][hh][mi][ss]`.
    For `0x10` add `[utc_offset int16 BE][is_dst]`. **There IS a length byte**
    after the status **[SPEC]** (see §10.1).

### 4.3 ⚠️ Multi-parameter TLV commands are ambiguous
The data-protocol page says each parameter is length-prefixed:
`[cmd][len][val][len][val]…`. The SDK builds **one** length for all parameters:
`[cmd][total_len][params…]` **[SDK]**. For single-parameter commands both give
the same bytes, e.g. Set Shutter = `01 01 01`.

They differ for **Press Button (0x1B)**, **Set Submode (0x03)** and **Set
Client Device Type (0x1C)**. Set Submode's own spec text shows explicit
per-parameter lengths (`03 01 <mode> 01 <submode>`). **Verify on the target
camera before shipping.** The debug UI has an "Encoding" selector for exactly
this.

### 4.4 Settings (GP-0074 → GP-0075)
- 1-byte ID: `[setting id][value_len][value BE]`.
- IDs above 255 (Mission 1 cameras): `[0xFF][id_hi][id_lo][value_len][value]`.
  Use this only if `Get Camera Capabilities` reports `supports_2byte_ids`.
- Value widths come from the spec DB: almost all are `int8ub`.
  - `int64ub`: **30** Photo Timelapse Rate, **32** Nightlapse Rate, **168** Scheduled Capture.
  - Unofficial `int32ub`: **62**, **67** (stream bitrates, option IDs like 250000) **[GPC][CAM]**.
- Response: `[setting id][status]`. A success means accepted, not applied, so
  confirm via a push or a read-back.

### 4.5 Queries (GP-0076 → GP-0077)
- Request: `[query id][element ids…]`. **An empty list means ALL** **[SPEC]**.
  `53` alone registers for every status.
- Response or push: `[query id][status]` followed by triplets
  `[id][len][value]`, or `[id_hi][id_lo][len][value]` for the 2-byte variants.

| Operation | 1-byte | 2-byte |
|---|---|---|
| Get setting values | 0x12 | 0x15 |
| Get status values | 0x13 | 0x16 |
| Get setting capabilities | 0x32 | — |
| Register setting / status updates | 0x52 / 0x53 | 0x55 / 0x56 |
| Register capability updates | 0x62 | — |
| Unregister setting / status / capability | 0x72 / 0x73 / 0x82 | 0x75 / 0x76 / — |
| **Push**: setting / status / capability | **0x92 / 0x93 / 0xA2** | 0x95 / 0x96 / — |

- **Capability responses repeat the ID once per allowed value.** Collect them
  into a list **[CAM]**.
- Register responses (`0x52`/`0x53`) carry the current values too, so treat
  them like a get.

### 4.6 Value decoding
- Integers are big-endian at whatever length the camera sends.
  - Status 54 (SD remaining, KB) and 35 (remaining video, s) can be 8 bytes.
    The original takes the low 32 bits (`gopro_driver.c:292`), which is fine for
    realistic values.
- `-1` arrives as `0xFF`, e.g. status 33 Primary Storage = Unknown.
- String statuses: **29** connected Wi-Fi SSID, **30** AP SSID, **122** camera
  name. They may be NUL-padded.
- Status 33 PrimaryStorage values **[SPEC]**:
  - `0` OK, `1` full, `2` removed, `3` format error, `4` busy (a card **is**
    present), `8` swapped.

---

## 5. Protobuf **[SPEC]**

- Frame: `[feature id][action id][proto2 message]`, packetized like everything
  else. Features: `02` network management and `03` wireless/pairing (both on
  GP-0091), `F1` commands (GP-0072), `F3` settings, `F5` queries (GP-0076).
- Responses usually use **action | 0x80**, with exceptions:
  - Preset status: request `F5/72` → response `F5/F2`, async modified push `F5/F3`.
  - Live stream status: `F5/74` → `F5/F4`, push `F5/F5`.
  - COHN status: `F5/6F` → `F5/EF`, for both response and push.
  - Network scan: progress arrives as `02/0B` `NotifStartScanning`;
    provisioning as `02/0C`.

  Appendix C lists the full map.
- Schemas are public for the 11 `.proto` files only (Appendix F). About 25
  operations have **no published schema**, including TimeWarp, offload/power-on
  events, preset create/remove/order, Bluetooth Classic and band select (marked
  `pub:false` in Appendix B). Their feature/action IDs are known; their fields
  aren't. Probe them with care.
- proto2 rules:
  - `required` fields must be present, or the camera answers `RESULT_ILL_FORMED`.
  - Encode repeated scalars **unpacked**, but accept packed when decoding.
  - Enums are varints, and negative `int32` values take 10 bytes.
- Useful defaults (exactly what the SDK sends):
  - Get Available Presets: `F5 72 08 01 08 02 18 01 20 01`, which registers for
    preset and group changes, uses constant setting IDs and includes hidden presets.
  - Set Camera Control (external): `F1 69 08 02`.
  - Pairing finish: `03 01 08 00 12 <len> <phoneName>`.

---

## 6. Settings

### 6.1 Official vs unofficial
- **59** settings are in the spec DB **[SPEC]**. **68** more are unofficial.
  Cameras support them, but the spec leaves them out: Protune ISO, shutter, EV,
  WB, color, sharpness, bitrate, RAW audio, wind and 10-bit, plus streaming and
  setup. Names come from the HERO11 → HERO10 → HERO9 gpControl dumps **[GPC]**.
- Our camera's capability response listed many of these IDs with sensible
  values **[CAM]**, so they exist over BLE.
- **Writing them is unverified.** Expect status 1 or 2 on some models or
  firmware, and confirm with a push.

### 6.2 ⚠️ Per-mode Protune duplicates
The camera stores Protune separately for each capture type, so "ISO Max" and
"Shutter" each exist several times **[GPC]**. Each mode below comes from that
ID's option list.

| What | Video | Photo | Night photo | Multishot / time lapse | Night lapse |
|---|---|---|---|---|---|
| ISO Max | **13** (100–6400, Auto) | 24 † | — | 37 † | — |
| ISO Min | **102** (100–6400, Auto) | 75 † | — | 76 † | — |
| Shutter | **145** (1/24…1/3840, Auto) | 146 (1/125…1/2000) | 19 (2–30 s) | — | 31 (0.5–30 s) |

† The photo vs multishot split follows legacy ID numbering; the two pairs have
identical options **[INF]**.

WB 115, Color 116, Sharpness 117, EV 118, Bitrate 124, 10-bit 174, RAW audio 139
and Wind 149 have one ID each. They apply to the active preset **[INF]**.

### 6.3 Shutter speed is a shutter angle
Video shutter options (145) are whole fractions of the frame rate **[GPC][CAM]**.

| Frame rate | 360° | 180° | 90° | 45° | 22.5° |
|---|---|---|---|---|---|
| 24 fps | 1/24 | 1/48 | 1/96 | 1/192 | 1/384 |
| 25 fps | 1/25 | 1/50 | 1/100 | 1/200 | 1/400 |
| 30 fps | 1/30 | 1/60 | 1/120 | 1/240 | 1/480 |
| 60 fps | 1/60 | 1/120 | 1/240 | 1/480 | 1/960 |

The allowed set changes with setting 3 (fps). Our camera at 30 fps offered
Auto, 1/30, 1/60, 1/120, 1/240 and 1/480 **[CAM]**. To implement a "shutter
angle" feature, map angle → option ID **through the current capability list**.
Don't hard-code it.

### 6.4 Before you write a setting
1. Query capabilities (`0x32 <id>`) **in the current preset**.
2. Only write values that appear in the list.
3. Some writes cascade. Changing resolution can invalidate fps, lens or
   aspect, so re-read dependent settings or register for pushes (`0x52`).
4. The spec DB lists which camera models support each option (Appendix D).
   Match on `model_name` from `0x3C`.

---

## 7. Statuses
- **83 statuses are in the spec plus 14 unofficial ones (97 total).**
  Appendix E has the full list with enum values.
- The ones RecLink uses: 10 encoding (recording), 8 busy, 6 overheating,
  70 battery %, 2 battery bars, 33 storage, 54 SD remaining KB, 35 remaining
  video s, 96 preset group (1000 video / 1001 photo / 1002 time lapse).
  The original firmware maps these (`gopro_driver.c`).
- Also useful:
  - 69 AP mode: wait for 1 after *Set AP Control*.
  - 114 camera control ID (who owns the UI).
  - 89: `FLATMODE` in the SDK, `current_mode` in gpControl; the camera's
    settings filters key off it.
  - 82 ready.
- Prefer registering for pushes (`0x53` with an empty list) over polling. Keep a
  slow poll as a fallback until you've confirmed pushes arrive on your camera
  and firmware.

---

## 8. Operations
Appendix B lists all **101** known BLE operations: kind, characteristic,
IDs, parameters, response message, and whether the schema is public. Notes:
- **Get OTA Status** and **Get CSI Value** are documented by name only. No IDs
  are published.
- `0x3E` is both "Load Preset Group" and "Set Mode": a `u16` group (1000 video,
  1001 photo, 1002 time lapse) **[SDK]**. `0x40` Load Preset takes a `u32`
  preset ID; get the IDs from *Get Available Presets*.
- `0x41` Delete All Files and `0x42` Delete Last File are **destructive**.
  Guard them in any UI.
- `0x17` Set AP Control: 0 off, 1 on, 2 bounce. A success only means
  "accepted", so wait for status 69 **[SPEC]**.
- Legacy and SDK-only commands, which aren't in the current spec DB:
  - `0x04` Power Down
  - `0x3B` Get Settings JSON
  - `0x03` was "Set Pairing Complete" in the old SDK; it's now Set Submode.

---

## 9. ESP32-C3 / ESP-IDF / NimBLE lessons **[HW]**

1. **Stack.** Bringing up Wi-Fi AP + esp_netif + httpd needs about 6 KB. The
   default main task stack is 3.5 KB, so we raised it to 8 KB. The original runs
   Wi-Fi bring-up in its own 6 KB `portal_task` for the same reason.
2. **Log hooks run on every task's stack.** Our `esp_log_set_vprintf` mirror
   formatted each line a second time into a stack buffer. That overflowed a
   2 KB task on its first `gpio_reset_pin()` log line, causing a crash loop at
   boot. If you hook logging, format once into a static buffer.
3. **Heap.** BLE + Wi-Fi AP + httpd left **22 KB** free until we trimmed buffers;
   it's now 58 KB. Big consumers:
   - `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` (each about 1.6 KB, allocated at
     Wi-Fi init),
   - static OTA buffers,
   - event rings.

   Log `esp_get_free_heap_size()` after init in every build.
4. **Silent rollback.** With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, an OTA
   image that crashes before `esp_ota_mark_app_valid_cancel_rollback()` is
   rolled back on the next reset. **From the outside, the upload looks like it
   did nothing.** Always have a serial capture path. Mark the image valid only
   after the user-facing path (Wi-Fi + HTTP) is up.
5. **Crash-loop guard.** Once an image is valid, rollback no longer protects
   you. `gopro_ble_debug` counts panic/watchdog resets in RTC memory and boots
   the other slot after 3 in a row, each within 60 s. **[HW]** This fired
   during the crash loop and restored the original firmware.
6. **OTA compatibility.** Both images must use the **byte-identical partition
   table** (dual 1.9 MB OTA slots, NVS outside them) and matching flash header
   settings. The original's `/api/ota` accepts any valid image: it skips to the
   `0xE9` magic and has no project-name check. Don't use `?erase=1`, which
   wipes settings and bonds.
7. **NVS sharing.** Never touch the original's `cfg` namespace, and never erase
   NVS from a secondary image.
8. **Original's bond reconcile.** At boot the original runs `cam_task.c:368`:
   - 0 configured cameras + any bonds → **erases all bonds**.
   - Configured cameras + 0 bonds → flags REPAIR_NEEDED.

   Pairing a new camera from a secondary image leaves a partial mismatch, which
   is tolerated.
9. **Wi-Fi + BLE on one radio.** Scan at about 50% duty
   (interval 0x60, window 0x30) while the AP serves clients. The original drops
   to a passive ~11% "gentle" scan while its portal is up.
10. **USB-Serial-JTAG reset gotcha.** pyserial asserts DTR when opening the
    port, which holds the C3's IO9 low and forces download mode. Set `dtr=False`
    and `rts=False` **before** `open()`, then pulse RTS to reset.

---

## 10. Issues found in the original RecLink firmware

| # | Where | Issue | Severity |
|---|---|---|---|
| 10.1 | `gopro_driver.c:438` | `GET_DATE_TIME_DST (0x10)` parse assumes no length byte, but the spec has `response_length` after `[cmd][status]`, so every field is off by one (this is the FIXME there). Fix: skip 1 byte. | Medium (clock sync) |
| 10.2 | `gopro_driver.c:263–273` | `gp_pull_all()` issues 16+ writes back-to-back without waiting for responses. Likely why pushes "aren't honoured" **[INF]**. Fix: serialize, or use single `0x53` / `0x52` with an empty list (= all) or one multi-ID request. | Medium |
| 10.3 | `ble_central.c:230` | CCCD assumed at `val_handle + 1`. Works on GoPro today; breaks silently if the table changes. Discover descriptors. | Low |
| 10.4 | `gopro_tlv.c` `gopro_pack()` | TX supports only general headers (≤ 31 B payload, and >19 B breaks the 20-byte packet rule). Add extended-13 + continuation. | Low (latent) |

---

## 11. Open questions (need a camera)
- Which encoding Press Button / Set Submode / Client Device Type really use (§4.3).
- Whether writes to unofficial settings are accepted over BLE, per model.
- Photo vs multishot mapping of ISO 24/75 vs 37/76.
- What Horizon Lock 165 vs 166 control.
- IDs for Get OTA Status and Get CSI Value, and request schemas for the ~25
  non-public protobuf operations.
- Whether push registration works once writes are serialized (§10.2).

---

## Appendix A — GoPro characteristic short names

| GP-XXXX | Name | Response on |
|---|---|---|
| GP-0002 | WiFi AP SSID |  |
| GP-0003 | WiFi AP Password |  |
| GP-0004 | WiFi AP Power |  |
| GP-0005 | WiFi AP State |  |
| GP-0006 | WiFi CSI Password |  |
| GP-0072 | Command | GP-0073 |
| GP-0073 | Command Response |  |
| GP-0074 | Settings | GP-0075 |
| GP-0075 | Settings Response |  |
| GP-0076 | Query | GP-0077 |
| GP-0077 | Query Response |  |
| GP-0078 | Sensor | GP-0079 |
| GP-0079 | Sensor Response |  |
| GP-0091 | Network Mgmt Command | GP-0092 |
| GP-0092 | Network Mgmt Response |  |
| GP-0081 | Internal 81 |  |
| GP-0082 | Internal 82 |  |
| GP-0083 | Internal 83 |  |
| GP-0084 | Internal 84 |  |

Packetized (Open GoPro headers) on receive: GP-0073, GP-0075, GP-0077, GP-0079, GP-0092.

## Appendix B — All BLE operations

Kind: `cmd` TLV on GP-0072 · `setk`/`set` GP-0074 · `query` GP-0076 · `pb` protobuf `[feature][action][msg]` · `read`/`write` plain GATT. ⚠️ = destructive · 🔒 = request schema not public.

Parameter wire format (TLV commands): `[cmd id]` then each parameter as `[len][value]`; see §4.3 for multi-parameter commands. Types: `u8`/`u16`/`u32` big-endian unsigned; `dt7` = `[year u16 BE][month][day][hour][min][sec]` (spec example 2023-01-31 03:04:05 → `07 E7 01 1F 03 04 05`); `dt10` = `dt7` + `[utc_offset_min int16 BE][is_dst u8]` (… utc-02:00, dst on → `… FF 88 01`); `rawstr` = UTF-8 bytes, no header (plain GATT write).

| Group | Operation | Kind | Char | ID | Parameters / body | Request → Response | Notes |
|---|---|---|---|---|---|---|---|
| Control | Set Shutter | cmd | GP-0072 | 0x01 | `mode` u8 {0=Off, 1=On} | TLV → `[id][status]…` |  |
| Control | Hilight Moment | cmd | GP-0072 | 0x18 | — | TLV → `[id][status]…` |  |
| Control | Sleep | cmd | GP-0072 | 0x05 | — | TLV → `[id][status]…` | Camera stays BLE-connectable while asleep. |
| Control | Power Down (SDK legacy) | cmd | GP-0072 | 0x04 | — | TLV → `[id][status]…` |  |
| Control | Reboot the Camera | cmd | GP-0072 | 0x11 | — | TLV → `[id][status]…` | Approximates a battery pull. |
| Control | Set AP Control | cmd | GP-0072 | 0x17 | `mode` u8 {0=Disable, 1=Enable, 2=Bounce} | TLV → `[id][status]…` | Wait for status 69 (AP Mode) = 1 before connecting over Wi-Fi. |
| Control | Locate Camera | cmd | GP-0072 | 0x16 | `enable` u8 {0=Off, 1=On} | TLV → `[id][status]…` |  |
| Control | Press Button | cmd | GP-0072 | 0x1B | `button_id` u8 {0=Shutter, 1=Mode}<br>`press_type` u8 {0=Short, 1=Long}<br>⚠️ multi-param encoding (§4.3) | TLV → `[id][status]…` | Multi-param commands: spec says each param is length-prefixed; the SDK sends one length for all. Pick in "Encoding". |
| Control | Load Preset Group (Set Mode) | cmd | GP-0072 | 0x3E | `group` u16 {1000=Video, 1001=Photo, 1002=Timelapse} | TLV → `[id][status]…` |  |
| Control | Load Preset | cmd | GP-0072 | 0x40 | `preset_id` u32 | TLV → `[id][status]…` | Preset IDs come from "Get Available Presets". |
| Control | Load Legacy Mode | cmd | GP-0072 | 0x02 | `mode` u8 | TLV → `[id][status]…` |  |
| Control | Set Submode | cmd | GP-0072 | 0x03 | `mode` u8<br>`submode` u8<br>⚠️ multi-param encoding (§4.3) | TLV → `[id][status]…` | Spec lists explicit 1-byte lengths: 03 01 <mode> 01 <submode>. |
| Control | Reset Protune | cmd | GP-0072 | 0x3F | — | TLV → `[id][status]…` |  |
| Control | Reset Video Protune | cmd | GP-0072 | 0x0A | `value` u8 | TLV → `[id][status]…` |  |
| Control | Reset Photo Protune | cmd | GP-0072 | 0x0C | `value` u8 | TLV → `[id][status]…` |  |
| Control | Reset Multishot Protune | cmd | GP-0072 | 0x0B | `value` u8 | TLV → `[id][status]…` |  |
| Control | ⚠️ Delete Last File | cmd | GP-0072 | 0x42 | — | TLV → `[id][status]…` |  |
| Control | ⚠️ Delete All Files (format SD) | cmd | GP-0072 | 0x41 | — | TLV → `[id][status]…` |  |
| Control | Set Date Time | cmd | GP-0072 | 0x0D | `date_time` dt7 | TLV → `[id][status]…` |  |
| Control | Set Local Date Time (tz + DST) | cmd | GP-0072 | 0x0F | `date_time` dt10 | TLV → `[id][status]…` |  |
| Control | Set Analytics (third-party client) | cmd | GP-0072 | 0x50 | — | TLV → `[id][status]…` |  |
| Control | Set Client Device Type | cmd | GP-0072 | 0x1C | `reserved` u8<br>`device_type` u8 {1=iOS, 2=Android}<br>`reserved` u8<br>`reserved` u8<br>`reserved` u8<br>`reserved` u8<br>⚠️ multi-param encoding (§4.3) | TLV → `[id][status]…` | Multi-param commands: spec says each param is length-prefixed; the SDK sends one length for all. Pick in "Encoding". |
| Control | Keep Alive | setk | GP-0074 | 0x5B | `keep_alive` u8 | TLV → `[id][status]…` | Setting 91 = 0x42 on GP-0074. The Connect tab can send this automatically every 3 s. |
| Control | Set Camera Control | pb | GP-0072 | 0xF1/0x69 | JSON per schema | `RequestSetCameraControlStatus` → `ResponseGeneric` |  |
| Control | Set Camera Name | pb | GP-0072 | 0xF1/0x62 | JSON per schema | `RequestSetCameraName` → `ResponseGeneric` |  |
| Control | Set Turbo Transfer | pb | GP-0072 | 0xF1/0x6B | JSON per schema | `RequestSetTurboActive` → `ResponseGeneric` |  |
| Control | 🔒 Set In Contextual Menu | pb | GP-0072 | 0xF1/0x6A | fields unknown | `RequestSetInContextualMenu` → `ResponseGeneric` |  |
| Control | 🔒 Clear New Media Flag | pb | GP-0072 | 0xF1/0x7C | fields unknown | `RequestClearNewMediaFlag` → `ResponseGeneric` |  |
| Control | 🔒 Reset Onscreen Shortcuts | pb | GP-0072 | 0xF1/0x6F | fields unknown | `RequestShortcutsReset` → `ResponseGeneric` |  |
| Control | 🔒 Set App Power-On Start | pb | GP-0072 | 0xF1/0x7D | fields unknown | `RequestSetSystemNotifyEvent` → `ResponseGeneric` |  |
| Control | 🔒 Set Mobile Offload Power-On Start | pb | GP-0072 | 0xF1/0x7D | fields unknown | `RequestSetSystemNotifyEvent` → `ResponseGeneric` |  |
| Control | 🔒 Set Mobile Offload Stop | pb | GP-0072 | 0xF1/0x7D | fields unknown | `RequestSetSystemNotifyEvent` → `ResponseGeneric` |  |
| Control | 🔒 Set Wakeup Reason | pb | GP-0072 | 0xF1/0x7D | fields unknown | `RequestSetSystemNotifyEvent` → `ResponseGeneric` |  |
| Control | 🔒 Set TimeWarp Speed 1x | pb | GP-0072 | 0xF1/0x71 | fields unknown | `RequestSetTimeWarpSpeed1X` → `ResponseGeneric` |  |
| Control | 🔒 Set TimeWarp Speed Ramp | pb | GP-0072 | 0xF1/0x6D | fields unknown | `RequestSetTimeWarpSpeedRamp` → `ResponseGeneric` |  |
| Control | 🔒 Set TimeWarp Speed Ramp Active | pb | GP-0072 | 0xF1/0x6C | fields unknown | `RequestSetTimeWarpSpeedRampActive` → `ResponseGeneric` |  |
| Query | Get Hardware Info | cmd | GP-0072 | 0x3C | — | TLV → `[id][status]…` | Poll until success after connecting = camera BLE ready. |
| Query | Get Open GoPro Version | cmd | GP-0072 | 0x51 | — | TLV → `[id][status]…` |  |
| Query | Get Date Time | cmd | GP-0072 | 0x0E | — | TLV → `[id][status]…` |  |
| Query | Get Local Date Time (tz + DST) | cmd | GP-0072 | 0x10 | — | TLV → `[id][status]…` |  |
| Query | Get Settings JSON (SDK legacy) | cmd | GP-0072 | 0x3B | — | TLV → `[id][status]…` |  |
| Query | Get Setting Values | query | GP-0076 | 0x12 | setting IDs (1-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Get Status Values | query | GP-0076 | 0x13 | status IDs (1-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Get Setting Capabilities | query | GP-0076 | 0x32 | setting IDs (1-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Register for Setting Value Updates | query | GP-0076 | 0x52 | setting IDs (1-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Register for Status Value Updates | query | GP-0076 | 0x53 | status IDs (1-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Register for Setting Capability Updates | query | GP-0076 | 0x62 | setting IDs (1-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Unregister for Setting Value Updates | query | GP-0076 | 0x72 | setting IDs (1-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Unregister for Status Value Updates | query | GP-0076 | 0x73 | status IDs (1-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Unregister for Setting Capability Updates | query | GP-0076 | 0x82 | setting IDs (1-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Get Setting Values (2-byte IDs) | query | GP-0076 | 0x15 | setting IDs (2-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Get Status Values (2-byte IDs) | query | GP-0076 | 0x16 | status IDs (2-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Register Setting Updates (2-byte IDs) | query | GP-0076 | 0x55 | setting IDs (2-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Register Status Updates (2-byte IDs) | query | GP-0076 | 0x56 | status IDs (2-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Unregister Setting Updates (2-byte IDs) | query | GP-0076 | 0x75 | setting IDs (2-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Unregister Status Updates (2-byte IDs) | query | GP-0076 | 0x76 | status IDs (2-byte), empty = all | TLV → `[id][status][id len val]…` |  |
| Query | Get Camera Capabilities | pb | GP-0076 | 0xF5/0x6C | JSON per schema | `RequestGetCameraCapabilities` → `ResponseGetCameraCapabilities` | supports_2byte_ids=true -> use the 2-byte query / 0xFF setting forms. |
| Query | Get Last Captured Media | pb | GP-0076 | 0xF5/0x6D | JSON per schema | `RequestGetLastCapturedMedia` → `ResponseLastCapturedMedia` |  |
| Query | 🔒 Get OTA Status / Get CSI Value | pb | GP-0076 | 0xF5/0x00 | fields unknown | — → — | Documented by name only -- the spec publishes no feature/action IDs for these. Fill in IDs to probe. |
| Presets | Get Available Presets | pb | GP-0076 | 0xF5/0x72 | JSON per schema | `RequestGetPresetStatus` → `NotifyPresetStatus` |  |
| Presets | Update Custom Preset | pb | GP-0072 | 0xF1/0x64 | JSON per schema | `RequestCustomPresetUpdate` → `ResponseGeneric` |  |
| Presets | Set Preset Visibility (V2) | pb | GP-0072 | 0xF1/0x63 | JSON per schema | `RequestPresetSetVisibility` → `ResponseGeneric` |  |
| Presets | 🔒 Create Custom Preset (legacy / V2 / V3) | pb | GP-0072 | 0xF1/0x76 | fields unknown | `RequestCreateCustomPreset` → `ResponseCreateCustomPreset` |  |
| Presets | 🔒 Duplicate Active Preset | pb | GP-0072 | 0xF1/0x61 | fields unknown | `RequestPresetDuplicate` → `ResponsePresetDuplicate` |  |
| Presets | ⚠️ 🔒 Remove Preset | pb | GP-0072 | 0xF1/0x6E | fields unknown | `RequestSetPresetRemove` → `ResponseGeneric` |  |
| Presets | ⚠️ 🔒 Reset Preset (factory) | pb | GP-0072 | 0xF1/0x75 | fields unknown | `RequestSetPresetFactoryReset` → `ResponseGeneric` |  |
| Presets | 🔒 Set Preset Order | pb | GP-0072 | 0xF1/0x77 | fields unknown | `RequestSetPresetOrder` → `ResponseGeneric` |  |
| Presets | 🔒 Update Preset (List) Order | pb | GP-0072 | 0xF1/0x60 | fields unknown | `RequestSetPresetListOrder` → `ResponseGeneric` |  |
| Networking | Read WiFi AP SSID | read | GP-0002 | — | — | GATT read |  |
| Networking | Read WiFi AP Password | read | GP-0003 | — | — | GATT read |  |
| Networking | Read WiFi AP State | read | GP-0005 | — | — | GATT read |  |
| Networking | Read WiFi CSI Password | read | GP-0006 | — | — | GATT read |  |
| Networking | Write WiFi AP Power | write | GP-0004 | — | `power` u8 {0=Off, 1=On} | GATT write |  |
| Networking | Set WiFi Config V2: write SSID | write | GP-0002 | — | `ssid` rawstr | GATT write |  |
| Networking | Set WiFi Config V2: write Password | write | GP-0003 | — | `password` rawstr | GATT write |  |
| Networking | Scan for Access Points | pb | GP-0091 | 0x02/0x02 | JSON per schema | `RequestStartScan` → `ResponseStartScanning` | Progress arrives as NotifStartScanning (02/0B); then Get AP Scan Results with the scan_id. |
| Networking | Get AP Scan Results | pb | GP-0091 | 0x02/0x03 | JSON per schema | `RequestGetApEntries` → `ResponseGetApEntries` |  |
| Networking | Connect to Provisioned AP | pb | GP-0091 | 0x02/0x04 | JSON per schema | `RequestConnect` → `ResponseConnect` |  |
| Networking | Connect to a New AP | pb | GP-0091 | 0x02/0x05 | JSON per schema | `RequestConnectNew` → `ResponseConnectNew` |  |
| Networking | Release Network Ownership | pb | GP-0072 | 0xF1/0x78 | JSON per schema | `RequestReleaseNetwork` → `ResponseGeneric` |  |
| Networking | Set Pairing State (finish pairing) | pb | GP-0091 | 0x03/0x01 | JSON per schema | `RequestPairingFinish` → `ResponseGeneric` | Clears the pairing screen on the camera after first bond. |
| Networking | 🔒 Get Network Capabilities | pb | GP-0091 | 0x02/0x01 | fields unknown | `RequestGetNetworkCapabilities` → `ResponseCapabilities` |  |
| Networking | ⚠️ 🔒 Delete Stored AP | pb | GP-0091 | 0x02/0x06 | fields unknown | `RequestDeleteSingleAp` → `ResponseGeneric` |  |
| Networking | ⚠️ 🔒 Delete All Stored APs | pb | GP-0091 | 0x02/0x07 | fields unknown | `RequestDeleteAllAp` → `ResponseGeneric` |  |
| Networking | 🔒 Connect to MFi AP | pb | GP-0091 | 0x02/0x0D | fields unknown | `RequestConnectMFi` → `ResponseConnectMFi` |  |
| Networking | 🔒 Take Network Ownership (undocumented) | pb | GP-0091 | 0x02/0x08 | fields unknown | `RequestTakeOwnership` → `ResponseTakeOwnership` |  |
| Networking | 🔒 Stop Network Features (undocumented) | pb | GP-0091 | 0x02/0x0A | fields unknown | `RequestStopNetworkFeatures` → `ResponseGeneric` |  |
| Networking | 🔒 Select Wireless Band | pb | GP-0072 | 0xF1/0x7B | fields unknown | `RequestSetBandSelect` → `ResponseGeneric` |  |
| Networking | ⚠️ 🔒 Start Network OTA Update | pb | GP-0072 | 0xF1/0x7A | fields unknown | `RequestSetOTAUpdate` → `ResponseGeneric` |  |
| Networking | 🔒 Pair Bluetooth Classic (HERO7-era) | pb | GP-0091 | 0x03/0x05 | fields unknown | `RequestPairBtc` → `ResponseGeneric` |  |
| Networking | 🔒 Connect Bluetooth Classic | pb | GP-0091 | 0x03/0x07 | fields unknown | `RequestConnectBtc` → `ResponseGeneric` |  |
| Networking | 🔒 Set Resume Sequence (undocumented) | pb | GP-0091 | 0x03/0x03 | fields unknown | `RequestSetSuspendSequence` → `ResponseGeneric` |  |
| Networking | 🔒 Set Cold Boot Sequence (undocumented) | pb | GP-0091 | 0x03/0x04 | fields unknown | `RequestSetSuspendSequence` → `ResponseGeneric` |  |
| Networking | 🔒 Ping (deprecated) | pb | GP-0091 | 0x02/0x00 | fields unknown | — → `ResponseGeneric` |  |
| COHN | Get COHN Status | pb | GP-0076 | 0xF5/0x6F | JSON per schema | `RequestGetCOHNStatus` → `NotifyCOHNStatus` |  |
| COHN | Get COHN Certificate | pb | GP-0076 | 0xF5/0x6E | JSON per schema | `RequestCOHNCert` → `ResponseCOHNCert` |  |
| COHN | Create COHN Certificate | pb | GP-0072 | 0xF1/0x67 | JSON per schema | `RequestCreateCOHNCert` → `ResponseGeneric` |  |
| COHN | ⚠️ Clear COHN Certificate | pb | GP-0072 | 0xF1/0x66 | JSON per schema | `RequestClearCOHNCert` → `ResponseGeneric` |  |
| COHN | Set COHN Setting | pb | GP-0072 | 0xF1/0x65 | JSON per schema | `RequestSetCOHNSetting` → `ResponseGeneric` |  |
| Live Streaming | Get Livestream Status | pb | GP-0076 | 0xF5/0x74 | JSON per schema | `RequestGetLiveStreamStatus` → `NotifyLiveStreamStatus` |  |
| Live Streaming | Set Livestream Mode | pb | GP-0072 | 0xF1/0x79 | JSON per schema | `RequestSetLiveStreamMode` → `ResponseGeneric` |  |

## Appendix C — Protobuf messages the camera sends (feature/action → message)

Use this table for the §4.1 protobuf-vs-TLV decision.

| Feature/Action | Message |
|---|---|
| 0x02/0x0B | `NotifStartScanning` |
| 0x02/0x0C | `NotifProvisioningState` |
| 0x02/0x80 | `ResponseGeneric` |
| 0x02/0x81 | `ResponseCapabilities` |
| 0x02/0x82 | `ResponseStartScanning` |
| 0x02/0x83 | `ResponseGetApEntries` |
| 0x02/0x84 | `ResponseConnect` |
| 0x02/0x85 | `ResponseConnectNew` |
| 0x02/0x86 | `ResponseGeneric` |
| 0x02/0x87 | `ResponseGeneric` |
| 0x02/0x88 | `ResponseTakeOwnership` |
| 0x02/0x8A | `ResponseGeneric` |
| 0x02/0x8D | `ResponseConnectMFi` |
| 0x03/0x81 | `ResponseGeneric` |
| 0x03/0x83 | `ResponseGeneric` |
| 0x03/0x84 | `ResponseGeneric` |
| 0x03/0x85 | `ResponseGeneric` |
| 0x03/0x87 | `ResponseGeneric` |
| 0xF1/0xE0 | `ResponseGeneric` |
| 0xF1/0xE1 | `ResponsePresetDuplicate` |
| 0xF1/0xE2 | `ResponseGeneric` |
| 0xF1/0xE3 | `ResponseGeneric` |
| 0xF1/0xE4 | `ResponseGeneric` |
| 0xF1/0xE5 | `ResponseGeneric` |
| 0xF1/0xE6 | `ResponseGeneric` |
| 0xF1/0xE7 | `ResponseGeneric` |
| 0xF1/0xE9 | `ResponseGeneric` |
| 0xF1/0xEA | `ResponseGeneric` |
| 0xF1/0xEB | `ResponseGeneric` |
| 0xF1/0xEC | `ResponseGeneric` |
| 0xF1/0xED | `ResponseGeneric` |
| 0xF1/0xEE | `ResponseGeneric` |
| 0xF1/0xEF | `ResponseGeneric` |
| 0xF1/0xF1 | `ResponseGeneric` |
| 0xF1/0xF5 | `ResponseGeneric` |
| 0xF1/0xF6 | `ResponseCreateCustomPreset` |
| 0xF1/0xF7 | `ResponseGeneric` |
| 0xF1/0xF8 | `ResponseGeneric` |
| 0xF1/0xF9 | `ResponseGeneric` |
| 0xF1/0xFA | `ResponseGeneric` |
| 0xF1/0xFB | `ResponseGeneric` |
| 0xF1/0xFC | `ResponseGeneric` |
| 0xF1/0xFD | `ResponseGeneric` |
| 0xF5/0xEC | `ResponseGetCameraCapabilities` |
| 0xF5/0xED | `ResponseLastCapturedMedia` |
| 0xF5/0xEE | `ResponseCOHNCert` |
| 0xF5/0xEF | `NotifyCOHNStatus` |
| 0xF5/0xF2 | `NotifyPresetStatus` |
| 0xF5/0xF3 | `NotifyPresetStatus` |
| 0xF5/0xF4 | `NotifyLiveStreamStatus` |
| 0xF5/0xF5 | `NotifyLiveStreamStatus` |

## Appendix D — Settings

Grouped like the debug UI's Settings tab. **Source**: `spec` = Open GoPro spec DB, `GPC (model)` = unofficial, from that camera's gpControl dump. **Width** = value bytes on the wire (§4.4). Camera support lists come from the spec DB; an option can still be unavailable in the current mode (query capabilities, §6.4). `?` in a name means its mode is inferred (§6.2).

### Video

| ID | Setting | Source | Width | Options (value = name) |
|---|---|---|---|---|
| 2 | Video Resolution | spec | 1 | 1=4K, 4=2.7K, 6=2.7K 4:3, 7=1440, 9=1080, 12=720, 18=4K 4:3, 21=5.6K, 24=5K, 25=5K 4:3, 26=5.3K 8:7, 27=5.3K 4:3, 28=4K 8:7, 31=8K, 35=5.3K 21:9, 36=4K 21:9, 37=4K 1:1, 38=900, 39=4K SPH, 40=8K 4:3, 44=1440 4:3, 100=5.3K, 107=5.3K 8:7 V2, 108=4K 8:7 V2, 109=4K 9:16 V2, 110=1080 9:16 V2, 111=2.7K 4:3 V2, 112=4K 4:3 V2, 113=5.3K 4:3 V2 |
| 3 | Frames Per Second | spec | 1 | 0=240.0, 1=120.0, 2=100.0, 3=90.0, 5=60.0, 6=50.0, 8=30.0, 9=25.0, 10=24.0, 13=200.0, 15=400.0, 16=360.0, 17=300.0, 18=480.0, 19=960.0, 20=800.0 |
| 6 | Looping Interval | GPC (HERO11 Black) | 1 | 0=Max, 1=5 Minutes, 2=20 Minutes, 3=60 Minutes, 4=120 Minutes |
| 13 | ISO Max (video) | GPC (HERO11 Black) | 1 | 0=6400, 1=1600, 2=400, 3=3200, 4=800, 7=200, 8=100, 9=Auto |
| 96 | No Audio Track | GPC (HERO9 Black) | 1 | 0=OFF, 1=ON |
| 102 | ISO Min (video) | GPC (HERO11 Black) | 1 | 0=6400, 1=1600, 2=400, 3=3200, 4=800, 7=200, 8=100, 9=Auto |
| 106 | Video Compression | GPC (HERO10 Black) | 1 | 1=HEVC, 2=H.264 + HEVC |
| 108 | Video Aspect Ratio<br><sub>models: HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1, LIT HERO</sub> | spec | 1 | 0=4:3, 1=16:9, 3=8:7, 4=9:16, 5=21:9, 6=1:1 |
| 121 | Video Lens | spec | 1 | 0=Wide, 2=Narrow, 3=Superview, 4=Linear, 7=Max SuperView, 8=Linear + Horizon Leveling, 9=HyperView, 10=Linear + Horizon Lock, 11=Max HyperView, 12=Ultra SuperView, 13=Ultra Wide, 14=Ultra Linear, 104=Ultra HyperView |
| 135 | Hypersmooth<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO11 Black Mini, HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Off, 1=Low, 2=High, 3=Boost, 4=Auto Boost, 100=Standard |
| 145 | Shutter (video) | GPC (HERO11 Black) | 1 | 0=Auto, 3=1/24, 4=1/25, 5=1/30, 6=1/48, 7=1/50, 8=1/60, 11=1/96, 12=1/100, 13=1/120, 16=1/192, 17=1/200, 18=1/240, 21=1/400, 22=1/480, 23=1/960, 24=1/1920, 25=1/384, 28=1/800, 29=1/1600, 30=1/3200, 31=1/3840 |
| 148 | Max HyperSmooth | GPC (HERO11 Black) | 1 | 0=Off, 1=On |
| 150 | Video Horizon Leveling<br><sub>models: HERO11 Black</sub> | spec | 1 | 0=Off, 2=Locked |
| 156 | Video Duration<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 1=15 Seconds, 2=30 Seconds, 3=1 Minute, 4=5 Minutes, 5=15 Minutes, 6=30 Minutes, 7=1 Hour, 8=2 Hours, 9=3 Hours, 10=5 Seconds, 11=10 Seconds, 100=No Limit |
| 162 | Max Lens<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black</sub> | spec | 1 | 0=Off, 1=On |
| 167 | HindSight<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO12 Black, HERO13 Black, Mission 1 Pro, Mission 1</sub> | spec | 1 | 2=15 Seconds, 3=30 Seconds, 4=Off |
| 173 | Video Performance Mode<br><sub>models: HERO10 Black</sub> | spec | 1 | 0=Maximum Video Performance, 1=Extended Battery, 2=Tripod / Stationary Video |
| 176 | Easy Mode Speed<br><sub>models: HERO11 Black, HERO12 Black, HERO13 Black</sub> | spec | 1 | 0=8X Ultra Slo-Mo, 1=4X Super Slo-Mo, 2=2X Slo-Mo, 3=1X Speed (Low Light), 4=4X Super Slo-Mo (Ext. Batt.), 5=2X Slo-Mo (Ext. Batt.), 6=1X Speed (Ext. Batt.) (Low Light), 7=8X Ultra Slo-Mo (50Hz), 8=4X Super Slo-Mo (50Hz), 9=2X Slo-Mo (50Hz), 10=1X Speed (50Hz) (Low Light), 11=4X Super Slo-Mo (50Hz) (Ext. Batt.), 12=2X Slo-Mo (50Hz) (Ext. Batt.), 13=1X Speed (50Hz) (Ext. Batt.) (Low Light), 14=8X Ultra Slo-Mo (Ext. Batt.), 15=8X Ultra Slo-Mo (50Hz) (Ext. Batt.), 16=8X Ultra Slo-Mo (Long. Batt.), 17=4X Super Slo-Mo (Long. Batt.), 18=2X Slo-Mo (Long. Batt.), 19=1X Speed (Long. Batt.) (Low Light), 20=8X Ultra Slo-Mo (50Hz) (Long. Batt.), 21=4X Super Slo-Mo (50Hz) (Long. Batt.), 22=2X Slo-Mo (50Hz) (Long. Batt.), 23=1X Speed (50Hz) (Long. Batt.) (Low Light), 24=2X Slo-Mo (4K), 25=4X Super Slo-Mo (2.7K), 26=2X Slo-Mo (4K) (50Hz), 27=4X Super Slo-Mo (2.7K) (50Hz), 100=8X Ultra Slo-Mo (V2), 101=4X Super Slo-Mo (V2), 102=2X Slo-Mo (V2), 103=1X Speed (Low Light) (V2), 104=8X Ultra Slo-Mo (50Hz) (V2), 105=4X Super Slo-Mo (50Hz) (V2), 106=2X Slo-Mo (50Hz) (V2), 107=1X Speed (50Hz) (Low Light) (V2), 108=8X Ultra Slo-Mo (Long. Batt.) (V2), 109=4X Super Slo-Mo (Long. Batt.) (V2), 110=2X Slo-Mo (Long. Batt.) (V2), 111=1X Speed (Long. Batt.) (Low Light) (V2), 112=8X Ultra Slo-Mo (50Hz) (Long. Batt.) (V2), 113=4X Super Slo-Mo (50Hz) (Long. Batt.) (V2), 114=2X Slo-Mo (50Hz) (Long. Batt.) (V2), 115=1X Speed (50Hz) (Long. Batt.) (Low Light) (V2), 116=2X Slo-Mo (4K) (V2), 117=2X Slo-Mo (4K) (50Hz) (V2), 118=1X Speed (Low Light) (V2) (Vertical), 119=1X Speed (50Hz) (Low Light) (V2) (Vertical), 120=2X Slo-Mo (V2) (Vertical), 121=2X Slo-Mo (50Hz) (V2) (Vertical), 122=1X Speed (Full Frame) (Low Light) (V2), 123=1X Speed (50Hz) (Full Frame) (Low Light) (V2), 124=2X Slo-Mo (Full Frame) (V2), 125=2X Slo-Mo (50Hz) (Full Frame) (V2), 126=1X Speed (4K) (Low Light) (V2), 127=1X Speed (4K) (50Hz) (Low Light) (V2), 128=1X Speed (2.7K) (Low Light) (V2), 129=1X Speed (2.7K) (50Hz) (Low Light) (V2), 130=2X Slo-Mo (2.7K) (V2), 131=2X Slo-Mo (2.7K) (50Hz) (V2), 132=2X Slo-Mo (Long. Batt.) (V2) (Vertical), 133=2X Slo-Mo (50Hz) (Long. Batt.) (V2) (Vertical), 134=1X Speed (Long. Batt.) (Low Light) (V2) (Vertical), 135=1X Speed (50Hz) (Long. Batt.) (Low Light) (V2) (Vertical), 136=1X Speed (4K) (Full Frame) (Low Light) (V2), 137=1X Speed (4K) (50Hz) (Full Frame) (Low Light) (V2), 138=1X Normal Speed (1:1) (30 Fps) (4K) (V2), 139=1X Normal Speed (1:1) (25 Fps) (4K) (V2), 140=2X Slo-Mo Speed (1:1) (4K) (60 Fps) (V2), 141=2X Slo-Mo Speed (1:1) (4K) (50 Fps) (V2), 142=1X Normal Speed (21:9) (30 Fps) (5.3K) (V2), 143=1X Normal Speed (21:9) (25 Fps) (5.3K) (V2), 144=2X Slo-Mo Speed (21:9) (5.3K) (60 Fps) (V2), 145=2X Slo-Mo Speed (21:9) (5.3K) (50 Fps) (V2), 146=1X Normal Speed (21:9) (30 Fps) (4K) (V2), 147=1X Normal Speed (21:9) (25 Fps) (4K) (V2), 148=2X Slo-Mo Speed (21:9) (4K) (60 Fps) (V2), 149=2X Slo-Mo Speed (21:9) (4K) (50 Fps) (V2), 150=120 4X Super Slo-Mo Speed (21:9) (4K) (V2), 151=100 4X Super Slo-Mo Speed (21:9) (4K) (V2), 152=1X Normal Speed (30 Fps) (4:3) (5.3K) (V2), 153=1X Normal Speed (25 Fps) (4:3) (5.3K) (V2), 154=1X Normal Speed (30 Fps) (4:3) (4K) (V2), 155=1X Normal Speed (25 Fps) (4:3) (4K) (V2), 156=2X Slo-Mo Speed (4:3) (4K) (60 Fps) (V2), 157=2X Slo-Mo Speed (4:3) (4K) (50 Fps) (V2), 158=120 4X Super Slo-Mo Speed (2.7K) (4:3) (V2), 159=100 4X Super Slo-Mo Speed (2.7K) (4:3) (V2) |
| 180 | System Video Mode<br><sub>models: HERO11 Black, HERO13 Black</sub> | spec | 1 | 0=Highest Quality, 101=Extended Battery, 102=Longest Battery, 111=Standard Quality, 112=Basic Quality |
| 182 | Video Bit Rate<br><sub>models: HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Standard, 1=High, 2=Max |
| 183 | Bit Depth<br><sub>models: HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=8-Bit, 2=10-Bit |
| 184 | Profiles<br><sub>models: HERO12 Black, HERO13 Black, MAX 2</sub> | spec | 1 | 0=Standard, 1=HDR, 2=Log, 101=HLG HDR |
| 186 | Video Easy Mode<br><sub>models: HERO12 Black, HERO13 Black</sub> | spec | 1 | 0=Highest Quality, 1=Standard Quality, 2=Basic Quality, 3=Standard Video, 4=HDR Video |
| 232 | Video Framing<br><sub>models: HERO13 Black</sub> | spec | 1 | 0=4:3, 1=16:9, 3=8:7, 4=9:16, 5=21:9, 6=1:1 |
| 234 | Frame Rate<br><sub>models: HERO13 Black, MAX 2</sub> | spec | 1 | 0=240.0, 1=120.0, 2=100.0, 3=90.0, 5=60.0, 6=50.0, 8=30.0, 9=25.0, 10=24.0, 13=200.0, 15=400.0, 16=360.0, 17=300.0 |

### Protune (active preset)

| ID | Setting | Source | Width | Options (value = name) |
|---|---|---|---|---|
| 79 | Audio Protune | GPC (HERO9 Black) | 1 | 0=OFF, 1=ON |
| 114 | Protune | GPC (HERO11 Black) | 1 | 0=OFF, 1=ON |
| 115 | White Balance | GPC (HERO11 Black) | 1 | 0=Auto, 2=5500K, 3=6500K, 4=Native, 5=4000K, 7=6000K, 8=2300K, 9=2800K, 10=3200K, 11=4500K, 12=5000K |
| 116 | Color | GPC (HERO11 Black) | 1 | 1=Flat, 2=Natural, 100=Vibrant |
| 117 | Sharpness | GPC (HERO11 Black) | 1 | 0=High, 1=Medium, 2=Low |
| 118 | EV Comp | GPC (HERO11 Black) | 1 | 0=2.0, 1=1.5, 2=1.0, 3=0.5, 4=0.0, 5=-0.5, 6=-1.0, 7=-1.5, 8=-2.0 |
| 124 | Bit Rate | GPC (HERO11 Black) | 1 | 1=High, 100=Standard |
| 139 | RAW Audio | GPC (HERO11 Black) | 1 | 0=Low, 1=Medium, 2=High, 3=Off |
| 149 | Wind | GPC (HERO11 Black) | 1 | 0=Off, 2=Auto, 4=On |
| 160 | Bit Rate (alt, 160) ?<br><sub>second bit-rate setting; mode unknown</sub> | GPC (HERO11 Black) | 1 | 0=Standard, 1=High |
| 174 | 10 Bit | GPC (HERO11 Black) | 1 | 0=Off, 1=On |

### Photo

| ID | Setting | Source | Width | Options (value = name) |
|---|---|---|---|---|
| 19 | Shutter (night photo) | GPC (HERO11 Black) | 1 | 0=Auto, 1=2 Seconds, 2=5 Seconds, 3=10 Seconds, 4=15 Seconds, 5=20 Seconds, 6=30 Seconds |
| 24 | ISO Max (photo) ?<br><sub>mode inferred from legacy ID numbering (photo and multishot pairs have identical options)</sub> | GPC (HERO11 Black) | 1 | 0=800, 1=400, 2=200, 3=100, 4=1600, 5=3200 |
| 75 | ISO Min (photo) ?<br><sub>mode inferred from legacy ID numbering (photo and multishot pairs have identical options)</sub> | GPC (HERO11 Black) | 1 | 0=800, 1=400, 2=200, 3=100, 4=1600, 5=3200 |
| 105 | Timer | GPC (HERO11 Black) | 1 | 0=Off, 1=3 Seconds, 2=10 Seconds |
| 122 | Photo Lens<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Wide 12 MP, 10=Linear 12 MP, 15=9MP Wide, 19=Narrow, 27=Wide 23 MP, 28=Linear 23 MP, 31=Wide 27 MP, 32=Linear 27 MP, 37=9MP Linear, 38=13MP Linear, 39=13MP Wide, 40=13MP Ultra Wide, 41=Ultra Wide 12 MP, 44=13MP Ultra Linear, 100=Max SuperView, 101=Wide, 102=Linear |
| 125 | Photo Output<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Standard, 1=Raw, 2=HDR, 3=SuperPhoto |
| 126 | Output (photo) | GPC (HERO11 Black) | 1 | 0=Standard, 1=Raw |
| 133 | MegaPixels | GPC (HERO9 Black) | 1 | 0=8MP, 1=12MP |
| 146 | Shutter (photo) | GPC (HERO11 Black) | 1 | 0=Auto, 1=1/125, 2=1/250, 3=1/500, 4=1/1000, 5=1/2000 |
| 147 | Burst Rate | GPC (HERO11 Black) | 1 | 0=3 Photos / 1 Second, 1=5 Photos / 1 Second, 2=10 Photos / 1 Second, 4=10 Photos / 3 Seconds, 5=30 Photos / 1 Second, 7=30 Photos / 3 Seconds, 8=30 Photos / 6 Seconds, 9=Auto, 12=60 Photos / 6 Seconds, 13=60 Photos / 10 Seconds |
| 151 | Photo Horizon Leveling<br><sub>models: HERO11 Black</sub> | spec | 1 | 0=Off, 2=Locked |
| 171 | Photo Single Interval<br><sub>models: HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Off, 2=0.5s, 3=1s, 4=2s, 5=5s, 6=10s, 7=30s, 8=60s, 9=120s, 10=3s |
| 177 | Enable Night Photo<br><sub>models: HERO11 Black</sub> | spec | 1 | 0=Off, 1=On |
| 181 | Night Photo | GPC (HERO11 Black) | 1 | 0=Super Photo, 1=Night Photo |
| 191 | Easy Night Photo<br><sub>models: HERO12 Black, HERO13 Black</sub> | spec | 1 | 0=Super Photo, 1=Night Photo, 2=Burst |
| 227 | Photo Mode<br><sub>models: HERO13 Black</sub> | spec | 1 | 0=SuperPhoto, 1=Night Photo, 2=Burst |

### Time lapse & multishot

| ID | Setting | Source | Width | Options (value = name) |
|---|---|---|---|---|
| 5 | Video Timelapse Rate<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO11 Black Mini, HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=0.5 Seconds, 1=1 Second, 2=2 Seconds, 3=5 Seconds, 4=10 Seconds, 5=30 Seconds, 6=60 Seconds, 7=2 Minutes, 8=5 Minutes, 9=30 Minutes, 10=60 Minutes, 11=3 Seconds, 12=4 Seconds, 13=15 Seconds, 14=20 Seconds, 15=Auto |
| 30 | Photo Timelapse Rate<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO12 Black, HERO13 Black, MAX 2</sub> | spec | 8 | 11=3 Seconds, 100=60 Minutes, 101=30 Minutes, 102=5 Minutes, 103=2 Minutes, 104=60 Seconds, 105=30 Seconds, 106=10 Seconds, 107=5 Seconds, 108=2 Seconds, 109=1 Second, 110=0.5 Seconds |
| 31 | Shutter (night lapse) | GPC (HERO11 Black) | 1 | 0=Auto, 1=2 Seconds, 2=5 Seconds, 3=10 Seconds, 4=15 Seconds, 5=20 Seconds, 6=30 Seconds, 7=0.5 Seconds, 8=1 Second |
| 32 | Nightlapse Rate<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO11 Black Mini, HERO12 Black, HERO13 Black, MAX 2</sub> | spec | 8 | 4=4 Seconds, 5=5 Seconds, 10=10 Seconds, 15=15 Seconds, 20=20 Seconds, 30=30 Seconds, 100=60 Seconds, 120=2 Minutes, 300=5 Minutes, 1800=30 Minutes, 3600=60 Minutes, 3601=Auto |
| 37 | ISO Max (multishot / time lapse) ?<br><sub>mode inferred from legacy ID numbering (photo and multishot pairs have identical options)</sub> | GPC (HERO11 Black) | 1 | 0=800, 1=400, 2=200, 3=100, 4=1600, 5=3200 |
| 76 | ISO Min (multishot / time lapse) ?<br><sub>mode inferred from legacy ID numbering (photo and multishot pairs have identical options)</sub> | GPC (HERO11 Black) | 1 | 0=800, 1=400, 2=200, 3=100, 4=1600, 5=3200 |
| 111 | Speed | GPC (HERO11 Black) | 1 | 0=15X, 1=30X, 7=2X, 8=5X, 9=10X, 10=Auto |
| 123 | Time Lapse Digital Lenses<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO12 Black, HERO13 Black, Mission 1 Pro, Mission 1</sub> | spec | 1 | 19=Narrow, 31=Wide 27 MP, 32=Linear 27 MP, 100=Max SuperView, 101=Wide, 102=Linear |
| 128 | Media Format<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO12 Black, HERO13 Black, MAX 2</sub> | spec | 1 | 13=Time Lapse Video, 20=Time Lapse Photo, 21=Night Lapse Photo, 26=Night Lapse Video |
| 155 | Speed Ramp | GPC (HERO11 Black) | 1 | 100=Real Speed, 101=Half Speed |
| 157 | Multi Shot Duration<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Off, 1=15 Seconds, 2=30 Seconds, 3=1 Minute, 4=5 Minutes, 5=15 Minutes, 6=30 Minutes, 7=1 Hour, 8=2 Hours, 9=3 Hours, 100=No Limit |
| 172 | Photo Interval Duration<br><sub>models: HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Off, 1=15 Seconds, 2=30 Seconds, 3=1 Minute, 4=5 Minutes, 5=15 Minutes, 6=30 Minutes, 7=1 Hour, 8=2 Hours, 9=3 Hours |
| 179 | Star Trails Length<br><sub>models: HERO11 Black, HERO11 Black Mini, HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 1=Short, 2=Long, 3=Max |
| 187 | Lapse Mode<br><sub>models: HERO12 Black, HERO13 Black</sub> | spec | 1 | 0=TimeWarp, 1=Star Trails, 2=Light Painting, 3=Vehicle Lights, 4=Max TimeWarp, 5=Max Star Trails, 6=Max Light Painting, 7=Max Vehicle Lights, 8=Time Lapse Video, 9=Night Lapse Video |
| 192 | Multi Shot Aspect Ratio<br><sub>models: HERO12 Black, HERO13 Black, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=4:3, 1=16:9, 3=8:7, 4=9:16 |
| 233 | Multi Shot Framing<br><sub>models: HERO13 Black</sub> | spec | 1 | 0=4:3, 1=16:9, 3=8:7, 4=9:16 |

### Streaming & webcam

| ID | Setting | Source | Width | Options (value = name) |
|---|---|---|---|---|
| 41 | Resolution (stream) | GPC (HERO11 Black) | 1 | 9=1080, 12=720 |
| 42 | Frame Rate (stream) | GPC (HERO11 Black) | 1 | 8=30 |
| 43 | Webcam Digital Lenses<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO11 Black Mini, HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Wide, 2=Narrow, 3=Superview, 4=Linear |
| 44 | BNR Resolution | GPC (HERO11 Black) | 1 | 9=1080 |
| 45 | BNR Frame Per Second | GPC (HERO11 Black) | 1 | 8=30 |
| 47 | Resolution (stream, alt) | GPC (HERO11 Black) | 1 | 7=720, 12=1080 |
| 48 | Privacy | GPC (HERO10 Black) | 1 | 0=Ask, 1=Public, 2=Hidden |
| 60 | Secondary Stream GOP Size | GPC (HERO11 Black) | 1 | 0=Default, 3=3, 4=4, 8=8, 15=15, 30=30 |
| 61 | Secondary Stream IDR Interval | GPC (HERO11 Black) | 1 | 0=Default, 1=1, 2=2, 4=4 |
| 62 | Secondary Stream Bit Rate | GPC (HERO11 Black) | 4 | 250000=250 Kbps, 400000=400 Kbps, 600000=600 Kbps, 700000=700 Kbps, 800000=800 Kbps, 1000000=1 Mbps, 1200000=1.2 Mbps, 1600000=1.6 Mbps, 2000000=2 Mbps, 2400000=2.4 Mbps, 2500000=2.5 Mbps, 4000000=4 Mbps |
| 64 | Secondary Stream Window Size | GPC (HERO11 Black) | 1 | 0=Default, 1=240, 2=240 3:4 Subsample, 3=240 1:2 Subsample, 4=480, 5=480 3:4 Subsample, 6=480 1:2 Subsample, 7=720, 8=720 3:4 Subsample, 9=720 1:2 Subsample, 10=720 Square, 11=480 Square, 12=1080 |
| 65 | GOP Size | GPC (HERO11 Black) | 1 | 0=Default, 3=3, 4=4, 8=8, 15=15, 30=30 |
| 66 | IDR Interval | GPC (HERO11 Black) | 1 | 0=Default, 1=1, 2=2, 4=4 |
| 67 | Bit Rate | GPC (HERO11 Black) | 4 | 250000=250 Kbps, 400000=400 Kbps, 600000=600 Kbps, 700000=700 Kbps, 800000=800 Kbps, 1000000=1 Mbps, 1200000=1.2 Mbps, 1600000=1.6 Mbps, 2000000=2 Mbps, 2400000=2.4 Mbps, 2500000=2.5 Mbps, 4000000=4 Mbps |

### Setup & other

| ID | Setting | Source | Width | Options (value = name) |
|---|---|---|---|---|
| 54 | Quick Capture | GPC (HERO11 Black) | 1 | 0=OFF, 1=ON |
| 59 | Auto Power Down | spec | 1 | 0=Never, 1=1 Min, 4=5 Min, 6=15 Min, 7=30 Min, 11=8 Seconds, 12=30 Seconds |
| 83 | GPS<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Off, 1=On |
| 84 | Language | GPC (HERO11 Black) | 1 | 0=English, 1=Chinese, 2=German, 3=Italian, 4=Spanish, 5=Japanese, 6=French, 7=Korean, 8=Portuguese, 9=Russian, 10=Swedish, 11=Chinese(Traditional) |
| 85 | Voice Control Language | GPC (HERO11 Black) | 1 | 0=English - US, 1=English - UK, 2=English - AUS, 3=German, 4=French, 5=Italian, 6=Spanish, 7=Spanish - NA, 8=Chinese, 9=Japanese, 10=Korean, 11=Portuguese, 12=Russian, 13=English - IND, 14=Swedish |
| 86 | Voice Control Enable | GPC (HERO11 Black) | 1 | 0=OFF, 1=ON |
| 87 | Beeps | GPC (HERO11 Black) | 1 | 0=Mute, 40=Low, 70=Medium, 100=High |
| 88 | LCD Brightness<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1, LIT HERO</sub> | spec | 1 | 10=10%, 20=20%, 30=30%, 40=40%, 50=50%, 60=60%, 70=70%, 80=80%, 90=90%, 100=100% |
| 91 | LED | spec | 1 | 0=Off, 2=On, 3=All On, 4=All Off, 5=Front Off Only, 100=Back Only |
| 103 | Auto Lock | GPC (HERO11 Black) | 1 | 3=Off, 100=On |
| 104 | Wake On Voice | GPC (HERO9 Black) | 1 | 0=OFF, 1=ON |
| 112 | Orientation | GPC (HERO11 Black) | 1 | 100=All, 255=Locked |
| 129 | Lower Left | GPC (HERO11 Black) | 1 | 0=Off, 1=Zoom, 2=Lens, 3=Slo-Mo, 5=White Balance, 6=Shutter, 7=Bit Rate, 8=Burst Rate, 10=Color, 11=EV Comp, 12=HyperSmooth, 13=Interval, 14=ISO Min, 15=ISO Max, 17=Output, 18=Raw Audio, 19=Sharpness, 20=Timer, 21=Speed, 24=Horizon Leveling, 28=10 Bit, 30=Trails Length, 100=Wind, 102=Duration, 103=Speed Ramp, 104=Scheduled Capture, 105=HindSight, 106=Media Mod, 109=Looping Interval |
| 130 | Lower Right | GPC (HERO11 Black) | 1 | 0=Off, 1=Zoom, 2=Lens, 3=Slo-Mo, 5=White Balance, 6=Shutter, 7=Bit Rate, 8=Burst Rate, 10=Color, 11=EV Comp, 12=HyperSmooth, 13=Interval, 14=ISO Min, 15=ISO Max, 17=Output, 18=Raw Audio, 19=Sharpness, 20=Timer, 21=Speed, 24=Horizon Leveling, 28=10 Bit, 30=Trails Length, 100=Wind, 102=Duration, 103=Speed Ramp, 104=Scheduled Capture, 105=HindSight, 106=Media Mod, 109=Looping Interval |
| 131 | Upper Left | GPC (HERO11 Black) | 1 | 0=Off, 1=Zoom, 2=Lens, 3=Slo-Mo, 5=White Balance, 6=Shutter, 7=Bit Rate, 8=Burst Rate, 10=Color, 11=EV Comp, 12=HyperSmooth, 13=Interval, 14=ISO Min, 15=ISO Max, 17=Output, 18=Raw Audio, 19=Sharpness, 20=Timer, 21=Speed, 24=Horizon Leveling, 28=10 Bit, 30=Trails Length, 100=Wind, 102=Duration, 103=Speed Ramp, 104=Scheduled Capture, 105=HindSight, 106=Media Mod, 109=Looping Interval |
| 132 | Upper Right | GPC (HERO11 Black) | 1 | 0=Off, 1=Zoom, 2=Lens, 3=Slo-Mo, 5=White Balance, 6=Shutter, 7=Bit Rate, 8=Burst Rate, 10=Color, 11=EV Comp, 12=HyperSmooth, 13=Interval, 14=ISO Min, 15=ISO Max, 17=Output, 18=Raw Audio, 19=Sharpness, 20=Timer, 21=Speed, 24=Horizon Leveling, 28=10 Bit, 30=Trails Length, 100=Wind, 102=Duration, 103=Speed Ramp, 104=Scheduled Capture, 105=HindSight, 106=Media Mod, 109=Looping Interval |
| 134 | Anti-Flicker | spec | 1 | 0=NTSC, 1=PAL, 2=60Hz, 3=50Hz |
| 144 | Mode | GPC (HERO11 Black) | 1 | 12=Video, 13=Time Lapse Video, 15=Looping, 16=Single Photo, 18=Night Photo, 19=Burst Photo, 20=Time Lapse Photo, 21=Night Lapse Photo, 24=Time Warp Video, 26=Night Lapse Video |
| 153 | Mode Button | GPC (HERO11 Black) | 1 | 1=HiLight, 100=Speed Ramp |
| 154 | Front LCD Mode | GPC (HERO11 Black) | 1 | 0=Screen Off, 1=Status Only, 2=Actual View, 3=Full Screen |
| 158 | Screen Saver Front | GPC (HERO11 Black) | 1 | 0=Never, 1=Match Rear Screen, 2=1 Min, 3=2 Min, 4=3 Min, 5=5 Min |
| 159 | Screen Saver Rear | GPC (HERO11 Black) | 1 | 0=Never, 1=1 Min, 2=2 Min, 3=3 Min, 4=5 Min |
| 161 | Default Preset | GPC (HERO11 Black) | 1 | 100=Last Used Video, 101=Last Used Photo, 102=Last Used Time Lapse, 200=Last Used |
| 163 | Max Lens Mod | GPC (HERO11 Black) | 1 | 0=OFF, 1=ON |
| 164 | Media Mod | GPC (HERO11 Black) | 1 | 1=Front, 2=Back, 100=Camera Mics |
| 165 | Horizon Lock (165) ?<br><sub>two Horizon Lock IDs exist; which mode each controls is unknown</sub> | GPC (HERO10 Black) | 1 | 0=Off, 1=On |
| 166 | Horizon Lock (166) ?<br><sub>two Horizon Lock IDs exist; which mode each controls is unknown</sub> | GPC (HERO10 Black) | 1 | 0=Off, 1=On |
| 168 | Scheduled Capture<br><sub>models: HERO9 Black, HERO10 Black, HERO11 Black, HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 8 | 0=Off |
| 169 | Mods | GPC (HERO11 Black) | 1 | 1=Standard Mic, 2=Standard Mic+, 3=Powered Mic, 4=Powered Mic+, 5=Line In |
| 175 | Control Mode<br><sub>models: HERO11 Black, HERO12 Black, HERO13 Black</sub> | spec | 1 | 0=Easy, 1=Pro |
| 178 | Wireless Band<br><sub>models: HERO11 Black, HERO11 Black Mini, HERO12 Black, HERO13 Black, MAX 2, Mission 1 Pro, Mission 1, LIT HERO</sub> | spec | 1 | 0=2.4GHz, 1=5GHz |
| 189 | Max Lens Mod<br><sub>models: HERO12 Black, HERO13 Black, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=None, 1=Max Lens 1.0, 2=Max Lens 2.0, 3=Max Lens 2.5, 4=Macro, 5=Anamorphic, 6=ND 4, 7=ND 8, 8=ND 16, 9=ND 32, 10=Standard Lens, 11=ND 64, 100=Auto Detect |
| 190 | Max Lens Mod Enable<br><sub>models: HERO12 Black</sub> | spec | 1 | 0=Off, 1=On |
| 193 | Framing<br><sub>models: HERO12 Black, HERO13 Black</sub> | spec | 1 | 0=Widescreen, 1=Vertical, 2=Full Frame, 100=Traditional 4:3 v2, 101=Widescreen 16:9 v2, 103=Full Frame 8:7 v2, 104=Vertical 9:16 v2, 105=Ultra Widescreen 21:9 v2, 106=Full Frame 1:1 v2 |
| 194 | Camera Mode<br><sub>models: MAX 2</sub> | spec | 1 | 0=Single Lens, 1=360° |
| 196 | 360 Photo Files Extension<br><sub>models: MAX 2</sub> | spec | 1 | 0=.JPG, 1=.36P |
| 216 | Beep Volume<br><sub>models: HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 70=Low, 85=Medium, 100=High |
| 219 | Setup Screen Saver<br><sub>models: HERO13 Black, MAX 2</sub> | spec | 1 | 0=Never, 1=1 Min, 2=2 Min, 3=3 Min, 4=5 Min |
| 223 | Setup Language<br><sub>models: HERO13 Black, MAX 2, Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=English - US, 1=English - UK, 2=English - AUS, 3=German, 4=French, 5=Italian, 6=Spanish, 7=Spanish - NA, 8=Chinese, 9=Japanese, 10=Korean, 11=Portuguese, 12=Russian, 13=English - IND, 14=Swedish |
| 236 | Automatic Wi-Fi Access Point<br><sub>models: MAX 2</sub> | spec | 1 | 0=Off, 1=On |
| 237 | Auto Power On USB<br><sub>models: LIT HERO</sub> | spec | 1 | 0=Off, 1=On |
| 239 | General Sub Mode<br><sub>models: Mission 1 Pro, Mission 1</sub> | spec | 1 | 12=Standard, 13=Stationary, 15=Looping, 16=Photo, 19=Burst, 24=Motion, 27=Slo-Mo, 29=Star Trails, 30=Light Painting, 31=Vehicle Lights, 32=Burst Slo-Mo, 36=Low Light, 100=Interval |
| 256 | Image Tuning<br><sub>models: Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Dive, 1=Face, 2=Sport, 3=Balanced |
| 257 | Subject Tracking<br><sub>models: Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Off, 1=On |
| 273 | Output<br><sub>models: Mission 1 Pro, Mission 1</sub> | spec | 1 | 0=Video, 1=Video + JPG, 2=Video + JPG + RAW |


## Appendix E — Statuses

Values are big-endian; widths vary (§4.6). `-1` arrives as `0xFF`.

| ID | Status | Source | Values |
|---|---|---|---|
| 1 | Battery present | spec | — |
| 2 | Internal battery bars | spec | 0=Zero, 1=One, 2=Two, 3=Three, 4=Charging |
| 3 | External battery present | GPC (HERO11 Black) | — |
| 4 | External battery level | GPC (HERO11 Black) | — |
| 6 | Overheating | spec | — |
| 8 | Busy | spec | — |
| 9 | Quick capture | spec | — |
| 10 | Encoding | spec | — |
| 11 | Lcd lock | spec | — |
| 13 | Video encoding duration | spec | — |
| 14 | Broadcast progress counter | GPC (HERO11 Black) | — |
| 17 | Wireless connections enabled | spec | — |
| 19 | Pairing state | spec | 0=Never started, 1=Started, 2=Aborted, 3=Cancelled, 4=Completed |
| 20 | Last pairing type | spec | 0=Not pairing, 1=Pairing app, 2=Pairing remote control, 3=Pairing bluetooth device |
| 21 | Last pairing success | spec | — |
| 22 | Wifi scan state | spec | 0=Never started, 1=Started, 2=Aborted, 3=Canceled, 4=Completed |
| 23 | Last wifi scan success | spec | — |
| 24 | Wifi provisioning state | spec | 0=Never started, 1=Started, 2=Aborted, 3=Canceled, 4=Completed |
| 26 | Remote version | spec | — |
| 27 | Remote connected | spec | — |
| 28 | Pairing State (Legacy) | spec | — |
| 29 | Connected wifi ssid | spec | — |
| 30 | Access point ssid | spec | — |
| 31 | Connected devices | spec | — |
| 32 | Preview stream | spec | — |
| 33 | Primary storage | spec | -1=Unknown, 0=Ok, 1=Sd card full, 2=Sd card removed, 3=Sd card format error, 4=Sd card busy, 8=Sd card swapped |
| 34 | Remaining photos | spec | — |
| 35 | Remaining video time | spec | — |
| 36 | Num group photos | GPC (HERO11 Black) | — |
| 37 | Num group videos | GPC (HERO11 Black) | — |
| 38 | Photos | spec | — |
| 39 | Videos | spec | — |
| 40 | Date time | GPC (HERO9 Black) | — |
| 41 | Ota | spec | 0=Idle, 1=Downloading, 2=Verifying, 3=Download failed, 4=Verify failed, 5=Ready, 6=Gopro app downloading, 7=Gopro app verifying, 8=Gopro app download failed, 9=Gopro app verify failed, 10=Gopro app ready |
| 42 | Pending fw update cancel | spec | — |
| 45 | Locate | spec | — |
| 49 | Timelapse interval countdown | spec | — |
| 54 | Sd card remaining | spec | — |
| 55 | Preview stream available | spec | — |
| 56 | Wifi bars | spec | — |
| 57 | Current time msec | GPC (HERO9 Black) | — |
| 58 | Active hilights | spec | — |
| 59 | Time since last hilight | spec | — |
| 60 | Minimum status poll period | spec | — |
| 61 | Analytics ready | GPC (HERO11 Black) | — |
| 62 | Analytics size | GPC (HERO11 Black) | — |
| 63 | In contextual menu | GPC (HERO11 Black) | — |
| 64 | Remaining timelapse time | GPC (HERO11 Black) | — |
| 65 | Liveview exposure select mode | spec | 0=Disabled, 1=Auto, 2=Iso lock, 3=Hemisphere |
| 66 | Liveview y | spec | — |
| 67 | Liveview x | spec | — |
| 68 | Gps lock | spec | — |
| 69 | Ap mode | spec | — |
| 70 | Internal battery percentage | spec | — |
| 74 | Microphone accessory | spec | 0=Accessory not connected, 1=Accessory connected, 2=Accessory connected and a microphone is plugged into the accessory |
| 75 | Zoom level | spec | — |
| 76 | Wireless band | spec | 0=2 4 ghz, 1=5 ghz |
| 77 | Zoom available | spec | — |
| 78 | Mobile friendly | spec | — |
| 79 | Ftu | spec | — |
| 81 | 5ghz available | spec | — |
| 82 | Ready | spec | — |
| 83 | Ota charged | spec | — |
| 85 | Cold | spec | — |
| 86 | Rotation | spec | 0=0 degrees upright, 1=180 degrees upside down, 2=90 degrees laying on right side, 3=270 degrees laying on left side |
| 88 | Zoom while encoding | spec | — |
| 89 | Flatmode | spec | — |
| 90 | Protune default | GPC (HERO11 Black) | — |
| 91 | Logs ready | GPC (HERO11 Black) | — |
| 93 | Video preset | spec | — |
| 94 | Photo preset | spec | — |
| 95 | Timelapse preset | spec | — |
| 96 | Preset group | spec | — |
| 97 | Preset | spec | — |
| 98 | Preset modified | spec | — |
| 99 | Remaining live bursts | spec | — |
| 100 | Live bursts | spec | — |
| 101 | Capture delay active | spec | — |
| 102 | Media mod state | spec | 0=Microphone removed, 2=Microphone only, 3=Microphone with external microphone |
| 103 | Time warp speed | spec | 0=15x, 1=30x, 2=60x, 3=150x, 4=300x, 5=900x, 6=1800x, 7=2x, 8=5x, 9=10x, 10=Auto, 11=1x realtime, 12=1 2x slow motion |
| 104 | Linux core | spec | — |
| 105 | Lens type | spec | 0=Default, 1=Max lens, 2=Max lens 2 0, 3=Max lens 2 5, 4=Macro lens, 5=Anamorphic lens, 6=Neutral density 4, 7=Neutral density 8, 8=Neutral density 16, 9=Neutral density 32 |
| 106 | Hindsight | spec | — |
| 107 | Scheduled capture preset id | spec | — |
| 108 | Scheduled capture | spec | — |
| 109 | Creating preset | GPC (HERO11 Black) | — |
| 110 | Display mod status | spec | 0=000 display mod 0 hdmi 0 display mod connected false, 1=001 display mod 0 hdmi 0 display mod connected true, 2=010 display mod 0 hdmi 1 display mod connected false, 3=011 display mod 0 hdmi 1 display mod connected true, 4=100 display mod 1 hdmi 0 display mod connected false, 5=101 display mod 1 hdmi 0 display mod connected true, 6=110 display mod 1 hdmi 1 display mod connected false, 7=111 display mod 1 hdmi 1 display mod connected true |
| 111 | Sd card write speed error | spec | — |
| 112 | Sd card errors | spec | — |
| 113 | Turbo transfer | spec | — |
| 114 | Camera control id | spec | 0=Camera idle no one is attempting to change camera settings, 1=Camera control camera is in a menu or changing settings to intervene app must request control, 2=Camera external control an outside entity app has control and is in a menu or modifying settings |
| 115 | Usb connected | spec | — |
| 116 | Usb controlled | spec | 0=Disabled, 1=Enabled |
| 117 | Sd card capacity | spec | — |
| 118 | Photo interval capture count | spec | — |
| 122 | Camera name | spec | — |
| 123 | Active Preset Is Custom | spec | — |

## Appendix F — Protobuf schema (public `.proto` files, proto2)

Field lines: `number name label type`. Enums are listed below the messages.

**`Media`**
```
  1  folder  optional  string
  2  file  optional  string
```
**`NotifProvisioningState`**
```
  1  provisioning_state  required  EnumProvisioning
```
**`NotifStartScanning`**
```
  1  scanning_state  required  EnumScanning
  2  scan_id  optional  int32
  3  total_entries  optional  int32
  4  total_configured_ssid  required  int32
```
**`NotifyCOHNStatus`**
```
  1  status  optional  EnumCOHNStatus
  2  state  optional  EnumCOHNNetworkState
  3  username  optional  string
  4  password  optional  string
  5  ipaddress  optional  string
  6  enabled  optional  bool
  7  ssid  optional  string
  8  macaddress  optional  string
```
**`NotifyLiveStreamStatus`**
```
  1  live_stream_status  optional  EnumLiveStreamStatus
  2  live_stream_error  optional  EnumLiveStreamError
  3  live_stream_encode  optional  bool
  4  live_stream_bitrate  optional  int32
  5  live_stream_window_size_supported_array  repeated  EnumWindowSize
  6  live_stream_encode_supported  optional  bool
  7  live_stream_max_lens_unsupported  optional  bool
  8  live_stream_minimum_stream_bitrate  optional  int32
  9  live_stream_maximum_stream_bitrate  optional  int32
 10  live_stream_lens_supported  optional  bool
 11  live_stream_lens_supported_array  repeated  EnumLens
 13  live_stream_protune_supported  optional  bool
```
**`NotifyPresetStatus`**
```
  1  preset_group_array  repeated  PresetGroup
```
**`Preset`**
```
  1  id  optional  int32
  2  mode  optional  EnumFlatMode
  3  title_id  optional  EnumPresetTitle
  4  title_number  optional  int32
  5  user_defined  optional  bool
  6  icon  optional  EnumPresetIcon
  7  setting_array  repeated  PresetSetting
  8  is_modified  optional  bool
  9  is_fixed  optional  bool
 10  custom_name  optional  string
 11  is_visible  optional  bool
```
**`PresetGroup`**
```
  1  id  optional  EnumPresetGroup
  2  preset_array  repeated  Preset
  3  can_add_preset  optional  bool
  4  icon  optional  EnumPresetGroupIcon
  5  mode_array  repeated  EnumFlatMode
  6  custom_name  optional  string
  7  active_preset_id  optional  int32
```
**`PresetSetting`**
```
  1  id  optional  int32
  2  value  optional  int32
  3  is_caption  optional  bool
```
**`ProtocolCapabilities`**
```
  1  supports_2byte_ids  optional  bool
```
**`RequestCOHNCert`**  _(empty)_
**`RequestClearCOHNCert`**  _(empty)_
**`RequestConnect`**
```
  1  ssid  required  string
```
**`RequestConnectNew`**
```
  1  ssid  required  string
  2  password  required  string
  3  static_ip  optional  bytes
  4  gateway  optional  bytes
  5  subnet  optional  bytes
  6  dns_primary  optional  bytes
  7  dns_secondary  optional  bytes
 10  bypass_eula_check  optional  bool
```
**`RequestCreateCOHNCert`**
```
  1  override  optional  bool
```
**`RequestCustomPresetUpdate`**
```
  1  title_id  optional  EnumPresetTitle
  2  custom_name  optional  string
  3  icon_id  optional  EnumPresetIcon
```
**`RequestGetApEntries`**
```
  1  start_index  required  int32
  2  max_entries  required  int32
  3  scan_id  required  int32
```
**`RequestGetCOHNStatus`**
```
  1  register_cohn_status  optional  bool
```
**`RequestGetCameraCapabilities`**  _(empty)_
**`RequestGetLastCapturedMedia`**  _(empty)_
**`RequestGetLiveStreamStatus`**
```
  1  register_live_stream_status  repeated  EnumRegisterLiveStreamStatus
  2  unregister_live_stream_status  repeated  EnumRegisterLiveStreamStatus
```
**`RequestGetPresetStatus`**
```
  1  register_preset_status  repeated  EnumRegisterPresetStatus
  2  unregister_preset_status  repeated  EnumRegisterPresetStatus
  3  use_constant_setting_ids  optional  bool
  4  include_hidden  optional  bool
```
**`RequestPairingFinish`**
```
  1  result  required  EnumPairingFinishState
  2  phoneName  required  string
```
**`RequestPresetSetVisibility`**
```
  1  id  optional  int32
  2  visible  optional  bool
```
**`RequestReleaseNetwork`**  _(empty)_
**`RequestSetCOHNSetting`**
```
  1  cohn_active  optional  bool
```
**`RequestSetCameraControlStatus`**
```
  1  camera_control_status  required  EnumCameraControlStatus
```
**`RequestSetCameraName`**
```
  1  name  optional  string
```
**`RequestSetLiveStreamMode`**
```
  1  url  optional  string
  2  encode  optional  bool
  3  window_size  optional  EnumWindowSize
  6  cert  optional  bytes
  7  minimum_bitrate  optional  int32
  8  maximum_bitrate  optional  int32
  9  starting_bitrate  optional  int32
 10  lens  optional  EnumLens
```
**`RequestSetTurboActive`**
```
  1  active  required  bool
```
**`RequestStartScan`**  _(empty)_
**`ResponseCOHNCert`**
```
  1  result  optional  EnumResultGeneric
  2  cert  optional  string
```
**`ResponseConnect`**
```
  1  result  required  EnumResultGeneric
  2  provisioning_state  required  EnumProvisioning
  3  timeout_seconds  required  int32
```
**`ResponseConnectNew`**
```
  1  result  required  EnumResultGeneric
  2  provisioning_state  required  EnumProvisioning
  3  timeout_seconds  required  int32
```
**`ResponseGeneric`**
```
  1  result  required  EnumResultGeneric
```
**`ResponseGetApEntries`**
```
  1  result  required  EnumResultGeneric
  2  scan_id  required  int32
  3  entries  repeated  ScanEntry
```
**`ResponseGetCameraCapabilities`**
```
  1  result  required  EnumResultGeneric
  2  schema_version  required  string
  3  protocol  optional  ProtocolCapabilities
```
**`ResponseLastCapturedMedia`**
```
  1  result  optional  EnumResultGeneric
  2  media  optional  Media
```
**`ResponseStartScanning`**
```
  1  result  required  EnumResultGeneric
  2  scanning_state  required  EnumScanning
```
**`ScanEntry`**
```
  1  ssid  required  string
  2  signal_strength_bars  required  int32
  4  signal_frequency_mhz  required  int32
  5  scan_entry_flags  required  int32
```

### Enums

| Enum | Values |
|---|---|
| `EnumCOHNNetworkState` | COHN_STATE_Init=0, COHN_STATE_Error=1, COHN_STATE_Exit=2, COHN_STATE_Idle=5, COHN_STATE_NetworkConnected=27, COHN_STATE_NetworkDisconnected=28, COHN_STATE_ConnectingToNetwork=29, COHN_STATE_Invalid=30 |
| `EnumCOHNStatus` | COHN_UNPROVISIONED=0, COHN_PROVISIONED=1 |
| `EnumCameraControlStatus` | CAMERA_IDLE=0, CAMERA_CONTROL=1, CAMERA_EXTERNAL_CONTROL=2, CAMERA_COF_SETUP=3 |
| `EnumFlatMode` | FLAT_MODE_UNKNOWN=-1, FLAT_MODE_PLAYBACK=4, FLAT_MODE_SETUP=5, FLAT_MODE_VIDEO=12, FLAT_MODE_TIME_LAPSE_VIDEO=13, FLAT_MODE_LOOPING=15, FLAT_MODE_PHOTO_SINGLE=16, FLAT_MODE_PHOTO=17, FLAT_MODE_PHOTO_NIGHT=18, FLAT_MODE_PHOTO_BURST=19, FLAT_MODE_TIME_LAPSE_PHOTO=20, FLAT_MODE_NIGHT_LAPSE_PHOTO=21, FLAT_MODE_BROADCAST_RECORD=22, FLAT_MODE_BROADCAST_BROADCAST=23, FLAT_MODE_TIME_WARP_VIDEO=24, FLAT_MODE_LIVE_BURST=25, FLAT_MODE_NIGHT_LAPSE_VIDEO=26, FLAT_MODE_SLOMO=27, FLAT_MODE_IDLE=28, FLAT_MODE_VIDEO_STAR_TRAIL=29, FLAT_MODE_VIDEO_LIGHT_PAINTING=30, FLAT_MODE_VIDEO_LIGHT_TRAIL=31, FLAT_MODE_VIDEO_BURST_SLOMO=32 |
| `EnumLens` | LENS_WIDE=0, LENS_SUPERVIEW=3, LENS_LINEAR=4 |
| `EnumLiveStreamError` | LIVE_STREAM_ERROR_NONE=0, LIVE_STREAM_ERROR_NETWORK=1, LIVE_STREAM_ERROR_CREATESTREAM=2, LIVE_STREAM_ERROR_OUTOFMEMORY=3, LIVE_STREAM_ERROR_INPUTSTREAM=4, LIVE_STREAM_ERROR_INTERNET=5, LIVE_STREAM_ERROR_OSNETWORK=6, LIVE_STREAM_ERROR_SELECTEDNETWORKTIMEOUT=7, LIVE_STREAM_ERROR_SSL_HANDSHAKE=8, LIVE_STREAM_ERROR_CAMERA_BLOCKED=9, LIVE_STREAM_ERROR_UNKNOWN=10, LIVE_STREAM_ERROR_SD_CARD_FULL=40, LIVE_STREAM_ERROR_SD_CARD_REMOVED=41 |
| `EnumLiveStreamStatus` | LIVE_STREAM_STATE_IDLE=0, LIVE_STREAM_STATE_CONFIG=1, LIVE_STREAM_STATE_READY=2, LIVE_STREAM_STATE_STREAMING=3, LIVE_STREAM_STATE_COMPLETE_STAY_ON=4, LIVE_STREAM_STATE_FAILED_STAY_ON=5, LIVE_STREAM_STATE_RECONNECTING=6, LIVE_STREAM_STATE_UNAVAILABLE=7 |
| `EnumPairingFinishState` | SUCCESS=0, FAILED=1 |
| `EnumPresetGroup` | PRESET_GROUP_ID_VIDEO=1000, PRESET_GROUP_ID_PHOTO=1001, PRESET_GROUP_ID_TIMELAPSE=1002, PRESET_GROUP_ID_GENERAL=2000 |
| `EnumPresetGroupIcon` | PRESET_GROUP_VIDEO_ICON_ID=0, PRESET_GROUP_PHOTO_ICON_ID=1, PRESET_GROUP_TIMELAPSE_ICON_ID=2, PRESET_GROUP_LONG_BAT_VIDEO_ICON_ID=3, PRESET_GROUP_ENDURANCE_VIDEO_ICON_ID=4, PRESET_GROUP_MAX_VIDEO_ICON_ID=5, PRESET_GROUP_MAX_PHOTO_ICON_ID=6, PRESET_GROUP_MAX_TIMELAPSE_ICON_ID=7, PRESET_GROUP_ND_MOD_VIDEO_ICON_ID=8, PRESET_GROUP_ND_MOD_PHOTO_ICON_ID=9, PRESET_GROUP_ND_MOD_TIMELAPSE_ICON_ID=10 |
| `EnumPresetIcon` | PRESET_ICON_VIDEO=0, PRESET_ICON_ACTIVITY=1, PRESET_ICON_CINEMATIC=2, PRESET_ICON_PHOTO=3, PRESET_ICON_LIVE_BURST=4, PRESET_ICON_BURST=5, PRESET_ICON_PHOTO_NIGHT=6, PRESET_ICON_TIMEWARP=7, PRESET_ICON_TIMELAPSE=8, PRESET_ICON_NIGHTLAPSE=9, PRESET_ICON_SNAIL=10, PRESET_ICON_VIDEO_2=11, PRESET_ICON_PHOTO_2=13, PRESET_ICON_PANORAMA=14, PRESET_ICON_BURST_2=15, PRESET_ICON_TIMEWARP_2=16, PRESET_ICON_TIMELAPSE_2=17, PRESET_ICON_CUSTOM=18, PRESET_ICON_AIR=19, PRESET_ICON_BIKE=20, PRESET_ICON_EPIC=21, PRESET_ICON_INDOOR=22, PRESET_ICON_MOTOR=23, PRESET_ICON_MOUNTED=24, PRESET_ICON_OUTDOOR=25, PRESET_ICON_POV=26, PRESET_ICON_SELFIE=27, PRESET_ICON_SKATE=28, PRESET_ICON_SNOW=29, PRESET_ICON_TRAIL=30, PRESET_ICON_TRAVEL=31, PRESET_ICON_WATER=32, PRESET_ICON_LOOPING=33, PRESET_ICON_STARS=34, PRESET_ICON_ACTION=35, PRESET_ICON_FOLLOW_CAM=36, PRESET_ICON_SURF=37, PRESET_ICON_CITY=38, PRESET_ICON_SHAKY=39, PRESET_ICON_CHESTY=40, PRESET_ICON_HELMET=41, PRESET_ICON_BITE=42, PRESET_ICON_CUSTOM_CINEMATIC=43, PRESET_ICON_VLOG=44, PRESET_ICON_FPV=45, PRESET_ICON_HDR=46, PRESET_ICON_LANDSCAPE=47, PRESET_ICON_LOG=48, PRESET_ICON_CUSTOM_SLOMO=49, PRESET_ICON_TRIPOD=50, PRESET_ICON_MAX_VIDEO=55, PRESET_ICON_MAX_PHOTO=56, PRESET_ICON_MAX_TIMEWARP=57, PRESET_ICON_BASIC=58, PRESET_ICON_ULTRA_SLO_MO=59, PRESET_ICON_STANDARD_ENDURANCE=60, PRESET_ICON_ACTIVITY_ENDURANCE=61, PRESET_ICON_CINEMATIC_ENDURANCE=62, PRESET_ICON_SLOMO_ENDURANCE=63, PRESET_ICON_STATIONARY_1=64, PRESET_ICON_STATIONARY_2=65, PRESET_ICON_STATIONARY_3=66, PRESET_ICON_STATIONARY_4=67, PRESET_ICON_SIMPLE_SUPER_PHOTO=70, PRESET_ICON_SIMPLE_NIGHT_PHOTO=71, PRESET_ICON_HIGHEST_QUALITY_VIDEO=73, PRESET_ICON_STANDARD_QUALITY_VIDEO=74, PRESET_ICON_BASIC_QUALITY_VIDEO=75, PRESET_ICON_STAR_TRAIL=76, PRESET_ICON_LIGHT_PAINTING=77, PRESET_ICON_LIGHT_TRAIL=78, PRESET_ICON_FULL_FRAME=79, PRESET_ICON_EASY_MAX_VIDEO=80, PRESET_ICON_EASY_MAX_PHOTO=81, PRESET_ICON_EASY_MAX_TIMEWARP=82, PRESET_ICON_EASY_MAX_STAR_TRAIL=83, PRESET_ICON_EASY_MAX_LIGHT_PAINTING=84, PRESET_ICON_EASY_MAX_LIGHT_TRAIL=85, PRESET_ICON_MAX_STAR_TRAIL=89, PRESET_ICON_MAX_LIGHT_PAINTING=90, PRESET_ICON_MAX_LIGHT_TRAIL=91, PRESET_ICON_EASY_STANDARD_PROFILE=100, PRESET_ICON_EASY_HDR_PROFILE=101, PRESET_ICON_BURST_SLOMO=102, PRESET_ICON_TIMELAPSE_PHOTO=1000, PRESET_ICON_NIGHTLAPSE_PHOTO=1001 |
| `EnumPresetTitle` | PRESET_TITLE_ACTIVITY=0, PRESET_TITLE_STANDARD=1, PRESET_TITLE_CINEMATIC=2, PRESET_TITLE_PHOTO=3, PRESET_TITLE_LIVE_BURST=4, PRESET_TITLE_BURST=5, PRESET_TITLE_NIGHT=6, PRESET_TITLE_TIME_WARP=7, PRESET_TITLE_TIME_LAPSE=8, PRESET_TITLE_NIGHT_LAPSE=9, PRESET_TITLE_VIDEO=10, PRESET_TITLE_SLOMO=11, PRESET_TITLE_PHOTO_2=13, PRESET_TITLE_PANORAMA=14, PRESET_TITLE_TIME_WARP_2=16, PRESET_TITLE_CUSTOM=18, PRESET_TITLE_AIR=19, PRESET_TITLE_BIKE=20, PRESET_TITLE_EPIC=21, PRESET_TITLE_INDOOR=22, PRESET_TITLE_MOTOR=23, PRESET_TITLE_MOUNTED=24, PRESET_TITLE_OUTDOOR=25, PRESET_TITLE_POV=26, PRESET_TITLE_SELFIE=27, PRESET_TITLE_SKATE=28, PRESET_TITLE_SNOW=29, PRESET_TITLE_TRAIL=30, PRESET_TITLE_TRAVEL=31, PRESET_TITLE_WATER=32, PRESET_TITLE_LOOPING=33, PRESET_TITLE_STARS=34, PRESET_TITLE_ACTION=35, PRESET_TITLE_FOLLOW_CAM=36, PRESET_TITLE_SURF=37, PRESET_TITLE_CITY=38, PRESET_TITLE_SHAKY=39, PRESET_TITLE_CHESTY=40, PRESET_TITLE_HELMET=41, PRESET_TITLE_BITE=42, PRESET_TITLE_CUSTOM_CINEMATIC=43, PRESET_TITLE_VLOG=44, PRESET_TITLE_FPV=45, PRESET_TITLE_HDR=46, PRESET_TITLE_LANDSCAPE=47, PRESET_TITLE_LOG=48, PRESET_TITLE_CUSTOM_SLOMO=49, PRESET_TITLE_TRIPOD=50, PRESET_TITLE_BASIC=58, PRESET_TITLE_ULTRA_SLO_MO=59, PRESET_TITLE_STANDARD_ENDURANCE=60, PRESET_TITLE_ACTIVITY_ENDURANCE=61, PRESET_TITLE_CINEMATIC_ENDURANCE=62, PRESET_TITLE_SLOMO_ENDURANCE=63, PRESET_TITLE_STATIONARY_1=64, PRESET_TITLE_STATIONARY_2=65, PRESET_TITLE_STATIONARY_3=66, PRESET_TITLE_STATIONARY_4=67, PRESET_TITLE_SIMPLE_VIDEO=68, PRESET_TITLE_SIMPLE_TIME_WARP=69, PRESET_TITLE_SIMPLE_SUPER_PHOTO=70, PRESET_TITLE_SIMPLE_NIGHT_PHOTO=71, PRESET_TITLE_SIMPLE_VIDEO_ENDURANCE=72, PRESET_TITLE_HIGHEST_QUALITY=73, PRESET_TITLE_EXTENDED_BATTERY=74, PRESET_TITLE_LONGEST_BATTERY=75, PRESET_TITLE_STAR_TRAIL=76, PRESET_TITLE_LIGHT_PAINTING=77, PRESET_TITLE_LIGHT_TRAIL=78, PRESET_TITLE_FULL_FRAME=79, PRESET_TITLE_STANDARD_QUALITY_VIDEO=82, PRESET_TITLE_BASIC_QUALITY_VIDEO=83, PRESET_TITLE_HIGHEST_QUALITY_VIDEO=93, PRESET_TITLE_USER_DEFINED_CUSTOM_NAME=94, PRESET_TITLE_EASY_STANDARD_PROFILE=99, PRESET_TITLE_EASY_HDR_PROFILE=100, PRESET_TITLE_BURST_SLOMO=106, PRESET_TITLE_4_3_VIDEO=125, PRESET_TITLE_16_9_VIDEO=126, PRESET_TITLE_16_9_SLOMO=127, PRESET_TITLE_TIME_LAPSE_VIDEO=131, PRESET_TITLE_TIME_LAPSE_PHOTO=132, PRESET_TITLE_NIGHT_LAPSE_VIDEO=133, PRESET_TITLE_NIGHT_LAPSE_PHOTO=134 |
| `EnumProvisioning` | PROVISIONING_UNKNOWN=0, PROVISIONING_NEVER_STARTED=1, PROVISIONING_STARTED=2, PROVISIONING_ABORTED_BY_SYSTEM=3, PROVISIONING_CANCELLED_BY_USER=4, PROVISIONING_SUCCESS_NEW_AP=5, PROVISIONING_SUCCESS_OLD_AP=6, PROVISIONING_ERROR_FAILED_TO_ASSOCIATE=7, PROVISIONING_ERROR_PASSWORD_AUTH=8, PROVISIONING_ERROR_EULA_BLOCKING=9, PROVISIONING_ERROR_NO_INTERNET=10, PROVISIONING_ERROR_UNSUPPORTED_TYPE=11 |
| `EnumRegisterLiveStreamStatus` | REGISTER_LIVE_STREAM_STATUS_STATUS=1, REGISTER_LIVE_STREAM_STATUS_ERROR=2, REGISTER_LIVE_STREAM_STATUS_MODE=3, REGISTER_LIVE_STREAM_STATUS_BITRATE=4 |
| `EnumRegisterPresetStatus` | REGISTER_PRESET_STATUS_PRESET=1, REGISTER_PRESET_STATUS_PRESET_GROUP_ARRAY=2 |
| `EnumResultGeneric` | RESULT_UNKNOWN=0, RESULT_SUCCESS=1, RESULT_ILL_FORMED=2, RESULT_NOT_SUPPORTED=3, RESULT_ARGUMENT_OUT_OF_BOUNDS=4, RESULT_ARGUMENT_INVALID=5, RESULT_RESOURCE_NOT_AVAILABLE=6 |
| `EnumScanEntryFlags` | SCAN_FLAG_OPEN=0, SCAN_FLAG_AUTHENTICATED=1, SCAN_FLAG_CONFIGURED=2, SCAN_FLAG_BEST_SSID=4, SCAN_FLAG_ASSOCIATED=8, SCAN_FLAG_UNSUPPORTED_TYPE=16 |
| `EnumScanning` | SCANNING_UNKNOWN=0, SCANNING_NEVER_STARTED=1, SCANNING_STARTED=2, SCANNING_ABORTED_BY_SYSTEM=3, SCANNING_CANCELLED_BY_USER=4, SCANNING_SUCCESS=5 |
| `EnumWindowSize` | WINDOW_SIZE_480=4, WINDOW_SIZE_720=7, WINDOW_SIZE_1080=12 |
