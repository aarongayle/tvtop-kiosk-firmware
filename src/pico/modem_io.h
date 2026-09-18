// Internal seams between the modem-backed network modules. net_modem.c owns the link's single
// dispatch handler and fans messages out to the HTTP client and the portal relay; these are the
// entry points it calls. Not part of any public API — kiosk_loop.c and provision.c keep talking to
// net_wifi.h, http_client.h and provision.h exactly as they do on the cyw43 build.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void http_modem_on_message(uint8_t type, const uint8_t *p, uint16_t len);
void portal_modem_on_message(uint8_t type, const uint8_t *p, uint16_t len);

// Called when the modem announces itself (first hello, or a hello after the modem restarted): the
// modem has forgotten everything, so whatever state we wanted has to be asked for again.
void http_modem_on_modem_restart(void);
void portal_modem_on_modem_restart(void);

// The portal relay asks for a scan through the link; results arrive as M_SCAN_RESULT and are
// handed to provision.c's table by this callback, which provision.c defines.
void provision_scan_result(const uint8_t *ssid, uint16_t ssid_len, int16_t rssi, bool open);
void provision_scan_done(void);
