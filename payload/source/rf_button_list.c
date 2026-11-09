#include "rf_button_list.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static void put(rf_button_entry_t *e, const char *ctx, const char *label, uint8_t ctype, uint8_t atype, int idx) {
    memset(e, 0, sizeof(*e));
    snprintf(e->ctx, sizeof(e->ctx), "%s", ctx ? ctx : "-1");
    snprintf(e->label, sizeof(e->label), "%s", label ? label : "Button");
    e->ctype = ctype;
    e->atype = atype;
    e->idx = (uint8_t)(idx < 0 ? 0 : (idx > 255 ? 255 : idx));
}

int rf_build_button_list(const cJSON *acts_map, rf_button_entry_t *out, int max) {
    if (!out || max <= 0) return 0;
    rf_button_entry_t trig[RF_BUTTON_TRIGGERS_MAX + 1];
    int nt = 0, n = 0;
    int has_pwroff = 0;
    const int maps = acts_map && cJSON_IsObject(acts_map);
    const cJSON *act = NULL;

    /* Hard keys that start or stop an activity, from every activity, collected first so the
     * on-screen buttons cannot push them out */
    if (maps) {
        cJSON_ArrayForEach(act, acts_map) {
            if (!cJSON_IsObject(act)) continue;
            const char *ctx = act->string ? act->string : "-1";
            int hidx = 0;
            const cJSON *b = NULL;
            cJSON_ArrayForEach(b, act) {
                const char *name = b->string;
                if (!name || !cJSON_IsObject(b) || strcmp(name, "screen_buttons") == 0) continue;
                int pos = hidx++;
                if (strcasecmp(name, "PwrOff") == 0 || strcasecmp(name, "PowerOffActivity") == 0) has_pwroff = 1;
                const cJSON *ja = cJSON_GetObjectItemCaseSensitive(b, "action");
                const char *action = cJSON_IsString(ja) ? ja->valuestring : "device_cmd";
                if (nt >= RF_BUTTON_TRIGGERS_MAX) continue;
                if (strcmp(action, "activity_start") == 0) {
                    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(b, "activityId");
                    put(&trig[nt++], cJSON_IsString(jid) ? jid->valuestring : ctx, name, 0x00, 0x02, pos);
                } else if (strcmp(action, "activity_stop") == 0) {
                    put(&trig[nt++], "-1", name, 0x00, 0x03, pos);
                }
            }
        }
    }
    /* Without a PwrOff mapping anywhere, PwrOff stops the activity */
    if (!has_pwroff) put(&trig[nt++], "-1", "PwrOff", 0x00, 0x03, 0);
    if (nt > max) nt = max;
    /* The start/stop keys go first: a remote with an older firmware keeps only the first 48
     * entries (the order means nothing else to the remote) */
    memcpy(out, trig, (size_t)nt * sizeof(trig[0]));
    n = nt;

    /* On-screen buttons of every activity, in the room the start/stop keys leave */
    if (maps) {
        cJSON_ArrayForEach(act, acts_map) {
            if (!cJSON_IsObject(act)) continue;
            const char *ctx = act->string ? act->string : "-1";
            const cJSON *sbtns = cJSON_GetObjectItemCaseSensitive(act, "screen_buttons");
            if (!cJSON_IsArray(sbtns)) continue;
            int sidx = 0;
            const cJSON *b = NULL;
            cJSON_ArrayForEach(b, sbtns) {
                if (!cJSON_IsObject(b)) continue;
                if (n >= max) break;
                const cJSON *jl = cJSON_GetObjectItemCaseSensitive(b, "label");
                put(&out[n++], ctx, cJSON_IsString(jl) ? jl->valuestring : "Button", 0x01, 0x01, sidx++);
            }
        }
    }
    return n;
}
