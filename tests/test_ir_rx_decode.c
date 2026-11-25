/* IR receive decoders (ir_i2s_decode_capture, ir_i2s_decode_capture_press) on a real Harmony Hub recording and on edge cases.
 *
 * tests/fixtures/ir/samsung36_volume_up.i2s: /dev/i2s receive data (as read, 470 ms) from a
 * Harmony Hub while a Samsung HW-Q90R soundbar remote held Volume Up about 8 cm away. The sensor
 * missed the start of the first frame; frames repeat every ~110 ms. The stored code for that key
 * is G:Samsung 16 and 20 Bit:()(0x0CF0_1x0EE11)(). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_i2s.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  [PASS] "); } else { printf("  [FAIL] "); failures++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static size_t parse(const char *raw, unsigned *carrier, unsigned *v, size_t max) {
    size_t n = 0;
    char *end = NULL;
    *carrier = (unsigned)strtoul(raw + 1, &end, 16);
    for (const char *p = end; *p && n < max; ) {
        if (*p == 'P' || *p == 'S') {
            v[n++] = (unsigned)strtoul(p + 1, &end, 16);
            p = end;
        } else {
            p++;
        }
    }
    return n;
}

static void test_recording(const char *path) {
    printf("Real recording: Samsung 36-bit Volume Up\n");
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "fixture opens (%s)", path);
    if (!f) return;
    static uint8_t buf[64 * 1024];
    size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    char out[8192];
    int rc = ir_i2s_decode_capture(buf, len, out, sizeof(out));
    CHECK(rc == 0 && out[0] == 'F', "decodes (rc %d)", rc);
    unsigned carrier = 0, v[256];
    size_t n = parse(out, &carrier, v, 256);
    CHECK(carrier >= 37000 && carrier <= 39000, "carrier %u Hz", carrier);
    /* header 2, 16 bits, separator, 20 bits, end mark: 2 + 32 + 2 + 40 + 1 = 77 timings + the frame gap */
    CHECK(n == 78, "one complete frame plus its gap (%zu timings)", n);
    if (n != 78) return;
    CHECK(v[0] > 4400 && v[0] < 4650 && v[1] > 4400 && v[1] < 4650, "header %u/%u us (4500/4500)", v[0], v[1]);
    int marks_ok = 1;
    char bits[64] = {0};
    size_t nb = 0;
    for (size_t k = 2; k + 1 < n; k += 2) {
        if (v[k] < 450 || v[k] > 560) marks_ok = 0;
        unsigned s = v[k + 1];
        if (s > 450 && s < 560) bits[nb++] = '0';
        else if (s > 1400 && s < 1600) bits[nb++] = '1';
        else if (s > 4400 && s < 4650) bits[nb++] = '|';
        else if (k + 1 == n - 1) break;   /* the frame gap */
        else bits[nb++] = '?';
    }
    CHECK(marks_ok, "every mark within 450..560 us (500)");
    CHECK(strcmp(bits, "0000110011110000|00001110111000010001") == 0,
          "bits %s = 0x0CF0 | 0x0EE11 (the stored code)", bits);
    CHECK(v[n - 1] > 50000 && v[n - 1] < 60000, "trailing space = the measured frame gap (%u us)", v[n - 1]);
}

/* ---- whole press (the one-shot learn, ir_i2s_decode_capture_press) ---- */

/* Renders mark/space timings (us, starting with a mark) as /dev/i2s receive data: one bit per
 * 1.5 us, a mark as 38 kHz carrier pulses (1/3 duty), stored as the Hub delivers it (each 16-bit
 * sample byte-swapped and inverted). idle_before/after_us of no light around it. Returns bytes. */
static size_t render(const unsigned *t, size_t n, unsigned idle_before_us, unsigned idle_after_us,
                     uint8_t *buf, size_t cap) {
    memset(buf, 0xFF, cap);   /* 0xFF = no light */
    unsigned long long pos_ns = (unsigned long long)idle_before_us * 1000ULL;
    for (size_t k = 0; k < n; k++) {
        unsigned long long len_ns = (unsigned long long)t[k] * 1000ULL;
        if (k % 2 == 0) {
            for (unsigned long long ns = 0; ns < len_ns; ns += 1500) {
                unsigned long long in_period = (ns * 38000ULL / 1000ULL) % 1000000ULL;   /* ns * f, in ppm of a period */
                if (in_period < 333333ULL) {
                    size_t bit = (size_t)((pos_ns + ns) / 1500ULL);
                    if (bit / 8 < cap) buf[(bit / 8) ^ 1u] &= (uint8_t)~(1u << (7 - bit % 8));
                }
            }
        }
        pos_ns += len_ns;
    }
    pos_ns += (unsigned long long)idle_after_us * 1000ULL;
    size_t bytes = (size_t)(pos_ns / 1500ULL / 8ULL + 1) & ~(size_t)1;
    return bytes < cap ? bytes : cap;
}

/* frames (a space of 20 ms or more ends one, the last space too) and timings of a raw string */
static size_t frames_of(const char *raw, unsigned *v, size_t max, size_t *frames) {
    unsigned carrier = 0;
    size_t n = parse(raw, &carrier, v, max);
    *frames = 0;
    for (size_t k = 1; k < n; k += 2) if (v[k] >= 20000) (*frames)++;
    return n;
}

static size_t nec_frame(unsigned *t) {
    size_t n = 0;
    t[n++] = 9000; t[n++] = 4500;
    unsigned code = 0x20DF10EFu;
    for (int b = 31; b >= 0; b--) { t[n++] = 560; t[n++] = ((code >> b) & 1u) ? 1690 : 560; }
    t[n++] = 560;
    return n;   /* 67: ends with a mark */
}

static void test_whole_press(const char *path) {
    printf("Whole press (one-shot learn)\n");
    static uint8_t buf[512 * 1024];
    static unsigned v[4096];
    static char out[32768];
    size_t frames = 0;

    /* the real recording: frame 1 cut by the sensor (56 timings), frames 2 and 3 whole (77),
     * frame 4 cut by the end of the recording (no quiet after it) */
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t len = fread(buf, 1, 64 * 1024, f);
        fclose(f);
        int rc = ir_i2s_decode_capture_press(buf, len, out, sizeof(out), NULL);
        size_t n = frames_of(out, v, 4096, &frames);
        CHECK(rc == 0 && frames == 2 && n == 156,
              "real recording: the two whole frames, the cut first and last dropped (rc %d, %zu frames, %zu timings)", rc, frames, n);
        CHECK(n >= 80 && v[0] > 4300 && v[0] < 4700 && v[78] > 4300 && v[78] < 4700,
              "both frames start with the 4.5 ms header (%u, %u us)", n ? v[0] : 0, n > 78 ? v[78] : 0);
    } else {
        CHECK(0, "fixture opens (%s)", path);
    }

    /* NEC: the frame, then two repeat codes (9 ms, 2.25 ms, 560 us) 108 ms apart, then quiet */
    unsigned t[1024];
    size_t m = nec_frame(t);
    unsigned frame_us = 0;
    for (size_t k = 0; k < m; k++) frame_us += t[k];
    t[m++] = 108000 - frame_us;
    for (int r = 0; r < 2; r++) {
        t[m++] = 9000; t[m++] = 2250; t[m++] = 560;
        if (r == 0) t[m++] = 108000 - 11810;
    }
    size_t len = render(t, m, 2000, 210000, buf, sizeof(buf));
    int rc = ir_i2s_decode_capture_press(buf, len, out, sizeof(out), NULL);
    size_t n = frames_of(out, v, 4096, &frames);
    CHECK(rc == 0 && frames == 3 && n == 68 + 4 + 4,
          "NEC: the frame and both repeat codes kept (rc %d, %zu frames, %zu timings)", rc, frames, n);
    CHECK(n > 70 && v[0] > 8900 && v[0] < 9100 && v[68] > 8900 && v[69] > 2150 && v[69] < 2350,
          "NEC: header first, then a repeat code (%u, %u, %u us)", v[0], n > 68 ? v[68] : 0, n > 69 ? v[69] : 0);

    /* a first frame the sensor cut: the second half of an NEC frame, then two whole frames */
    m = 0;
    unsigned full[80];
    size_t fl = nec_frame(full);
    for (size_t k = 40; k < fl; k++) t[m++] = full[k];   /* starts at a mark */
    t[m++] = 40000;
    for (int r = 0; r < 2; r++) {
        for (size_t k = 0; k < fl; k++) t[m++] = full[k];
        if (r == 0) t[m++] = 40000;
    }
    len = render(t, m, 2000, 210000, buf, sizeof(buf));
    rc = ir_i2s_decode_capture_press(buf, len, out, sizeof(out), NULL);
    n = frames_of(out, v, 4096, &frames);
    CHECK(rc == 0 && frames == 2 && n == 2 * 68 && v[0] > 8900 && v[0] < 9100,
          "a cut first frame is dropped, the two whole ones kept (rc %d, %zu frames, %zu timings, first %u us)", rc, frames, n, v[0]);

    /* no quiet at the end (a recording limit cut the last frame): it is dropped */
    m = 0;
    for (int r = 0; r < 3; r++) {
        for (size_t k = 0; k < fl; k++) t[m++] = full[k];
        t[m++] = 40000;
    }
    for (size_t k = 0; k < 20; k++) t[m++] = full[k];   /* cut: ends at a space */
    len = render(t, m, 2000, 0, buf, sizeof(buf));
    rc = ir_i2s_decode_capture_press(buf, len, out, sizeof(out), NULL);
    n = frames_of(out, v, 4096, &frames);
    CHECK(rc == 0 && frames == 3 && n == 3 * 68,
          "a last frame cut by the recording end is dropped (rc %d, %zu frames, %zu timings)", rc, frames, n);

    /* a single frame: the same code as the capture mode's one-frame decoder */
    m = nec_frame(t);
    len = render(t, m, 2000, 210000, buf, sizeof(buf));
    static char one[8192];
    rc = ir_i2s_decode_capture_press(buf, len, out, sizeof(out), NULL);
    int rc1 = ir_i2s_decode_capture(buf, len, one, sizeof(one));
    CHECK(rc == 0 && rc1 == 0 && strcmp(out, one) == 0, "a single frame decodes as in the capture mode");

    /* stock's limit: at most 1023 mark/space pairs (2046 timings) */
    static unsigned big[40 * 70];
    size_t bm = 0;
    for (int r = 0; r < 40; r++) {
        for (size_t k = 0; k < fl; k++) big[bm++] = full[k];
        big[bm++] = 40000;
    }
    len = render(big, bm, 2000, 0, buf, sizeof(buf));
    rc = ir_i2s_decode_capture_press(buf, len, out, sizeof(out), NULL);
    n = frames_of(out, v, 4096, &frames);
    CHECK(rc == 0 && n <= 2047 && n % 68 == 0, "a long hold stops at 1023 pairs, whole frames only (rc %d, %zu timings, %zu frames)", rc, n, frames);

    CHECK(ir_i2s_decode_capture_press(buf, len, out, 31, NULL) == -1, "output buffer too small (-1)");
}

static void test_edges(void) {
    printf("Edge cases\n");
    static uint8_t idle[4096];
    memset(idle, 0xFF, sizeof(idle));
    char out[256];
    CHECK(ir_i2s_decode_capture(idle, sizeof(idle), out, sizeof(out)) == -5, "an idle line is 'no signal' (-5)");

    /* one short flash: a single mark is not a code */
    static uint8_t flash[4096];
    memcpy(flash, idle, sizeof(flash));
    for (size_t i = 1000; i < 1004; i++) flash[i] = 0x00;
    CHECK(ir_i2s_decode_capture(flash, sizeof(flash), out, sizeof(out)) == -6, "a single flash is rejected (-6)");

    CHECK(ir_i2s_decode_capture(NULL, 10, out, sizeof(out)) == -1, "NULL buffer (-1)");
    CHECK(ir_i2s_decode_capture(idle, sizeof(idle), out, 8) == -1, "output buffer too small (-1)");
}

int main(int argc, char **argv) {
    test_recording(argc > 1 ? argv[1] : "tests/fixtures/ir/samsung36_volume_up.i2s");
    test_edges();
    test_whole_press(argc > 1 ? argv[1] : "tests/fixtures/ir/samsung36_volume_up.i2s");
    if (failures) {
        printf("test_ir_rx_decode: %d FAILED\n", failures);
        return 1;
    }
    printf("test_ir_rx_decode: all passed\n");
    return 0;
}
