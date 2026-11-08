/*
 * While a stalled receive session is being closed in the background, an IR send must not touch
 * /dev/i2s (the driver may hang in that close): ir_i2s_blast_words() returns -EBUSY at once.
 * Runs with the mock library: <mock>/i2s_rx gives the receiver data, <mock>/i2s_rx_stall
 * stalls its reads and its close.
 */
#include "ir_i2s.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static int g_fail = 0;

static void check(const char *name, int ok, const char *detail) {
    printf("%s %s (%s)\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) g_fail++;
}

static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static void write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (len) fwrite(data, 1, len, f);
    fclose(f);
}

int main(void) {
    const char *md = getenv("HARMONY_MOCK_DIR");
    if (!md || !md[0]) {
        fprintf(stderr, "HARMONY_MOCK_DIR not set\n");
        return 1;
    }
    char rx[512], stall[512], detail[128];
    snprintf(rx, sizeof(rx), "%s/i2s_rx", md);
    snprintf(stall, sizeof(stall), "%s/i2s_rx_stall", md);
    static uint8_t lit[4096];
    memset(lit, 0xff, sizeof(lit));
    write_file(rx, lit, sizeof(lit));
    write_file(stall, NULL, 0);

    const uint16_t words[] = {0x8000 | 560, 560, 0x8000 | 560, 1690};
    char raw[2048];
    long t0 = now_ms();
    int rc = ir_i2s_capture(raw, sizeof(raw), 8);
    snprintf(detail, sizeof(detail), "rc %d after %ld ms", rc, now_ms() - t0);
    check("a stalled receiver ends the capture with an error", rc < 0, detail);

    t0 = now_ms();
    rc = ir_i2s_blast_words(38000, 33, words, 4, 7);
    long took = now_ms() - t0;
    snprintf(detail, sizeof(detail), "rc %d after %ld ms", rc, took);
    check("a send while the stalled close is pending: -EBUSY at once", rc == -EBUSY && took < 500, detail);

    unlink(stall);
    usleep(600 * 1000); /* the closer thread returns once the stall ends */
    rc = ir_i2s_blast_words(38000, 33, words, 4, 7);
    snprintf(detail, sizeof(detail), "rc %d", rc);
    /* the devbox has no /dev/i2s for sending, so the send fails further on, but not with EBUSY */
    check("a send after the close returned gets past the guard", rc != -EBUSY, detail);

    unlink(rx);
    printf("%s\n", g_fail ? "IR send during a stalled close: FAILED" : "IR send during a stalled close: all passed");
    return g_fail ? 1 : 0;
}
