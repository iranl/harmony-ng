/* payload/source/icon_pack.c against archives from tests/test_icon_pack.py --fixture DIR: icons
 * come out byte for byte (also one larger than a part), folder PNGs by file name with the old
 * linking order, a plain archive is not used (callers unpack it as before), and a changed archive
 * is read again. Built with -DICON_PACK_ARCHIVES='{"DIR/icons.tar.gz"}'. Usage: test_icon_pack DIR */
#include "icon_pack.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void expect(const char *name, const char *want) {
    size_t len = 0;
    unsigned char *got = icon_pack_read(name, &len);
    assert(got);
    assert(len == strlen(want) && memcmp(got, want, len) == 0);
    free(got);
}

static int s_count, s_with_slash, s_sorted = 1;
static char s_last[128];
static void count_name(const char *name, void *ctx) {
    (void)ctx;
    s_count++;
    if (strchr(name, '/')) s_with_slash++;
    if (s_last[0] && strcmp(s_last, name) >= 0) s_sorted = 0;
    snprintf(s_last, sizeof(s_last), "%s", name);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    char packed[512], plain[512], held[512], big[512];
    snprintf(packed, sizeof(packed), "%s/icons.tar.gz", argv[1]);
    snprintf(plain, sizeof(plain), "%s/plain.tar.gz", argv[1]);
    snprintf(held, sizeof(held), "%s/held.tar.gz", argv[1]);
    snprintf(big, sizeof(big), "%s/big.png", argv[1]);

    assert(icon_pack_available());
    expect("shared.png", "from-b");
    expect("a/shared.png", "from-a");
    expect("top01.png", "folder-wins");
    expect("a/deep/x.png", "two levels: not linked");
    assert(icon_pack_has("top02.png"));
    assert(!icon_pack_has("x.png"));
    assert(!icon_pack_has("note.txt"));
    assert(!icon_pack_has("missing.png"));
    assert(!icon_pack_read("missing.png", NULL));

    size_t want_len = 0, got_len = 0;
    unsigned char *want = read_file(big, &want_len);
    unsigned char *got = icon_pack_read("big.png", &got_len);
    assert(got && got_len == want_len && memcmp(got, want, want_len) == 0);
    free(got);
    free(want);

    icon_pack_list(count_name, NULL);
    /* 60 topNN + big + shared (top01 is one of the 60) */
    assert(s_count == 62 && s_with_slash == 0 && s_sorted);

    /* a plain archive in its place: not a pack, callers unpack it as before */
    assert(rename(packed, held) == 0);
    size_t plain_len = 0;
    unsigned char *pl = read_file(plain, &plain_len);
    FILE *f = fopen(packed, "wb");
    assert(f && fwrite(pl, 1, plain_len, f) == plain_len);
    fclose(f);
    free(pl);
    assert(!icon_pack_available());
    assert(!icon_pack_read("shared.png", NULL));

    /* no archive at all, then the pack again */
    assert(remove(packed) == 0);
    assert(!icon_pack_available());
    assert(rename(held, packed) == 0);
    assert(icon_pack_available());
    expect("shared.png", "from-b");

    printf("test_icon_pack (C): OK\n");
    return 0;
}
