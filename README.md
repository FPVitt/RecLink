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
  rate, colour profile, shooting mode, link status — 22 tokens across 4 freely
  editable 16-character lines.
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
- **Config portal + OTA.** The board raises its own Wi-Fi access point while
  disarmed; everything is configured from a captive web page and firmware
  updates upload straight to it. Wi-Fi drops within 100 ms of arming.

### Supported cameras

| Brand | Models | Protocol |
|---|---|---|
| GoPro | **Supported:** HERO 9 / 10 / 11 / 12 / 13 (and newer with the same BLE API); HERO11 Black is the bench-tested one. **Mission 1 / Mission 1 Pro: expected, unverified** (may need two-byte setting ids this firmware does not speak; BLE advertising unknown). HERO8 and older are not supported (no Open GoPro BLE) | [Open GoPro](https://gopro.github.io/OpenGoPro/) BLE |
| DJI | **Experimental / unverified:** Osmo Action 4 / 5 Pro / 6, Osmo Nano, Osmo 360 — no DJI camera has been tested yet; record, mode switch and status are implemented from DJI's open-source demo, but behaviour per model is not confirmed | DUML over BLE |

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

### Boards

The pin map is a build-time board profile. The default is the RecLink PCB (MSP on UART0 GPIO21/GPIO20,
LED on GPIO10 active-high). For a generic **ESP32-C3 SuperMini** (MSP on UART1 GPIO6/GPIO7, LED on GPIO8
active-low) use the release image `fc_cam_bridge-esp32c3-supermini.bin`, or build it yourself:

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c3;sdkconfig.defaults.board-supermini" \
       -B build_supermini set-target esp32c3 build
```

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
  you), the zone builder (add up to 6 zones, per-zone camera settings, capture
  from the camera, options filtered by the camera's capabilities), highlight
  switch.
- **OSD** — pick analog or digital, drag telemetry tokens onto the four lines,
  preview them against live or sample data.
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
5. **Wi-Fi only while disarmed.** The access point is down within 100 ms of the
   armed edge; in flight only BLE is active.
6. **Deterministic memory.** No heap allocation in the flight path (everything
   the bridge needs while flying is static). The config portal exists only on
   the bench and does use the heap, so it is sized for it: large JSON responses
   are streamed section by section, and task stacks are measured and logged.

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
| `components/portal/` | SoftAP + captive HTTP portal + OTA |
| `main/` | `app_main` init order + `housekeeping` |

Adding a camera brand is a new `components/camera_<brand>/` directory that
implements `camera_driver_t` — it never touches core code.

---

## Status

Early development. The firmware builds for both targets and the full feature set
is implemented, and it has been bench-tested on real ESP32-C3 hardware wired to
a Betaflight FC and a GoPro HERO11 Black — MSP link, OSD rendering, recording
triggers, the config portal and the settings-based zone presets all working —
but it has **not been flight tested**, and several
protocol constants are still assumed pending verification against a live
GoPro / DJI capture (see [VERIFY.md](VERIFY.md)).

> [!WARNING]
> This is not a released image. Do not fly it.

## License

No license is granted yet. The DJI driver reuses parser, CRC and enum tables
from DJI's MIT-licensed `Osmo-Action-GPS-BLE` demo, pinned by commit hash in the
source.
