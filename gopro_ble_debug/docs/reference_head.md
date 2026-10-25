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

