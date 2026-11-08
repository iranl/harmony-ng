/* IR receive decoder (ir_i2s_decode_capture) on a real Harmony Hub recording and on edge cases.
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
    if (failures) {
        printf("test_ir_rx_decode: %d FAILED\n", failures);
        return 1;
    }
    printf("test_ir_rx_decode: all passed\n");
    return 0;
}
