/* Philips RC5 encoder regression test.
 * Build: cc -Ipayload/source tests/test_ir_rc5.c payload/source/ir_encoder.c -o /tmp/test_ir_rc5 && /tmp/test_ir_rc5
 */
#include "ir_encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define HALF_US 889

/* Decode the first RC5 frame of an encoded IR blob back into its 14-bit value, or -1 */
static int decode_rc5(const uint8_t *b, size_t len) {
    size_t start = ((size_t)b[10] << 8) | b[11];
    size_t words = ((size_t)b[start] << 8) | b[start + 1];
    int halves[32];
    int n = 0;
    halves[n++] = 0; /* idle line before the first pulse is the first half of start bit S1 */
    for (size_t i = 0; i < words && start + 3 + 2 * i < len; i++) {
        uint16_t w = ((uint16_t)b[start + 2 + 2 * i] << 8) | b[start + 3 + 2 * i];
        int level = (w & 0x8000) ? 1 : 0;
        int us = w & 0x7FFF;
        if (i == 0 && !level) return -1; /* must start with a pulse */
        if (us > 20000) break;           /* inter-frame gap */
        int count = (us + HALF_US / 2) / HALF_US;
        if (count < 1 || count > 2 || n + count > 28) return -1;
        while (count--) halves[n++] = level;
    }
    if (n == 27) halves[n++] = 0;
    if (n != 28) return -1;
    int value = 0;
    for (int i = 0; i < 28; i += 2) {
        if (halves[i] == halves[i + 1]) return -1; /* every RC5 bit has a mid-bit transition */
        value = (value << 1) | halves[i + 1];
    }
    return value;
}

int main(void) {
    struct { const char *keycode; int expected; } cases[] = {
        /* Marantz receiver: S1=1, S2=1, T=0, address 16, command 13 (Mute) / 16 (Volume up) */
        { "G:Philips RC5 13 Bit Toggle:()(0x140D)():3", 0x340D },
        { "G:Philips RC5 13 Bit Toggle:()(0x1410)():3", 0x3410 },
        /* S2=0 (command >= 64) */
        { "G:Philips RC5 13 Bit Toggle:()(0x0450)():3", 0x2450 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t *buf = NULL;
        size_t len = 0;
        assert(ir_encode_harmony_keycode(cases[i].keycode, 0, &buf, &len) == 0);
        int got = decode_rc5(buf, len);
        printf("%s -> 0x%04X (expected 0x%04X)\n", cases[i].keycode, got, cases[i].expected);
        assert(got == cases[i].expected);
        ir_free_encoded(buf);
    }
    printf("ALL RC5 ENCODER TESTS PASSED!\n");
    return 0;
}
