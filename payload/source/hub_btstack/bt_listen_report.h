#ifndef HARMONY_BT_LISTEN_REPORT_H
#define HARMONY_BT_LISTEN_REPORT_H

/* The pure parts of "adv_listen" (no BTstack): the filter and the datagram a plugin gets for
 * each advertising report. Host-tested in tests/test_bt_listen_report.c. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ADV_LISTEN_FILTER_MAX 16

static inline int adv_listen_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Hex (even length, at most ADV_LISTEN_FILTER_MAX bytes) to bytes; an empty string is no
 * filter (0). Returns the byte count, or -1 for anything else. */
static inline int adv_listen_parse_filter(const char *hex, uint8_t out[ADV_LISTEN_FILTER_MAX]) {
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > ADV_LISTEN_FILTER_MAX) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = adv_listen_hex_nibble(hex[2 * i]), lo = adv_listen_hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(n / 2);
}

/* 1 when the filter is empty or occurs anywhere in the advertising data */
static inline int adv_listen_match(const uint8_t *ad, int ad_len, const uint8_t *flt, int flt_len) {
    if (flt_len <= 0) return 1;
    for (int i = 0; i + flt_len <= ad_len; i++) {
        if (memcmp(ad + i, flt, (size_t)flt_len) == 0) return 1;
    }
    return 0;
}

/* The datagram for one report. addr in reading order (AA:BB:..). Returns the length, or -1
 * when it does not fit. */
static inline int adv_listen_format(char *out, size_t out_sz, const char *plugin_id, const uint8_t addr[6],
                                    int addr_type, int event_type, int rssi, const uint8_t *ad, int ad_len) {
    char hex[2 * 31 + 1];
    if (ad_len < 0 || ad_len > 31) return -1;
    for (int i = 0; i < ad_len; i++) snprintf(hex + 2 * i, 3, "%02X", ad[i]);
    hex[2 * ad_len] = '\0';
    int n = snprintf(out, out_sz,
                     "{\"plugin_id\":\"%s\",\"command\":\"adv_report\",\"action\":\"tap\",\"value\":0,"
                     "\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"addr_type\":%d,\"event_type\":%d,"
                     "\"rssi\":%d,\"data\":\"%s\"}",
                     plugin_id, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr_type, event_type,
                     rssi, hex);
    return (n < 0 || (size_t)n >= out_sz) ? -1 : n;
}

#endif /* HARMONY_BT_LISTEN_REPORT_H */
