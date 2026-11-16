#ifndef HARMONY_BT_WAKE_KEY_H
#define HARMONY_BT_WAKE_KEY_H

/* The key kept for a host whose link is down (g_pending_wake_btn), sent once that host's HID
 * channel opens (bt_main.c). The text is the key, "KEY <mod> <code>" or "CONSUMER <usage>",
 * followed by " @<address>" of the host it was meant for. Without the address the key went to
 * whichever host opened first and then to whatever host was the target 1.5 s later.
 *
 * Header only, on purpose: it uses nothing but the shared state declared in bt_state.h (no
 * function of another file), so every hub_btstack file and the host tests that build one of them
 * with stubs can use it. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* g_active_host_idx when a TARGET line named a host that cannot be used (an address that does not
 * parse, or a full host table): its keys are dropped. -1 still means "no target chosen", where
 * any connected host is used. */
#define BT_HOST_IDX_NONE (-2)

/* A kept key older than this is not sent any more, and its slot is free */
#define WAKE_KEY_MAX_AGE_S 10

static inline bool wake_key_fresh(time_t now) {
    return g_pending_wake_btn[0] && now >= g_pending_wake_time && now - g_pending_wake_time < WAKE_KEY_MAX_AGE_S;
}

static inline int wake_key_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The host the kept key was meant for; false when it names none */
static inline bool wake_key_owner(bd_addr_t out) {
    const char *at = strchr(g_pending_wake_btn, '@');
    if (!at) return false;
    const char *a = at + 1;
    for (int i = 0; i < 6; i++) {
        int hi = wake_key_hex(a[i * 3]), lo = wake_key_hex(a[i * 3 + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && a[i * 3 + 2] != ':')) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

/* Keep a key for the current target host, whose link is down. A fresh key kept for the same host
 * stays (the first key of a start is its power-on key); a stale key, or one kept for another host,
 * is replaced. It used to stay in the slot for ever when no HID channel opened after it (Hub off,
 * host not answering), and then blocked the next start's power-on key or went out in its place. */
static inline void wake_key_keep(const char *key) {
    if (!key || !key[0]) return;
    const uint8_t *owner = NULL;
    static const uint8_t no_addr[6] = {0};
    if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && memcmp(g_hosts[g_active_host_idx].addr, no_addr, 6) != 0) {
        owner = g_hosts[g_active_host_idx].addr;
    } else if (g_active_host_idx != -1) {
        return; /* the target could not be resolved: there is no host to wake */
    }
    time_t now = time(NULL);
    if (wake_key_fresh(now)) {
        bd_addr_t cur;
        bool has = wake_key_owner(cur);
        if (owner ? (has && memcmp(cur, owner, 6) == 0) : !has) return;
    }
    if (owner) {
        snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "%s @%02X:%02X:%02X:%02X:%02X:%02X", key,
                 owner[0], owner[1], owner[2], owner[3], owner[4], owner[5]);
    } else {
        snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "%s", key);
    }
    g_pending_wake_time = now;
}

/* Take the kept key for the host whose HID channel just opened: false when there is none, when it
 * is stale (then the slot is cleared) or when it was kept for another host (then it stays). */
static inline bool wake_key_take(const bd_addr_t opened, char *out, size_t out_len) {
    if (!g_pending_wake_btn[0]) return false;
    if (!wake_key_fresh(time(NULL))) {
        g_pending_wake_btn[0] = '\0';
        return false;
    }
    bd_addr_t owner;
    if (wake_key_owner(owner) && memcmp(owner, opened, 6) != 0) return false;
    snprintf(out, out_len, "%s", g_pending_wake_btn);
    char *at = strchr(out, '@');
    if (at) {
        *at = '\0';
        while (at > out && at[-1] == ' ') *--at = '\0';
    }
    g_pending_wake_btn[0] = '\0';
    return true;
}

/* Drop the kept key: for every host (addr NULL), or only when it was kept for addr (or for no
 * host in particular) */
static inline void wake_key_drop(const uint8_t *addr) {
    if (!g_pending_wake_btn[0]) return;
    bd_addr_t owner;
    if (addr && wake_key_owner(owner) && memcmp(owner, addr, 6) != 0) return;
    g_pending_wake_btn[0] = '\0';
}

#endif /* HARMONY_BT_WAKE_KEY_H */
