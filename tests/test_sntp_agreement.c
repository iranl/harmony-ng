/*
 * hub_ntp.c steps the clock by more than an hour only when two servers agree.
 *
 * Fake SNTP servers answer on 127.0.0.11-15 (ports 9261-9265, any free port when one is busy),
 * each with its own time; the
 * server names hub_ntp.c asks are resolved to them (getaddrinfo is replaced below; an unmapped
 * name fails at once). The Hub's clock is a fake too: gettimeofday() and settimeofday() are
 * replaced, so "the clock is not set" (1970) and the steps can be checked without root.
 * The file that marks a time from one server is a temporary file (SNTP_UNCONFIRMED_FILE).
 */
#define _GNU_SOURCE
#include "hub_ntp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NTP_DELTA 2208988800ULL
#define NSERVERS 5
#define YEAR (365LL * 86400)

static int g_failures;
#define CHECK(c, msg) do { if (c) printf("  PASS %s\n", msg); else { printf("  FAIL %s (line %d)\n", msg, __LINE__); g_failures++; } } while (0)

/* ---- fake Hub clock: real time + g_clock_shift_us ---- */
static int64_t g_clock_shift_us;
static int g_set_calls;

static int64_t real_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int gettimeofday(struct timeval *restrict tv, void *restrict tz) {
    (void)tz;
    int64_t t = real_us() + g_clock_shift_us;
    tv->tv_sec = (time_t)(t / 1000000);
    tv->tv_usec = (suseconds_t)(t % 1000000);
    return 0;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz) {
    (void)tz;
    g_clock_shift_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec - real_us();
    g_set_calls++;
    return 0;
}

static int64_t hub_now(void) { return (real_us() + g_clock_shift_us) / 1000000; }

/* access() on the mark file is counted (the rest goes to the kernel as usual) */
static long g_mark_access;
int access(const char *path, int mode) {
    if (strcmp(path, SNTP_UNCONFIRMED_FILE) == 0) g_mark_access++;
    return (int)syscall(SYS_faccessat, AT_FDCWD, path, mode, 0);
}

/* ---- fake servers ---- */
static int g_fd[NSERVERS];
static int g_port[NSERVERS];
static volatile int g_answers[NSERVERS];      /* 1 = answers, 0 = silent */
static volatile int64_t g_server_shift[NSERVERS]; /* seconds away from the real time */
static volatile int g_stop;

/* name -> server index; -1 = name does not resolve */
static const char *g_names[] = { "ntp.test", "pool.ntp.org", "0.pool.ntp.org", "1.pool.ntp.org",
                                 "time.cloudflare.com", "time.google.com" };
static int g_map[6];

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    (void)service; (void)hints;
    int idx = -1;
    for (int i = 0; i < 6; i++) {
        if (node && strcmp(node, g_names[i]) == 0) idx = g_map[i];
    }
    if (idx < 0) return EAI_NONAME;
    struct addrinfo *ai = calloc(1, sizeof(*ai));
    struct sockaddr_in *sa = calloc(1, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)g_port[idx]);
    sa->sin_addr.s_addr = htonl(0x7F00000BU + (uint32_t)idx); /* 127.0.0.11 + idx */
    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_DGRAM;
    ai->ai_addr = (struct sockaddr *)sa;
    ai->ai_addrlen = sizeof(*sa);
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    if (!res) return;
    free(res->ai_addr);
    free(res);
}

static void *servers(void *arg) {
    (void)arg;
    struct pollfd p[NSERVERS];
    for (int i = 0; i < NSERVERS; i++) {
        p[i].fd = g_fd[i];
        p[i].events = POLLIN;
    }
    while (!g_stop) {
        if (poll(p, NSERVERS, 50) <= 0) continue;
        for (int i = 0; i < NSERVERS; i++) {
            if (!(p[i].revents & POLLIN)) continue;
            uint8_t req[48], rep[48];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            if (recvfrom(g_fd[i], req, sizeof(req), 0, (struct sockaddr *)&from, &fl) != 48) continue;
            if (!g_answers[i]) continue;
            int64_t t = real_us() + g_server_shift[i] * 1000000;
            memset(rep, 0, sizeof(rep));
            rep[0] = (0 << 6) | (4 << 3) | 4; /* LI 0, version 4, server */
            rep[1] = 2;                        /* stratum 2 */
            memcpy(rep + 24, req + 40, 8);     /* origin = the request's transmit timestamp */
            uint32_t s = htonl((uint32_t)((uint64_t)(t / 1000000) + NTP_DELTA));
            uint32_t f = htonl((uint32_t)(((uint64_t)(t % 1000000) << 32) / 1000000));
            memcpy(rep + 40, &s, 4);
            memcpy(rep + 44, &f, 4);
            sendto(g_fd[i], rep, sizeof(rep), 0, (struct sockaddr *)&from, fl);
        }
    }
    return NULL;
}

static int open_server(int i) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)(9261 + i));
    sa.sin_addr.s_addr = htonl(0x7F00000BU + (uint32_t)i);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        sa.sin_port = 0; /* the test's port is busy: any free one */
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            perror("bind");
            exit(2);
        }
    }
    socklen_t l = sizeof(sa);
    getsockname(fd, (struct sockaddr *)&sa, &l);
    g_port[i] = ntohs(sa.sin_port);
    return fd;
}

/* One scenario: which names resolve to which server, each server's time, the Hub clock */
static void setup(const int map[6], const int64_t shift[NSERVERS], int64_t hub_shift_s) {
    for (int i = 0; i < 6; i++) g_map[i] = map[i];
    for (int i = 0; i < NSERVERS; i++) {
        g_server_shift[i] = shift[i];
        g_answers[i] = 1;
    }
    g_clock_shift_us = hub_shift_s * 1000000;
    g_set_calls = 0;
}

static int near(int64_t a, int64_t b) { return a - b <= 2 && b - a <= 2; }

int main(void) {
    for (int i = 0; i < NSERVERS; i++) g_fd[i] = open_server(i);
    unlink(SNTP_UNCONFIRMED_FILE);
    pthread_t th;
    pthread_create(&th, NULL, servers, NULL);

    const int64_t real = real_us() / 1000000;
    const int64_t unset = -(real - 86400); /* the Hub clock reads 1970-01-02 */
    const int ONLY0[6] = { 0, -1, -1, -1, -1, -1 };
    const int TWO[6] = { 0, 1, -1, -1, -1, -1 };
    const int THREE[6] = { 0, 1, 2, -1, -1, -1 };
    const int SAME[6] = { 0, 0, 0, -1, -1, -1 }; /* three names, one server */
    time_t t;

    printf("Clock set, one server:\n");
    setup(ONLY0, (int64_t[NSERVERS]){ 30, 0, 0, 0, 0 }, 0);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(near(t, real + 30) && near(hub_now(), real + 30), "a 30 s correction is applied");
    setup(ONLY0, (int64_t[NSERVERS]){ 2 * YEAR, 0, 0, 0, 0 }, 0);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(t == 0 && g_set_calls == 0, "a 2-year step from one server is not applied");
    CHECK(sntp_sync_due() == 1, "a round is due right away the first time");
    CHECK(sntp_sync_due() == 0, "and not again at once");
    g_clock_shift_us += 299LL * 1000000;
    CHECK(sntp_sync_due() == 0, "not after 299 s");
    g_clock_shift_us += 2LL * 1000000;
    CHECK(sntp_sync_due() == 1, "after a refused step the next round is due in 5 minutes, not 4 hours");
    setup(ONLY0, (int64_t[NSERVERS]){ -2 * 3600, 0, 0, 0, 0 }, 0);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(t == 0 && g_set_calls == 0, "a 2-hour step back from one server is not applied");

    printf("Clock set, two or three servers:\n");
    setup(TWO, (int64_t[NSERVERS]){ 2 * YEAR, 2 * YEAR + 1, 0, 0, 0 }, 0);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(near(t, real + 2 * YEAR) && near(hub_now(), real + 2 * YEAR), "a 2-year step two servers agree on is applied");
    setup(TWO, (int64_t[NSERVERS]){ 2 * YEAR, 2 * YEAR + 60, 0, 0, 0 }, 0);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(t == 0 && g_set_calls == 0, "two servers 60 s apart do not agree");
    setup(THREE, (int64_t[NSERVERS]){ 2 * YEAR, 10, 11, 0, 0 }, 0);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(near(t, real + 10) && near(hub_now(), real + 10), "one server 2 years off, the next one's 10 s correction is used");
    setup(SAME, (int64_t[NSERVERS]){ 2 * YEAR, 0, 0, 0, 0 }, 0);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(t == 0 && g_set_calls == 0, "one server under three names is not a second opinion");
    setup(TWO, (int64_t[NSERVERS]){ 2 * YEAR, 2 * YEAR, 0, 0, 0 }, 0);
    t = sntp_sync_time("ntp.test", 0);
    CHECK(near(t, real + 2 * YEAR) && g_set_calls == 0, "apply = 0 only reports the time");

    printf("Clock not set:\n");
    setup(TWO, (int64_t[NSERVERS]){ 0, 1, 0, 0, 0 }, unset);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(near(t, real) && near(hub_now(), real), "two servers that agree set the clock");
    CHECK(access(SNTP_UNCONFIRMED_FILE, F_OK) != 0, "and the time counts as checked");

    setup(ONLY0, (int64_t[NSERVERS]){ 0, 0, 0, 0, 0 }, unset);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(near(t, real) && near(hub_now(), real), "one server only: its time is used for now");
    CHECK(access(SNTP_UNCONFIRMED_FILE, F_OK) == 0, "marked as not checked yet");
    (void)sntp_sync_due();
    g_clock_shift_us += 301LL * 1000000;
    CHECK(sntp_sync_due() == 1, "so the next round comes after 5 minutes");

    /* that time is checked: one server alone (even with a small correction) does not clear it */
    setup(ONLY0, (int64_t[NSERVERS]){ 3, 0, 0, 0, 0 }, 0);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(near(t, real + 3), "a small correction is still applied");
    CHECK(access(SNTP_UNCONFIRMED_FILE, F_OK) == 0, "but one server does not count as the check");
    setup(TWO, (int64_t[NSERVERS]){ 0, 1, 0, 0, 0 }, 0);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(t != 0 && access(SNTP_UNCONFIRMED_FILE, F_OK) != 0, "two servers that agree clear the mark");

    /* a wrong first time is corrected once two servers agree */
    setup(ONLY0, (int64_t[NSERVERS]){ 4 * YEAR, 0, 0, 0, 0 }, unset);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(near(hub_now(), real + 4 * YEAR), "one server 4 years ahead, clock not set: used for now");
    const int OTHERS[6] = { 0, 1, 2, -1, -1, -1 };
    setup(OTHERS, (int64_t[NSERVERS]){ 4 * YEAR, 0, 0, 0, 0 }, 4 * YEAR);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(near(hub_now(), real), "two other servers that agree bring it back");

    setup(ONLY0, (int64_t[NSERVERS]){ 8 * YEAR, 0, 0, 0, 0 }, unset);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(t == 0 && g_set_calls == 0, "one server 8 years ahead is not used even with the clock not set");
    setup(TWO, (int64_t[NSERVERS]){ 8 * YEAR, 8 * YEAR, 0, 0, 0 }, unset);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(near(hub_now(), real + 8 * YEAR), "two servers that agree have no upper bound");
    setup(ONLY0, (int64_t[NSERVERS]){ -200 * 86400LL, 0, 0, 0, 0 }, unset);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(t == 0 && g_set_calls == 0, "a time before this build is not used");

    printf("Checking a time that one server gave:\n");
    /* one server set the clock 30 min ahead (clock not set before); the next round asks it first,
     * then two others that agree on the real time */
    {
        int fd = open(SNTP_UNCONFIRMED_FILE, O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) close(fd);
    }
    setup(THREE, (int64_t[NSERVERS]){ 1800, 0, 0, 0, 0 }, 1800);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(near(hub_now(), real), "the two that agree set the clock back, although the first agreed with it");
    CHECK(access(SNTP_UNCONFIRMED_FILE, F_OK) != 0, "and only then the time counts as checked");

    printf("Rounds and small corrections:\n");
    setup(ONLY0, (int64_t[NSERVERS]){ 0, 0, 0, 0, 0 }, 0);
    t = sntp_sync_time("ntp.test", 1);
    CHECK(near(t, real) && g_set_calls == 0, "a correction under a second does not set the clock");
    setup(ONLY0, (int64_t[NSERVERS]){ 2 * YEAR, 0, 0, 0, 0 }, 0);
    for (int i = 0; i < 12; i++) (void)sntp_sync_time("ntp.test", 1);
    g_clock_shift_us += 4LL * 3600 * 1000000; /* past the 4 h round: one is due now */
    CHECK(sntp_sync_due() == 1, "a round is due after 4 hours");
    g_clock_shift_us += 301LL * 1000000;
    CHECK(sntp_sync_due() == 0, "after an hour of refused steps the next round is not after 5 minutes");
    g_clock_shift_us += 3300LL * 1000000;
    CHECK(sntp_sync_due() == 1, "but after an hour");

    /* a checked clock between two 4 h rounds, the main loop asking 20 times a second (here once a
     * second of fake time): the mark file is looked at only now and then */
    setup(TWO, (int64_t[NSERVERS]){ 0, 1, 0, 0, 0 }, 0);
    (void)sntp_sync_time("ntp.test", 1);
    g_clock_shift_us += 5LL * 3600 * 1000000;
    (void)sntp_sync_due(); /* the 4 h round */
    g_mark_access = 0;
    long passes = 0;
    for (int s = 0; s < 4 * 3600 - 10; s++) {
        g_clock_shift_us += 1000000;
        for (int k = 0; k < 20; k++, passes++) (void)sntp_sync_due();
    }
    printf("  (%ld passes, %ld looks at the mark file)\n", passes, g_mark_access);
    CHECK(g_mark_access <= 4 * 3600 / 300 + 2, "the mark file is looked at once per 5 minutes at most");

    /* a recheck round that moves the clock back by a minute: the next recheck is still due 5
     * minutes after the round (not 5 minutes after the look at the mark file before it) */
    {
        int fd = open(SNTP_UNCONFIRMED_FILE, O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) close(fd);
    }
    setup(TWO, (int64_t[NSERVERS]){ 0, 1, 0, 0, 0 }, 0);
    (void)sntp_sync_time("ntp.test", 1); /* two agree: rounds counted from 0 again */
    {
        int fd = open(SNTP_UNCONFIRMED_FILE, O_WRONLY | O_CREAT, 0644); /* hub_sntp set the mark */
        if (fd >= 0) close(fd);
    }
    sntp_sync_done((time_t)hub_now());
    g_clock_shift_us += 301LL * 1000000;
    CHECK(sntp_sync_due() == 1, "a recheck is due while the mark is there");
    g_clock_shift_us -= 60LL * 1000000; /* what the round's correction did */
    sntp_sync_done((time_t)hub_now());
    g_clock_shift_us += 301LL * 1000000;
    CHECK(sntp_sync_due() == 1, "after a correction back, the next recheck comes 5 minutes after the round");
    unlink(SNTP_UNCONFIRMED_FILE);

    g_stop = 1;
    pthread_join(th, NULL);
    for (int i = 0; i < NSERVERS; i++) close(g_fd[i]);
    unlink(SNTP_UNCONFIRMED_FILE);
    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All SNTP agreement checks passed\n");
    return 0;
}
