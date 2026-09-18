// The UART link between the RP2350 and the ESP32 modem. Shared verbatim by both firmwares: the
// Pico build compiles it from src/common, the ESP-IDF build from modem/main (same file, one copy).
//
// Wire format, little-endian throughout:
//
//     A5 5A | type u8 | flags u8 | len u16 | payload[len] | crc16 u16
//
// The CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type, flags, len and payload.
// A receiver that fails the CRC, or sees a length past MODEM_MAX_PAYLOAD, throws the frame away
// and hunts for the next A5 5A. That matters on this link: the ESP's ROM prints a boot banner on
// its UART at a baud rate we do not control, and the kiosk must ride through it rather than wedge.
//
// Message ids are split by direction — 0x01..0x7F host (RP2350) to modem, 0x80..0xFF modem to
// host — so a frame that arrives on the wrong side is obvious in a trace.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MODEM_SYNC0 0xA5u
#define MODEM_SYNC1 0x5Au
#define MODEM_PROTO_VERSION 1u

// The largest payload either side will send or accept. Body chunks are the only messages that get
// near it; 1024 of payload keeps a frame inside 1030 bytes, comfortably under the Pico's RX ring.
#define MODEM_MAX_PAYLOAD 1024u
#define MODEM_HDR_BYTES 6u
#define MODEM_CRC_BYTES 2u
#define MODEM_FRAME_MAX (MODEM_HDR_BYTES + MODEM_MAX_PAYLOAD + MODEM_CRC_BYTES)

// The link runs fixed at this rate; both ends set it before the first frame. 921600 is the fastest
// rate that is reliable over the dupont jumpers of the bring-up rig. On the v3 PCB the same code
// runs at MODEM_BAUD_FAST over 20 mm of trace.
#ifndef MODEM_BAUD
#define MODEM_BAUD 921600u
#endif
#define MODEM_BAUD_FAST 2000000u

// ---- host (RP2350) → modem -------------------------------------------------------------------
enum {
    H_HELLO         = 0x01,   // u8 version, u32 baud (informational: the rate already in use)
    H_PING          = 0x02,   // —
    H_WIFI_CONNECT  = 0x10,   // u8 ssid_len, ssid, u8 pass_len, pass  (pass_len 0 = open network)
    H_WIFI_STOP     = 0x11,   // —
    H_AP_START      = 0x12,   // u8 ssid_len, ssid   (open AP, 192.168.4.1, DHCP + DNS hijack)
    H_AP_STOP       = 0x13,   // —
    H_SCAN          = 0x14,   // — (answered by any number of M_SCAN_RESULT then M_SCAN_DONE)
    H_HTTP_GET      = 0x20,   // u8 id, u32 timeout_ms, u32 credit, u16 url_len, url
    H_HTTP_CANCEL   = 0x21,   // u8 id
    H_HTTP_CREDIT   = 0x22,   // u8 id, u32 extra_bytes
    H_SOCK_DATA     = 0x30,   // u8 conn, bytes            (portal: write to the accepted socket)
    H_SOCK_CLOSE    = 0x31,   // u8 conn, u8 abort
    H_LED           = 0x40,   // u8 on
};

// ---- modem → host (RP2350) -------------------------------------------------------------------
enum {
    // u8 version, u8 chip, u8 mac[6], u32 session, u8 fw_len, fw.
    // `session` is drawn afresh every time the modem boots. It is how the host tells "the modem I
    // have been talking to" from "a modem that has just restarted and forgotten everything": the
    // modem re-announces itself several times at start-up, and the host must replay its state for
    // a new session exactly once, not once per hello.
    M_HELLO         = 0x81,
    M_PONG          = 0x82,   // —
    M_WIFI_STATE    = 0x90,   // u8 state (modem_wifi_state_t), u32 ip, i8 rssi, u8 channel
    M_SCAN_RESULT   = 0x91,   // i16 rssi, u8 open, u8 ssid_len, ssid
    M_SCAN_DONE     = 0x92,   // —
    M_HTTP_STATUS   = 0xA0,   // u8 id, u16 status
    M_HTTP_HEADER   = 0xA1,   // u8 id, u8 name_len, name, u16 value_len, value
    M_HTTP_BODY     = 0xA2,   // u8 id, bytes           (never more than the credit outstanding)
    M_HTTP_DONE     = 0xA3,   // u8 id, i16 err (modem_http_err_t), u16 status
    M_SOCK_OPEN     = 0xB0,   // u8 conn
    M_SOCK_DATA     = 0xB1,   // u8 conn, bytes
    M_SOCK_SENT     = 0xB2,   // u8 conn, u16 n         (bytes the socket has taken: flow control)
    M_SOCK_CLOSE    = 0xB3,   // u8 conn
    M_LOG           = 0xC0,   // u8 level, text (no NUL)
};

// M_WIFI_STATE.state. Deliberately the same order as wifi_state_t in net_wifi.h.
typedef enum {
    MODEM_WIFI_DOWN = 0,
    MODEM_WIFI_CONNECTING = 1,
    MODEM_WIFI_UP = 2,
    MODEM_WIFI_FAILED = 3,
    MODEM_WIFI_AP = 4,
} modem_wifi_state_t;

// M_HTTP_DONE.err. The same numbering as http_client.h's HTTP_ERR_*, so the Pico passes them
// through to the sink unchanged.
typedef enum {
    MODEM_HTTP_OK = 0,
    MODEM_HTTP_ERR_URL = -1,
    MODEM_HTTP_ERR_DNS = -2,
    MODEM_HTTP_ERR_CONNECT = -3,
    MODEM_HTTP_ERR_TIMEOUT = -4,
    MODEM_HTTP_ERR_CLOSED = -5,
    MODEM_HTTP_ERR_PROTOCOL = -6,
    MODEM_HTTP_ERR_ABORTED = -7,
    MODEM_HTTP_ERR_TLS = -8,
    MODEM_HTTP_ERR_BUSY = -9,
    MODEM_HTTP_ERR_TOO_MANY_REDIRECTS = -10,
} modem_http_err_t;

// M_HELLO.chip
enum { MODEM_CHIP_UNKNOWN = 0, MODEM_CHIP_ESP32C3 = 1, MODEM_CHIP_ESP32C2 = 2 };

// M_LOG.level, matching ESP_LOG_*.
enum { MODEM_LOG_ERROR = 1, MODEM_LOG_WARN = 2, MODEM_LOG_INFO = 3, MODEM_LOG_DEBUG = 4 };

// Portal relay limits. The captive portal itself lives on the RP2350 (src/pico/provision.c): the
// modem accepts the TCP connections and shovels bytes, so there is one portal implementation and
// one place that validates what the form submits.
#define MODEM_MAX_SOCKS 3

// CRC-16/CCITT-FALSE. Bitwise rather than table-driven: the link is 92 KB/s, and on the RP2350
// side this runs on core 0 between renders where a 512-byte table would cost more cache than the
// eight shifts a byte cost in time.
static inline uint16_t modem_crc16(uint16_t crc, const uint8_t *p, size_t n) {
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}
#define MODEM_CRC_INIT 0xFFFFu
