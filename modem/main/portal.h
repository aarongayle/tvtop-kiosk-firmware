// The provisioning portal's transport: a TCP listener on :80 and a DNS responder that points every
// name at 192.168.4.1. The portal itself — the page, the form, the validation — lives on the
// RP2350 (src/pico/provision.c); this file only accepts connections and relays bytes, so there is
// one portal implementation and the modem never has to know what a kiosk setting is.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void portal_init(void);
void portal_enable(bool on);
// From the link dispatcher: bytes the host wants written to a relayed connection, and a close.
void portal_write(uint8_t conn, const uint8_t *data, uint16_t len);
void portal_close(uint8_t conn, bool abort);
void portal_close_all(void);
