# Third-party notices

RecLink is licensed under the **GNU General Public License v3.0** (see `LICENSE`). It includes or
builds against the third-party software below, under their own licences. The firmware image you
build or download contains compiled code from all of them.

## DJI Osmo GPS Controller Demo (MIT)

The DJI camera driver (`components/camera_dji/`) reuses protocol constants, the CRC-16/CRC-32
routines and enum tables from DJI's open-source demo
<https://github.com/dji-sdk/Osmo-GPS-Controller-Demo>, pinned to commit
`92fe23e5a749f189593f980a26a105c3bb66aa1c`. That part of the demo is offered under the MIT License:

```
The MIT License (MIT)
Copyright (c) 2025 SZ DJI Technology Co., Ltd.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Note: the same upstream `LICENSE` file also states that the demo and the "DJI R SDK Protocol"
documents are offered under DJI's End User License Agreement
(<http://developer.dji.com/policies/eula/>). DJI's own file lists both; this project relies on the MIT
terms above for the code it reuses. DJI's EULA is DJI's document, not part of this project, and anyone
redistributing or commercialising the DJI driver should read it themselves.

## opendroneid-core-c (Apache-2.0)

The Remote ID receiver (`components/rid/`, spec §8.8) decodes Open Drone ID Basic ID and Location
messages with the per-message decoders of the Open Drone ID core C library
<https://github.com/opendroneid/opendroneid-core-c>, pinned to commit
`849c03adbe0cdd68997a462fafab397edcfe53f3` (2026-10-07). Two files are vendored unmodified (a
header comment recording the pin was added at the top of each) under
`components/rid/opendroneid/`: `opendroneid.c` and `opendroneid.h`, together with the library's
`LICENSE` (Apache License 2.0). Copyright (C) 2019 Intel Corporation; SPDX-License-Identifier:
Apache-2.0. Only `decodeBasicIDMessage()` / `decodeLocationMessage()` are used.

## Other components

| Component | Licence | Where |
|---|---|---|
| ESP-IDF (FreeRTOS, lwIP, mbedTLS, Wi-Fi/BT drivers, ...) | Apache-2.0 (with its own bundled third-party licences) | <https://github.com/espressif/esp-idf> |
| Apache NimBLE (Bluetooth LE host, shipped inside ESP-IDF) | Apache-2.0 | <https://github.com/apache/mynewt-nimble> |
| cJSON (shipped inside ESP-IDF) | MIT | <https://github.com/DaveGamble/cJSON> |
| `espressif/led_strip` 3.0.3 (fetched at build time by the IDF component manager) | Apache-2.0 | <https://components.espressif.com/components/espressif/led_strip> |
| Open GoPro BLE protocol | Protocol specification only; no GoPro code is included | <https://gopro.github.io/OpenGoPro/> |
| Betaflight MSP protocol | Protocol specification only; no Betaflight code is included | <https://github.com/betaflight/betaflight> |

Apache-2.0 software can be combined into a GPL-3.0 work; the combined firmware is distributed under
GPL-3.0, and each component above keeps its own copyright notice and licence.
