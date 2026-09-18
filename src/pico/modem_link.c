// UART framing to the ESP32 modem, with DMA in both directions.
//
// Receive is a hardware ring: one DMA channel in ENDLESS mode (an RP2350 feature the RP2040 lacks)
// writes UART bytes into a power-of-two buffer for ever, wrapping its own write address. Nothing
// the CPU does can lose a byte — which is the whole point, because core 0 disappears for a hundred
// milliseconds at a time to render a frame or erase a flash sector, and the modem has no way to
// know. modem_link_poll() reads the DMA's write pointer to find out how far the producer got.
//
// Both FIFOs are disabled. The PL011 raises its DMA request from the FIFO trigger level, so with
// FIFOs on, the last few bytes of a burst can sit unclaimed until more arrive — harmless for a
// stream, fatal for a request/response link where the stalled bytes are the end of the reply. With
// the FIFO off the request is per byte, and a byte at 921600 baud lasts 10.8 µs, which is an age
// next to DMA arbitration even with HSTX holding the bus priority bits.
#include <stdio.h>
#include <string.h>

#include "modem_link.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"

#define MODEM_UART uart1

static uint8_t rx_ring[MODEM_RX_RING_BYTES] __attribute__((aligned(MODEM_RX_RING_BYTES)));
static uint8_t tx_ring[MODEM_TX_RING_BYTES] __attribute__((aligned(4)));

static int rx_dma = -1, tx_dma = -1;
static uint32_t rx_tail;             // bytes consumed, monotonic
static uint32_t rx_head_idx;         // last known DMA write index, for the wrap arithmetic
static uint32_t tx_head, tx_tail;    // monotonic
static uint32_t tx_inflight;

static modem_handler_t handler;
static modem_link_stats_t stats;
static bool ready;
static char modem_fw[24];

// ---- receive parser ---------------------------------------------------------------------------

static void dispatch(uint8_t type, const uint8_t *p, uint16_t len);

typedef enum { S_SYNC0, S_SYNC1, S_HEAD, S_PAYLOAD, S_CRC } parse_state_t;
static parse_state_t pstate;
static uint8_t phead[4];             // type, flags, len_lo, len_hi
static uint16_t pfill;
static uint16_t plen;
static uint16_t pcrc_want;
static uint16_t pcrc;
static uint8_t pbuf[MODEM_MAX_PAYLOAD];

static void parse_reset(bool count_resync) {
    if (count_resync && pstate != S_SYNC0) stats.rx_resyncs++;
    pstate = S_SYNC0;
    pfill = 0;
}

static void parse_byte(uint8_t b) {
    switch (pstate) {
    case S_SYNC0:
        if (b == MODEM_SYNC0) pstate = S_SYNC1;
        return;
    case S_SYNC1:
        // A5 A5 is a plausible run in binary noise; stay armed on the second A5 rather than
        // dropping the sync we may already have.
        if (b == MODEM_SYNC1) { pstate = S_HEAD; pfill = 0; }
        else if (b != MODEM_SYNC0) pstate = S_SYNC0;
        return;
    case S_HEAD:
        phead[pfill++] = b;
        if (pfill < 4) return;
        plen = (uint16_t)(phead[2] | (phead[3] << 8));
        if (plen > MODEM_MAX_PAYLOAD) { stats.rx_resyncs++; parse_reset(false); return; }
        pcrc = modem_crc16(MODEM_CRC_INIT, phead, 4);
        pfill = 0;
        pstate = plen ? S_PAYLOAD : S_CRC;
        return;
    case S_PAYLOAD:
        pbuf[pfill++] = b;
        if (pfill < plen) return;
        pcrc = modem_crc16(pcrc, pbuf, plen);
        pfill = 0;
        pstate = S_CRC;
        return;
    case S_CRC:
        if (pfill == 0) { pcrc_want = b; pfill = 1; return; }
        pcrc_want = (uint16_t)(pcrc_want | (b << 8));
        pstate = S_SYNC0;
        pfill = 0;
        if (pcrc_want != pcrc) { stats.rx_crc_errors++; return; }
        stats.rx_frames++;
        stats.rx_bytes += plen;
        dispatch(phead[0], pbuf, plen);
        return;
    }
}

// ---- transmit ---------------------------------------------------------------------------------

static void tx_kick(void) {
    if (tx_dma < 0) return;
    if (dma_channel_is_busy((uint)tx_dma)) return;
    if (tx_inflight) { tx_tail += tx_inflight; tx_inflight = 0; }
    uint32_t avail = tx_head - tx_tail;
    if (!avail) return;
    uint32_t idx = tx_tail & (MODEM_TX_RING_BYTES - 1u);
    uint32_t n = MODEM_TX_RING_BYTES - idx;
    if (n > avail) n = avail;
    tx_inflight = n;
    dma_channel_set_read_addr((uint)tx_dma, &tx_ring[idx], false);
    dma_channel_set_transfer_count((uint)tx_dma, dma_encode_transfer_count(n), true);
}

static uint32_t tx_free(void) {
    return MODEM_TX_RING_BYTES - (tx_head - tx_tail);
}

static void tx_push(const uint8_t *p, uint16_t n) {
    while (n--) {
        tx_ring[tx_head & (MODEM_TX_RING_BYTES - 1u)] = *p++;
        tx_head++;
    }
}

uint16_t modem_link_tx_room(void) {
    tx_kick();   // retire anything the DMA finished, so the answer is not stale
    uint32_t free = tx_free();
    uint32_t over = MODEM_HDR_BYTES + MODEM_CRC_BYTES + 8u;   // framing plus a message header
    if (free <= over) return 0;
    free -= over;
    return free > MODEM_MAX_PAYLOAD ? (uint16_t)MODEM_MAX_PAYLOAD : (uint16_t)free;
}

bool modem_link_send2(uint8_t type, const void *a, uint16_t alen, const void *b, uint16_t blen) {
    uint16_t len = (uint16_t)(alen + blen);
    if (len > MODEM_MAX_PAYLOAD) return false;
    tx_kick();
    uint32_t need = MODEM_HDR_BYTES + len + MODEM_CRC_BYTES;
    if (tx_free() < need) { stats.tx_full++; return false; }

    uint8_t hdr[MODEM_HDR_BYTES] = { MODEM_SYNC0, MODEM_SYNC1, type, 0, (uint8_t)(len & 0xff), (uint8_t)(len >> 8) };
    uint16_t crc = modem_crc16(MODEM_CRC_INIT, hdr + 2, 4);
    if (alen) crc = modem_crc16(crc, a, alen);
    if (blen) crc = modem_crc16(crc, b, blen);
    uint8_t tail[MODEM_CRC_BYTES] = { (uint8_t)(crc & 0xff), (uint8_t)(crc >> 8) };

    tx_push(hdr, MODEM_HDR_BYTES);
    if (alen) tx_push(a, alen);
    if (blen) tx_push(b, blen);
    tx_push(tail, MODEM_CRC_BYTES);
    stats.tx_frames++;
    stats.tx_bytes += len;
    tx_kick();
    return true;
}

bool modem_link_send(uint8_t type, const void *payload, uint16_t len) {
    return modem_link_send2(type, payload, len, NULL, 0);
}

// ---- modem reset ------------------------------------------------------------------------------

// EN and IO9 are driven open-drain: released means input (the modem's own pull-up, or the DevKit's
// RC and buttons, hold them high). Pushing them high would fight the DevKit's RST/BOOT buttons and
// the USB bridge's auto-reset transistors, all of which pull down.
static void od_init(uint pin) {
    gpio_init(pin);
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_IN);
}
static void od_drive(uint pin, bool low) {
    if (low) gpio_set_dir(pin, GPIO_OUT);   // the output latch is already 0
    else gpio_set_dir(pin, GPIO_IN);
}

void modem_link_reset(bool bootloader) {
    ready = false;
    modem_fw[0] = 0;
    od_drive(MODEM_PIN_BOOT, bootloader);
    od_drive(MODEM_PIN_EN, true);
    sleep_ms(20);
    od_drive(MODEM_PIN_EN, false);
    // The strap is sampled as the chip leaves reset; the ROM then wants IO9 back as an input.
    sleep_ms(bootloader ? 60 : 5);
    od_drive(MODEM_PIN_BOOT, false);
    parse_reset(false);
}

// ---- init and poll ----------------------------------------------------------------------------

void modem_link_set_handler(modem_handler_t h) { handler = h; }
bool modem_link_ready(void) { return ready; }
const char *modem_link_fw(void) { return modem_fw; }
const modem_link_stats_t *modem_link_stats(void) { return &stats; }

// Consumed here rather than in net_modem.c because the link owns readiness: everything else waits
// on modem_link_ready().
static void on_hello(const uint8_t *p, uint16_t len) {
    if (len < 9) return;
    if (p[0] != MODEM_PROTO_VERSION) {
        printf("modem: protocol version %u, expected %u\n", p[0], MODEM_PROTO_VERSION);
        return;
    }
    uint8_t fw_len = p[8];
    if (9u + fw_len > len) fw_len = (uint8_t)(len - 9u);
    if (fw_len > sizeof modem_fw - 1) fw_len = sizeof modem_fw - 1;
    memcpy(modem_fw, p + 9, fw_len);
    modem_fw[fw_len] = 0;
    ready = true;
    printf("modem: %s fw %s, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
           p[1] == MODEM_CHIP_ESP32C2 ? "esp32-c2" : p[1] == MODEM_CHIP_ESP32C3 ? "esp32-c3" : "esp32",
           modem_fw, p[2], p[3], p[4], p[5], p[6], p[7]);
}

static void dispatch(uint8_t type, const uint8_t *p, uint16_t len) {
    if (type == M_HELLO) { on_hello(p, len); return; }
    if (type == M_LOG) {
        static const char *lvl[] = { "?", "E", "W", "I", "D" };
        uint8_t l = len ? p[0] : 3;
        printf("modem[%s]: %.*s\n", lvl[l < 5 ? l : 0], len ? len - 1 : 0, (const char *)p + 1);
        return;
    }
    if (handler) handler(type, p, len);
}

static void drain_rx(void) {
    uint32_t idx = (uint32_t)((uintptr_t)dma_hw->ch[rx_dma].write_addr - (uintptr_t)rx_ring);
    idx &= MODEM_RX_RING_BYTES - 1u;
    uint32_t avail = (idx - rx_head_idx) & (MODEM_RX_RING_BYTES - 1u);
    if (!avail) return;
    rx_head_idx = idx;
    // The ring only tells us where the producer is, not how many times it lapped. Polling every
    // main-loop iteration and keeping the HTTP window below the ring size makes a lap impossible;
    // a near-full ring is still worth counting, because it means the margin has gone.
    if (avail > MODEM_RX_RING_BYTES - 512u) stats.rx_overruns++;
    for (uint32_t i = 0; i < avail; i++)
        parse_byte(rx_ring[(rx_tail + i) & (MODEM_RX_RING_BYTES - 1u)]);
    rx_tail += avail;
}

void modem_link_poll(void) {
    drain_rx();
    tx_kick();
}

bool modem_link_init(void) {
    handler = NULL;
    memset(&stats, 0, sizeof stats);

    od_init(MODEM_PIN_EN);
    od_init(MODEM_PIN_BOOT);

    uart_init(MODEM_UART, MODEM_BAUD);
    gpio_set_function(MODEM_PIN_TX, UART_FUNCSEL_NUM(MODEM_UART, MODEM_PIN_TX));
    gpio_set_function(MODEM_PIN_RX, UART_FUNCSEL_NUM(MODEM_UART, MODEM_PIN_RX));
    uart_set_hw_flow(MODEM_UART, false, false);
    uart_set_format(MODEM_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(MODEM_UART, false);
    hw_set_bits(&uart_get_hw(MODEM_UART)->dmacr, UART_UARTDMACR_RXDMAE_BITS | UART_UARTDMACR_TXDMAE_BITS);

    rx_dma = dma_claim_unused_channel(true);
    tx_dma = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config((uint)rx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, MODEM_RX_RING_BITS);   // wrap the write address in hardware
    channel_config_set_dreq(&c, uart_get_dreq_num(MODEM_UART, false));
    dma_channel_configure((uint)rx_dma, &c, rx_ring, &uart_get_hw(MODEM_UART)->dr,
                          dma_encode_endless_transfer_count(), true);

    c = dma_channel_get_default_config((uint)tx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, uart_get_dreq_num(MODEM_UART, true));
    dma_channel_configure((uint)tx_dma, &c, &uart_get_hw(MODEM_UART)->dr, tx_ring, 0, false);

    rx_tail = 0;
    rx_head_idx = 0;
    tx_head = tx_tail = tx_inflight = 0;
    parse_reset(false);

    modem_link_reset(false);

    // The ESP's ROM prints its own banner at its own baud rate before our firmware starts; those
    // bytes arrive as noise and the sync hunt drops them. Wait for a hello, but do not make it
    // fatal — a modem that boots late is picked up by a later poll, and the console still works.
    uint8_t hello[5];
    hello[0] = MODEM_PROTO_VERSION;
    uint32_t baud = MODEM_BAUD;
    memcpy(hello + 1, &baud, 4);
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + 3000u;
    uint32_t next_hello = 0;
    while (!ready && (int32_t)(to_ms_since_boot(get_absolute_time()) - deadline) < 0) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now - next_hello) >= 0) {
            modem_link_send(H_HELLO, hello, sizeof hello);
            next_hello = now + 250u;
        }
        modem_link_poll();
        sleep_ms(2);
    }
    if (!ready) printf("modem: no hello 3 s after reset; still listening\n");
    return ready;
}
