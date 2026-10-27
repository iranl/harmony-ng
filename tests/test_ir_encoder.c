/* IR keycode encoder regression test.
 *
 * Expected waveforms are built from the protocol definitions in a stock Harmony
 * Hub's /data/resources/ProtocolList.json (copied below), not from the encoder.
 *
 * Build: cc -Ipayload/source tests/test_ir_encoder.c payload/source/ir_encoder.c -o /tmp/test_ir_encoder && /tmp/test_ir_encoder
 */
#include "ir_encoder.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Durations: positive = mark (carrier on), negative = space */
typedef struct {
    int header[2];
    int bits;
    int zero[2]; /* atoms for a 0 bit */
    int one[2];  /* atoms for a 1 bit */
    int trailer[2];
    int total;   /* frame length from the start of the segment, 0 = unpadded */
} segment_t;

static const segment_t RC5 = { { 889 }, 13, { 889, -889 }, { -889, 889 }, { 0 }, 113792 };
static const segment_t RC5EX = { { 900 }, 7, { 900, -900 }, { -900, 900 }, { -3515 }, 0 };
static const segment_t RC5EX_1 = { { 0 }, 12, { 900, -900 }, { -900, 900 }, { 0 }, 95700 };

typedef struct {
    int d[256];
    int n;
} wave_t;

static int wave_sum(const wave_t *w) {
    int sum = 0;
    for (int i = 0; i < w->n; i++) sum += abs(w->d[i]);
    return sum;
}

/* Appends a duration, merging same-level neighbours and dropping leading idle space */
static void wave_add(wave_t *w, int d) {
    if (d == 0 || (w->n == 0 && d < 0)) return;
    if (w->n > 0 && (w->d[w->n - 1] > 0) == (d > 0)) {
        w->d[w->n - 1] += d;
    } else {
        w->d[w->n++] = d;
    }
}

static void wave_add_segment(wave_t *w, const segment_t *s, int bits, unsigned long value) {
    int start = wave_sum(w);
    for (int i = 0; i < 2; i++) wave_add(w, s->header[i]);
    for (int b = bits - 1; b >= 0; b--) {
        const int *atoms = ((value >> b) & 1) ? s->one : s->zero;
        wave_add(w, atoms[0]);
        wave_add(w, atoms[1]);
    }
    for (int i = 0; i < 2; i++) wave_add(w, s->trailer[i]);
    if (s->total) wave_add(w, -(s->total - (wave_sum(w) - start)));
}

/* Reads the HAL sequence whose offset is at byte `slot` of the encoded blob */
static wave_t decode_sequence(const uint8_t *b, int slot) {
    wave_t w = { { 0 }, 0 };
    size_t loc = ((size_t)b[slot] << 8) | b[slot + 1];
    size_t words = ((size_t)b[loc] << 8) | b[loc + 1];
    for (size_t i = 0; i < words; i++) {
        uint16_t word = ((uint16_t)b[loc + 2 + 2 * i] << 8) | b[loc + 3 + 2 * i];
        int us = word & 0x7FFF;
        wave_add(&w, (word & 0x8000) ? us : -us);
    }
    return w;
}

static void expect_wave(const char *what, const wave_t *got, const wave_t *want) {
    int same = got->n == want->n;
    for (int i = 0; same && i < want->n; i++) same = got->d[i] == want->d[i];
    if (!same) {
        printf("FAIL %s\n  got: ", what);
        for (int i = 0; i < got->n; i++) printf(" %d", got->d[i]);
        printf("\n want:");
        for (int i = 0; i < want->n; i++) printf(" %d", want->d[i]);
        printf("\n");
        exit(1);
    }
}

static void check(const char *keycode, uint32_t carrier_hz, const wave_t *start, const wave_t *repeat) {
    uint8_t *buf = NULL;
    size_t len = 0;
    assert(ir_encode_harmony_keycode(keycode, 0, &buf, &len) == 0);
    uint32_t period_ns = ((uint32_t)buf[1] << 24) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 8) | buf[4];
    assert(period_ns == 1000000000u / carrier_hz);
    wave_t got_start = decode_sequence(buf, 10);
    wave_t got_repeat = decode_sequence(buf, 12);
    expect_wave(keycode, &got_start, start);
    expect_wave(keycode, &got_repeat, repeat ? repeat : start);
    printf("ok %s\n", keycode);
    ir_free_encoded(buf);
}

static void check_frames(const char *keycode, uint32_t carrier_hz, const segment_t *s, int bits,
                         unsigned long value, int frames) {
    wave_t w = { { 0 }, 0 };
    for (int f = 0; f < frames; f++) wave_add_segment(&w, s, bits, value);
    check(keycode, carrier_hz, &w, NULL);
}

static void check_rc5ex(const char *keycode, unsigned long address, unsigned long command) {
    wave_t w = { { 0 }, 0 };
    wave_add_segment(&w, &RC5EX, 7, address);
    wave_add_segment(&w, &RC5EX_1, 12, command);
    check(keycode, 36000, &w, NULL);
}

int main(void) {
    /* Marantz receiver: address 16, command 13 (Mute) / 16 (Volume up) / 80 (S2 = 0) */
    check_frames("G:Philips RC5 13 Bit Toggle:()(0x140D)():3", 36000, &RC5, 13, 0x140D, 1);
    check_frames("G:Philips RC5 13 Bit Toggle:()(0x1410)():3", 36000, &RC5, 13, 0x1410, 1);
    check_frames("G:Philips RC5 13 Bit Toggle:()(0x0450)():3", 36000, &RC5, 13, 0x0450, 1);

    /* Marantz receiver PowerOn (address 16, command 12, data 1) and Back (field bit 0) */
    check_rc5ex("G:Philips RC5Ex:()(0x50_1x301)():3", 0x50, 0x301);
    check_rc5ex("G:Philips RC5Ex:()(0x10_1x5C4)():3", 0x10, 0x5C4);

    printf("ALL IR ENCODER TESTS PASSED!\n");
    return 0;
}
