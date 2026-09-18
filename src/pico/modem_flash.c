// The ESP ROM bootloader's serial protocol, enough of it to write a flash image.
//
// No stub loader is uploaded. esptool normally pushes a small program into the ESP's RAM first
// because it is much faster, but it also means carrying that program around and keeping it in step
// with the chip. The ROM alone can do everything needed here — attach the flash, take compressed
// data, verify and reboot — and at 921600 baud a 600 KB image takes about fifteen seconds, which
// for something done at the factory and occasionally in the field is not worth a second binary.
//
// Frames are SLIP: 0xC0 delimits, 0xDB escapes (0xDB 0xDC for a literal 0xC0, 0xDB 0xDD for 0xDB).
// A command is <0x00, op, len16, checksum32, payload>; the reply is <0x01, op, len16, value32,
// payload> where the payload's first byte is zero on success.
#include <stdio.h>
#include <string.h>

#include "modem_flash.h"
#include "modem_link.h"
#include "kiosk_config.h"
#include "flash_store.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/watchdog.h"

#define SLIP_END 0xC0u
#define SLIP_ESC 0xDBu
#define SLIP_ESC_END 0xDCu
#define SLIP_ESC_ESC 0xDDu

enum {
    OP_FLASH_BEGIN = 0x02, OP_FLASH_DATA = 0x03, OP_FLASH_END = 0x04,
    OP_SYNC = 0x08, OP_SPI_ATTACH = 0x0D, OP_CHANGE_BAUDRATE = 0x0F,
    OP_FLASH_DEFL_BEGIN = 0x10, OP_FLASH_DEFL_DATA = 0x11, OP_FLASH_DEFL_END = 0x12,
};

#define ROM_BAUD 115200u            // where the ROM always starts
#define FLASH_BLOCK 1024u           // what the ROM accepts per data command
#define CHECKSUM_SEED 0xEFu

static const uint8_t *image_base(void) {
    return flash_store_ptr(KIOSK_MODEM_IMAGE_OFFSET);
}

bool modem_flash_image_present(uint32_t *uncompressed_len, uint32_t *compressed_len) {
    const modem_image_header_t *h = (const modem_image_header_t *)image_base();
    if (h->magic != MODEM_IMAGE_MAGIC || h->version != 1) return false;
    if (!h->compressed_len || h->compressed_len > 4u * 1024u * 1024u) return false;
    uint32_t crc = crc32_update(0, image_base() + sizeof *h, h->compressed_len);
    if (crc != h->crc32) return false;
    if (uncompressed_len) *uncompressed_len = h->uncompressed_len;
    if (compressed_len) *compressed_len = h->compressed_len;
    return true;
}

// ---- SLIP ---------------------------------------------------------------------------------------

static void slip_write(const uint8_t *p, size_t n) {
    // Escaped in short runs so the UART keeps moving rather than going byte at a time.
    uint8_t out[128];
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (o + 2 > sizeof out) { modem_link_raw_write(out, o); o = 0; }
        if (p[i] == SLIP_END) { out[o++] = SLIP_ESC; out[o++] = SLIP_ESC_END; }
        else if (p[i] == SLIP_ESC) { out[o++] = SLIP_ESC; out[o++] = SLIP_ESC_ESC; }
        else out[o++] = p[i];
    }
    if (o) modem_link_raw_write(out, o);
}

// Reads one SLIP frame into buf. Returns its length, or -1 on timeout. Leading rubbish (the ROM's
// own boot banner, at a baud rate we are not using) is skipped by waiting for a delimiter.
static int slip_read(uint8_t *buf, size_t cap, uint32_t timeout_ms) {
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + timeout_ms;
    bool in_frame = false;
    size_t n = 0;
    for (;;) {
        int32_t left = (int32_t)(deadline - to_ms_since_boot(get_absolute_time()));
        if (left <= 0) return -1;
        watchdog_update();
        int c = modem_link_raw_getc(left > 200 ? 200u : (uint32_t)left);
        if (c < 0) continue;
        if (c == SLIP_END) {
            if (!in_frame) { in_frame = true; n = 0; continue; }
            if (n == 0) continue;          // back-to-back delimiters
            return (int)n;
        }
        if (!in_frame) continue;
        if (c == SLIP_ESC) {
            int e = modem_link_raw_getc(200);
            if (e < 0) return -1;
            c = (e == SLIP_ESC_END) ? SLIP_END : (e == SLIP_ESC_ESC) ? SLIP_ESC : e;
        }
        if (n < cap) buf[n++] = (uint8_t)c;
    }
}

// Sends a command and waits for the reply to that same opcode. `value` is the reply's 32-bit field,
// which only SPI_FLASH_MD5 and friends use.
static bool command(uint8_t op, const uint8_t *data, uint16_t len, uint32_t checksum,
                    uint32_t timeout_ms, uint32_t *value) {
    uint8_t hdr[8] = { 0x00, op, (uint8_t)(len & 0xff), (uint8_t)(len >> 8),
                       (uint8_t)(checksum), (uint8_t)(checksum >> 8),
                       (uint8_t)(checksum >> 16), (uint8_t)(checksum >> 24) };
    uint8_t end = SLIP_END;
    modem_link_raw_write(&end, 1);
    slip_write(hdr, sizeof hdr);
    if (len) slip_write(data, len);
    modem_link_raw_write(&end, 1);

    // The ROM answers other things too (and echoes during sync), so read until the opcode matches.
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + timeout_ms;
    uint8_t rsp[256];
    for (;;) {
        int32_t left = (int32_t)(deadline - to_ms_since_boot(get_absolute_time()));
        if (left <= 0) return false;
        int n = slip_read(rsp, sizeof rsp, (uint32_t)left);
        if (n < 10) continue;
        if (rsp[0] != 0x01 || rsp[1] != op) continue;
        uint16_t plen = (uint16_t)(rsp[2] | (rsp[3] << 8));
        if (value) memcpy(value, rsp + 4, 4);
        if (8u + plen > (uint16_t)n) return false;
        // The status is the first byte of the trailing status field, whether the ROM sends two
        // bytes or four; for these commands the payload is nothing else.
        if (plen >= 2 && rsp[8] != 0) {
            printf("modem-flash: op 0x%02x refused (err 0x%02x)\n", op, rsp[9]);
            return false;
        }
        return true;
    }
}

static bool sync_rom(void) {
    uint8_t payload[36];
    payload[0] = 0x07; payload[1] = 0x07; payload[2] = 0x12; payload[3] = 0x20;
    memset(payload + 4, 0x55, 32);
    for (int attempt = 0; attempt < 12; attempt++) {
        modem_link_raw_purge();
        if (command(OP_SYNC, payload, sizeof payload, 0, 300, NULL)) {
            // The ROM answers a sync several times over; swallow the rest so they are not mistaken
            // for the reply to whatever comes next.
            sleep_ms(50);
            modem_link_raw_purge();
            return true;
        }
        watchdog_update();
    }
    return false;
}

static uint32_t data_checksum(const uint8_t *p, uint32_t n) {
    uint32_t c = CHECKSUM_SEED;
    while (n--) c ^= *p++;
    return c;
}

// ---- the flash itself ---------------------------------------------------------------------------

bool modem_flash_run(void) {
    uint32_t ulen = 0, clen = 0;
    if (!modem_flash_image_present(&ulen, &clen)) {
        printf("modem-flash: no valid image at 0x%08x — build and install one (tools/mkmodemimg.py)\n",
               (unsigned)KIOSK_MODEM_IMAGE_OFFSET);
        return false;
    }
    const modem_image_header_t *h = (const modem_image_header_t *)image_base();
    const uint8_t *payload = image_base() + sizeof *h;

    printf("modem-flash: %lu bytes (%lu compressed) to ESP flash 0x%lx\n",
           (unsigned long)ulen, (unsigned long)clen, (unsigned long)h->esp_offset);

    bool ok = false;
    modem_link_raw_mode(true);
    modem_link_set_baud(ROM_BAUD);
    modem_link_reset(true);          // into the ROM bootloader: IO9 low as it leaves reset
    sleep_ms(100);

    if (!sync_rom()) { printf("modem-flash: the modem's bootloader did not answer\n"); goto done; }
    printf("modem-flash: bootloader up\n");

    {
        uint8_t attach[8] = {0};     // no special SPI pin configuration: the module's own flash
        if (!command(OP_SPI_ATTACH, attach, sizeof attach, 0, 3000, NULL)) {
            printf("modem-flash: SPI attach failed\n");
            goto done;
        }
    }

    // Faster than the ROM's 115200, and a rate this wiring is already proven at by the link itself.
    {
        uint8_t baud[8];
        uint32_t want = MODEM_BAUD, old = 0;
        memcpy(baud, &want, 4);
        memcpy(baud + 4, &old, 4);
        if (command(OP_CHANGE_BAUDRATE, baud, sizeof baud, 0, 3000, NULL)) {
            sleep_ms(50);
            modem_link_set_baud(MODEM_BAUD);
            sleep_ms(50);
            modem_link_raw_purge();
        } else {
            printf("modem-flash: staying at %u baud\n", (unsigned)ROM_BAUD);
        }
    }

    {
        uint32_t blocks = (clen + FLASH_BLOCK - 1u) / FLASH_BLOCK;
        // The ROM wants the erase size rounded up to whole write blocks, not the raw length.
        uint32_t erase = ((ulen + FLASH_BLOCK - 1u) / FLASH_BLOCK) * FLASH_BLOCK;
        uint8_t begin[16];
        memcpy(begin, &erase, 4);
        memcpy(begin + 4, &blocks, 4);
        uint32_t bs = FLASH_BLOCK;
        memcpy(begin + 8, &bs, 4);
        memcpy(begin + 12, &h->esp_offset, 4);
        printf("modem-flash: erasing...\n");
        // Erasing a megabyte takes the ROM several seconds, and the watchdog is 8 s, so the wait
        // inside slip_read feeds it.
        if (!command(OP_FLASH_DEFL_BEGIN, begin, sizeof begin, 0, 30000, NULL)) {
            printf("modem-flash: erase failed\n");
            goto done;
        }

        uint8_t pkt[16 + FLASH_BLOCK];
        uint32_t sent = 0;
        for (uint32_t seq = 0; seq < blocks; seq++) {
            uint32_t n = clen - sent;
            if (n > FLASH_BLOCK) n = FLASH_BLOCK;
            memcpy(pkt, &n, 4);
            memcpy(pkt + 4, &seq, 4);
            memset(pkt + 8, 0, 8);
            memcpy(pkt + 16, payload + sent, n);
            if (!command(OP_FLASH_DEFL_DATA, pkt, (uint16_t)(16 + n),
                         data_checksum(pkt + 16, n), 10000, NULL)) {
                printf("modem-flash: block %lu of %lu failed\n", (unsigned long)seq, (unsigned long)blocks);
                goto done;
            }
            sent += n;
            if ((seq % 64u) == 0 || seq + 1 == blocks)
                printf("modem-flash: %lu%%\n", (unsigned long)((sent * 100ull) / clen));
            watchdog_update();
        }
    }

    {
        uint8_t fin[4] = { 0, 0, 0, 0 };   // 0 = reboot into the new firmware
        if (!command(OP_FLASH_DEFL_END, fin, sizeof fin, 0, 5000, NULL))
            printf("modem-flash: the modem did not acknowledge the end of the write\n");
    }
    ok = true;

done:
    modem_link_set_baud(MODEM_BAUD);
    modem_link_raw_mode(false);
    // Whatever happened, leave the modem running its firmware rather than sitting in download mode.
    modem_link_reset(false);
    if (!ok) return false;

    printf("modem-flash: written; waiting for the modem to come back\n");
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + 10000u;
    while ((int32_t)(to_ms_since_boot(get_absolute_time()) - deadline) < 0) {
        watchdog_update();
        modem_link_poll();
        if (modem_link_ready()) {
            printf("modem-flash: done — modem fw %s\n", modem_link_fw());
            return true;
        }
        sleep_ms(10);
    }
    printf("modem-flash: the modem did not say hello; it may not have taken the image\n");
    return false;
}
