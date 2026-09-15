// flash_store.c on the host: the config sector is a RAM image with NOR semantics (erase → 0xFF,
// program only clears bits) so load/save/validate/debounce and the raw helpers' bounds checks
// can be exercised without a device.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_STORE_HOST_TEST 1
#include "../../src/pico/flash_store.c"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

uint8_t host_flash_sector[4096];
uint32_t host_now_ms;
int host_video_mode_from_name(const char *name) {
    if (strcmp(name, "720p30rb") == 0) return 1;
    if (strcmp(name, "480p60") == 0) return 2;
    return 0;
}

static void erase_sector(void) { memset(host_flash_sector, 0xFF, sizeof host_flash_sector); }

static void test_crc32(void) {
    // Standard check value for "123456789".
    CHECK(crc32_update(0, "123456789", 9) == 0xCBF43926u);
    CHECK(crc32_update(0, "", 0) == 0);
    // Incremental == one-shot.
    uint32_t a = crc32_update(0, "1234", 4);
    a = crc32_update(a, "56789", 5);
    CHECK(a == 0xCBF43926u);
}

static void test_defaults_on_blank(void) {
    erase_sector();
    CHECK(config_load() == false);
    CHECK(kiosk_config.magic == CONFIG_MAGIC);
    CHECK(kiosk_config.version == CONFIG_VERSION);
    CHECK(kiosk_config.length == sizeof(kiosk_config_t));
    CHECK(strcmp(kiosk_config.server_base, "https://kiosk.tvtop.games") == 0);
    CHECK(kiosk_config.wifi_ssid[0] == 0);
    CHECK(kiosk_config.token[0] == 0);
    CHECK(kiosk_config.video_mode == 0);
    CHECK(kiosk_config.crc32 == crc32_update(0, &kiosk_config, offsetof(kiosk_config_t, crc32)));
}

static void test_save_load_roundtrip(void) {
    erase_sector();
    config_load();
    strcpy(kiosk_config.wifi_ssid, "Home Net");
    strcpy(kiosk_config.wifi_pass, "hunter22");
    strcpy(kiosk_config.token, "k7f2qmabcdefghjklmnpqrstuv");
    strcpy(kiosk_config.device_id, "vy6kx4");
    strcpy(kiosk_config.next_url, "https://kiosk.tvtop.games/v1/frame/k7f2qm?rev=1&s=s-a8d91f");
    strcpy(kiosk_config.static_id, "s-a8d91f");
    kiosk_config.video_mode = 2;
    kiosk_config.crc32 = 0;   // save must recompute it
    CHECK(config_save());
    // The image starts at the sector base and the tail of the last page stays erased.
    CHECK(memcmp(host_flash_sector, &kiosk_config, sizeof kiosk_config) == 0);
    size_t padded = ((sizeof(kiosk_config_t) + 255) / 256) * 256;
    for (size_t i = sizeof(kiosk_config_t); i < padded; i++) CHECK(host_flash_sector[i] == 0xFF);
    CHECK(host_flash_sector[padded] == 0xFF);

    memset(&kiosk_config, 0, sizeof kiosk_config);
    CHECK(config_load() == true);
    CHECK(strcmp(kiosk_config.wifi_ssid, "Home Net") == 0);
    CHECK(strcmp(kiosk_config.wifi_pass, "hunter22") == 0);
    CHECK(strcmp(kiosk_config.token, "k7f2qmabcdefghjklmnpqrstuv") == 0);
    CHECK(strcmp(kiosk_config.device_id, "vy6kx4") == 0);
    CHECK(strcmp(kiosk_config.static_id, "s-a8d91f") == 0);
    CHECK(kiosk_config.video_mode == 2);
}

static void test_corruption_rejected(void) {
    erase_sector();
    config_load();
    strcpy(kiosk_config.wifi_ssid, "x");
    CHECK(config_save());

    // One flipped bit in the payload → crc mismatch → defaults.
    host_flash_sector[offsetof(kiosk_config_t, wifi_ssid)] ^= 0x01;
    CHECK(config_load() == false);
    CHECK(kiosk_config.wifi_ssid[0] == 0);

    // Wrong version.
    strcpy(kiosk_config.wifi_ssid, "y");
    CHECK(config_save());
    kiosk_config_t *img = (kiosk_config_t *)host_flash_sector;
    uint16_t v = CONFIG_VERSION + 1;
    // Emulate "an older layout": rewrite version and fix the crc so only the version check trips.
    memcpy(&img->version, &v, sizeof v);
    uint32_t crc = crc32_update(0, img, offsetof(kiosk_config_t, crc32));
    memcpy(&img->crc32, &crc, sizeof crc);
    CHECK(config_load() == false);

    // Wrong length with a valid crc.
    strcpy(kiosk_config.wifi_ssid, "z");
    CHECK(config_save());
    uint16_t l = sizeof(kiosk_config_t) - 4;
    memcpy(&img->length, &l, sizeof l);
    crc = crc32_update(0, img, offsetof(kiosk_config_t, crc32));
    memcpy(&img->crc32, &crc, sizeof crc);
    CHECK(config_load() == false);

    // Unterminated string with a valid crc.
    strcpy(kiosk_config.wifi_ssid, "w");
    CHECK(config_save());
    memset(img->server_base, 'A', sizeof img->server_base);
    crc = crc32_update(0, img, offsetof(kiosk_config_t, crc32));
    memcpy(&img->crc32, &crc, sizeof crc);
    CHECK(config_load() == false);

    // Out-of-range video mode with a valid crc.
    strcpy(kiosk_config.wifi_ssid, "v");
    CHECK(config_save());
    img->video_mode = 7;
    crc = crc32_update(0, img, offsetof(kiosk_config_t, crc32));
    memcpy(&img->crc32, &crc, sizeof crc);
    CHECK(config_load() == false);
}

static void test_debounce(void) {
    erase_sector();
    config_load();
    host_now_ms = 1000;
    strcpy(kiosk_config.wifi_ssid, "debounced");
    config_mark_dirty();
    config_poll();
    CHECK(host_flash_sector[0] == 0xFF);            // nothing written yet
    host_now_ms = 2500;
    config_poll();
    CHECK(host_flash_sector[0] == 0xFF);            // still inside the quiet window
    host_now_ms = 2900;
    config_mark_dirty();                             // a new mark restarts the window
    host_now_ms = 4000;
    config_poll();
    CHECK(host_flash_sector[0] == 0xFF);
    host_now_ms = 4900;
    config_poll();
    CHECK(host_flash_sector[0] != 0xFF);
    kiosk_config_t img;
    memcpy(&img, host_flash_sector, sizeof img);
    CHECK(strcmp(img.wifi_ssid, "debounced") == 0);
    // Once written, polling again is a no-op (erase the sector to see whether it writes).
    erase_sector();
    host_now_ms = 9000;
    config_poll();
    CHECK(host_flash_sector[0] == 0xFF);
    // Wrap-around safe: dirty near UINT32_MAX, poll after the wrap.
    host_now_ms = 0xFFFFFF00u;
    config_mark_dirty();
    host_now_ms = 0x00000800u;   // 0x900 ms after the mark, across the wrap
    config_poll();
    CHECK(host_flash_sector[0] != 0xFF);
}

static void test_raw_helpers_bounds(void) {
    erase_sector();
    static uint8_t page[256];
    memset(page, 0x55, sizeof page);
    // Misaligned offset / length: ignored.
    flash_store_program(KIOSK_CONFIG_FLASH_OFFSET + 1, page, 256);
    flash_store_program(KIOSK_CONFIG_FLASH_OFFSET, page, 100);
    flash_store_program(KIOSK_CONFIG_FLASH_OFFSET, page, 0);
    CHECK(host_flash_sector[0] == 0xFF && host_flash_sector[256] == 0xFF);
    // Past the end of flash: ignored.
    flash_store_program(PICO_FLASH_SIZE_BYTES, page, 256);
    flash_store_program(PICO_FLASH_SIZE_BYTES - 256, page, 512);
    flash_store_erase(PICO_FLASH_SIZE_BYTES, 4096);
    flash_store_erase(KIOSK_CONFIG_FLASH_OFFSET + 8, 4096);   // unaligned erase ignored
    // A good program lands, only clearing bits.
    flash_store_program(KIOSK_CONFIG_FLASH_OFFSET + 256, page, 256);
    CHECK(host_flash_sector[256] == 0x55 && host_flash_sector[511] == 0x55 && host_flash_sector[512] == 0xFF);
    memset(page, 0xAA, sizeof page);
    flash_store_program(KIOSK_CONFIG_FLASH_OFFSET + 256, page, 256);
    CHECK(host_flash_sector[256] == 0x00);   // 0x55 & 0xAA
    // Erase rounds a short length up to the sector.
    flash_store_erase(KIOSK_CONFIG_FLASH_OFFSET, 1);
    CHECK(host_flash_sector[256] == 0xFF);
}

static void test_save_after_defaults_roundtrips_exactly(void) {
    // config_save must produce bytes that config_load accepts even when nothing was changed,
    // and the in-RAM copy must equal the flash copy (the verify path).
    erase_sector();
    config_defaults();
    CHECK(config_save());
    kiosk_config_t before = kiosk_config;
    memset(&kiosk_config, 0xAB, sizeof kiosk_config);
    CHECK(config_load());
    CHECK(memcmp(&before, &kiosk_config, sizeof before) == 0);
}

int main(void) {
    test_crc32();
    test_defaults_on_blank();
    test_save_load_roundtrip();
    test_corruption_rejected();
    test_debounce();
    test_raw_helpers_bounds();
    test_save_after_defaults_roundtrips_exactly();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_flash_store: ok\n");
    return 0;
}
