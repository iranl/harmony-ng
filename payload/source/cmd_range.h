#ifndef CMD_RANGE_H
#define CMD_RANGE_H

/* A command's optional "range": [min, max], the values its plugin accepts (a slider, a level).
 * Kept through device saves and reloads; hw_action refuses a value outside it. */

#include "cJSON.h"
#include <string.h>

/* 1 for an array of two integers with min <= max */
static inline int cmd_range_valid(const cJSON *r) {
    if (!cJSON_IsArray(r) || cJSON_GetArraySize(r) != 2) return 0;
    const cJSON *lo = cJSON_GetArrayItem(r, 0), *hi = cJSON_GetArrayItem(r, 1);
    if (!cJSON_IsNumber(lo) || !cJSON_IsNumber(hi)) return 0;
    if (lo->valuedouble != (double)lo->valueint || hi->valuedouble != (double)hi->valueint) return 0;
    return lo->valueint <= hi->valueint;
}

/* "range": null or [] in a request: remove the command's range */
static inline int cmd_range_is_clear(const cJSON *r) {
    return cJSON_IsNull(r) || (cJSON_IsArray(r) && cJSON_GetArraySize(r) == 0);
}

/* 1 when src has no "range", a valid one, or a clear one (requests: anything else is a 400) */
static inline int cmd_range_request_ok(const cJSON *src) {
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(src, "range");
    return !r || cmd_range_valid(r) || cmd_range_is_clear(r);
}

/* Copies a valid "range" of src to dst (replacing dst's); null or [] removes dst's; a missing
 * one keeps it, an invalid one is left out */
static inline void cmd_range_copy(cJSON *dst, const cJSON *src) {
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(src, "range");
    if (cmd_range_is_clear(r)) {
        cJSON_DeleteItemFromObjectCaseSensitive(dst, "range");
        return;
    }
    if (!cmd_range_valid(r)) return;
    cJSON_DeleteItemFromObjectCaseSensitive(dst, "range");
    cJSON *n = cJSON_CreateArray();
    cJSON_AddItemToArray(n, cJSON_CreateNumber(cJSON_GetArrayItem(r, 0)->valueint));
    cJSON_AddItemToArray(n, cJSON_CreateNumber(cJSON_GetArrayItem(r, 1)->valueint));
    cJSON_AddItemToObject(dst, "range", n);
}

/* A command without "range" takes the one of the manifest's default_commands entry of the same
 * name (dev_def: the plugin's device_definition); nothing when the command has one */
static inline void cmd_range_fill_from_manifest(cJSON *cmd, const cJSON *dev_def) {
    const cJSON *cn = cJSON_GetObjectItemCaseSensitive(cmd, "name");
    if (!cJSON_IsString(cn) || cJSON_GetObjectItemCaseSensitive(cmd, "range") || !dev_def) return;
    const cJSON *dc = cJSON_GetObjectItemCaseSensitive(dev_def, "default_commands");
    const cJSON *d = NULL;
    cJSON_ArrayForEach(d, (cJSON_IsArray(dc) ? dc : NULL)) {
        const cJSON *dn = cJSON_GetObjectItemCaseSensitive(d, "name");
        if (cJSON_IsString(dn) && strcmp(dn->valuestring, cn->valuestring) == 0) {
            cmd_range_copy(cmd, d);
            return;
        }
    }
}

/* A device save replaces the device's commands with the request's. A command of the request
 * without a "range" key keeps the range of the stored command of the same name (a client that
 * sends back only name and keyCode must not drop it); "range": null or [] removes it, a new
 * name gets nothing here. stored: the commands before the save */
static inline void cmd_ranges_keep_stored(cJSON *cmds, const cJSON *stored) {
    cJSON *c = NULL;
    cJSON_ArrayForEach(c, (cJSON_IsArray(cmds) && cJSON_IsArray(stored) ? cmds : NULL)) {
        const cJSON *cn = cJSON_GetObjectItemCaseSensitive(c, "name");
        if (!cJSON_IsObject(c) || !cJSON_IsString(cn) || cJSON_GetObjectItemCaseSensitive(c, "range")) continue;
        const cJSON *s = NULL;
        cJSON_ArrayForEach(s, stored) {
            const cJSON *sn = cJSON_GetObjectItemCaseSensitive(s, "name");
            if (cJSON_IsString(sn) && strcmp(sn->valuestring, cn->valuestring) == 0) {
                if (cmd_range_valid(cJSON_GetObjectItemCaseSensitive(s, "range"))) cmd_range_copy(c, s);
                break;
            }
        }
    }
}

#endif
