// HTTP/HTTPS for the kiosk, one request at a time, with the host's credit window controlling how
// fast the body is handed over.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void httpc_init(void);
// From the link dispatcher. url need not be NUL-terminated; it is copied.
void httpc_get(uint8_t id, uint32_t timeout_ms, uint32_t credit, const char *url, uint16_t url_len);
void httpc_credit(uint8_t id, uint32_t extra);
void httpc_cancel(uint8_t id);
void httpc_abort_all(void);
// Probes whether this network reaches the internet at all; answers with M_NET_CHECK.
void httpc_netcheck(const char *url, uint16_t url_len);
