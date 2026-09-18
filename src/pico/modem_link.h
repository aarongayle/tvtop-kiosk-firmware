// The RP2350 end of the UART link to the ESP32 modem: framing, a lossless DMA receive ring, and
// modem reset / bootloader strapping. Everything above this (net_modem.c, http_modem.c, the
// portal relay) is written against modem_link_send / a dispatch handler.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "modem_proto.h"

// Bring-up rig: Pico 2 W + PiCowBell HSTX DVI (which takes GPIO3, 4/5, 6/7 and 12-19) wired to an
// ESP32-C3-DevKitM-1. The v3 board overrides these to the RP2354A's 24/25 (UART1) and 23/29.
#ifndef MODEM_PIN_TX
#define MODEM_PIN_TX 8      // → ESP RX
#endif
#ifndef MODEM_PIN_RX
#define MODEM_PIN_RX 9      // ← ESP TX
#endif
#ifndef MODEM_PIN_EN
#define MODEM_PIN_EN 10     // → ESP CHIP_PU / EN, open-drain
#endif
#ifndef MODEM_PIN_BOOT
#define MODEM_PIN_BOOT 11   // → ESP IO9 strap, open-drain: low at reset = ROM bootloader
#endif

// 16 KB of receive ring. At 921600 baud that is 178 ms of link, which covers the longest stall in
// the main loop (a detailed 1080p render) without the modem having to pause. The HTTP window is
// held well under it so body data can never lap the consumer; see http_modem.c.
#ifndef MODEM_RX_RING_BITS
#define MODEM_RX_RING_BITS 14
#endif
#define MODEM_RX_RING_BYTES (1u << MODEM_RX_RING_BITS)
#ifndef MODEM_TX_RING_BYTES
#define MODEM_TX_RING_BYTES 4096u
#endif

// Called from modem_link_poll() in main context, once per complete frame.
typedef void (*modem_handler_t)(uint8_t type, const uint8_t *payload, uint16_t len);

// Configures the UART and both DMA channels, then resets the modem and waits (up to ~3 s) for its
// M_HELLO. Returns false if the modem never answered; the link keeps running either way, so a
// modem that boots late still joins once modem_link_poll() sees its hello.
bool modem_link_init(void);
void modem_link_set_handler(modem_handler_t h);

// Drains the receive ring, dispatches whole frames, and keeps the transmit DMA fed. Call as often
// as possible from the main loop.
void modem_link_poll(void);

// Frames and queues a message. Returns false only when the transmit ring is full, which means the
// modem has stopped consuming: callers treat it as a link error rather than retrying forever.
bool modem_link_send(uint8_t type, const void *payload, uint16_t len);
// Same, for a small header followed by a payload the caller does not want to stage into one
// buffer (socket writes, which are already sitting in the portal's connection buffer).
bool modem_link_send2(uint8_t type, const void *a, uint16_t alen, const void *b, uint16_t blen);
// Bytes the transmit ring can still take (minus framing overhead), so callers can size a write.
uint16_t modem_link_tx_room(void);

bool modem_link_ready(void);          // M_HELLO received and the version matched
const char *modem_link_fw(void);      // modem firmware version string, "" until hello
void modem_link_reset(bool bootloader);   // pulse EN, optionally with IO9 held low

// Counters for the `stats` console command.
typedef struct {
    uint32_t rx_frames, rx_bytes, rx_crc_errors, rx_resyncs, rx_overruns;
    uint32_t tx_frames, tx_bytes, tx_full;
} modem_link_stats_t;
const modem_link_stats_t *modem_link_stats(void);
