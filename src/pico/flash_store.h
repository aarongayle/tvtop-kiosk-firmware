// Persistent configuration (last 4 KB flash sector) and low-level flash helpers used by the
// geometry cache. All flash writes go through here: core-0 interrupts are disabled for the
// duration and the XIP cache is invalidated afterwards. Core 1 (video) never touches flash.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kiosk_config.h"

#define CONFIG_MAGIC 0x4b56544bu   // 'KTVK'
#define CONFIG_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;            // sizeof(kiosk_config_t)
    char wifi_ssid[33];
    char wifi_pass[65];
    char server_base[KIOSK_MAX_URL];   // e.g. "https://kiosk.tvtop.games" (no trailing slash)
    char token[27];             // 26 chars; "" when unregistered
    char device_id[7];
    char next_url[KIOSK_MAX_URL];      // last persisted next_url ("" = start at /v1/config)
    char static_id[KIOSK_MAX_STATIC_ID];
    uint8_t video_mode;         // video_mode_t
    uint8_t flags;              // bit0: tls verification disabled (dev)
    uint8_t wifi_channel;       // 2.4 GHz channel last joined (1-14), 0 = unknown; picks the video clock
    uint8_t reserved[61];
    uint32_t crc32;             // of everything above
} kiosk_config_t;

extern kiosk_config_t kiosk_config;          // the in-RAM copy

bool config_load(void);                       // false → defaults loaded (KIOSK_DEFAULT_* from CMake)
bool config_save(void);                       // writes the sector (erase + program), ~50 ms
void config_defaults(void);
// Debounced save: marks dirty; config_poll() writes ≥ 2 s after the last mark.
void config_mark_dirty(void);
void config_poll(void);

// Raw flash helpers (offsets from flash start; 4 KB-aligned erase, 256 B-aligned program).
void flash_store_erase(uint32_t offset, size_t len);
void flash_store_program(uint32_t offset, const uint8_t *data, size_t len);
static inline const uint8_t *flash_store_ptr(uint32_t offset) { return (const uint8_t *)(uintptr_t)(0x10000000u + offset); }

uint32_t crc32_update(uint32_t crc, const void *data, size_t len);   // IEEE, init 0xFFFFFFFF, final xor
