// cyw43 bring-up, station mode and the provisioning access point.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { WIFI_DOWN, WIFI_CONNECTING, WIFI_UP, WIFI_FAILED, WIFI_AP } wifi_state_t;

// Initialises the cyw43 arch (threadsafe background). Must be called after scanout_setup_clocks:
// it derives the PIO SPI clock divider from the actual sys clock. Returns false on failure.
bool net_wifi_init(void);
void net_wifi_connect(const char *ssid, const char *password);   // async; poll net_wifi_state
wifi_state_t net_wifi_state(void);
const char *net_wifi_ip(char *buf, size_t cap);   // dotted quad or "" when not up
int net_wifi_rssi(void);
// SoftAP for provisioning (open network). Returns the SSID used ("TVTOP-xxxx", from the unique id).
const char *net_wifi_start_ap(void);
void net_wifi_stop_ap(void);
void net_wifi_led(bool on);
// Call from the main loop (drives reconnect attempts / state transitions).
void net_wifi_poll(void);
