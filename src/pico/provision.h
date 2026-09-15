// Wi-Fi provisioning: SoftAP + DHCP + DNS (captive portal) + a tiny HTTP form, and the USB serial
// console. Both write kiosk_config and call kiosk_loop_restart().
#pragma once
#include <stdbool.h>

void provision_start(void);      // enables the AP and servers; shows BUILTIN_PROVISION
void provision_stop(void);
bool provision_active(void);
void provision_poll(void);
const char *provision_ap_ssid(void);

// USB serial console: `help`, `status`, `wifi <ssid> <password>`, `server <url>`, `mode <name>`,
// `reset` (forget token), `factory` (forget everything), `reboot`, `stats`, `test` (test pattern).
void console_poll(void);
