/*
 * ir_i2s_blast_blob() parses a Logitech IR blob: a 16-byte header, then a 2-byte word count at
 * the header's start location, then the timing words. A blob of 16 or 17 bytes has no room for
 * the count: it is refused with -2 before anything past the end is read. Built with
 * AddressSanitizer, so a read past a heap buffer fails the test. No send happens: every blob
 * here is refused before the I2S device is opened.
 */
#include "ir_i2s.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(c, msg) do { if (c) printf("  PASS %s\n", msg); else { printf("  FAIL %s (line %d)\n", msg, __LINE__); g_failures++; } } while (0)

/* One part of a blob, as a held key is played (ir_i2s_blast_blob_part) */
static int blast_part(size_t len, int part, uint16_t start_loc, uint16_t repeat_loc, uint16_t finish_loc) {
    uint8_t *b = malloc(len);
    if (!b) return -100;
    memset(b, 0, len);
    b[3] = 0x66;
    b[5] = 33;
    b[10] = (uint8_t)(start_loc >> 8);
    b[11] = (uint8_t)start_loc;
    if (len >= 16) {
        b[12] = (uint8_t)(repeat_loc >> 8);
        b[13] = (uint8_t)repeat_loc;
        b[14] = (uint8_t)(finish_loc >> 8);
        b[15] = (uint8_t)finish_loc;
    }
    int rc = ir_i2s_blast_blob_part(b, len, part, 0x07);
    free(b);
    return rc;
}

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
    CHECK(blast_part(15, IR_BLOB_START, 16, 16, 0) == -1, "part: a blob shorter than the header is refused");
    CHECK(blast_part(17, IR_BLOB_START, 16, 16, 0) == -2, "part: the start of a 17-byte blob is refused");
    CHECK(blast_part(18, IR_BLOB_REPEAT, 16, 400, 0) == -2, "part: a repeat location past the end falls back to the start");
    CHECK(blast_part(18, IR_BLOB_REPEAT, 16, 17, 0) == -2, "part: a repeat location on the last byte is not read");
    CHECK(blast_part(18, IR_BLOB_FINISH, 16, 16, 0) == -ENOENT, "part: a code without a finish sequence has none");
    CHECK(blast_part(18, IR_BLOB_FINISH, 16, 16, 400) == -ENOENT, "part: a finish location past the end counts as none");
    CHECK(blast_part(18, IR_BLOB_FINISH, 16, 16, 16) == -ENOENT, "part: a finish location with no room for a word counts as none");
    CHECK(blast_part(24, IR_BLOB_FINISH, 16, 16, 18) == -ENOENT, "part: a finish sequence with a word count of 0 counts as none");
    CHECK(blast_part(18, 7, 16, 16, 0) == -1, "part: an unknown part is refused");
    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All IR blob checks passed\n");
    return 0;
}
