# The ESP32 modem

v3 drops the Raspberry Pi RM2 radio (~$6.50) for an ESP32 module on a UART (~$1.65–$2.39). The
RP2354A keeps video and rendering; the modem gets Wi-Fi, the IP stack, provisioning and TLS. See
[`../../TV-Top Kiosk/v3/README.md`](../../TV-Top%20Kiosk/v3/README.md) for the board.

Two firmwares, one wire format:

| | | |
|---|---|---|
| `src/common/modem_proto.h` | both | the wire format, compiled from one copy by both builds |
| `src/pico/modem_link.c` | RP2350 | framing, a DMA receive ring, modem reset and strapping |
| `src/pico/net_modem.c` | RP2350 | `net_wifi.h` over the link |
| `src/pico/http_modem.c` | RP2350 | `http_client.h` over the link |
| `modem/` | ESP32 | ESP-IDF app: Wi-Fi, HTTPS, the portal's sockets |

`kiosk_loop.c` is unchanged. Both backends provide the same two headers, so the protocol state
machine, the renderer and the geometry cache do not know which radio they are talking to.

## Wiring

### The v3 board

Four GPIOs, the same ones the RM2 used on v2:

| RP2354A | ESP MINI-1 pad | Purpose |
|---|---|---|
| GPIO24 (UART1 TX) | 30, RXD0 | data to the modem |
| GPIO25 (UART1 RX) | 31, TXD0 | data from the modem |
| GPIO23 | 8, EN | reset (10 k pull-up, 1 µF delay) |
| GPIO29 | 23, IO9 | boot strap: high boots the firmware, low enters the ROM bootloader |

The link is on the modem's UART0, which is also its ROM bootloader, so the RP2354A can reflash the
modem itself and the board needs only one USB-C programming path.

### The bring-up rig: Pico 2 W + PiCowBell HSTX DVI + ESP32-C3-DevKitM-1

The Pico 2 W's GPIO23/24/25/29 are wired to its own cyw43 and are not on the header, and the
PiCowBell takes GPIO3, 4/5 (DDC), 6/7 (USB host) and 12–19 (HSTX). What is left:

| Pico 2 W | Pin | | ESP pin | ESP GPIO | Purpose |
|---|---|---|---|---|---|
| GP8 (UART1 TX) | 11 | → | **IO6** (J3-9) | GPIO6 | data to the modem |
| GP9 (UART1 RX) | 12 | ← | **IO7** (J3-8) | GPIO7 | data from the modem |
| GND | 13 | — | **GND** (J3-7) | — | common ground |
| GP10 | 14 | → | **RST** (J1-7) | CHIP_PU | modem reset |
| GP11 | 15 | → | **IO9** (J3-5) | GPIO9 | boot strap |

**Power each board from its own USB cable.** Espressif's DevKitM-1 guide calls its three supplies
(USB, the 5V pins, the 3V3 pins) mutually exclusive, and keeping the ESP on its own USB gives you
its serial monitor while the link is running.

**The link is not on the ESP's UART0 here.** On the DevKitM-1, UART0 (IO20/21) also goes to the
onboard CP2102N, so with the ESP's USB plugged in the bridge's transmitter and the Pico's would
both be driving IO20. IO6/IO7 sidesteps that entirely. To test the v3 arrangement instead, unplug
the ESP's USB, power it from the Pico's VBUS, and build the modem with
`idf.py -DMODEM_LINK_UART=0 -DMODEM_LINK_TX_PIN=21 -DMODEM_LINK_RX_PIN=20 build`.

RST and IO9 are driven **open-drain** by the RP2350 — released is high-impedance, asserted is low —
so they do not fight the DevKit's RST/BOOT buttons or the CP2102N's auto-reset transistors. All
five wires can stay in place while you flash the ESP over its own USB.

## Building and flashing

Modem, once per session:

```bash
source ~/esp/esp-idf-v5.5.5/export.sh
```

```bash
cd modem && idf.py set-target esp32c3 && idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

Kiosk:

```bash
cmake -S . -B build-modem -G Ninja -DPICO_BOARD=pico2_w -DKIOSK_MODEM=ON -DKIOSK_VIDEO_MODE=1080p30
```

```bash
ninja -C build-modem && picotool load -f build-modem/tvtop_kiosk.uf2
```

`-DKIOSK_MODEM=ON` swaps `net_wifi.c`/`http_client.c` for the modem pair and drops
`pico_cyw43_arch_lwip_threadsafe_background`, the DHCP server and the DNS server from the build.
Leave it off and the existing cyw43 firmware builds exactly as before.

## The link

921600 baud, 8N1, no hardware flow control (v3 has no pins for it). Frames are

```
A5 5A | type u8 | flags u8 | len u16 | payload[len] | crc16 u16
```

with CRC-16/CCITT-FALSE over everything but the sync bytes. A receiver that fails the CRC hunts for
the next `A5 5A`, which is how the kiosk rides through the ESP ROM's boot banner at reset.

### Not losing bytes

Core 0 vanishes for a hundred milliseconds at a time to render a frame or erase a flash sector, and
nothing on the UART can wait for it. Two mechanisms cover that:

- **A hardware receive ring.** One DMA channel in the RP2350's ENDLESS mode writes UART bytes into
  a 16 KB power-of-two buffer for ever, wrapping its own write address. `modem_link_poll()` reads
  the DMA's write pointer to see how far the producer got. Both UART FIFOs are off, so the PL011
  raises a DMA request per byte: with FIFOs on, the trailing bytes of a burst can sit below the
  trigger level until more arrive, which is fatal when those bytes are the end of a reply.
- **Credit for body data.** The modem may only send response-body bytes the kiosk has granted
  credit for — 8 KB up front, topped up as bytes reach the frame decoder. When the kiosk stops
  draining, the modem stops reading its socket, TCP's window closes, and the server waits. A 300 KB
  static set streams into the flash geometry cache with nothing buffered on either side.

### Messages

Host → modem: `H_HELLO`, `H_PING`, `H_WIFI_CONNECT`, `H_WIFI_STOP`, `H_AP_START`, `H_AP_STOP`,
`H_SCAN`, `H_HTTP_GET`, `H_HTTP_CANCEL`, `H_HTTP_CREDIT`, `H_SOCK_DATA`, `H_SOCK_CLOSE`, `H_LED`.

Modem → host: `M_HELLO`, `M_PONG`, `M_WIFI_STATE`, `M_SCAN_RESULT`, `M_SCAN_DONE`,
`M_HTTP_STATUS`, `M_HTTP_HEADER`, `M_HTTP_BODY`, `M_HTTP_DONE`, `M_SOCK_OPEN`, `M_SOCK_DATA`,
`M_SOCK_SENT`, `M_SOCK_CLOSE`, `M_LOG`.

`M_HTTP_DONE`'s error codes are `http_client.h`'s `HTTP_ERR_*` values, so they pass to the sink
unchanged. `M_HTTP_HEADER` exists but is not sent: nothing on the kiosk reads response headers, and
they would cost link time on every frame.

The modem keeps **no** settings — no stored credentials, no server URL. It boots blank, and
`net_modem.c` replays whatever was asked for whenever a fresh `M_HELLO` arrives, so a modem that
crashes and restarts rejoins by itself and the kiosk above never notices.

## The captive portal

The portal stays on the RP2350. The modem runs the AP, the DHCP server and a DNS responder that
answers every name with 192.168.4.1, accepts connections on port 80, and relays the bytes as
`M_SOCK_*` / `H_SOCK_*`. `src/pico/provision.c` serves the page, decodes the form and validates it
exactly as on the cyw43 build — one implementation, covered by the same `test_provision` host test,
and no portal HTML on the modem. The network list comes back from `H_SCAN` and lands in the same
table the cyw43 scan callback filled.

## TLS

HTTPS terminates on the modem, verified against ESP-IDF's bundled Mozilla roots. The kiosk no
longer needs `ca_certs.h`, a 16 KB TLS record buffer or a 32 KB TCP window, so `KIOSK_TLS` is
irrelevant to a modem build and the production server can be reached over `https://` — which the
RP2040 never could afford.

mbedTLS checks certificate validity dates and the ESP boots at 1970, so the modem starts SNTP as
soon as the station has an address and the first HTTPS request waits up to 10 s for a clock.

## Logs

The modem has no console of its own once the board has one USB-C port, so `ESP_LOG` output is
turned into `M_LOG` frames and printed on the kiosk's USB console prefixed with `modem[I]:`. Lines
are queued and sent by the receive task, and a full queue drops lines rather than blocking whatever
was logging.

## Console

`stats` gains a modem line: link state, the modem's firmware version, frame and byte counters, and
CRC, resync, overrun and transmit-full counts. A healthy link shows zeros across all five.

```
modem [reset|boot]
```

`modem reset` restarts the modem firmware; `modem boot` releases it into its ROM bootloader so
esptool — or, on v3, the RP2354A's own flasher — can reprogram it.

## Still to do

- **The flasher.** v3's plan is that the RP2354A carries the modem image inside its own firmware
  and writes it over the UART with the esptool protocol, so one USB-C port programs the whole
  board. `modem boot` already puts the modem in the right state; the stub loader and the write
  loop are not written yet.
- **2 Mbaud on the PCB.** `MODEM_BAUD_FAST` is defined and 20 mm of trace should take it; the
  dupont rig is left at 921600.
- **`KIOSK_MODEM` on the RP2040.** Untested and pointless — the Pico W build keeps its cyw43 — but
  nothing in `modem_link.c` is RP2350-specific except the DMA's ENDLESS transfer count, which the
  RP2040 lacks.
