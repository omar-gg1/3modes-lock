// Host self-check for qr_parse_wifi. Build & run (POSIX):
//   cc -DQR_PARSE_HOST_TEST -o /tmp/t test_qr_parse.c qr_parse.c && /tmp/t
// The parser is pure C; we declare its prototype + the two size macros here so
// this compiles without the ESP headers that qr_scan.h pulls in.
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define WIFI_CREDS_SSID_MAX 32
#define WIFI_CREDS_PASS_MAX 63

bool qr_parse_wifi(const char *payload, char *ssid_out, size_t ssid_cap,
                   char *pass_out, size_t pass_cap);

int main(void)
{
    char s[WIFI_CREDS_SSID_MAX + 1], p[WIFI_CREDS_PASS_MAX + 1];
    size_t sc = sizeof(s), pc = sizeof(p);

    // Standard payload, field order S,T,P.
    assert(qr_parse_wifi("WIFI:S:SPACEDOME;T:WPA;P:spacedome@2021;;", s, sc, p, pc));
    assert(strcmp(s, "SPACEDOME") == 0);
    assert(strcmp(p, "spacedome@2021") == 0);

    // Reordered fields, lowercase keys, hidden flag present.
    assert(qr_parse_wifi("WIFI:T:WPA;P:secret;H:false;S:MyNet;;", s, sc, p, pc));
    assert(strcmp(s, "MyNet") == 0 && strcmp(p, "secret") == 0);

    // Open network: password field empty, still valid (SSID present).
    assert(qr_parse_wifi("WIFI:S:Cafe;T:nopass;P:;;", s, sc, p, pc));
    assert(strcmp(s, "Cafe") == 0 && p[0] == '\0');

    // Open network with no P field at all.
    assert(qr_parse_wifi("WIFI:S:Guest;;", s, sc, p, pc));
    assert(strcmp(s, "Guest") == 0 && p[0] == '\0');

    // Escaped separators inside the values ( \; \: \\ ).
    assert(qr_parse_wifi("WIFI:S:my\\;net;P:a\\:b\\\\c;;", s, sc, p, pc));
    assert(strcmp(s, "my;net") == 0);
    assert(strcmp(p, "a:b\\c") == 0);

    // No SSID -> reject.
    assert(!qr_parse_wifi("WIFI:T:WPA;P:orphan;;", s, sc, p, pc));

    // Not a WIFI payload -> reject (e.g. a URL QR).
    assert(!qr_parse_wifi("https://example.com", s, sc, p, pc));

    // Case-insensitive scheme.
    assert(qr_parse_wifi("wifi:S:Lower;;", s, sc, p, pc));
    assert(strcmp(s, "Lower") == 0);

    printf("qr_parse OK\n");
    return 0;
}
