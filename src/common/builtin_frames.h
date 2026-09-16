// The screens firmware draws itself, as protocol-v3 JSON so the same decoder/renderer draws them.
#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    BUILTIN_PROVISION,     // arg1 = AP ssid, arg2 = portal URL
    BUILTIN_CONNECTING,    // arg1 = Wi-Fi ssid
    BUILTIN_REGISTERING,   // no args
    BUILTIN_NO_TLS,        // arg1 = the https URL we cannot fetch
    BUILTIN_TEST_PATTERN,  // colour bars + text, for bring-up
    BUILTIN_SOLID,         // three flat colour bands, no text: near-zero encode cost (signal-integrity check)
    BUILTIN_LABEL,         // the same bands with arg1 in large type (signal sweep step labels)
} builtin_frame_t;

// Writes a complete frame JSON into buf (NUL-terminated). Args are JSON-escaped. Returns length,
// or 0 if buf is too small.
size_t builtin_frame_json(builtin_frame_t which, char *buf, size_t cap, const char *arg1, const char *arg2);
