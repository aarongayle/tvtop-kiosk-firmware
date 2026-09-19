// Wi-Fi: station join with retry, the provisioning SoftAP, and scans. State changes are pushed to
// the host as M_WIFI_STATE; nothing here is polled.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void net_init(void);
void net_connect(const char *ssid, const char *pass);   // async; reports through M_WIFI_STATE
void net_stop(void);
void net_ap_start(const char *ssid, bool keep_station);   // open AP on 192.168.4.1
void net_ap_stop(void);
void net_scan(void);                                     // async; M_SCAN_RESULT… then M_SCAN_DONE
void net_send_state(void);                               // push the current state to the host
bool net_sta_up(void);
