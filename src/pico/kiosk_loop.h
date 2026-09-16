// The protocol state machine: register → config → long-poll frames → render.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void kiosk_loop_init(void);
// Rendering only (no HTTP, no Wi-Fi): for radio-less diagnostic builds.
void kiosk_loop_init_display_only(void);
void kiosk_loop_poll(void);          // main loop step, non-blocking
// Diagnostics for the console.
void kiosk_loop_status(char *buf, size_t cap);
// Forces a fresh start (after config changes from the console/portal).
void kiosk_loop_restart(void);
// Draws a built-in frame (JSON) through the normal decoder/renderer.
bool kiosk_show_builtin(int which, const char *arg1, const char *arg2);
