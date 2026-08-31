# RecLink — FC ⇄ action-cam bridge firmware

ESP32-C3 (production) / ESP32-S3 (dev) firmware that bridges a Betaflight flight
controller (MSP over UART) and one action camera (BLE — GoPro HERO 11+ or DJI
Osmo Action 4/5/6/360). It pushes camera telemetry to the Betaflight OSD, drives
record/preset from arm state or a radio switch, and syncs the camera clock from
the FC's GPS time.

**The spec is the contract:** [`fc-cam-bridge-spec.md`](fc-cam-bridge-spec.md).
Read it before changing code. Every `VERIFY` tag is tracked in
[`VERIFY.md`](VERIFY.md).

Requires **Betaflight ≥ 2025.12.0 / MSP API ≥ 1.47** (custom OSD messages,
betaflight PR #14097).

## Build

```sh
# once
. $IDF_PATH/export.sh                 # ESP-IDF v5.1+

idf.py set-target esp32c3             # production (default). or: esp32s3
idf.py build
idf.py -p <port> flash monitor
```

`sdkconfig.defaults` + `sdkconfig.defaults.esp32{c3,s3}` carry the spec-mandated
options (static FreeRTOS allocation, persistent NimBLE bonds, 4 s Task WDT with
panic, brown-out detector). Do not check in a generated `sdkconfig`.

Portal web assets in `components/portal/www/` are gzipped at build time and
embedded in the app image — `gzip` must be on `PATH`.

## Layout

| Path | Role |
|---|---|
| `components/common/` | CRC, monotonic clock, log ring, shared leaf types |
| `components/platform/` | board pins (Kconfig), LED, button, Task WDT, RTC counters |
| `components/cfg/` | versioned NVS blob, migration chain, runtime flags, stats |
| `components/msp/` | MSPv2 codec, request scheduler, `fc_state`, the FC-UART owner task |
| `components/camera/` | driver interface, `cam_state`, NimBLE central, the BLE-link owner task |
| `components/camera_gopro/` | Open GoPro BLE driver |
| `components/camera_dji/` | DJI DUML BLE driver |
| `components/control/` | record / preset / clock-sync / hilight+GPS-push state machines |
| `components/osd/` | token → 4×16 line formatter + osd_task |
| `components/portal/` | SoftAP + captive HTTP config portal + OTA (bench only) |
| `main/` | `app_main` init order + `housekeeping` |

## The rules that shape the code (spec §1.2 / §3)

1. **Passenger, not pilot** — the bridge only *reads* the FC and *writes OSD
   text*. No exceptions. It never touches arming: `msp_task` refuses
   `MSP_SET_ARMING_DISABLED` (and RC/GPS/RTC/DisplayPort/reboot writes)
   structurally, not by convention.
2. **One owner per resource** — exactly one task touches the FC UART
   (`msp_task`), exactly one touches the camera BLE link (`cam_task`). Everything
   else communicates by fixed-length queue.
3. **Fail open, fail quiet** — any bridge failure leaves the aircraft fully
   flyable; worst in-flight outcome is stale OSD text. On doubt after a fault →
   reboot (~300 ms), don't recover in place.
4. **No blocking on the camera** — every BLE write is fire-and-forget with async
   verify.
5. **Wi-Fi only while disarmed** — the AP is down within 100 ms of the armed edge.
6. **Deterministic memory** — no heap allocation in steady state.

## Status

Greenfield build following spec §14 build order. All milestones scaffolded
through M11b; **not yet compiled against ESP-IDF or run on hardware.** Protocol
constants are `ASSUMED` pending the `protocol-verifier` pass (see `VERIFY.md`).

Not a released image — do not fly without completing the FMEA walk (spec §12.5)
and the `protocol-verifier` / `perf-coherence-reviewer` / `embedded-security-reviewer`
passes.
