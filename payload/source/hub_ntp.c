#include "hub_ntp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
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

static time_t sntp_query_single(const char *host, int apply) {
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
    close(sockfd);
    if (!valid) return 0;

    uint32_t txTm = ntohl(packet.txTm_s);
    if (txTm < NTP_TIMESTAMP_DELTA) return 0;

    time_t epoch = (time_t)(txTm - NTP_TIMESTAMP_DELTA);
    if (epoch < (time_t)MIN_VALID_EPOCH) return 0;

    if (apply) {
        struct timeval new_tv;
        new_tv.tv_sec = epoch;
        new_tv.tv_usec = (suseconds_t)((ntohl(packet.txTm_f) * 1000000ULL) >> 32);
        if (settimeofday(&new_tv, NULL) == 0) {
            printf("[+] SNTP: Synchronized system clock to %lu from %s\n", (unsigned long)epoch, host);
        }
    }

    return epoch;
}

time_t sntp_sync_time(const char *server, int apply) {
    if (server && server[0]) {
        time_t t = sntp_query_single(server, apply);
        if (t > 0) return t;
    }

    static const char *fallback_pools[] = {
        "pool.ntp.org",
        "0.pool.ntp.org",
        "1.pool.ntp.org",
        "time.cloudflare.com",
        "time.google.com",
        NULL
    };

    for (int i = 0; fallback_pools[i]; i++) {
        if (server && strcmp(server, fallback_pools[i]) == 0) continue;
        time_t t = sntp_query_single(fallback_pools[i], apply);
        if (t > 0) return t;
    }

    return 0;
}

static time_t g_last_sync_attempt = 0;
static int g_time_valid = 0;

int sntp_sync_due(void) {
    time_t now = time(NULL);

    /* Check if system time currently appears sane (past 2025-01-01) */
    if (now >= (time_t)MIN_VALID_EPOCH) {
        g_time_valid = 1;
    } else {
        g_time_valid = 0;
    }

    time_t interval = g_time_valid ? RESYNC_INTERVAL_SEC : RETRY_INTERVAL_SEC;

    if (g_last_sync_attempt == 0 || (now - g_last_sync_attempt) >= interval || now < g_last_sync_attempt) {
        g_last_sync_attempt = now;
        return 1;
    }
    return 0;
}

void sntp_sync_done(time_t res) {
    if (res > 0) {
        g_time_valid = 1;
        g_last_sync_attempt = res;
    }
}

void sntp_tick(const char *server) {
    if (sntp_sync_due()) sntp_sync_done(sntp_sync_time(server, 1));
}
