#pragma once
#include <stdbool.h>
#include <stddef.h>

// Parse a standard "WIFI:S:<ssid>;T:<type>;P:<pass>;;" MECARD-style payload.
// Handles \; \: \, \\ escapes, tolerates field order, case-insensitive scheme
// and keys. Returns true if an SSID was found (password may be empty for open
// networks). Pure C, no ESP dependencies — compilable on the host.
bool qr_parse_wifi(const char *payload, char *ssid_out, size_t ssid_cap,
                   char *pass_out, size_t pass_cap);
