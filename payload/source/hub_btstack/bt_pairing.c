#include "bt_le_pair_window.h"
#include "bt_pairing.h"
#include "bt_remote.h"
#include "bt_listen.h"
#include "bt_adv.h"
#include "bt_status_json.h"
#include "../atomic_file.h"

/* Hosts connected right now, kept in /tmp: after a hub_btstack restart (deploy, crash) the same
 * hosts are reconnected, not only the activity's. Gone after a reboot. */
#define BT_HOSTS_UP_FILE "/tmp/hub_btstack_hosts_up"
static uint8_t s_hosts_before[MAX_HOST_CONNS][6];
static int s_hosts_before_n;
static bool s_hosts_restore_done;
static uint8_t s_hosts_saved[MAX_HOST_CONNS][6];
static int s_hosts_saved_n = -1;

#include "bt_wake_key.h"

#ifndef HOST_TARGET_FILE
#define HOST_TARGET_FILE "/data/harmony/bthid_host_target"
#endif
#ifndef DEVICES_FILE
#define DEVICES_FILE "/data/harmony/config/devices.json"
#endif

discovered_remote_t g_discovered[MAX_DISCOVERED];
int g_discovered_count = 0;
bool g_user_scan_active = false;
time_t g_user_scan_end = 0;

uint16_t get_target_host_hid_cid(void) {
    /* Once a target host is chosen (TARGET line), its keys go to it or nowhere: falling back to
     * another connected host sent e.g. Shield keys to a TV. Without a target (g_active_host_idx
     * -1, e.g. the web UI's "Auto") any connected host is used, as before. A target that could
     * not be used (BT_HOST_IDX_NONE) gets nothing. */
    if (g_active_host_idx == BT_HOST_IDX_NONE) return 0;
    if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS) {
        if (g_hosts[g_active_host_idx].connected && g_hosts[g_active_host_idx].hid_cid) {
            return g_hosts[g_active_host_idx].hid_cid;
        }
        return 0;
    }
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (g_hosts[i].connected && g_hosts[i].hid_cid) {
            return g_hosts[i].hid_cid;
        }
    }
    return 0;
}

bool is_any_host_connected(void) {
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (g_hosts[i].connected && g_hosts[i].hid_cid) return true;
    }
    return false;
}

/* Every host with an address is connected (a null-target reconnect loop connects them all);
 * false with no known host, so such a loop still tries the fallbacks in try_connect_host() */
bool all_known_hosts_connected(void) {
    bool any = false;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (is_null_bd_addr(g_hosts[i].addr)) continue;
        any = true;
        if (!g_hosts[i].connected || !g_hosts[i].hid_cid) return false;
    }
    return any;
}

bool is_host_connected(const bd_addr_t addr) {
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (memcmp(g_hosts[i].addr, addr, 6) == 0 && g_hosts[i].connected && g_hosts[i].hid_cid) return true;
    }
    return false;
}

bool host_manual_disconnect(const bd_addr_t addr) {
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (!is_null_bd_addr(g_hosts[i].addr) && memcmp(g_hosts[i].addr, addr, 6) == 0) {
            return g_hosts[i].manual_disconnect;
        }
    }
    return false;
}

void set_host_manual_disconnect(const bd_addr_t addr, bool on) {
    bool all = is_null_bd_addr(addr);
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (is_null_bd_addr(g_hosts[i].addr)) continue;
        if (all || memcmp(g_hosts[i].addr, addr, 6) == 0) g_hosts[i].manual_disconnect = on;
    }
}

/* True when the requested host is connected; for a null address (a loop over every known host),
 * when all of them are: with two hosts the first to connect used to end the loop for the other */
static bool host_target_connected(const bd_addr_t target) {
    return is_null_bd_addr(target) ? all_known_hosts_connected() : is_host_connected(target);
}

bool get_reconnect_target(bd_addr_t out) {
    /* The host of the current activity (last TARGET) first, then the pinned one */
    if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && !is_null_bd_addr(g_hosts[g_active_host_idx].addr)) {
        memcpy(out, g_hosts[g_active_host_idx].addr, 6);
        return true;
    }
    return get_pinned_host_target(out) != 0;
}

/* Names that only stand in until the host's device name is known */
static bool is_placeholder_host_name(const char *name) {
    return !name || !name[0] || strcmp(name, "Harmony Keyboard") == 0 || strncmp(name, "Host ", 5) == 0;
}

host_conn_t * register_host_target(const bd_addr_t addr, const char *name) {
    if (is_null_bd_addr(addr)) return NULL;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (memcmp(g_hosts[i].addr, addr, 6) == 0) {
            if (name && name[0] && is_placeholder_host_name(g_hosts[i].name)) {
                strncpy(g_hosts[i].name, name, sizeof(g_hosts[i].name) - 1);
                g_hosts[i].name[sizeof(g_hosts[i].name) - 1] = '\0';
            }
            return &g_hosts[i];
        }
    }
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (is_null_bd_addr(g_hosts[i].addr)) {
            memcpy(g_hosts[i].addr, addr, 6);
            /* no link yet: 0 is a valid connection handle, and a failed connection to this host
             * used to call gap_disconnect(0), which can close another device's link */
            g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
            if (name && name[0]) {
                strncpy(g_hosts[i].name, name, sizeof(g_hosts[i].name) - 1);
                g_hosts[i].name[sizeof(g_hosts[i].name) - 1] = '\0';
            } else {
                snprintf(g_hosts[i].name, sizeof(g_hosts[i].name), "Host %s", bd_addr_to_str(addr));
            }
            return &g_hosts[i];
        }
    }
    return NULL;
}

/* Read once at start, before anything rewrites the file */
void bt_hosts_load_before_restart(void) {
    s_hosts_before_n = bt_hosts_file_read(BT_HOSTS_UP_FILE, s_hosts_before, MAX_HOST_CONNS);
}

/* Rewrites the file when the set of connected hosts changed; not before the hosts connected
 * before the restart were queued, so a crash during the start does not lose them */
static void bt_hosts_save_connected(void) {
    if (!s_hosts_restore_done) return;
    uint8_t now[MAX_HOST_CONNS][6];
    int n = 0;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (!is_null_bd_addr(g_hosts[i].addr) && g_hosts[i].connected && g_hosts[i].hid_cid) memcpy(now[n++], g_hosts[i].addr, 6);
    }
    if (n == s_hosts_saved_n && memcmp(now, s_hosts_saved, (size_t)n * 6) == 0) return;
    if (bt_hosts_file_write(BT_HOSTS_UP_FILE, (const uint8_t (*)[6])now, n) == 0) {
        memcpy(s_hosts_saved, now, (size_t)n * 6);
        s_hosts_saved_n = n;
    }
}

void update_bthid_status(void) {
    bt_hosts_save_connected();
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddBoolToObject(root, "runtime", 1);
    cJSON_AddNumberToObject(root, "pid", (double)getpid());
    cJSON_AddNumberToObject(root, "updated", (double)time(NULL));
    bool any_conn = is_any_host_connected();
    cJSON_AddStringToObject(root, "state", any_conn ? "listening" : (g_host_pairing ? "pairing" : "waiting_host"));
    host_conn_t *ah = (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS) ? &g_hosts[g_active_host_idx] : NULL;
    /* hub_daemon and bt_reconnect.sh read "listening" plus a matching "target" as "this host is
     * connected", so name the chosen host only while it really is */
    const char *target = "";
    if (ah && ah->connected && ah->hid_cid) {
        target = bd_addr_to_str(ah->addr);
    } else if (!ah && any_conn) {
        target = bd_addr_to_str(g_host_addr);
    }
    cJSON_AddStringToObject(root, "target", target);
    cJSON_AddNumberToObject(root, "sent", (double)g_keys_sent);
    cJSON_AddNumberToObject(root, "skipped", 0);
    cJSON_AddStringToObject(root, "error", "");

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return;

    FILE *f = fopen(BTHID_STATUS_FILE ".new", "w");
    if (f) {
        fputs(out, f);
        fputc('\n', f);
        fclose(f);
        rename(BTHID_STATUS_FILE ".new", BTHID_STATUS_FILE);
    }
    free(out);
}

void update_status(const char *state_str) {
    /* The values; the layout is bt_status_write() (bt_status_json.c), which the tests run too */
    bt_status_view_t v;
    memset(&v, 0, sizeof(v));
    v.pid = (int)getpid();
    v.state = state_str;
    snprintf(v.target, sizeof(v.target), "%s", bd_addr_to_str(remote_device.addr));
    v.last_button = g_last_btn;
    v.last_seen = (long)g_last_seen;
    v.remote_connected = (connection_handle != HCI_CON_HANDLE_INVALID);
    snprintf(v.remote_addr, sizeof(v.remote_addr), "%s", bd_addr_to_str(remote_device.addr));
    uint16_t active_cid = get_target_host_hid_cid();
    host_conn_t *ah = (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS) ? &g_hosts[g_active_host_idx] : NULL;
    v.host_connected = (active_cid != 0);
    snprintf(v.host_addr, sizeof(v.host_addr), "%s",
             ah ? bd_addr_to_str(ah->addr) : (g_host_connected ? bd_addr_to_str(g_host_addr) : ""));
    v.host_name = (ah && ah->name[0]) ? ah->name : g_host_name;
    v.host_pairing = g_host_pairing;
    for (int i = 0; i < MAX_HOST_CONNS && v.nhosts < BT_STATUS_MAX_HOSTS; i++) {
        if (is_null_bd_addr(g_hosts[i].addr)) continue;
        bt_status_host_t *h = &v.hosts[v.nhosts++];
        /* paired = BTstack has a link key for it; hosts known only from devices.json
         * (e.g. after a reset, or migrated from the stock config) have none */
        link_key_t lk;
        link_key_type_t lkt;
        bd_addr_t ha;
        memcpy(ha, g_hosts[i].addr, 6);
        snprintf(h->addr, sizeof(h->addr), "%s", bd_addr_to_str(g_hosts[i].addr));
        h->name = g_hosts[i].name;
        h->connected = g_hosts[i].connected;
        h->paired = gap_get_link_key_for_bd_addr(ha, lk, &lkt);
        h->cid = g_hosts[i].hid_cid;
    }
    /* Written whole, then renamed over the old one: a reader (GET /api/bt-status) that came
     * between the truncation and the write saw an empty file and answered "no hosts" */
    FILE *f = fopen(STATUS_FILE ".new", "w");
    if (f) {
        bt_status_write(f, &v);
        if (fclose(f) == 0) rename(STATUS_FILE ".new", STATUS_FILE);
        else unlink(STATUS_FILE ".new");
    }
    update_bthid_status();
}

int get_pinned_target(bd_addr_t addr) {
    FILE *f = fopen(TARGET_FILE, "r");
    if (!f) f = fopen(MAP_FILE, "r");
    if (!f) return 0;
    char buf[1024];
    size_t rn = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rn] = 0;
    if (parse_bd_addr(buf, addr)) return 1;
    /* bt_remote_map.json (hub_daemon's): "remote": {"bdaddr": ...}, or "bdaddr" at the top. The
     * text search used before started on the colon after the key and never found the address,
     * so a remote pinned only in the map (after a restore) was never looked for. */
    cJSON *mj = cJSON_Parse(buf);
    cJSON *rem = mj ? cJSON_GetObjectItemCaseSensitive(mj, "remote") : NULL;
    cJSON *ja = rem ? cJSON_GetObjectItemCaseSensitive(rem, "bdaddr") : NULL;
    if (!ja && mj) ja = cJSON_GetObjectItemCaseSensitive(mj, "bdaddr");
    int found = ja && cJSON_IsString(ja) && parse_bd_addr(ja->valuestring, addr) && !is_null_bd_addr(addr);
    cJSON_Delete(mj);
    return found;
}

int get_pinned_host_target(bd_addr_t addr) {
    const char *files[] = {HOST_TARGET_FILE, DEVICES_FILE, NULL};
    for (int fi = 0; files[fi]; fi++) {
        FILE *f = fopen(files[fi], "r");
        if (!f) continue;
        char buf[4096];
        size_t rn = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[rn] = 0;
        /* stop where a whole address no longer fits: the tests below read up to p[14], which
         * near the end went past the data and past the buffer */
        for (char *p = buf; p + 17 <= buf + rn; p++) {
            if (isxdigit((unsigned char)*p) && p[2] == ':' && p[5] == ':' && p[8] == ':' && p[11] == ':' && p[14] == ':') {
                char mac[18];
                strncpy(mac, p, 17);
                mac[17] = 0;
                if (parse_bd_addr(mac, addr) && !is_null_bd_addr(addr)) return 1;
            }
        }
    }
    return 0;
}

/* Name of the device with this id in config/devices.json; out is "" when not found */
static void device_name_for_id(const char *dev_id, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!dev_id) return;
    FILE *f = fopen("/data/harmony/config/devices.json", "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (sz > 0 && sz < 1000000) ? (char *)malloc((size_t)sz + 1) : NULL;
    if (buf) {
        size_t r = fread(buf, 1, (size_t)sz, f);
        buf[r] = 0;
        cJSON *root = cJSON_Parse(buf);
        free(buf);
        cJSON *devs = root ? cJSON_GetObjectItemCaseSensitive(root, "devices") : NULL;
        cJSON *item = NULL;
        if (devs && cJSON_IsArray(devs)) {
            cJSON_ArrayForEach(item, devs) {
                cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
                char id_str[32] = {0};
                if (jid && cJSON_IsNumber(jid)) snprintf(id_str, sizeof(id_str), "%ld", (long)jid->valuedouble);
                else if (jid && cJSON_IsString(jid)) strncpy(id_str, jid->valuestring, sizeof(id_str) - 1);
                cJSON *jn = cJSON_GetObjectItemCaseSensitive(item, "name");
                if (strcmp(id_str, dev_id) == 0 && jn && cJSON_IsString(jn) && jn->valuestring[0]) {
                    strncpy(out, jn->valuestring, out_len - 1);
                    out[out_len - 1] = '\0';
                    break;
                }
            }
        }
        cJSON_Delete(root);
    }
    fclose(f);
}

/* ---- Device links handed to hub_daemon ----
 * hub_daemon owns devices.json and keeps it cached: a file written behind its back (the fallback
 * this replaces) was not seen by its command sending and was undone by the next device edit.
 * A link is sent with POST /api/bt-link-device on loopback (trusted). The exchange runs on the
 * run loop (non-blocking socket, a data source and a timeout timer): keys keep flowing while it
 * waits. A link hub_daemon did not answer is kept and sent again after a pause that doubles from
 * 1 s up to 60 s, until it answers. A link it refused (no such device) is dropped. */
#define LINK_PENDING_MAX 8
#define LINK_RETRY_FIRST_MS 1000
#define LINK_RETRY_MAX_MS 60000
#define LINK_EXCHANGE_TIMEOUT_MS 3000

typedef struct {
    char dev_id[64];
    char addr[18];
    uint8_t ok_false; /* 200 {"ok":false} answers so far (an older hub_daemon: no such device) */
} link_job_t;

/* An older hub_daemon answers 200 {"ok":false} for an unknown device (and for nothing else that
 * could pass later, except a missing device id): after this many such answers the link is dropped */
#define LINK_OK_FALSE_MAX 3

typedef enum { LINK_OK = 0, LINK_REFUSED, LINK_NO_ANSWER, LINK_OK_FALSE } link_result_t;
typedef enum { LINK_IDLE = 0, LINK_CONNECTING, LINK_READING } link_phase_t;

static link_job_t s_links[LINK_PENDING_MAX];
static int s_links_n;
static uint32_t s_link_retry_ms = LINK_RETRY_FIRST_MS;
static unsigned s_link_failures; /* tries in a row hub_daemon did not answer */
static btstack_timer_source_t s_link_retry_timer;
static bool s_link_pause; /* the retry timer is running */
static btstack_timer_source_t s_link_timeout_timer;
static btstack_data_source_t s_link_ds;
static int s_link_fd = -1;
static link_phase_t s_link_phase = LINK_IDLE;
static char s_link_req[512];
static size_t s_link_req_len, s_link_req_sent;
static char s_link_resp[512];
static size_t s_link_resp_len;
/* the link being sent was replaced by a newer one: send it again after this answer */
static bool s_link_resend;

static void link_try_next(void);
static void link_retry_handler(btstack_timer_source_t *ts);

static void link_pop_front(void) {
    if (s_links_n <= 0) return;
    memmove(&s_links[0], &s_links[1], sizeof(s_links[0]) * (size_t)(s_links_n - 1));
    s_links_n--;
}

/* 200 with {"ok":true} links it. 404 (no such device) and 400 refuse it: it can never succeed.
 * Anything else is no answer and is sent again: nothing, a cut answer, a 200 without its body,
 * 5xx (hub_daemon could not save it). A 200 {"ok":false} (older hub_daemon) is sent again up to
 * LINK_OK_FALSE_MAX times. */
static link_result_t link_parse_answer(const char *resp, size_t len) {
    int code = 0;
    if (len < 12 || strncmp(resp, "HTTP/1.", 7) != 0 || sscanf(resp + 8, " %3d", &code) != 1) return LINK_NO_ANSWER;
    if (code == 404 || code == 400) return LINK_REFUSED;
    if (code != 200) return LINK_NO_ANSWER;
    const char *body = strstr(resp, "\r\n\r\n");
    if (!body) return LINK_NO_ANSWER;
    cJSON *j = cJSON_Parse(body + 4);
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(j, "ok");
    link_result_t r = cJSON_IsTrue(ok) ? LINK_OK : cJSON_IsFalse(ok) ? LINK_OK_FALSE : LINK_NO_ANSWER;
    cJSON_Delete(j);
    return r;
}

static void link_exchange_end(link_result_t r) {
    if (s_link_fd >= 0) {
        btstack_run_loop_remove_data_source(&s_link_ds);
        close(s_link_fd);
        s_link_fd = -1;
    }
    btstack_run_loop_remove_timer(&s_link_timeout_timer);
    s_link_phase = LINK_IDLE;
    if (s_links_n <= 0) return;
    char msg[192];
    if (s_link_resend) {
        /* the answer is for the old address: the newer one goes now, whatever the answer was */
        s_link_resend = false;
        s_link_retry_ms = LINK_RETRY_FIRST_MS;
        link_try_next();
        return;
    }
    if (r == LINK_OK_FALSE) {
        if (++s_links[0].ok_false >= LINK_OK_FALSE_MAX) {
            snprintf(msg, sizeof(msg), "hub_daemon answered {\"ok\":false} %d times for the link of device %s: dropped",
                     LINK_OK_FALSE_MAX, s_links[0].dev_id);
            log_msg(msg);
            link_pop_front();
            s_link_failures = 0;
            s_link_retry_ms = LINK_RETRY_FIRST_MS;
            link_try_next();
            return;
        }
        r = LINK_NO_ANSWER;
    }
    if (r == LINK_OK || r == LINK_REFUSED) {
        if (r == LINK_OK) {
            snprintf(msg, sizeof(msg), "Device %s linked to host %s by hub_daemon%s", s_links[0].dev_id, s_links[0].addr,
                     s_link_failures ? " (after waiting for it)" : "");
        } else {
            snprintf(msg, sizeof(msg), "hub_daemon refused the link of device %s to host %s (no such device): not kept", s_links[0].dev_id, s_links[0].addr);
        }
        log_msg(msg);
        link_pop_front();
        s_link_failures = 0;
        s_link_retry_ms = LINK_RETRY_FIRST_MS;
        link_try_next();
        return;
    }
    s_link_failures++;
    /* the first miss, then once the pause is at its longest (about once a minute) */
    if (s_link_failures == 1 || s_link_retry_ms >= LINK_RETRY_MAX_MS) {
        snprintf(msg, sizeof(msg), "hub_daemon did not answer the link of device %s to host %s: trying again in %u s",
                 s_links[0].dev_id, s_links[0].addr, (unsigned)(s_link_retry_ms / 1000));
        log_msg(msg);
    }
    btstack_run_loop_remove_timer(&s_link_retry_timer);
    btstack_run_loop_set_timer_handler(&s_link_retry_timer, link_retry_handler);
    btstack_run_loop_set_timer(&s_link_retry_timer, s_link_retry_ms);
    btstack_run_loop_add_timer(&s_link_retry_timer);
    s_link_pause = true;
    s_link_retry_ms = s_link_retry_ms * 2 > LINK_RETRY_MAX_MS ? LINK_RETRY_MAX_MS : s_link_retry_ms * 2;
}

static void link_timeout_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    link_exchange_end(LINK_NO_ANSWER);
}

static void link_retry_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    s_link_pause = false;
    link_try_next();
}

static void link_ds_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t type) {
    UNUSED(ds);
    if (s_link_phase == LINK_CONNECTING && (type & DATA_SOURCE_CALLBACK_WRITE)) {
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(s_link_fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            link_exchange_end(LINK_NO_ANSWER);
            return;
        }
        while (s_link_req_sent < s_link_req_len) {
            ssize_t n = send(s_link_fd, s_link_req + s_link_req_sent, s_link_req_len - s_link_req_sent, MSG_NOSIGNAL);
            if (n > 0) {
                s_link_req_sent += (size_t)n;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                return; /* the rest on the next write callback */
            } else {
                link_exchange_end(LINK_NO_ANSWER);
                return;
            }
        }
        btstack_run_loop_disable_data_source_callbacks(&s_link_ds, DATA_SOURCE_CALLBACK_WRITE);
        btstack_run_loop_enable_data_source_callbacks(&s_link_ds, DATA_SOURCE_CALLBACK_READ);
        s_link_phase = LINK_READING;
        return;
    }
    if (s_link_phase == LINK_READING && (type & DATA_SOURCE_CALLBACK_READ)) {
        for (;;) {
            if (s_link_resp_len >= sizeof(s_link_resp) - 1) break; /* enough to judge */
            ssize_t n = recv(s_link_fd, s_link_resp + s_link_resp_len, sizeof(s_link_resp) - 1 - s_link_resp_len, 0);
            if (n > 0) {
                s_link_resp_len += (size_t)n;
                s_link_resp[s_link_resp_len] = '\0';
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return; /* more later */
            break; /* the end of the answer (Connection: close), or an error */
        }
        s_link_resp[s_link_resp_len] = '\0';
        link_exchange_end(link_parse_answer(s_link_resp, s_link_resp_len));
    }
}

/* Send the oldest waiting link, unless an exchange or a pause is running */
static void link_try_next(void) {
    if (s_link_phase != LINK_IDLE || s_links_n <= 0) return;
    btstack_run_loop_remove_timer(&s_link_retry_timer);
    s_link_pause = false;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "deviceId", s_links[0].dev_id);
    cJSON_AddStringToObject(b, "bdaddr", s_links[0].addr);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    int rlen = body ? snprintf(s_link_req, sizeof(s_link_req),
                               "POST /api/bt-link-device HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                               "Content-Length: %u\r\nConnection: close\r\n\r\n%s", (unsigned)strlen(body), body) : -1;
    free(body);
    if (rlen <= 0 || rlen >= (int)sizeof(s_link_req)) {
        link_exchange_end(LINK_REFUSED); /* cannot be sent: drop it */
        return;
    }
    s_link_req_len = (size_t)rlen;
    s_link_req_sent = 0;
    s_link_resp_len = 0;
    s_link_resp[0] = '\0';

    s_link_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_link_fd < 0) {
        link_exchange_end(LINK_NO_ANSWER);
        return;
    }
    int fl = fcntl(s_link_fd, F_GETFL, 0);
    fcntl(s_link_fd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
    fcntl(s_link_fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(DAEMON_PORT);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s_link_fd, (struct sockaddr *)&sin, sizeof(sin)) != 0 && errno != EINPROGRESS) {
        close(s_link_fd);
        s_link_fd = -1;
        link_exchange_end(LINK_NO_ANSWER); /* hub_daemon not listening (restarting) */
        return;
    }
    s_link_phase = LINK_CONNECTING;
    memset(&s_link_ds, 0, sizeof(s_link_ds));
    btstack_run_loop_set_data_source_fd(&s_link_ds, s_link_fd);
    btstack_run_loop_set_data_source_handler(&s_link_ds, link_ds_process);
    btstack_run_loop_enable_data_source_callbacks(&s_link_ds, DATA_SOURCE_CALLBACK_WRITE);
    btstack_run_loop_add_data_source(&s_link_ds);
    btstack_run_loop_set_timer_handler(&s_link_timeout_timer, link_timeout_handler);
    btstack_run_loop_remove_timer(&s_link_timeout_timer);
    btstack_run_loop_set_timer(&s_link_timeout_timer, LINK_EXCHANGE_TIMEOUT_MS);
    btstack_run_loop_add_timer(&s_link_timeout_timer);
}

/* Queue a link and send it as soon as nothing else is being sent. A newer link of the same device
 * replaces a waiting one. */
static void link_queue(const char *dev_id, const char *addr_str) {
    int i;
    for (i = 0; i < s_links_n; i++) {
        if (strcmp(s_links[i].dev_id, dev_id) == 0) break;
    }
    if (i == s_links_n) {
        if (s_links_n == LINK_PENDING_MAX) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Too many device links waiting: link of device %s dropped", s_links[s_links_n - 1].dev_id);
            log_msg(msg);
            i = s_links_n - 1; /* the front may be in flight: replace the newest */
        } else {
            s_links_n++;
        }
    }
    if (i == 0 && s_link_phase != LINK_IDLE && strcmp(s_links[0].addr, addr_str) != 0) {
        /* That link is being sent with the old address. Its request may have reached hub_daemon
         * already: let it finish (answer or 3 s timeout), then send the new one, so that the old
         * one is not applied after it. The same address again needs nothing more. */
        s_link_resend = true;
    }
    snprintf(s_links[i].dev_id, sizeof(s_links[i].dev_id), "%s", dev_id);
    snprintf(s_links[i].addr, sizeof(s_links[i].addr), "%s", addr_str);
    s_links[i].ok_false = 0;
    /* a new link does not wait for the pause of an older one */
    s_link_retry_ms = LINK_RETRY_FIRST_MS;
    link_try_next();
}

/* Drop waiting links to this host (it was forgotten); one being sent is abandoned. The pause and
 * the count of tries without an answer belong to the front link: when it goes, they go with it and
 * the next link is sent at once (hub_daemon just sent the forget command, so it answers). */
static void link_drop_host(const bd_addr_t addr) {
    int w = 0;
    bool front_gone = false;
    for (int r = 0; r < s_links_n; r++) {
        bd_addr_t a;
        if (parse_bd_addr(s_links[r].addr, a) && memcmp(a, addr, 6) == 0) {
            if (r == 0) front_gone = true;
            if (r == 0 && s_link_phase != LINK_IDLE) {
                s_link_resend = false;
                if (s_link_fd >= 0) {
                    btstack_run_loop_remove_data_source(&s_link_ds);
                    close(s_link_fd);
                    s_link_fd = -1;
                }
                btstack_run_loop_remove_timer(&s_link_timeout_timer);
                s_link_phase = LINK_IDLE;
            }
            continue;
        }
        if (w != r) s_links[w] = s_links[r];
        w++;
    }
    s_links_n = w;
    if (s_links_n == 0 || front_gone) {
        btstack_run_loop_remove_timer(&s_link_retry_timer);
        s_link_pause = false;
        s_link_failures = 0;
        s_link_retry_ms = LINK_RETRY_FIRST_MS;
    }
    if (s_links_n > 0 && s_link_phase == LINK_IDLE && !s_link_pause) link_try_next();
}

void link_bt_device_to_addr(const char *dev_id, const char *addr_str) {
    if (!dev_id || !dev_id[0] || !addr_str || !addr_str[0]) return;
    char msg[128];
    snprintf(msg, sizeof(msg), "Linking device %s to Bluetooth address %s", dev_id, addr_str);
    log_msg(msg);
    char dev_name[64];
    device_name_for_id(dev_id, dev_name, sizeof(dev_name));

    /* config/devices.json is the only device-to-host mapping; bt-devices.json, which used to
     * hold a second copy, is no longer written (hub_daemon folds old copies in, bt_migrate.c).
     * Only hub_daemon writes it: the link waits here until hub_daemon has taken it. */
    link_queue(dev_id, addr_str);

    /* Show the linked device's name for the host right away; until hub_btstack restarted, a
     * host paired from the web UI was listed as "Host <bdaddr>" */
    bd_addr_t ba;
    if (dev_name[0] && parse_bd_addr(addr_str, ba)) {
        host_conn_t *h = register_host_target(ba, NULL);
        if (h) {
            strncpy(h->name, dev_name, sizeof(h->name) - 1);
            h->name[sizeof(h->name) - 1] = '\0';
        }
        update_status(app_state == APP_READY ? "ready" : "standby");
    }
}

/* An outgoing HID connection failed (also when it is cancelled: closing a channel that still waits
 * for its link reports "open failed" at once, inside hid_device_disconnect()). The reconnect loop is
 * restarted only for a host still in the table: a host that was just forgotten must stay gone. */
void host_open_failed(const bd_addr_t conn_addr_in, uint16_t cid, uint8_t status) {
    bd_addr_t conn_addr;
    memcpy(conn_addr, conn_addr_in, 6);
    g_host_connecting = false;
    char msg[128];
    snprintf(msg, sizeof(msg), "Classic HID Host connection to %s failed (0x%02x)", bd_addr_to_str(conn_addr), status);
    log_msg(msg);
    bool known = false;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        bool same = memcmp(g_hosts[i].addr, conn_addr, 6) == 0;
        if (same && !is_null_bd_addr(conn_addr)) known = true;
        if (same || g_hosts[i].hid_cid == cid) {
            g_hosts[i].connected = false;
            g_hosts[i].hid_cid = 0;
            if (g_hosts[i].acl_handle != HCI_CON_HANDLE_INVALID) {
                gap_disconnect(g_hosts[i].acl_handle);
                g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
            }
        }
    }
    g_host_connected = is_any_host_connected();
    g_host_hid_cid = get_target_host_hid_cid();
    update_status(app_state == APP_READY ? "ready" : "standby");
    update_bthid_status();
    char cur_act[64] = "-1";
    read_current_activity(cur_act, sizeof(cur_act));
    if (!g_host_pairing && cur_act[0] && strcmp(cur_act, "-1") != 0 && strcasecmp(cur_act, "poweroff") != 0) {
        if (known && !g_host_reconnect_active && !host_manual_disconnect(conn_addr)) {
            start_host_reconnect(conn_addr, MAX_HOST_RECONNECT_ATTEMPTS);
        }
    } else {
        log_msg("Host disconnected while Activity is PowerOff, suppressing reconnect");
    }
}

/* host_forget <addr>: remove a classic host for good. Its link key goes (so it cannot reconnect
 * outside pairing mode: a new pairing request is refused), its link is closed, it leaves the host
 * table and every reconnect list, and no kept key or waiting device link goes to it. hub_daemon
 * clears the devices linked to it and the default host file before it sends the command. */
void forget_classic_host(const bd_addr_t addr_in) {
    bd_addr_t addr;
    if (is_null_bd_addr(addr_in)) return;
    memcpy(addr, addr_in, 6);
    char msg[128];
    snprintf(msg, sizeof(msg), "Forgetting Bluetooth host %s", bd_addr_to_str(addr));
    log_msg(msg);

    /* no more reconnects to it: its loop stops (another host's loop goes on), it leaves the queue
     * and the hosts to restore after a restart */
    rq_remove(addr);
    if (g_host_reconnect_active && memcmp(g_host_reconnect_target, addr, 6) == 0) {
        stop_host_reconnect();
    }
    int w = 0;
    for (int r = 0; r < s_hosts_before_n; r++) {
        if (memcmp(s_hosts_before[r], addr, 6) == 0) continue;
        if (w != r) memcpy(s_hosts_before[w], s_hosts_before[r], 6);
        w++;
    }
    s_hosts_before_n = w;
    wake_key_drop(addr);
    link_drop_host(addr);
    if (g_pairing_new_key_valid && memcmp(g_pairing_new_key_addr, addr, 6) == 0) g_pairing_new_key_valid = false;

    /* The entry goes first, the channels are closed after: closing a connection that is still being
     * set up reports a failed open at once, and that must not find the host and reconnect it. */
    uint16_t cids[MAX_HOST_CONNS];
    hci_con_handle_t handles[MAX_HOST_CONNS];
    int n = 0;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (memcmp(g_hosts[i].addr, addr, 6) != 0) continue;
        cids[n] = g_hosts[i].hid_cid;
        handles[n] = g_hosts[i].acl_handle;
        n++;
        memset(&g_hosts[i], 0, sizeof(g_hosts[i]));
        g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
        /* the next key brings its own TARGET; until then nothing goes to another host */
        if (g_active_host_idx == i) g_active_host_idx = BT_HOST_IDX_NONE;
    }
    gap_drop_link_key_for_bd_addr(addr);
    for (int k = 0; k < n; k++) {
        /* close the HID channels, then the link itself: an open link stays authenticated without
         * the key and the host could open the channels again on it */
        if (cids[k]) hid_device_disconnect(cids[k]);
        if (handles[k] != HCI_CON_HANDLE_INVALID) gap_disconnect(handles[k]);
    }
    if (memcmp(g_host_addr, addr, 6) == 0) {
        /* the "last paired host" record: it was tried when no other host was known */
        memset(g_host_addr, 0, sizeof(g_host_addr));
        if (tlv_impl) tlv_impl->delete_tag(&tlv_context, TLV_TAG_HIDR);
    }
    /* hub_daemon removed the default host file already; a connection that opened in between may
     * have written it again (it is written when missing) */
    bd_addr_t def;
    FILE *hf = fopen(HOST_TARGET_FILE, "r");
    if (hf) {
        char line[64] = "";
        bool names_it = false;
        while (fgets(line, sizeof(line), hf)) {
            const char *v = strncmp(line, "bdaddr=", 7) == 0 ? line + 7 : line;
            char a17[18];
            snprintf(a17, sizeof(a17), "%s", v);
            if (parse_bd_addr(a17, def) && memcmp(def, addr, 6) == 0) names_it = true;
        }
        fclose(hf);
        if (names_it) unlink(HOST_TARGET_FILE);
    }
    g_host_connected = is_any_host_connected();
    g_host_hid_cid = get_target_host_hid_cid();
    host_reconnect_next();
    update_status(app_state == APP_READY ? "ready" : "standby");
    update_bthid_status();
}

static cJSON *load_json_file(const char *path, long max_size) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    cJSON *root = NULL;
    if (sz > 0 && sz < max_size) {
        char *buf = (char *)malloc(sz + 1);
        if (buf) {
            size_t r = fread(buf, 1, sz, f);
            buf[r] = 0;
            root = cJSON_Parse(buf);
            free(buf);
        }
    }
    fclose(f);
    return root;
}

/* The host of an activity: the first of its devices (config/activities.json deviceIds) that is a
 * Bluetooth device with an address in config/devices.json. With two hosts paired, the pinned host
 * (the last one paired) is not necessarily the one the running activity uses. */
/* An "id" as text: string ids as they are, numeric ids (older or hand-written files) printed */
static bool json_id_str(const cJSON *j, char *out, size_t out_len) {
    if (!j || !out || out_len == 0) return false;
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
        snprintf(out, out_len, "%s", j->valuestring);
        return true;
    }
    if (cJSON_IsNumber(j)) {
        snprintf(out, out_len, "%ld", (long)j->valuedouble);
        return true;
    }
    return false;
}

bool get_activity_host_target(const char *act_id, bd_addr_t out) {
    if (!act_id || !act_id[0]) return false;
    bool found = false;
    cJSON *acts = load_json_file("/data/harmony/config/activities.json", 1000000);
    cJSON *devs = load_json_file("/data/harmony/config/devices.json", 1000000);
    cJSON *alist = acts ? cJSON_GetObjectItemCaseSensitive(acts, "activities") : NULL;
    cJSON *dlist = devs ? cJSON_GetObjectItemCaseSensitive(devs, "devices") : NULL;
    cJSON *act = NULL;
    if (alist && dlist && cJSON_IsArray(alist) && cJSON_IsArray(dlist)) {
        cJSON_ArrayForEach(act, alist) {
            char aid[64];
            if (!json_id_str(cJSON_GetObjectItemCaseSensitive(act, "id"), aid, sizeof(aid)) || strcmp(aid, act_id) != 0) continue;
            cJSON *ids = cJSON_GetObjectItemCaseSensitive(act, "deviceIds");
            cJSON *did = NULL;
            if (ids && cJSON_IsArray(ids)) {
                cJSON_ArrayForEach(did, ids) {
                    char want[64];
                    if (!json_id_str(did, want, sizeof(want))) continue;
                    cJSON *dev = NULL;
                    cJSON_ArrayForEach(dev, dlist) {
                        char have[64];
                        if (!json_id_str(cJSON_GetObjectItemCaseSensitive(dev, "id"), have, sizeof(have)) || strcmp(have, want) != 0) continue;
                        /* ids are unique: this is the device, BT or not */
                        cJSON *jtype = cJSON_GetObjectItemCaseSensitive(dev, "type");
                        cJSON *jaddr = cJSON_GetObjectItemCaseSensitive(dev, "bdaddr");
                        if (jtype && cJSON_IsString(jtype) && strcasecmp(jtype->valuestring, "BT") == 0 &&
                            jaddr && cJSON_IsString(jaddr) && parse_bd_addr(jaddr->valuestring, out) && !is_null_bd_addr(out)) {
                            found = true;
                        }
                        break;
                    }
                    if (found) break;
                }
            }
            break;
        }
    }
    if (acts) cJSON_Delete(acts);
    if (devs) cJSON_Delete(devs);
    return found;
}

void init_known_hosts(void) {
    /* An empty default host file (cut by a power loss while it was written) would never be
     * rewritten: it is only written when missing or at a new pairing */
    struct stat hst;
    if (stat(HOST_TARGET_FILE, &hst) == 0 && hst.st_size == 0) unlink(HOST_TARGET_FILE);
    bd_addr_t def_target;
    /* Register the pinned host without a name: the devices below name it by address.
     * It used to get the first BT device's name in devices.json, whichever host that was. */
    if (get_pinned_host_target(def_target)) {
        register_host_target(def_target, NULL);
    }

    const char *dpath = "/data/harmony/config/devices.json";
    FILE *df = fopen(dpath, "r");
    if (df) {
        fseek(df, 0, SEEK_END);
        long sz = ftell(df);
        fseek(df, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000) {
            char *buf = (char *)malloc(sz + 1);
            if (buf) {
                size_t r = fread(buf, 1, sz, df);
                buf[r] = 0;
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *item = NULL;
                        cJSON_ArrayForEach(item, devs) {
                            /* Host names come only from the device mapped to the host here */
                            cJSON *ja = cJSON_GetObjectItemCaseSensitive(item, "bdaddr");
                            if (!ja) ja = cJSON_GetObjectItemCaseSensitive(item, "btHost");
                            cJSON *jn = cJSON_GetObjectItemCaseSensitive(item, "name");
                            if (ja && cJSON_IsString(ja) && ja->valuestring[0]) {
                                bd_addr_t ba;
                                if (parse_bd_addr(ja->valuestring, ba)) {
                                    const char *nm = (jn && cJSON_IsString(jn)) ? jn->valuestring : NULL;
                                    register_host_target(ba, nm);
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(df);
    }
}

void try_connect_host_addr(const bd_addr_t addr) {
    if (is_null_bd_addr(addr)) return;
    set_host_manual_disconnect(addr, false);
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (g_hosts[i].connected && g_hosts[i].hid_cid != 0 && memcmp(g_hosts[i].addr, addr, 6) == 0) {
            return;
        }
    }
    if (g_host_connecting && (time(NULL) - g_host_connecting_start) < 5) {
        /* Another attempt is running: this host waits for its turn (the loop's own target is
         * tried again by the loop). It used to be skipped and never tried. */
        if (!(g_host_reconnect_active && memcmp(g_host_reconnect_target, addr, 6) == 0) && rq_push(addr)) {
            char qmsg[96];
            snprintf(qmsg, sizeof(qmsg), "Host %s waits: another connection attempt is running", bd_addr_to_str(addr));
            log_msg(qmsg);
        }
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Initiating connection to Classic HID Host: %s", bd_addr_to_str(addr));
    log_msg(msg);
    g_host_connecting = true;
    g_host_connecting_start = time(NULL);

    // Suspend LE scanning during Classic ACL paging to dedicate CSR8811 radio to Host connection
    // (a plugin's adv_listen too: it scans more than the reconnect, and goes on after the page)
    if ((app_state == APP_SCANNING && !g_user_scan_active) || g_adv_listen_active) {
        gap_stop_scan();
    }

    uint16_t cid = 0;
    uint8_t status = hid_device_connect((uint8_t*)addr, &cid);
    if (status != ERROR_CODE_SUCCESS || cid == 0) {
        g_host_connecting = false;
        if ((app_state == APP_SCANNING && !g_user_scan_active) || g_adv_listen_active) {
            gap_start_scan(); /* paused for the page above */
        }
        snprintf(msg, sizeof(msg), "Classic HID Host connect request failed (status=0x%02x, cid=0x%04x)", status, cid);
        log_msg(msg);
        return;
    }
    host_conn_t *h = register_host_target(addr, NULL);
    if (h) {
        h->hid_cid = cid;
    }
}

void try_connect_host(void) {
    bool found = false;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (!is_null_bd_addr(g_hosts[i].addr)) {
            found = true;
            if (!g_hosts[i].connected) {
                try_connect_host_addr(g_hosts[i].addr);
            }
        }
    }
    if (!found) {
        if (!is_null_bd_addr(g_host_addr)) {
            try_connect_host_addr(g_host_addr);
        } else {
            bd_addr_t target;
            if (get_pinned_host_target(target)) {
                register_host_target(target, NULL);
                try_connect_host_addr(target);
            }
        }
    }
}

static bool rq_skip_host(const uint8_t addr[6]) {
    return is_host_connected(addr) || host_manual_disconnect(addr);
}

/* A loop ended (its host connected, or it gave up): the next waiting host gets its turn */
void host_reconnect_next(void) {
    if (g_host_reconnect_active || g_host_pairing) return;
    /* No reconnects while the activity is off (start_host_reconnect refuses them): drop the
     * waiting hosts at once instead of one refused start per host */
    char cur_act[64] = "-1";
    read_current_activity(cur_act, sizeof(cur_act));
    if (strcmp(cur_act, "-1") == 0 || strcasecmp(cur_act, "poweroff") == 0) {
        rq_clear();
        return;
    }
    bd_addr_t next;
    if (!rq_pop(next, rq_skip_host)) return;
    char msg[96];
    snprintf(msg, sizeof(msg), "Host reconnect: next waiting host %s", bd_addr_to_str(next));
    log_msg(msg);
    start_host_reconnect(next, MAX_HOST_RECONNECT_ATTEMPTS);
}

/* Controller ready (after the activity's host loop was started): the hosts that were connected
 * before this hub_btstack started wait for their turn */
void bt_hosts_restore_after_start(void) {
    int queued = 0;
    for (int i = 0; i < s_hosts_before_n; i++) {
        register_host_target(s_hosts_before[i], NULL);
        if (g_host_reconnect_active && memcmp(g_host_reconnect_target, s_hosts_before[i], 6) == 0) continue;
        if (rq_push(s_hosts_before[i])) queued++;
    }
    if (queued) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Host reconnect: %d host(s) connected before the restart wait for their turn", queued);
        log_msg(msg);
    }
    s_hosts_restore_done = true;
    host_reconnect_next();
}

static void host_reconnect_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (!g_host_reconnect_active) return;

    if (host_target_connected(g_host_reconnect_target)) {
        log_msg("Host reconnect: Host connected, stopping retry loop");
        g_host_reconnect_active = false;
        g_host_connecting = false;
        host_reconnect_next();
        return;
    }

    if (g_host_reconnect_attempts >= MAX_HOST_RECONNECT_ATTEMPTS) {
        log_msg("Host reconnect: Maximum retry attempts reached");
        /* a key kept for this host would otherwise wait for any later connection */
        wake_key_drop(is_null_bd_addr(g_host_reconnect_target) ? NULL : g_host_reconnect_target);
        g_host_reconnect_active = false;
        g_host_connecting = false;
        host_reconnect_next();
        return;
    }

    if (g_host_connecting && (time(NULL) - g_host_connecting_start) < 5) {
        btstack_run_loop_set_timer(&g_host_reconnect_timer, 1000);
        btstack_run_loop_add_timer(&g_host_reconnect_timer);
        return;
    }

    g_host_reconnect_attempts++;
    char msg[128];
    snprintf(msg, sizeof(msg), "Host reconnect: Attempt %d/%d to %s",
             g_host_reconnect_attempts, MAX_HOST_RECONNECT_ATTEMPTS,
             is_null_bd_addr(g_host_reconnect_target) ? "targets" : bd_addr_to_str(g_host_reconnect_target));
    log_msg(msg);

    if (!is_null_bd_addr(g_host_reconnect_target)) {
        try_connect_host_addr(g_host_reconnect_target);
    } else {
        try_connect_host();
    }

    uint32_t next_ms = (g_host_reconnect_attempts <= 5) ? 1000 : 2000;
    btstack_run_loop_set_timer(&g_host_reconnect_timer, next_ms);
    btstack_run_loop_add_timer(&g_host_reconnect_timer);
}

void start_host_reconnect(const bd_addr_t target, int max_attempts) {
    (void)max_attempts;
    char cur_act[64] = "-1";
    read_current_activity(cur_act, sizeof(cur_act));
    if (!g_host_pairing && (strcmp(cur_act, "-1") == 0 || strcasecmp(cur_act, "poweroff") == 0)) {
        log_msg("Host reconnect: Suppressed while Activity is PowerOff");
        /* No key waits for a connection while the Hub is off: a kept key (e.g. a power-off key
         * that came after the link closed) went out at the next start in place of its own key */
        wake_key_drop(NULL);
        return;
    }

    set_host_manual_disconnect(target, false);
    /* Before the controller is ready a connection attempt goes nowhere: wait for it (the
     * hosts waiting are started from bt_hosts_restore_after_start). Only an address waits:
     * every caller passes one (the activity's, pinned or requested host), none asks for a
     * loop over all hosts. */
    if (hci_get_state() != HCI_STATE_WORKING) {
        if (rq_push(target)) {
            char dmsg[96];
            snprintf(dmsg, sizeof(dmsg), "Host reconnect for %s waits until the controller is ready", bd_addr_to_str(target));
            log_msg(dmsg);
        }
        return;
    }
    /* Another connected host must not block this one (several HID hosts are supported) */
    if (host_target_connected(target)) {
        log_msg("Host reconnect: Host already connected");
        rq_remove(target);
        return;
    }

    if (g_host_reconnect_active && !is_null_bd_addr(target) && memcmp(g_host_reconnect_target, target, 6) == 0 && g_host_connecting) {
        return;
    }

    /* One loop at a time: the host of a loop replaced by this one waits for its turn instead of
     * being dropped (after a restart the activity's host replaced the other paired host) */
    if (g_host_reconnect_active && !is_null_bd_addr(g_host_reconnect_target) &&
        (is_null_bd_addr(target) || memcmp(g_host_reconnect_target, target, 6) != 0) &&
        !rq_skip_host(g_host_reconnect_target) && rq_push(g_host_reconnect_target)) {
        char qmsg[128];
        snprintf(qmsg, sizeof(qmsg), "Host reconnect: %s waits, ", bd_addr_to_str(g_host_reconnect_target));
        size_t ql = strlen(qmsg);
        snprintf(qmsg + ql, sizeof(qmsg) - ql, "%s first", is_null_bd_addr(target) ? "targets" : bd_addr_to_str(target));
        log_msg(qmsg);
    }
    if (!is_null_bd_addr(target)) rq_remove(target);

    if (!is_null_bd_addr(target)) {
        memcpy(g_host_reconnect_target, target, 6);
    } else {
        memset(g_host_reconnect_target, 0, 6);
    }
    g_host_reconnect_attempts = 0;
    g_host_reconnect_active = true;

    char msg[128];
    snprintf(msg, sizeof(msg), "Starting host reconnect loop for %s (fast retry: 1s interval)",
             is_null_bd_addr(g_host_reconnect_target) ? "targets" : bd_addr_to_str(g_host_reconnect_target));
    log_msg(msg);

    if (!is_null_bd_addr(g_host_reconnect_target)) {
        try_connect_host_addr(g_host_reconnect_target);
    } else {
        try_connect_host();
    }

    btstack_run_loop_remove_timer(&g_host_reconnect_timer);
    btstack_run_loop_set_timer(&g_host_reconnect_timer, 1000);
    btstack_run_loop_set_timer_handler(&g_host_reconnect_timer, host_reconnect_timer_handler);
    btstack_run_loop_add_timer(&g_host_reconnect_timer);
}

void stop_host_reconnect(void) {
    if (g_host_reconnect_active) {
        g_host_reconnect_active = false;
        btstack_run_loop_remove_timer(&g_host_reconnect_timer);
        log_msg("Host reconnect: Loop stopped");
    }
    g_host_connecting = false;
}

static void hog_reconnect_timeout(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (app_state == APP_READY) return;
    if (app_state == APP_CONNECTING) {
        log_msg("Connection attempt timed out (10s); cancelling and resuming scan");
        gap_connect_cancel();
        hog_start_scan();
        return;
    }
    log_msg("Reconnect timer fired: starting scan");
    hog_start_scan();
}

void schedule_reconnect(uint32_t delay_ms) {
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_set_timer(&reconnect_timer, delay_ms);
    btstack_run_loop_set_timer_handler(&reconnect_timer, &hog_reconnect_timeout);
    btstack_run_loop_add_timer(&reconnect_timer);
}

void write_scan_results(bool scanning) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddBoolToObject(root, "scanning", scanning);
    cJSON *devs = cJSON_CreateArray();
    for (int i = 0; i < g_discovered_count; i++) {
        cJSON *d = cJSON_CreateObject();
        const char *addr_str = bd_addr_to_str(g_discovered[i].addr);
        cJSON_AddStringToObject(d, "addr", addr_str);
        cJSON_AddStringToObject(d, "bdaddr", addr_str);
        cJSON_AddStringToObject(d, "name", g_discovered[i].name[0] ? g_discovered[i].name : "Bluetooth Remote");
        cJSON_AddNumberToObject(d, "rssi", g_discovered[i].rssi);
        cJSON_AddBoolToObject(d, "pairing", 1);
        cJSON_AddItemToArray(devs, d);
    }
    cJSON_AddItemToObject(root, "devices", devs);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return;

    FILE *f = fopen(SCAN_RESULT_FILE ".tmp", "w");
    if (f) {
        fputs(out, f);
        fputc('\n', f);
        fclose(f);
        rename(SCAN_RESULT_FILE ".tmp", SCAN_RESULT_FILE);
    }
    free(out);
}

void hog_start_scan(void) {
    bd_addr_t pinned;
    if (get_pinned_target(pinned)) {
        if (is_null_bd_addr(remote_device.addr)) {
            memcpy(remote_device.addr, pinned, 6);
        }
        update_status("standby");
        gap_set_scan_parameters(0, 96, 24); // passive scanning (25% duty cycle): no SCAN_REQ delay, avoids Classic BT paging collision
    } else {
        /* No remote pinned: nothing to look for (a user scan from the web UI runs its own; a
         * plugin's adv_listen keeps the scan until it ends) */
        if (!g_adv_listen_active) gap_stop_scan();
        app_state = APP_IDLE;
        update_status("standby");
        return;
    }
    app_state = APP_SCANNING;
    gap_start_scan();
}

void forget_ble_remote(void) {
    bd_addr_t addr;
    bool have = get_pinned_target(addr) != 0;
    if (!have && !is_null_bd_addr(remote_device.addr)) {
        memcpy(addr, remote_device.addr, 6);
        have = true;
    }
    g_user_scan_active = false;
    le_pair_window_close();
    btstack_run_loop_remove_timer(&reconnect_timer);
    /* a connection being set up would complete later and pair the forgotten remote again */
    if (app_state == APP_CONNECTING) gap_connect_cancel();
    gap_stop_scan();
    unlink(TARGET_FILE);
    if (have) {
        gap_delete_bonding(BD_ADDR_TYPE_LE_PUBLIC, addr);
        gap_delete_bonding(BD_ADDR_TYPE_LE_RANDOM, addr);
    }
    if (tlv_impl) tlv_impl->delete_tag(&tlv_context, TLV_TAG_HOGD);
    char msg[96];
    snprintf(msg, sizeof(msg), "BLE remote forgotten: %s", have ? bd_addr_to_str(addr) : "none pinned");
    log_msg(msg);
    memset(&remote_device, 0, sizeof(remote_device));
    bd_addr_t still;
    if (get_pinned_target(still)) {
        /* bt_remote_map.json (hub_daemon's) still names one: POST /api/remote-unpair clears it */
        snprintf(msg, sizeof(msg), "BLE remote still pinned by %s: %s", MAP_FILE, bd_addr_to_str(still));
        log_msg(msg);
    }
    app_state = APP_IDLE;
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(connection_handle); /* the disconnect event resumes hog_start_scan(): idle now */
    }
    update_status("standby");
}

void hog_connect(void) {
    /* A plugin's adv_listen has the scan: the remote connects once it ends (a connection
     * attempt stops the scan) */
    if (g_adv_listen_active) return;
    /* nor while a broadcast borrows another address (the connection would use it): the remote
     * advertises again and connects after it */
    if (bt_adv_borrowed_addr()) return;
    log_msg("Connecting to target BLE remote...");
    app_state = APP_CONNECTING;
    update_status("connecting");
    gap_stop_scan();
    gap_connect(remote_device.addr, remote_device.addr_type);
    schedule_reconnect(10000); // 10s connection timeout
}

void hog_start_connect(void) {
    hog_start_scan();
}

void start_user_scan(void) {
    log_msg("User requested BLE scan for remotes in pairing mode...");
    bt_listen_end("a remote scan started"); /* the user's action wins over a plugin's listen */
    g_discovered_count = 0;
    memset(g_discovered, 0, sizeof(g_discovered));
    g_user_scan_active = true;
    g_user_scan_end = time(NULL) + 8;

    btstack_run_loop_remove_timer(&reconnect_timer);
    if (app_state == APP_CONNECTING) gap_connect_cancel();
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(connection_handle);
    }
    write_scan_results(true);

    app_state = APP_SCANNING;
    update_status("scanning");
    gap_set_scan_parameters(1, 48, 48);
    gap_start_scan();
}

void stop_user_scan(void) {
    if (g_user_scan_active) {
        g_user_scan_active = false;
        log_msg("User stopped BLE remote scan");
        write_scan_results(false);
        gap_stop_scan();
        hog_start_connect();
    }
}

void start_user_pair(const char *addr_str) {
    bd_addr_t new_addr;
    if (!parse_bd_addr(addr_str, new_addr)) {
        log_msg("Invalid pair address requested");
        return;
    }
    bt_listen_end("a remote pairing started"); /* the remote must not wait for a plugin's listen */
    char msg[128];
    snprintf(msg, sizeof(msg), "User pair requested for %s (LE pairing window open for %d s)", addr_str, LE_PAIR_WINDOW_SEC);
    log_msg(msg);
    le_pair_window_open(new_addr, le_pair_now_s());

    char tline[32];
    snprintf(tline, sizeof(tline), "%s\n", addr_str);
    if (atomic_file_write_str(TARGET_FILE, tline) != 0) log_msg("Could not save " TARGET_FILE);

    g_user_scan_active = false;
    btstack_run_loop_remove_timer(&reconnect_timer);
    if (app_state == APP_CONNECTING) gap_connect_cancel();
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(connection_handle);
    }
    gap_stop_scan();

    gap_delete_bonding(BD_ADDR_TYPE_LE_PUBLIC, new_addr);
    gap_delete_bonding(BD_ADDR_TYPE_LE_RANDOM, new_addr);

    memcpy(remote_device.addr, new_addr, 6);
    remote_device.addr_type = BD_ADDR_TYPE_LE_PUBLIC;
    for (int i = 0; i < g_discovered_count; i++) {
        if (memcmp(g_discovered[i].addr, new_addr, 6) == 0) {
            remote_device.addr_type = (bd_addr_type_t)g_discovered[i].addr_type;
            break;
        }
    }

    app_state = APP_SCANNING;
    update_status("pairing");
    gap_set_scan_parameters(1, 48, 48);
    gap_start_scan();
}
