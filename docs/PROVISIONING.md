# Provisioning

A kiosk needs three things: Wi-Fi credentials, the server it should talk to, and (after the first
successful `/v1/register`) the token the server issued. All three live in the config sector at the
end of flash (`flash_store.c`). Everything else is learned at runtime from `next_url`.

## First boot: the setup network

With no Wi-Fi credentials stored the kiosk starts an open access point called **TVTOP-xxxx** (the
suffix is from the board's unique id) and shows that name on the TV.

1. Join `TVTOP-xxxx` from a phone or laptop.
2. The captive-portal page opens by itself (Android, iOS and Windows all probe a known URL; the
   kiosk answers every DNS query with its own address and redirects those probes). If nothing
   opens, browse to `http://192.168.4.1/`.
3. Pick the network from the scan list (or type it), enter the password, adjust the server URL if
   needed, and press Save. The kiosk stores the settings and reboots into station mode.

## The USB serial console

The Pico's USB port is a serial console (115200 8N1, any terminal program). Commands:

| Command | Effect |
|---|---|
| `help` | list commands |
| `status` | state machine, current URL (token masked), Wi-Fi, heap, render and video statistics |
| `stats` | the numbers only |
| `wifi <ssid> [password]` | store credentials (quote an SSID with spaces: `wifi "My Net" pass`) and reconnect |
| `server <url>` | store the server base URL, e.g. `server http://192.168.1.10:8080` |
| `mode 720p30\|720p30rb\|480p60\|720x480p60\|960x540p60\|1066x600p50` | store the video mode and reboot |
| `reset` | forget the token, device id and cached URL: the kiosk registers as a new device |
| `factory` | forget everything and reboot into the setup network |
| `test` | draw the built-in test pattern |
| `reboot` | reboot |

The console is the fastest way to point a bench unit at a local kiosk server:

```
wifi MyNetwork hunter2
server http://192.168.1.10:8080
```

## What persists

* Wi-Fi SSID/password, server base URL, video mode.
* `token` and `id` from `/v1/register` — the device's identity; `reset` clears them.
* The last `next_url` shape and its static-set id, so a reboot mid-game goes straight back to the
  frame it was showing. It is rewritten only when the endpoint or static id changes (never per
  frame revision), and the geometry cache itself lives in its own flash region.

Writes are debounced (2 s) and happen from the main loop with interrupts off for the ~50 ms a
sector erase + program takes; video is unaffected because core 1 runs entirely from SRAM.
