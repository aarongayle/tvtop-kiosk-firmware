# Building and flashing

## Prerequisites (already installed on this Mac)

| Tool | Where |
|---|---|
| pico-sdk 2.2.0 (with cyw43-driver, lwip, mbedtls, tinyusb submodules) | `~/.pico-sdk/sdk/2.2.0` |
| Arm GNU Toolchain 15.3.rel1 (arm-none-eabi, with newlib) | `~/.pico-sdk/toolchain/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi` |
| cmake, ninja, picotool | Homebrew |
| resvg (only for the reference renders) | Homebrew |

`CMakeLists.txt` defaults `PICO_SDK_PATH` and `PICO_TOOLCHAIN_PATH` to those locations; export
either environment variable to override.

## Firmware

```bash
cmake -S . -B build -G Ninja -DPICO_BOARD=pico_w -DKIOSK_TLS=OFF
cmake --build build
```

Output: `build/tvtop_kiosk.uf2`. Options:

| Option | Default | Meaning |
|---|---|---|
| `PICO_BOARD` | `pico_w` | `pico_w` or `pico2_w` (the Pico 2 W build is untested) |
| `KIOSK_TLS` | `OFF` | build mbedTLS so `https://` servers work (`build-tls/` is a second tree with it on) |
| `KIOSK_VIDEO_MODE` | `720p30` | default mode until one is stored: `720p30`, `720p30rb`, `480p60`, `720x480p60`, `960x540p60`, `1066x600p50` |
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
