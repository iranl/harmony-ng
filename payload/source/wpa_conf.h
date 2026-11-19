#ifndef WPA_CONF_H
#define WPA_CONF_H

/*
 * Values written into a wpa_supplicant network block (ssid="...", psk="...").
 *
 * wpa_supplicant takes a quoted value as it is: the text between the first quote and the last
 * quote of the line, with no escape sequences. So a value is written unchanged (a backslash
 * added in front of $ or " would become part of the key), and only what the format cannot carry
 * is refused: line breaks and other control characters, and lengths wpa_supplicant rejects.
 * A pre-shared key of exactly 64 hex digits is the raw key and is written without quotes.
 *
 * Header only, used by the Wi-Fi settings API in hub_daemon.
 */

#include <stddef.h>
#include <string.h>

static inline int wpa_psk_is_hex(const char *v) {
    if (!v || strlen(v) != 64) return 0;
    for (const char *p = v; *p; p++) {
        char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

/* NULL when v can be written as ssid="v" (is_psk 0) or as the network's psk (is_psk 1),
 * otherwise the reason, for an error message */
static inline const char *wpa_conf_value_error(const char *v, int is_psk) {
    if (!v) return is_psk ? "Password missing" : "SSID missing";
    size_t n = strlen(v);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c < 0x20 || c == 0x7f) {
            return is_psk ? "The password contains a line break or another control character"
                          : "The SSID contains a line break or another control character";
        }
    }
    if (!is_psk) {
        if (n == 0) return "SSID missing";
        if (n > 32) return "The SSID is longer than 32 bytes";
        return NULL;
    }
    if (wpa_psk_is_hex(v)) return NULL;
    if (n < 8 || n > 63) return "The Wi-Fi password must have 8 to 63 characters (or be 64 hex digits)";
    return NULL;
}

/* Quote to put around a psk value: none for the 64 hex digit raw key */
static inline const char *wpa_psk_quote(const char *v) {
    return wpa_psk_is_hex(v) ? "" : "\"";
}

#endif /* WPA_CONF_H */
