// uart_bridge.ino -- a genuinely "dumb" USB<->UART passthrough for the
// ESP32-C3 SuperMini, so it can act as a plain programmer for a second board
// wired to its UART pins (e.g. for esptool flashing over UART).
//
// It does exactly one thing: copy bytes verbatim, in both directions, as fast
// as they arrive. No framing, no parsing, no printed debug text on the relayed
// channel -- anything printed there would corrupt whatever protocol is riding
// on top (esptool's SLIP framing included). If you need to know it's alive,
// blink the onboard LED, don't print.
//
// ---------------------------------------------------------------------------
// Arduino IDE setup (required):
//   Board:          "ESP32C3 Dev Module" (or your SuperMini's board entry)
//   USB CDC On Boot: Enabled   <-- critical. This makes `Serial` the native
//                                  USB-CDC channel (what your computer sees as
//                                  /dev/cu.usbmodemXXXX) and frees `Serial0`
//                                  to be the hardware UART wired to the target
//                                  board. With it Disabled, `Serial` would BE
//                                  UART0 and there would be nothing left to
//                                  bridge to.
//
// Wiring (SuperMini -> target board):
//   SuperMini TX (GPIO21, silkscreened "TX")  -> target board RX
//   SuperMini RX (GPIO20, silkscreened "RX")  -> target board TX
//   SuperMini GND                              -> target board GND
//   (leave the target's own power/EN/GPIO0 alone -- this sketch does not
//   touch reset lines; put the target in bootloader mode yourself: hold BOOT,
//   power/reset it, then release BOOT.)
//
// If your specific SuperMini variant labels its UART0 pins differently, edit
// UART_RX_PIN / UART_TX_PIN below to match.
//
// Usage:
//   Flash this sketch once. From then on, the SuperMini's USB port relays
//   raw bytes to/from the target's UART0 at UART_BAUD. Point esptool /
//   idf.py at the SuperMini's port with `--before no_reset --after no_reset`
//   (it must not try to reset the SuperMini itself -- only the target, which
//   you put in bootloader mode by hand) and flash as normal.
// ---------------------------------------------------------------------------

static const int      UART_RX_PIN = 20;   // SuperMini "RX" pin <- target TX
static const int      UART_TX_PIN = 21;   // SuperMini "TX" pin -> target RX
static const uint32_t UART_BAUD   = 115200;  // esptool's initial ROM sync is
                                             // fixed-rate -- a mismatched
                                             // bridge baud breaks it outright,
                                             // so this must stay 115200 to
                                             // match the esptool -b flag.

// esptool bursts several KB at a time over full-speed USB (the flasher stub,
// block-write payloads) -- far faster than a 115200 baud UART can drain. With
// only the tiny default buffers, that excess gets silently dropped the moment
// a real burst hits, which looks like "serial noise/corruption" on the
// esptool side even though every small command/response before that worked
// fine. Give both sides real headroom to absorb a burst while the UART side
// drains it out at its own pace.
static const size_t USB_RX_BUF  = 8192;
static const size_t UART_RX_BUF = 4096;
static const size_t UART_TX_BUF = 4096;
static const size_t CHUNK       = 512;

static uint8_t chunkBuf[CHUNK];

void setup() {
  Serial.setRxBufferSize(USB_RX_BUF);
  Serial.begin(115200);                                   // native USB-CDC

  Serial0.setRxBufferSize(UART_RX_BUF);
  Serial0.setTxBufferSize(UART_TX_BUF);   // delete this line if your core
                                          // predates setTxBufferSize()
  Serial0.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
}

void loop() {
  // USB -> UART (host -> target), chunked instead of byte-at-a-time
  int n;
  while ((n = Serial.available()) > 0) {
    size_t len = Serial.readBytes(chunkBuf, (size_t)n < CHUNK ? (size_t)n : CHUNK);
    if (len) { Serial0.write(chunkBuf, len); }
  }
  // UART -> USB (target -> host)
  while ((n = Serial0.available()) > 0) {
    size_t len = Serial0.readBytes(chunkBuf, (size_t)n < CHUNK ? (size_t)n : CHUNK);
    if (len) { Serial.write(chunkBuf, len); }
  }
}
