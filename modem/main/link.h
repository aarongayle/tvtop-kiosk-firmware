// The ESP end of the UART link: framing, a receive task, and a mutex-guarded sender that any task
// may call. The wire format itself is src/common/modem_proto.h, shared with the RP2350 firmware.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "modem_proto.h"

// Bring-up rig (ESP32-C3-DevKitM-1 wired to a Pico 2 W): the link is on UART1 and free GPIOs, so
// the DevKit's own USB-serial console stays on UART0 and cannot fight the Pico's transmitter.
// The v3 board has no second USB port: there the link moves to UART0, which doubles as the ROM
// bootloader so the RP2354A can reflash this firmware. Override with -DMODEM_LINK_UART=0 etc.
#ifndef MODEM_LINK_UART
#define MODEM_LINK_UART 1
#endif
#ifndef MODEM_LINK_TX_PIN
#define MODEM_LINK_TX_PIN 7     // → Pico GP9 (its RX)
#endif
#ifndef MODEM_LINK_RX_PIN
#define MODEM_LINK_RX_PIN 6     // ← Pico GP8 (its TX)
#endif

typedef void (*link_handler_t)(uint8_t type, const uint8_t *payload, uint16_t len);

void link_init(link_handler_t handler);
bool link_send(uint8_t type, const void *payload, uint16_t len);
bool link_send2(uint8_t type, const void *a, uint16_t alen, const void *b, uint16_t blen);

// Sends M_HELLO. Called at start-up and whenever the host asks with H_HELLO, because a host that
// has just rebooted needs to hear it again.
void link_send_hello(void);

// Installs the ESP_LOG hook that turns log lines into M_LOG frames. The kiosk's USB console is the
// only place anyone can read them once the board has no second USB port, so they have to travel.
void link_log_redirect(void);
