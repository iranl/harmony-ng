#include "codex_ntp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
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

static time_t sntp_query_single(const char *host, int apply) {
    if (!host || !host[0]) return 0;

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(NTP_PORT);

    if (inet_aton(host, &serv_addr.sin_addr) == 0) {
        struct hostent *server = gethostbyname(host);
        if (!server || !server->h_addr_list || !server->h_addr_list[0]) return 0;
        memcpy(&serv_addr.sin_addr, server->h_addr_list[0], sizeof(serv_addr.sin_addr));
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) return 0;

    struct timeval timeout = { .tv_sec = 2, .tv_usec = 500000 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct ntp_packet packet;
    memset(&packet, 0, sizeof(packet));
    packet.li_vn_mode = 0x1B; /* VN=3, Client */

    if (sendto(sockfd, &packet, sizeof(packet), 0, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd);
        return 0;
    }

    socklen_t addr_len = sizeof(serv_addr);
    ssize_t n = recvfrom(sockfd, &packet, sizeof(packet), 0, (struct sockaddr *)&serv_addr, &addr_len);
    close(sockfd);

    if (n < (ssize_t)sizeof(packet)) return 0;

    uint32_t tx_sec = ntohl(packet.txTm_s);
    uint32_t tx_frac = ntohl(packet.txTm_f);

    if (tx_sec < NTP_TIMESTAMP_DELTA) return 0;

    time_t unix_time = (time_t)(tx_sec - NTP_TIMESTAMP_DELTA);
    if ((unsigned long)unix_time < MIN_VALID_EPOCH) return 0;

    uint32_t usec = (uint32_t)(((uint64_t)tx_frac * 1000000ULL) >> 32);

    if (apply) {
        struct timeval tv;
        tv.tv_sec = unix_time;
        tv.tv_usec = usec;
        if (settimeofday(&tv, NULL) == 0) {
            char tbuf[64];
            struct tm *g = gmtime(&unix_time);
            if (g) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S UTC", g);
            else snprintf(tbuf, sizeof(tbuf), "%lu", (unsigned long)unix_time);
            printf("[+] NTP synced clock to %s (via %s)\n", tbuf, host);
        }
    }

    return unix_time;
}

time_t sntp_sync_time(const char *server, int apply) {
    if (server && server[0]) {
        time_t t = sntp_query_single(server, apply);
        if (t > 0) return t;
    }

    /* Fallback public pools */
    const char *fallbacks[] = {
        "pool.ntp.org",
        "time.cloudflare.com",
        "time.google.com"
    };

    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        if (server && strcmp(server, fallbacks[i]) == 0) continue;
        time_t t = sntp_query_single(fallbacks[i], apply);
        if (t > 0) return t;
    }

    return 0;
}

void sntp_tick(const char *server) {
    static time_t last_check = 0;
    static time_t last_success = 0;

    time_t now = time(NULL);
    if (now - last_check < 5) return; /* Rate-limit check */

    int needs_init = (now < MIN_VALID_EPOCH);
    time_t interval = needs_init ? RETRY_INTERVAL_SEC : RESYNC_INTERVAL_SEC;

    if (last_success == 0 || (now - last_success >= interval) || (needs_init && (now - last_check >= RETRY_INTERVAL_SEC))) {
        last_check = now;
        time_t synced = sntp_sync_time(server, 1);
        if (synced > 0) {
            last_success = synced;
        }
    }
}
