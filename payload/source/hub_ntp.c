#include "hub_ntp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define NTP_TIMESTAMP_DELTA 2208988800ULL
#define NTP_PORT 123
#define MIN_VALID_EPOCH 1735689600UL /* 2025-01-01 00:00:00 UTC */
#define RESYNC_INTERVAL_SEC (4 * 3600) /* 4 hours */
#define RETRY_INTERVAL_SEC 20          /* 20 seconds */
#define CONFIRM_RETRY_SEC 300          /* next round while a time waits for a second server */
#define CONFIRM_RETRY_ROUNDS 12        /* ... for the first hour; then once an hour */
#define CONFIRM_RETRY_SLOW_SEC 3600

/* A clock step larger than this is applied only when two servers (two different addresses)
 * agree on the time within AGREE_SEC. Smaller corrections from one server are applied at once. */
#define STEP_CONFIRM_SEC 3600
#define AGREE_SEC 5
/* While the clock is not set yet and no two servers agree, the time of one server is used for
 * now (so the Hub is not left without a clock) only if it is not later than the build date plus
 * this many years; it is checked again at the next round. */
#define UNCONFIRMED_MAX_YEARS 5
#define SNTP_MAX_SERVERS 8
/* Present while the clock holds a time that one server alone gave (RAM only, gone at reboot
 * with the clock itself): shared by hub_sntp at boot and hub_daemon's rounds. */
#ifndef SNTP_UNCONFIRMED_FILE
#define SNTP_UNCONFIRMED_FILE "/tmp/hub_sntp_unconfirmed"
#endif

struct ntp_packet {
    uint8_t li_vn_mode;
    uint8_t stratum;
    uint8_t poll;
    uint8_t precision;
    uint32_t rootDelay;
    uint32_t rootDispersion;
    uint32_t refId;
    uint32_t refTm_s;
    uint32_t refTm_f;
    uint32_t origTm_s;
    uint32_t origTm_f;
    uint32_t rxTm_s;
    uint32_t rxTm_f;
    uint32_t txTm_s;
    uint32_t txTm_f;
};

/* Request transmit timestamp: random, so a reply must echo it (RFC 4330 section 5) and a
 * forged one cannot be guessed */
static void sntp_request_nonce(uint32_t *s, uint32_t *f) {
    uint32_t v[2] = { 0, 0 };
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, v, sizeof(v)) != (ssize_t)sizeof(v)) {
        struct timeval now;
        gettimeofday(&now, NULL);
        v[0] = (uint32_t)now.tv_sec ^ ((uint32_t)getpid() << 16);
        v[1] = (uint32_t)now.tv_usec ^ (uint32_t)(uintptr_t)&now;
    }
    if (fd >= 0) close(fd);
    *s = v[0];
    *f = v[1] ? v[1] : 1;
}

/* Days from 1970-01-01 to year-month-day (proleptic Gregorian) */
static int64_t sntp_days_from_civil(int year, int month, int day) {
    int y = year - (month <= 2);
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

/* Build date as a Unix time, 0 if unknown. build/compile_all.sh passes the UTC date
 * (HUB_BUILD_DATE_UTC=YYYYMMDD). Without it __DATE__ is used, which is the build host's local
 * date: one day is taken off, so a build east of UTC is never dated in the future. */
static int64_t sntp_build_epoch(void) {
    static int64_t cached;
    static volatile int cached_set;
    if (cached_set) return cached;
    int64_t epoch = 0;
#ifdef HUB_BUILD_DATE_UTC
    long v = (long)(HUB_BUILD_DATE_UTC);
    int year = (int)(v / 10000), month = (int)(v / 100 % 100), day = (int)(v % 100);
    if (year >= 2000 && month >= 1 && month <= 12 && day >= 1 && day <= 31)
        epoch = sntp_days_from_civil(year, month, day) * 86400;
#else
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    char mon[4] = { d[0], d[1], d[2], 0 };
    const char *m = strstr(months, mon);
    int day = atoi(d + 4), year = atoi(d + 7);
    if (m && day >= 1 && year >= 2000)
        epoch = (sntp_days_from_civil(year, (int)(m - months) / 3 + 1, day) - 1) * 86400;
#endif
    /* the same value on every call: a second thread computing it too is harmless */
    cached = epoch;
    cached_set = 1;
    return epoch;
}

/* Earliest time the clock can have: a reply before this is wrong, a clock before it is not set */
static int64_t sntp_valid_floor(void) {
    int64_t b = sntp_build_epoch();
    return b > (int64_t)MIN_VALID_EPOCH ? b : (int64_t)MIN_VALID_EPOCH;
}

static int64_t sntp_now_us(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (int64_t)now.tv_sec * 1000000 + now.tv_usec;
}

/* A clock that settimeofday() does not move: answers taken before and after a step compare */
static int64_t sntp_mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t sntp_abs64(int64_t v) {
    return v < 0 ? -v : v;
}

struct sntp_answer {
    const char *host;
    uint32_t addr;     /* server address (network order): two answers count only from two servers */
    int64_t epoch;     /* server time, whole seconds */
    int64_t offset_us; /* server time minus the local clock when the reply arrived */
    int64_t ref_us;    /* server time minus the monotonic clock then: what answers are compared by */
};

static int sntp_query_single(const char *host, struct sntp_answer *ans) {
    if (!host || !host[0]) return 0;

    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) return 0;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, "123", &hints, &res) != 0 || !res) {
        close(sockfd);
        return 0;
    }

    /* A connected socket only receives datagrams from the server it asked; an unconnected one
     * took a reply from any address and port. */
    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(sockfd);
        return 0;
    }
    uint32_t addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);

    struct ntp_packet packet;
    memset(&packet, 0, sizeof(packet));
    packet.li_vn_mode = (0 << 6) | (4 << 3) | 3; /* LI=0, VN=4, Mode=3 (client) */
    uint32_t nonce_s, nonce_f;
    sntp_request_nonce(&nonce_s, &nonce_f);
    packet.txTm_s = nonce_s;
    packet.txTm_f = nonce_f;

    if (send(sockfd, &packet, sizeof(packet), 0) < 0) {
        close(sockfd);
        return 0;
    }

    /* RFC 4330 section 5: the reply must echo our transmit timestamp as its origin, come from a
     * synchronized server (LI not 3, stratum 1 to 15) in server mode, and carry a transmit time.
     * Anything else is skipped; a few stray datagrams are read before giving up. */
    int valid = 0;
    for (int tries = 0; tries < 4 && !valid; tries++) {
        ssize_t n = recv(sockfd, &packet, sizeof(packet), 0);
        if (n < 0) break; /* timeout or error */
        if (n < (ssize_t)sizeof(packet)) continue;
        int li = packet.li_vn_mode >> 6, mode = packet.li_vn_mode & 0x07;
        if (packet.origTm_s != nonce_s || packet.origTm_f != nonce_f) continue;
        if (mode != 4 || li == 3 || packet.stratum == 0 || packet.stratum > 15) continue;
        valid = 1;
    }
    int64_t local_us = sntp_now_us();
    int64_t mono_us = sntp_mono_us();
    close(sockfd);
    if (!valid) return 0;

    uint32_t txTm = ntohl(packet.txTm_s);
    if (txTm < NTP_TIMESTAMP_DELTA) return 0;

    int64_t epoch = (int64_t)(txTm - NTP_TIMESTAMP_DELTA);
    if (epoch < sntp_valid_floor()) return 0; /* before this build: certainly wrong */

    int64_t server_us = epoch * 1000000 + (int64_t)((ntohl(packet.txTm_f) * 1000000ULL) >> 32);
    ans->host = host;
    ans->addr = addr;
    ans->epoch = epoch;
    ans->offset_us = server_us - local_us;
    ans->ref_us = server_us - mono_us;
    return 1;
}

/* The clock holds a time that one server alone gave while it was not set: the next rounds
 * look for a second server, and small corrections do not count as that check. */
static int sntp_unconfirmed(void) {
    return access(SNTP_UNCONFIRMED_FILE, F_OK) == 0;
}

static void sntp_set_unconfirmed(int on) {
    if (!on) {
        unlink(SNTP_UNCONFIRMED_FILE);
        return;
    }
    int fd = open(SNTP_UNCONFIRMED_FILE, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);
}

/* the last round saw a large step from one server only and did not apply it */
static int g_step_refused = 0;
/* rounds in a row that ended with the time not checked by two servers (or a step refused) */
static int g_confirm_rounds = 0;

static time_t sntp_apply(const struct sntp_answer *a, int apply) {
    int64_t t = sntp_mono_us() + a->ref_us;
    if (sizeof(time_t) < 8 && t / 1000000 > 0x7FFFFFFFLL) return 0; /* past 2038 on a 32-bit time_t */
    if (apply) {
        struct timeval new_tv;
        new_tv.tv_sec = (time_t)(t / 1000000);
        new_tv.tv_usec = (suseconds_t)(t % 1000000);
        if (settimeofday(&new_tv, NULL) != 0) return 0;
        printf("[+] SNTP: Synchronized system clock to %lu from %s\n", (unsigned long)new_tv.tv_sec, a->host);
    }
    return (time_t)(t / 1000000);
}

/* Asks the servers in order (the configured one first, then the fallback pools):
 * - while the clock is set, an answer within STEP_CONFIRM_SEC of it is applied at once;
 * - a larger step (or the first time while the clock is not set) needs two servers, at two
 *   different addresses, that agree within AGREE_SEC;
 * - when the clock is not set and no two servers agree, the first answer not later than the
 *   build date + UNCONFIRMED_MAX_YEARS is used for now and checked again in CONFIRM_RETRY_SEC.
 * After such a time the next rounds also look for a second server, until two agree. apply = 0
 * only returns the time that would be applied (and changes no state). */
time_t sntp_sync_time(const char *server, int apply) {
    static const char *fallback_pools[] = {
        "pool.ntp.org",
        "0.pool.ntp.org",
        "1.pool.ntp.org",
        "time.cloudflare.com",
        "time.google.com",
        NULL
    };
    const char *names[SNTP_MAX_SERVERS];
    int nn = 0;
    if (server && server[0]) names[nn++] = server;
    for (int i = 0; fallback_pools[i] && nn < SNTP_MAX_SERVERS; i++) {
        if (server && strcmp(server, fallback_pools[i]) == 0) continue;
        names[nn++] = fallback_pools[i];
    }

    const int64_t limit_us = (int64_t)STEP_CONFIRM_SEC * 1000000;
    const int64_t agree_us = (int64_t)AGREE_SEC * 1000000;
    int clock_set = sntp_now_us() / 1000000 >= sntp_valid_floor();
    int unconfirmed = !clock_set || sntp_unconfirmed();
    struct sntp_answer ans[SNTP_MAX_SERVERS];
    int na = 0;
    time_t result = 0;

    for (int i = 0; i < nn; i++) {
        struct sntp_answer a;
        if (!sntp_query_single(names[i], &a)) continue;
        int seen = 0;
        for (int j = 0; j < na; j++) {
            if (ans[j].addr == a.addr) seen = 1;
        }
        if (seen) continue; /* the same server under another name is not a second opinion */

        if (clock_set && sntp_abs64(a.offset_us) <= limit_us && result == 0) {
            /* small correction: as before; under a second the clock is left alone */
            result = sntp_apply(&a, apply && sntp_abs64(a.offset_us) >= 1000000);
            if (!unconfirmed) {
                if (apply) {
                    g_step_refused = 0;
                    g_confirm_rounds = 0;
                }
                return result;
            }
        }
        for (int j = 0; j < na; j++) {
            if (sntp_abs64(a.ref_us - ans[j].ref_us) <= agree_us) {
                /* the clock is set to the time they agree on unless it is within AGREE_SEC of it
                 * already (also after a small correction from another server in this round) */
                int step = !clock_set || sntp_abs64(a.offset_us) > agree_us;
                if (step)
                    printf("[+] SNTP: %s and %s agree, stepping the clock by %lld s\n", ans[j].host, a.host,
                           (long long)(a.offset_us / 1000000));
                result = sntp_apply(&a, apply && step);
                if (result && apply) {
                    sntp_set_unconfirmed(0);
                    g_step_refused = 0;
                    g_confirm_rounds = 0;
                }
                return result;
            }
        }
        ans[na++] = a;
    }

    if (result) {
        if (apply) {
            g_step_refused = 0;
            g_confirm_rounds++;
        }
        return result; /* a small correction from one server; a later round looks for a second */
    }
    if (!clock_set) {
        int64_t max_epoch = sntp_build_epoch() + (int64_t)UNCONFIRMED_MAX_YEARS * 365 * 86400;
        for (int j = 0; j < na; j++) {
            if (ans[j].epoch > max_epoch) continue;
            printf("[!] SNTP: clock not set and no two servers agree: using %s for now, checking again\n", ans[j].host);
            result = sntp_apply(&ans[j], apply);
            if (result && apply) {
                sntp_set_unconfirmed(1);
                g_step_refused = 0;
                g_confirm_rounds++;
            }
            return result;
        }
    }
    for (int j = 0; j < na; j++) {
        printf("[!] SNTP: %s is %lld s away from the Hub clock: not applied until a second server agrees\n",
               ans[j].host, (long long)(ans[j].offset_us / 1000000));
    }
    if (na > 0 && apply) {
        g_step_refused = 1;
        g_confirm_rounds++;
    }
    return 0;
}

static time_t g_last_sync_attempt = 0;
static int g_time_valid = 0;
static time_t g_mark_checked = 0; /* when the "not checked yet" file was last looked at */

int sntp_sync_due(void) {
    time_t now = (time_t)(sntp_now_us() / 1000000);

    /* Check if system time currently appears sane (not before this build) */
    if ((int64_t)now >= sntp_valid_floor()) {
        g_time_valid = 1;
    } else {
        g_time_valid = 0;
    }

    int due;
    time_t elapsed = now - g_last_sync_attempt;
    if (g_last_sync_attempt == 0 || now < g_last_sync_attempt) {
        due = 1;
    } else if (!g_time_valid) {
        due = elapsed >= RETRY_INTERVAL_SEC;
    } else if (elapsed >= RESYNC_INTERVAL_SEC) {
        due = 1;
    } else {
        /* a time not checked by two servers yet, or a refused step: every 5 minutes for the first
         * hour, then once an hour (a Hub that reaches only one server stays in this state). The
         * file is looked at only when such a round could be due. */
        time_t confirm = g_confirm_rounds < CONFIRM_RETRY_ROUNDS ? CONFIRM_RETRY_SEC : CONFIRM_RETRY_SLOW_SEC;
        if (elapsed < confirm) {
            due = 0;
        } else if (g_step_refused) {
            due = 1;
        } else if (g_mark_checked == 0 || now - g_mark_checked >= CONFIRM_RETRY_SEC || now < g_mark_checked) {
            /* hub_sntp may have set the mark: looked at once per CONFIRM_RETRY_SEC at most */
            g_mark_checked = now;
            due = sntp_unconfirmed();
        } else {
            due = 0;
        }
    }
    if (due) g_last_sync_attempt = now;
    return due;
}

void sntp_sync_done(time_t res) {
    /* the round may have moved the clock (also back): the next look at the mark file is timed
     * from the round, as the next round is */
    g_mark_checked = 0;
    if (res > 0) {
        g_time_valid = 1;
        g_last_sync_attempt = res;
    }
}

void sntp_tick(const char *server) {
    if (sntp_sync_due()) sntp_sync_done(sntp_sync_time(server, 1));
}
