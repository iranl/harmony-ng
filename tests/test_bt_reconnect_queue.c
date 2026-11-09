/*
 * hub_btstack's reconnect queue and connected-hosts file (bt_reconnect_queue.c).
 * Build: gcc -Wall -Wextra -Ipayload/source/hub_btstack tests/test_bt_reconnect_queue.c \
 *            payload/source/hub_btstack/bt_reconnect_queue.c
 */
#include "bt_reconnect_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int g_fail;
#define CHECK(c, m) do { if (!(c)) { g_fail++; printf("  FAIL %s (line %d)\n", m, __LINE__); } else printf("  PASS %s\n", m); } while (0)

static const uint8_t A[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x01};
static const uint8_t B[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x02};
static const uint8_t C[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x03};
static const uint8_t Z[6] = {0};

static const uint8_t *g_skip_addr;
static bool skip_one(const uint8_t a[6]) { return g_skip_addr && memcmp(a, g_skip_addr, 6) == 0; }

int main(void) {
    uint8_t out[6];

    rq_clear();
    CHECK(!rq_push(Z), "a null address is not queued");
    CHECK(rq_push(A) && rq_push(B) && rq_count() == 2, "two hosts queued");
    CHECK(!rq_push(A) && rq_count() == 2, "a host already queued is not added twice");
    CHECK(rq_pop(out, NULL) && memcmp(out, A, 6) == 0, "first in, first out");
    CHECK(rq_pop(out, NULL) && memcmp(out, B, 6) == 0 && rq_count() == 0, "then the second, queue empty");
    CHECK(!rq_pop(out, NULL), "nothing to pop from an empty queue");

    rq_push(A); rq_push(B); rq_push(C);
    g_skip_addr = A;
    CHECK(rq_pop(out, skip_one) && memcmp(out, B, 6) == 0 && rq_count() == 1,
          "a skipped host (connected or disconnected on request) is dropped, the next one is returned");
    g_skip_addr = NULL;
    CHECK(rq_remove(C) && rq_count() == 0 && !rq_remove(C), "remove a queued host (it connected by itself)");

    rq_clear();
    uint8_t d[6] = {0x10};
    int pushed = 0;
    for (int i = 0; i < RQ_MAX + 2; i++) { d[5] = (uint8_t)i + 1; pushed += rq_push(d); }
    CHECK(pushed == RQ_MAX && rq_count() == RQ_MAX, "the queue holds at most RQ_MAX hosts");
    rq_clear();

    char dir[] = "/tmp/btrqXXXXXX";
    CHECK(mkdtemp(dir) != NULL, "temp dir");
    char path[128];
    snprintf(path, sizeof(path), "%s/hosts", dir);
    uint8_t rd[4][6];
    CHECK(bt_hosts_file_read(path, rd, 4) == 0, "no file: no hosts");
    const uint8_t two[2][6] = {{0x48, 0xB0, 0x2D, 0x00, 0x00, 0x01}, {0x00, 0xC3, 0xF4, 0x00, 0x00, 0x02}};
    CHECK(bt_hosts_file_write(path, two, 2) == 0, "write two hosts");
    memset(rd, 0, sizeof(rd));
    CHECK(bt_hosts_file_read(path, rd, 4) == 2 && memcmp(rd[0], two[0], 6) == 0 && memcmp(rd[1], two[1], 6) == 0,
          "read back the same two hosts, in order");
    CHECK(bt_hosts_file_write(path, two, 0) == 0 && bt_hosts_file_read(path, rd, 4) == 0, "an empty list (all disconnected)");
    FILE *f = fopen(path, "w");
    fputs("garbage\n00:00:00:00:00:00\n00:11:22:33:44:01\n00:11:22:33:44:01\n", f);
    fclose(f);
    CHECK(bt_hosts_file_read(path, rd, 4) == 1 && memcmp(rd[0], A, 6) == 0, "bad lines, null and duplicate addresses ignored");
    unlink(path);
    /* A failed rename leaves no temporary file behind: the target is a directory */
    char sub[160], tmpf[176];
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    snprintf(tmpf, sizeof(tmpf), "%s.new", sub);
    CHECK(mkdir(sub, 0700) == 0 && bt_hosts_file_write(sub, two, 2) == -1 && access(tmpf, F_OK) != 0,
          "write fails on a directory and removes its temporary file");
    rmdir(sub);
    rmdir(dir);

    printf("%s\n", g_fail ? "bt_reconnect_queue: FAILED" : "bt_reconnect_queue: all passed");
    return g_fail ? 1 : 0;
}
