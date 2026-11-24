/* payload/source/icon_pack.c, post-merge check of #258 (findings 7 and 8): each part's CRC is
 * computed once per loaded index (not at every icon read) and again after the archive is
 * replaced; a damaged pack is still refused; a reader racing archive swaps never gets wrong
 * bytes; a file with the pack header whose index is not readable is logged once per file state;
 * a pack of another version is not read and is reported with its version (callers unpack it
 * into /tmp only when the RAM allows it); a gzip file with an 'HI' subfield of another layout
 * is a plain archive.
 * Archives from tests/icon_pack_verify_fixture.py DIR. Built with -DICON_PACK_TEST and
 * -DICON_PACK_ARCHIVES='{"DIR/icons.tar.gz"}'. Usage: test_icon_pack_verify DIR */
#include "icon_pack.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

unsigned long icon_pack_test_crc_runs(void);
extern void (*icon_pack_test_before_open)(void);

static const char *s_dir;
static char s_log[512];
static int s_failed;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            fprintf(stderr, "  PASS ");                                                            \
        } else {                                                                                   \
            fprintf(stderr, "  FAIL ");                                                            \
            s_failed++;                                                                            \
        }                                                                                          \
        fprintf(stderr, __VA_ARGS__);                                                              \
        fprintf(stderr, "\n");                                                                     \
    } while (0)

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Puts DIR/<name> in place of the archive as a new file (copy, then rename), like a deploy */
static void install(const char *name) {
    char src[600], tmp[600], dst[600];
    snprintf(src, sizeof(src), "%s/%s", s_dir, name);
    snprintf(tmp, sizeof(tmp), "%s/icons.tar.gz.new", s_dir);
    snprintf(dst, sizeof(dst), "%s/icons.tar.gz", s_dir);
    size_t len = 0;
    unsigned char *b = read_file(src, &len);
    FILE *f = fopen(tmp, "wb");
    if (!b || !f || fwrite(b, 1, len, f) != len) {
        fprintf(stderr, "cannot copy %s\n", src);
        exit(2);
    }
    fclose(f);
    free(b);
    if (rename(tmp, dst) != 0) exit(2);
}

/* How often needle is in the library's output so far (stdout goes to s_log) */
static int log_count(const char *needle) {
    fflush(stdout);
    size_t len = 0;
    char *t = (char *)read_file(s_log, &len);
    int n = 0;
    for (char *p = t; p && (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    free(t);
    return n;
}

/* The parts count of the last "Icons read from" line */
static int last_parts(void) {
    fflush(stdout);
    size_t len = 0;
    char *t = (char *)read_file(s_log, &len);
    int parts = -1;
    for (char *p = t; p && (p = strstr(p, "names, ")) != NULL; p += 7) parts = atoi(p + 7);
    free(t);
    return parts;
}

#define NICONS 41
static const char *icon_name(int i) {
    static char names[NICONS][16];
    if (!names[i][0]) {
        if (i == NICONS - 1) snprintf(names[i], sizeof(names[i]), "big.png");
        else snprintf(names[i], sizeof(names[i]), "top%02d.png", i);
    }
    return names[i];
}

static unsigned char *s_want[2][NICONS];
static size_t s_want_len[2][NICONS];

/* 0: the bytes of set a, 1: of set b, -1: neither (NULL: -2) */
static int which_set(int i, const unsigned char *got, size_t len) {
    if (!got) return -2;
    for (int s = 0; s < 2; s++) {
        if (len == s_want_len[s][i] && memcmp(got, s_want[s][i], len) == 0) return s;
    }
    return -1;
}

static int read_all(int set) {
    int ok = 1;
    for (int i = 0; i < NICONS; i++) {
        size_t len = 0;
        unsigned char *got = icon_pack_read(icon_name(i), &len);
        if (which_set(i, got, len) != set) ok = 0;
        free(got);
    }
    return ok;
}

static int s_stop; /* __atomic: read by the reader threads */
static unsigned long s_wrong, s_reads;
static pthread_mutex_t s_count_lock = PTHREAD_MUTEX_INITIALIZER;

static void *reader(void *arg) {
    unsigned seed = (unsigned)(size_t)arg;
    unsigned long wrong = 0, reads = 0;
    while (!__atomic_load_n(&s_stop, __ATOMIC_SEQ_CST)) {
        int i = (int)(rand_r(&seed) % NICONS);
        size_t len = 0;
        unsigned char *got = icon_pack_read(icon_name(i), &len);
        if (which_set(i, got, len) == -1) wrong++;
        reads++;
        free(got);
    }
    pthread_mutex_lock(&s_count_lock);
    s_wrong += wrong;
    s_reads += reads;
    pthread_mutex_unlock(&s_count_lock);
    return NULL;
}

static void swap_to_flipped(void) { install("aflip.tar.gz"); }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    s_dir = argv[1];
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < NICONS; i++) {
            char p[1024];
            snprintf(p, sizeof(p), "%.900s/%c/%.100s", s_dir, s ? 'b' : 'a', icon_name(i));
            s_want[s][i] = read_file(p, &s_want_len[s][i]);
            if (!s_want[s][i]) return 2;
        }
    }
    /* the library's log lines are counted: its stdout goes to a file */
    snprintf(s_log, sizeof(s_log), "%s/icon_pack.log", s_dir);
    int lfd = open(s_log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lfd < 0 || dup2(lfd, 1) < 0) return 2;
    close(lfd);
    setvbuf(stdout, NULL, _IONBF, 0);

    fprintf(stderr, "[-] icon pack: part check once per index, unreadable index logged\n");

    /* finding 7: each part's CRC once per loaded index */
    install("a.tar.gz");
    CHECK(read_all(0), "set a: every icon read byte for byte");
    unsigned long runs1 = icon_pack_test_crc_runs();
    int parts = last_parts();
    CHECK(read_all(0) && read_all(0), "set a read twice more");
    for (int k = 0; k < 50; k++) free(icon_pack_read("big.png", NULL));
    unsigned long runs2 = icon_pack_test_crc_runs();
    CHECK(runs1 >= 1 && (int)runs1 <= parts, "first pass: %lu part checks for %d parts", runs1, parts);
    CHECK(runs2 == runs1, "123 more reads: no more part checks (%lu -> %lu)", runs1, runs2);

    /* a damaged pack in its place: every part checked again and refused */
    install("badcrc.tar.gz");
    int nulls = 0;
    for (int i = 0; i < NICONS; i++) {
        unsigned char *got = icon_pack_read(icon_name(i), NULL);
        if (!got) nulls++;
        free(got);
    }
    CHECK(nulls == NICONS, "pack with wrong part CRCs: %d of %d icons refused", nulls, NICONS);
    CHECK(log_count("does not decode to its bytes") >= 1, "damaged part logged");
    free(icon_pack_read("top00.png", NULL));
    free(icon_pack_read("top00.png", NULL));
    int bad_lines = log_count("does not decode to its bytes");
    free(icon_pack_read("top00.png", NULL));
    CHECK(log_count("does not decode to its bytes") == bad_lines, "the same damaged part is logged once");

    /* the good pack again: checked again (a new index), served */
    unsigned long runs3 = icon_pack_test_crc_runs();
    install("a.tar.gz");
    CHECK(read_all(0), "set a again after the damaged pack");
    CHECK(icon_pack_test_crc_runs() > runs3, "the replaced archive's parts are checked again");

    /* readers racing archive swaps (a and b differ in every icon and in size) */
    pthread_t th[4];
    __atomic_store_n(&s_stop, 0, __ATOMIC_SEQ_CST);
    for (int t = 0; t < 4; t++) pthread_create(&th[t], NULL, reader, (void *)(size_t)(t + 1));
    for (int k = 0; k < 60; k++) {
        install(k % 2 ? "a.tar.gz" : "b.tar.gz");
        struct timespec ts = { 0, 3 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    __atomic_store_n(&s_stop, 1, __ATOMIC_SEQ_CST);
    for (int t = 0; t < 4; t++) pthread_join(th[t], NULL);
    CHECK(s_wrong == 0 && s_reads > 0, "60 swaps during %lu reads: %lu answers with wrong bytes", s_reads, s_wrong);
    /* the archive replaced after the index lookup and before the read opens it (the window the
     * racing readers rarely hit) by a copy with the same size and index whose big.png part
     * inflates to other bytes: a part verified on the indexed file must be checked again */
    install("a.tar.gz");
    CHECK(read_all(0), "set a read (its parts verified)");
    icon_pack_test_before_open = swap_to_flipped;
    size_t flen = 0;
    unsigned char *fgot = icon_pack_read("big.png", &flen);
    icon_pack_test_before_open = NULL;
    CHECK(which_set(NICONS - 1, fgot, flen) == -2, "replaced between lookup and read: big.png refused, not served with wrong bytes");
    free(fgot);
    fgot = icon_pack_read("big.png", &flen);
    CHECK(!fgot, "the replaced copy indexed again: big.png refused");
    free(fgot);
    install("b.tar.gz");
    CHECK(read_all(1), "set b read after the swaps");

    /* finding 8: a file with the pack header whose index is not readable */
    const char *msg = "index not readable, no stock icons until it is deployed again";
    int before = log_count(msg);
    install("cut.tar.gz");
    int avail = icon_pack_available() + icon_pack_available() + icon_pack_available();
    CHECK(avail == 0, "cut pack: no icons");
    CHECK(log_count(msg) == before + 1, "cut pack: logged once over three calls (%d)", log_count(msg) - before);
    install("cut2.tar.gz");
    (void)icon_pack_available();
    (void)icon_pack_available();
    CHECK(log_count(msg) == before + 2, "another cut: logged once more");
    char p[600];
    snprintf(p, sizeof(p), "%s/icons.tar.gz", s_dir);
    int ver = -2;
    CHECK(icon_pack_file_kind(p, &ver) == ICON_ARCHIVE_PACK && ver == ICON_PACK_VERSION,
          "cut pack is still a pack of this version (not unpacked into RAM)");
    install("plain.tar.gz");
    (void)icon_pack_available();
    CHECK(log_count(msg) == before + 2 && icon_pack_file_kind(p, &ver) == ICON_ARCHIVE_PLAIN && ver == -1,
          "plain archive: no pack, nothing logged");

    /* a pack of another version (a downgrade): not read here, nothing logged here (the caller
     * that may unpack it logs the outcome), reported as PACK_OTHER with its version */
    const char *vmsg = "this firmware reads version";
    int vbefore = log_count(vmsg);
    install("vnext.tar.gz");
    avail = icon_pack_available() + icon_pack_available() + icon_pack_available();
    CHECK(avail == 0, "pack version %d: not read here", ICON_PACK_VERSION + 1);
    CHECK(icon_pack_file_kind(p, &ver) == ICON_ARCHIVE_PACK_OTHER && ver == ICON_PACK_VERSION + 1,
          "pack version %d: another pack version (%d)", ICON_PACK_VERSION + 1, ver);
    char ap[600];
    struct stat ast;
    ver = -2;
    CHECK(icon_pack_archive(ap, sizeof(ap), &ast, &ver) == ICON_ARCHIVE_PACK_OTHER && strcmp(ap, p) == 0 &&
              ver == ICON_PACK_VERSION + 1 && ast.st_size > 0,
          "icon_pack_archive names the file icon_pack reads (%s)", ap);
    CHECK(log_count(vmsg) == vbefore, "pack of another version: icon_pack logs nothing");

    /* a gzip file of another tool whose first subfield happens to be 'HI' (XLEN not 24): plain */
    install("hi_other.tar.gz");
    CHECK(icon_pack_file_kind(p, &ver) == ICON_ARCHIVE_PLAIN && ver == -1, "'HI' subfield, other layout: plain");
    CHECK(icon_pack_archive(ap, sizeof(ap), NULL, NULL) == ICON_ARCHIVE_PLAIN, "and for icon_pack_archive");
    /* icon_pack looks at this file (other size) before the next swap: a.tar.gz has the size of
     * vnext.tar.gz and may get its freed inode in the same second, which its stat cache would
     * take for the file it already saw */
    CHECK(icon_pack_available() == 0, "'HI' subfield, other layout: no pack index");

    install("a.tar.gz");
    CHECK(icon_pack_available() && read_all(0), "set a once more");

    if (s_failed) {
        fprintf(stderr, "[-] test_icon_pack_verify: %d check(s) failed\n", s_failed);
        return 1;
    }
    fprintf(stderr, "[+] test_icon_pack_verify: all checks passed\n");
    return 0;
}
