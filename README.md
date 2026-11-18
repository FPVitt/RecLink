<div align="center">

<img src="LOGO.svg" alt="RecLink" width="230">

# RecLink

**Firmware that bridges a Betaflight flight controller and an action camera.**

Camera telemetry on your OSD, recording that follows your arm switch, and one
config portal — no phone app, no cloud.

![platform](https://img.shields.io/badge/target-ESP32--C3%20%2F%20S3-333)
![framework](https://img.shields.io/badge/ESP--IDF-5.x-blue)
![link](https://img.shields.io/badge/FC-MSP%20over%20UART-333)
![link](https://img.shields.io/badge/camera-BLE-5b2c9e)

**[Guides, board explorer and firmware installer → fpvitt.github.io/RecLink](https://fpvitt.github.io/RecLink/)**

</div>

---

## What it does

RecLink is an ESP32 board that sits between your flight controller (MSP over a
spare UART) and one action camera (Bluetooth LE). It is a **passenger**: it only
*reads* from the FC and *writes text to the OSD*. It never touches arming, RC,
PIDs, or navigation.

- **Camera telemetry → Betaflight OSD.** Recording state, elapsed / remaining
  time, camera battery, SD card free space (minutes *and* GB), resolution, frame
  rate, colour profile, shooting mode, Protune settings (ISO min/max, shutter, white balance,
  colour, sharpness, EV, on/off), link status — 31 tokens across 4 freely
  editable 16-character lines.
- **Remote ID receiver (new in 0.3.0, off by default).** Hears other aircraft's
  Open Drone ID broadcasts — Bluetooth always, Wi-Fi only while armed — and puts
  a proximity warning on the OSD with the aircraft's number, distance, bearing
  arrow and above/below: `!RID1 240M ↗+`. One aircraft per message; alerts take
  the whole line, warnings alternate with it; only the `!` blinks. Receive-only:
  it never transmits Remote ID, keeps only each aircraft's ID, range and bearing
  (no operator data), and runs only with a GPS fix. An informational aid, **not
  collision avoidance**. Pre-release: not yet validated in flight or for
  coexistence with the camera link.
- **Recording that follows the aircraft.** Start/stop on arm, on a radio switch,
  or a combination — arm OR the switch (either one triggers it), or arm AND the
  switch (both must be true at once). Every command is verified against what the
  camera actually reports.
- **Preset switching from a 2–6 zone radio switch.** On a GoPro each zone
  pushes its own **camera settings** — resolution, frame rate, lens, HyperSmooth,
  Protune (ISO, shutter angle, white balance, colour, sharpness, EV, 10-bit) —
  to the camera's video preset, checked against what the camera reports; on a
  DJI each zone selects a camera mode. Zones and their settings are built in the
  portal, with option lists that follow what the camera actually allows.
- **Highlight tags** punched into the recording from a momentary switch.
- **Up to 4 paired cameras.** Any mix of GoPro and (experimental) DJI. On power-up RecLink
  connects to whichever paired camera has the strongest signal — one at a time.
- **Camera clock sync** from the FC's GPS time, so your footage timestamps are
  right.
- **DJI GPS position push** — feed the aircraft's fix to the camera for its own
  telemetry overlay.
- **Analog *and* digital OSD.** A "digital" mode restricts output to the glyphs
  the DJI goggle OSD can render.
- **Settings per camera.** With several cameras paired, the Cam control, OSD, Remote ID and
  Clock settings are saved for each camera and switch automatically to the ones of whichever
  camera connects; a selector on those tabs (shown once a camera is paired) edits any paired camera's settings. A newly paired
  camera starts as a copy of the previous one.
- **Config portal + OTA.** The board raises its own Wi-Fi access point while
  disarmed; everything is configured from a captive web page and firmware
  updates upload straight to it. The access point drops within 100 ms of arming.

### Supported cameras

| Brand | Models | Protocol |
|---|---|---|
| GoPro | **Supported:** HERO 9 / 10 / 11 / 12 / 13 (and newer with the same BLE API); HERO11 Black is the bench-tested one. **Mission 1 / Mission 1 Pro: expected, unverified** (may need two-byte setting ids this firmware does not speak; BLE advertising unknown). HERO8 and older are not supported (no Open GoPro BLE) | [Open GoPro](https://gopro.github.io/OpenGoPro/) BLE |
| DJI | **Experimental / unverified:** Osmo Action 4 / 5 Pro / 6 and Osmo 360 — no DJI camera has been tested yet; record, mode switch and status are implemented from DJI's open-source demo (the Action-line protocol), but behaviour per model is not confirmed. **Osmo Nano: not expected to work** — it speaks a different DJI protocol that this driver does not implement | DUML over BLE |

### Requirements

- **Betaflight ≥ 2025.12.0 / MSP API ≥ 1.47** — the custom OSD message support
  ([betaflight #14097](https://github.com/betaflight/betaflight/pull/14097)).
- An ESP32-**C3** (production) or ESP32-**S3** (development) board.
- A free UART on the flight controller.

---

## Build & flash

```sh
. $IDF_PATH/export.sh                 # ESP-IDF v5.x

idf.py set-target esp32c3             # production default — or: esp32s3
idf.py build
idf.py -p <port> flash monitor
```

`sdkconfig.defaults` + `sdkconfig.defaults.esp32{c3,s3}` carry the mandatory
options (static FreeRTOS allocation, persistent NimBLE bonds, 4 s task watchdog
with panic, brown-out detector). A generated `sdkconfig` is **not** checked in.

### Tests and CI

The pure logic (CRCs, MSP codec and parser, DJI frames, the record-trigger state machine, switch decode,
BLE failure classification, the GoPro packetizer, and the Remote ID decoder, geometry, message formatting,
config migration and heading logic) has host unit tests that need no ESP-IDF:

```sh
make -C test/host        # builds with AddressSanitizer + UBSan and runs everything
```

GitHub Actions (`.github/workflows/ci.yml`) runs them and builds all four board profiles (RecLink board,
SuperMini, generic ESP32-S3, S3-Zero) on ESP-IDF v5.4.1. `dependencies.lock` is committed, so builds of
the same commit resolve the same components.

### Boards

The pin map is a build-time board profile. The default is the RecLink PCB (MSP on UART0 GPIO21/GPIO20,
LED on GPIO10 active-high). On that board the portal Wi-Fi is always on while disarmed (and off while
armed), so the portal's "Open the portal" option is hidden; the prototype boards keep it
(`CONFIG_BRIDGE_BOARD_RECLINK`). For a generic **ESP32-C3 SuperMini** (MSP on UART1 GPIO6/GPIO7, LED on GPIO8
active-low) use the release image `fc_cam_bridge-esp32c3-supermini.bin`, or build it yourself:

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c3;sdkconfig.defaults.board-supermini" \
       -B build_supermini set-target esp32c3 build
```

For the **Waveshare ESP32-S3-Zero** (4 MB flash, one WS2812 RGB LED on GPIO21, BOOT button on GPIO0; MSP on
UART1, FC TX to GPIO4 and FC RX from GPIO5):

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.board-s3zero" \
       -B build_s3zero set-target esp32s3 build
```

An RGB LED shows each state in its own colour (same blink timing as the plain LED): white = booting,
purple = no flight controller, amber = searching for the camera, cyan = camera ready, green = recording,
red = camera lost. The LED does not change while the config portal is open. `led_brightness` in the portal's System tab dims it.

Other boards: copy `sdkconfig.defaults.board-supermini` and change the `CONFIG_BRIDGE_*` pins.

The portal's web assets (`components/portal/www/`) are gzipped at build time and
embedded in the app image, so `gzip` must be on `PATH`.

---

## The config portal

While the aircraft is disarmed, RecLink hosts an open Wi-Fi network
`RECLINK-XXXX`. Join it and any page redirects to the portal at `http://10.0.0.1`.

- **Status** — live link state, camera telemetry (including the GoPro Protune
  values and the result of the last zone push), and MSP diagnostics.
- **Cam control** — recording mode, the arm/switch triggers (with one-touch AUX
  channel auto-detect — flip the switch you want and it finds the channel for
  you), the zone builder (add up to 6 zones and set their edges on the slider;
  per-zone camera settings, editable while a camera is connected, capture from
  the camera, options filtered by the camera's capabilities), highlight switch.
- **OSD** — pick analog or digital, add and arrange telemetry tokens on the four
  lines, preview them against live or sample data (including a simulation of the
  Remote ID message on the line that holds `%rp`).
- **Remote ID** — switch the receiver on, choose Bluetooth / Wi-Fi / both, set
  the warning distances and options, list your own aircraft to ignore, see the
  aircraft heard right now, and run a 30 s reception test that lists them
  (ID, range, bearing, above/below).
- **Binding** — pair, list and forget cameras; a guided GoPro pairing flow with a
  GoPro Labs QR shortcut.
- **Network** — Wi-Fi credentials, MSP baud rate and TX/RX swap, Wi-Fi/BLE
  transmit power.
- **System** — LED brightness, advanced-mode toggle, OTA firmware upload,
  factory reset.

Change the default Wi-Fi password on first use — the portal will nag until you do.

---

## How it works

Six rules shape every line of the firmware:

1. **Passenger, not pilot.** The bridge only reads the FC and writes OSD text.
   `msp_task` structurally refuses `MSP_SET_ARMING_DISABLED` and every RC / GPS /
   RTC / DisplayPort / reboot write — not by convention, by code.
2. **One owner per resource.** Exactly one task touches the FC UART, exactly one
   touches the camera BLE link. Everything else talks through fixed-length
   queues.
3. **Fail open, fail quiet.** Any bridge failure leaves the aircraft fully
   flyable; the worst in-flight outcome is stale OSD text. On any doubt after a
   fault the board reboots (~300 ms, the camera re-bonds automatically) rather
   than recover in place.
4. **Never block on the camera.** Every BLE write is fire-and-forget with async
   verification.
5. **Wi-Fi never transmits in flight.** The access point is down within 100 ms of
   the armed edge. In flight the radio is off — or, only if the Remote ID
   receiver's Wi-Fi mode is enabled, passively listening; it never transmits.
6. **Deterministic memory.** No heap allocation in the flight path: everything
   the bridge needs while flying is allocated once at boot (the Remote ID
   receiver's memory only if it is enabled, which needs a reboot). The config
   portal exists only on the bench and does use the heap, so it is sized for
   it: large JSON responses are streamed section by section, and task stacks
   are measured and logged.

### Layout

| Path | Role |
|---|---|
| `components/common/` | CRC, monotonic clock, log ring, shared leaf types |
| `components/platform/` | board pins (Kconfig), LED, button, task WDT, RTC counters |
| `components/cfg/` | versioned NVS blob, migration chain, runtime flags, stats |
| `components/msp/` | MSPv2 codec, request scheduler, `fc_state`, the FC-UART owner task |
| `components/camera/` | driver interface, `cam_state`, NimBLE central, the BLE-link owner task |
| `components/camera_gopro/` | Open GoPro BLE driver |
| `components/camera_dji/` | DJI DUML BLE driver |
| `components/control/` | record / preset / clock-sync / highlight + GPS-push state machines |
| `components/osd/` | token → 4×16 line formatter + `osd_task` |
| `components/portal/` | SoftAP + captive HTTP portal + OTA; the single owner of the Wi-Fi driver |
| `components/rid/` | Remote ID receiver: Open Drone ID decode, aircraft table, `rid_task` |
| `main/` | `app_main` init order + `housekeeping` |

Adding a camera brand is a new `components/camera_<brand>/` directory that
implements `camera_driver_t` — it never touches core code.

---

## Status

Early development. The firmware builds for both targets and the full feature set
is implemented, and it has been bench-tested on real ESP32-C3 hardware wired to
a Betaflight FC and a GoPro HERO11 Black — MSP link, OSD rendering, recording
triggers, the config portal and the settings-based zone presets all working.
Version 0.3.1 is a **pre-release and may contain bugs**. Several protocol
constants are still assumed pending verification against a live GoPro / DJI
capture (see [VERIFY.md](VERIFY.md)). The Remote ID receiver (new in 0.3.0) has
been built for every board profile and booted on the ESP32-C3 board; its Wi-Fi
reception, its effect on the camera link and its warnings in flight have **not**
been validated yet, which is why it ships switched off.

> [!WARNING]
> This is a pre-release and may contain bugs. Use it at your own risk.

## License

RecLink's firmware is free software under the **GNU General Public License v3.0**
(see [LICENSE](LICENSE)): you can use, study, modify and sell it, and anyone who distributes a
modified version must share its source under the same licence. Copyright (C) 2026 Vittorio.

The DJI driver reuses code from DJI's MIT-licensed `Osmo-GPS-Controller-Demo`, and the Remote ID
receiver uses the Apache-2.0 Open Drone ID core C library; that and the other
third-party components (ESP-IDF, NimBLE, cJSON, `led_strip`) keep their own licences, listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The PCB design files and the website guides are not
covered by this licence.
