/*
 * mantra_codec.h — Build and recognise Mantra Lighting fan/light BLE commands.
 *
 * Mantra ceiling fans and lights (mAntra lighting app, HuiZhou NewEnergy) take commands as
 * BLE advertisements. The packet layout, whitening and command values here follow the
 * "mantra" codec of ha-ble-adv by @NicoIIT
 * (https://github.com/NicoIIT/ha-ble-adv, custom_components/ble_adv/codecs/mantra.py),
 * which is distributed under this license:
 *
 *   MIT License
 *
 *   Copyright (c) 2019 - 2025  @NicoIIT
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 *
 * Packet body (18 bytes): 72 <0E|0F> <counter:2> 06 <cmd> 12 34 56 78 <id:2> <param> <arg0..arg4>
 * Bytes from offset 5 are whitened with a 16-bit LFSR seeded by the counter. The app sends the
 * body as manufacturer data (company 0x6F4E) and, from iOS, as a list of six 32-bit service
 * UUIDs: 4E 6F <body> 04 03 02 01. The id is the fan's own 16-bit id, which it announces when
 * paired (mantra_encode_pair_adv, mantra_decode_search_reply).
 *
 * All functions are static inline so each translation unit gets its own copy.
 */
#ifndef MANTRA_CODEC_H
#define MANTRA_CODEC_H

#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define MANTRA_KEYCODE_PREFIX "MANTRA:"
#define MANTRA_KEYCODE_PREFIX_LEN 7
#define MANTRA_BODY_LEN 18
/* Flags + complete list of six 32-bit service UUIDs */
#define MANTRA_ADV_LEN 29

static inline void mantra_whiten16(uint8_t *buf, int len, uint16_t seed) {
    uint16_t r = seed;
    for (int i = 0; i < len; i++) {
        uint8_t b = 0;
        for (int j = 0; j < 8; j++) {
            uint16_t high = r & 0x8000;
            r = (uint16_t)(r << 1);
            if (high) {
                r ^= 4777;
                b |= (uint8_t)(1 << (7 - j));
            }
            if (r == 0) r = 1061;
        }
        buf[i] ^= (uint8_t)(73 ^ b);
    }
}

/* Whiten an 18-byte body (2-byte prefix, then counter) and wrap it as the app's iOS-style
 * advertisement: flags, then a list of six 32-bit service UUIDs <header> <body> 04 03 02 01. */
static inline int mantra_wrap_adv(uint8_t h0, uint8_t h1, uint8_t body[MANTRA_BODY_LEN], uint8_t out[MANTRA_ADV_LEN]) {
    mantra_whiten16(body + 5, MANTRA_BODY_LEN - 5, (uint16_t)((body[2] << 8) | body[3]));
    static const uint8_t head[5] = {0x02, 0x01, 0x1A, 0x19, 0x05};
    static const uint8_t foot[4] = {0x04, 0x03, 0x02, 0x01};
    memcpy(out, head, sizeof(head));
    out[5] = h0;
    out[6] = h1;
    memcpy(out + 7, body, MANTRA_BODY_LEN);
    memcpy(out + 7 + MANTRA_BODY_LEN, foot, sizeof(foot));
    return MANTRA_ADV_LEN;
}

/* cmd7 = cmd, param, arg0..arg4. version 0 or 1 selects the 0x0E / 0x0F codec variant. */
static inline int mantra_encode_adv(int version, uint16_t id, uint16_t counter, const uint8_t cmd7[7], uint8_t out[MANTRA_ADV_LEN]) {
    uint8_t body[MANTRA_BODY_LEN] = {
        0x72, version ? 0x0F : 0x0E, (uint8_t)(counter >> 8), (uint8_t)counter, 0x06, cmd7[0],
        0x12, 0x34, 0x56, 0x78, (uint8_t)(id >> 8), (uint8_t)id,
        cmd7[1], cmd7[2], cmd7[3], cmd7[4], cmd7[5], cmd7[6],
    };
    return mantra_wrap_adv(0x4E, 0x6F, body, out);
}

/* Pairing, decoded from the mAntra lighting app's broadcasts (ha-ble-adv has no pairing for
 * Mantra). Within 30 s of being powered on, the fan answers a search (header 69 63, prefix 5F 48,
 * same whitening) with its own id; the app then sends a bind repeating "00 01 <id> <id>" from the
 * answer, and the light blinks 3 times. The id belongs to the fan: every controller uses it. */
static inline int mantra_encode_pair_adv(int bind, uint16_t id, uint16_t counter, uint8_t out[MANTRA_ADV_LEN]) {
    uint8_t hi = (uint8_t)(id >> 8), lo = (uint8_t)id;
    uint8_t body[MANTRA_BODY_LEN] = {0x5F, 0x48, (uint8_t)(counter >> 8), (uint8_t)counter, 0x06};
    if (bind) {
        const uint8_t rest[13] = {0x05, 0x00, 0x01, hi, lo, hi, lo, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00};
        memcpy(body + 5, rest, sizeof(rest));
    } else {
        const uint8_t rest[13] = {0xF4, 0x12, 0x34, 0x56, 0x78, 0x01, 0xE1, 0xE2, 0xE3, 0xE4, 0x00, 0x00, 0x00};
        memcpy(body + 5, rest, sizeof(rest));
    }
    return mantra_wrap_adv(0x69, 0x63, body, out);
}

/* Find the 18-byte Mantra body in raw advertising data, in either form: manufacturer data
 * (company 0x6F4E) or the iOS-style UUID list. Returns NULL if there is none. */
static inline const uint8_t *mantra_find_body(const uint8_t *ad, int ad_len) {
    int pos = 0;
    while (pos + 1 < ad_len) {
        int len = ad[pos];
        if (len == 0 || pos + 1 + len > ad_len) break;
        uint8_t type = ad[pos + 1];
        const uint8_t *data = ad + pos + 2;
        int data_len = len - 1;
        if (data_len >= 2 + MANTRA_BODY_LEN && data[0] == 0x4E && data[1] == 0x6F &&
            data[2] == 0x72 && (data[3] == 0x0E || data[3] == 0x0F)) {
            if ((type == 0xFF && data_len == 2 + MANTRA_BODY_LEN) ||
                (type == 0x05 && data_len == 2 + MANTRA_BODY_LEN + 4 &&
                 data[20] == 0x04 && data[21] == 0x03 && data[22] == 0x02 && data[23] == 0x01)) {
                return data + 2;
            }
        }
        pos += 1 + len;
    }
    return NULL;
}

static inline void mantra_unwhiten_body(const uint8_t *in, uint8_t body[MANTRA_BODY_LEN]) {
    memcpy(body, in, MANTRA_BODY_LEN);
    mantra_whiten16(body + 5, MANTRA_BODY_LEN - 5, (uint16_t)((body[2] << 8) | body[3]));
}

/* Recognise a command from a controller (06) or the fan's acknowledgement of one (01, cmd F6);
 * both carry the family bytes and the fan id. Returns 1 and sets id/version. */
static inline int mantra_decode_adv(const uint8_t *ad, int ad_len, uint16_t *id, int *version) {
    const uint8_t *in = mantra_find_body(ad, ad_len);
    if (!in) return 0;
    uint8_t body[MANTRA_BODY_LEN];
    mantra_unwhiten_body(in, body);
    int from_app = (body[4] == 0x06);
    int fan_ack = (body[4] == 0x01 && body[5] == 0xF6);
    if (!(from_app || fan_ack) || body[6] != 0x12 || body[7] != 0x34 || body[8] != 0x56 || body[9] != 0x78) return 0;
    if (id) *id = (uint16_t)((body[10] << 8) | body[11]);
    if (version) *version = (in[1] == 0x0F);
    return 1;
}

/* Recognise the fan's answer to a pairing search: 01 F5 <2 bytes> 00 01 <id> <id>.
 * Returns 1 and sets the fan id and codec version. */
static inline int mantra_decode_search_reply(const uint8_t *ad, int ad_len, uint16_t *id, int *version) {
    const uint8_t *in = mantra_find_body(ad, ad_len);
    if (!in) return 0;
    uint8_t body[MANTRA_BODY_LEN];
    mantra_unwhiten_body(in, body);
    if (body[4] != 0x01 || body[5] != 0xF5 || body[8] != 0x00 || body[9] != 0x01) return 0;
    if (body[10] != body[12] || body[11] != body[13]) return 0;
    if (id) *id = (uint16_t)((body[10] << 8) | body[11]);
    if (version) *version = (in[1] == 0x0F);
    return 1;
}

static inline int mantra_round255(double v) {
    int r = (int)(v * 255.0 + 0.5);
    return r < 0 ? 0 : (r > 255 ? 255 : r);
}

/* The fan sets brightness and colour temperature together (cmd 0x02).
 * br_pct 1-100; ct_pct 0 (cool) - 100 (warm). Values follow ha-ble-adv's cold/warm light. */
static inline void mantra_light_command(int br_pct, int ct_pct, uint8_t cmd7[7]) {
    double br = br_pct / 100.0, ct = ct_pct / 100.0, ct_rev = 1.0 - ct;
    double cold = (1.0 - ct) * 2.0, warm = ct * 2.0;
    if (cold > 1.0) cold = 1.0;
    if (warm > 1.0) warm = 1.0;
    cmd7[0] = 0x02;
    cmd7[1] = (uint8_t)mantra_round255(br * warm);
    cmd7[2] = (uint8_t)mantra_round255(br * cold);
    cmd7[3] = (uint8_t)(int)(7.0 * br + 0.5);
    cmd7[4] = (uint8_t)(int)(6.0 * ct_rev + 0.5);
    cmd7[5] = (uint8_t)mantra_round255(br);
    cmd7[6] = (uint8_t)mantra_round255(ct_rev);
}

/* Turn an action into cmd7. Actions (case-insensitive):
 *   LIGHT_ON LIGHT_OFF FAN_ON FAN_OFF ALL_OFF FORWARD REVERSE BREEZE SLEEP
 *   TIMER_1H TIMER_2H TIMER_4H TIMER_8H   (version 0: all; version 1: LIGHT_ON / LIGHT_OFF)
 *   SPEED:<0-31>       fan speed (version 0)
 *   BRIGHTNESS:<1-100> keeps the last colour temperature
 *   CT:<0-100>         colour temperature, 0 cool to 100 warm; keeps the last brightness
 *   RAW:<cmd>:<param>:<a0>:<a1>:<a2>:<a3>:<a4>  hex bytes
 * br_pct / ct_pct carry the light's last brightness and colour temperature between calls.
 * Returns 0, or -1 for an unknown action or out-of-range value. */
static inline int mantra_build_command(int version, const char *action, int *br_pct, int *ct_pct, uint8_t cmd7[7]) {
    static const struct { const char *name; uint8_t v0; uint8_t v1; } simple[] = {
        {"LIGHT_ON", 0x05, 0x01}, {"LIGHT_OFF", 0x06, 0x02}, {"FAN_ON", 0x07, 0},
        {"FAN_OFF", 0x08, 0}, {"ALL_OFF", 0x02, 0}, {"FORWARD", 0x12, 0},
        {"REVERSE", 0x14, 0}, {"BREEZE", 0x0D, 0}, {"SLEEP", 0x0E, 0},
        {"TIMER_1H", 0x09, 0}, {"TIMER_2H", 0x0A, 0}, {"TIMER_4H", 0x0B, 0},
        {"TIMER_8H", 0x0C, 0},
    };
    if (!action) return -1;
    memset(cmd7, 0, 7);

    for (size_t i = 0; i < sizeof(simple) / sizeof(simple[0]); i++) {
        if (strcasecmp(action, simple[i].name) == 0) {
            uint8_t param = version ? simple[i].v1 : simple[i].v0;
            if (!param) return -1;
            cmd7[0] = 0x01;
            cmd7[1] = param;
            return 0;
        }
    }

    const char *colon = strchr(action, ':');
    if (!colon) return -1;
    size_t name_len = (size_t)(colon - action);
    const char *val = colon + 1;
    char *end = NULL;

    if (name_len == 5 && strncasecmp(action, "SPEED", 5) == 0) {
        long n = strtol(val, &end, 10);
        if (version || end == val || *end || n < 0 || n > 31) return -1;
        cmd7[0] = 0x03;
        cmd7[1] = 0x01;
        cmd7[2] = (uint8_t)n;
        return 0;
    }
    if ((name_len == 10 && strncasecmp(action, "BRIGHTNESS", 10) == 0) ||
        (name_len == 2 && strncasecmp(action, "CT", 2) == 0)) {
        long n = strtol(val, &end, 10);
        int is_br = (name_len == 10);
        if (end == val || *end || n < (is_br ? 1 : 0) || n > 100) return -1;
        if (is_br) *br_pct = (int)n;
        else *ct_pct = (int)n;
        mantra_light_command(*br_pct, *ct_pct, cmd7);
        return 0;
    }
    if (name_len == 3 && strncasecmp(action, "RAW", 3) == 0) {
        const char *p = val;
        for (int i = 0; i < 7; i++) {
            long b = strtol(p, &end, 16);
            if (end == p || b < 0 || b > 255) return -1;
            cmd7[i] = (uint8_t)b;
            if (i < 6) {
                if (*end != ':') return -1;
                p = end + 1;
            } else if (*end) {
                return -1;
            }
        }
        return 0;
    }
    return -1;
}

/* Whether an action names a value but leaves it out ("BRIGHTNESS" rather than "BRIGHTNESS:50"):
 * the value then comes with each send. Returns 1 and sets the range, or 0. */
static inline int mantra_value_range(const char *action, int *min, int *max) {
    static const struct { const char *name; int min, max; } ranged[] = {
        {"SPEED", 0, 31}, {"BRIGHTNESS", 1, 100}, {"CT", 0, 100},
    };
    if (!action) return 0;
    for (size_t i = 0; i < sizeof(ranged) / sizeof(ranged[0]); i++) {
        if (strcasecmp(action, ranged[i].name) == 0) {
            *min = ranged[i].min;
            *max = ranged[i].max;
            return 1;
        }
    }
    return 0;
}

/* Parse a fan id written as hex ("C5F0" or "0xC5F0"). Returns 0 and sets *id, or -1. */
static inline int mantra_parse_id(const char *s, uint16_t *id) {
    char *end = NULL;
    if (!s || !s[0]) return -1;
    long v = strtol(s, &end, 16);
    if (end == s || *end || v < 0 || v > 0xFFFF) return -1;
    *id = (uint16_t)v;
    return 0;
}

#endif /* MANTRA_CODEC_H */
