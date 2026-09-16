// Persistent configuration and the raw flash helpers.
//
// Concurrency model: every flash erase/program happens on core 0 with core-0 interrupts disabled
// (the cyw43 background IRQ included). Core 1 is deliberately NOT stopped: it runs the scanout
// entirely from SRAM (code and data, see scanout.h), so it never fetches from XIP while the flash
// is busy. That is what keeps video alive during a 50 ms sector erase. The flip side: none of the
// functions in this file may ever be called from core 1, and nothing that core 1 touches may live
// in flash. The XIP cache is invalidated after each operation so core 0 reads back fresh data.
//
// The host test (host/tests/test_flash_store.c) compiles this file with FLASH_STORE_HOST_TEST,
// which swaps the SDK flash primitives for a RAM-emulated sector.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "flash_store.h"

#ifdef FLASH_STORE_HOST_TEST
// Provided by the test: a 4 KB sector image, a fake clock and a fake mode-name lookup.
extern uint8_t host_flash_sector[];
extern uint32_t host_now_ms;
int host_video_mode_from_name(const char *name);
#define CONFIG_FLASH_PTR() (host_flash_sector)
#define VIDEO_MODE_FROM_NAME(n) ((uint8_t)host_video_mode_from_name(n))
#define VIDEO_MODE_LIMIT 6
#define FLASH_PAGE_SIZE 256u
#define FLASH_SECTOR_SIZE 4096u
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2u * 1024 * 1024)
#endif
static uint32_t now_ms(void) { return host_now_ms; }
#else
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/time.h"
#include "board.h"
#include "hardware/watchdog.h"
#define CONFIG_FLASH_PTR() flash_store_ptr(KIOSK_CONFIG_FLASH_OFFSET)
#define VIDEO_MODE_FROM_NAME(n) ((uint8_t)video_mode_from_name(n))
#define VIDEO_MODE_LIMIT ((uint8_t)VIDEO_MODE_COUNT)
static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
#endif

// The CMake build always defines these; the fallbacks keep a bare compile working.
#ifndef KIOSK_DEFAULT_SERVER_BASE
#define KIOSK_DEFAULT_SERVER_BASE "https://kiosk.tvtop.games"
#endif
#ifndef KIOSK_DEFAULT_WIFI_SSID
#define KIOSK_DEFAULT_WIFI_SSID ""
#endif
#ifndef KIOSK_DEFAULT_WIFI_PASSWORD
#define KIOSK_DEFAULT_WIFI_PASSWORD ""
#endif
#ifndef KIOSK_DEFAULT_VIDEO_MODE_NAME
#define KIOSK_DEFAULT_VIDEO_MODE_NAME "720p30"
#endif

#define CONFIG_CRC_LEN offsetof(kiosk_config_t, crc32)
#define CONFIG_SAVE_QUIET_MS 2000u

kiosk_config_t kiosk_config;

static bool dirty;
static uint32_t dirty_since_ms;

// ---- CRC-32 (IEEE 802.3, reflected, init/final 0xFFFFFFFF). Table-free: it runs once per
// config save/load over < 1 KB, so 8 iterations per byte is irrelevant and saves 1 KB of flash.
uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t config_crc(const kiosk_config_t *c) {
    return crc32_update(0, c, CONFIG_CRC_LEN);
}

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void config_defaults(void) {
    memset(&kiosk_config, 0, sizeof kiosk_config);
    kiosk_config.magic = CONFIG_MAGIC;
    kiosk_config.version = CONFIG_VERSION;
    kiosk_config.length = (uint16_t)sizeof(kiosk_config_t);
    copy_str(kiosk_config.wifi_ssid, sizeof kiosk_config.wifi_ssid, KIOSK_DEFAULT_WIFI_SSID);
    copy_str(kiosk_config.wifi_pass, sizeof kiosk_config.wifi_pass, KIOSK_DEFAULT_WIFI_PASSWORD);
    copy_str(kiosk_config.server_base, sizeof kiosk_config.server_base, KIOSK_DEFAULT_SERVER_BASE);
    // A trailing slash would double up when paths are appended.
    size_t n = strlen(kiosk_config.server_base);
    while (n > 0 && kiosk_config.server_base[n - 1] == '/') kiosk_config.server_base[--n] = 0;
    kiosk_config.video_mode = VIDEO_MODE_FROM_NAME(KIOSK_DEFAULT_VIDEO_MODE_NAME);
    kiosk_config.crc32 = config_crc(&kiosk_config);
}

// Everything read back from flash is untrusted: a half-written sector or an older layout must
// not turn into an unterminated string somewhere downstream.
static bool config_validate(kiosk_config_t *c) {
    if (c->magic != CONFIG_MAGIC || c->version != CONFIG_VERSION) return false;
    if (c->length != sizeof(kiosk_config_t)) return false;
    if (c->crc32 != config_crc(c)) return false;
    if (memchr(c->wifi_ssid, 0, sizeof c->wifi_ssid) == NULL) return false;
    if (memchr(c->wifi_pass, 0, sizeof c->wifi_pass) == NULL) return false;
    if (memchr(c->server_base, 0, sizeof c->server_base) == NULL) return false;
    if (memchr(c->token, 0, sizeof c->token) == NULL) return false;
    if (memchr(c->device_id, 0, sizeof c->device_id) == NULL) return false;
    if (memchr(c->next_url, 0, sizeof c->next_url) == NULL) return false;
    if (memchr(c->static_id, 0, sizeof c->static_id) == NULL) return false;
    if (c->video_mode >= VIDEO_MODE_LIMIT) return false;
    return true;
}

bool config_load(void) {
    kiosk_config_t tmp;
    memcpy(&tmp, CONFIG_FLASH_PTR(), sizeof tmp);
    if (config_validate(&tmp)) {
        kiosk_config = tmp;
        dirty = false;
        return true;
    }
    config_defaults();
    dirty = false;
    return false;
}

bool config_save(void) {
    kiosk_config.magic = CONFIG_MAGIC;
    kiosk_config.version = CONFIG_VERSION;
    kiosk_config.length = (uint16_t)sizeof(kiosk_config_t);
    kiosk_config.crc32 = config_crc(&kiosk_config);

    // The struct is not a multiple of the 256 B page; pad the tail with the erased value so the
    // page program never touches bytes we did not mean to set.
    static uint8_t page_img[((sizeof(kiosk_config_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE]
        __attribute__((aligned(4)));
    memset(page_img, 0xFF, sizeof page_img);
    memcpy(page_img, &kiosk_config, sizeof kiosk_config);

    flash_store_erase(KIOSK_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_store_program(KIOSK_CONFIG_FLASH_OFFSET, page_img, sizeof page_img);
    dirty = false;

    // Verify by reading back through XIP (the cache was invalidated by the helper).
    return memcmp(CONFIG_FLASH_PTR(), &kiosk_config, sizeof kiosk_config) == 0;
}

void config_mark_dirty(void) {
    dirty = true;
    dirty_since_ms = now_ms();
}

void config_poll(void) {
    if (dirty && (uint32_t)(now_ms() - dirty_since_ms) >= CONFIG_SAVE_QUIET_MS) config_save();
}

// ---- Raw helpers -----------------------------------------------------------------------------

#ifdef FLASH_STORE_HOST_TEST
// Host emulation: only the config sector exists; other offsets are ignored.
static bool host_in_sector(uint32_t offset, size_t len) {
    return offset >= KIOSK_CONFIG_FLASH_OFFSET && offset + len <= KIOSK_CONFIG_FLASH_OFFSET + FLASH_SECTOR_SIZE;
}
static void raw_erase(uint32_t offset, size_t len) {
    if (host_in_sector(offset, len)) memset(host_flash_sector + (offset - KIOSK_CONFIG_FLASH_OFFSET), 0xFF, len);
}
static void raw_program(uint32_t offset, const uint8_t *data, size_t len) {
    if (!host_in_sector(offset, len)) return;
    uint8_t *dst = host_flash_sector + (offset - KIOSK_CONFIG_FLASH_OFFSET);
    for (size_t i = 0; i < len; i++) dst[i] &= data[i];   // programming only clears bits, as real NOR does
}
static bool in_sram(const void *p) { (void)p; return true; }
#else
#if KIOSK_HSTX
// scanout_hstx.c: the ROM routines behind erase and program re-enter XIP with slow default timing.
void scanout_flash_timing_restore(void);
#define KIOSK_FLASH_TIMING_RESTORE() scanout_flash_timing_restore()
// Until the timing is reapplied, the flash may be running with the ROM's defaults, which are not
// guaranteed at 372 MHz: nothing between the ROM call and the restore may execute from flash.
#define KIOSK_FLASH_WRAPPER(name) __no_inline_not_in_flash_func(name)
#else
#define KIOSK_FLASH_TIMING_RESTORE() ((void)0)
#define KIOSK_FLASH_WRAPPER(name) name
#endif

static void KIOSK_FLASH_WRAPPER(raw_erase)(uint32_t offset, size_t len) {
    uint32_t s = save_and_disable_interrupts();
    flash_range_erase(offset, len);
    KIOSK_FLASH_TIMING_RESTORE();
    restore_interrupts(s);
    xip_cache_invalidate_all();
}
static void KIOSK_FLASH_WRAPPER(raw_program)(uint32_t offset, const uint8_t *data, size_t len) {
    uint32_t s = save_and_disable_interrupts();
    flash_range_program(offset, data, len);
    KIOSK_FLASH_TIMING_RESTORE();
    restore_interrupts(s);
    xip_cache_invalidate_all();
}
// flash_range_program reads its source with XIP disabled, so the source must not itself be in
// flash; and the SDK wants it word-aligned.
static bool in_sram(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= SRAM_BASE && a < SRAM_END && (a & 3u) == 0;
}
#endif

void flash_store_erase(uint32_t offset, size_t len) {
    if ((offset & (FLASH_SECTOR_SIZE - 1)) != 0 || len == 0) return;
    len = (len + FLASH_SECTOR_SIZE - 1) & ~(size_t)(FLASH_SECTOR_SIZE - 1);
    if (offset >= PICO_FLASH_SIZE_BYTES || len > PICO_FLASH_SIZE_BYTES - offset) return;
    // One 4 KB sector per interrupts-off window (~40 ms) instead of a 64 KB block erase (hundreds of
    // ms). The Wi-Fi driver runs from core-0 interrupts, and long gaps in servicing it were one
    // suspect for the radio wedging right after a board's geometry was written.
    for (size_t done = 0; done < len; done += FLASH_SECTOR_SIZE) {
        raw_erase(offset + (uint32_t)done, FLASH_SECTOR_SIZE);
#ifndef FLASH_STORE_HOST_TEST
        watchdog_update();
#endif
    }
}

void flash_store_program(uint32_t offset, const uint8_t *data, size_t len) {
    if ((offset & (FLASH_PAGE_SIZE - 1)) != 0 || (len & (FLASH_PAGE_SIZE - 1)) != 0 || len == 0) return;
    if (offset >= PICO_FLASH_SIZE_BYTES || len > PICO_FLASH_SIZE_BYTES - offset) return;
    if (in_sram(data)) {
        raw_program(offset, data, len);
        return;
    }
    // Unaligned or flash-resident source: stage one page at a time through an aligned RAM copy.
    static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    for (size_t done = 0; done < len; done += FLASH_PAGE_SIZE) {
        memcpy(page, data + done, FLASH_PAGE_SIZE);
        raw_program(offset + (uint32_t)done, page, FLASH_PAGE_SIZE);
    }
}
