/*
 * Unit test of the mfg partition parser (payload/source/mfg_id.c): a synthetic blob with the
 * records at 0x9000, one with the records at 0, a blob without the key, a truncated one, an
 * implausible length, and a file read through mfg_read_unitid().
 *
 * Build: gcc -Wall -Wextra -O2 -Ipayload/source tests/test_mfg.c payload/source/mfg_id.c
 */
#include "mfg_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

static size_t put_rec(uint8_t *p, const char *s) {
    size_t n = strlen(s);
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16); p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
    memcpy(p + 4, s, n);
    return 4 + n;
}

/* Records area as on the devices: magic, unitid, arch, skin, ..., key (binary) */
static size_t build_records(uint8_t *p, const char *unitid, int with_unitid) {
    size_t n = 0;
    memcpy(p, MFG_MAGIC, MFG_MAGIC_LEN);
    n += MFG_MAGIC_LEN;
    if (with_unitid) {
        n += put_rec(p + n, "unitid");
        n += put_rec(p + n, unitid);
    }
    n += put_rec(p + n, "arch");
    n += put_rec(p + n, "0x11");
    n += put_rec(p + n, "skin");
    n += put_rec(p + n, "0x61");
    n += put_rec(p + n, "usb_vendor_id");
    n += put_rec(p + n, "0x046D");
    n += put_rec(p + n, "hw_ver");
    n += put_rec(p + n, "01.00");
    n += put_rec(p + n, "key");
    /* a 1192-byte binary value, as on the Hub */
    size_t klen = 1192;
    p[n] = 0; p[n + 1] = 0; p[n + 2] = (uint8_t)(klen >> 8); p[n + 3] = (uint8_t)klen;
    n += 4;
    for (size_t i = 0; i < klen; i++) p[n + i] = (uint8_t)(i * 7);
    n += klen;
    return n;
}

int main(void) {
    printf("mfg_id test\n");
    char out[MFG_UNITID_MAX];
    const char *id = "20140101120000-000001";

    /* 1. Full partition image: 0xFF everywhere, records at 0x9000 */
    size_t len = 0x10000;
    uint8_t *blob = malloc(len);
    memset(blob, 0xFF, len);
    build_records(blob + MFG_RECORDS_OFF, id, 1);
    CHECK(mfg_parse_unitid(blob, len, out, sizeof(out)) == 0 && strcmp(out, id) == 0, "unit id from a full image");

    /* 2. Only the record area (small harness file) */
    uint8_t small[2048];
    size_t slen = build_records(small, id, 1);
    CHECK(mfg_parse_unitid(small, slen, out, sizeof(out)) == 0 && strcmp(out, id) == 0, "unit id from the record area alone");

    /* 3. Value with trailing blanks is trimmed */
    uint8_t padded[2048];
    size_t plen = build_records(padded, "  20140101120000-000002 \n", 1);
    CHECK(mfg_parse_unitid(padded, plen, out, sizeof(out)) == 0 && strcmp(out, "20140101120000-000002") == 0, "trimmed value");

    /* 4. No unitid key: -1, out empty */
    uint8_t nokey[2048];
    size_t nlen = build_records(nokey, id, 0);
    CHECK(mfg_parse_unitid(nokey, nlen, out, sizeof(out)) == -1 && out[0] == '\0', "blob without the key");

    /* 5. Truncated before the unitid value */
    CHECK(mfg_parse_unitid(small, 20, out, sizeof(out)) == -1, "truncated blob");

    /* 6. Implausible length in the first record */
    uint8_t bad[64];
    memcpy(bad, MFG_MAGIC, MFG_MAGIC_LEN);
    bad[8] = 0x7F; bad[9] = 0xFF; bad[10] = 0xFF; bad[11] = 0xFF;
    CHECK(mfg_parse_unitid(bad, sizeof(bad), out, sizeof(out)) == -1, "implausible record length");

    /* 7. Wrong magic, empty blob, erased partition */
    CHECK(mfg_parse_unitid((const uint8_t *)"nothing here at all", 19, out, sizeof(out)) == -1, "no magic");
    CHECK(mfg_parse_unitid(small, 0, out, sizeof(out)) == -1, "empty blob");
    memset(blob, 0xFF, len);
    CHECK(mfg_parse_unitid(blob, len, out, sizeof(out)) == -1, "erased partition");

    /* 8. Output buffer too small */
    char tiny[8];
    CHECK(mfg_parse_unitid(small, slen, tiny, sizeof(tiny)) == -1, "small output buffer");

    /* 9. Through a file, as the harness does */
    char path[] = "/tmp/mfg_test_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "temp file");
    if (fd >= 0) {
        build_records(blob + MFG_RECORDS_OFF, id, 1);
        FILE *f = fdopen(fd, "wb");
        fwrite(blob, 1, len, f);
        fclose(f);
        CHECK(mfg_read_unitid(path, out, sizeof(out)) == 0 && strcmp(out, id) == 0, "read from a file");
        unlink(path);
    }
    CHECK(mfg_read_unitid("/nonexistent/mfg.bin", out, sizeof(out)) == -1, "missing file");
    free(blob);

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All mfg_id checks passed\n");
    return 0;
}
