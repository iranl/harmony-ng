/*
 * State watcher: turns Hub state changes into events on the bus (hub_events), which the
 * WebSocket sink pushes to the web UIs and the integration.
 *
 * Once a second, whether or not a client is connected (MQTT and other sinks need the events
 * too), it checks:
 *   - Bluetooth: hub_btstack's status file (same JSON as GET /api/bt-status)
 *       -> HUB_EV_BT_STATUS, payload = the /api/bt-status object
 *   - RF remotes: pushed by hub_rf to POST /api/rf/status-changed (state_watch_offer_rf_status),
 *     fetched once from GET /api/rf/status on 127.0.0.1:8092 at start as the baseline
 *       -> HUB_EV_RF_STATUS, payload = the /api/rf/status object
 *   - Configuration files (devices, activities, settings, remote button maps), by mtime/size
 *       -> HUB_EV_CONFIG_CHANGED, payload = {"files":["devices", ...]}
 * Each event is emitted only when the content changed; fields that change on every report
 * (button counters, last-seen times, link counters, battery voltage) do not count as a change.
 * Baselines are taken once at start and never reset: a client that connects later gets the
 * current state from the snapshot, not from a replay.
 * Activity state and progress come from the orchestrator (activity_state, activity_progress).
 */
#include "state_watch.h"
#include "hub_events.h"
#include "config_store.h"
#include "cJSON.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BT_STATUS_FILE "/tmp/hub_btstack_status.json"
#define RF_API_PORT 8092
#define WATCH_INTERVAL_US 1000000

static char *read_small_file(const char *path, size_t max_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = (char *)malloc(max_len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, max_len, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* GET /api/rf/status from hub_rf; returns the parsed body or NULL */
static cJSON *fetch_rf_status(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return NULL;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(RF_API_PORT);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(s);
        return NULL;
    }
    static const char req[] = "GET /api/rf/status HTTP/1.1\r\nHost: 127.0.0.1:8092\r\nConnection: close\r\n\r\n";
    if (send(s, req, sizeof(req) - 1, MSG_NOSIGNAL) <= 0) {
        close(s);
        return NULL;
    }
    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap + 1);
    if (!buf) {
        close(s);
        return NULL;
    }
    ssize_t n;
    while (len < cap && (n = recv(s, buf + len, cap - len, 0)) > 0) len += (size_t)n;
    close(s);
    buf[len] = '\0';
    /* Only a 200 carries the status object; an error reply (e.g. while hub_rf restarts) must not
     * be pushed to the web UIs as remote status */
    if (strncmp(buf, "HTTP/1.1 200", 12) != 0 && strncmp(buf, "HTTP/1.0 200", 12) != 0) {
        free(buf);
        return NULL;
    }
    char *body = strstr(buf, "\r\n\r\n");
    cJSON *root = body ? cJSON_Parse(body + 4) : NULL;
    free(buf);
    return root;
}

/* Fingerprint of a status object without the fields that change on every report */
static char *fingerprint(const cJSON *obj, const char *const *drop_top, const char *const *drop_slot) {
    cJSON *copy = cJSON_Duplicate(obj, 1);
    if (!copy) return NULL;
    for (int i = 0; drop_top && drop_top[i]; i++) cJSON_DeleteItemFromObjectCaseSensitive(copy, drop_top[i]);
    cJSON *slots = cJSON_GetObjectItemCaseSensitive(copy, "slots");
    if (drop_slot && cJSON_IsArray(slots)) {
        cJSON *slot = NULL;
        cJSON_ArrayForEach(slot, slots) {
            for (int i = 0; drop_slot[i]; i++) cJSON_DeleteItemFromObjectCaseSensitive(slot, drop_slot[i]);
            cJSON *bat = cJSON_GetObjectItemCaseSensitive(slot, "battery");
            if (bat) cJSON_DeleteItemFromObjectCaseSensitive(bat, "voltage_mv");
        }
    }
    char *out = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    return out;
}

/* Emit obj (ownership passes to the bus) if its fingerprint differs from *last (updated);
 * otherwise obj is freed. Returns 1 when emitted. */
static int emit_if_changed(cJSON *obj, hub_event_type_t type, char **last,
                           const char *const *drop_top, const char *const *drop_slot) {
    char *fp = fingerprint(obj, drop_top, drop_slot);
    if (!fp) {
        cJSON_Delete(obj);
        return 0;
    }
    if (*last && strcmp(*last, fp) == 0) {
        free(fp);
        cJSON_Delete(obj);
        return 0;
    }
    free(*last);
    *last = fp;
    hub_events_emit(type, obj);
    return 1;
}

typedef struct {
    const char *name;
    const char *path;
    time_t mtime;
    off_t size;
    int seen;
} watched_file_t;

static const watched_file_t k_watched_files[] = {
    { "devices", HARMONY_DEVICES_FILE, 0, 0, 0 },
    { "activities", HARMONY_ACTIVITIES_FILE, 0, 0, 0 },
    { "settings", HARMONY_SETTINGS_FILE, 0, 0, 0 },
    { "remote_map", HARMONY_DIR "/remote_slot1_map.json", 0, 0, 0 },
    { "remote_map", HARMONY_DIR "/remote_slot2_map.json", 0, 0, 0 },
};
#define N_WATCHED_FILES (sizeof(k_watched_files) / sizeof(k_watched_files[0]))

cJSON *state_watch_config_mtimes(void) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    for (size_t i = 0; i < N_WATCHED_FILES; i++) {
        struct stat st;
        double mt = (stat(k_watched_files[i].path, &st) == 0) ? (double)st.st_mtime : 0;
        cJSON *cur = cJSON_GetObjectItemCaseSensitive(obj, k_watched_files[i].name);
        if (cur) {
            if (mt > cur->valuedouble) cJSON_SetNumberValue(cur, mt);
        } else {
            cJSON_AddNumberToObject(obj, k_watched_files[i].name, mt);
        }
    }
    return obj;
}

static const char *const k_rf_drop_slot[] = { "link", NULL };
static char *g_last_rf = NULL;
static pthread_mutex_t g_rf_lock = PTHREAD_MUTEX_INITIALIZER;

int state_watch_offer_rf_status(cJSON *obj) {
    if (!obj) return 0;
    pthread_mutex_lock(&g_rf_lock);
    int emitted = emit_if_changed(obj, HUB_EV_RF_STATUS, &g_last_rf, NULL, k_rf_drop_slot);
    pthread_mutex_unlock(&g_rf_lock);
    return emitted;
}

void bt_status_normalize(cJSON *bt) {
    if (!bt || !cJSON_IsObject(bt)) return;
    if (!cJSON_GetObjectItem(bt, "ok")) cJSON_AddBoolToObject(bt, "ok", 1);
    if (!cJSON_GetObjectItem(bt, "running")) {
        int running = 1;
        const cJSON *pid = cJSON_GetObjectItem(bt, "pid");
        if (cJSON_IsNumber(pid) && pid->valueint > 0) {
            running = (kill((pid_t)pid->valueint, 0) == 0 || errno == EPERM);
        }
        cJSON_AddBoolToObject(bt, "running", running);
    }
    const cJSON *host = cJSON_GetObjectItem(bt, "host");
    if (!cJSON_GetObjectItem(bt, "connected")) {
        int connected = cJSON_IsTrue(cJSON_GetObjectItem(host, "connected"));
        const cJSON *h = NULL;
        cJSON_ArrayForEach(h, cJSON_GetObjectItem(bt, "hosts")) {
            if (cJSON_IsTrue(cJSON_GetObjectItem(h, "connected"))) connected = 1;
        }
        cJSON_AddBoolToObject(bt, "connected", connected);
    }
    if (!cJSON_GetObjectItem(bt, "pairing")) {
        cJSON_AddBoolToObject(bt, "pairing", cJSON_IsTrue(cJSON_GetObjectItem(host, "pairing")));
    }
}

static int s_inotify_fd = -1;

void state_watch_notify_config_changed(const char *name) {
    if (!name || s_inotify_fd >= 0) return;
    cJSON *changed = cJSON_CreateArray();
    if (!changed) return;
    cJSON_AddItemToArray(changed, cJSON_CreateString(name));
    cJSON *msg = cJSON_CreateObject();
    if (!msg) {
        cJSON_Delete(changed);
        return;
    }
    cJSON_AddItemToObject(msg, "files", changed);
    hub_events_emit(HUB_EV_CONFIG_CHANGED, msg);
}

static void *state_watch_thread(void *arg) {
    (void)arg;
    static const char *const bt_drop[] = { "last_button", "last_seen", "updated", NULL };
    char *last_bt = NULL;
    int rf_baseline = 0;

    s_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (s_inotify_fd >= 0) {
        inotify_add_watch(s_inotify_fd, HARMONY_CONFIG_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_ATTRIB);
        inotify_add_watch(s_inotify_fd, HARMONY_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_ATTRIB);
    }

    for (;;) {
        usleep(WATCH_INTERVAL_US);

        char *raw = read_small_file(BT_STATUS_FILE, 16384);
        cJSON *bt = raw ? cJSON_Parse(raw) : NULL;
        free(raw);
        if (bt) {
            bt_status_normalize(bt); /* also catches hub_btstack dying with the file unchanged */
            emit_if_changed(bt, HUB_EV_BT_STATUS, &last_bt, bt_drop, NULL);
        }

        /* RF status: one fetch at start so the snapshot is complete before hub_rf's first push
         * (hub_rf also pushes right after it starts); then pushes only */
        if (!rf_baseline) {
            pthread_mutex_lock(&g_rf_lock);
            int have = (g_last_rf != NULL);
            pthread_mutex_unlock(&g_rf_lock);
            if (have) {
                rf_baseline = 1;
            } else {
                cJSON *rf = fetch_rf_status();
                if (rf) {
                    state_watch_offer_rf_status(rf);
                    rf_baseline = 1;
                }
            }
        }

        if (s_inotify_fd >= 0) {
            char ibuf[2048] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t len;
            cJSON *changed = NULL;
            while ((len = read(s_inotify_fd, ibuf, sizeof(ibuf))) > 0) {
                for (char *ptr = ibuf; ptr < ibuf + len; ) {
                    const struct inotify_event *ev = (const struct inotify_event *)ptr;
                    if (ev->len > 0) {
                        const char *name = NULL;
                        if (strcmp(ev->name, "devices.json") == 0) name = "devices";
                        else if (strcmp(ev->name, "activities.json") == 0) name = "activities";
                        else if (strcmp(ev->name, "settings.json") == 0) name = "settings";
                        else if (strcmp(ev->name, "remote_slot1_map.json") == 0 || strcmp(ev->name, "remote_slot2_map.json") == 0) name = "remote_map";
                        if (name) {
                            if (!changed) changed = cJSON_CreateArray();
                            int dup = 0;
                            cJSON *it = NULL;
                            cJSON_ArrayForEach(it, changed) {
                                if (cJSON_IsString(it) && strcmp(it->valuestring, name) == 0) dup = 1;
                            }
                            if (!dup) cJSON_AddItemToArray(changed, cJSON_CreateString(name));
                        }
                    }
                    ptr += sizeof(struct inotify_event) + ev->len;
                }
            }
            if (changed) {
                cJSON *msg = cJSON_CreateObject();
                cJSON_AddItemToObject(msg, "files", changed);
                hub_events_emit(HUB_EV_CONFIG_CHANGED, msg);
            }
        }
    }
    return NULL;
}

int state_watch_start(void) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    int rc = pthread_create(&th, &attr, state_watch_thread, NULL);
    pthread_attr_destroy(&attr);
    return rc == 0 ? 0 : -1;
}
