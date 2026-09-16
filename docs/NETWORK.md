# Networking

## Stack

Pico W: cyw43 driver + lwIP 2.2 in the SDK's `threadsafe_background` mode (the driver runs from a
low-priority interrupt; the application never blocks on it). The HTTP client (`http_client.c`) is
written on lwIP's raw/altcp API: one connection at a time, kept alive between requests to the same
origin, reconnected transparently when the server drops it.

Body bytes are not copied into a second buffer: the receive callback queues the pbufs and
`http_poll()` (main context) drains them into the sink, acknowledging the TCP window as it goes.
That is what lets a 300 KB static set stream straight into the flash geometry cache with 8 KB of
pbufs in flight.

## The long poll

`GET /v1/frame/<token>?rev=N` is held open by the server for up to 25 s. The client's receive
timeout is **45 s** (`LONG_POLL_TIMEOUT_MS`); a 25 s silence is the normal idle case, not an
error. Responses:

| Status | Client action |
|---|---|
| 200 | decode + draw; follow `next_url`; persist it if its shape changed |
| 304 | nothing changed; ask again after a 250 ms floor |
| 404 | from a frame URL: go to `/v1/config?t=<token>`. From `/v1/config` itself three times in a row: the server no longer knows the token (registry reset, kiosk deleted), so forget it and register again, showing a new pairing code |
| other / error / timeout | keep the last frame; exponential backoff 1 s → 60 s with full jitter; after 2 failures overlay the offline badge |

`next_ms` is treated as the failsafe the protocol describes: requests are free, so the client
reconnects immediately (with the 250 ms floor).

## TLS

`KIOSK_TLS=ON` builds mbedTLS (TLS 1.2, ECDHE-RSA/ECDSA, AES-GCM, P-256/P-384) behind lwIP's
altcp layer and verifies the server against the bundled roots in `ca_certs.h` (ISRG Root X1/X2 for
Let's Encrypt, DigiCert Global Root G2, GTS Root R1/R4). There is no RTC, so certificate validity
*dates* are not checked; chains and names are.

The cost on an RP2040 is roughly 60 KB of the 264 KB: a 16 KB TLS record buffer, a 32 KB TCP
window (a record must fit in the window or the connection deadlocks), and the handshake heap.
The default Pico W build therefore has TLS **off** and expects an `http://` server — the kiosk
server run locally (`DEV_NO_FIREBASE=1 PUBLIC_URL=http://<lan-ip>:8080 npm start`), or a plain
HTTP listener in front of the production one. An `https://` URL on a no-TLS build draws a screen
saying so instead of failing silently. The Pico 2 W has the RAM for `KIOSK_TLS=ON`.

## Memory profile

| | no TLS | TLS |
|---|---|---|
| lwIP heap (`MEM_SIZE`) | 4 KB | 6 KB |
| pbuf pool | 8 × 1.6 KB | 24 × 1.6 KB |
| TCP window | 4 × MSS | 32 KB |
| mbedTLS | — | ~40 KB peak |
| line pool (`KIOSK_LINEPOOL_BYTES`) | 84 KB | 56 KB |

## Provisioning services

In setup mode the kiosk runs a DHCP server (192.168.4.x), a DNS server that answers every name
with 192.168.4.1, and a tiny HTTP server on port 80 for the portal page; all three stop when the
kiosk leaves setup mode.
