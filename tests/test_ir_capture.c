/* The raw-string parser shared by the IR encoder and the capture events (spec 3.14), on a real
 * Harmony Hub recording (tests/fixtures/ir/samsung36_volume_up.i2s: a Samsung HW-Q90R soundbar
 * remote holding Volume Up, stored code G:Samsung 16 and 20 Bit:()(0x0CF0_1x0EE11)()). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_encoder.h"
#include "ir_i2s.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  [PASS] "); } else { printf("  [FAIL] "); failures++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static void test_recording(const char *path) {
    printf("Recorded capture -> ir_received timings\n");
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "fixture opens (%s)", path);
    if (!f) return;
    static uint8_t buf[64 * 1024];
    size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    char raw[8192];
    uint32_t measured = 0;
    int rc = ir_i2s_decode_capture_ex(buf, len, raw, sizeof(raw), &measured);
    CHECK(rc == 0, "decodes (rc %d)", rc);
    CHECK(measured >= 37000 && measured <= 39000, "measured carrier %u Hz (the event's modulation)", measured);

    static int32_t t[IR_RAW_MAX_TIMINGS];
    uint32_t carrier = 0;
    int n = ir_parse_harmony_raw(raw, &carrier, t, IR_RAW_MAX_TIMINGS);
    CHECK(n == 78, "78 timings (one frame and its gap), got %d", n);
    CHECK(carrier == measured, "F value %u = the measured carrier", carrier);
    int alternates = n > 0;
    for (int i = 0; i < n; i++) {
        if ((i % 2 == 0 && t[i] <= 0) || (i % 2 == 1 && t[i] >= 0)) alternates = 0;
    }
    CHECK(alternates, "marks positive, spaces negative, alternating from a mark");
    CHECK(n > 2 && t[0] > 4400 && t[0] < 4650 && t[1] < -4400 && t[1] > -4650, "header +%d / %d us", n > 0 ? t[0] : 0, n > 1 ? t[1] : 0);
    CHECK(n > 0 && t[n - 1] < -50000, "last value is the frame gap (%d us)", n > 0 ? t[n - 1] : 0);

    uint8_t *blob = NULL;
    size_t blob_len = 0;
    int erc = ir_encode_harmony_keycode(raw, 1, &blob, &blob_len);
    CHECK(erc == 0 && blob && blob_len > 16, "the encoder plays the same string (rc %d, %zu bytes)", erc, blob_len);
    free(blob);
}

static void test_parser(void) {
    printf("Parser\n");
    int32_t t[8];
    uint32_t c = 1;
    CHECK(ir_parse_harmony_raw("F9470P100S200P300", &c, t, 8) == 3 && c == 0x9470 &&
          t[0] == 0x100 && t[1] == -0x200 && t[2] == 0x300, "F/P/S tokens, signs, carrier");
    CHECK(ir_parse_harmony_raw("f9470 p10 s20", &c, t, 8) == 2 && t[0] == 0x10 && t[1] == -0x20, "lower case and spaces");
    CHECK(ir_parse_harmony_raw("F9470P10S20:3", &c, t, 8) == 2, "anything else ends the list");
    CHECK(ir_parse_harmony_raw("F9470P1S2P3S4P5S6P7S8P9S10", &c, t, 8) == 8, "stops at max");
    CHECK(ir_parse_harmony_raw("0000 006D 0022", &c, t, 8) == -1, "a Pronto string is not a raw string");
    CHECK(ir_parse_harmony_raw("G:NEC:(0x1)", &c, t, 8) == -1, "a Harmony keycode is not a raw string");
    CHECK(ir_parse_harmony_raw("FP10", &c, t, 8) == -1, "F without a value");
    CHECK(ir_parse_harmony_raw(NULL, &c, t, 8) == -1, "NULL");
}

int main(int argc, char **argv) {
    test_parser();
    test_recording(argc > 1 ? argv[1] : "tests/fixtures/ir/samsung36_volume_up.i2s");
    if (failures) {
        printf("test_ir_capture: %d FAILED\n", failures);
        return 1;
    }
    printf("test_ir_capture: all passed\n");
    return 0;
}
