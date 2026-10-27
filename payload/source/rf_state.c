#define _GNU_SOURCE
#include "rf_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

volatile bool g_running = true;
int g_rf_fd = -1;
pthread_mutex_t g_rf_mutex = PTHREAD_MUTEX_INITIALIZER;
uint8_t g_paired_addr[4] = {0};
uint8_t g_hub_rf_addr[4] = {0};
uint8_t g_remote_slot = 0x01;
bool g_has_paired_device = false;
uint8_t g_fw_version = 0;
volatile bool g_pairing_mode = false;
uint8_t g_config_version = 1;
int g_log_level = LOG_LEVEL_INFO;
int g_debug_logging = 0;

remote_battery_t g_remote_batteries[MAX_PAIRED_REMOTES] = {
    { .percent = 0, .charging = 0, .voltage_mv = 0, .last_update_ms = 0 },
    { .percent = 0, .charging = 0, .voltage_mv = 0, .last_update_ms = 0 }
};

remote_version_t g_remote_versions[MAX_PAIRED_REMOTES] = {
    { .sw_version = "unknown", .config_version = 0, .last_report_ms = 0 },
    { .sw_version = "unknown", .config_version = 0, .last_report_ms = 0 }
};

paired_remote_t g_paired_remotes[MAX_PAIRED_REMOTES] = {
    { .slot = 0x01, .active = false, .link_lost = false, .last_seen_ms = 0 },
    { .slot = 0x02, .active = false, .link_lost = false, .last_seen_ms = 0 }
};

rf_ota_state_t g_rf_ota = { false, 0, 0, 0, 0, 0, 0, "" };
pthread_mutex_t g_ota_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void mark_remote_slot_active(uint8_t slot) {
    if (slot == 0x01 || slot == 0x02) {
        int idx = slot - 1;
        g_paired_remotes[idx].active = true;
        g_paired_remotes[idx].link_lost = false;
        g_paired_remotes[idx].last_seen_ms = get_monotonic_ms();
        g_remote_slot = slot;
        g_has_paired_device = true;
    }
}

void http_post_local(int port, const char *endpoint, const char *json_body) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    /* Non-blocking connect with 150ms timeout */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rc = connect(sock, (struct sockaddr *)&sin, sizeof(sin));
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval ctv = { .tv_sec = 0, .tv_usec = 150000 };
        if (select(sock + 1, NULL, &wfds, NULL, &ctv) <= 0) {
            close(sock);
            return;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err) { close(sock); return; }
    } else if (rc < 0) {
        close(sock);
        return;
    }

    /* Restore blocking mode with timeouts */
    fcntl(sock, F_SETFL, flags);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[1024];
    size_t blen = json_body ? strlen(json_body) : 0;
    int req_len = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        endpoint, port, blen, json_body ? json_body : "");
    write(sock, req, req_len);
    char resp[256];
    read(sock, resp, sizeof(resp));
    close(sock);
}
