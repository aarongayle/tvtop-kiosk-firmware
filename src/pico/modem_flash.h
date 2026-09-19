// Reflashing the ESP modem from the RP2350, over the UART they already talk on.
//
// This is why v3 has one USB-C port. The modem's UART0 is also its ROM bootloader, so the kiosk can
// hold it in download mode and write its flash directly — which means "pre-flashed" production is
// one image for the RP2354A that carries the modem's inside it, and a modem that will not boot is
// recoverable in the field without opening anything.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The blob the image lives in (tools/mkmodemimg.py writes it; KIOSK_MODEM_IMAGE_OFFSET says where).
// The payload is the ESP's whole flash image, zlib-compressed — the ROM inflates it on the way in,
// so compressing costs us nothing and roughly halves both the flash it occupies and its time on
// the wire.
#define MODEM_IMAGE_MAGIC 0x314D444Du   // 'MDM1'

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t esp_offset;        // where in the ESP's flash it goes (0: the whole thing)
    uint32_t uncompressed_len;
    uint32_t compressed_len;
    uint32_t crc32;             // of the compressed payload
    uint8_t reserved[40];
} modem_image_header_t;         // 64 bytes

// True when a usable image is present (magic, version and CRC all check out). Fills in the version
// and size for the console to report.
bool modem_flash_image_present(uint32_t *uncompressed_len, uint32_t *compressed_len);

// Puts the modem into its ROM bootloader, writes the stored image, reboots it and waits for the
// hello. Blocking, tens of seconds, and it feeds the watchdog as it goes. Returns false with a
// reason printed; the modem is left reset either way, so a failure does not leave it in download
// mode.
bool modem_flash_run(void);
