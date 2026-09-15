// Boot: clocks first (the video mode decides the system clock), then video on core 1, then Wi-Fi
// and the kiosk loop on core 0. Everything after scanout_start() is a cooperative poll loop.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
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

linepool_t kiosk_linepool;
static uint8_t linepool_mem[KIOSK_LINEPOOL_BYTES] __attribute__((aligned(4)));

// Publishes a black line for every row so the first thing on the TV is black, not noise.
static void blank_pool(uint16_t w, uint16_t h) {
    linepool_init(&kiosk_linepool, linepool_mem, sizeof linepool_mem, h, w);
    linepool_frame_begin(&kiosk_linepool);
    for (uint16_t y = 0; y < h; y++) {
        uint8_t *p = linepool_alloc(&kiosk_linepool, y, 2 * ((w + 255) / 256));
        if (!p) { linepool_commit_dup(&kiosk_linepool, y); continue; }
        uint16_t n = 0;
        for (uint32_t x = 0; x < w; x += 256) { p[n++] = 0; p[n++] = (uint8_t)((w - x >= 256 ? 256 : w - x) - 1); }
        linepool_commit(&kiosk_linepool, y, p, n);
    }
}

int main(void) {
    // The config lives in flash and only tells us which video mode to run; reading it at the
    // boot clock is fine. Everything else waits until the clock is final.
    config_load();
    const video_mode_info_t *mode = scanout_setup_clocks((video_mode_t)kiosk_config.video_mode);
    stdio_init_all();

    blank_pool(mode->w, mode->h);
    scanout_init(mode, &kiosk_linepool);
    scanout_set_entry(0, 0x000000);
    scanout_start();   // returns once core 1 is running video from SRAM

    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    printf("\nTV-Top Kiosk " KIOSK_FW_VERSION " | %s %ux%u | clk_sys %lu kHz | board %02x%02x%02x%02x\n",
           mode->name, mode->w, mode->h, (unsigned long)(clock_get_hz(clk_sys) / 1000),
           id.id[4], id.id[5], id.id[6], id.id[7]);

    bool wifi_ok = net_wifi_init();
    if (!wifi_ok) printf("wifi: cyw43 init failed; the console still works\n");
    kiosk_loop_init();

    while (true) {
        http_poll(http_client_get());
        net_wifi_poll();
        kiosk_loop_poll();
        provision_poll();
        console_poll();
        config_poll();
    }
}
