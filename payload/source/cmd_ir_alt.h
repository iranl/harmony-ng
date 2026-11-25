#ifndef CMD_IR_ALT_H
#define CMD_IR_ALT_H

/* The infrared alternative of a plugin device's command (docs/PLUGINS.md): "irKeyCode" is an
 * infrared code the Hub's IR path takes (a Harmony keycode, a learned raw code, Pronto hex) and
 * "irWhen" the key of a boolean field of the device's plugin config. While that field is true
 * the Hub sends the command as infrared instead of handing it to the plugin (a Samsung TV that
 * ignores the HDMI keys of its network API). Both are kept through device saves and reloads,
 * like "range" (cmd_range.h). */

#include "cJSON.h"
#include <string.h>
#include <strings.h>

#define CMD_IR_WHEN_MAX 63

/* An irKeyCode: any non-empty text, as a keyCode */
static inline int cmd_ir_keycode_ok(const cJSON *j) {
    return cJSON_IsString(j) && j->valuestring && j->valuestring[0];
}

/* An irWhen: a config key of 1..63 letters, digits, '_' or '-' */
static inline int cmd_ir_when_ok(const cJSON *j) {
    if (!cJSON_IsString(j) || !j->valuestring) return 0;
    size_t n = 0;
    for (const char *p = j->valuestring; *p; p++, n++) {
        char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
            return 0;
    }
    return n >= 1 && n <= CMD_IR_WHEN_MAX;
}

/* "irKeyCode": null or "" in a request: remove it */
static inline int cmd_ir_field_is_clear(const cJSON *j) {
    return cJSON_IsNull(j) || (cJSON_IsString(j) && j->valuestring && !j->valuestring[0]);
}

/* Copies a valid irKeyCode / irWhen of src to dst (replacing dst's); null or "" removes dst's;
 * a missing one keeps it, an invalid one is left out */
static inline void cmd_ir_alt_copy(cJSON *dst, const cJSON *src) {
    static const char *const keys[2] = { "irKeyCode", "irWhen" };
    for (int i = 0; i < 2; i++) {
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(src, keys[i]);
        if (!j) continue;
        if (cmd_ir_field_is_clear(j)) {
            cJSON_DeleteItemFromObjectCaseSensitive(dst, keys[i]);
            continue;
        }
        if (i == 0 ? !cmd_ir_keycode_ok(j) : !cmd_ir_when_ok(j)) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(dst, keys[i]);
        cJSON_AddStringToObject(dst, keys[i], j->valuestring);
    }
}

/* 1 when a request names irKeyCode or irWhen (to set or to clear them) */
static inline int cmd_ir_alt_in_request(const cJSON *src) {
    return cJSON_GetObjectItemCaseSensitive(src, "irKeyCode") || cJSON_GetObjectItemCaseSensitive(src, "irWhen");
}

/* 1 when the command has a valid irKeyCode (it then keeps IR fields: port, repeats) */
static inline int cmd_has_ir_alt(const cJSON *cmd) {
    return cmd_ir_keycode_ok(cJSON_GetObjectItemCaseSensitive(cmd, "irKeyCode"));
}

/* A command without irKeyCode and irWhen takes those of the manifest's default_commands entry
 * of the same name (dev_def: the plugin's device_definition) */
static inline void cmd_ir_alt_fill_from_manifest(cJSON *cmd, const cJSON *dev_def) {
    const cJSON *cn = cJSON_GetObjectItemCaseSensitive(cmd, "name");
    if (!cJSON_IsString(cn) || !dev_def || cJSON_GetObjectItemCaseSensitive(cmd, "irKeyCode") ||
        cJSON_GetObjectItemCaseSensitive(cmd, "irWhen")) return;
    const cJSON *dc = cJSON_GetObjectItemCaseSensitive(dev_def, "default_commands");
    const cJSON *d = NULL;
    cJSON_ArrayForEach(d, (cJSON_IsArray(dc) ? dc : NULL)) {
        const cJSON *dn = cJSON_GetObjectItemCaseSensitive(d, "name");
        if (cJSON_IsString(dn) && strcmp(dn->valuestring, cn->valuestring) == 0) {
            cmd_ir_alt_copy(cmd, d);
            return;
        }
    }
}

/* A device save replaces the device's commands with the request's. Each of irKeyCode and irWhen
 * that a command of the request leaves out keeps the stored command's value (same name), so a
 * client that sends back only name and keyCode, or changes only the code, keeps the other one;
 * null or "" removes it. stored: the commands before the save */
static inline void cmd_ir_alt_keep_stored(cJSON *cmds, const cJSON *stored) {
    static const char *const keys[2] = { "irKeyCode", "irWhen" };
    cJSON *c = NULL;
    cJSON_ArrayForEach(c, (cJSON_IsArray(cmds) && cJSON_IsArray(stored) ? cmds : NULL)) {
        const cJSON *cn = cJSON_GetObjectItemCaseSensitive(c, "name");
        if (!cJSON_IsObject(c) || !cJSON_IsString(cn)) continue;
        const cJSON *s = NULL;
        cJSON_ArrayForEach(s, stored) {
            const cJSON *sn = cJSON_GetObjectItemCaseSensitive(s, "name");
            if (!cJSON_IsString(sn) || strcmp(sn->valuestring, cn->valuestring) != 0) continue;
            for (int i = 0; i < 2; i++) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(s, keys[i]);
                if (cJSON_GetObjectItemCaseSensitive(c, keys[i]) || !cJSON_IsString(v)) continue;
                if (i == 0 ? !cmd_ir_keycode_ok(v) : !cmd_ir_when_ok(v)) continue;
                cJSON_AddStringToObject(c, keys[i], v->valuestring);
            }
            break;
        }
    }
}

/* A config value that turns the infrared alternative on: true, a number other than 0, "true"
 * or "1" */
static inline int cmd_ir_switch_on(const cJSON *v) {
    if (cJSON_IsTrue(v)) return 1;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v) && v->valuestring)
        return strcasecmp(v->valuestring, "true") == 0 || strcmp(v->valuestring, "1") == 0;
    return 0;
}

/* The infrared code to send instead of the plugin command, or NULL: the command has an
 * irKeyCode and an irWhen, and the device's config[irWhen] is on. Reads only the device as
 * held in memory (the key path reads no file). */
static inline const char *cmd_ir_alt_active(const cJSON *dev, const cJSON *cmd) {
    const cJSON *kc = cJSON_GetObjectItemCaseSensitive(cmd, "irKeyCode");
    const cJSON *when = cJSON_GetObjectItemCaseSensitive(cmd, "irWhen");
    if (!cmd_ir_keycode_ok(kc) || !cmd_ir_when_ok(when)) return NULL;
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(dev, "config");
    if (!cJSON_IsObject(cfg)) return NULL;
    return cmd_ir_switch_on(cJSON_GetObjectItemCaseSensitive(cfg, when->valuestring)) ? kc->valuestring : NULL;
}

#endif
