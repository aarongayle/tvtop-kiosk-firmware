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

## Finding a network away from home

A kiosk that travels is plugged into a strange TV in a strange room, and nothing is where it was.
Three rules follow from that, and they are what `kiosk_loop.c`'s join manager implements.

**It remembers several networks, not one.** Up to `KIOSK_MAX_NETWORKS` (8), most recently joined
first, so the list is its own LRU and the eighth pushes out the one you have not seen for longest.
Home, the office, a phone's hotspot, the last place it was plugged in. The config sector is
versioned and a v1 sector is migrated in place, so upgrading does not cost a paired kiosk its
token.

**It picks by what is actually in the room.** Every few seconds it scans, and tries whichever
remembered network has the strongest live signal rather than working down a list blindly — so a
dead entry at the front costs nothing. A network it cannot see is still tried in rotation, because
a hidden SSID never appears in a scan and a scan can miss one that is really there. Scans only run
between attempts: a radio mid-join refuses them (`ESP_ERR_WIFI_STATE`), and a fruitless search is a
continuous stream of attempts, so asking at the wrong moment would mean never scanning at all in
the one case that needs it.

**It always leaves a way in.** After 45 fruitless seconds the setup AP comes up *alongside* the
search — the stored networks are never cleared, and if one reappears the kiosk simply joins and
takes the AP down again. Before this, a device whose network was absent sat on "Connecting…" for
ever, and the only route back was a laptop and a USB cable: for a device whose premise is "plug it
into any TV", precisely backwards.

The ESP modem runs AP and station together, so this costs nothing. The cyw43 build cannot (a join
in flight keeps the radio hopping channels), so there the two alternate — 60 s of AP, 25 s of
trying — and it never interrupts a phone that is mid-way through the portal.
`net_wifi_ap_is_concurrent()` is the seam.

The screen says which networks it is looking for and which it can see, so you learn "your network
is not here" from across the room instead of after two AP hops.

## Joined, but is anything out there?

Associating is not the same as reaching the internet, and a hotel or airport network will do the
first and not the second until someone clicks "I agree" in a browser. The kiosk has no browser, and
the authorisation is tied to the device asking, so a phone cannot do it on its behalf.

The kiosk's own polling cannot tell that apart from the server being down: those requests are
HTTPS, and a portal cannot intercept TLS — all it can do is break the handshake. So after two
consecutive failures with the link up, the modem probes a **plain HTTP** URL whose only correct
answer is `204` with no body (`KIOSK_NET_CHECK_URL`, Google's `generate_204` by default). Anything
else means something is answering on the network's behalf.

| Verdict | What the kiosk does |
|---|---|
| online | The network is fine and the kiosk server is not. Says so on the console; the offline badge over the last frame already covers it on screen. |
| captive portal | Says this network needs a browser sign-in and that a phone hotspot is the way round it — and raises the setup AP so you can switch without hunting for a laptop. |
| no dns / no route | Says it joined but cannot reach anything, and raises the AP the same way. |

`netcheck` on the console runs the probe on demand. The cyw43 build has no probe — there is no
second processor to run one — and reports `unsupported`, falling back to the offline badge.

**This is a real limit, not a bug to be fixed later.** Hotel Wi-Fi is not reachable for a device
with no browser, by anyone. What the kiosk can do is say so in one screen instead of showing
"connecting" for ever, and make switching to a hotspot a ten-second job.

## Provisioning services

In setup mode the kiosk runs a DHCP server (192.168.4.x), a DNS server that answers every name
with 192.168.4.1, and a tiny HTTP server on port 80 for the portal page; all three stop when the
kiosk leaves setup mode.
