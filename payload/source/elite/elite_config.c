#include "elite_config.h"
#include "elite_rf.h"
#include "elite_power.h"

void map_icon_type_to_str(uint8_t type, const char *name, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    switch (type) {
        case 1:  strncpy(out, "watchtv", out_len - 1); break;
        case 2:  strncpy(out, "movie", out_len - 1); break;
        case 3:  strncpy(out, "music", out_len - 1); break;
        case 4:  strncpy(out, "game", out_len - 1); break;
        case 5:  strncpy(out, "ps", out_len - 1); break;
        case 6:  strncpy(out, "xbox", out_len - 1); break;
        case 7:  strncpy(out, "switch", out_len - 1); break;
        case 8:  strncpy(out, "shield", out_len - 1); break;
        case 9:  strncpy(out, "apple", out_len - 1); break;
        case 10: strncpy(out, "roku", out_len - 1); break;
        case 11: strncpy(out, "firetv", out_len - 1); break;
        case 12: strncpy(out, "pc", out_len - 1); break;
        case 13: strncpy(out, "smarttv", out_len - 1); break;
        case 14: strncpy(out, "sonos", out_len - 1); break;
        case 15: strncpy(out, "receiver", out_len - 1); break;
        case 16: strncpy(out, "light", out_len - 1); break;
        case 17: strncpy(out, "power", out_len - 1); break;
        default:
            if (name && name[0]) {
                char low[64] = {0};
                for (int k = 0; name[k] && k < 63; k++) low[k] = tolower((unsigned char)name[k]);
                if (strstr(low, "switch") || strstr(low, "nintendo")) strncpy(out, "switch", out_len - 1);
                else strncpy(out, name, out_len - 1);
            } else {
                strncpy(out, "watchtv", out_len - 1);
            }
            break;
    }
    out[out_len - 1] = '\0';
}

void resolve_activity_icon(const char *image_key, const char *clean_name, char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return;
    out_path[0] = '\0';

    char base[64] = {0};
    if (image_key && image_key[0]) {
        const char *slash = strrchr(image_key, '/');
        const char *fn = slash ? slash + 1 : image_key;
        strncpy(base, fn, sizeof(base) - 1);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        size_t blen = strlen(base);
        if (blen > 6 && strcmp(base + blen - 6, "_small") == 0) {
            base[blen - 6] = '\0';
        }
    }

    if (base[0]) {
        char test_path[128];
        snprintf(test_path, sizeof(test_path), "/data/assets/custom/activity_%s.png", base);
        if (access(test_path, R_OK) == 0) {
            strncpy(out_path, test_path, max_len - 1);
            return;
        }
        snprintf(test_path, sizeof(test_path), "/data/assets/custom/%s.png", base);
        if (access(test_path, R_OK) == 0) {
            strncpy(out_path, test_path, max_len - 1);
            return;
        }
    }
    if (clean_name && clean_name[0]) {
        char test_path[128];
        snprintf(test_path, sizeof(test_path), "/data/assets/custom/activity_%s.png", clean_name);
        if (access(test_path, R_OK) == 0) {
            strncpy(out_path, test_path, max_len - 1);
            return;
        }
        snprintf(test_path, sizeof(test_path), "/data/assets/custom/%s.png", clean_name);
        if (access(test_path, R_OK) == 0) {
            strncpy(out_path, test_path, max_len - 1);
            return;
        }
    }

    char low[64] = {0};
    const char *src = base[0] ? base : (clean_name ? clean_name : "");
    for (int k = 0; src[k] && k < 63; k++) low[k] = tolower((unsigned char)src[k]);

    if (strstr(low, "watchtv") || strstr(low, "tv") || strstr(low, "television")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/3BF99E7B-9904-4AFA-8D3D-7337CD8E9BFD_small.png");
    } else if (strstr(low, "movie") || strstr(low, "cinema") || strstr(low, "stream") || strstr(low, "netflix") || strstr(low, "plex")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/BAB09C4A-E61D-4284-B1F3-84135F562217_small.png");
    } else if (strstr(low, "switch") || strstr(low, "nintendo") || strstr(low, "game") || strstr(low, "console")) {
        if (access("/data/assets/switch.png", R_OK) == 0) {
            snprintf(out_path, max_len, "/data/assets/switch.png");
        } else if (access("/opt/luadraws/appscommon/asset/hopsactivity/9D24EB35-468B-4148-A6A2-BB764AA9609C_small.png", R_OK) == 0) {
            snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/hopsactivity/9D24EB35-468B-4148-A6A2-BB764AA9609C_small.png");
        } else {
            snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/9D24EB35-468B-4148-A6A2-BB764AA9609C_small.png");
        }
    } else if (strstr(low, "ps") || strstr(low, "playstation")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/DEC07396-B858-47E8-AE50-4AB346AF1D98_small.png");
    } else if (strstr(low, "xbox")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/xbox.png");
    } else if (strstr(low, "shield")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/shield.png");
    } else if (strstr(low, "music") || strstr(low, "audio") || strstr(low, "radio") || strstr(low, "spotify")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/72D0AAB4-2ED8-4644-84D2-0031BDAE356E_small.png");
    } else if (strstr(low, "apple")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/0F40F257-68C8-4F24-9561-3CB3441DA395_small.png");
    } else if (strstr(low, "roku")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/6EBD03B8-C815-48BE-ADBB-D36E06CAAC5E_small.png");
    } else if (strstr(low, "firetv") || strstr(low, "amazon") || strstr(low, "fire")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/5F5E0C9D-1D5A-4F6F-9DD4-243AFCCD8285_small.png");
    } else if (strstr(low, "sonos")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/sonos.png");
    } else if (strstr(low, "heos") || strstr(low, "denon")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/heos.png");
    } else if (strstr(low, "pc") || strstr(low, "computer") || strstr(low, "laptop")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/468668AB-2587-4662-9E89-CF66BD70F314_small.png");
    } else if (strstr(low, "power") || strstr(low, "off")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/116.png");
    } else {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/53469038-7E1C-4241-B9BD-BAE95CC0509C_small.png");
    }
}

bool resolve_button_icon(const char *icon_key, const char *label, char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return false;
    out_path[0] = '\0';

    char low[48] = {0};
    const char *src = (icon_key && icon_key[0]) ? icon_key : (label ? label : "");
    for (int k = 0; src[k] && k < 47; k++) low[k] = tolower((unsigned char)src[k]);

    if (src[0]) {
        char test_path[128];
        snprintf(test_path, sizeof(test_path), "/data/assets/custom/%s.png", src);
        if (access(test_path, R_OK) == 0) {
            strncpy(out_path, test_path, max_len - 1);
            return true;
        }
        snprintf(test_path, sizeof(test_path), "/data/assets/custom/button_%s.png", src);
        if (access(test_path, R_OK) == 0) {
            strncpy(out_path, test_path, max_len - 1);
            return true;
        }
        snprintf(test_path, sizeof(test_path), "/data/assets/custom/device_%s.png", src);
        if (access(test_path, R_OK) == 0) {
            strncpy(out_path, test_path, max_len - 1);
            return true;
        }
    }

    bool is_num = (low[0] >= '0' && low[0] <= '9');
    for (int i = 0; low[i]; i++) {
        if (low[i] < '0' || low[i] > '9') { is_num = false; break; }
    }
    if (is_num && low[0]) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/%s.png", low);
        return true;
    }

    if (strstr(low, "power") || strstr(low, "pwr") || strstr(low, "standby")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/116.png");
    } else if (strstr(low, "playpause")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/playpause.png");
    } else if (strstr(low, "play")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/113.png");
    } else if (strstr(low, "pause")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/106.png");
    } else if (strstr(low, "stop")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/footer/close.png");
    } else if (strstr(low, "ff") || strstr(low, "fastforward") || strstr(low, "fwd")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/76.png");
    } else if (strstr(low, "rew") || strstr(low, "rewind")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/131.png");
    } else if (strstr(low, "next") || strstr(low, "skipfwd") || strstr(low, "skip+")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/144.png");
    } else if (strstr(low, "prev") || strstr(low, "skiprev") || strstr(low, "skip-")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/143.png");
    } else if (strstr(low, "mute")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/91.png");
    } else if (strstr(low, "vol+") || strstr(low, "volup") || strstr(low, "volume up")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/171.png");
    } else if (strstr(low, "vol-") || strstr(low, "voldn") || strstr(low, "volume down")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/166.png");
    } else if (strstr(low, "ch+") || strstr(low, "channel up")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/52.png");
    } else if (strstr(low, "ch-") || strstr(low, "channel down")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/51.png");
    } else if (strstr(low, "home")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/195.png");
    } else if (strstr(low, "menu")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/90.png");
    } else if (strstr(low, "back") || strstr(low, "return")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/39.png");
    } else if (strstr(low, "exit")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/75.png");
    } else if (strstr(low, "info")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/85.png");
    } else if (strstr(low, "search")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/135.png");
    } else if (strstr(low, "denon") || strstr(low, "avr") || strstr(low, "receiver") || strstr(low, "amp")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/5.png");
    } else if (strstr(low, "shield")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/shield.png");
    } else if (strstr(low, "ps") || strstr(low, "playstation")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/ps.png");
    } else if (strstr(low, "xbox")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/xbox.png");
    } else if (strstr(low, "tv")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/1.png");
    } else if (strstr(low, "sonos")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/sonos.png");
    } else if (strstr(low, "firetv") || strstr(low, "fire")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/firetv.png");
    } else if (strstr(low, "light") || strstr(low, "lamp") || strstr(low, "bulb")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperHomeAutomation/bulb_on.png");
    } else if (strstr(low, "plug")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperHomeAutomation/plug_on.png");
    } else if (strstr(low, "blind") || strstr(low, "shade")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperHomeAutomation/blinds_on.png");
    } else if (strstr(low, "numpad") || strstr(low, "123")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/numpad.png");
    } else if (strstr(low, "seq") || strstr(low, "macro")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/sequence.png");
    } else if (strstr(low, "red")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/127.png");
    } else if (strstr(low, "green")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/83.png");
    } else if (strstr(low, "yellow")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/172.png");
    } else if (strstr(low, "blue")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/40.png");
    } else {
        return false;
    }
    return true;
}

void load_activities(void) {
    g_activity_count = 0;
    const char *act_path = ACTIVITY_JSON_FILE;
    if (access(act_path, F_OK) != 0) act_path = "/data/harmony/config/activities.json";
    if (access(act_path, F_OK) != 0) act_path = STOCK_ACTIVITY_JSON_FILE;
    FILE *f = fopen(act_path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return; }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *acts = cJSON_GetObjectItem(root, "activities");
    if (!acts) acts = cJSON_GetObjectItem(root, "Activities");
    if (acts && cJSON_IsArray(acts)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, acts) {
            if (g_activity_count >= MAX_ACTIVITIES - 1) break;
            cJSON *j_name = cJSON_GetObjectItem(item, "name");
            if (!j_name) j_name = cJSON_GetObjectItem(item, "Name");
            cJSON *j_id = cJSON_GetObjectItem(item, "id");
            if (!j_id) j_id = cJSON_GetObjectItem(item, "Id");
            cJSON *j_type = cJSON_GetObjectItem(item, "type");
            if (!j_type) j_type = cJSON_GetObjectItem(item, "Type");
            cJSON *j_icon = cJSON_GetObjectItem(item, "icon");
            if (!j_icon) j_icon = cJSON_GetObjectItem(item, "Icon");

            if (j_name && j_id) {
                const char *name = j_name->valuestring;
                const char *id = j_id->valuestring;
                if (!name || !id) continue;
                if (strcmp(id, "-1") == 0) continue;

                strncpy(g_activities[g_activity_count].id, id, sizeof(g_activities[0].id) - 1);
                strncpy(g_activities[g_activity_count].name, name, sizeof(g_activities[0].name) - 1);

                char cpath[128];
                snprintf(cpath, sizeof(cpath), "/data/assets/custom/activity_%s.png", id);
                if (access(cpath, R_OK) != 0) {
                    snprintf(cpath, sizeof(cpath), "/data/assets/custom/%s.png", id);
                }
                if (access(cpath, R_OK) == 0) {
                    strncpy(g_activities[g_activity_count].icon_path, cpath, sizeof(g_activities[0].icon_path) - 1);
                } else {
                    const char *img_key = (j_icon && j_icon->valuestring) ? j_icon->valuestring : (j_type ? j_type->valuestring : "");
                    resolve_activity_icon(img_key, name, g_activities[g_activity_count].icon_path, sizeof(g_activities[0].icon_path));
                }
                g_activity_count++;
            }
        }
    }
    cJSON_Delete(root);

    if (g_activity_count > 0) {
        bool has_power_off = false;
        for (int i = 0; i < g_activity_count; i++) {
            if (strcmp(g_activities[i].id, "-1") == 0) { has_power_off = true; break; }
        }
        if (!has_power_off && g_activity_count < MAX_ACTIVITIES) {
            strncpy(g_activities[g_activity_count].id, "-1", sizeof(g_activities[0].id) - 1);
            strncpy(g_activities[g_activity_count].name, "Power Off", sizeof(g_activities[0].name) - 1);
            strncpy(g_activities[g_activity_count].icon_path, "/opt/luadraws/appscommon/asset/commands/116.png", sizeof(g_activities[0].icon_path) - 1);
            g_activity_count++;
        }
    }
}

void init_act_triggers(void) {
    g_act_trigger_count = 1;
    strncpy(g_act_triggers[0].button_name, "PwrOff", sizeof(g_act_triggers[0].button_name) - 1);
    g_act_triggers[0].rf_key_code = 0x01EC;
    g_act_triggers[0].atype = 0x03;
    strncpy(g_act_triggers[0].target_act_id, "-1", sizeof(g_act_triggers[0].target_act_id) - 1);
}

void save_rf_config(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddNumberToObject(root, "version", g_config_version);
    cJSON_AddNumberToObject(root, "slot", g_remote_slot);

    cJSON *acts = cJSON_AddArrayToObject(root, "activities");
    for (int i = 0; i < g_rf_activity_count; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "id", g_rf_activities[i].id);
        cJSON_AddStringToObject(a, "name", g_rf_activities[i].name);
        if (g_rf_activities[i].icon[0]) {
            cJSON_AddStringToObject(a, "icon", g_rf_activities[i].icon);
        }
        cJSON_AddItemToArray(acts, a);
    }

    cJSON *devs = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < g_rf_device_count; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "id", g_rf_devices[i].id);
        cJSON_AddStringToObject(d, "name", g_rf_devices[i].name);
        int max_idx = -1;
        for (int c = 0; c < g_rf_dev_cmd_count; c++) {
            if (g_rf_dev_cmds[c].dev_idx == i && g_rf_dev_cmds[c].cmd_idx > max_idx) max_idx = g_rf_dev_cmds[c].cmd_idx;
        }
        if (max_idx >= 0) {
            cJSON *cmds = cJSON_AddArrayToObject(d, "commands");
            for (int k = 0; k <= max_idx; k++) {
                const char *cname = "";
                for (int c = 0; c < g_rf_dev_cmd_count; c++) {
                    if (g_rf_dev_cmds[c].dev_idx == i && g_rf_dev_cmds[c].cmd_idx == k) { cname = g_rf_dev_cmds[c].name; break; }
                }
                cJSON_AddItemToArray(cmds, cJSON_CreateString(cname));
            }
        }
        cJSON_AddItemToArray(devs, d);
    }

    cJSON *btns = cJSON_AddArrayToObject(root, "buttons");
    for (int i = 0; i < g_rf_button_count; i++) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "ctx", g_rf_buttons[i].context_id);
        cJSON_AddStringToObject(b, "label", g_rf_buttons[i].label);
        cJSON_AddNumberToObject(b, "ctype", g_rf_buttons[i].context_type);
        cJSON_AddNumberToObject(b, "atype", g_rf_buttons[i].action_type);
        cJSON_AddNumberToObject(b, "bidx", g_rf_buttons[i].btn_index);
        if (g_rf_buttons[i].icon_key[0]) {
            cJSON_AddStringToObject(b, "icon", g_rf_buttons[i].icon_key);
        }
        cJSON_AddItemToArray(btns, b);
    }

    cJSON *steps = cJSON_AddArrayToObject(root, "steps");
    for (int i = 0; i < g_rf_step_count; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "aid", g_rf_steps[i].act_id);
        cJSON_AddNumberToObject(s, "idx", g_rf_steps[i].step_idx);
        cJSON_AddNumberToObject(s, "tot", g_rf_steps[i].total_steps);
        cJSON_AddNumberToObject(s, "del", g_rf_steps[i].delay_ms);
        cJSON_AddStringToObject(s, "name", g_rf_steps[i].name);
        cJSON_AddItemToArray(steps, s);
    }

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        FILE *f = fopen(RF_CONFIG_FILE, "w");
        if (f) { fputs(json, f); fclose(f); }
        free(json);
    }
    cJSON_Delete(root);
}

void load_rf_config(void) {
    FILE *f = fopen(RF_CONFIG_FILE, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 50000) { fclose(f); return; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *j_ver = cJSON_GetObjectItem(root, "version");
    if (j_ver && cJSON_IsNumber(j_ver)) g_config_version = (uint8_t)j_ver->valueint;

    cJSON *j_slot = cJSON_GetObjectItem(root, "slot");
    if (j_slot && cJSON_IsNumber(j_slot) && j_slot->valueint >= 1 && j_slot->valueint <= 15) {
        g_remote_slot = (uint8_t)j_slot->valueint;
    }

    cJSON *acts = cJSON_GetObjectItem(root, "activities");
    g_rf_activity_count = 0;
    if (acts && cJSON_IsArray(acts)) {
        cJSON *a = NULL;
        cJSON_ArrayForEach(a, acts) {
            if (g_rf_activity_count >= MAX_RF_ACTIVITIES) break;
            cJSON *jid = cJSON_GetObjectItem(a, "id");
            cJSON *jname = cJSON_GetObjectItem(a, "name");
            cJSON *jico = cJSON_GetObjectItem(a, "icon");
            if (jid && jname && cJSON_IsString(jid) && cJSON_IsString(jname)) {
                strncpy(g_rf_activities[g_rf_activity_count].id, jid->valuestring, sizeof(g_rf_activities[0].id) - 1);
                g_rf_activities[g_rf_activity_count].id[sizeof(g_rf_activities[0].id) - 1] = '\0';
                strncpy(g_rf_activities[g_rf_activity_count].name, jname->valuestring, sizeof(g_rf_activities[0].name) - 1);
                g_rf_activities[g_rf_activity_count].name[sizeof(g_rf_activities[0].name) - 1] = '\0';
                if (jico && cJSON_IsString(jico) && jico->valuestring[0]) {
                    strncpy(g_rf_activities[g_rf_activity_count].icon, jico->valuestring, sizeof(g_rf_activities[0].icon) - 1);
                    g_rf_activities[g_rf_activity_count].icon[sizeof(g_rf_activities[0].icon) - 1] = '\0';
                } else {
                    map_icon_type_to_str(0, g_rf_activities[g_rf_activity_count].name, g_rf_activities[g_rf_activity_count].icon, sizeof(g_rf_activities[0].icon));
                }
                g_rf_activity_count++;
            }
        }
    }

    cJSON *devs = cJSON_GetObjectItem(root, "devices");
    g_rf_device_count = 0;
    g_rf_dev_cmd_count = 0;
    if (devs && cJSON_IsArray(devs)) {
        cJSON *d = NULL;
        cJSON_ArrayForEach(d, devs) {
            if (g_rf_device_count >= MAX_RF_DEVICES) break;
            cJSON *jid = cJSON_GetObjectItem(d, "id");
            cJSON *jname = cJSON_GetObjectItem(d, "name");
            if (jid && jname && cJSON_IsString(jid) && cJSON_IsString(jname)) {
                strncpy(g_rf_devices[g_rf_device_count].id, jid->valuestring, 12);
                g_rf_devices[g_rf_device_count].id[12] = '\0';
                strncpy(g_rf_devices[g_rf_device_count].name, jname->valuestring, 14);
                g_rf_devices[g_rf_device_count].name[14] = '\0';
                int k = 0;
                cJSON *jc = NULL;
                cJSON_ArrayForEach(jc, cJSON_GetObjectItem(d, "commands")) {
                    if (k > 255) break;
                    if (cJSON_IsString(jc) && jc->valuestring[0]) store_rf_dev_cmd(g_rf_device_count, k, jc->valuestring);
                    k++;
                }
                g_rf_device_count++;
            }
        }
    }

    cJSON *btns = cJSON_GetObjectItem(root, "buttons");
    g_rf_button_count = 0;
    if (btns && cJSON_IsArray(btns)) {
        cJSON *b = NULL;
        cJSON_ArrayForEach(b, btns) {
            if (g_rf_button_count >= MAX_RF_BUTTONS) break;
            cJSON *jctx = cJSON_GetObjectItem(b, "ctx");
            cJSON *jlbl = cJSON_GetObjectItem(b, "label");
            cJSON *jctype = cJSON_GetObjectItem(b, "ctype");
            cJSON *jatype = cJSON_GetObjectItem(b, "atype");
            cJSON *jico = cJSON_GetObjectItem(b, "icon");
            if (!jico) jico = cJSON_GetObjectItem(b, "icon_key");
            if (jctx && jlbl && cJSON_IsString(jctx) && cJSON_IsString(jlbl)) {
                strncpy(g_rf_buttons[g_rf_button_count].context_id, jctx->valuestring, 12);
                g_rf_buttons[g_rf_button_count].context_id[12] = '\0';
                strncpy(g_rf_buttons[g_rf_button_count].label, jlbl->valuestring, 15);
                g_rf_buttons[g_rf_button_count].label[15] = '\0';
                g_rf_buttons[g_rf_button_count].context_type = jctype ? (uint8_t)jctype->valueint : 0;
                g_rf_buttons[g_rf_button_count].action_type = jatype ? (uint8_t)jatype->valueint : 1;
                cJSON *jbidx = cJSON_GetObjectItem(b, "bidx");
                if (jbidx && cJSON_IsNumber(jbidx)) {
                    g_rf_buttons[g_rf_button_count].btn_index = (uint8_t)jbidx->valueint;
                } else {
                    uint8_t auto_idx = 0;
                    for (int k = 0; k < g_rf_button_count; k++) {
                        if (g_rf_buttons[k].context_type == g_rf_buttons[g_rf_button_count].context_type &&
                            strcmp(g_rf_buttons[k].context_id, g_rf_buttons[g_rf_button_count].context_id) == 0) {
                            auto_idx++;
                        }
                    }
                    g_rf_buttons[g_rf_button_count].btn_index = auto_idx;
                }
                if (jico && cJSON_IsString(jico) && jico->valuestring[0]) {
                    strncpy(g_rf_buttons[g_rf_button_count].icon_key, jico->valuestring, sizeof(g_rf_buttons[0].icon_key) - 1);
                    g_rf_buttons[g_rf_button_count].icon_key[sizeof(g_rf_buttons[0].icon_key) - 1] = '\0';
                } else {
                    g_rf_buttons[g_rf_button_count].icon_key[0] = '\0';
                }
                g_rf_button_count++;
            }
        }
    }

    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    g_rf_step_count = 0;
    if (steps && cJSON_IsArray(steps)) {
        cJSON *s = NULL;
        cJSON_ArrayForEach(s, steps) {
            if (g_rf_step_count >= MAX_RF_STEPS) break;
            cJSON *jaid = cJSON_GetObjectItem(s, "aid");
            cJSON *jidx = cJSON_GetObjectItem(s, "idx");
            cJSON *jtot = cJSON_GetObjectItem(s, "tot");
            cJSON *jdel = cJSON_GetObjectItem(s, "del");
            cJSON *jname = cJSON_GetObjectItem(s, "name");
            if (jaid && jname && cJSON_IsString(jaid) && cJSON_IsString(jname)) {
                strncpy(g_rf_steps[g_rf_step_count].act_id, jaid->valuestring, sizeof(g_rf_steps[0].act_id) - 1);
                g_rf_steps[g_rf_step_count].act_id[sizeof(g_rf_steps[0].act_id) - 1] = '\0';
                g_rf_steps[g_rf_step_count].step_idx = jidx ? (uint8_t)jidx->valueint : 0;
                g_rf_steps[g_rf_step_count].total_steps = jtot ? (uint8_t)jtot->valueint : 1;
                g_rf_steps[g_rf_step_count].delay_ms = jdel ? (uint16_t)jdel->valueint : 1000;
                strncpy(g_rf_steps[g_rf_step_count].name, jname->valuestring, sizeof(g_rf_steps[0].name) - 1);
                g_rf_steps[g_rf_step_count].name[sizeof(g_rf_steps[0].name) - 1] = '\0';
                g_rf_step_count++;
            }
        }
    }

    cJSON_Delete(root);
    g_has_hub_config = (g_rf_activity_count > 0);
    if (g_has_hub_config) {
        g_activity_count = 0;
        for (int i = 0; i < g_rf_activity_count && g_activity_count < MAX_ACTIVITIES; i++) {
            strncpy(g_activities[g_activity_count].id, g_rf_activities[i].id, sizeof(g_activities[0].id) - 1);
            strncpy(g_activities[g_activity_count].name, g_rf_activities[i].name, sizeof(g_activities[0].name) - 1);
            char cpath[128];
            snprintf(cpath, sizeof(cpath), "/data/assets/custom/activity_%s.png", g_rf_activities[i].id);
            if (access(cpath, R_OK) != 0) {
                snprintf(cpath, sizeof(cpath), "/data/assets/custom/%s.png", g_rf_activities[i].id);
            }
            if (access(cpath, R_OK) == 0) {
                strncpy(g_activities[g_activity_count].icon_path, cpath, sizeof(g_activities[0].icon_path) - 1);
            } else {
                resolve_activity_icon(g_rf_activities[i].icon, g_rf_activities[i].name,
                                      g_activities[g_activity_count].icon_path, sizeof(g_activities[0].icon_path));
            }
            g_activity_count++;
        }
        if (g_activity_count < MAX_ACTIVITIES) {
            strncpy(g_activities[g_activity_count].id, "-1", sizeof(g_activities[0].id) - 1);
            strncpy(g_activities[g_activity_count].name, "Power Off", sizeof(g_activities[0].name) - 1);
            strncpy(g_activities[g_activity_count].icon_path, "/opt/luadraws/appscommon/asset/commands/116.png", sizeof(g_activities[0].icon_path) - 1);
            g_activity_count++;
        }
    }
}

void load_settings_conf(void) {
    FILE *f = fopen("/data/elite_ui.conf", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        char sval[64] = {0};
        if (sscanf(line, "%63[^=]=%63[^\n\r]", key, sval) == 2) {
            int val = atoi(sval);
            if (strcmp(key, "touch") == 0) g_touch_enabled = (val != 0);
            else if (strcmp(key, "cap_buttons") == 0 || strcmp(key, "capacitive_buttons") == 0) g_cap_buttons_enabled = (val != 0);
            else if (strcmp(key, "touch_sens") == 0) g_touch_sensitivity = (val >= 1 && val <= 3) ? val : 2;
            else if (strcmp(key, "remote_mode") == 0) g_remote_mode = (val == 1) ? 1 : 0;
            else if (strcmp(key, "gyro") == 0) g_gyro_enabled = (val != 0);
            else if (strcmp(key, "gyro_rate") == 0) g_gyro_rate_hz = (val > 0) ? val : 25;
            else if (strcmp(key, "btn_backlight") == 0) g_btn_backlight_mode = val;
            else if (strcmp(key, "night_start") == 0) g_night_start_hour = val;
            else if (strcmp(key, "night_end") == 0) g_night_end_hour = val;
            else if (strcmp(key, "timeout") == 0) g_timeout_seconds = val;
            else if (strcmp(key, "brightness") == 0) g_brightness_val = (val >= 1 && val <= 5) ? val : 2;
            else if (strcmp(key, "screen") == 0) g_screen_on = (val != 0);
            else if (strcmp(key, "nav_key") == 0) g_nav_toggle_key = val;
            else if (strcmp(key, "nav_mode") == 0) g_nav_toggle_mode = val;
            else if (strcmp(key, "usbeth") == 0) g_usbeth_enabled = (val != 0);
            else if (strcmp(key, "confirm_act") == 0 || strcmp(key, "confirm") == 0) g_confirm_activity = (val != 0);
            else if (strcmp(key, "no_screen") == 0) g_no_screen_mode = (val != 0);
            else if (strcmp(key, "log_level") == 0) g_log_level = val;
            else if (strcmp(key, "slot") == 0) g_remote_slot = (val >= 1 && val <= 15) ? (uint8_t)val : 0x01;
            else if (strcmp(key, "tz") == 0) {
                expand_timezone(sval, g_timezone_str, sizeof(g_timezone_str));
                setenv("TZ", g_timezone_str, 1);
                tzset();
            }
        }
    }
    fclose(f);
    if (access("/data/debug_logging.conf", F_OK) == 0) {
        g_log_level = LOG_LEVEL_DEBUG;
    }
    if (!g_touch_enabled) {
        g_screen_nav_mode = true;
    }
    if (g_nav_toggle_key != 139 && g_nav_toggle_key != 358 &&
        g_nav_toggle_key != 395 && g_nav_toggle_key != 389 &&
        g_nav_toggle_key != 174) {
        g_nav_toggle_key = 139;
    }
}

void save_settings_conf(void) {
    FILE *f = fopen("/data/elite_ui.conf", "w");
    if (!f) return;
    fprintf(f, "touch=%d\n", g_touch_enabled ? 1 : 0);
    fprintf(f, "cap_buttons=%d\n", g_cap_buttons_enabled ? 1 : 0);
    fprintf(f, "touch_sens=%d\n", g_touch_sensitivity);
    fprintf(f, "remote_mode=%d\n", g_remote_mode);
    fprintf(f, "gyro=%d\n", g_gyro_enabled ? 1 : 0);
    fprintf(f, "gyro_rate=%d\n", g_gyro_rate_hz);
    fprintf(f, "btn_backlight=%d\n", g_btn_backlight_mode);
    fprintf(f, "night_start=%d\n", g_night_start_hour);
    fprintf(f, "night_end=%d\n", g_night_end_hour);
    fprintf(f, "timeout=%d\n", g_timeout_seconds);
    fprintf(f, "brightness=%d\n", g_brightness_val);
    fprintf(f, "screen=1\n");
    fprintf(f, "no_screen=%d\n", g_no_screen_mode ? 1 : 0);
    fprintf(f, "log_level=%d\n", g_log_level);
    fprintf(f, "nav_key=%d\n", g_nav_toggle_key);
    fprintf(f, "nav_mode=%d\n", g_nav_toggle_mode);
    fprintf(f, "confirm_act=%d\n", g_confirm_activity ? 1 : 0);
    fprintf(f, "usbeth=%d\n", g_usbeth_enabled ? 1 : 0);
    fprintf(f, "slot=%d\n", g_remote_slot);
    fprintf(f, "tz=%s\n", g_timezone_str);
    fclose(f);
    g_settings_dirty = true;
    if (g_rf_link_connected) {
        send_remote_config_backup_rf();
    }
}
