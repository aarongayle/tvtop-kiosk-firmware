// Boot: clocks first (the video mode decides the system clock), then video on core 1, then Wi-Fi
// and the kiosk loop on core 0. Everything after scanout_start() is a cooperative poll loop.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/clocks.h"
#include "board.h"
#include "scanout.h"
#include "linepool.h"
#include "palette.h"
#include "flash_store.h"
#include "net_wifi.h"
#include "http_client.h"
#include "kiosk_loop.h"
#include "provision.h"
#include "kiosk_config.h"
#include "builtin_frames.h"

linepool_t kiosk_linepool;
static uint8_t linepool_mem[KIOSK_LINEPOOL_BYTES] __attribute__((aligned(4)));

// Publishes a black line for every row so the first thing on the TV is black, not noise. Must run
// after scanout_init (which sets up the colour-pair table the lines are encoded against).
static void blank_pool(uint16_t w, uint16_t h) {
    static const uint8_t black_row[OUT_MAX_W];
    linepool_init(&kiosk_linepool, linepool_mem, sizeof linepool_mem, h, w);
    linepool_frame_begin(&kiosk_linepool);
    for (uint16_t y = 0; y < h; y++) {
        uint8_t *p = linepool_alloc(&kiosk_linepool, y, 64);
        uint16_t n = p ? scanout_encode_line(black_row, w, p, 64) : 0;
        if (!n) { linepool_commit_dup(&kiosk_linepool, y); continue; }
        linepool_commit(&kiosk_linepool, y, p, n);
    }
}

int main(void) {
    // The config lives in flash and only tells us which video mode to run; reading it at the
    // boot clock is fine. Everything else waits until the clock is final.
    config_load();
    const video_mode_info_t *mode = scanout_setup_clocks((video_mode_t)kiosk_config.video_mode,
        video_mode_clock_khz((video_mode_t)kiosk_config.video_mode, kiosk_config.wifi_channel));
    stdio_init_all();

    scanout_init(mode, &kiosk_linepool);
    scanout_set_entry(0, 0x000000);
    blank_pool(mode->w, mode->h);
    const char *expander = scanout_expander_name();
    scanout_start();   // returns once core 1 is running video from SRAM
    if (watchdog_hw->scratch[3] == 0x544d4f46u) scanout_set_tmds_enabled(false);   // "tmds off" diagnostic survives resets

    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    printf("\nTV-Top Kiosk " KIOSK_FW_VERSION " | %s %ux%u | clk_sys %lu kHz | %s expander | board %02x%02x%02x%02x\n",
           mode->name, mode->w, mode->h, (unsigned long)(clock_get_hz(clk_sys) / 1000), expander,
           id.id[4], id.id[5], id.id[6], id.id[7]);

#if KIOSK_NO_RADIO
    // Diagnostic build: the cyw43 radio is never started (its PIO program and DMA channels stay
    // unused), so video shares the chip with nothing but the USB console.
    printf("radio disabled (KIOSK_NO_RADIO build)\n");
    kiosk_loop_init_display_only();
    kiosk_show_builtin(BUILTIN_LABEL, "no radio", NULL);
    while (true) {
        console_poll();
        config_poll();
    }
#endif
    bool wifi_ok = net_wifi_init();
    // The radio occasionally fails to start (seen as "Failed to start CYW43" or a failed CLM load).
    // It never recovers without a reset, so reset after a pause long enough to use the console, at
    // most three times in a row; a fourth failure leaves the kiosk up with the console usable.
    const uint32_t RADIO_MAGIC = 0x52414400u;
    uint32_t radio_fail_boot_ms = 0;
    if (!wifi_ok) {
        uint32_t s = watchdog_hw->scratch[1];
        uint32_t n = (s & 0xffffff00u) == RADIO_MAGIC ? (s & 0xffu) : 0u;
        if (n < 3) {
            watchdog_hw->scratch[1] = RADIO_MAGIC | (n + 1u);
            radio_fail_boot_ms = to_ms_since_boot(get_absolute_time()) + 10000u;
            printf("wifi: cyw43 init failed; resetting in 10 s (attempt %lu of 3)\n", (unsigned long)(n + 1u));
        } else {
            printf("wifi: cyw43 init failed %lu times; staying up without Wi-Fi (console still works)\n", (unsigned long)n);
        }
    } else if (watchdog_hw->scratch[1]) {
        watchdog_hw->scratch[1] = 0;
    }
    kiosk_loop_init();
    // Hangs reset the kiosk rather than freezing the TV. 8 s covers the longest legitimate stall in
    // the loop (rendering a detailed board, a flash sector erase is fed separately).
    watchdog_enable(8000, true);

    while (true) {
        watchdog_update();
        if (radio_fail_boot_ms && to_ms_since_boot(get_absolute_time()) >= radio_fail_boot_ms) {
            printf("wifi: resetting to retry the radio\n");
            watchdog_reboot(0, 0, 100);
            for (;;) tight_loop_contents();
        }
        http_poll(http_client_get());
        net_wifi_poll();
        kiosk_loop_poll();
        provision_poll();
        console_poll();
        config_poll();
    }
}
