# TV-Top Kiosk firmware

Firmware that turns a Raspberry Pi Pico 2 W into an HDMI games console for
[TV-Top Games](https://play.tvtop.games), a board game platform where the TV is the board and
everyone plays from their own phone. The kiosk joins your Wi-Fi, pairs with the TV-Top server, and
draws whatever the current game sends it.

It outputs **1920x1080 at 30 Hz in full 8-bit-per-channel colour**, with no framebuffer and no
external video hardware. A 1080p frame at 24 bit is almost 6 MB and the RP2350 has 520 KB, so there
is no frame in memory at all. Each scanline is built as a run-length HSTX command stream just
before the beam reaches it, six lines ahead, out of vector geometry cached in flash.

The RP2350's HSTX serialiser is rated for 300 Mbit/s per pin. 1080p30 needs 744, so it runs at
372 MHz with the core regulator at 1.30 V. See [docs/HARDWARE.md](docs/HARDWARE.md) for the clock
tables, the overclock, and the Wi-Fi interference problem that comes with it.

## Build your own

You do not need custom hardware. There are two ways to put one together, matching the two
revisions of the custom board.

**Option 1: a Pico 2 W on its own.** The simplest build, two boards and a cable.

| Part | Why |
|---|---|
| **Raspberry Pi Pico 2 W** | the RP2350 has the HSTX serialiser, and the on-board cyw43 does Wi-Fi |
| **Adafruit PiCowBell HSTX DVI** (product 6363) | routes the TMDS pairs to an HDMI socket. A Pico DVI Sock works too, same pinout |

**Option 2: a Pico 2 W plus an ESP32-C3 DevKitM-1.** Wi-Fi, the IP stack and TLS move onto the
ESP32 over a UART, which is what the v3 board does and what the 1080p work is developed on. Wiring
is in [docs/MODEM.md](docs/MODEM.md), and the ESP32 gets flashed separately with `idf.py`.

Either way, plug the PiCowBell onto the Pico 2 W, run an HDMI cable to the TV, and power it over
USB. A Pico W (RP2040) also runs this firmware, but it tops out well below 1080p.

## Quick start

Install the [Pico SDK](https://github.com/raspberrypi/pico-sdk) (2.2.0 or later), the Arm GNU
toolchain, cmake, ninja and picotool. Then, for option 1:

```bash
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w -DKIOSK_TLS=ON -DKIOSK_VIDEO_MODE=1080p30
cmake --build build
picotool load -f build/tvtop_kiosk.uf2 && picotool reboot
```

Or for option 2, where the ESP32 terminates TLS so this firmware does not need mbedTLS:

```bash
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w -DKIOSK_MODEM=ON -DKIOSK_VIDEO_MODE=1080p30
cmake --build build
picotool load -f build/tvtop_kiosk.uf2 && picotool reboot
```

Option 1 at 1080p with TLS is the tightest configuration there is: it links with about 48 KB of
the RP2350's 520 KB left for the heap, and an mbedTLS handshake wants 30 to 40 KB of that. It
builds, but it has not been run on hardware. If it will not reach the server, drop to
`-DKIOSK_VIDEO_MODE=720p60` (which frees roughly 140 KB of line pool) or use option 2, where TLS
lives on the ESP32 and there is about 79 KB spare.

On first boot the kiosk has no Wi-Fi credentials, so it starts an open access point called
`TVTOP-xxxx` and shows that name on the TV. Join it from your phone, the setup page opens by
itself, pick your network and press Save. Full details in
[docs/PROVISIONING.md](docs/PROVISIONING.md).

The kiosk then registers itself with the server and shows a pairing code on the TV to enter in the
app.

## Video modes

Set with `-DKIOSK_VIDEO_MODE=` at build time, or `mode <name>` on the USB serial console followed
by a reboot. The setting persists in flash.

On the Pico 2 W (RP2350, HSTX):

| Mode | Output | clk_sys | Notes |
|---|---|---|---|
| `720p60` | 1280x720, CEA VIC 4 | 372 MHz | the safe default |
| `1080p30` | 1920x1080, CEA VIC 34 | 372 MHz | needs a sink that accepts a 33.8 kHz line rate |
| `1080p25` | 1920x1080, CEA VIC 33 | 372 MHz | for TVs that will not go above 30 kHz |
| `1080p24` | 1920x1080, CEA VIC 32 | 372 MHz | likewise |
| `960x540p60` | 960x540 | 351-372 MHz | fall back here if a cable or chip lacks the margin |
| `480p60` | 640x480 | 252 MHz | gentlest overclock, widest compatibility |

The refresh rate being low costs nothing here: the kiosk only redraws when the game state changes,
so 30 Hz and 60 Hz look identical in use.

No picture? [docs/HARDWARE.md](docs/HARDWARE.md) has a checklist. The usual causes are a TV that
rejects the timing, or a long HDMI cable at 744 Mbit/s from 3.3 V pins.

## Pointing it somewhere else

The kiosk talks to `https://kiosk.tvtop.games` by default. Override it at build time with
`-DKIOSK_SERVER_BASE=`, on the setup page, or with `server <url>` on the serial console. The wire
protocol is documented in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); the server itself is not
open source.

## Docs

| | |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | how the pieces fit together, and the protocol |
| [BUILD.md](docs/BUILD.md) | build options and flashing |
| [HARDWARE.md](docs/HARDWARE.md) | wiring, video modes, the overclock, Wi-Fi interference |
| [RENDERING.md](docs/RENDERING.md) | the drawing ops and how frames are rasterised |
| [NETWORK.md](docs/NETWORK.md) | the long poll, backoff and TLS |
| [PROVISIONING.md](docs/PROVISIONING.md) | first boot, the setup portal, serial console |
| [MODEM.md](docs/MODEM.md) | the ESP32 Wi-Fi modem used by the custom board |
| [FONTS.md](docs/FONTS.md) | text rendering |

## Built on

- [PicoDVI](https://github.com/Wren6991/PicoDVI) by Luke Wren, vendored as `vendor/libdvi`, which
  does the video on the RP2040 path and showed that any of this was possible.
- [pico-examples](https://github.com/raspberrypi/pico-examples) for the DHCP server used by the
  setup portal.
- [stb](https://github.com/nothings/stb) for font rasterisation.

## License

TODO: pick one. MIT is the obvious fit and is compatible with the vendored BSD-3 and MIT code.
