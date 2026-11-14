/*
 * ir_i2s_blast_blob() parses a Logitech IR blob: a 16-byte header, then a 2-byte word count at
 * the header's start location, then the timing words. A blob of 16 or 17 bytes has no room for
 * the count: it is refused with -2 before anything past the end is read. Built with
 * AddressSanitizer, so a read past a heap buffer fails the test. No send happens: every blob
 * here is refused before the I2S device is opened.
 */
#include "ir_i2s.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(c, msg) do { if (c) printf("  PASS %s\n", msg); else { printf("  FAIL %s (line %d)\n", msg, __LINE__); g_failures++; } } while (0)

static int blast_len(size_t len, uint16_t start_loc) {
    uint8_t *b = malloc(len); /* exactly len bytes: ASan sees any read past it */
    if (!b) return -100;
    memset(b, 0, len);
    b[3] = 0x66;                       /* carrier period 26214 ns */
    b[5] = 33;                         /* duty */
    b[10] = (uint8_t)(start_loc >> 8); /* start location */
    b[11] = (uint8_t)start_loc;
    int rc = ir_i2s_blast_blob(b, len, 0x07);
    free(b);
    return rc;
}

int main(void) {
    CHECK(blast_len(15, 16) == -1, "a blob shorter than the header is refused");
    CHECK(blast_len(16, 16) == -2, "a 16-byte blob (header only) is refused without reading past it");
    CHECK(blast_len(17, 16) == -2, "a 17-byte blob is refused without reading past it");
    CHECK(blast_len(17, 0) == -2, "a 17-byte blob with a start location inside the header is refused");
    CHECK(blast_len(16, 400) == -2, "a 16-byte blob with a start location past the end is refused");
    CHECK(blast_len(18, 16) == -2, "an 18-byte blob (count, no words) is refused");
    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All IR blob checks passed\n");
    return 0;
}
