# Building and flashing

## Prerequisites

| Tool | Notes |
|---|---|
| pico-sdk 2.2.0 or later, with the cyw43-driver, lwip, mbedtls and tinyusb submodules | |
| Arm GNU Toolchain (arm-none-eabi, with newlib) | 15.3.rel1 is what this is developed against |
| cmake, ninja, picotool | |
| ESP-IDF 5.x | only for the `KIOSK_MODEM` build, to build the ESP32 side |
| resvg | only for regenerating the reference renders |

`CMakeLists.txt` falls back to `~/.pico-sdk/sdk/2.2.0` and a matching toolchain path if neither
`PICO_SDK_PATH` nor `PICO_TOOLCHAIN_PATH` is set. Export either environment variable, or pass
`-DPICO_SDK_PATH=`, to point it at your own install.

## The two builds

There are two hardware configurations, matching the two revisions of the custom board:

| | Radio | Build | Board it mirrors |
|---|---|---|---|
| **Pico 2 W on its own** | the on-board cyw43 | `-DPICO_BOARD=pico2_w -DKIOSK_TLS=ON` | v2 |
| **Pico 2 W + an ESP32-C3 DevKitM-1** | the ESP32, over a UART | `-DPICO_BOARD=pico2_w -DKIOSK_MODEM=ON` | v3 |

The first is the simpler thing to build: two boards and an HDMI cable. The second moves Wi-Fi, the
IP stack and TLS onto the ESP32, which is what the v3 board does, and is the configuration the
1080p work is developed on. Wiring for it is in [MODEM.md](MODEM.md), and the ESP32 side has to be
flashed separately with `idf.py`.

With `KIOSK_MODEM=ON`, leave `KIOSK_TLS=OFF`: the modem terminates TLS, so mbedTLS is not built
into this firmware at all.

## Firmware

```bash
cmake -S . -B build -G Ninja -DPICO_BOARD=pico_w -DKIOSK_TLS=OFF
cmake --build build
```

Output: `build/tvtop_kiosk.uf2`. Options:

| Option | Default | Meaning |
|---|---|---|
| `PICO_BOARD` | `pico_w` | `pico_w` (RP2040, DVI from PIO) or `pico2_w` (RP2350, DVI from HSTX). `pico2_w` is the one that reaches 1080p. |
| `KIOSK_TLS` | `OFF` | build mbedTLS so `https://` servers work (`build-tls/` is a second tree with it on) |
| `KIOSK_VIDEO_MODE` | per board | default mode until one is stored. RP2040: `720p30`, `720p30rb`, `480p60`, `720x480p60`, `960x540p60`, `1066x600p50` (default `720p30`). RP2350: `720p60`, `1080p30`, `1080p25`, `1080p24`, `960x540p60`, `480p60` (default `720p60`) |
| `KIOSK_MODEM` | `OFF` | take Wi-Fi from an ESP32 over a UART instead of the on-board cyw43 ([MODEM.md](MODEM.md)) |
| `KIOSK_NO_RADIO` | `OFF` | diagnostic build that never starts the radio |
| `KIOSK_NET_CHECK_URL` | Google's `generate_204` | the plain-HTTP URL used to detect sign-in portals |
| `KIOSK_SERVER_BASE` | `https://kiosk.tvtop.games` | default server (`server <url>` on the console overrides) |
| `KIOSK_WIFI_SSID` / `KIOSK_WIFI_PASSWORD` | empty | compile-in credentials to skip provisioning on a bench unit |

A bench build that skips the setup network and talks to a local server:

```bash
cmake -S . -B build -G Ninja -DPICO_BOARD=pico_w -DKIOSK_TLS=OFF \
  -DKIOSK_WIFI_SSID=MyNetwork -DKIOSK_WIFI_PASSWORD=hunter2 \
  -DKIOSK_SERVER_BASE=http://192.168.1.10:8080
cmake --build build
```

## Flashing

Hold BOOTSEL while plugging the Pico W in, then copy the UF2 to the `RPI-RP2` drive, or:

```bash
picotool load -f build/tvtop_kiosk.uf2 && picotool reboot
```

Then open the USB serial console (`screen /dev/tty.usbmodem* 115200` or any terminal) to watch
the boot banner and to provision (`docs/PROVISIONING.md`).

## Pico 2 W (RP2350)

The same sources build for the Pico 2 W. CMake picks the video backend from the board: HSTX
(`src/pico/scanout_hstx.c`) on the RP2350, PicoDVI's libdvi (`src/pico/scanout.c`) on the RP2040.
Use a separate build directory:

```bash
cmake -S . -B build-pico2w -G Ninja -DPICO_BOARD=pico2_w
cmake --build build-pico2w
picotool load -f -v -x build-pico2w/tvtop_kiosk.uf2
```

The default video mode is `720p60`; `1080p30` (and `1080p25` / `1080p24` for TVs), `960x540p60` and `480p60` are
the alternatives (console `mode`).
The Pico 2 W build uses the last sector of its 4 MB flash for the config and a 192 KB line pool.

## Host build (tests and the renderer)

```bash
cmake -S host -B host/build -G Ninja
cmake --build host/build
ctest --test-dir host/build --output-on-failure
host/build/render_frame test/fixtures/gc-classic.json --stats -o out.png
```

Fifteen test programs cover JSON streaming, colours, TMDS symbols, the palette, path parsing,
the geometry store (RAM and a fake-flash backend), the rasteriser, the line pool, fonts, the
frame decoder over every fixture, the TMDS run-length encoder, the config sector and the portal.

## Measured resources (pico_w, 2026-09-14)

| | KIOSK_TLS=OFF | KIOSK_TLS=ON |
|---|---|---|
| flash (text) | 658 KB of the 1 MB firmware region | 750 KB |
| static RAM (.data + .bss) | 227 KB | 227 KB (the TLS profile trades 28 KB of line pool for lwIP's bigger pools) |
| heap left for lwIP/cyw43 (and mbedTLS) | 30 KB | 29 KB — **marginal**: an mbedTLS handshake wants ~30–40 KB, so treat the RP2040 TLS build as experimental until it has been seen to connect |
| core-0 stack | 4 KB (SCRATCH_Y) | 4 KB |
| core-1 stack | 2 KB (SCRATCH_X) | 2 KB |

The largest static blocks: line pool 84 KB, decode/render scratch 28 KB, frame table 23 KB, TMDS
line buffers 23 KB, lwIP pbuf pool 12 KB.

Check core 1 stays in SRAM after changing its code:

```bash
arm-none-eabi-nm build/tvtop_kiosk.elf | grep -E ' (core1_main|tmds_rle_encode_line|dvi_dma_irq_handler)$'
```

All three must start with `2000`/`2004` (SRAM), never `1000` (flash).
