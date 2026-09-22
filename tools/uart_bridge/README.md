# uart_bridge

A "dumb" USB↔UART passthrough sketch for an ESP32-C3 SuperMini, used as a bench
programmer for a second board wired to its UART pins. Not part of the RecLink
firmware — dev tooling only.

## Why

The previous bridge firmware wasn't a raw relay: it parsed the UART stream as
some kind of framed protocol and printed `invalid header: 0xffffffff` onto the
same channel esptool needs, corrupting the link. This sketch does nothing but
copy bytes both ways.

## Setup

1. Open `uart_bridge.ino` in the Arduino IDE.
2. Board: your ESP32-C3 SuperMini entry (e.g. "ESP32C3 Dev Module").
3. Tools → **USB CDC On Boot: Enabled** — required, see the comment at the top
   of the sketch for why.
4. Flash it to the SuperMini over its own USB port, same as before.

## Wiring

```
SuperMini TX (GPIO21) -> target board RX
SuperMini RX (GPIO20) -> target board TX
SuperMini GND         -> target board GND
```

Edit `UART_RX_PIN` / `UART_TX_PIN` at the top of the sketch if your board's
silkscreen differs. The sketch never touches reset/EN/GPIO0 on either board —
put the target into bootloader mode yourself (hold BOOT, power/reset it,
release BOOT) before flashing through the bridge.

## Flashing through it

Once wired and the target is in bootloader mode, flash it from this repo's
`build/` directory with `esptool.py` directly (not `idf.py flash`, since that
always tries to reset the port it's attached to — here that would hit the
SuperMini, not the target):

```sh
esptool.py --chip esp32c3 -p /dev/cu.usbmodemXXXX -b 115200 \
  --before no_reset --after no_reset --no-stub write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0     bootloader/bootloader.bin \
  0x20000 fc_cam_bridge.bin \
  0x8000  partition_table/partition-table.bin \
  0x11000 ota_data_initial.bin
```
