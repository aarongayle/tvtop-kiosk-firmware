// Streaming (SAX-style) JSON tokenizer. Feed bytes in arbitrary chunks; get events. No allocation,
// no recursion: the container stack is explicit. String *values* are delivered in chunks (a path
// string in a static set can be 20 KB); keys are delivered whole (truncated to JSON_KEY_MAX).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JSON_MAX_DEPTH 12
#define JSON_KEY_MAX 24
#define JSON_NUM_MAX 31

typedef enum {
    JSON_EV_OBJ_START, JSON_EV_OBJ_END, JSON_EV_ARR_START, JSON_EV_ARR_END,
    JSON_EV_KEY,        // data/len = whole key (UTF-8, escapes decoded), final=true
    JSON_EV_STRING,     // chunk of a string value; final marks the last chunk (may be len 0)
    JSON_EV_NUMBER,     // whole number text, final=true
    JSON_EV_TRUE, JSON_EV_FALSE, JSON_EV_NULL,
} json_event_t;

typedef struct json_stream json_stream_t;
// Return false to abort parsing (json_stream_feed then returns false and sets error).
typedef bool (*json_cb_t)(void *ctx, const json_stream_t *js, json_event_t ev, const char *data, size_t len, bool final);

struct json_stream {
    json_cb_t cb;
    void *ctx;
    // Context available to the callback (read-only):
    uint8_t depth;                  // number of open containers (0 at top level)
    uint8_t stack[JSON_MAX_DEPTH];  // 'o' or 'a' per open container
    uint16_t index[JSON_MAX_DEPTH]; // index of the current value within its container (0-based)
    char key[JSON_MAX_DEPTH][JSON_KEY_MAX + 1]; // current key at each object depth
    bool error;
    bool complete;                  // a complete top-level value has been consumed
    // Internal tokenizer state — do not touch.
    uint8_t st, sub;
    uint8_t keylen, numlen;
    uint16_t uescape, pending_surrogate;
    char num[JSON_NUM_MAX + 1];
    char keybuf[JSON_KEY_MAX + 1];
    uint8_t utf8[4]; uint8_t utf8len;
    bool in_key, expect_value, after_value;
};

void json_stream_init(json_stream_t *js, json_cb_t cb, void *ctx);
// Returns false on syntax error or callback abort (js->error set). Extra data after the top-level
// value is ignored once js->complete is set.
bool json_stream_feed(json_stream_t *js, const char *data, size_t len);
// Call at end of input: true iff a complete top-level value was parsed without error.
bool json_stream_finish(json_stream_t *js);

// Helpers for JSON_EV_NUMBER text.
bool json_number_to_int(const char *text, size_t len, int32_t *out);          // rounds to nearest
bool json_number_to_fixed(const char *text, size_t len, int frac_bits, int32_t *out); // rounds
bool json_number_to_float(const char *text, size_t len, float *out);
