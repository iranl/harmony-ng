/* Stock configuration object, see hbus_config.h. */
#define _GNU_SOURCE
#include "hbus_config.h"
#include "config_store.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ------------------------------------------------------------------------------------------ */

static uint32_t crc32_of(const char *s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        crc ^= *p;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int is_numeric_id(const char *id) {
    if (!id || !id[0]) return 0;
    const char *p = id;
    if (*p == '-') p++;
    if (!*p) return 0;
    for (; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

void hbus_numeric_id(const char *id, char *out, size_t cap) {
    if (!id) id = "";
    if (is_numeric_id(id)) snprintf(out, cap, "%s", id);
    else snprintf(out, cap, "%u", (unsigned)(crc32_of(id) & 0x7FFFFFFFu));
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

static cJSON *list_of(cJSON *root, const char *key) {
    if (!root) return NULL;
    return cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, key);
}

int hbus_resolve_id(const char *numeric, char *out, size_t cap) {
    if (!numeric) return 0;
    const char *keys[2] = { "devices", "activities" };
    cJSON *roots[2] = { config_store_get_devices_json(), config_store_get_activities_json() };
    for (int r = 0; r < 2; r++) {
        cJSON *list = list_of(roots[r], keys[r]);
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, list) {
            char id[32], num[32];
            id_of(it, id, sizeof(id));
            if (!id[0]) continue;
            hbus_numeric_id(id, num, sizeof(num));
            if (strcmp(num, numeric) == 0) {
                snprintf(out, cap, "%s", id);
                return 1;
            }
        }
    }
    snprintf(out, cap, "%s", numeric);
    return 0;
}

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

void hbus_inventory_version(char out[9]) {
    size_t dlen = 0, alen = 0;
    char *dtxt = read_text(HARMONY_DEVICES_FILE, &dlen);
    char *atxt = read_text(HARMONY_ACTIVITIES_FILE, &alen);
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    if (dtxt) sha256_update(&ctx, (const uint8_t *)dtxt, dlen);
    if (atxt) sha256_update(&ctx, (const uint8_t *)atxt, alen);
    uint8_t digest[32];
    sha256_final(digest, &ctx);
    snprintf(out, 9, "%02x%02x%02x%02x", digest[0], digest[1], digest[2], digest[3]);
    free(dtxt);
    free(atxt);
}

/* ------------------------------------------------------------------------------------------ */

const char *hbus_control_group(const char *c) {
    if (!c || !c[0]) return "Miscellaneous";
    if (strncasecmp(c, "Power", 5) == 0) return "Power";
    if (strncasecmp(c, "Volume", 6) == 0 || strcasecmp(c, "Mute") == 0) return "Volume";
    if (strncasecmp(c, "Channel", 7) == 0 || strcasecmp(c, "PrevChannel") == 0) return "Channel";
    if (strlen(c) == 1 && isdigit((unsigned char)c[0])) return "NumericBasic";
    if (strcasecmp(c, "Play") == 0 || strcasecmp(c, "Pause") == 0 || strcasecmp(c, "Stop") == 0 ||
        strcasecmp(c, "Rewind") == 0 || strcasecmp(c, "FastForward") == 0 || strcasecmp(c, "Record") == 0 ||
        strcasecmp(c, "SkipForward") == 0 || strcasecmp(c, "SkipBackward") == 0 || strcasecmp(c, "PlayPause") == 0) return "TransportBasic";
    if (strncasecmp(c, "Direction", 9) == 0 || strcasecmp(c, "Select") == 0 || strcasecmp(c, "OK") == 0 ||
        strcasecmp(c, "Back") == 0 || strcasecmp(c, "Menu") == 0 || strcasecmp(c, "Exit") == 0 ||
        strcasecmp(c, "Home") == 0 || strcasecmp(c, "Guide") == 0 || strcasecmp(c, "Info") == 0) return "NavigationBasic";
    return "Miscellaneous";
}

/* {"name","label","action":"{\"command\":...,\"type\":\"IRCommand\",\"deviceId\":...}"} */
static cJSON *make_function(const char *cmd, const char *dev_numeric) {
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "name", cmd);
    cJSON_AddStringToObject(f, "label", cmd);
    cJSON *act = cJSON_CreateObject();
    cJSON_AddStringToObject(act, "command", cmd);
    cJSON_AddStringToObject(act, "type", "IRCommand");
    cJSON_AddStringToObject(act, "deviceId", dev_numeric);
    char *txt = cJSON_PrintUnformatted(act);
    cJSON_Delete(act);
    cJSON_AddStringToObject(f, "action", txt ? txt : "");
    free(txt);
    return f;
}

/* Adds (cmd, device) to the group array, creating the group when needed */
static void add_to_groups(cJSON *groups, const char *cmd, const char *dev_numeric) {
    const char *gname = hbus_control_group(cmd);
    cJSON *g = NULL;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, groups) {
        if (strcmp(obj_str(it, "name"), gname) == 0) { g = it; break; }
    }
    if (!g) {
        g = cJSON_CreateObject();
        cJSON_AddStringToObject(g, "name", gname);
        cJSON_AddArrayToObject(g, "function");
        cJSON_AddItemToArray(groups, g);
    }
    cJSON *fn = cJSON_GetObjectItemCaseSensitive(g, "function");
    /* one entry per command name */
    cJSON *f = NULL;
    cJSON_ArrayForEach(f, fn) {
        if (strcmp(obj_str(f, "name"), cmd) == 0) return;
    }
    cJSON_AddItemToArray(fn, make_function(cmd, dev_numeric));
}

static cJSON *device_control_groups(const cJSON *dev, const char *dev_numeric) {
    cJSON *groups = cJSON_CreateArray();
    const cJSON *c = NULL;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(dev, "commands")) {
        const char *nm = cJSON_IsString(c) ? c->valuestring : obj_str(c, "name");
        if (nm && nm[0]) add_to_groups(groups, nm, dev_numeric);
    }
    return groups;
}

static const cJSON *find_device(cJSON *devs, const char *id) {
    cJSON *d = NULL;
    cJSON_ArrayForEach(d, devs) {
        char did[32];
        id_of(d, did, sizeof(did));
        if (strcmp(did, id) == 0) return d;
    }
    return NULL;
}

/* Device ids an activity uses: its sequences' steps plus an optional deviceIds list */
int hbus_activity_devices(const cJSON *act, char ids[16][32]) {
    int n = 0;
    const char *seqs[2] = { "startSequence", "stopSequence" };
    for (int s = 0; s < 2; s++) {
        const cJSON *st = NULL;
        cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(act, seqs[s])) {
            char did[32];
            const cJSON *j = cJSON_GetObjectItemCaseSensitive(st, "deviceId");
            if (j && cJSON_IsString(j) && j->valuestring) snprintf(did, sizeof(did), "%s", j->valuestring);
            else if (j && cJSON_IsNumber(j)) snprintf(did, sizeof(did), "%ld", (long)j->valuedouble);
            else continue;
            int dup = 0;
            for (int i = 0; i < n; i++) if (strcmp(ids[i], did) == 0) dup = 1;
            if (!dup && n < 16) snprintf(ids[n++], 32, "%s", did);
        }
    }
    const cJSON *extra = NULL;
    cJSON_ArrayForEach(extra, cJSON_GetObjectItemCaseSensitive(act, "deviceIds")) {
        if (!cJSON_IsString(extra) || !extra->valuestring) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (strcmp(ids[i], extra->valuestring) == 0) dup = 1;
        if (!dup && n < 16) snprintf(ids[n++], 32, "%s", extra->valuestring);
    }
    return n;
}

static cJSON *make_activity(const cJSON *act, cJSON *devs, int is_power_off) {
    cJSON *a = cJSON_CreateObject();
    char id[32], num[32];
    if (is_power_off) {
        snprintf(num, sizeof(num), "-1");
        cJSON_AddStringToObject(a, "id", "-1");
        cJSON_AddStringToObject(a, "label", "PowerOff");
        cJSON_AddStringToObject(a, "type", "PowerOff");
        cJSON_AddNumberToObject(a, "activityOrder", 0);
    } else {
        id_of(act, id, sizeof(id));
        hbus_numeric_id(id, num, sizeof(num));
        cJSON_AddStringToObject(a, "id", num);
        cJSON_AddStringToObject(a, "label", obj_str(act, "name"));
        const char *type = obj_str(act, "type");
        cJSON_AddStringToObject(a, "type", type[0] ? type : "VirtualGeneric");
        const cJSON *ord = cJSON_GetObjectItemCaseSensitive(act, "order");
        cJSON_AddNumberToObject(a, "activityOrder", (ord && cJSON_IsNumber(ord)) ? ord->valueint : 0);
    }
    cJSON_AddBoolToObject(a, "isTuningDefault", 0);
    cJSON *groups = cJSON_AddArrayToObject(a, "controlGroup");
    cJSON *fixit = cJSON_AddObjectToObject(a, "fixit");
    if (!is_power_off) {
        char ids[16][32];
        int n = hbus_activity_devices(act, ids);
        for (int i = 0; i < n; i++) {
            const cJSON *dev = find_device(devs, ids[i]);
            if (!dev) continue;
            char dnum[32];
            hbus_numeric_id(ids[i], dnum, sizeof(dnum));
            const cJSON *c = NULL;
            cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(dev, "commands")) {
                const char *nm = cJSON_IsString(c) ? c->valuestring : obj_str(c, "name");
                if (nm && nm[0]) add_to_groups(groups, nm, dnum);
            }
            cJSON *fx = cJSON_CreateObject();
            cJSON_AddStringToObject(fx, "id", dnum);
            cJSON_AddStringToObject(fx, "Power", "On");
            cJSON_AddStringToObject(fx, "Input", "");
            cJSON_AddBoolToObject(fx, "isRelativePower", 0);
            cJSON_AddStringToObject(fx, "isManualPower", "false");
            cJSON_AddItemToObject(fixit, dnum, fx);
        }
    }
    cJSON_AddArrayToObject(a, "sequences");
    cJSON_AddArrayToObject(a, "rules");
    return a;
}

cJSON *hbus_build_config(const char *remote_id, const char *inventory_version) {
    cJSON *devs = list_of(config_store_get_devices_json(), "devices");
    cJSON *acts = list_of(config_store_get_activities_json(), "activities");
    cJSON *root = cJSON_CreateObject();
    cJSON *activity = cJSON_AddArrayToObject(root, "activity");
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, acts) {
        char id[32];
        id_of(a, id, sizeof(id));
        if (!id[0]) continue;
        cJSON_AddItemToArray(activity, make_activity(a, devs, 0));
    }
    cJSON_AddItemToArray(activity, make_activity(NULL, devs, 1));

    cJSON *device = cJSON_AddArrayToObject(root, "device");
    cJSON *d = NULL;
    cJSON_ArrayForEach(d, devs) {
        char id[32], num[32];
        id_of(d, id, sizeof(id));
        if (!id[0]) continue;
        hbus_numeric_id(id, num, sizeof(num));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", num);
        cJSON_AddStringToObject(o, "label", obj_str(d, "name"));
        const char *type = obj_str(d, "type");
        cJSON_AddStringToObject(o, "type", type[0] ? type : "Other");
        cJSON_AddStringToObject(o, "manufacturer", obj_str(d, "manufacturer"));
        cJSON_AddStringToObject(o, "model", obj_str(d, "model"));
        cJSON_AddStringToObject(o, "deviceTypeDisplayName", type[0] ? type : "Other");
        cJSON_AddItemToObject(o, "controlGroup", device_control_groups(d, num));
        cJSON_AddStringToObject(o, "isManualPower", "false");
        cJSON_AddBoolToObject(o, "IsKeyboardAssociated", 0);
        cJSON_AddArrayToObject(o, "Capabilities");
        cJSON_AddNumberToObject(o, "DongleRFID", 0);
        cJSON_AddItemToArray(device, o);
    }

    cJSON *global = cJSON_AddObjectToObject(root, "global");
    char ts[80];
    snprintf(ts, sizeof(ts), "%s;%s", inventory_version ? inventory_version : "", remote_id ? remote_id : "");
    cJSON_AddStringToObject(global, "timeStampHash", ts);
    cJSON_AddStringToObject(global, "locale", "en-US");
    cJSON_AddObjectToObject(root, "content");
    cJSON_AddArrayToObject(root, "sequence");
    return root;
}
