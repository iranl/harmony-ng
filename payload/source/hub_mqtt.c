/*
 * MQTT v2 (spec section 5): one bus sink publishes activity_state, activity_progress, bt_status,
 * rf_status and button; retained topics are republished on (re)connect from the bus's last
 * payloads, never on a timer; config_changed rebuilds the config and inventory topics; the
 * command channel <base>/cmd mirrors the WebSocket actions through hub_cmd_execute.
 *
 * Topics (<base> = baseTopic, default harmony/hub):
 *   status (LWT online/offline), hub, activity_state, progress, bt, rf, config,
 *   inventory/index, inventory/<deviceId>, remote/<slot>/button (button, no slot: <base>/button),
 *   cmd (in), cmd/response (out), ir/raw (in), ir/received (out, while the IR capture mode is on).
 *
 * Kept from the first MQTT layer: the Home Assistant discovery entities (hub_mqtt_ha.c, on the
 * v2 topics), the inbound command topics activity/set, send_command, command/<dev>/<cmd>, sync,
 * remote/<n>/sleep and harmony/ir/pronto, and the pulse of MQTT-type devices on their own topic.
 */
#define _GNU_SOURCE
#include "hub_mqtt.h"
#include "hub_events.h"
#include "hub_cmd.h"
#include "hub_ws.h"
#include "config_store.h"
#include "state_watch.h"
#include "mqtt_client.h"
#include "sha256.h"
#include "hub_mqtt_ha.h"
#include "hub_info.h"
#include "hw_action.h"
#include "cJSON.h"
#include <stdint.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define MQTT_V2_MARKER "/data/harmony/mqtt_v2_migrated"
#define MAX_INVENTORY_DEVICES 64
#define CHUNK_MAX_BYTES 3800   /* under the 4 KB packet buffer with the topic and headers */

/* MQTT configuration */
static char g_mqtt_host[128] = "";
static int  g_mqtt_port = 1883;
char g_mqtt_base_topic[128] = "harmony/hub";
static char g_mqtt_cid[64] = "harmony-local-mqtt";
static char g_mqtt_user[256] = "";
static char g_mqtt_pass[256] = "";
static char g_mqtt_discovery_prefix[64] = "homeassistant";
static int  g_mqtt_ha_discovery = 1;
static int  g_mqtt_enabled = 0;
static char g_status_topic[160] = "harmony/hub/status";

/* Inventory chunks published so far (to clear removed devices) and their content hashes */
static struct {
    char id[32];
    char hash[17];
} g_inv_pub[MAX_INVENTORY_DEVICES];
static int g_inv_pub_count;
static char g_inventory_version[9] = "";
static char g_last_config_payload[512] = "";

static time_t g_last_mqtt_retry = 0;
static int g_last_mqtt_connected = 0;
static int g_mqtt_retry_interval = 5;
static int g_cleared_old = 0;

/* ------------------------------------------------------------------------------------------ */

void daemon_get_mqtt_info(char *host, size_t hlen, int *port, char *base, size_t blen, int *connected) {
    if (host && hlen > 0) {
        strncpy(host, g_mqtt_enabled ? g_mqtt_host : "", hlen - 1);
        host[hlen - 1] = '\0';
    }
    if (port) *port = g_mqtt_port;
    if (base && blen > 0) {
        strncpy(base, g_mqtt_base_topic, blen - 1);
        base[blen - 1] = '\0';
    }
    if (connected) *connected = mqtt_is_connected();
}

static void set_str(char *dst, size_t cap, const cJSON *j, const char *dflt) {
    if (j && cJSON_IsString(j) && j->valuestring && j->valuestring[0]) snprintf(dst, cap, "%s", j->valuestring);
    else snprintf(dst, cap, "%s", dflt ? dflt : "");
}

/* enabled, host, port, username, password, baseTopic, clientId, discoveryPrefix, haDiscovery.
 * Returns 1 when MQTT is enabled and has a host. */
static int load_mqtt_config(void) {
    cJSON *root = config_store_get_mqtt_json();
    if (!root) return 0;
    cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (!en || !cJSON_IsTrue(en) || !h || !cJSON_IsString(h) || !h->valuestring || !h->valuestring[0]) {
        cJSON_Delete(root);
        return 0;
    }
    set_str(g_mqtt_host, sizeof(g_mqtt_host), h, "");
    cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "port");
    g_mqtt_port = (p && cJSON_IsNumber(p) && p->valueint > 0) ? p->valueint : 1883;
    set_str(g_mqtt_cid, sizeof(g_mqtt_cid), cJSON_GetObjectItemCaseSensitive(root, "clientId"), "harmony-local-mqtt");
    set_str(g_mqtt_user, sizeof(g_mqtt_user), cJSON_GetObjectItemCaseSensitive(root, "username"), "");
    set_str(g_mqtt_pass, sizeof(g_mqtt_pass), cJSON_GetObjectItemCaseSensitive(root, "password"), "");
    set_str(g_mqtt_base_topic, sizeof(g_mqtt_base_topic), cJSON_GetObjectItemCaseSensitive(root, "baseTopic"), "harmony/hub");
    size_t blen = strlen(g_mqtt_base_topic);
    while (blen > 0 && g_mqtt_base_topic[blen - 1] == '/') g_mqtt_base_topic[--blen] = '\0';
    if (!g_mqtt_base_topic[0]) snprintf(g_mqtt_base_topic, sizeof(g_mqtt_base_topic), "harmony/hub");
    set_str(g_mqtt_discovery_prefix, sizeof(g_mqtt_discovery_prefix), cJSON_GetObjectItemCaseSensitive(root, "discoveryPrefix"), "homeassistant");
    cJSON *ha = cJSON_GetObjectItemCaseSensitive(root, "haDiscovery");
    g_mqtt_ha_discovery = (ha && cJSON_IsBool(ha)) ? (cJSON_IsTrue(ha) ? 1 : 0) : 1;
    snprintf(g_status_topic, sizeof(g_status_topic), "%s/status", g_mqtt_base_topic);
    cJSON_Delete(root);
    return 1;
}

/* ------------------------------------------------------------------------------------------ */
/* Publishing helpers                                                                          */

static int pub_json(const char *suffix, const cJSON *obj, int retain, int qos) {
    if (!mqtt_is_connected() && mqtt_publishing_live()) return -1;
    char topic[256];
    snprintf(topic, sizeof(topic), "%s/%s", g_mqtt_base_topic, suffix);
    char *txt = cJSON_PrintUnformatted(obj);
    if (!txt) return -1;
    int rc = mqtt_publish_ex(topic, txt, strlen(txt), qos, retain);
    free(txt);
    return rc;
}

static void pub_clear(const char *full_topic) {
    mqtt_publish_ex(full_topic, "", 0, 1, 1);
}

int hub_mqtt_publish_plain(const char *topic, const char *payload) {
    if (!g_mqtt_enabled || !topic || !topic[0]) return -1;
    return mqtt_publish_ex(topic, payload ? payload : "", payload ? strlen(payload) : 0, 0, 0);
}

/* ------------------------------------------------------------------------------------------ */
/* Inventory                                                                                   */

static char *read_text(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (len_out) *len_out = n;
    return buf;
}

static void short_hash(const char *txt, size_t len, char out[17], int digits) {
    char hex[65];
    sha256_hex((const uint8_t *)txt, len, hex);
    snprintf(out, 17, "%.*s", digits, hex);
}

static const char *obj_str(const cJSON *o, const char *k) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);
    return (j && cJSON_IsString(j) && j->valuestring) ? j->valuestring : "";
}

static void id_of(const cJSON *d, char *out, size_t cap) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(d, "id");
    if (j && cJSON_IsString(j) && j->valuestring) snprintf(out, cap, "%s", j->valuestring);
    else if (j && cJSON_IsNumber(j)) snprintf(out, cap, "%ld", (long)j->valuedouble);
    else out[0] = '\0';
}

static int inv_pub_find(const char *id) {
    for (int i = 0; i < g_inv_pub_count; i++) if (strcmp(g_inv_pub[i].id, id) == 0) return i;
    return -1;
}

/* Publishes one device as inventory/<id> (split into parts when it does not fit the packet) */
static void publish_device_chunk(const char *id, cJSON *chunk) {
    char suffix[80];
    char *txt = cJSON_PrintUnformatted(chunk);
    if (!txt) return;
    size_t len = strlen(txt);
    if (len <= CHUNK_MAX_BYTES) {
        snprintf(suffix, sizeof(suffix), "inventory/%s", id);
        pub_json(suffix, chunk, 1, 1);
        free(txt);
        return;
    }
    free(txt);
    /* Split the command names over several parts */
    cJSON *cmds = cJSON_DetachItemFromObjectCaseSensitive(chunk, "commands");
    int ncmd = cmds ? cJSON_GetArraySize(cmds) : 0;
    int per_part = 120;
    int parts = (ncmd + per_part - 1) / per_part;
    if (parts < 1) parts = 1;
    for (int part = 1; part <= parts; part++) {
        cJSON *pc = cJSON_Duplicate(chunk, 1);
        cJSON *arr = cJSON_AddArrayToObject(pc, "commands");
        for (int i = (part - 1) * per_part; i < part * per_part && i < ncmd; i++) {
            cJSON_AddItemToArray(arr, cJSON_Duplicate(cJSON_GetArrayItem(cmds, i), 1));
        }
        cJSON_AddNumberToObject(pc, "part", part);
        cJSON_AddNumberToObject(pc, "parts", parts);
        snprintf(suffix, sizeof(suffix), "inventory/%s/%d", id, part);
        pub_json(suffix, pc, 1, 1);
        cJSON_Delete(pc);
    }
    cJSON_Delete(cmds);
}

/* Rebuilds config and inventory. force: republish every chunk (a reconnect). */
static void publish_inventory(int force) {
    size_t dlen = 0, alen = 0;
    char *dtxt = read_text(HARMONY_DEVICES_FILE, &dlen);
    char *atxt = read_text(HARMONY_ACTIVITIES_FILE, &alen);
    /* inventory_version: first 8 hex of SHA-256 over both files */
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    if (dtxt) sha256_update(&ctx, (const uint8_t *)dtxt, dlen);
    if (atxt) sha256_update(&ctx, (const uint8_t *)atxt, alen);
    uint8_t digest[32];
    sha256_final(digest, &ctx);
    char version[9];
    snprintf(version, sizeof(version), "%02x%02x%02x%02x", digest[0], digest[1], digest[2], digest[3]);
    int version_changed = strcmp(version, g_inventory_version) != 0;
    snprintf(g_inventory_version, sizeof(g_inventory_version), "%s", version);

    cJSON *droot = dtxt ? cJSON_Parse(dtxt) : NULL;
    cJSON *aroot = atxt ? cJSON_Parse(atxt) : NULL;
    free(dtxt);
    free(atxt);
    cJSON *devs = droot ? (cJSON_IsArray(droot) ? droot : cJSON_GetObjectItemCaseSensitive(droot, "devices")) : NULL;
    cJSON *acts = aroot ? (cJSON_IsArray(aroot) ? aroot : cJSON_GetObjectItemCaseSensitive(aroot, "activities")) : NULL;

    /* Device chunks: republished only when their own content changed */
    char seen[MAX_INVENTORY_DEVICES][32];
    int nseen = 0;
    cJSON *index = cJSON_CreateObject();
    cJSON_AddStringToObject(index, "type", "inventory_index");
    cJSON_AddStringToObject(index, "inventory_version", version);
    cJSON *idx_devs = cJSON_AddArrayToObject(index, "devices");
    cJSON *d = NULL;
    cJSON_ArrayForEach(d, devs) {
        char id[32];
        id_of(d, id, sizeof(id));
        if (!id[0] || nseen >= MAX_INVENTORY_DEVICES) continue;
        snprintf(seen[nseen++], 32, "%s", id);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", id);
        cJSON_AddStringToObject(entry, "name", obj_str(d, "name"));
        cJSON_AddStringToObject(entry, "deviceType", obj_str(d, "type"));
        cJSON_AddStringToObject(entry, "manufacturer", obj_str(d, "manufacturer"));
        cJSON_AddStringToObject(entry, "model", obj_str(d, "model"));
        cJSON_AddItemToArray(idx_devs, entry);

        /* the device's own "type" (Television, ...) goes under deviceType: "type" is the
         * message type on every topic */
        cJSON *chunk = cJSON_CreateObject();
        cJSON_AddStringToObject(chunk, "type", "inventory_device");
        cJSON_AddStringToObject(chunk, "id", id);
        cJSON_AddStringToObject(chunk, "name", obj_str(d, "name"));
        cJSON_AddStringToObject(chunk, "deviceType", obj_str(d, "type"));
        cJSON_AddStringToObject(chunk, "manufacturer", obj_str(d, "manufacturer"));
        cJSON_AddStringToObject(chunk, "model", obj_str(d, "model"));
        const cJSON *cp = cJSON_GetObjectItemCaseSensitive(d, "controlPort");
        cJSON_AddNumberToObject(chunk, "controlPort", (cp && cJSON_IsNumber(cp)) ? cp->valueint : 0);
        cJSON *cmds = cJSON_AddArrayToObject(chunk, "commands");
        const cJSON *c = NULL;
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(d, "commands")) {
            const char *nm = cJSON_IsString(c) ? c->valuestring : obj_str(c, "name");
            if (nm && nm[0]) cJSON_AddItemToArray(cmds, cJSON_CreateString(nm));
        }
        char *txt = cJSON_PrintUnformatted(chunk);
        char h[17] = "";
        if (txt) short_hash(txt, strlen(txt), h, 16);
        free(txt);
        int k = inv_pub_find(id);
        if (force || k < 0 || strcmp(g_inv_pub[k].hash, h) != 0) {
            publish_device_chunk(id, chunk);
            if (k < 0 && g_inv_pub_count < MAX_INVENTORY_DEVICES) {
                k = g_inv_pub_count++;
                snprintf(g_inv_pub[k].id, sizeof(g_inv_pub[k].id), "%s", id);
            }
            if (k >= 0) snprintf(g_inv_pub[k].hash, sizeof(g_inv_pub[k].hash), "%s", h);
        }
        cJSON_Delete(chunk);
    }
    /* Removed devices: clear their retained chunk */
    for (int i = 0; i < g_inv_pub_count; i++) {
        int still = 0;
        for (int j = 0; j < nseen; j++) if (strcmp(seen[j], g_inv_pub[i].id) == 0) still = 1;
        if (!still) {
            char topic[256];
            snprintf(topic, sizeof(topic), "%s/inventory/%s", g_mqtt_base_topic, g_inv_pub[i].id);
            pub_clear(topic);
            g_inv_pub[i] = g_inv_pub[g_inv_pub_count - 1];
            g_inv_pub_count--;
            i--;
        }
    }
    /* index last, so a consumer that waits for it sees complete chunks */
    cJSON *idx_acts = cJSON_AddArrayToObject(index, "activities");
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, acts) {
        char id[32];
        id_of(a, id, sizeof(id));
        if (!id[0]) continue;
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", id);
        cJSON_AddStringToObject(entry, "name", obj_str(a, "name"));
        cJSON_AddStringToObject(entry, "activityType", obj_str(a, "type"));
        const cJSON *ord = cJSON_GetObjectItemCaseSensitive(a, "order");
        cJSON_AddNumberToObject(entry, "order", (ord && cJSON_IsNumber(ord)) ? ord->valueint : 0);
        cJSON_AddItemToArray(idx_acts, entry);
    }
    if (force || version_changed) pub_json("inventory/index", index, 1, 1);
    cJSON_Delete(index);
    cJSON_Delete(droot);
    cJSON_Delete(aroot);

    /* config: the watcher's mtimes plus the inventory version */
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "type", "config");
    cJSON *files = state_watch_config_mtimes();
    if (files) cJSON_AddItemToObject(cfg, "files", files);
    cJSON_AddStringToObject(cfg, "inventory_version", version);
    cJSON_AddNumberToObject(cfg, "ts", (double)time(NULL));
    char *ctxt = cJSON_PrintUnformatted(cfg);
    if (ctxt) {
        /* the ts changes every time: compare without it */
        char *cut = strstr(ctxt, ",\"ts\":");
        size_t cmp_len = cut ? (size_t)(cut - ctxt) : strlen(ctxt);
        if (force || strncmp(ctxt, g_last_config_payload, cmp_len) != 0 || g_last_config_payload[cmp_len] != '\0') {
            pub_json("config", cfg, 1, 1);
            snprintf(g_last_config_payload, sizeof(g_last_config_payload), "%.*s", (int)cmp_len, ctxt);
        }
        free(ctxt);
    }
    cJSON_Delete(cfg);
}

/* ------------------------------------------------------------------------------------------ */
/* Old state topics of the previous MQTT layer: cleared once                                    */

static void clear_old_topics(void) {
    if (g_cleared_old) return;
    g_cleared_old = 1;
    if (access(MQTT_V2_MARKER, F_OK) == 0) return;
    static const char *const old_state[] = { "state", "activity", "current_activity", "activity_id", "activity/status", NULL };
    char topic[256];
    for (int i = 0; old_state[i]; i++) {
        snprintf(topic, sizeof(topic), "%s/%s", g_mqtt_base_topic, old_state[i]);
        pub_clear(topic);
    }
    printf("[*] MQTT: cleared the old state topics (%s/{state,activity,current_activity,activity_id})\n", g_mqtt_base_topic);
    FILE *f = fopen(MQTT_V2_MARKER, "w");
    if (f) {
        fprintf(f, "%ld\n", (long)time(NULL));
        fclose(f);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Command channel                                                                             */

static void publish_cmd_response(cJSON *resp) {
    if (!resp) return;
    pub_json("cmd/response", resp, 0, 1);
}

/* Recovers "id":"..." from a payload that failed to parse */
static int recover_id(const char *payload, char *out, size_t cap) {
    const char *p = payload ? strstr(payload, "\"id\"") : NULL;
    if (!p) return 0;
    p = strchr(p + 4, '"');
    if (!p) return 0;
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < cap && n < 64) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

static void on_cmd(const char *topic, const char *payload, size_t len, void *ud) {
    (void)ud;
    (void)len;
    int is_ir_raw = strstr(topic, "/ir/raw") != NULL;
    cJSON *req = (payload && payload[0]) ? cJSON_Parse(payload) : NULL;
    if (!req || !cJSON_IsObject(req)) {
        cJSON_Delete(req);
        char id[65];
        if (recover_id(payload, id, sizeof(id))) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "type", "cmd_response");
            cJSON_AddStringToObject(resp, "id", id);
            cJSON_AddStringToObject(resp, "action", is_ir_raw ? "ir_raw" : "");
            cJSON_AddBoolToObject(resp, "ok", 0);
            cJSON_AddNumberToObject(resp, "code", 400);
            cJSON_AddStringToObject(resp, "error", "malformed JSON");
            publish_cmd_response(resp);
            cJSON_Delete(resp);
        } else {
            printf("[!] MQTT: malformed request on %s\n", topic);
        }
        return;
    }
    if (is_ir_raw) {
        cJSON_DeleteItemFromObjectCaseSensitive(req, "action");
        cJSON_AddStringToObject(req, "action", "ir_raw");
    }
    /* NULL: the reply comes later (update_check), through hub_mqtt_deliver_reply */
    cJSON *reply = hub_cmd_execute_from(req, 2 /* ROLE_ADMIN: the broker is trusted */, HUB_CMD_ORIGIN_MQTT);
    if (reply) hub_mqtt_deliver_reply(req, reply);
    cJSON_Delete(reply);
    cJSON_Delete(req);
}

void hub_mqtt_deliver_reply(const cJSON *req, const cJSON *reply) {
    const cJSON *jid = req ? cJSON_GetObjectItemCaseSensitive(req, "id") : NULL;
    if (!reply || !jid || !cJSON_IsString(jid) || !jid->valuestring[0]) return; /* no id: no answer */
    const char *rtype = obj_str(reply, "type");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "cmd_response");
    cJSON_AddStringToObject(resp, "id", jid->valuestring);
    cJSON_AddStringToObject(resp, "action", obj_str(req, "action"));
    if (strcmp(rtype, "ack") == 0) {
        const cJSON *ok = cJSON_GetObjectItemCaseSensitive(reply, "ok");
        int okv = ok && cJSON_IsTrue(ok);
        cJSON_AddBoolToObject(resp, "ok", okv);
        if (!okv) {
            const cJSON *code = cJSON_GetObjectItemCaseSensitive(reply, "code");
            cJSON_AddNumberToObject(resp, "code", code && cJSON_IsNumber(code) ? code->valueint : 500);
            cJSON_AddStringToObject(resp, "error", obj_str(reply, "error"));
        }
        /* every other field of the ack (count, duration_ms, the update and Help fields) */
        static const char *const own[] = { "type", "action", "id", "msgId", "ok", "code", "error", NULL };
        const cJSON *it;
        cJSON_ArrayForEach(it, reply) {
            int skip = 0;
            for (int i = 0; own[i]; i++) if (it->string && strcmp(it->string, own[i]) == 0) skip = 1;
            if (!skip && it->string) cJSON_AddItemToObject(resp, it->string, cJSON_Duplicate(it, 1));
        }
    } else {
        /* pong, activity_state, snapshot: the object itself under its type name */
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON *copy = cJSON_Duplicate(reply, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "id");
        cJSON_AddItemToObject(resp, strcmp(rtype, "pong") == 0 ? "pong" : rtype, copy);
    }
    publish_cmd_response(resp);
    cJSON_Delete(resp);
}

/* ------------------------------------------------------------------------------------------ */
/* Inbound topics of the first MQTT layer, kept as aliases of the command channel               */

static void publish_hub_object(void);
static long g_published_boot_time = 0; /* boot_time in the last hub object published, 0 = null */
static void publish_last(hub_event_type_t type, const char *suffix);

static void publish_discovery(void) {
    if (g_mqtt_ha_discovery) {
        struct hub_settings st;
        config_store_get_settings(&st);
        publish_ha_discovery(g_mqtt_cid, st.hub_name, hub_info_id(), g_mqtt_base_topic, g_mqtt_discovery_prefix,
                             g_status_topic, g_mqtt_host, 1);
    } else {
        clear_ha_discovery(g_mqtt_cid, g_mqtt_discovery_prefix);
    }
}

static void run_simple_action(const char *action, const char *key, const char *value, int slot) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "action", action);
    if (key && value) cJSON_AddStringToObject(req, key, value);
    if (slot > 0) cJSON_AddNumberToObject(req, "slot", slot);
    cJSON *reply = hub_cmd_execute(req, 2 /* ROLE_ADMIN: the broker is trusted */);
    const cJSON *ok = reply ? cJSON_GetObjectItemCaseSensitive(reply, "ok") : NULL;
    if (reply && ok && !cJSON_IsTrue(ok)) printf("[-] MQTT %s: %s\n", action, obj_str(reply, "error"));
    cJSON_Delete(reply);
    cJSON_Delete(req);
}

/* <base>/activity/set: an activity name, an id, or PowerOff / -1 */
static void handle_activity_set(const char *payload) {
    if (!payload) return;
    char trimmed[256];
    snprintf(trimmed, sizeof(trimmed), "%s", payload);
    char *start = trimmed;
    while (*start && isspace((unsigned char)*start)) start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    if (!*start) return;
    if (strcasecmp(start, "poweroff") == 0 || strcmp(start, "-1") == 0) {
        printf("[*] MQTT activity/set: PowerOff\n");
        run_simple_action("stop_activity", NULL, NULL, 0);
        return;
    }
    struct daemon_activity_list acts;
    load_daemon_activities(&acts);
    const char *id = NULL;
    for (int i = 0; i < acts.count && !id; i++) if (strcmp(start, acts.items[i].id) == 0) id = acts.items[i].id;
    for (int i = 0; i < acts.count && !id; i++) if (strcasecmp(start, acts.items[i].name) == 0) id = acts.items[i].id;
    if (!id) {
        printf("[-] MQTT activity/set: activity '%s' not found\n", start);
        return;
    }
    printf("[*] MQTT activity/set: start activity %s\n", id);
    run_simple_action("start_activity", "activity", id, 0);
}

/* <base>/send_command: {"device"|"deviceId", "command"|"commands"|"cmd" (string or list),
 * "repeats", "delay"/"delay_ms", "hold"/"hold_ms", "action"}; a list, repeats or a hold run
 * from the sequencer in hub_mqtt_tick() */
typedef struct {
    int active;
    char dev[128];
    int dev_is_id;
    char cmd_list[32][64];
    int cmd_count;
    int num_repeats;
    int cur_repeat;
    int cur_cmd;
    uint32_t hold_ms;
    uint32_t delay_ms;
    uint64_t next_exec_ms;
} mqtt_cmd_seq_t;
static mqtt_cmd_seq_t g_mqtt_seq;

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void seq_send(const mqtt_cmd_seq_t *q, const char *cmd) {
    if (q->dev_is_id) hw_device_command_send_by_id(q->dev, cmd);
    else hw_device_command_send_by_name(q->dev, cmd);
}

static void seq_tick(void) {
    if (!g_mqtt_seq.active) return;
    uint64_t now = mono_ms();
    if (now < g_mqtt_seq.next_exec_ms) return;
    seq_send(&g_mqtt_seq, g_mqtt_seq.cmd_list[g_mqtt_seq.cur_cmd]);
    g_mqtt_seq.cur_cmd++;
    if (g_mqtt_seq.cur_cmd >= g_mqtt_seq.cmd_count) {
        g_mqtt_seq.cur_cmd = 0;
        g_mqtt_seq.cur_repeat++;
    }
    if (g_mqtt_seq.cur_repeat >= g_mqtt_seq.num_repeats) {
        g_mqtt_seq.active = 0;
    } else {
        uint32_t wait_ms = g_mqtt_seq.hold_ms + g_mqtt_seq.delay_ms;
        g_mqtt_seq.next_exec_ms = now + (wait_ms > 0 ? wait_ms : 50);
    }
}

static double num_of(const cJSON *root, const char *k1, const char *k2, const char *k3, double dflt) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(root, k1);
    if (!j && k2) j = cJSON_GetObjectItemCaseSensitive(root, k2);
    if (!j && k3) j = cJSON_GetObjectItemCaseSensitive(root, k3);
    return (j && cJSON_IsNumber(j)) ? j->valuedouble : dflt;
}

static void handle_send_command(const char *payload) {
    if (!payload || !payload[0]) return;
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        printf("[-] MQTT send_command: invalid JSON payload\n");
        return;
    }
    const cJSON *j_dev_id = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
    if (!j_dev_id) j_dev_id = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    const cJSON *j_dev_name = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (!j_dev_name) j_dev_name = cJSON_GetObjectItemCaseSensitive(root, "dev");
    char dev[128] = {0};
    int is_id = 0;
    const cJSON *j_dev = j_dev_id ? j_dev_id : j_dev_name;
    if (j_dev) {
        is_id = (j_dev == j_dev_id);
        if (cJSON_IsString(j_dev) && j_dev->valuestring) snprintf(dev, sizeof(dev), "%s", j_dev->valuestring);
        else if (cJSON_IsNumber(j_dev)) snprintf(dev, sizeof(dev), "%ld", (long)j_dev->valuedouble);
    }
    if (!dev[0]) {
        printf("[-] MQTT send_command: missing device_id or device in payload\n");
        cJSON_Delete(root);
        return;
    }
    char cmd_list[32][64];
    int cmd_count = 0;
    const cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!j_cmd) j_cmd = cJSON_GetObjectItemCaseSensitive(root, "commands");
    if (!j_cmd) j_cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (j_cmd && cJSON_IsArray(j_cmd)) {
        const cJSON *elem;
        cJSON_ArrayForEach(elem, j_cmd) {
            if (cmd_count >= 32) break;
            if (cJSON_IsString(elem) && elem->valuestring && elem->valuestring[0]) snprintf(cmd_list[cmd_count++], 64, "%.63s", elem->valuestring);
            else if (cJSON_IsNumber(elem)) snprintf(cmd_list[cmd_count++], 64, "%ld", (long)elem->valuedouble);
        }
    } else if (j_cmd && cJSON_IsString(j_cmd) && j_cmd->valuestring && j_cmd->valuestring[0]) {
        snprintf(cmd_list[0], 64, "%.63s", j_cmd->valuestring);
        cmd_count = 1;
    } else if (j_cmd && cJSON_IsNumber(j_cmd)) {
        snprintf(cmd_list[0], 64, "%ld", (long)j_cmd->valuedouble);
        cmd_count = 1;
    }
    if (cmd_count == 0) {
        printf("[-] MQTT send_command: no commands parsed for device '%s'\n", dev);
        cJSON_Delete(root);
        return;
    }
    const cJSON *act_j = cJSON_GetObjectItemCaseSensitive(root, "action");
    const char *action = (act_j && cJSON_IsString(act_j)) ? act_j->valuestring : "tap";
    double delay_secs = num_of(root, "delay_secs", "delaySecs", "delay", -1.0);
    if (delay_secs < 0) {
        double ms = num_of(root, "delay_ms", "delayMs", NULL, -1.0);
        if (ms >= 0) delay_secs = ms / 1000.0;
    }
    double hold_secs = num_of(root, "hold_secs", "holdSecs", "hold", -1.0);
    if (hold_secs < 0) {
        double ms = num_of(root, "hold_ms", "holdMs", NULL, -1.0);
        hold_secs = ms >= 0 ? ms / 1000.0 : 0.0;
    }
    if (strcasecmp(action, "hold") == 0 && hold_secs <= 0.001) hold_secs = 0.5;
    int num_repeats = (int)num_of(root, "num_repeats", "numRepeats", "repeats", 1);
    cJSON_Delete(root);
    if (num_repeats < 1) num_repeats = 1;
    if (num_repeats > 50) num_repeats = 50;
    int is_multi = (cmd_count > 1) || (num_repeats > 1);
    if (delay_secs < 0) delay_secs = is_multi ? 0.25 : 0.0;
    if (delay_secs > 60.0) delay_secs = 60.0;
    if (hold_secs > 10.0) hold_secs = 10.0;
    printf("[*] MQTT send_command: dev='%s' cmds=%d repeats=%d action=%s delay=%.2fs hold=%.2fs\n",
           dev, cmd_count, num_repeats, action, delay_secs, hold_secs);
    snprintf(g_mqtt_seq.dev, sizeof(g_mqtt_seq.dev), "%s", dev);
    g_mqtt_seq.dev_is_id = is_id;
    g_mqtt_seq.cmd_count = cmd_count;
    for (int i = 0; i < cmd_count; i++) snprintf(g_mqtt_seq.cmd_list[i], 64, "%.63s", cmd_list[i]);
    g_mqtt_seq.num_repeats = num_repeats;
    g_mqtt_seq.cur_repeat = 0;
    g_mqtt_seq.cur_cmd = 0;
    g_mqtt_seq.hold_ms = (uint32_t)(hold_secs * 1000.0);
    g_mqtt_seq.delay_ms = (uint32_t)(delay_secs * 1000.0);
    g_mqtt_seq.next_exec_ms = 0;
    g_mqtt_seq.active = 1;
    seq_tick();
}

/* <base>/command/<device>/<command> */
static void handle_command_topic(const char *topic, const char *payload) {
    const char *p = strstr(topic, "/command/");
    if (!p) return;
    p += 9;
    const char *slash = strchr(p, '/');
    if (!slash) return;
    char dev[64] = {0};
    size_t dev_len = (size_t)(slash - p);
    if (dev_len >= sizeof(dev)) dev_len = sizeof(dev) - 1;
    memcpy(dev, p, dev_len);
    const char *cmd = slash + 1;
    if (!dev[0] || !cmd[0]) return;
    printf("[*] MQTT command/%s/%s (payload %s)\n", dev, cmd, payload ? payload : "");
    hw_device_command_send(dev, cmd);
}

static void on_alias(const char *topic, const char *payload, size_t len, void *ud) {
    (void)len;
    (void)ud;
    size_t blen = strlen(g_mqtt_base_topic);
    const char *sub = (strncmp(topic, g_mqtt_base_topic, blen) == 0 && topic[blen] == '/') ? topic + blen + 1 : NULL;
    char birth[160];
    snprintf(birth, sizeof(birth), "%s/status", g_mqtt_discovery_prefix);
    if (strcmp(topic, birth) == 0) {
        if (payload && strcmp(payload, "online") == 0) {
            printf("[*] MQTT: Home Assistant birth message, republishing the discovery entities\n");
            publish_discovery();
        }
    } else if (strcmp(topic, "harmony/ir/pronto") == 0) {
        if (payload && payload[0]) {
            printf("[*] MQTT: Pronto code received\n");
            hw_ir_send_pronto(payload, IR_PORT_ALL, 1);
        }
    } else if (!sub) {
        return;
    } else if (strcmp(sub, "activity/set") == 0) {
        handle_activity_set(payload);
    } else if (strcmp(sub, "send_command") == 0) {
        handle_send_command(payload);
    } else if (strcmp(sub, "sync") == 0) {
        printf("[*] MQTT sync: republishing the retained state\n");
        publish_hub_object();
        publish_last(HUB_EV_ACTIVITY_STATE, "activity_state");
        publish_last(HUB_EV_BT_STATUS, "bt");
        publish_last(HUB_EV_RF_STATUS, "rf");
        publish_inventory(1);
        publish_discovery();
    } else if (strcmp(sub, "remote/1/sleep") == 0 || strcmp(sub, "remote/sleep") == 0) {
        run_simple_action("rf_sleep", NULL, NULL, 1);
    } else if (strcmp(sub, "remote/2/sleep") == 0) {
        run_simple_action("rf_sleep", NULL, NULL, 2);
    } else if (strncmp(sub, "command/", 8) == 0) {
        handle_command_topic(topic, payload);
    }
}

static void subscribe_cmd_topics(void) {
    char t[256];
    snprintf(t, sizeof(t), "%s/cmd", g_mqtt_base_topic);
    mqtt_subscribe(t, on_cmd, NULL);
    snprintf(t, sizeof(t), "%s/ir/raw", g_mqtt_base_topic);
    mqtt_subscribe(t, on_cmd, NULL);
    static const char *const aliases[] = { "activity/set", "send_command", "command/#", "sync", "remote/+/sleep", "remote/sleep", NULL };
    for (int i = 0; aliases[i]; i++) {
        snprintf(t, sizeof(t), "%s/%s", g_mqtt_base_topic, aliases[i]);
        mqtt_subscribe(t, on_alias, NULL);
    }
    mqtt_subscribe("harmony/ir/pronto", on_alias, NULL);
    snprintf(t, sizeof(t), "%s/status", g_mqtt_discovery_prefix);
    mqtt_subscribe(t, on_alias, NULL);
}

/* ------------------------------------------------------------------------------------------ */
/* MQTT-type devices: a pulse ("1", then "0" after mqttPulseMs) on the device's topic template  */

struct mqtt_pulse_entry {
    char topic[256];
    uint64_t off_deadline_ms;
    int active;
};
static struct mqtt_pulse_entry g_pulse_queue[16];

static void queue_pulse_off(const char *topic, int pulse_ms) {
    uint64_t deadline = mono_ms() + (pulse_ms > 0 ? (uint64_t)pulse_ms : 50ULL);
    for (int i = 0; i < 16; i++) {
        if (!g_pulse_queue[i].active) {
            snprintf(g_pulse_queue[i].topic, sizeof(g_pulse_queue[i].topic), "%s", topic);
            g_pulse_queue[i].off_deadline_ms = deadline;
            g_pulse_queue[i].active = 1;
            return;
        }
    }
    mqtt_publish_ex(topic, "0", 1, 0, 0); /* queue full: release at once */
}

static void pulse_tick(void) {
    uint64_t now = mono_ms();
    for (int i = 0; i < 16; i++) {
        if (g_pulse_queue[i].active && now >= g_pulse_queue[i].off_deadline_ms) {
            mqtt_publish_ex(g_pulse_queue[i].topic, "0", 1, 0, 0);
            g_pulse_queue[i].active = 0;
        }
    }
}

/* Replaces one {placeholder} (any of the given spellings) by value */
static void replace_token(char *buf, size_t cap, const char *const *names, const char *value) {
    for (int i = 0; names[i]; i++) {
        char *pos = strstr(buf, names[i]);
        if (!pos) continue;
        char tmp[256];
        size_t pre = (size_t)(pos - buf);
        snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)pre, buf, value, pos + strlen(names[i]));
        snprintf(buf, cap, "%.*s", (int)(cap - 1), tmp);
        return;
    }
}

int mqtt_dispatch_button_pulse_topic(const char *dev_id, const char *dev_name_in, const char *cmd_name,
                                     const char *topic_in, int req_pulse_ms,
                                     char *out_topic, size_t out_topic_len, int *out_pulse_ms) {
    if (!cmd_name || !cmd_name[0]) return -1;
    char dev_name[128] = {0};
    char topic_pattern[256] = "{root}/button/{device}/{command}";
    int pulse_ms = 1000;
    if (dev_name_in && dev_name_in[0]) snprintf(dev_name, sizeof(dev_name), "%s", dev_name_in);
    else if (dev_id && dev_id[0]) snprintf(dev_name, sizeof(dev_name), "%s", dev_id);
    else snprintf(dev_name, sizeof(dev_name), "device");

    if (topic_in && topic_in[0]) {
        snprintf(topic_pattern, sizeof(topic_pattern), "%s", topic_in);
    } else if (dev_id && dev_id[0]) {
        cJSON *droot = config_store_get_devices_json();
        cJSON *darr = droot ? cJSON_GetObjectItemCaseSensitive(droot, "devices") : NULL;
        const cJSON *di;
        if (darr) cJSON_ArrayForEach(di, darr) {
            char d_id[64];
            id_of(di, d_id, sizeof(d_id));
            const char *d_name = obj_str(di, "name");
            if (strcmp(d_id, dev_id) == 0 || (d_name[0] && strcmp(d_name, dev_id) == 0)) {
                if (!dev_name_in || !dev_name_in[0]) snprintf(dev_name, sizeof(dev_name), "%s", d_name[0] ? d_name : dev_id);
                const cJSON *jtop = cJSON_GetObjectItemCaseSensitive(di, "mqttTopic");
                if (jtop && cJSON_IsString(jtop) && jtop->valuestring && jtop->valuestring[0]) snprintf(topic_pattern, sizeof(topic_pattern), "%s", jtop->valuestring);
                const cJSON *jp = cJSON_GetObjectItemCaseSensitive(di, "mqttPulseMs");
                if (jp && cJSON_IsNumber(jp) && jp->valueint > 0) pulse_ms = jp->valueint;
                break;
            }
        }
        cJSON_Delete(droot);
    }
    if (req_pulse_ms > 0) pulse_ms = req_pulse_ms;

    char topic[256];
    snprintf(topic, sizeof(topic), "%s", topic_pattern);
    static const char *const base_names[] = { "{base_topic}", "{baseTopic}", "{base}", "{root}", NULL };
    static const char *const dev_names[] = { "{device_id}", "{deviceId}", "{device}", "{deviceName}", NULL };
    static const char *const cmd_names[] = { "{command}", "{button}", NULL };
    if (strstr(topic, "{base_topic}") || strstr(topic, "{baseTopic}") || strstr(topic, "{base}") || strstr(topic, "{root}")) {
        replace_token(topic, sizeof(topic), base_names, g_mqtt_base_topic);
    } else if (topic[0] == '/') {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s%.120s", g_mqtt_base_topic, topic);
        snprintf(topic, sizeof(topic), "%.255s", tmp);
    }
    replace_token(topic, sizeof(topic), dev_names, dev_name);
    replace_token(topic, sizeof(topic), cmd_names, cmd_name);

    if (out_topic && out_topic_len > 0) snprintf(out_topic, out_topic_len, "%s", topic);
    if (out_pulse_ms) *out_pulse_ms = pulse_ms;
    if (!g_mqtt_enabled || (!mqtt_is_connected() && mqtt_publishing_live())) return -1;
    mqtt_publish_ex(topic, "1", 1, 0, 0);
    queue_pulse_off(topic, pulse_ms);
    return 0;
}

int daemon_mqtt_button_handler(const char *device_id, const char *dev_name, const char *command) {
    return mqtt_dispatch_button_pulse_topic(device_id, dev_name, command, NULL, 0, NULL, 0, NULL);
}

/* ------------------------------------------------------------------------------------------ */
/* Bus sink and (re)connect                                                                    */

static void publish_hub_object(void) {
    cJSON *snap = hub_ws_snapshot_object(0);
    if (!snap) return;
    cJSON *hub = cJSON_DetachItemFromObjectCaseSensitive(snap, "hub");
    cJSON_Delete(snap);
    if (!hub) return;
    cJSON_DeleteItemFromObjectCaseSensitive(hub, "type");
    cJSON *typed = cJSON_CreateObject();
    cJSON_AddStringToObject(typed, "type", "hub");
    cJSON *child;
    while ((child = hub->child) != NULL) {
        cJSON_DetachItemViaPointer(hub, child);
        cJSON_AddItemToObject(typed, child->string, child);
    }
    cJSON_Delete(hub);
    char ip[64] = {0};
    get_local_ip(g_mqtt_host, ip, sizeof(ip));
    cJSON_AddStringToObject(typed, "ip", ip);
    int dev_count = 0, cmd_count = 0;
    get_device_and_command_counts(&dev_count, &cmd_count);
    cJSON_AddNumberToObject(typed, "device_count", dev_count);
    cJSON_AddNumberToObject(typed, "command_count", cmd_count);
    /* whether this Hub also publishes Home Assistant discovery entities, and where: a client on
     * MQTT alone can tell the two layers apart (a settings save reconnects, so this is current) */
    cJSON_AddBoolToObject(typed, "ha_discovery", g_mqtt_ha_discovery);
    cJSON_AddStringToObject(typed, "discovery_prefix", g_mqtt_discovery_prefix);
    cJSON_AddNumberToObject(typed, "ts", (double)time(NULL));
    const cJSON *bt = cJSON_GetObjectItemCaseSensitive(typed, "boot_time");
    g_published_boot_time = cJSON_IsNumber(bt) ? (long)bt->valuedouble : 0;
    pub_json("hub", typed, 1, 1);
    cJSON_Delete(typed);
}

/* The retained hub object carries uptime_sec and ts as of its publish, and boot_time. Published
 * before hub_sntp set the clock, its ts is from 1970 and boot_time is null; republish it once the
 * clock is set (or jumps), so a client reading the retained copy later gets a usable boot time. */
static void republish_hub_on_clock_change(void) {
    static time_t last_check = 0;
    time_t now = time(NULL);
    if (now - last_check < 5 && now >= last_check) return;
    last_check = now;
    long boot = hub_info_boot_time();
    int was_set = g_published_boot_time > 0, is_set = boot > 0;
    long diff = boot - g_published_boot_time;
    if (diff < 0) diff = -diff;
    if (was_set != is_set || (is_set && diff > 30)) {
        printf("[*] MQTT: clock %s, republishing the hub object (boot_time %ld)\n",
               was_set ? "changed" : "set", boot);
        publish_hub_object();
    }
}

static void publish_last(hub_event_type_t type, const char *suffix) {
    const cJSON *last = hub_events_last(type);
    if (!last) return;
    pub_json(suffix, last, 1, 1);
}

static void on_connected(void) {
    /* The command topics were registered before connecting (subscribe_cmd_topics) and are
     * subscribed by mqtt_connect() before it publishes "online" */
    clear_old_topics();
    publish_hub_object();
    publish_last(HUB_EV_ACTIVITY_STATE, "activity_state");
    publish_last(HUB_EV_BT_STATUS, "bt");
    publish_last(HUB_EV_RF_STATUS, "rf");
    publish_inventory(1);
    publish_discovery();
}

static void mqtt_event_sink(const hub_event_t *ev, void *user) {
    (void)user;
    if (!g_mqtt_enabled || !ev || !ev->payload) return;
    if (!mqtt_is_connected() && mqtt_publishing_live()) return; /* dropped while the broker is down */
    switch (ev->type) {
        case HUB_EV_ACTIVITY_STATE: pub_json("activity_state", ev->payload, 1, 1); break;
        case HUB_EV_ACTIVITY_PROGRESS: pub_json("progress", ev->payload, 0, 0); break;
        case HUB_EV_BT_STATUS: pub_json("bt", ev->payload, 1, 1); break;
        case HUB_EV_RF_STATUS: pub_json("rf", ev->payload, 1, 1); break;
        case HUB_EV_IR_RECEIVED: pub_json("ir/received", ev->payload, 0, 0); break;
        case HUB_EV_IR_CAPTURE: publish_hub_object(); break;   /* the hub object carries ir_capture */
        case HUB_EV_BUTTON: {
            const cJSON *slot = cJSON_GetObjectItemCaseSensitive(ev->payload, "slot");
            char suffix[48];
            if (slot && cJSON_IsNumber(slot)) snprintf(suffix, sizeof(suffix), "remote/%d/button", slot->valueint);
            else snprintf(suffix, sizeof(suffix), "button");
            pub_json(suffix, ev->payload, 0, 1);
            break;
        }
        case HUB_EV_CONFIG_CHANGED:
            publish_inventory(0);
            publish_hub_object(); /* hub_name, device and command counts may have changed */
            publish_discovery();  /* activity names in the select, hub name on the device */
            break;
        default: break;
    }
}

/* mqtt_status on the bus (WebSocket clients, the snapshot): enabled, connected, broker, port,
 * base topic and discovery flag. Emitted at start, on connect, on loss and after a settings save. */
static void emit_mqtt_status(void) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddBoolToObject(o, "enabled", g_mqtt_enabled);
    cJSON_AddBoolToObject(o, "connected", g_mqtt_enabled && mqtt_is_connected());
    cJSON_AddStringToObject(o, "broker", g_mqtt_enabled ? g_mqtt_host : "");
    cJSON_AddNumberToObject(o, "port", g_mqtt_port);
    cJSON_AddStringToObject(o, "baseTopic", g_mqtt_base_topic);
    cJSON_AddBoolToObject(o, "haDiscovery", g_mqtt_ha_discovery);
    hub_events_emit(HUB_EV_MQTT_STATUS, o);
}

int hub_mqtt_init(void) {
    hub_events_subscribe(mqtt_event_sink, NULL);
    hw_set_mqtt_button_handler(daemon_mqtt_button_handler);
    g_mqtt_enabled = load_mqtt_config();
    emit_mqtt_status();
    if (!g_mqtt_enabled) {
        printf("[*] MQTT disabled or config missing\n");
        return 0;
    }
    printf("[+] MQTT config loaded: %s:%d (user=%s, client=%s, base=%s)\n",
           g_mqtt_host, g_mqtt_port, g_mqtt_user[0] ? g_mqtt_user : "none", g_mqtt_cid, g_mqtt_base_topic);
    mqtt_init(g_mqtt_host, g_mqtt_port, g_mqtt_cid, g_mqtt_user, g_mqtt_pass);
    mqtt_set_lwt(g_status_topic, "offline", 1);
    mqtt_set_birth(g_status_topic, "online", 1);
    subscribe_cmd_topics(); /* registered now, subscribed on connect before "online" */
    printf("[*] Connecting to MQTT broker %s:%d in background...\n", g_mqtt_host, g_mqtt_port);
    mqtt_connect_async();
    return 1;
}

void daemon_sync_mqtt(void) {
    int was_enabled = g_mqtt_enabled;
    g_mqtt_enabled = load_mqtt_config();
    if (mqtt_is_connected() || was_enabled) {
        mqtt_disconnect();
        g_last_mqtt_connected = 0;
    }
    g_inv_pub_count = 0;
    g_inventory_version[0] = '\0';
    g_last_config_payload[0] = '\0';
    emit_mqtt_status();
    if (!g_mqtt_enabled) return;
    mqtt_init(g_mqtt_host, g_mqtt_port, g_mqtt_cid, g_mqtt_user, g_mqtt_pass);
    mqtt_set_lwt(g_status_topic, "offline", 1);
    mqtt_set_birth(g_status_topic, "online", 1);
    subscribe_cmd_topics(); /* mqtt_init cleared them; the base topic may have changed */
    g_mqtt_retry_interval = 5;
    g_last_mqtt_retry = 0;
    mqtt_connect_async();
}

void hub_mqtt_tick(void) {
    if (!g_mqtt_enabled) return;
    if (mqtt_is_connected()) {
        if (!g_last_mqtt_connected) {
            g_last_mqtt_connected = 1;
            g_mqtt_retry_interval = 5;
            printf("[+] Connected to MQTT broker %s:%d\n", g_mqtt_host, g_mqtt_port);
            on_connected();
            emit_mqtt_status();
        }
        mqtt_tick();
        pulse_tick();
        seq_tick();
        republish_hub_on_clock_change();
    } else {
        if (g_last_mqtt_connected) {
            g_last_mqtt_connected = 0;
            printf("[-] MQTT connection lost, will reconnect with backoff...\n");
            emit_mqtt_status();
        }
        time_t now = time(NULL);
        if (now - g_last_mqtt_retry >= g_mqtt_retry_interval) {
            g_last_mqtt_retry = now;
            if (!mqtt_is_connecting()) {
                mqtt_connect_async();
                g_mqtt_retry_interval = (g_mqtt_retry_interval * 2 > 60) ? 60 : (g_mqtt_retry_interval * 2);
            }
        }
    }
}

void hub_mqtt_shutdown(void) {
    if (g_mqtt_enabled) mqtt_disconnect();
}
