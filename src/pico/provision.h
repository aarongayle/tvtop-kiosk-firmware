// Wi-Fi provisioning: SoftAP + DHCP + DNS (captive portal) + a tiny HTTP form, and the USB serial
// console. Both write kiosk_config and call kiosk_loop_restart().
#pragma once
#include <stdbool.h>

// Enables the setup AP and the portal servers. keep_station asks the radio to go on trying the
// stored networks at the same time, which is what the travel fallback wants: a way in, without
// giving up on getting back by itself. The caller draws the screen when keep_station is set.
void provision_start(bool keep_station);
void provision_stop(void);
bool provision_active(void);
// True while a phone is mid-request on the portal, so the caller does not yank the AP out from
// under someone who is halfway through typing a password.
bool provision_busy(void);
void provision_poll(void);
const char *provision_ap_ssid(void);

// --------------- Nearby networks ---------------
//
// One scan table serves the portal's picker, the join manager's choice of which stored network to
// try, and the list drawn on the TV. Results are merged per scan, strongest kept, hidden SSIDs
// dropped.
typedef struct {
    const char *ssid;
    int16_t rssi;
    bool open;
} wifi_sighting_t;

uint8_t wifi_scan_count(void);
bool wifi_scan_get(uint8_t index, wifi_sighting_t *out);   // index 0 is the strongest
void wifi_scan_request(void);                              // rate-limited; harmless to spam
bool wifi_scan_running(void);
// Milliseconds since the last scan finished, or UINT32_MAX if none has.
uint32_t wifi_scan_age_ms(void);

// USB serial console: `help`, `status`, `wifi <ssid> <password>`, `server <url>`, `mode <name>`,
// `reset` (forget token), `factory` (forget everything), `reboot`, `stats`, `test` (test pattern).
void console_poll(void);
