/*
 * hub_ntp.c accepts only the reply to its own request (RFC 4330 section 5). The test resolves
 * the server name to a local UDP socket (getaddrinfo is replaced below) and answers from a
 * thread:
 * - a well-formed reply from the server's address, echoing the request: accepted
 * - a reply from another port of the same host (spoofed or stray): ignored
 * - a reply that does not echo the request's transmit timestamp: ignored
 * - a reply in the wrong mode or from an unsynchronized server (stratum 0): ignored
 * Each ignored reply costs the 2 s receive timeout. Nothing is applied (apply = 0).
 */
#define _GNU_SOURCE
#include "hub_ntp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NTP_DELTA 2208988800UL
#define T_2030 1893456000UL /* 2030-01-01, the time every reply carries */

static int g_failures;
#define CHECK(c, msg) do { if (c) printf("  PASS %s\n", msg); else { printf("  FAIL %s (line %d)\n", msg, __LINE__); g_failures++; } } while (0)

static int g_server_port;

/* "ntp.test" resolves to 127.0.0.1:<server port>; every other name fails (no fallback pools) */
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    (void)service; (void)hints;
    if (!node || strcmp(node, "ntp.test") != 0) return EAI_NONAME;
    struct addrinfo *ai = calloc(1, sizeof(*ai));
    struct sockaddr_in *sa = calloc(1, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)g_server_port);
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

enum { R_GOOD, R_OTHER_PORT, R_BAD_ORIGIN, R_CLIENT_MODE, R_STRATUM0 };

static int g_server_fd, g_other_fd, g_case;

static void *server(void *arg) {
    (void)arg;
    uint8_t req[48], rep[48];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    if (recvfrom(g_server_fd, req, sizeof(req), 0, (struct sockaddr *)&from, &fl) != 48) return NULL;
    memset(rep, 0, sizeof(rep));
    rep[0] = (0 << 6) | (4 << 3) | 4; /* LI 0, version 4, server */
    rep[1] = 2;                        /* stratum 2 */
    memcpy(rep + 24, req + 40, 8);     /* origin = the request's transmit timestamp */
    uint32_t tx = htonl((uint32_t)(T_2030 + NTP_DELTA));
    memcpy(rep + 40, &tx, 4);
    int fd = g_server_fd;
    switch (g_case) {
    case R_OTHER_PORT: fd = g_other_fd; break;
    case R_BAD_ORIGIN: rep[24] ^= 0x5A; break;
    case R_CLIENT_MODE: rep[0] = (4 << 3) | 3; break;
    case R_STRATUM0: rep[1] = 0; break;
    }
    sendto(fd, rep, sizeof(rep), 0, (struct sockaddr *)&from, fl);
    return NULL;
}

static int udp_socket(int *port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    socklen_t l = sizeof(sa);
    getsockname(fd, (struct sockaddr *)&sa, &l);
    if (port) *port = ntohs(sa.sin_port);
    return fd;
}

static time_t query(int c) {
    g_case = c;
    pthread_t th;
    pthread_create(&th, NULL, server, NULL);
    time_t t = sntp_sync_time("ntp.test", 0);
    pthread_join(th, NULL);
    return t;
}

int main(void) {
    g_server_fd = udp_socket(&g_server_port);
    g_other_fd = udp_socket(NULL);

    CHECK(query(R_GOOD) == (time_t)T_2030, "a reply from the server that echoes the request is accepted");
    CHECK(query(R_OTHER_PORT) == 0, "a reply from another port is ignored");
    CHECK(query(R_BAD_ORIGIN) == 0, "a reply that does not echo the request's timestamp is ignored");
    CHECK(query(R_CLIENT_MODE) == 0, "a reply that is not in server mode is ignored");
    CHECK(query(R_STRATUM0) == 0, "a reply with stratum 0 (unsynchronized) is ignored");
    CHECK(query(R_GOOD) == (time_t)T_2030, "and a good reply is accepted again");

    close(g_server_fd);
    close(g_other_fd);
    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All SNTP reply checks passed\n");
    return 0;
}
