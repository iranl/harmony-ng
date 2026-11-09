#include "bt_reconnect_queue.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint8_t s_q[RQ_MAX][6];
static int s_n;

static bool is_null(const uint8_t a[6]) {
    static const uint8_t z[6] = {0};
    return memcmp(a, z, 6) == 0;
}

static int find(const uint8_t a[6]) {
    for (int i = 0; i < s_n; i++) {
        if (memcmp(s_q[i], a, 6) == 0) return i;
    }
    return -1;
}

static void drop_at(int i) {
    memmove(&s_q[i], &s_q[i + 1], (size_t)(s_n - i - 1) * 6);
    s_n--;
}

void rq_clear(void) { s_n = 0; }

int rq_count(void) { return s_n; }

bool rq_push(const uint8_t addr[6]) {
    if (!addr || is_null(addr) || find(addr) >= 0 || s_n >= RQ_MAX) return false;
    memcpy(s_q[s_n++], addr, 6);
    return true;
}

bool rq_remove(const uint8_t addr[6]) {
    int i = addr ? find(addr) : -1;
    if (i < 0) return false;
    drop_at(i);
    return true;
}

bool rq_pop(uint8_t out[6], bool (*skip)(const uint8_t addr[6])) {
    while (s_n > 0) {
        uint8_t a[6];
        memcpy(a, s_q[0], 6);
        drop_at(0);
        if (skip && skip(a)) continue;
        memcpy(out, a, 6);
        return true;
    }
    return false;
}

int bt_hosts_file_write(const char *path, const uint8_t (*addrs)[6], int n) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    for (int i = 0; i < n; i++) {
        const uint8_t *a = addrs[i];
        fprintf(f, "%02X:%02X:%02X:%02X:%02X:%02X\n", a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int bt_hosts_file_read(const char *path, uint8_t (*out)[6], int max) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[64];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        unsigned int b[6];
        if (sscanf(line, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) continue;
        uint8_t a[6];
        for (int k = 0; k < 6; k++) a[k] = (uint8_t)b[k];
        if (is_null(a)) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) dup = dup || memcmp(out[k], a, 6) == 0;
        if (dup) continue;
        memcpy(out[n++], a, 6);
    }
    fclose(f);
    return n;
}
