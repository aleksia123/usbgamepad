#include "cdc_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "tusb.h"
#include "pico/stdlib.h"
#include "bsp/board_api.h"
#include "pad_config.h"
#include "pad_config_store.h"
#include "boot_mode.h"
#include "Gamepad.h"

#define DIRTY_DEBOUNCE_MS 1000u
#define STREAM_INTERVAL_MS 33u // ~30Hz
#define GRACE_WINDOW_MS 60000u

// A full PAD_CONFIG.GET reply is ~1120 bytes with these field names (28 fields,
// worst-case 3-digit values and "false" bools, plus the envelope). The old
// 512-byte buffer silently produced BOTH failure modes below, which is why
// the editor connected fine and then did nothing:
//
//   1. append_cfg_json accumulated snprintf's return value - which is the
//      length it WOULD have written, not what it did - so `n` ran past bufsz,
//      `bufsz - n` underflowed as size_t to ~SIZE_MAX, and the next snprintf
//      was handed a pointer past the end of txbuf with a "huge" size. Silent
//      out-of-bounds write into .bss.
//   2. Even without that, send_line's single tud_cdc_write() could only hand
//      512 bytes to a 512-byte FIFO, so the JSON went out truncated mid-object.
//      The browser's JSON.parse() threw, applyCfgToUI() never ran, and every
//      slider stayed at its placeholder.
//
// Fixed on three fronts: a bigger buffer (below), a cursor helper that can
// never advance past it (append_fmt), and a write loop that drains the FIFO
// instead of assuming one write is enough (send_line).
#define TXBUF_SIZE 1536

static enum InputMode boot_mode;

static char   linebuf[256];
static size_t linelen;

static bool     pad_config_dirty;
static uint32_t pad_config_dirty_deadline_ms;

static bool     grace_active;
static uint32_t grace_deadline_ms;

static bool     stream_enabled;
static uint32_t stream_last_ms;

static char txbuf[TXBUF_SIZE];

// ---- bounded formatting ----------------------------------------------------
// Appends at offset n and returns the NEW offset, clamped so it can never
// exceed bufsz-1. Every caller chains these; a truncation just stops the line
// growing instead of walking off the end of the buffer.
static size_t append_fmt(char* buf, size_t bufsz, size_t n, const char* fmt, ...)
{
    if (bufsz == 0) return 0;
    if (n >= bufsz - 1) return bufsz - 1;   // already full; keep the NUL slot

    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + n, bufsz - n, fmt, ap);
    va_end(ap);

    if (w < 0) return n;                    // encoding error: leave as-is
    size_t nn = n + (size_t)w;
    if (nn > bufsz - 1) nn = bufsz - 1;     // vsnprintf truncated
    return nn;
}

// ---- tiny JSON field extraction (strstr-based, dependency-free) ----
// Assumes JSON.stringify-style output with no whitespace around ':' - true
// for our own web client and for what we emit ourselves.
//
// needle[] is 64, not 40: the longest field name here is
// "right_stick_angular_restrict_enabled" (36) which with quotes and colon is
// 39 chars + NUL = exactly 40. That fit by one byte, and any field name added
// later would have been silently truncated by snprintf - turning the lookup
// into a prefix match against the wrong field. Headroom is cheaper than that
// bug.
#define JSON_NEEDLE_MAX 64

static bool json_get_string(const char* json, const char* key, char* out, size_t outsz)
{
    char needle[JSON_NEEDLE_MAX];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    const char* end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool json_get_long(const char* json, const char* key, long* out)
{
    char needle[JSON_NEEDLE_MAX];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '-' && (*p < '0' || *p > '9')) return false;
    *out = strtol(p, NULL, 10);
    return true;
}

static bool json_get_bool(const char* json, const char* key, bool* out)
{
    char needle[JSON_NEEDLE_MAX];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

// ---- pad_config_t <-> JSON, table-driven so GET/SET share one field list ----
//
// Note the name pairs: "left_stick_axial_deadzone" is a strict prefix of
// "left_stick_axial_deadzone_enabled". The lookups above are safe anyway
// because every needle ends in `":` - the character after the value name is a
// quote, not an underscore - but keep that in mind before renaming anything.

typedef enum { CFG_FIELD_U8, CFG_FIELD_BOOL } cfg_field_type_t;

typedef struct {
    const char*      name;
    cfg_field_type_t type;
    size_t           offset;
} cfg_field_t;

static const cfg_field_t CFG_FIELDS[] = {
    { "left_stick_square_to_circle_enabled",  CFG_FIELD_BOOL, offsetof(pad_config_t, left_stick_square_to_circle_enabled) },
    { "left_stick_square_to_circle_pct",       CFG_FIELD_U8,   offsetof(pad_config_t, left_stick_square_to_circle_pct) },
    { "right_stick_square_to_circle_enabled", CFG_FIELD_BOOL, offsetof(pad_config_t, right_stick_square_to_circle_enabled) },
    { "right_stick_square_to_circle_pct",      CFG_FIELD_U8,   offsetof(pad_config_t, right_stick_square_to_circle_pct) },

    { "left_stick_axial_deadzone_enabled",    CFG_FIELD_BOOL, offsetof(pad_config_t, left_stick_axial_deadzone_enabled) },
    { "left_stick_axial_deadzone",             CFG_FIELD_U8,   offsetof(pad_config_t, left_stick_axial_deadzone) },
    { "right_stick_axial_deadzone_enabled",   CFG_FIELD_BOOL, offsetof(pad_config_t, right_stick_axial_deadzone_enabled) },
    { "right_stick_axial_deadzone",            CFG_FIELD_U8,   offsetof(pad_config_t, right_stick_axial_deadzone) },

    { "left_stick_radial_deadzone_enabled",   CFG_FIELD_BOOL, offsetof(pad_config_t, left_stick_radial_deadzone_enabled) },
    { "left_stick_radial_deadzone",            CFG_FIELD_U8,   offsetof(pad_config_t, left_stick_radial_deadzone) },
    { "right_stick_radial_deadzone_enabled",  CFG_FIELD_BOOL, offsetof(pad_config_t, right_stick_radial_deadzone_enabled) },
    { "right_stick_radial_deadzone",           CFG_FIELD_U8,   offsetof(pad_config_t, right_stick_radial_deadzone) },

    { "left_stick_angular_restrict_enabled",  CFG_FIELD_BOOL, offsetof(pad_config_t, left_stick_angular_restrict_enabled) },
    { "left_stick_angular_restrict_deg",       CFG_FIELD_U8,   offsetof(pad_config_t, left_stick_angular_restrict_deg) },
    { "right_stick_angular_restrict_enabled", CFG_FIELD_BOOL, offsetof(pad_config_t, right_stick_angular_restrict_enabled) },
    { "right_stick_angular_restrict_deg",      CFG_FIELD_U8,   offsetof(pad_config_t, right_stick_angular_restrict_deg) },

    { "left_stick_corner_cap_enabled",        CFG_FIELD_BOOL, offsetof(pad_config_t, left_stick_corner_cap_enabled) },
    { "left_stick_corner_cap_pct",             CFG_FIELD_U8,   offsetof(pad_config_t, left_stick_corner_cap_pct) },
    { "right_stick_corner_cap_enabled",       CFG_FIELD_BOOL, offsetof(pad_config_t, right_stick_corner_cap_enabled) },
    { "right_stick_corner_cap_pct",            CFG_FIELD_U8,   offsetof(pad_config_t, right_stick_corner_cap_pct) },

    { "left_stick_output_scale_enabled",      CFG_FIELD_BOOL, offsetof(pad_config_t, left_stick_output_scale_enabled) },
    { "left_stick_output_scale_pct",           CFG_FIELD_U8,   offsetof(pad_config_t, left_stick_output_scale_pct) },
    { "right_stick_output_scale_enabled",     CFG_FIELD_BOOL, offsetof(pad_config_t, right_stick_output_scale_enabled) },
    { "right_stick_output_scale_pct",          CFG_FIELD_U8,   offsetof(pad_config_t, right_stick_output_scale_pct) },

    { "left_stick_dither_enabled",            CFG_FIELD_BOOL, offsetof(pad_config_t, left_stick_dither_enabled) },
    { "left_stick_dither_amp_deg10",           CFG_FIELD_U8,   offsetof(pad_config_t, left_stick_dither_amp_deg10) },
    { "right_stick_dither_enabled",           CFG_FIELD_BOOL, offsetof(pad_config_t, right_stick_dither_enabled) },
    { "right_stick_dither_amp_deg10",          CFG_FIELD_U8,   offsetof(pad_config_t, right_stick_dither_amp_deg10) },
};
#define CFG_FIELD_COUNT (sizeof(CFG_FIELDS) / sizeof(CFG_FIELDS[0]))

static size_t append_cfg_json(char* buf, size_t bufsz, size_t n, const pad_config_t* cfg)
{
    n = append_fmt(buf, bufsz, n, "{");
    for (size_t i = 0; i < CFG_FIELD_COUNT; i++) {
        const cfg_field_t* f = &CFG_FIELDS[i];
        const uint8_t* base = (const uint8_t*)cfg + f->offset;
        if (f->type == CFG_FIELD_U8) {
            n = append_fmt(buf, bufsz, n, "%s\"%s\":%u",
                           i ? "," : "", f->name, (unsigned)(*base));
        } else {
            n = append_fmt(buf, bufsz, n, "%s\"%s\":%s",
                           i ? "," : "", f->name, (*(const bool*)base) ? "true" : "false");
        }
    }
    return append_fmt(buf, bufsz, n, "}");
}

// Field names are unique across the struct and never collide with the
// top-level "cmd"/"mode" keys, so matching against the whole line finds each
// field correctly without isolating the nested "cfg" object first.
static void apply_cfg_subset(const char* line, pad_config_t* cfg)
{
    for (size_t i = 0; i < CFG_FIELD_COUNT; i++) {
        const cfg_field_t* f = &CFG_FIELDS[i];
        uint8_t* base = (uint8_t*)cfg + f->offset;
        if (f->type == CFG_FIELD_U8) {
            long v;
            if (json_get_long(line, f->name, &v)) {
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                *base = (uint8_t)v;
            }
        } else {
            bool v;
            if (json_get_bool(line, f->name, &v)) {
                *(bool*)base = v;
            }
        }
    }
}

// ---- live input telemetry (INPUT.STREAM) ----
// Named fields, not a packed bitmask - GamepadButtons is a bitfield struct
// (can't take &field), and this keeps the wire format self-describing rather
// than depending on a bit-layout the web client would have to guess (see
// XInputDriver.cpp, which builds its own named->masked conversion the same
// way for the same reason).

static void send_line(const char* s); // defined below, in "responses"

static size_t append_buttons_json(char* buf, size_t bufsz, size_t n, const GamepadButtons* b)
{
    return append_fmt(buf, bufsz, n,
        "{\"up\":%s,\"down\":%s,\"left\":%s,\"right\":%s,"
        "\"a\":%s,\"b\":%s,\"x\":%s,\"y\":%s,"
        "\"l3\":%s,\"r3\":%s,\"back\":%s,\"start\":%s,"
        "\"lb\":%s,\"rb\":%s,\"sys\":%s,\"misc\":%s}",
        b->up    ? "true" : "false", b->down  ? "true" : "false",
        b->left  ? "true" : "false", b->right ? "true" : "false",
        b->a     ? "true" : "false", b->b     ? "true" : "false",
        b->x     ? "true" : "false", b->y     ? "true" : "false",
        b->l3    ? "true" : "false", b->r3    ? "true" : "false",
        b->back  ? "true" : "false", b->start ? "true" : "false",
        b->lb    ? "true" : "false", b->rb    ? "true" : "false",
        b->sys   ? "true" : "false", b->misc  ? "true" : "false");
}

static void send_input_event(void)
{
    const Gamepad* gp = gamepad(0);
    size_t n = append_fmt(txbuf, sizeof(txbuf), 0,
        "{\"evt\":\"input\",\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,\"lt\":%u,\"rt\":%u,\"btn\":",
        gp->joysticks.lx, gp->joysticks.ly, gp->joysticks.rx, gp->joysticks.ry,
        (unsigned)gp->triggers.l, (unsigned)gp->triggers.r);
    n = append_buttons_json(txbuf, sizeof(txbuf), n, &gp->buttons);
    append_fmt(txbuf, sizeof(txbuf), n, "}");
    send_line(txbuf);
}

// ---- flash debounce ----

static void mark_dirty(void)
{
    pad_config_dirty = true;
    pad_config_dirty_deadline_ms = board_millis() + DIRTY_DEBOUNCE_MS;
}

static void flush_dirty_now(void)
{
    if (!pad_config_dirty) return;
    pad_config_store_save(&g_pad_config);
    pad_config_dirty = false;
}

// ---- responses ----

// Writes the whole line + '\n', draining the CDC TX FIFO as it goes.
//
// tud_cdc_write() only accepts what currently fits in the FIFO and returns
// that count - a single call is NOT a guarantee the line went out. Anything
// longer than the FIFO (a full PAD_CONFIG.GET reply is 654 bytes) used to be
// chopped, and a chopped JSON line is worse than no line at all: the client
// can't parse it, so the reply is lost AND the failure is invisible.
//
// Bounded by a deadline so an attached-but-not-reading host can stall the main
// loop for at most a few ms rather than forever.
static void send_line(const char* s)
{
    size_t len  = strlen(s);
    size_t sent = 0;
    uint32_t deadline = board_millis() + 50;

    while (sent < len) {
        uint32_t w = tud_cdc_write(s + sent, (uint32_t)(len - sent));
        sent += w;
        if (sent < len) {
            if (board_millis() >= deadline) {
                printf("[cdc] send_line truncated: %u/%u bytes\n",
                       (unsigned)sent, (unsigned)len);
                break;
            }
            tud_cdc_write_flush(); // make room, then let the stack move it
            tud_task();
        }
    }

    tud_cdc_write("\n", 1);
    tud_cdc_write_flush();
}

// ---- command handlers ----

static void handle_ping(void)
{
    send_line("{\"ok\":true,\"cmd\":\"PING\",\"pong\":true}");
}

static void handle_info(void)
{
    append_fmt(txbuf, sizeof(txbuf), 0,
               "{\"ok\":true,\"cmd\":\"INFO\",\"fw\":\"tusb_gamepad\",\"version\":\"1.1\",\"mode\":\"USBSERIAL\"}");
    send_line(txbuf);
}

static void handle_pad_config_get(void)
{
    size_t n = append_fmt(txbuf, sizeof(txbuf), 0,
                          "{\"ok\":true,\"cmd\":\"PAD_CONFIG.GET\",\"cfg\":");
    n = append_cfg_json(txbuf, sizeof(txbuf), n, &g_pad_config);
    append_fmt(txbuf, sizeof(txbuf), n, "}");
    send_line(txbuf);
}

static void handle_pad_config_set(const char* line)
{
    apply_cfg_subset(line, &g_pad_config);

    // Clamp before echoing, so the reply is the config that is actually live.
    // "clamped":true tells the editor to re-read the values it just sent
    // instead of assuming they landed verbatim.
    bool clamped = pad_config_sanitize(&g_pad_config);
    mark_dirty();

    size_t n = append_fmt(txbuf, sizeof(txbuf), 0,
                          "{\"ok\":true,\"cmd\":\"PAD_CONFIG.SET\",\"clamped\":%s,\"cfg\":",
                          clamped ? "true" : "false");
    n = append_cfg_json(txbuf, sizeof(txbuf), n, &g_pad_config);
    append_fmt(txbuf, sizeof(txbuf), n, "}");
    send_line(txbuf);
}

static void handle_mode_get(void)
{
    send_line("{\"ok\":true,\"cmd\":\"MODE.GET\",\"mode\":\"USBSERIAL\"}");
}

static void handle_mode_set(const char* line)
{
    char mode[16];
    if (json_get_string(line, "mode", mode, sizeof(mode)) && strcmp(mode, "XINPUT") == 0) {
        send_line("{\"ok\":true,\"cmd\":\"MODE.SET\",\"mode\":\"XINPUT\",\"rebooting\":true}");
        tud_cdc_write_flush();
        sleep_ms(50); // let the host drain the response before we drop the CDC interface

        // Force a real electrical detach before the reboot. The watchdog
        // reset itself also drops the D+ pull-up, but that gap can be short
        // or inconsistent enough that some hosts never register a disconnect
        // and just keep the old CDC interface "paired" instead of asking for
        // fresh (XInput) descriptors after the reboot.
        tud_disconnect();
        sleep_ms(300); // hold long enough for the host's own debounce to notice

        flush_dirty_now();
        boot_mode_request_switch(INPUT_MODE_XINPUT); // does not return
    } else {
        send_line("{\"ok\":false,\"cmd\":\"MODE.SET\",\"error\":\"unsupported mode\"}");
    }
}

// Restores PAD_CONFIG_DEFAULTS in one command. Cheaper and less error-prone
// than having the client push 16 individual SET lines, and it guarantees the
// editor's "Restore defaults" agrees with the firmware's own defaults rather
// than a duplicated copy of them in JavaScript.
static void handle_pad_config_reset(void)
{
    pad_config_t defaults = PAD_CONFIG_DEFAULTS;
    g_pad_config = defaults;
    mark_dirty();

    size_t n = append_fmt(txbuf, sizeof(txbuf), 0,
                          "{\"ok\":true,\"cmd\":\"PAD_CONFIG.RESET\",\"cfg\":");
    n = append_cfg_json(txbuf, sizeof(txbuf), n, &g_pad_config);
    append_fmt(txbuf, sizeof(txbuf), n, "}");
    send_line(txbuf);
}

static void handle_input_stream(const char* line)
{
    bool v;
    if (!json_get_bool(line, "enable", &v)) {
        send_line("{\"ok\":false,\"cmd\":\"INPUT.STREAM\",\"error\":\"missing enable\"}");
        return;
    }
    stream_enabled = v;
    stream_last_ms = board_millis();
    append_fmt(txbuf, sizeof(txbuf), 0,
               "{\"ok\":true,\"cmd\":\"INPUT.STREAM\",\"enable\":%s}", v ? "true" : "false");
    send_line(txbuf);
}

static void dispatch(const char* line)
{
    char cmd[32];
    if (!json_get_string(line, "cmd", cmd, sizeof(cmd))) {
        send_line("{\"ok\":false,\"error\":\"bad command\"}");
        return;
    }

    if      (strcmp(cmd, "PING") == 0)             handle_ping();
    else if (strcmp(cmd, "INFO") == 0)             handle_info();
    else if (strcmp(cmd, "PAD_CONFIG.GET") == 0)   handle_pad_config_get();
    else if (strcmp(cmd, "PAD_CONFIG.SET") == 0)   handle_pad_config_set(line);
    else if (strcmp(cmd, "PAD_CONFIG.RESET") == 0) handle_pad_config_reset();
    else if (strcmp(cmd, "MODE.GET") == 0)         handle_mode_get();
    else if (strcmp(cmd, "MODE.SET") == 0)         handle_mode_set(line);
    else if (strcmp(cmd, "INPUT.STREAM") == 0)     handle_input_stream(line);
    else send_line("{\"ok\":false,\"error\":\"bad command\"}");
}

// ---- public API ----

void cdc_config_init(enum InputMode mode)
{
    boot_mode = mode;
    linelen = 0;
    pad_config_dirty = false;
    stream_enabled = false;

    grace_active = (mode == INPUT_MODE_USBSERIAL);
    if (grace_active) {
        grace_deadline_ms = board_millis() + GRACE_WINDOW_MS;
    }
}

void cdc_config_task(void)
{
    if (boot_mode != INPUT_MODE_USBSERIAL) return;

    while (tud_cdc_available()) {
        char c;
        if (tud_cdc_read(&c, 1) == 0) break;

        if (c == '\n' || c == '\r') {
            if (linelen > 0) {
                grace_active = false; // any complete line cancels the grace timer for this boot
                linebuf[linelen] = '\0';
                dispatch(linebuf);
                linelen = 0;
            }
        } else if (linelen < sizeof(linebuf) - 1) {
            linebuf[linelen++] = c;
        } else {
            linelen = 0; // overflow: drop the line rather than dispatch garbage
        }
    }

    if (pad_config_dirty && board_millis() >= pad_config_dirty_deadline_ms) {
        flush_dirty_now();
    }

    if (grace_active && board_millis() >= grace_deadline_ms) {
        flush_dirty_now();
        boot_mode_request_switch(INPUT_MODE_XINPUT); // does not return
    }

    if (stream_enabled && tud_cdc_connected()) {
        uint32_t now = board_millis();
        if (now - stream_last_ms >= STREAM_INTERVAL_MS) {
            stream_last_ms = now;
            send_input_event();
        }
    }
}