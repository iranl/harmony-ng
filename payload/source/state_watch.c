/*
 * State watcher: pushes Hub state changes to the web UIs over the existing WebSocket, so that
 * pages update without a reload.
 *
 * Once a second, and only while at least one WebSocket client is connected, it checks:
 *   - Bluetooth: hub_btstack's status file (same JSON as GET /api/bt-status)
 *       -> {"type":"bt_status", ...the /api/bt-status object...}
 *   - RF remotes: hub_rf's GET /api/rf/status on 127.0.0.1:8092
 *       -> {"type":"rf_status", ...the /api/rf/status object...}
 *   - Configuration files (devices, activities, settings, remote button maps), by mtime/size
 *       -> {"type":"config_changed","files":["devices", ...]}
 * Each message is sent only when its content changed; fields that change on every report
 * (button counters, last-seen times, link counters, battery voltage) do not count as a change.
 * Activity progress is already pushed by the orchestrator ("activity_progress").
 */
#include "state_watch.h"
#include "ws_server.h"
#include "config_store.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

/* Broadcast obj with "type" set, if its fingerprint differs from *last (which is updated) */
static void push_if_changed(cJSON *obj, const char *type, char **last,
                            const char *const *drop_top, const char *const *drop_slot) {
    char *fp = fingerprint(obj, drop_top, drop_slot);
    if (!fp) return;
    if (*last && strcmp(*last, fp) == 0) {
        free(fp);
        return;
    }
    free(*last);
    *last = fp;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, "type");
    cJSON_AddStringToObject(obj, "type", type);
    char *msg = cJSON_PrintUnformatted(obj);
    if (msg) {
        ws_broadcast_text(msg);
        free(msg);
    }
}

typedef struct {
    const char *name;
    const char *path;
    time_t mtime;
    off_t size;
    int seen;
} watched_file_t;

static void *state_watch_thread(void *arg) {
    (void)arg;
    static const char *const bt_drop[] = { "last_button", "last_seen", "updated", NULL };
    static const char *const rf_drop_slot[] = { "link", NULL };
    watched_file_t files[] = {
        { "devices", HARMONY_DEVICES_FILE, 0, 0, 0 },
        { "activities", HARMONY_ACTIVITIES_FILE, 0, 0, 0 },
        { "settings", HARMONY_SETTINGS_FILE, 0, 0, 0 },
        { "remote_map", HARMONY_DIR "/remote_slot1_map.json", 0, 0, 0 },
        { "remote_map", HARMONY_DIR "/remote_slot2_map.json", 0, 0, 0 },
    };
    const size_t nfiles = sizeof(files) / sizeof(files[0]);
    char *last_bt = NULL, *last_rf = NULL;
    int had_clients = 0;

    for (;;) {
        usleep(WATCH_INTERVAL_US);
        int fds[WS_MAX_CLIENTS];
        int clients = ws_get_clients(fds, WS_MAX_CLIENTS);
        if (clients <= 0) {
            had_clients = 0;
            continue;
        }
        if (!had_clients) {
            /* A client (re)connected: it loads the state itself, so start from the current
             * state instead of sending it everything again */
            free(last_bt);
            free(last_rf);
            last_bt = last_rf = NULL;
        }

        char *raw = read_small_file(BT_STATUS_FILE, 16384);
        cJSON *bt = raw ? cJSON_Parse(raw) : NULL;
        free(raw);
        if (bt) {
            if (!had_clients) {
                last_bt = fingerprint(bt, bt_drop, NULL);
            } else {
                push_if_changed(bt, "bt_status", &last_bt, bt_drop, NULL);
            }
            cJSON_Delete(bt);
        }

        cJSON *rf = fetch_rf_status();
        if (rf) {
            if (!had_clients) {
                last_rf = fingerprint(rf, NULL, rf_drop_slot);
            } else {
                push_if_changed(rf, "rf_status", &last_rf, NULL, rf_drop_slot);
            }
            cJSON_Delete(rf);
        }

        cJSON *changed = NULL;
        for (size_t i = 0; i < nfiles; i++) {
            struct stat st;
            int ok = (stat(files[i].path, &st) == 0);
            time_t mt = ok ? st.st_mtime : 0;
            off_t sz = ok ? st.st_size : -1;
            if (files[i].seen && (mt != files[i].mtime || sz != files[i].size) && had_clients) {
                if (!changed) changed = cJSON_CreateArray();
                int dup = 0;
                cJSON *it = NULL;
                cJSON_ArrayForEach(it, changed) {
                    if (cJSON_IsString(it) && strcmp(it->valuestring, files[i].name) == 0) dup = 1;
                }
                if (!dup) cJSON_AddItemToArray(changed, cJSON_CreateString(files[i].name));
            }
            files[i].mtime = mt;
            files[i].size = sz;
            files[i].seen = 1;
        }
        if (changed) {
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "config_changed");
            cJSON_AddItemToObject(msg, "files", changed);
            char *txt = cJSON_PrintUnformatted(msg);
            cJSON_Delete(msg);
            if (txt) {
                ws_broadcast_text(txt);
                free(txt);
            }
        }
        had_clients = 1;
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
