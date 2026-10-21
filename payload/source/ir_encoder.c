#include "ir_encoder.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_HEADER_SIZE 16

static inline void write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

static inline void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} byte_builder_t;

static int bb_init(byte_builder_t *bb, size_t initial_cap) {
    bb->cap = initial_cap ? initial_cap : 256;
    bb->len = 0;
    bb->data = (uint8_t *)malloc(bb->cap);
    return bb->data ? 0 : -1;
}

static int bb_append_bytes(byte_builder_t *bb, const uint8_t *src, size_t n) {
    if (bb->len + n > bb->cap) {
        size_t new_cap = bb->cap * 2;
        while (bb->len + n > new_cap) new_cap *= 2;
        uint8_t *next = (uint8_t *)realloc(bb->data, new_cap);
        if (!next) return -1;
        bb->data = next;
        bb->cap = new_cap;
    }
    memcpy(bb->data + bb->len, src, n);
    bb->len += n;
    return 0;
}

static int bb_append_duration(byte_builder_t *bb, uint32_t duration_us, int is_pulse) {
    uint32_t val = duration_us;
    uint8_t chunk[2];
    int words_added = 0;

    while (val > 32767) {
        if (is_pulse) {
            chunk[0] = 0xFF;
            chunk[1] = 0xFF;
        } else {
            chunk[0] = 0x7F;
            chunk[1] = 0xFF;
        }
        if (bb_append_bytes(bb, chunk, 2) != 0) return -1;
        val -= 32767;
        words_added++;
    }

    if (val < 30) val = 30;

    uint16_t word = (uint16_t)(val & 0x7FFF);
    if (is_pulse) word |= 0x8000;

    write_be16(chunk, word);
    if (bb_append_bytes(bb, chunk, 2) != 0) return -1;
    words_added++;
    return words_added;
}

static int parse_hex_words(const char *str, uint16_t **out_words, size_t *out_count) {
    const char *p = str;
    size_t cap = 64, count = 0;
    uint16_t *words = (uint16_t *)malloc(cap * sizeof(uint16_t));
    if (!words) return -1;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char hex[5] = {0};
        int i = 0;
        while (*p && !isspace((unsigned char)*p) && i < 4) {
            hex[i++] = *p++;
        }
        if (i == 0) continue;

        unsigned int val = 0;
        if (sscanf(hex, "%x", &val) != 1) {
            free(words);
            return -1;
        }

        if (count >= cap) {
            cap *= 2;
            uint16_t *nw = (uint16_t *)realloc(words, cap * sizeof(uint16_t));
            if (!nw) {
                free(words);
                return -1;
            }
            words = nw;
        }
        words[count++] = (uint16_t)val;
    }

    *out_words = words;
    *out_count = count;
    return 0;
}

int ir_encode_pronto(const char *pronto_hex, uint8_t min_repeats, uint16_t pre_silence_us,
                     uint8_t **out_buf, size_t *out_len) {
    if (!pronto_hex || !out_buf || !out_len) return -1;

    uint16_t *words = NULL;
    size_t word_count = 0;
    if (parse_hex_words(pronto_hex, &words, &word_count) != 0 || word_count < 4) {
        if (words) free(words);
        return -2;
    }

    /* Word 0 must be 0000 (Learned IR) */
    if (words[0] != 0x0000) {
        free(words);
        return -3;
    }

    uint16_t freq_word = words[1];
    if (freq_word == 0) freq_word = 109; /* default 38kHz (0x006d) */

    /* Carrier period in nanoseconds = freq_word * 241.246 ns */
    uint32_t period_ns = (uint32_t)((double)freq_word * 241.246 + 0.5);
    if (period_ns == 0) period_ns = 26316; /* 38 kHz fallback */

    size_t once_pairs = words[2];
    size_t repeat_pairs = words[3];
    size_t available_pairs = (word_count > 4) ? ((word_count - 4) / 2) : 0;
    if (available_pairs == 0) {
        free(words);
        return -4;
    }
    if (once_pairs > available_pairs) {
        once_pairs = available_pairs;
        repeat_pairs = 0;
    } else if (once_pairs + repeat_pairs > available_pairs) {
        repeat_pairs = available_pairs - once_pairs;
    }

    byte_builder_t bb;
    if (bb_init(&bb, 512) != 0) {
        free(words);
        return -5;
    }

    /* Reserve 16-byte HAL header */
    uint8_t header[IR_HEADER_SIZE] = {0};
    header[0] = 0x01; /* Packet Type 1 */
    write_be32(&header[1], period_ns);
    header[5] = 50;   /* Duty cycle 50% */
    write_be16(&header[6], pre_silence_us ? pre_silence_us : 500);
    header[8] = min_repeats ? min_repeats : 3;
    header[9] = 0x00;

    bb_append_bytes(&bb, header, IR_HEADER_SIZE);

    size_t cur_word = 4;
    uint16_t start_loc = 0;
    uint16_t repeat_loc = 0;
    uint8_t zero_count[2] = {0, 0};

    /* Encode Once-sequence */
    if (once_pairs > 0) {
        start_loc = (uint16_t)bb.len;
        bb_append_bytes(&bb, zero_count, 2);
        uint16_t once_words = 0;
        for (size_t i = 0; i < once_pairs; i++) {
            uint32_t mark_cycles = words[cur_word++];
            uint32_t space_cycles = words[cur_word++];
            uint32_t mark_us = (uint32_t)(((uint64_t)mark_cycles * period_ns + 500) / 1000);
            uint32_t space_us = (uint32_t)(((uint64_t)space_cycles * period_ns + 500) / 1000);
            int w1 = bb_append_duration(&bb, mark_us, 1);
            int w2 = bb_append_duration(&bb, space_us, 0);
            if (w1 > 0) once_words += (uint16_t)w1;
            if (w2 > 0) once_words += (uint16_t)w2;
        }
        write_be16(&bb.data[start_loc], once_words);
    }

    /* Encode Repeat-sequence */
    if (repeat_pairs > 0) {
        repeat_loc = (uint16_t)bb.len;
        bb_append_bytes(&bb, zero_count, 2);
        uint16_t rep_words = 0;
        for (size_t i = 0; i < repeat_pairs; i++) {
            uint32_t mark_cycles = words[cur_word++];
            uint32_t space_cycles = words[cur_word++];
            uint32_t mark_us = (uint32_t)(((uint64_t)mark_cycles * period_ns + 500) / 1000);
            uint32_t space_us = (uint32_t)(((uint64_t)space_cycles * period_ns + 500) / 1000);
            int w1 = bb_append_duration(&bb, mark_us, 1);
            int w2 = bb_append_duration(&bb, space_us, 0);
            if (w1 > 0) rep_words += (uint16_t)w1;
            if (w2 > 0) rep_words += (uint16_t)w2;
        }
        write_be16(&bb.data[repeat_loc], rep_words);
    } else if (once_pairs > 0) {
        /* If no explicit repeat burst, repeat points to once sequence */
        repeat_loc = start_loc;
    }

    free(words);

    /* Update header location offsets */
    write_be16(&bb.data[10], start_loc);
    write_be16(&bb.data[12], repeat_loc);
    write_be16(&bb.data[14], 0); /* finishLocation */

    *out_buf = bb.data;
    *out_len = bb.len;
    return 0;
}

int ir_encode_raw(uint32_t freq_hz, const uint32_t *timings_us, size_t count,
                  uint8_t min_repeats, uint8_t **out_buf, size_t *out_len) {
    if (!timings_us || count == 0 || !out_buf || !out_len) return -1;

    uint32_t period_ns = 26316; /* 38 kHz */
    if (freq_hz > 0) {
        period_ns = (uint32_t)(1000000000ULL / freq_hz);
    }

    byte_builder_t bb;
    if (bb_init(&bb, count * 2 + IR_HEADER_SIZE + 32) != 0) return -1;

    uint8_t header[IR_HEADER_SIZE] = {0};
    header[0] = 0x01;
    write_be32(&header[1], period_ns);
    header[5] = 50;
    write_be16(&header[6], 500);
    header[8] = min_repeats ? min_repeats : 3;
    header[9] = 0x00;

    bb_append_bytes(&bb, header, IR_HEADER_SIZE);

    uint16_t start_loc = (uint16_t)bb.len;
    uint8_t zero_count[2] = {0, 0};
    bb_append_bytes(&bb, zero_count, 2);

    uint16_t word_count = 0;
    for (size_t i = 0; i < count; i++) {
        int is_pulse = (i % 2 == 0);
        int w = bb_append_duration(&bb, timings_us[i], is_pulse);
        if (w > 0) word_count += (uint16_t)w;
    }
    if (count % 2 != 0) {
        /* Ends on pulse - append trailing silence to return IR line to idle level */
        int w = bb_append_duration(&bb, 40000, 0);
        if (w > 0) word_count += (uint16_t)w;
    }

    write_be16(&bb.data[start_loc], word_count);

    write_be16(&bb.data[10], start_loc);
    write_be16(&bb.data[12], start_loc);
    write_be16(&bb.data[14], 0);

    *out_buf = bb.data;
    *out_len = bb.len;
    return 0;
}

int ir_encode_harmony_keycode(const char *keycode, uint8_t min_repeats,
                             uint8_t **out_buf, size_t *out_len) {
    if (!keycode || !keycode[0] || !out_buf || !out_len) return -1;

    /* Handle standard Pronto Hex starting with "0000 " */
    if (strncmp(keycode, "0000 ", 5) == 0) {
        return ir_encode_pronto(keycode, min_repeats, 500, out_buf, out_len);
    }

    /* Handle Harmony raw timing format: F<freq_hex>P<mark_hex>S<space_hex>... */
    if ((keycode[0] == 'F' || keycode[0] == 'f') &&
        (strchr(keycode, 'P') || strchr(keycode, 'p') || strchr(keycode, 'S') || strchr(keycode, 's'))) {
        const char *p = keycode + 1;
        char *endp = NULL;
        unsigned long freq = strtoul(p, &endp, 16);
        uint32_t carrier = freq ? (uint32_t)freq : 38000;
        p = endp;
        uint32_t timings[2048];
        size_t count = 0;
        while (*p && count < sizeof(timings)/sizeof(timings[0])) {
            char type = *p++;
            if (type == 'P' || type == 'p' || type == 'S' || type == 's') {
                unsigned long dur = strtoul(p, &endp, 16);
                p = endp;
                timings[count++] = (uint32_t)dur;
            } else if (isspace((unsigned char)type)) {
                continue;
            } else {
                break;
            }
        }
        if (count >= 4) {
            return ir_encode_raw(carrier, timings, count, min_repeats ? min_repeats : 1, out_buf, out_len);
        }
    }

    char proto[128] = {0};
    char hex[256] = {0};
    uint8_t parsed_repeats = min_repeats;

    /* Check for Harmony protocol format: G:<Proto>:()(<HexCode>)():[Repeats] */
    if (strncmp(keycode, "G:", 2) == 0) {
        const char *p1 = keycode + 2;
        const char *p2 = strchr(p1, ':');
        if (!p2) return -2;
        size_t proto_len = p2 - p1;
        if (proto_len >= sizeof(proto)) proto_len = sizeof(proto) - 1;
        strncpy(proto, p1, proto_len);

        const char *open_p = strchr(p2, '(');
        if (open_p) {
            /* May have multiple sets of parens e.g. ()(0x...)(), find the one with hex */
            while (open_p) {
                const char *close_p = strchr(open_p, ')');
                if (!close_p) break;
                if (close_p > open_p + 1) {
                    size_t hex_len = close_p - (open_p + 1);
                    if (hex_len >= sizeof(hex)) hex_len = sizeof(hex) - 1;
                    strncpy(hex, open_p + 1, hex_len);
                    break;
                }
                open_p = strchr(close_p + 1, '(');
            }
        }

        /* Check for trailing repeat count e.g. :3 */
        const char *last_colon = strrchr(keycode, ':');
        if (last_colon && last_colon > p2) {
            int r = atoi(last_colon + 1);
            if (r > 0 && min_repeats == 0) parsed_repeats = (uint8_t)r;
        }
    } else {
        /* Raw hex or unknown format, fallback */
        strncpy(hex, keycode, sizeof(hex) - 1);
        strcpy(proto, "NEC");
    }

    if (!hex[0]) return -3;
    if (!parsed_repeats) parsed_repeats = 3;

    uint32_t carrier_hz = 38000;
    uint32_t timings[1024];
    size_t timing_count = 0;

    if (strstr(proto, "Sharp 48 Bit 2") || strcasecmp(proto, "Sharp 48 Bit 2") == 0) {
        carrier_hz = 38000;
        timings[timing_count++] = 3364;
        timings[timing_count++] = 1682;
        const char *h = hex;
        if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
        for (int i = 0; h[i] && timing_count < 1000; i++) {
            int val = 0;
            char c = h[i];
            if (c >= '0' && c <= '9') val = c - '0';
            else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
            else continue;
            for (int b = 3; b >= 0; b--) {
                int bit = (val >> b) & 1;
                timings[timing_count++] = 408;
                timings[timing_count++] = (bit == 0) ? 431 : 1272;
            }
        }
        timings[timing_count++] = 408;
        timings[timing_count++] = 73611;
    } else if (strstr(proto, "Sharp 15 Bit 2") || strcasecmp(proto, "Sharp 15 Bit 2") == 0) {
        carrier_hz = 37000;
        char chunk1[64] = {0}, chunk2[64] = {0};
        const char *sep = strchr(hex, '_');
        if (sep) {
            size_t c1_len = sep - hex;
            if (c1_len >= sizeof(chunk1)) c1_len = sizeof(chunk1) - 1;
            strncpy(chunk1, hex, c1_len);
            strncpy(chunk2, sep + 1, sizeof(chunk2) - 1);
        } else {
            strncpy(chunk1, hex, sizeof(chunk1) - 1);
        }

        timings[timing_count++] = 270;
        const char *h = chunk1;
        if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
        unsigned long val1 = strtoul(h, NULL, 16);
        for (int b = 14; b >= 0; b--) {
            int bit = (val1 >> b) & 1;
            timings[timing_count++] = (bit == 0) ? 790 : 1850;
            timings[timing_count++] = 260;
        }

        if (chunk2[0]) {
            timings[timing_count++] = 100000;
            timings[timing_count++] = 270;
            h = chunk2;
            if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
            unsigned long val2 = strtoul(h, NULL, 16);
            for (int b = 14; b >= 0; b--) {
                int bit = (val2 >> b) & 1;
                timings[timing_count++] = (bit == 0) ? 790 : 1850;
                timings[timing_count++] = 260;
            }
        }
    } else if (strstr(proto, "GoVideoO1 32 Bit") || strcasecmp(proto, "GoVideoO1 32 Bit") == 0) {
        carrier_hz = 37900;
        timings[timing_count++] = 4500;
        timings[timing_count++] = 4500;
        const char *h = hex;
        if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
        for (int i = 0; h[i] && timing_count < 1000; i++) {
            int val = 0;
            char c = h[i];
            if (c >= '0' && c <= '9') val = c - '0';
            else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
            else continue;
            for (int b = 3; b >= 0; b--) {
                int bit = (val >> b) & 1;
                timings[timing_count++] = 560;
                timings[timing_count++] = (bit == 0) ? 560 : 1690;
            }
        }
        timings[timing_count++] = 560;
        timings[timing_count++] = 45000; /* Trailing space: 45ms gap */
    } else if (strstr(proto, "Roku 32 Bit 1") || strcasecmp(proto, "Roku 32 Bit 1") == 0) {
        carrier_hz = 38000;
        timings[timing_count++] = 9000;
        timings[timing_count++] = 4500;
        const char *h = hex;
        if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
        for (int i = 0; h[i] && timing_count < 1000; i++) {
            int val = 0;
            char c = h[i];
            if (c >= '0' && c <= '9') val = c - '0';
            else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
            else continue;
            for (int b = 3; b >= 0; b--) {
                int bit = (val >> b) & 1;
                timings[timing_count++] = 562;
                timings[timing_count++] = (bit == 0) ? 562 : 1688;
            }
        }
        timings[timing_count++] = 562;
        timings[timing_count++] = 40000; /* Trailing space: 40ms gap */
    } else if (strstr(proto, "Sony") || strcasecmp(proto, "Sony") == 0) {
        carrier_hz = 40000;
        timings[timing_count++] = 2400;
        timings[timing_count++] = 600;
        const char *h = hex;
        if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
        for (int i = 0; h[i] && timing_count < 1000; i++) {
            int val = 0;
            char c = h[i];
            if (c >= '0' && c <= '9') val = c - '0';
            else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
            else continue;
            for (int b = 3; b >= 0; b--) {
                int bit = (val >> b) & 1;
                timings[timing_count++] = (bit == 0) ? 600 : 1200;
                timings[timing_count++] = 600;
            }
        }
        timings[timing_count++] = 25000; /* Trailing space: 25ms gap */
    } else if (strstr(proto, "RC5") || strcasecmp(proto, "RC5") == 0) {
        carrier_hz = 36000;
        const char *h = hex;
        if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
        unsigned long val = strtoul(h, NULL, 16);
        /* RC-5 Manchester encoding: 14 bits */
        int half_bits[28];
        for (int b = 0; b < 14; b++) {
            int bit = (val >> (13 - b)) & 1;
            if (bit) {
                half_bits[b * 2] = 0;
                half_bits[b * 2 + 1] = 1;
            } else {
                half_bits[b * 2] = 1;
                half_bits[b * 2 + 1] = 0;
            }
        }
        int cur_level = half_bits[0];
        uint32_t cur_dur = 889;
        for (int i = 1; i < 28; i++) {
            if (half_bits[i] == cur_level) {
                cur_dur += 889;
            } else {
                timings[timing_count++] = cur_dur;
                cur_level = half_bits[i];
                cur_dur = 889;
            }
        }
        timings[timing_count++] = cur_dur;
        if (cur_level == 1) {
            timings[timing_count++] = 88900;
        }
    } else {
        /* Standard NEC 32-bit / fallback */
        carrier_hz = 38000;
        timings[timing_count++] = 9000;
        timings[timing_count++] = 4500;
        const char *h = hex;
        if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
        for (int i = 0; h[i] && timing_count < 1000; i++) {
            int val = 0;
            char c = h[i];
            if (c >= '0' && c <= '9') val = c - '0';
            else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
            else continue;
            for (int b = 3; b >= 0; b--) {
                int bit = (val >> b) & 1;
                timings[timing_count++] = 560;
                timings[timing_count++] = (bit == 0) ? 560 : 1690;
            }
        }
        timings[timing_count++] = 560;
        timings[timing_count++] = 40000;
    }

    return ir_encode_raw(carrier_hz, timings, timing_count, parsed_repeats, out_buf, out_len);
}

void ir_free_encoded(uint8_t *buf) {
    if (buf) free(buf);
}
