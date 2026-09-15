# Hardware notes

## Wiring — Adafruit PiCowBell HSTX DVI (product 6363)

The PiCowBell plugs onto a Pico / Pico 2 and routes the TMDS pairs to the RP2350's HSTX pins.
On an RP2040 those same GPIOs are driven by PIO (PicoDVI). libdvi's `pico_sock_cfg` is exactly
this pinout, so `board.h` uses it unchanged.

| Signal | GPIO | libdvi lane |
|---|---|---|
| D0+ / D0− (blue) | 12 / 13 | `pins_tmds[0]` = 12 |
| D1+ / D1− (green) | 18 / 19 | `pins_tmds[1]` = 18 |
| D2+ / D2− (red) | 16 / 17 | `pins_tmds[2]` = 16 |
| CK+ / CK− | 14 / 15 | `pins_clk` = 14 (PWM slice 7) |

The positive line of every pair is the lower GPIO, so `invert_diffpairs` is false.

## Why 372 MHz at 1.30 V

The PIO serialiser shifts one TMDS bit per system-clock cycle, so the system clock *is* the TMDS
bit clock: 10 × pixel clock. 1280×720 needs 37.2 MHz of pixels at 30 Hz, hence 372 MHz. That is
~2.8× the RP2040's rated 133 MHz; PicoDVI's author measured a clean eye at 372 Mbit/s with the
core regulator raised, and 1.25–1.30 V is what the community runs 720p30 at. We use 1.30 V (the
regulator's ceiling without unlocking). Expect the chip to run warm; it does not need a heatsink.

Flash is clocked at clk_sys/4 = 93 MHz (`PICO_FLASH_SPI_CLKDIV=4`, set for the boot stage 2 in
`CMakeLists.txt`) because the Pico W's W25Q16 is rated for 133 MHz and the default divider of 2
would run it at 186 MHz. The cyw43 Wi-Fi SPI divider is recomputed from the actual clock in
`net_wifi.c` so the modem still sees ≈31 MHz.

### The 720p30 timing is not the CEA 720p30

CEA-861 720p30 keeps the 74.25 MHz pixel clock and doubles the horizontal blanking, which would
need a 742 MHz system clock. PicoDVI's mode is the 720p60 raster (1650×750) at half the pixel
clock: 22.5 kHz horizontal, 30.06 Hz vertical. Most TVs and monitors lock to it, some refuse. The
signal is otherwise a normal 1280×720 DVI stream.

## Modes

| Name | Output | clk_sys | Vreg | Notes |
|---|---|---|---|---|
| `720p30` (default) | 1280×720 | 372 MHz | 1.30 V | full canvas resolution |
| `720p30rb` | 1280×720 | 319.2 MHz | 1.25 V | CVT reduced blanking; gentler overclock, fewer sinks accept it |
| `480p60` | 640×480 | 252 MHz | 1.20 V | standard VGA timing, everything accepts it; the 16:9 canvas is letterboxed |

Select with the USB console (`mode 720p30rb`, then reboot) or at build time
(`-DKIOSK_VIDEO_MODE=480p60`). The setting persists in the config sector.

## No picture?

1. Try `mode 480p60`. If that shows the test pattern (`test` on the console) the TMDS path is
   fine and the TV rejected the 720p30 timing or the board could not reach 372 MHz.
2. Try `mode 720p30rb`: a 14% lower overclock.
3. Check `stats`: `late_lines` must stay 0. A non-zero count means core 1 could not encode a line
   in time (44 µs at 720p30); `max_line_cycles` says how close it got.
4. Long or poor HDMI cables matter at 372 Mbit/s from 3.3 V CMOS pins. Use a short cable.

## Scanline budget (720p30)

A line is 1650 pixels / 37.2 MHz = 44.4 µs = 16,500 cycles. The run-length encoder writes
1,920 words per line: about 1.3 cycles per word for long runs (unrolled store loop, one lane at a
time) plus a fixed cost per span. `stats` reports the worst line seen since the last query.
