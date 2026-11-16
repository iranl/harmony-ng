#define _GNU_SOURCE
#include "http_api_devices.h"
#include <errno.h>
#include "hub_cmd.h"
#include "hub_mqtt.h"
#include "http_server.h"
#include "http_util.h"
#include "config_store.h"
#include "resource_cache.h"
#include "hw_action.h"
#include "ir_i2s.h"
#include "config_sanitize.h"
#include "cJSON.h"
#include "bt_common.h"
#include "state_watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>

/* The IR port mask of a device (its controlPort, 0..7), or all ports when it has none */
static uint8_t device_ir_ports(const char *dev_id) {
    uint8_t ports = 7;
    if (!dev_id || !dev_id[0]) return ports;
    cfg_snap_t *snap = NULL;
    const cJSON *root = config_store_devices_acquire(&snap);
    if (!root) return ports;
    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    cJSON *d = NULL;
    cJSON_ArrayForEach(d, devs) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
        char id[32] = {0};
        if (jid && cJSON_IsString(jid) && jid->valuestring) snprintf(id, sizeof(id), "%s", jid->valuestring);
        else if (jid && cJSON_IsNumber(jid)) snprintf(id, sizeof(id), "%ld", (long)jid->valuedouble);
        if (strcmp(id, dev_id) != 0) continue;
        cJSON *cp = cJSON_GetObjectItemCaseSensitive(d, "controlPort");
        if (cp && cJSON_IsNumber(cp) && cp->valueint >= 0 && cp->valueint <= 7) ports = (uint8_t)cp->valueint;
        break;
    }
    config_store_release(&snap);
    return ports;
}

/* Counts the P/S durations of a learned raw string (F<carrier hex>P<hex>S<hex>...) and adds them up */
static void learned_stats(const char *raw, int *pulses, unsigned long *total_us) {
    *pulses = 0;
    *total_us = 0;
    for (const char *p = raw; p && *p; p++) {
        if (*p == 'P' || *p == 'S') {
            char *end = NULL;
            unsigned long v = strtoul(p + 1, &end, 16);
            (*pulses)++;
            *total_us += v;
            if (end) p = end - 1;
        }
    }
}

/* A timing field of the device save body as an int: a JSON number or a numeric string.
 * Returns 0 for any other type (null, bool, array, object), whose valuestring is NULL. */
static int device_int_field(const cJSON *j, int *out) {
    if (cJSON_IsNumber(j)) {
        double d = j->valuedouble;
        if (d != d) return 0;
        if (d > 2147483647.0) d = 2147483647.0;
        if (d < -2147483648.0) d = -2147483648.0;
        *out = (int)d;
        return 1;
    }
    if (cJSON_IsString(j) && j->valuestring) {
        *out = atoi(j->valuestring);
        return 1;
    }
    return 0;
}

/* Every timing field present in the body has a usable type */
static int device_int_fields_valid(const cJSON *p) {
    static const char *const keys[] = {
        "interDeviceDelay", "interKeyDelay", "holdInterKeyDelay", "powerOnDelay",
        "btReconnectDurationSec", "btReconnectDelaySec"
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(p, keys[i]);
        int v;
        if (j && !device_int_field(j, &v)) return 0;
    }
    return 1;
}

/* The send delays present in the body are within 0..DEVICE_DELAY_MAX_MS: the sender sleeps them
 * out before the next command, and hw_action.c holds its command lock meanwhile */
static int device_delay_fields_in_range(const cJSON *p) {
    static const char *const keys[] = { "interDeviceDelay", "interKeyDelay", "holdInterKeyDelay" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(p, keys[i]);
        int v;
        if (j && device_int_field(j, &v) && (v < 0 || v > DEVICE_DELAY_MAX_MS)) return 0;
    }
    return 1;
}

/* Device references: what still uses a device, so that a delete can be refused (a deleted device
 * left its steps, key bindings and trigger behind, and they were skipped in silence). */
#define DEVICE_USER_KEYS_MAX 32

/* j (string or number) names the device id */
static int names_device(const cJSON *j, const char *dev_id) {
    char buf[64];
    if (cJSON_IsString(j) && j->valuestring) return strcmp(j->valuestring, dev_id) == 0;
    if (cJSON_IsNumber(j)) {
        snprintf(buf, sizeof(buf), "%ld", (long)j->valuedouble);
        return strcmp(buf, dev_id) == 0;
    }
    return 0;
}

/* Some "deviceId" (or "targetDevice") below j names the device, or a "deviceIds" list holds it */
static int tree_uses_device(const cJSON *j, const char *dev_id) {
    const cJSON *c;
    cJSON_ArrayForEach(c, j) {
        if (c->string && (strcmp(c->string, "deviceId") == 0 || strcmp(c->string, "targetDevice") == 0) &&
            names_device(c, dev_id)) return 1;
        if (c->string && strcmp(c->string, "deviceIds") == 0 && cJSON_IsArray(c)) {
            const cJSON *e;
            cJSON_ArrayForEach(e, c) if (names_device(e, dev_id)) return 1;
        }
        if ((cJSON_IsObject(c) || cJSON_IsArray(c)) && tree_uses_device(c, dev_id)) return 1;
    }
    return 0;
}

/* A section of a remote map that no longer has its activity ("activities/<id>") or its device
 * ("devices/<id>", a page left from before deleted devices' pages were removed): no editor shows
 * it, so its bindings cannot be changed and must not hold a device. "-1" and "default" are the
 * fallback sections. */
static int stale_section(const char *path, const cJSON *act_ids, const cJSON *dev_ids) {
    const char *id;
    const cJSON *ids;
    if (strncmp(path, "activities/", 11) == 0) {
        id = path + 11;
        ids = act_ids;
        if (strcmp(id, "-1") == 0 || strcmp(id, "default") == 0) return 0;
    } else if (strncmp(path, "devices/", 8) == 0) {
        id = path + 8;
        ids = dev_ids;
    } else {
        return 0;
    }
    if (strchr(id, '/')) return 0;
    return cJSON_GetObjectItemCaseSensitive(ids, id) == NULL; /* ids: id -> name */
}

/* Key bindings of a remote map that name the device, as "activities/10/VolumeUp", counted per
 * section ("activities/10", "devices/5") in groups. The map's own page for the device
 * ("devices/<id>") belongs to the device and is not counted, nor are the sections of activities
 * that no longer exist. */
static void map_keys_using(const cJSON *j, const char *dev_id, const char *path, int depth,
                           const cJSON *act_ids, const cJSON *dev_ids, cJSON *keys, int *count,
                           const char *group, cJSON *groups) {
    const cJSON *c;
    int idx = 0;
    cJSON_ArrayForEach(c, j) {
        char sub[192];
        if (c->string) snprintf(sub, sizeof(sub), "%s%s%s", path, path[0] ? "/" : "", c->string);
        else snprintf(sub, sizeof(sub), "%s/%d", path, idx);
        idx++;
        if (depth == 1 && strcmp(path, "devices") == 0 && c->string && strcmp(c->string, dev_id) == 0) continue;
        if (depth == 1 && stale_section(sub, act_ids, dev_ids)) continue;
        if (c->string && (strcmp(c->string, "deviceId") == 0 || strcmp(c->string, "targetDevice") == 0) &&
            names_device(c, dev_id)) {
            if (*count < DEVICE_USER_KEYS_MAX) cJSON_AddItemToArray(keys, cJSON_CreateString(path));
            (*count)++;
            const char *g = group ? group : path;
            cJSON *n = cJSON_GetObjectItemCaseSensitive(groups, g);
            if (n) cJSON_SetNumberValue(n, n->valuedouble + 1);
            else cJSON_AddNumberToObject(groups, g, 1);
            continue;
        }
        if (cJSON_IsObject(c) || cJSON_IsArray(c)) {
            map_keys_using(c, dev_id, sub, depth + 1, act_ids, dev_ids, keys, count, depth == 1 ? sub : group, groups);
        }
    }
}

static void add_map_users(cJSON *users, const char *file, const char *type, int slot, const char *dev_id,
                          const cJSON *act_ids, const cJSON *dev_ids) {
    size_t len = 0;
    char *txt = read_file_text_alloc(config_store_path(file), 512 * 1024, &len);
    cJSON *map = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    if (!map) return;
    cJSON *keys = cJSON_CreateArray();
    cJSON *groups = cJSON_CreateObject();
    int count = 0;
    map_keys_using(map, dev_id, "", 0, act_ids, dev_ids, keys, &count, NULL, groups);
    cJSON_Delete(map);
    if (count == 0) {
        cJSON_Delete(keys);
        cJSON_Delete(groups);
        return;
    }
    /* per section, with its name, for a message people can read: "46 keys in TV (DTT)" */
    cJSON *ga = cJSON_CreateArray();
    const cJSON *g;
    cJSON_ArrayForEach(g, groups) {
        cJSON *o = cJSON_CreateObject();
        const char *sec = g->string;
        if (strncmp(sec, "activities/", 11) == 0) {
            const char *aid = sec + 11;
            int fallback = strcmp(aid, "-1") == 0 || strcmp(aid, "default") == 0;
            cJSON_AddStringToObject(o, "type", fallback ? "no_activity" : "activity");
            cJSON_AddStringToObject(o, "id", aid);
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(act_ids, aid);
            if (cJSON_IsString(nm)) cJSON_AddStringToObject(o, "name", nm->valuestring);
        } else if (strncmp(sec, "devices/", 8) == 0) {
            cJSON_AddStringToObject(o, "type", "device_page");
            cJSON_AddStringToObject(o, "id", sec + 8);
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(dev_ids, sec + 8);
            if (cJSON_IsString(nm)) cJSON_AddStringToObject(o, "name", nm->valuestring);
        } else {
            cJSON_AddStringToObject(o, "type", "other");
            cJSON_AddStringToObject(o, "id", sec);
        }
        cJSON_AddNumberToObject(o, "count", g->valuedouble);
        cJSON_AddItemToArray(ga, o);
    }
    cJSON_Delete(groups);
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "type", type);
    if (slot > 0) cJSON_AddNumberToObject(u, "slot", slot);
    cJSON_AddNumberToObject(u, "count", count);
    cJSON_AddItemToObject(u, "keys", keys);
    cJSON_AddItemToObject(u, "groups", ga);
    cJSON_AddItemToArray(users, u);
}

static void id_text(const cJSON *j, char *out, size_t out_sz) {
    out[0] = '\0';
    if (cJSON_IsString(j) && j->valuestring) snprintf(out, out_sz, "%s", j->valuestring);
    else if (cJSON_IsNumber(j)) snprintf(out, out_sz, "%ld", (long)j->valuedouble);
}

/* Which touchscreen remote slots hub_rf reports active (GET /api/rf/status, "slots"[].active),
 * into paired[1..2]; stays 0 when hub_rf does not answer */
static int rf_slots_active(int paired[3]) {
    paired[1] = paired[2] = 0;
    cJSON *st = state_watch_fetch_rf_status();
    const cJSON *slots = st ? cJSON_GetObjectItemCaseSensitive(st, "slots") : NULL;
    int answered = cJSON_IsArray(slots);
    const cJSON *sl;
    cJSON_ArrayForEach(sl, slots) {
        const cJSON *jn = cJSON_GetObjectItemCaseSensitive(sl, "slot");
        int k = cJSON_IsNumber(jn) ? jn->valueint : 0;
        if (k >= 1 && k <= 2) paired[k] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sl, "active"));
    }
    cJSON_Delete(st);
    return answered;
}

/* The slot's remote_slot<N>_version.json names a remote (remote_id or skin; an unpair clears both
 * and keeps the file): require_remote. Else: the file exists. */
static int slot_version_file_counts(int slot, int require_remote) {
    char name[40];
    snprintf(name, sizeof(name), "remote_slot%d_version.json", slot);
    if (!require_remote) return access(config_store_path(name), F_OK) == 0;
    size_t len = 0;
    char *txt = read_file_text_alloc(config_store_path(name), 8192, &len);
    cJSON *v = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    const cJSON *rid = v ? cJSON_GetObjectItemCaseSensitive(v, "remote_id") : NULL;
    const cJSON *skin = v ? cJSON_GetObjectItemCaseSensitive(v, "skin") : NULL;
    int named = (cJSON_IsString(rid) && rid->valuestring[0]) || (cJSON_IsNumber(skin) && skin->valueint != 0);
    cJSON_Delete(v);
    return named;
}

/* Key bindings that use the device, in the maps of paired touchscreen slots and of the Bluetooth
 * remote. A slot counts when hub_rf reports it active, or, not active, when its version file still
 * names a remote (remote_id or skin: an unpair clears them); when hub_rf does not answer, when its
 * version file exists. A map of any other slot (an old file, an unpaired remote) cannot be opened
 * by the legacy editor and does not count. Sections of activities and pages of devices that no longer exist
 * do not count either. Read without the configuration lock (the maps are not under it). */
static cJSON *device_map_users(const char *dev_id) {
    cJSON *users = cJSON_CreateArray();
    cJSON *act_ids = cJSON_CreateObject(); /* id -> name */
    cJSON *dev_ids = cJSON_CreateObject();
    cJSON *devs_root = config_store_devices_copy();
    const cJSON *devs = devs_root ? cJSON_GetObjectItemCaseSensitive(devs_root, "devices") : (const cJSON *)0;
    const cJSON *d;
    cJSON_ArrayForEach(d, devs) {
        char did[64];
        id_text(cJSON_GetObjectItemCaseSensitive(d, "id"), did, sizeof(did));
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(d, "name");
        if (did[0] && !cJSON_GetObjectItemCaseSensitive(dev_ids, did)) {
            cJSON_AddStringToObject(dev_ids, did, cJSON_IsString(nm) && nm->valuestring ? nm->valuestring : did);
        }
    }
    cJSON_Delete(devs_root);
    cJSON *acts_root = config_store_activities_copy();
    const cJSON *acts = acts_root ? (cJSON_IsArray(acts_root) ? acts_root
                                     : cJSON_GetObjectItemCaseSensitive(acts_root, "activities")) : NULL;
    const cJSON *a;
    cJSON_ArrayForEach(a, acts) {
        char aid[64];
        id_text(cJSON_GetObjectItemCaseSensitive(a, "id"), aid, sizeof(aid));
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(a, "name");
        if (aid[0] && !cJSON_GetObjectItemCaseSensitive(act_ids, aid)) {
            cJSON_AddStringToObject(act_ids, aid, cJSON_IsString(nm) && nm->valuestring ? nm->valuestring : aid);
        }
    }
    cJSON_Delete(acts_root);

    int paired[3];
    int answered = rf_slots_active(paired);
    for (int slot = 1; slot <= 2; slot++) {
        char name[40];
        if (!paired[slot] && !slot_version_file_counts(slot, answered)) continue;
        snprintf(name, sizeof(name), "remote_slot%d_map.json", slot);
        add_map_users(users, name, "remote_map", slot, dev_id, act_ids, dev_ids);
    }
    add_map_users(users, "bt_remote_map.json", "bt_remote_map", 0, dev_id, act_ids, dev_ids);
    cJSON_Delete(act_ids);
    cJSON_Delete(dev_ids);
    return users;
}

/* What uses the device in the configuration: activities (sequences, device lists) and other
 * devices (Bluetooth trigger), appended to users. 0 when the device does not exist, else 1.
 * Call with config_store_lock() held. */
static int device_config_users(const char *dev_id, cJSON *users) {
    cJSON *devs_root = config_store_devices_copy();
    const cJSON *devs = devs_root ? cJSON_GetObjectItemCaseSensitive(devs_root, "devices") : NULL;
    int exists = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, devs) {
        char did[64];
        id_text(cJSON_GetObjectItemCaseSensitive(d, "id"), did, sizeof(did));
        if (strcmp(did, dev_id) == 0) {
            exists = 1;
            continue;
        }
        if (names_device(cJSON_GetObjectItemCaseSensitive(d, "btTriggerDeviceId"), dev_id)) {
            const cJSON *jn = cJSON_GetObjectItemCaseSensitive(d, "name");
            cJSON *u = cJSON_CreateObject();
            cJSON_AddStringToObject(u, "type", "device");
            cJSON_AddStringToObject(u, "id", did);
            cJSON_AddStringToObject(u, "name", cJSON_IsString(jn) && jn->valuestring ? jn->valuestring : "");
            cJSON_AddStringToObject(u, "field", "btTriggerDeviceId");
            cJSON_AddItemToArray(users, u);
        }
    }
    cJSON_Delete(devs_root);
    if (!exists) return 0;

    cJSON *acts_root = config_store_activities_copy();
    const cJSON *acts = acts_root ? (cJSON_IsArray(acts_root) ? acts_root
                                     : cJSON_GetObjectItemCaseSensitive(acts_root, "activities")) : NULL;
    const cJSON *a;
    cJSON_ArrayForEach(a, acts) {
        cJSON *fields = cJSON_CreateArray();
        const cJSON *f;
        cJSON_ArrayForEach(f, a) {
            if (!f->string) continue;
            int used = 0;
            if (strcmp(f->string, "deviceIds") == 0 && cJSON_IsArray(f)) {
                const cJSON *e;
                cJSON_ArrayForEach(e, f) if (names_device(e, dev_id)) used = 1;
            } else if (cJSON_IsObject(f) || cJSON_IsArray(f)) {
                used = tree_uses_device(f, dev_id);
            } else if (strcmp(f->string, "deviceId") == 0) {
                used = names_device(f, dev_id);
            }
            if (used) cJSON_AddItemToArray(fields, cJSON_CreateString(f->string));
        }
        if (cJSON_GetArraySize(fields) == 0) {
            cJSON_Delete(fields);
            continue;
        }
        char aid[64];
        id_text(cJSON_GetObjectItemCaseSensitive(a, "id"), aid, sizeof(aid));
        const cJSON *jn = cJSON_GetObjectItemCaseSensitive(a, "name");
        cJSON *u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "type", "activity");
        cJSON_AddStringToObject(u, "id", aid);
        cJSON_AddStringToObject(u, "name", cJSON_IsString(jn) && jn->valuestring ? jn->valuestring : "");
        cJSON_AddItemToObject(u, "fields", fields);
        cJSON_AddItemToArray(users, u);
    }
    cJSON_Delete(acts_root);
    return 1;
}

/* The ids of the configured devices or activities ("devices" or "activities"), as strings */
/* NULL when the configuration cannot be read (no memory): nothing may be removed then */
static cJSON *configured_ids(const char *what) {
    int devices = strcmp(what, "devices") == 0;
    cJSON *root = devices ? config_store_devices_copy() : config_store_activities_copy();
    if (!root) return NULL;
    cJSON *ids = cJSON_CreateArray();
    const cJSON *list = !root ? (const cJSON *)0 : cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, what);
    const cJSON *it;
    cJSON_ArrayForEach(it, list) {
        char id[64];
        id_text(cJSON_GetObjectItemCaseSensitive(it, "id"), id, sizeof(id));
        if (id[0]) cJSON_AddItemToArray(ids, cJSON_CreateString(id));
    }
    cJSON_Delete(root);
    return ids;
}

static int id_listed(const cJSON *ids, const char *id) {
    const cJSON *e;
    cJSON_ArrayForEach(e, ids) {
        if (cJSON_IsString(e) && strcmp(e->valuestring, id) == 0) return 1;
    }
    return 0;
}

static volatile int s_drop_busy; /* one rewrite of the maps at a time (they share a temp name) */

int remote_maps_drop_section(const char *section, const char *id) {
    if (!section || !id || !id[0]) return 0;
    /* only once the configuration no longer has it (its save may have failed) */
    cJSON *ids = configured_ids(section);
    if (!ids || id_listed(ids, id)) {
        cJSON_Delete(ids);
        return 0;
    }
    /* No futex on the Hub: wait for another delete's rewrite with a flag and short sleeps (5 s) */
    int waited_ms = 0;
    while (__sync_lock_test_and_set(&s_drop_busy, 1)) {
        if (waited_ms >= 5000) {
            printf("[-] Maps busy: %s/%s left in the remote maps\n", section, id);
            cJSON_Delete(ids);
            return 0;
        }
        usleep(2000);
        waited_ms += 2;
    }
    static const char *const files[] = { "remote_slot1_map.json", "remote_slot2_map.json", "bt_remote_map.json" };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s", config_store_path(files[i]));
        char stale[300];
        snprintf(stale, sizeof(stale), "%s.drop", path);
        unlink(stale); /* left by a power cut during an earlier rewrite */
        /* The map editors save a whole file with no lock; a save in between is not overwritten:
         * the file is read again when it changed while it was being edited here (a few tries) */
        for (int attempt = 0; attempt < 3; attempt++) {
            struct stat before, after;
            if (stat(path, &before) != 0) break;
            size_t len = 0;
            char *txt = read_file_text_alloc(path, 512 * 1024, &len);
            cJSON *map = txt ? cJSON_Parse(txt) : NULL;
            free(txt);
            cJSON *sec = map ? cJSON_GetObjectItemCaseSensitive(map, section) : NULL;
            int removed = 0;
            if (cJSON_IsObject(sec)) {
                for (cJSON *c = sec->child, *next; c; c = next) {
                    next = c->next;
                    /* this id, and (devices) the pages of devices deleted before pages went with them */
                    if (c->string && (strcmp(c->string, id) == 0 || (strcmp(section, "devices") == 0 && !id_listed(ids, c->string)))) {
                        cJSON_Delete(cJSON_DetachItemViaPointer(sec, c));
                        removed++;
                    }
                }
            }
            if (!removed) {
                cJSON_Delete(map);
                break;
            }
            /* compact, as the editors save it: the readers take maps up to 200000 bytes */
            char *out = cJSON_PrintUnformatted(map);
            cJSON_Delete(map);
            /* own temp file (one name per map: rewrites are serialized, and a file left by a power
             * cut is replaced by the next one), written and synced, then the map is compared once
             * more right before the rename: an editor save (a rename too: new inode) in between is
             * read again */
            char tmp[300];
            snprintf(tmp, sizeof(tmp), "%s.drop", path);
            int fd = out ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
            size_t olen = out ? strlen(out) : 0, done = 0;
            while (fd >= 0 && done < olen) {
                ssize_t w = write(fd, out + done, olen - done);
                if (w <= 0) break;
                done += (size_t)w;
            }
            int written = fd >= 0 && done == olen && fsync(fd) == 0;
            if (fd >= 0) close(fd);
            free(out);
            if (!written) {
                unlink(tmp);
                printf("[-] Could not remove %s/%s from %s\n", section, id, files[i]);
                break;
            }
            if (stat(path, &after) != 0 || after.st_mtime != before.st_mtime || after.st_size != before.st_size ||
                after.st_ino != before.st_ino) {
                unlink(tmp);
                continue; /* saved meanwhile: read it again */
            }
            if (rename(tmp, path) != 0) {
                unlink(tmp);
                printf("[-] Could not remove %s/%s from %s\n", section, id, files[i]);
            } else {
                printf("[*] Removed %d %s section(s) from %s\n", removed, section, files[i]);
                if (i == 2) hub_send_btstack_cmd("reload_map");
            }
            break;
        }
    }
    cJSON_Delete(ids);
    __sync_lock_release(&s_drop_busy);
    return 1;
}

static pthread_mutex_t s_capture_lock = PTHREAD_MUTEX_INITIALIZER;

int http_api_devices_handle(int client_fd, const char *method, const char *clean_path,
                            const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    /* Route: GET /api/inventory */
    if (strcmp(clean_path, "/api/inventory") == 0 && strcmp(method, "GET") == 0) {
        cfg_snap_t *snap = NULL;
        const cJSON *devs = config_store_devices_acquire(&snap);
        if (devs) {
            send_cjson_resp(client_fd, 200, (cJSON *)devs); /* printed only */
            config_store_release(&snap);
        } else {
            send_json_error(client_fd, 404, "DeviceList not found");
        }
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/devices */
    if (strcmp(clean_path, "/api/devices") == 0 && strcmp(method, "GET") == 0) {
        cfg_snap_t *snap = NULL;
        const cJSON *root = config_store_devices_acquire(&snap);
        if (root) {
            send_cjson_resp(client_fd, 200, (cJSON *)root); /* printed only */
            config_store_release(&snap);
        } else {
            send_json_error(client_fd, 404, "Devices not found");
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/pronto-blast */
    if (strcmp(clean_path, "/api/pronto-blast") == 0 && strcmp(method, "POST") == 0) {
        /* the code whole: a 511-character copy cut longer Pronto codes and sent the cut one */
        char *pronto = NULL;
        uint8_t ports = IR_PORT_ALL;
        uint8_t repeats = 3;

        cJSON *req_j = body ? cJSON_Parse(body) : NULL;
        if (req_j) {
            cJSON *jp = cJSON_GetObjectItemCaseSensitive(req_j, "pronto");
            if (jp && cJSON_IsString(jp) && jp->valuestring && jp->valuestring[0]) pronto = strdup(jp->valuestring);

            cJSON *jport = cJSON_GetObjectItemCaseSensitive(req_j, "ports");
            if (jport && cJSON_IsNumber(jport)) ports = (uint8_t)jport->valuedouble;
            else if (jport && cJSON_IsString(jport) && jport->valuestring) ports = (uint8_t)atoi(jport->valuestring);

            cJSON *jrep = cJSON_GetObjectItemCaseSensitive(req_j, "repeats");
            if (jrep && cJSON_IsNumber(jrep)) repeats = (uint8_t)jrep->valuedouble;
            else if (jrep && cJSON_IsString(jrep) && jrep->valuestring) repeats = (uint8_t)atoi(jrep->valuestring);

            cJSON_Delete(req_j);
        }

        if (!pronto) {
            send_json_error(client_fd, 400, "Missing pronto parameter");
            close(client_fd);
            return 1;
        }

        int rc = hw_ir_send_pronto(pronto, ports, repeats);
        free(pronto);
        if (rc == 0) {
            cJSON *suc = cJSON_CreateObject();
            cJSON_AddBoolToObject(suc, "success", 1);
            send_cjson_resp(client_fd, 200, suc);
            cJSON_Delete(suc);
        } else {
            send_json_error(client_fd, 500, "IR send failed");
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/ir-raw: carrier + microsecond timings (spec 3.11); the validation and
     * the send live in hub_cmd_ir_raw, shared with the MQTT command channel */
    if (strcmp(clean_path, "/api/ir-raw") == 0 && strcmp(method, "POST") == 0) {
        cJSON *req_j = (body && body[0]) ? cJSON_Parse(body) : NULL;
        cJSON *resp = NULL;
        int code = hub_cmd_ir_raw(req_j, &resp);
        cJSON_Delete(req_j);
        if (!resp) resp = cJSON_CreateObject();
        send_cjson_resp(client_fd, code, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }
#ifdef TEST_HARNESS
    /* Harness only: the arguments of the last hw_ir_send_raw() call */
    if (strcmp(clean_path, "/api/test/ir-raw-last") == 0 && strcmp(method, "GET") == 0) {
        cJSON *o = hw_test_last_raw_json();
        send_cjson_resp(client_fd, 200, o);
        cJSON_Delete(o);
        close(client_fd);
        return 1;
    }
    /* Harness only: shared configuration trees not released yet (a missed release leaks a tree) */
    if (strcmp(clean_path, "/api/test/config-snapshots-held") == 0 && strcmp(method, "GET") == 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "held", config_store_snapshots_held());
        send_cjson_resp(client_fd, 200, o);
        cJSON_Delete(o);
        close(client_fd);
        return 1;
    }
#endif

    /* Route: POST /api/ir-send or POST /ir/send */
    if ((strcmp(clean_path, "/api/ir-send") == 0 || strcmp(clean_path, "/ir/send") == 0) && strcmp(method, "POST") == 0) {
        char dev_id[64] = {0};
        char cmd_name[128] = {0};
        char *pronto_buf = NULL; /* the code whole (a 2047-character copy cut longer ones) */
        int port = 7;
        int repeats = 1;
        int btn_action = HW_BTN_ACTION_TAP;
        cJSON *req_j = body ? parse_request_params(body) : NULL;
        if (req_j) {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(req_j, "deviceId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(req_j, "device_id");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(req_j, "device");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(req_j, "id");
            if (jid && cJSON_IsString(jid) && jid->valuestring) strncpy(dev_id, jid->valuestring, sizeof(dev_id) - 1);
            else if (jid && cJSON_IsNumber(jid)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jid->valuedouble);

            cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "command");
            if (!jcmd) jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "name");
            if (jcmd && cJSON_IsString(jcmd) && jcmd->valuestring) strncpy(cmd_name, jcmd->valuestring, sizeof(cmd_name) - 1);

            cJSON *jact = cJSON_GetObjectItemCaseSensitive(req_j, "action");
            if (jact && cJSON_IsString(jact) && jact->valuestring) {
                if (strcasecmp(jact->valuestring, "press") == 0 || strcasecmp(jact->valuestring, "down") == 0) {
                    btn_action = HW_BTN_ACTION_PRESS;
                } else if (strcasecmp(jact->valuestring, "hold") == 0 || strcasecmp(jact->valuestring, "repeat") == 0) {
                    btn_action = HW_BTN_ACTION_REPEAT;
                } else if (strcasecmp(jact->valuestring, "release") == 0 || strcasecmp(jact->valuestring, "up") == 0) {
                    btn_action = HW_BTN_ACTION_RELEASE;
                }
            }

            cJSON *jpronto = cJSON_GetObjectItemCaseSensitive(req_j, "pronto");
            if (!jpronto) jpronto = cJSON_GetObjectItemCaseSensitive(req_j, "code");
            if (!jpronto) jpronto = cJSON_GetObjectItemCaseSensitive(req_j, "raw");
            if (jpronto && cJSON_IsString(jpronto) && jpronto->valuestring && jpronto->valuestring[0]) {
                pronto_buf = strdup(jpronto->valuestring);
            }

            cJSON *jport = cJSON_GetObjectItemCaseSensitive(req_j, "port");
            if (jport && cJSON_IsNumber(jport) && jport->valueint > 0) port = jport->valueint;

            cJSON *jrep = cJSON_GetObjectItemCaseSensitive(req_j, "repeats");
            if (jrep && cJSON_IsNumber(jrep) && jrep->valueint > 0) repeats = jrep->valueint;

            cJSON_Delete(req_j);
        }

        int rc = -1;
        if (pronto_buf) {
            rc = hw_ir_send_pronto(pronto_buf, (uint8_t)port, (uint8_t)repeats);
            /* Harmony codes (G:<protocol>:... and the raw timing form F<carrier>P..S..) are not
             * Pronto: the keycode sender takes them */
            if (rc != 0 && (strncmp(pronto_buf, "G:", 2) == 0 || pronto_buf[0] == 'F' || pronto_buf[0] == 'f')) {
                rc = hw_ir_send_harmony_keycode(pronto_buf, (uint8_t)port, (uint8_t)repeats);
            }
            free(pronto_buf);
        } else if (dev_id[0] && cmd_name[0]) {
            rc = hw_device_command_send_action(dev_id, 0, cmd_name, btn_action);
        } else {
            send_json_error(client_fd, 400, "Missing deviceId/command or pronto code");
            close(client_fd);
            return 1;
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", rc == 0);
        if (dev_id[0]) cJSON_AddStringToObject(resp, "deviceId", dev_id);
        if (cmd_name[0]) cJSON_AddStringToObject(resp, "command", cmd_name);
        if (rc != 0) cJSON_AddStringToObject(resp, "error", "IR transmit failed");
        send_cjson_resp(client_fd, rc == 0 ? 200 : 500, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/ir-capture {"enabled": bool}: the IR capture mode (spec 3.14). While it is
     * on, every learned signal is an ir_received event (WebSocket, MQTT <base>/ir/received); the
     * one-shot POST /api/capture answers 409. Shared with the ir_capture command action. */
    if (strcmp(clean_path, "/api/ir-capture") == 0 && strcmp(method, "POST") == 0) {
        cJSON *req_j = (body && body[0]) ? cJSON_Parse(body) : NULL;
        cJSON *resp = NULL;
        int code = hub_cmd_ir_capture(req_j, &resp);
        cJSON_Delete(req_j);
        send_cjson_resp(client_fd, code, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/capture: learn one IR signal from the Hub's IR receiver.
     * Blocks this worker until a signal arrives or the timeout ends (seconds, default 10, 1..30).
     * The capture holds the I2S lock that IR sends take too, so a send waits for it. The result is
     * a Harmony raw string (F<carrier hex>P<us hex>S<us hex>...) that /ir/command saves as the
     * command's code and the IR sender plays back as it is. */
    if (strcmp(clean_path, "/api/capture") == 0 &&
        strcmp(method, "POST") == 0) {
        int timeout_s = 10;
        cJSON *req_j = (body && body[0]) ? parse_request_params(body) : NULL;
        if (!req_j && query && query[0]) req_j = parse_request_params(query);
        if (req_j) {
            cJSON *jt = cJSON_GetObjectItemCaseSensitive(req_j, "timeout");
            if (jt && cJSON_IsNumber(jt)) timeout_s = jt->valueint;
            else if (jt && cJSON_IsString(jt) && jt->valuestring) timeout_s = atoi(jt->valuestring);
            cJSON_Delete(req_j);
        }
        if (timeout_s < 1) timeout_s = 1;
        if (timeout_s > 30) timeout_s = 30;

        if (pthread_mutex_trylock(&s_capture_lock) != 0) {
            send_json_error(client_fd, 409, "An IR capture is already running");
            close(client_fd);
            return 1;
        }
        char raw[8192];
        int rc = ir_i2s_capture(raw, sizeof(raw), (unsigned int)timeout_s);
        pthread_mutex_unlock(&s_capture_lock);

        if (rc == 0) {
            int pulses = 0;
            unsigned long total_us = 0;
            learned_stats(raw, &pulses, &total_us);
            unsigned long carrier = strtoul(raw + 1, NULL, 16);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "raw", raw);
            cJSON_AddStringToObject(resp, "keycode", raw);
            cJSON_AddStringToObject(resp, "mode", "raw");
            cJSON_AddNumberToObject(resp, "carrier_hz", (double)carrier);
            cJSON_AddNumberToObject(resp, "pulses", pulses);
            cJSON_AddNumberToObject(resp, "duration_us", (double)total_us);
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        } else if (rc == -EBUSY) {
            send_json_error(client_fd, 409, "IR hardware is busy (an IR send is running); try again");
        } else if (rc == -11) {
            send_json_error(client_fd, 409, raw[0] ? raw : "IR capture mode is on");
        } else if (rc == -5 || rc == -6) {
            send_json_error(client_fd, 408, raw[0] ? raw : "No IR signal received");
        } else if (rc == -2 || rc == -3 || rc == -7) {
            send_json_error(client_fd, 503, raw[0] ? raw : "IR receiver not available");
        } else {
            send_json_error(client_fd, 500, raw[0] ? raw : "IR capture failed");
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/ir-test-learned: play a learned code once on the device's IR port
     * (fields of the learn wizard: deviceId, keycode or raw) */
    if (strcmp(clean_path, "/api/ir-test-learned") == 0 && strcmp(method, "POST") == 0) {
        char dev_id[64] = {0};
        char *code = NULL; /* the code whole (an 8191-character copy cut longer ones) */
        cJSON *req_j = body ? parse_request_params(body) : NULL;
        if (req_j) {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(req_j, "deviceId");
            if (jid && cJSON_IsString(jid) && jid->valuestring) snprintf(dev_id, sizeof(dev_id), "%s", jid->valuestring);
            else if (jid && cJSON_IsNumber(jid)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jid->valuedouble);
            const char *keys[] = { "keycode", "raw", "code" };
            for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && !code; i++) {
                cJSON *jc = cJSON_GetObjectItemCaseSensitive(req_j, keys[i]);
                if (jc && cJSON_IsString(jc) && jc->valuestring && jc->valuestring[0]) code = strdup(jc->valuestring);
            }
            cJSON_Delete(req_j);
        }
        if (!code) {
            send_json_error(client_fd, 400, "Missing keycode or raw");
            close(client_fd);
            return 1;
        }
        uint8_t ports = device_ir_ports(dev_id);
        int rc = hw_ir_send_harmony_keycode(code, ports, 1);
        free(code);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", rc == 0);
        cJSON_AddNumberToObject(resp, "port", ports);
        if (rc != 0) cJSON_AddStringToObject(resp, "error", rc == -2 ? "Code not understood by the IR encoder" : "IR transmit failed");
        send_cjson_resp(client_fd, rc == 0 ? 200 : (rc == -2 ? 400 : 500), resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/device-commands */
    if (strncmp(clean_path, "/api/device-commands", 20) == 0 && strcmp(method, "GET") == 0) {
        char dev_id[64] = {0};
        if (query) {
            cJSON *qp = parse_request_params(query);
            if (qp) {
                cJSON *jid = cJSON_GetObjectItemCaseSensitive(qp, "deviceId");
                if (!jid) jid = cJSON_GetObjectItemCaseSensitive(qp, "id");
                if (jid && cJSON_IsString(jid)) strncpy(dev_id, jid->valuestring, sizeof(dev_id) - 1);
                cJSON_Delete(qp);
            }
        }
        cJSON *res_cmds = cJSON_CreateArray();
        if (dev_id[0]) {
            cfg_snap_t *snap = NULL;
            const cJSON *root = config_store_devices_acquire(&snap);
            if (root) {
                cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                if (devs && cJSON_IsArray(devs)) {
                    cJSON *di = NULL;
                    cJSON_ArrayForEach(di, devs) {
                        cJSON *jid = cJSON_GetObjectItemCaseSensitive(di, "id");
                        char d_id_str[64] = {0};
                        if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                        else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                        if (strcmp(d_id_str, dev_id) == 0) {
                            cJSON *cmds = cJSON_GetObjectItemCaseSensitive(di, "commands");
                            if (cmds && cJSON_IsArray(cmds)) {
                                cJSON *ci = NULL;
                                cJSON_ArrayForEach(ci, cmds) {
                                    cJSON_AddItemToArray(res_cmds, cJSON_Duplicate(ci, 1));
                                }
                            }
                            break;
                        }
                    }
                }
                config_store_release(&snap);
            }
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddItemToObject(resp, "commands", res_cmds);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/device-mqtt-test: publish the device's MQTT pulse once (the topic
     * template and pulse of the request, else the device's) */
    if (strcmp(clean_path, "/api/device-mqtt-test") == 0 && strcmp(method, "POST") == 0) {
        char dev_id[64] = {0};
        char dev_name[128] = {0};
        char cmd_name[128] = "PowerToggle";
        char req_topic[256] = {0};
        int req_pulse_ms = 1000;

        cJSON *req_j = body ? parse_request_params(body) : NULL;
        if (req_j) {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(req_j, "deviceId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(req_j, "id");
            if (jid && cJSON_IsString(jid)) strncpy(dev_id, jid->valuestring, sizeof(dev_id) - 1);
            else if (jid && cJSON_IsNumber(jid)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jid->valuedouble);

            cJSON *jdev = cJSON_GetObjectItemCaseSensitive(req_j, "device");
            if (jdev && cJSON_IsString(jdev)) strncpy(dev_name, jdev->valuestring, sizeof(dev_name) - 1);

            cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "command");
            if (jcmd && cJSON_IsString(jcmd) && jcmd->valuestring[0]) strncpy(cmd_name, jcmd->valuestring, sizeof(cmd_name) - 1);

            cJSON *jtop = cJSON_GetObjectItemCaseSensitive(req_j, "topic");
            if (!jtop) jtop = cJSON_GetObjectItemCaseSensitive(req_j, "mqttTopic");
            if (jtop && cJSON_IsString(jtop) && jtop->valuestring[0]) strncpy(req_topic, jtop->valuestring, sizeof(req_topic) - 1);

            cJSON *jp = cJSON_GetObjectItemCaseSensitive(req_j, "pulseMs");
            if (!jp) jp = cJSON_GetObjectItemCaseSensitive(req_j, "mqttPulse");
            if (jp && cJSON_IsNumber(jp) && jp->valueint > 0) req_pulse_ms = jp->valueint;
            cJSON_Delete(req_j);
        }

        if (!dev_name[0] && dev_id[0]) {
            strncpy(dev_name, dev_id, sizeof(dev_name) - 1);
        }

        char final_topic[256] = {0};
        int final_pulse_ms = 0;
        int mrc = mqtt_dispatch_button_pulse_topic(dev_id, dev_name, cmd_name, req_topic[0] ? req_topic : NULL, req_pulse_ms,
                                                  final_topic, sizeof(final_topic), &final_pulse_ms);
        if (mrc == 0) {
            cJSON *resp_j = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp_j, "ok", 1);
            cJSON_AddNumberToObject(resp_j, "pulseMs", final_pulse_ms);
            cJSON_AddStringToObject(resp_j, "topic", final_topic);
            cJSON_AddStringToObject(resp_j, "message", "MQTT test publish sent");
            send_cjson_resp(client_fd, 200, resp_j);
            cJSON_Delete(resp_j);
        } else {
            send_json_error(client_fd, 503, "MQTT broker not connected");
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /ir/device, POST /api/device-save, POST /ir/device-mqtt, POST /ir/device-power, or POST /ir/new-device */
    if ((strcmp(clean_path, "/ir/device") == 0 || strcmp(clean_path, "/api/device-save") == 0 ||
         strcmp(clean_path, "/ir/device-mqtt") == 0 || strcmp(clean_path, "/ir/device-power") == 0 ||
         strcmp(clean_path, "/ir/new-device") == 0 || strcmp(clean_path, "/api/new-device") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        if (p && (!device_int_fields_valid(p) || !device_delay_fields_in_range(p))) {
            /* A timing field of the wrong type, or a send delay out of range: refuse the whole save (400) */
            cJSON_Delete(p);
            p = NULL;
        }
        int success = 0;
        int save_failed = 0; /* valid request, devices.json could not be written: 500, not 400 */
        char dev_id_out[32] = {0};
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            cJSON *j_name = cJSON_GetObjectItemCaseSensitive(p, "name");
            cJSON *j_type = cJSON_GetObjectItemCaseSensitive(p, "type");
            cJSON *j_mfg = cJSON_GetObjectItemCaseSensitive(p, "manufacturer");
            cJSON *j_model = cJSON_GetObjectItemCaseSensitive(p, "model");
            cJSON *j_cp = cJSON_GetObjectItemCaseSensitive(p, "controlPort");
            cJSON *j_mqtt = cJSON_GetObjectItemCaseSensitive(p, "mqttTopic");
            cJSON *j_en = cJSON_GetObjectItemCaseSensitive(p, "mqttEnabled");

            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[32] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }
            int cp_val = 0;
            if (j_cp) {
                if (cJSON_IsNumber(j_cp)) cp_val = (int)j_cp->valuedouble;
                else if (cJSON_IsString(j_cp)) cp_val = atoi(j_cp->valuestring);
            }
            const char *mqtt_top = (j_mqtt && cJSON_IsString(j_mqtt)) ? j_mqtt->valuestring : NULL;

            cJSON *j_pulse = cJSON_GetObjectItemCaseSensitive(p, "mqttPulseMs");
            int pulse_val = (j_pulse && cJSON_IsNumber(j_pulse)) ? (int)j_pulse->valuedouble : 0;
            if (!pulse_val && j_pulse && cJSON_IsString(j_pulse)) pulse_val = atoi(j_pulse->valuestring);

            int has_en = (j_en != NULL);
            int en_val = 0;
            if (has_en) {
                if (cJSON_IsTrue(j_en)) en_val = 1;
                else if (cJSON_IsNumber(j_en)) en_val = (j_en->valuedouble > 0) ? 1 : 0;
                else if (cJSON_IsString(j_en)) en_val = (strcmp(j_en->valuestring, "1") == 0 || strcasecmp(j_en->valuestring, "true") == 0) ? 1 : 0;
            }

            /* held until the new list is saved: another save cannot slip in between */
            config_store_lock();
            cJSON *root = config_store_devices_copy(); /* NULL only when out of memory: nothing saved */
            cJSON *devs = root ? cJSON_GetObjectItemCaseSensitive(root, "devices") : NULL;
            if (root && !devs) {
                devs = cJSON_CreateArray();
                cJSON_AddItemToObject(root, "devices", devs);
            }

            if (devs && !dev_id[0]) {
                int cand = 1;
                while (1) {
                    char cand_s[16];
                    snprintf(cand_s, sizeof(cand_s), "%d", cand);
                    int used = 0;
                    cJSON *chk = NULL;
                    cJSON_ArrayForEach(chk, devs) {
                        cJSON *jid = cJSON_GetObjectItemCaseSensitive(chk, "id");
                        char cur_id[32] = {0};
                        if (jid && cJSON_IsNumber(jid)) snprintf(cur_id, sizeof(cur_id), "%ld", (long)jid->valuedouble);
                        else if (jid && cJSON_IsString(jid)) strncpy(cur_id, jid->valuestring, sizeof(cur_id) - 1);
                        if (strcmp(cur_id, cand_s) == 0) { used = 1; break; }
                    }
                    if (!used) {
                        snprintf(dev_buf, sizeof(dev_buf), "%d", cand);
                        dev_id = dev_buf;
                        break;
                    }
                    cand++;
                }
            }

            if (devs && dev_id[0]) {
                strncpy(dev_id_out, dev_id, sizeof(dev_id_out) - 1);
                cJSON *found_dev = NULL;
                cJSON *dev_item = NULL;
                cJSON_ArrayForEach(dev_item, devs) {
                    cJSON *jid = cJSON_GetObjectItemCaseSensitive(dev_item, "id");
                    char d_id_str[32] = {0};
                    if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                    else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                    if (strcmp(d_id_str, dev_id) == 0) {
                        found_dev = dev_item;
                        break;
                    }
                }
                if (!found_dev) {
                    found_dev = cJSON_CreateObject();
                    cJSON_AddStringToObject(found_dev, "id", dev_id);
                    cJSON_AddStringToObject(found_dev, "name", (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : dev_id);
                    cJSON_AddStringToObject(found_dev, "type", (j_type && cJSON_IsString(j_type)) ? j_type->valuestring : "Generic");
                    cJSON_AddStringToObject(found_dev, "manufacturer", (j_mfg && cJSON_IsString(j_mfg)) ? j_mfg->valuestring : "");
                    cJSON_AddStringToObject(found_dev, "model", (j_model && cJSON_IsString(j_model)) ? j_model->valuestring : "");
                    cJSON_AddNumberToObject(found_dev, "controlPort", cp_val > 0 ? cp_val : 7);
                    cJSON_AddItemToObject(found_dev, "commands", cJSON_CreateArray());
                    cJSON_AddItemToArray(devs, found_dev);
                }
                if (j_cp != NULL) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "controlPort");
                    cJSON_AddNumberToObject(found_dev, "controlPort", cp_val);
                }
                if (has_en) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqttEnabled");
                    cJSON_AddNumberToObject(found_dev, "mqttEnabled", en_val);
                }
                if (mqtt_top) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqttTopic");
                    cJSON_AddStringToObject(found_dev, "mqttTopic", mqtt_top);
                }
                if (pulse_val > 0) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqttPulseMs");
                    cJSON_AddNumberToObject(found_dev, "mqttPulseMs", pulse_val);
                }
                if (j_name && cJSON_IsString(j_name) && j_name->valuestring[0]) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "name");
                    cJSON_AddStringToObject(found_dev, "name", j_name->valuestring);
                }
                if (j_type && cJSON_IsString(j_type)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "type");
                    cJSON_AddStringToObject(found_dev, "type", j_type->valuestring);
                }
                if (j_mfg && cJSON_IsString(j_mfg)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "manufacturer");
                    cJSON_AddStringToObject(found_dev, "manufacturer", j_mfg->valuestring);
                }
                if (j_model && cJSON_IsString(j_model)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "model");
                    cJSON_AddStringToObject(found_dev, "model", j_model->valuestring);
                }
                cJSON *j_icon = cJSON_GetObjectItemCaseSensitive(p, "icon");
                if (j_icon) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "icon");
                    if (cJSON_IsString(j_icon)) cJSON_AddStringToObject(found_dev, "icon", j_icon->valuestring);
                    else if (cJSON_IsNumber(j_icon)) cJSON_AddNumberToObject(found_dev, "icon", j_icon->valueint);
                }

                cJSON *j_idd = cJSON_GetObjectItemCaseSensitive(p, "interDeviceDelay");
                if (j_idd) {
                    int val = 0;
                    device_int_field(j_idd, &val);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "interDeviceDelay");
                    cJSON_AddNumberToObject(found_dev, "interDeviceDelay", val);
                }

                cJSON *j_ikd = cJSON_GetObjectItemCaseSensitive(p, "interKeyDelay");
                if (j_ikd) {
                    int val = 0;
                    device_int_field(j_ikd, &val);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "interKeyDelay");
                    cJSON_AddNumberToObject(found_dev, "interKeyDelay", val);
                }

                cJSON *j_hikd = cJSON_GetObjectItemCaseSensitive(p, "holdInterKeyDelay");
                if (j_hikd) {
                    int val = 0;
                    device_int_field(j_hikd, &val);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "holdInterKeyDelay");
                    cJSON_AddNumberToObject(found_dev, "holdInterKeyDelay", val);
                }

                cJSON *j_pod = cJSON_GetObjectItemCaseSensitive(p, "powerOnDelay");
                if (j_pod) {
                    int val = 0;
                    device_int_field(j_pod, &val);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "powerOnDelay");
                    cJSON_AddNumberToObject(found_dev, "powerOnDelay", val);
                }

                cJSON *j_pao = cJSON_GetObjectItemCaseSensitive(p, "isPowerAlwaysOn");
                if (j_pao) {
                    int val = cJSON_IsTrue(j_pao) || (cJSON_IsNumber(j_pao) && j_pao->valuedouble > 0) || (cJSON_IsString(j_pao) && (strcmp(j_pao->valuestring, "1") == 0 || strcasecmp(j_pao->valuestring, "true") == 0));
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "isPowerAlwaysOn");
                    cJSON_AddBoolToObject(found_dev, "isPowerAlwaysOn", val);
                }

                cJSON *j_bthost = cJSON_GetObjectItemCaseSensitive(p, "bdaddr");
                if (!j_bthost) j_bthost = cJSON_GetObjectItemCaseSensitive(p, "btHost");
                if (j_bthost && cJSON_IsString(j_bthost)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btHost");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bdaddr");
                    cJSON_AddStringToObject(found_dev, "bdaddr", j_bthost->valuestring);
                }

                cJSON *j_btrec = cJSON_GetObjectItemCaseSensitive(p, "btAutoReconnect");
                if (j_btrec) {
                    int val = 1;
                    if (cJSON_IsNumber(j_btrec)) val = (int)j_btrec->valuedouble ? 1 : 0;
                    else if (cJSON_IsBool(j_btrec)) val = cJSON_IsTrue(j_btrec) ? 1 : 0;
                    else if (cJSON_IsString(j_btrec)) val = atoi(j_btrec->valuestring) ? 1 : 0;
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btAutoReconnect");
                    cJSON_AddNumberToObject(found_dev, "btAutoReconnect", val);
                }

                cJSON *j_btdur = cJSON_GetObjectItemCaseSensitive(p, "btReconnectDurationSec");
                if (j_btdur) {
                    int val = 0;
                    device_int_field(j_btdur, &val);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btReconnectDurationSec");
                    cJSON_AddNumberToObject(found_dev, "btReconnectDurationSec", val);
                }

                cJSON *j_btdly = cJSON_GetObjectItemCaseSensitive(p, "btReconnectDelaySec");
                if (j_btdly) {
                    int val = 0;
                    device_int_field(j_btdly, &val);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btReconnectDelaySec");
                    cJSON_AddNumberToObject(found_dev, "btReconnectDelaySec", val);
                }

                cJSON *j_trig_dev = cJSON_GetObjectItemCaseSensitive(p, "btTriggerDeviceId");
                if (j_trig_dev && cJSON_IsString(j_trig_dev)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btTriggerDeviceId");
                    if (j_trig_dev->valuestring[0]) {
                        cJSON_AddStringToObject(found_dev, "btTriggerDeviceId", j_trig_dev->valuestring);
                    }
                }

                cJSON *j_trig_cmd = cJSON_GetObjectItemCaseSensitive(p, "btTriggerCommandName");
                if (j_trig_cmd && cJSON_IsString(j_trig_cmd)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btTriggerCommandName");
                    if (j_trig_cmd->valuestring[0]) {
                        cJSON_AddStringToObject(found_dev, "btTriggerCommandName", j_trig_cmd->valuestring);
                    }
                }

                cJSON *j_po = cJSON_GetObjectItemCaseSensitive(p, "powerOnSequence");
                if (j_po && cJSON_IsArray(j_po)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "powerOnSequence");
                    cJSON *po_copy = cJSON_Duplicate(j_po, 1);
                    sanitize_activity_steps(po_copy);
                    cJSON_AddItemToObject(found_dev, "powerOnSequence", po_copy);
                }

                cJSON *j_pf = cJSON_GetObjectItemCaseSensitive(p, "powerOffSequence");
                if (j_pf && cJSON_IsArray(j_pf)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "powerOffSequence");
                    cJSON *pf_copy = cJSON_Duplicate(j_pf, 1);
                    sanitize_activity_steps(pf_copy);
                    cJSON_AddItemToObject(found_dev, "powerOffSequence", pf_copy);
                }

                if (config_store_save_devices(root) == 0) {
                    root = NULL;
                    success = 1;
                } else {
                    save_failed = 1;
                }
            }
            cJSON_Delete(root); /* NULL once the store took it */
            config_store_unlock();
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        if (success && dev_id_out[0]) {
            cJSON_AddStringToObject(resp, "deviceId", dev_id_out);
            cJSON_AddStringToObject(resp, "id", dev_id_out);
        }
        if (save_failed) cJSON_AddStringToObject(resp, "error", "Could not save the devices (flash full?)");
        send_cjson_resp(client_fd, success ? 200 : (save_failed ? 500 : 400), resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST/DELETE /ir/delete-device or /api/device-delete */
    if ((strcmp(clean_path, "/ir/delete-device") == 0 || strcmp(clean_path, "/api/device-delete") == 0) &&
        (strcmp(method, "POST") == 0 || strcmp(method, "DELETE") == 0)) {
        cJSON *p = parse_request_params(body);
        if (!p && query) {
            p = parse_request_params(query);
        }
        int success = 0;
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[64] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }
            if (!dev_id[0] && query) {
                http_query_param(query, "id", dev_buf, sizeof(dev_buf));
                dev_id = dev_buf;
            }
            if (dev_id[0]) {
                cJSON *users = device_map_users(dev_id); /* the maps are not under the lock */
                config_store_lock();
                /* Refused while activities, other devices or remote key maps still use the device
                 * (as agreed in #220): the reply lists them so they can be changed first */
                int exists = device_config_users(dev_id, users);
                if (exists && cJSON_GetArraySize(users) > 0) {
                    config_store_unlock();
                    cJSON *err = cJSON_CreateObject();
                    cJSON_AddBoolToObject(err, "ok", 0);
                    cJSON_AddStringToObject(err, "error", "Device is in use: remove it from these activities, devices and remote keys first");
                    cJSON_AddStringToObject(err, "deviceId", dev_id);
                    cJSON_AddItemToObject(err, "users", users);
                    cJSON_Delete(p);
                    send_cjson_resp(client_fd, 409, err);
                    cJSON_Delete(err);
                    close(client_fd);
                    return 1;
                }
                cJSON_Delete(users);

                cJSON *root = config_store_devices_copy();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (devs && cJSON_IsArray(devs)) {
                        int d_count = cJSON_GetArraySize(devs);
                        for (int di = 0; di < d_count; di++) {
                            cJSON *dev_item = cJSON_GetArrayItem(devs, di);
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(dev_item, "id");
                            char d_id_str[64] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON_DeleteItemFromArray(devs, di);
                                if (config_store_save_devices(root) == 0) {
                                    root = NULL;
                                    success = 1;
                                }
                                break;
                            }
                        }
                    }
                    cJSON_Delete(root); /* NULL once the store took it */
                }
                config_store_unlock();
                /* its own page in the remote maps: a new device given the same id would inherit it */
                if (success) remote_maps_drop_section("devices", dev_id);
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    return 0;
}
