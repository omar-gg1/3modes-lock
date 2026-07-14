// Pure WIFI: payload parser — no ESP dependencies so the host self-check can
// compile it directly. See qr_parse_wifi() in qr_parse.h.
#include "qr_parse.h"
#include <string.h>

// Copy one MECARD field value into dst, un-escaping \; \: \, \\ and stopping at
// an unescaped ';'. Returns the pointer just past the terminating ';' (or the
// NUL). dst is always NUL-terminated within cap.
static const char *copy_field(const char *p, char *dst, size_t cap)
{
    size_t i = 0;
    while (*p && *p != ';') {
        char c = *p++;
        if (c == '\\' && *p) c = *p++;   // consume the escaped char literally
        if (i + 1 < cap) dst[i++] = c;
    }
    if (i < cap) dst[i] = '\0'; else if (cap) dst[cap - 1] = '\0';
    if (*p == ';') p++;
    return p;
}

bool qr_parse_wifi(const char *payload, char *ssid_out, size_t ssid_cap,
                   char *pass_out, size_t pass_cap)
{
    if (!payload || !ssid_out || !pass_out || ssid_cap == 0 || pass_cap == 0)
        return false;

    ssid_out[0] = '\0';
    pass_out[0] = '\0';
    bool have_ssid = false;

    // Case-insensitive "WIFI:" prefix (hand-rolled — no <strings.h> dependency).
    static const char scheme[] = "wifi:";
    for (int i = 0; i < 5; i++) {
        char c = payload[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != scheme[i]) return false;
    }
    const char *p = payload + 5;

    // Fields are "<KEY>:<value>;" in any order, terminated by an extra ';'.
    while (*p && *p != ';') {
        char key = *p;
        // Advance to the ':' that separates key from value.
        if (p[1] != ':') { p++; continue; }
        p += 2;
        if (key == 'S' || key == 's') {
            p = copy_field(p, ssid_out, ssid_cap);
            have_ssid = ssid_out[0] != '\0';
        } else if (key == 'P' || key == 'p') {
            p = copy_field(p, pass_out, pass_cap);
        } else {
            // Skip unknown field (T:, H:, etc.).
            char junk[64];
            p = copy_field(p, junk, sizeof(junk));
        }
    }
    return have_ssid;
}
