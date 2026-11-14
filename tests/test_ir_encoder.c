/* IR keycode encoder regression test.
 *
 * Expected waveforms are built from the protocol definitions in a stock Harmony
 * Hub's /data/resources/ProtocolList.json (copied below), not from the encoder.
 *
 * Build: cc -Ipayload/source tests/test_ir_encoder.c payload/source/ir_encoder.c -o /tmp/test_ir_encoder && /tmp/test_ir_encoder
 */
#include "ir_encoder.h"
#include "ir_i2s.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
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
static const segment_t SONY = { { 2400 }, 0, { -600, 600 }, { -600, 1200 }, { 0 }, 45000 };
static const segment_t CAMBRIDGE = { { 9025, -4750 }, 32, { 525, -1740 }, { 525, -610 }, { 525, -41360 }, 0 };
static const segment_t SAMSUNG16 = { { 4500, -4480 }, 16, { 495, -500 }, { 495, -1495 }, { 495, -4495 }, 0 };
static const segment_t SAMSUNG20 = { { 0 }, 20, { 495, -500 }, { 495, -1495 }, { 495, -57500 }, 0 };
static const segment_t TOSHIBA = { { 8990, -4490 }, 32, { 568, -552 }, { 568, -1662 }, { 568 }, 107870 };
static const segment_t TOSHIBA_REPEAT = { { 8990, -2230 }, 0, { 0 }, { 0 }, { 568, -96077 }, 0 };
static const segment_t CAMBRIDGE_REPEAT = { { 9025, -2283 }, 0, { 0 }, { 0 }, { 525, -95419 }, 0 };
static const segment_t SAITEK = { { 1874, -520 }, 11, { -525, 0 }, { 460, 0 }, { -809, 0 }, 0 };

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

    /* Sony projector: the 15-bit code has a leading 0 hex bit that must not be sent,
     * and receivers need the frame repeated (the keycode asks for 3) */
    check_frames("G:Sony 15 Bit:()(0x7C2A)():3", 40000, &SONY, 15, 0x7C2A, 3);
    check_frames("G:Sony 20 Bit:()(0xDCB54)():3", 40000, &SONY, 20, 0xDCB54, 3);

    /* Apple TV: two frames once per press, then the repeat code */
    wave_t start = { { 0 }, 0 }, repeat = { { 0 }, 0 };
    wave_add_segment(&start, &CAMBRIDGE, 32, 0x881E057F);
    wave_add_segment(&start, &CAMBRIDGE, 32, 0x881E5F7F);
    wave_add_segment(&repeat, &CAMBRIDGE_REPEAT, 0, 0);
    check("G:Cambridge 32 Bit Dual:(0x881E057F_0x881E5F7F)(Repeat)():3", 38000, &start, &repeat);

    /* NEC family still encodes; a protocol without an encoder is an error, not an NEC frame */
    uint8_t *buf = NULL;
    size_t len = 0;
    assert(ir_encode_harmony_keycode("G:Toshiba 32 Bit:(0x011B54AB)(Repeat)():3", 3, &buf, &len) == 0 && len > 0);
    ir_free_encoded(buf);
    buf = NULL;
    assert(ir_encode_harmony_keycode("G:NEC1:()(0x20DF10EF)():3", 3, &buf, &len) == 0 && len > 0);
    ir_free_encoded(buf);
    buf = NULL;
    assert(ir_encode_harmony_keycode("20DF10EF", 3, &buf, &len) == 0 && len > 0);
    ir_free_encoded(buf);
    buf = NULL;
    assert(ir_encode_harmony_keycode("0x20DF10EF", 3, &buf, &len) == 0 && len > 0);
    ir_free_encoded(buf);
    buf = NULL;
    /* a string that is no code at all is not understood (it used to become an NEC frame of its
     * hex letters): command names that reach this as a fallback, short or long hex, junk */
    assert(ir_encode_harmony_keycode("FastForward", 3, &buf, &len) == -2 && buf == NULL);
    assert(ir_encode_harmony_keycode("Favorite", 3, &buf, &len) == -2 && buf == NULL);
    assert(ir_encode_harmony_keycode("hello", 3, &buf, &len) == -2 && buf == NULL);
    assert(ir_encode_harmony_keycode("zzz", 3, &buf, &len) == -2 && buf == NULL);
    assert(ir_encode_harmony_keycode("20DF10", 3, &buf, &len) == -2 && buf == NULL);
    assert(ir_encode_harmony_keycode("20DF10EF00", 3, &buf, &len) == -2 && buf == NULL);
    assert(ir_encode_harmony_keycode("20DF 10EF", 3, &buf, &len) == -2 && buf == NULL);
    assert(ir_encode_harmony_keycode("G:HID 16 Bit:()(07000027)():1", 1, &buf, &len) != 0);
    assert(ir_encode_harmony_keycode("G:Unknown 24 Bit:()(0x123456)():3", 3, &buf, &len) != 0);
    assert(ir_encode_harmony_keycode("G:Roku IP:()(Home)():1", 1, &buf, &len) == -5);

    /* Saitek 11 Bit: 38150 Hz carrier, 1874us mark / 520us space header, 460us mark (1) / 525us space (0), 809us space trailer */
    check_frames("G:Saitek 11 Bit:()(0x5A5)():3", 38150, &SAITEK, 11, 0x5A5, 1);
    check_frames("G:Saitek 11 Bit:()(0x000)():3", 38150, &SAITEK, 11, 0x000, 1);
    check_frames("G:Saitek 11 Bit:()(0x7FF)():3", 38150, &SAITEK, 11, 0x7FF, 1);

    /* Samsung HW-Q90R soundbar: PowerToggle and Source, a 16-bit and a 20-bit segment
     * per frame; the HAL repeat count sends the frame again */
    wave_t sb = { { 0 }, 0 };
    wave_add_segment(&sb, &SAMSUNG16, 16, 0x0CF0);
    wave_add_segment(&sb, &SAMSUNG20, 20, 0x00EF1);
    check("G:Samsung 16 and 20 Bit:()(0x0CF0_1x00EF1)():3", 38000, &sb, NULL);
    wave_t sb2 = { { 0 }, 0 };
    wave_add_segment(&sb2, &SAMSUNG16, 16, 0x0CF0);
    wave_add_segment(&sb2, &SAMSUNG20, 20, 0x0AE51);
    check("G:Samsung 16 and 20 Bit:()(0x0CF0_1x0AE51)():3", 38000, &sb2, NULL);

    /* Xbox Series X PowerOff: one NEC-style frame, then the repeat code */
    wave_t xs = { { 0 }, 0 }, xr = { { 0 }, 0 };
    wave_add_segment(&xs, &TOSHIBA, 32, 0x011B946B);
    wave_add_segment(&xr, &TOSHIBA_REPEAT, 0, 0);
    check("G:Toshiba 32 Bit:(0x011B946B)(Repeat)():3", 38000, &xs, &xr);

    /* Size limits: a sequence over 10 s is refused (four 0x1FFF8064 us timings also wrapped
     * the 16-bit word count to 0, which played every word), and so is a bit stream over the
     * blaster's limit (600 MHz carrier: 1 ns period). Before the limits the last case built a
     * 31 MB stream; the alarm stops a build that runs away. */
    alarm(20);
    buf = NULL;
    assert(ir_encode_harmony_keycode("F9470P1FFF8064S1FFF8064P1FFF8064S1FFF8064", 1, &buf, &len) != 0);
    assert(ir_encode_pronto("0000 ffff 0001 0000 ffff ffff", 1, 500, &buf, &len) != 0);
    uint32_t long_timings[2048];
    for (int i = 0; i < 2048; i++) long_timings[i] = 65535;
    assert(ir_encode_raw(38000, long_timings, 2048, 1, &buf, &len) != 0);
    assert(ir_encode_raw(2000000000U, long_timings, 4, 1, &buf, &len) != 0);
    uint16_t short_words[4] = { 0x8000 | 9000, 4500, 0x8000 | 560, 30000 };
    assert(ir_i2s_blast_words(2000000000U, 50, short_words, 4, 7) == -1);
    assert(ir_encode_harmony_keycode("F23C34600P7FFFS7FFFP7FFFS7FFF", 1, &buf, &len) == 0 && len > 0);
    assert(ir_i2s_blast_blob(buf, len, 7) == -E2BIG);
    ir_free_encoded(buf);
    buf = NULL;
    /* a normal frame is not refused for its size (here it fails later: no /dev/i2s) */
    assert(ir_encode_harmony_keycode("G:NEC1:()(0x20DF10EF)():3", 3, &buf, &len) == 0);
    assert(ir_i2s_blast_blob(buf, len, 7) != -E2BIG);
    ir_free_encoded(buf);
    alarm(0);

    printf("ALL IR ENCODER TESTS PASSED!\n");
    return 0;
}
