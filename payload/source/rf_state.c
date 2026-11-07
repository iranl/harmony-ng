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

remote_link_stats_t g_remote_link[MAX_PAIRED_REMOTES];

paired_remote_t g_paired_remotes[MAX_PAIRED_REMOTES] = {
    { .slot = 0x01, .active = false, .link_lost = false, .last_seen_ms = 0 },
    { .slot = 0x02, .active = false, .link_lost = false, .last_seen_ms = 0 }
};

rf_ota_state_t g_rf_ota = { false, 0, 0, 0, 0, 0, 0, "" };
pthread_mutex_t g_ota_lock = PTHREAD_MUTEX_INITIALIZER;

/* Rename <old> to <new> unless <new> exists; 1 when it moved */
static int rf_migrate_one(const char *old_path, const char *new_path) {
    if (access(old_path, F_OK) != 0 || access(new_path, F_OK) == 0) return 0;
    if (rename(old_path, new_path) != 0) return 0;
    printf("[*] Migrated %s -> %s\n", old_path, new_path);
    return 1;
}

int rf_migrate_legacy_names_in(const char *harmony_dir, const char *cache_dir) {
    char o[160], n[160];
    int moved = 0;
    for (int slot = 1; slot <= MAX_PAIRED_REMOTES; slot++) {
        static const char *const kinds[] = { "version", "config", NULL };
        for (int k = 0; kinds[k]; k++) {
            snprintf(o, sizeof(o), "%s/elite_slot%d_%s.json", harmony_dir, slot, kinds[k]);
            snprintf(n, sizeof(n), "%s/remote_slot%d_%s.json", harmony_dir, slot, kinds[k]);
            moved += rf_migrate_one(o, n);
        }
    }
    static const char *const staged[] = { "elite_ui.ota.gz", "elite_ui.version", NULL };
    for (int k = 0; staged[k]; k++) {
        snprintf(o, sizeof(o), "%s/%s", harmony_dir, staged[k]);
        snprintf(n, sizeof(n), "%s/remote_%s", harmony_dir, staged[k] + strlen("elite_"));
        moved += rf_migrate_one(o, n);
    }
    if (cache_dir) {
        snprintf(o, sizeof(o), "%s/elite_ui.ota.gz", cache_dir);
        snprintf(n, sizeof(n), "%s/remote_ui.ota.gz", cache_dir);
        moved += rf_migrate_one(o, n);
    }
    if (moved) sync();
    return moved;
}

void rf_migrate_legacy_names(void) {
    rf_migrate_legacy_names_in("/data/harmony", "/cache");
}

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

bool is_companion_slot(uint8_t slot) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return false;
    int idx = slot - 1;
    if (g_paired_remotes[idx].model == REMOTE_MODEL_COMPANION ||
        g_paired_remotes[idx].model == REMOTE_MODEL_SMART_CONTROL) return true;
    if (g_paired_remotes[idx].equad_id == 0x401E ||
        strstr(g_paired_remotes[idx].name, "20+") != NULL ||
        strstr(g_paired_remotes[idx].name, "Smart") != NULL) {
        g_paired_remotes[idx].model = REMOTE_MODEL_SMART_CONTROL;
        return true;
    }
    if (g_paired_remotes[idx].equad_id == 0x4026 ||
        g_paired_remotes[idx].equad_id == 0x4027 ||
        strstr(g_paired_remotes[idx].name, "Companion") != NULL ||
        strstr(g_paired_remotes[idx].name, "Home") != NULL) {
        g_paired_remotes[idx].model = REMOTE_MODEL_COMPANION;
        return true;
    }
    return false;
}

const char *get_remote_model_name(remote_model_t model) {
    switch (model) {
        case REMOTE_MODEL_SMART_CONTROL: return "smart_control";
        case REMOTE_MODEL_COMPANION: return "companion";
        case REMOTE_MODEL_ELITE: return "elite";
        default: return "unknown";
    }
}

int http_request_local(int port, const char *method, const char *endpoint, const char *json_body,
                       char *resp, size_t resp_len, int timeout_ms) {
    if (resp && resp_len) resp[0] = '\0';
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

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
            return -1;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err) { close(sock); return -1; }
    } else if (rc < 0) {
        close(sock);
        return -1;
    }

    /* Restore blocking mode with timeouts */
    fcntl(sock, F_SETFL, flags);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[1024];
    size_t blen = json_body ? strlen(json_body) : 0;
    int req_len = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        method, endpoint, port, blen, json_body ? json_body : "");
    write(sock, req, req_len);
    shutdown(sock, SHUT_WR);

    /* Keep the headers and as much of the body as fits; drain the rest */
    char buf[4096], drain[512];
    size_t got = 0;
    for (;;) {
        ssize_t r = got < sizeof(buf) - 1 ? read(sock, buf + got, sizeof(buf) - 1 - got) : read(sock, drain, sizeof(drain));
        if (r <= 0) break;
        if (got < sizeof(buf) - 1) got += (size_t)r;
    }
    close(sock);
    buf[got] = '\0';

    int status = -1;
    if (sscanf(buf, "HTTP/%*s %d", &status) != 1) return -1;
    const char *body = strstr(buf, "\r\n\r\n");
    if (resp && resp_len && body) snprintf(resp, resp_len, "%s", body + 4);
    return status;
}

void http_post_local(int port, const char *endpoint, const char *json_body) {
    http_request_local(port, "POST", endpoint, json_body, NULL, 0, 400);
}
