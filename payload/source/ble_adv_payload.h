/*
 * ble_adv_payload.h — Parse and check BLE advertising payloads for broadcast commands.
 *
 * A broadcast command stores legacy advertising data (a sequence of AD structures,
 * at most 31 bytes) as hex after the "BLEADV:" keyCode prefix. hub_daemon checks the
 * payload before handing it to hub_btstack, which parses it again before advertising.
 *
 * All functions are static inline so each translation unit gets its own copy.
 */
#ifndef BLE_ADV_PAYLOAD_H
#define BLE_ADV_PAYLOAD_H

#include <stdint.h>

#define BLE_ADV_MAX_LEN 31
#define BLE_ADV_KEYCODE_PREFIX "BLEADV:"
#define BLE_ADV_KEYCODE_PREFIX_LEN 7

static inline int ble_adv_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Each AD structure is a length byte followed by that many bytes (type + data);
 * the structures must fill the payload exactly. */
static inline int ble_adv_structure_valid(const uint8_t *data, int len) {
    int pos = 0;
    if (len <= 0 || len > BLE_ADV_MAX_LEN) return 0;
    while (pos < len) {
        int ad_len = data[pos];
        if (ad_len == 0) return 0;
        pos += 1 + ad_len;
    }
    return pos == len;
}

/* Parse hex advertising data up to the first space, tab, newline or end of string.
 * Returns the byte count, or -1 if the hex has an odd digit count, a non-hex
 * character, more than BLE_ADV_MAX_LEN bytes, or invalid AD structures.
 * When end is not NULL it is set to the character after the hex. */
static inline int ble_adv_parse_hex(const char *hex, uint8_t *out, const char **end) {
    int len = 0;
    if (!hex) return -1;
    while (*hex && *hex != ' ' && *hex != '\t' && *hex != '\r' && *hex != '\n') {
        int hi = ble_adv_hex_nibble(hex[0]);
        int lo = hex[1] ? ble_adv_hex_nibble(hex[1]) : -1;
        if (hi < 0 || lo < 0 || len >= BLE_ADV_MAX_LEN) return -1;
        out[len++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    if (end) *end = hex;
    return ble_adv_structure_valid(out, len) ? len : -1;
}

#endif /* BLE_ADV_PAYLOAD_H */
