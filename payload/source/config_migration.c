#define _GNU_SOURCE
#include "config_migration.h"
#include "config_store.h"
#include "config_sanitize.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>

struct id_pair {
    char old_id[32];
    char new_id[8];
};

static void remap_activity_refs_in_json(cJSON *node, const struct id_pair *pairs, int pair_count, int *changed) {
    if (!node) return;
    if (cJSON_IsObject(node)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            if (child->string && (strcmp(child->string, "activityId") == 0 ||
                strcmp(child->string, "targetActivity") == 0 ||
                strcmp(child->string, "activity") == 0)) {
                if (cJSON_IsString(child) && child->valuestring) {
                    for (int i = 0; i < pair_count; i++) {
                        if (strcmp(child->valuestring, pairs[i].old_id) == 0) {
                            cJSON_SetValuestring(child, pairs[i].new_id);
                            if (changed) *changed = 1;
                            break;
                        }
                    }
                } else if (cJSON_IsNumber(child)) {
                    for (int i = 0; i < pair_count; i++) {
                        if (atoi(pairs[i].old_id) == child->valueint) {
                            child->valueint = atoi(pairs[i].new_id);
                            child->valuedouble = (double)child->valueint;
                            if (changed) *changed = 1;
                            break;
                        }
                    }
                }
            } else {
                remap_activity_refs_in_json(child, pairs, pair_count, changed);
            }
        }
    } else if (cJSON_IsArray(node)) {
        cJSON *elem = NULL;
        cJSON_ArrayForEach(elem, node) {
            remap_activity_refs_in_json(elem, pairs, pair_count, changed);
        }
    }
}

void migrate_activity_ids_to_short(cJSON *root) {
    if (!root) return;

    cJSON *acts = cJSON_GetObjectItemCaseSensitive(root, "activities");
    if (!acts) acts = cJSON_GetObjectItemCaseSensitive(root, "Activities");
    if (!acts || !cJSON_IsArray(acts)) return;

    /* Check if any activity has an ID with length > 2 (ignoring "-1") */
    int needs_migration = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, acts) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "Id");
        if (jid) {
            char id_s[32] = {0};
            if (cJSON_IsNumber(jid)) snprintf(id_s, sizeof(id_s), "%ld", (long)jid->valuedouble);
            else if (cJSON_IsString(jid) && jid->valuestring) strncpy(id_s, jid->valuestring, sizeof(id_s) - 1);
            if (id_s[0] && strcmp(id_s, "-1") != 0 && (strlen(id_s) > 2 || atoi(id_s) > 99)) {
                needs_migration = 1;
                break;
            }
        }
    }
    if (!needs_migration) return;

    printf("[*] Migrating activity IDs to short format (1, 2, 3...)\n");

    struct id_pair map[32];
    int map_count = 0;
    int next_id = 1;

    cJSON_ArrayForEach(item, acts) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "Id");
        if (jid) {
            char id_s[32] = {0};
            if (cJSON_IsNumber(jid)) snprintf(id_s, sizeof(id_s), "%ld", (long)jid->valuedouble);
            else if (cJSON_IsString(jid) && jid->valuestring) strncpy(id_s, jid->valuestring, sizeof(id_s) - 1);
            if (!id_s[0] || strcmp(id_s, "-1") == 0) continue;
            if (map_count < 32) {
                strncpy(map[map_count].old_id, id_s, sizeof(map[0].old_id) - 1);
                snprintf(map[map_count].new_id, sizeof(map[0].new_id), "%d", next_id++);
                cJSON_DeleteItemFromObject(item, "id");
                cJSON_DeleteItemFromObject(item, "Id");
                cJSON_AddStringToObject(item, "id", map[map_count].new_id);
                printf("[*] Remapped activity ID: %s -> %s\n", map[map_count].old_id, map[map_count].new_id);
                map_count++;
            }
        }
    }

    /* Save updated activities.json */
    char *out = cJSON_Print(root);
    if (out) {
        write_file_atomic(HARMONY_ACTIVITIES_FILE, out);
        free(out);
    }

    /* Remap current_activity files */
    const char *cur_act_files[] = {
        "/data/harmony/current_activity",
        NULL
    };
    for (int ci = 0; cur_act_files[ci]; ci++) {
        FILE *cf = fopen(cur_act_files[ci], "r");
        if (cf) {
            char cur_id[32] = {0};
            if (fscanf(cf, "%31s", cur_id) == 1) {
                fclose(cf);
                for (int i = 0; i < map_count; i++) {
                    if (strcmp(cur_id, map[i].old_id) == 0) {
                        char new_act_buf[16];
                        snprintf(new_act_buf, sizeof(new_act_buf), "%s\n", map[i].new_id);
                        write_file_atomic(cur_act_files[ci], new_act_buf);
                        sync();
                        break;
                    }
                }
            } else {
                fclose(cf);
            }
        }
    }

    /* Helper loop to remap JSON mapping files */
    const char *map_files[] = {
        "/data/harmony/elite_remote_map.json",
        "/data/harmony/bt_remote_map.json",
        NULL
    };
    for (int fi = 0; map_files[fi]; fi++) {
        char *mraw = read_file_text(map_files[fi], 512 * 1024);
        if (mraw) {
            cJSON *mroot = cJSON_Parse(mraw);
            free(mraw);
            if (mroot) {
                int changed = 0;
                cJSON *macts = cJSON_GetObjectItemCaseSensitive(mroot, "activities");
                if (macts && cJSON_IsObject(macts)) {
                    for (int mi = 0; mi < map_count; mi++) {
                        cJSON *entry = cJSON_DetachItemFromObject(macts, map[mi].old_id);
                        if (entry) {
                            cJSON_AddItemToObject(macts, map[mi].new_id, entry);
                            changed = 1;
                        }
                    }
                }
                remap_activity_refs_in_json(mroot, map, map_count, &changed);
                if (changed) {
                    char *mout = cJSON_Print(mroot);
                    if (mout) {
                        write_file_atomic(map_files[fi], mout);
                        free(mout);
                        printf("[*] Remapped activities and button actions in %s\n", map_files[fi]);
                    }
                }
                cJSON_Delete(mroot);
            }
        }
    }
}

static void remap_device_refs_in_json(cJSON *node, const struct id_pair *pairs, int pair_count, int *changed) {
    if (!node) return;
    if (cJSON_IsObject(node)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            if (child->string && (strcmp(child->string, "deviceId") == 0 ||
                strcmp(child->string, "targetDevice") == 0 ||
                strcmp(child->string, "device") == 0)) {
                if (cJSON_IsString(child) && child->valuestring) {
                    for (int i = 0; i < pair_count; i++) {
                        if (strcmp(child->valuestring, pairs[i].old_id) == 0) {
                            cJSON_SetValuestring(child, pairs[i].new_id);
                            if (changed) *changed = 1;
                            break;
                        }
                    }
                } else if (cJSON_IsNumber(child)) {
                    for (int i = 0; i < pair_count; i++) {
                        if (atoi(pairs[i].old_id) == child->valueint) {
                            child->valueint = atoi(pairs[i].new_id);
                            child->valuedouble = (double)child->valueint;
                            if (changed) *changed = 1;
                            break;
                        }
                    }
                }
            } else if (child->string && strcmp(child->string, "deviceIds") == 0 && cJSON_IsArray(child)) {
                cJSON *elem = NULL;
                cJSON_ArrayForEach(elem, child) {
                    if (cJSON_IsString(elem) && elem->valuestring) {
                        for (int i = 0; i < pair_count; i++) {
                            if (strcmp(elem->valuestring, pairs[i].old_id) == 0) {
                                cJSON_SetValuestring(elem, pairs[i].new_id);
                                if (changed) *changed = 1;
                                break;
                            }
                        }
                    } else if (cJSON_IsNumber(elem)) {
                        for (int i = 0; i < pair_count; i++) {
                            if (atoi(pairs[i].old_id) == elem->valueint) {
                                elem->valueint = atoi(pairs[i].new_id);
                                elem->valuedouble = (double)elem->valueint;
                                if (changed) *changed = 1;
                                break;
                            }
                        }
                    }
                }
            } else {
                remap_device_refs_in_json(child, pairs, pair_count, changed);
            }
        }
    } else if (cJSON_IsArray(node)) {
        cJSON *elem = NULL;
        cJSON_ArrayForEach(elem, node) {
            remap_device_refs_in_json(elem, pairs, pair_count, changed);
        }
    }
}

void migrate_device_ids_to_short(cJSON *root) {
    if (!root) return;

    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "Devices");
    if (!devs || !cJSON_IsArray(devs)) return;

    int needs_migration = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devs) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "Id");
        if (jid) {
            char id_s[32] = {0};
            if (cJSON_IsNumber(jid)) snprintf(id_s, sizeof(id_s), "%ld", (long)jid->valuedouble);
            else if (cJSON_IsString(jid) && jid->valuestring) strncpy(id_s, jid->valuestring, sizeof(id_s) - 1);
            if (id_s[0] && strcmp(id_s, "-1") != 0 && strcmp(id_s, "0") != 0 && (strlen(id_s) > 2 || atoi(id_s) > 99)) {
                needs_migration = 1;
                break;
            }
        }
    }
    if (!needs_migration) return;

    printf("[*] Migrating device IDs to short format (1, 2, 3...)\n");

    struct id_pair map[64];
    int map_count = 0;
    int next_id = 1;

    cJSON_ArrayForEach(item, devs) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "Id");
        if (jid) {
            char id_s[32] = {0};
            if (cJSON_IsNumber(jid)) snprintf(id_s, sizeof(id_s), "%ld", (long)jid->valuedouble);
            else if (cJSON_IsString(jid) && jid->valuestring) strncpy(id_s, jid->valuestring, sizeof(id_s) - 1);
            if (!id_s[0] || strcmp(id_s, "-1") == 0 || strcmp(id_s, "0") == 0) continue;
            if (map_count < 64) {
                strncpy(map[map_count].old_id, id_s, sizeof(map[0].old_id) - 1);
                snprintf(map[map_count].new_id, sizeof(map[0].new_id), "%d", next_id++);
                cJSON_DeleteItemFromObject(item, "id");
                cJSON_DeleteItemFromObject(item, "Id");
                cJSON_AddStringToObject(item, "id", map[map_count].new_id);
                printf("[*] Remapped device ID: %s -> %s\n", map[map_count].old_id, map[map_count].new_id);
                map_count++;
            }
        }
    }

    char *out = cJSON_Print(root);
    if (out) {
        write_file_atomic(HARMONY_DEVICES_FILE, out);
        write_file_atomic("/data/devices.json", out);
        free(out);
    }

    char *araw = read_file_text(HARMONY_ACTIVITIES_FILE, 2 * 1024 * 1024);
    if (araw) {
        cJSON *aroot = cJSON_Parse(araw);
        free(araw);
        if (aroot) {
            int changed = 0;
            remap_device_refs_in_json(aroot, map, map_count, &changed);
            if (changed) {
                char *aout = cJSON_Print(aroot);
                if (aout) {
                    write_file_atomic(HARMONY_ACTIVITIES_FILE, aout);
                    write_file_atomic("/data/activities.json", aout);
                    free(aout);
                    printf("[*] Remapped device references in %s\n", HARMONY_ACTIVITIES_FILE);
                }
            }
            cJSON_Delete(aroot);
        }
    }

    const char *map_files[] = {
        "/data/harmony/elite_remote_map.json",
        "/data/harmony/remote_slot1_map.json",
        "/data/harmony/remote_slot2_map.json",
        "/data/harmony/bt_remote_map.json",
        NULL
    };
    for (int fi = 0; map_files[fi]; fi++) {
        char *mraw = read_file_text(map_files[fi], 512 * 1024);
        if (mraw) {
            cJSON *mroot = cJSON_Parse(mraw);
            free(mraw);
            if (mroot) {
                int changed = 0;
                cJSON *mdevs = cJSON_GetObjectItemCaseSensitive(mroot, "devices");
                if (mdevs && cJSON_IsObject(mdevs)) {
                    for (int mi = 0; mi < map_count; mi++) {
                        cJSON *entry = cJSON_DetachItemFromObject(mdevs, map[mi].old_id);
                        if (entry) {
                            cJSON_AddItemToObject(mdevs, map[mi].new_id, entry);
                            changed = 1;
                        }
                    }
                }
                remap_device_refs_in_json(mroot, map, map_count, &changed);
                if (changed) {
                    char *mout = cJSON_Print(mroot);
                    if (mout) {
                        write_file_atomic(map_files[fi], mout);
                        free(mout);
                        printf("[*] Remapped device references in %s\n", map_files[fi]);
                    }
                }
                cJSON_Delete(mroot);
            }
        }
    }
}

/* Migrate stock DeviceList.json -> devices.json without touching original */
void migrate_stock_devices(void) {
    if (access(HARMONY_DEVICES_FILE, F_OK) == 0) return;

    const char *src = DEVICE_LIST_FILE;
    if (access(src, F_OK) != 0) src = "/cache/stock_import/data/resources/DeviceList.json";
    if (access(src, F_OK) != 0) src = "/data/backups/DeviceList.json";
    if (access(src, F_OK) != 0) return;

    char *raw = read_file_text(src, 4 * 1024 * 1024);
    if (!raw) return;
    cJSON *stock = cJSON_Parse(raw);
    free(raw);
    if (!stock) return;

    cJSON *clean_root = cJSON_CreateObject();
    cJSON *devs_arr = cJSON_AddArrayToObject(clean_root, "devices");

    cJSON *dwf = cJSON_GetObjectItemCaseSensitive(stock, "DevicesWithFeatures");
    if (dwf && cJSON_IsArray(dwf)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, dwf) {
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(item, "Device");
            if (!d) continue;
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id");
            char id_str[32] = {0};
            if (jid && cJSON_IsNumber(jid)) snprintf(id_str, sizeof(id_str), "%ld", (long)jid->valuedouble);
            else if (jid && cJSON_IsString(jid) && jid->valuestring) strncpy(id_str, jid->valuestring, sizeof(id_str) - 1);
            if (!id_str[0]) continue;

            cJSON *ndev = cJSON_CreateObject();
            cJSON_AddStringToObject(ndev, "id", id_str);

            const cJSON *name = cJSON_GetObjectItemCaseSensitive(d, "Name");
            cJSON_AddStringToObject(ndev, "name", (name && name->valuestring) ? name->valuestring : "Device");

            const cJSON *mfg = cJSON_GetObjectItemCaseSensitive(d, "Manufacturer");
            cJSON_AddStringToObject(ndev, "manufacturer", (mfg && mfg->valuestring) ? mfg->valuestring : "");

            const cJSON *mdl = cJSON_GetObjectItemCaseSensitive(d, "Model");
            cJSON_AddStringToObject(ndev, "model", (mdl && mdl->valuestring) ? mdl->valuestring : "");

            const cJSON *type = cJSON_GetObjectItemCaseSensitive(d, "DeviceTypeDisplayName");
            if (!type) type = cJSON_GetObjectItemCaseSensitive(d, "Type");
            const char *typestr = (type && type->valuestring) ? type->valuestring : "";

            const cJSON *tr = cJSON_GetObjectItemCaseSensitive(d, "Transport");
            int tr_val = (tr && cJSON_IsNumber(tr)) ? tr->valueint : 1;
            int dev_is_bt = (tr_val == 32 || strcasestr(typestr, "bluetooth") != NULL || strcasestr(typestr, "bt") != NULL);
            int dev_is_mqtt = (strcasestr(typestr, "mqtt") != NULL);

            cJSON_AddStringToObject(ndev, "type", dev_is_bt ? "BT" : (dev_is_mqtt ? "MQTT" : "IR"));

            const cJSON *cp = cJSON_GetObjectItemCaseSensitive(d, "ControlPort");
            cJSON_AddNumberToObject(ndev, "controlPort", (cp && cJSON_IsNumber(cp)) ? cp->valueint : 7);

            /* Timings */
            const cJSON *idd = cJSON_GetObjectItemCaseSensitive(d, "InterDeviceDelay");
            cJSON_AddNumberToObject(ndev, "interDeviceDelay", (idd && cJSON_IsNumber(idd)) ? idd->valueint : 500);
            const cJSON *ikd = cJSON_GetObjectItemCaseSensitive(d, "InterKeyDelay");
            cJSON_AddNumberToObject(ndev, "interKeyDelay", (ikd && cJSON_IsNumber(ikd)) ? ikd->valueint : 100);
            const cJSON *hikd = cJSON_GetObjectItemCaseSensitive(d, "HoldInterKeyDelay");
            cJSON_AddNumberToObject(ndev, "holdInterKeyDelay", (hikd && cJSON_IsNumber(hikd)) ? hikd->valueint : 200);
            const cJSON *didd = cJSON_GetObjectItemCaseSensitive(d, "DefaultInterDeviceDelay");
            cJSON_AddNumberToObject(ndev, "defaultInterDeviceDelay", (didd && cJSON_IsNumber(didd)) ? didd->valueint : 500);
            const cJSON *pod = cJSON_GetObjectItemCaseSensitive(d, "PowerOnDelay");
            cJSON_AddNumberToObject(ndev, "powerOnDelay", (pod && cJSON_IsNumber(pod)) ? pod->valueint : 1500);
            cJSON_AddNumberToObject(ndev, "isPowerAlwaysOn", 0);

            /* BT reconnect settings */
            cJSON_AddNumberToObject(ndev, "btAutoReconnect", -1);
            cJSON_AddNumberToObject(ndev, "btReconnectDuration", 0);
            cJSON_AddNumberToObject(ndev, "btReconnectDelay", -1);

            const cJSON *bta = cJSON_GetObjectItemCaseSensitive(d, "BTAddress");
            if (bta && cJSON_IsString(bta) && bta->valuestring) {
                cJSON_AddStringToObject(ndev, "bdaddr", bta->valuestring);
            }

            /* MQTT config defaults */
            cJSON_AddNumberToObject(ndev, "mqttEnabled", dev_is_mqtt ? 1 : 0);
            cJSON_AddStringToObject(ndev, "mqttTopic", "{base_topic}/button/{device}/{command}");
            cJSON_AddNumberToObject(ndev, "mqttPulseMs", 1000);

            /* Commands */
            cJSON *ncmds = cJSON_AddArrayToObject(ndev, "commands");
            const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(item, "Commands");
            int has_connect = 0, has_disconnect = 0;

            if (cmds && cJSON_IsArray(cmds)) {
                const cJSON *c = NULL;
                cJSON_ArrayForEach(c, cmds) {
                    const cJSON *cname = cJSON_GetObjectItemCaseSensitive(c, "Name");
                    if (!cname || !cname->valuestring) continue;
                    if (strcasecmp(cname->valuestring, "Connect") == 0) has_connect = 1;
                    if (strcasecmp(cname->valuestring, "Disconnect") == 0) has_disconnect = 1;

                    cJSON *ncmd = cJSON_CreateObject();
                    cJSON_AddStringToObject(ncmd, "name", cname->valuestring);
                    const cJSON *ckey = cJSON_GetObjectItemCaseSensitive(c, "KeyCode");
                    const char *keystr = (ckey && ckey->valuestring) ? ckey->valuestring : "";
                    const cJSON *craw = cJSON_GetObjectItemCaseSensitive(c, "Raw");
                    const char *rawstr = (craw && craw->valuestring) ? craw->valuestring : "";

                    char clean_key[64] = {0};
                    convert_legacy_hid_code(keystr, clean_key, sizeof(clean_key));
                    cJSON_AddStringToObject(ncmd, "keyCode", clean_key[0] ? clean_key : keystr);

                    char clean_raw[64] = {0};
                    convert_legacy_hid_code(rawstr, clean_raw, sizeof(clean_raw));
                    cJSON_AddStringToObject(ncmd, "raw", clean_raw[0] ? clean_raw : rawstr);

                    int cmd_is_bt = dev_is_bt || (strncmp(clean_key, "HID:", 4) == 0);
                    int cmd_is_mqtt = dev_is_mqtt || (strncmp(keystr, "G:MQTT", 6) == 0);

                    /* repeats and port are IR ONLY */
                    if (!cmd_is_bt && !cmd_is_mqtt) {
                        cJSON_AddNumberToObject(ncmd, "port", (cp && cJSON_IsNumber(cp)) ? cp->valueint : 7);
                        cJSON_AddNumberToObject(ncmd, "repeats", 3);
                    }
                    cJSON_AddItemToArray(ncmds, ncmd);
                }
            }

            if (dev_is_bt) {
                if (!has_connect) {
                    cJSON *cc = cJSON_CreateObject();
                    cJSON_AddStringToObject(cc, "name", "Connect");
                    cJSON_AddStringToObject(cc, "keyCode", "CONNECT");
                    cJSON_AddStringToObject(cc, "raw", "CONNECT");
                    cJSON_AddItemToArray(ncmds, cc);
                }
                if (!has_disconnect) {
                    cJSON *dc = cJSON_CreateObject();
                    cJSON_AddStringToObject(dc, "name", "Disconnect");
                    cJSON_AddStringToObject(dc, "keyCode", "DISCONNECT");
                    cJSON_AddStringToObject(dc, "raw", "DISCONNECT");
                    cJSON_AddItemToArray(ncmds, dc);
                }
            }

            /* Power On / Off actions */
            const cJSON *po = cJSON_GetObjectItemCaseSensitive(item, "PowerOnActions");
            if (po && cJSON_IsArray(po)) {
                cJSON_AddItemToObject(ndev, "powerOnSequence", cJSON_Duplicate(po, 1));
            }
            const cJSON *pf = cJSON_GetObjectItemCaseSensitive(item, "PowerOffActions");
            if (pf && cJSON_IsArray(pf)) {
                cJSON_AddItemToObject(ndev, "powerOffSequence", cJSON_Duplicate(pf, 1));
            }

            cJSON_AddItemToArray(devs_arr, ndev);
        }
    }
    cJSON_Delete(stock);

    sanitize_devices_json(clean_root);

    char *json_out = cJSON_Print(clean_root);
    if (json_out) {
        write_file_atomic(HARMONY_DEVICES_FILE, json_out);
        free(json_out);
    }
    cJSON_Delete(clean_root);
    printf("[+] Migrated stock DeviceList.json -> %s\n", HARMONY_DEVICES_FILE);
}

/* Migrate stock ActivityList.json -> activities.json without touching original */
void migrate_stock_activities(void) {
    if (access(HARMONY_ACTIVITIES_FILE, F_OK) == 0) return;

    const char *src = ACTIVITY_LIST_FILE;
    if (access(src, F_OK) != 0) src = "/cache/stock_import/data/resources/ActivityList.json";
    if (access(src, F_OK) != 0) src = "/data/backups/ActivityList.json";
    if (access(src, F_OK) != 0) return;

    char *raw = read_file_text(src, 2 * 1024 * 1024);
    if (!raw) return;
    cJSON *stock = cJSON_Parse(raw);
    free(raw);
    if (!stock) return;

    cJSON *clean_root = cJSON_CreateObject();
    cJSON *acts_arr = cJSON_AddArrayToObject(clean_root, "activities");

    cJSON *arr = cJSON_IsArray(stock) ? stock : cJSON_GetObjectItemCaseSensitive(stock, "Activities");
    if (arr && cJSON_IsArray(arr)) {
        int next_short_id = 1;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, arr) {
            const cJSON *a = entry;
            const cJSON *inner = cJSON_GetObjectItemCaseSensitive(entry, "Activity");
            if (inner && cJSON_IsObject(inner)) a = inner;

            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(a, "Id-");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(a, "Id");
            char id_str[32] = {0};
            if (jid && cJSON_IsNumber(jid)) snprintf(id_str, sizeof(id_str), "%ld", (long)jid->valuedouble);
            else if (jid && cJSON_IsString(jid) && jid->valuestring) strncpy(id_str, jid->valuestring, sizeof(id_str) - 1);
            if (!id_str[0] || strcmp(id_str, "0") == 0) continue;

            cJSON *nact = cJSON_CreateObject();
            if (strcmp(id_str, "-1") == 0) {
                cJSON_AddStringToObject(nact, "id", "-1");
            } else {
                char short_id[8];
                snprintf(short_id, sizeof(short_id), "%d", next_short_id++);
                cJSON_AddStringToObject(nact, "id", short_id);
            }

            const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "Name");
            cJSON_AddStringToObject(nact, "name", (name && name->valuestring) ? name->valuestring : (strcmp(id_str, "-1") == 0 ? "PowerOff" : "Activity"));

            const cJSON *type = cJSON_GetObjectItemCaseSensitive(a, "SuggestedDisplay");
            if (!type) type = cJSON_GetObjectItemCaseSensitive(a, "Type");
            cJSON_AddStringToObject(nact, "type", (type && type->valuestring) ? type->valuestring : "Generic");

            const cJSON *order = cJSON_GetObjectItemCaseSensitive(a, "ActivityOrder");
            if (!order) order = cJSON_GetObjectItemCaseSensitive(a, "Order");
            cJSON_AddNumberToObject(nact, "order", (order && cJSON_IsNumber(order)) ? order->valueint : 0);

            cJSON_AddStringToObject(nact, "icon", "tv");

            /* deviceIds: sort roles by PowerOnOrder */
            cJSON *dev_ids = cJSON_AddArrayToObject(nact, "deviceIds");
            const cJSON *roles = cJSON_GetObjectItemCaseSensitive(a, "Roles");
            if (roles && cJSON_IsArray(roles)) {
                for (int po = 1; po <= 32; po++) {
                    const cJSON *r = NULL;
                    cJSON_ArrayForEach(r, roles) {
                        const cJSON *pord = cJSON_GetObjectItemCaseSensitive(r, "PowerOnOrder");
                        int ord_val = (pord && cJSON_IsNumber(pord)) ? (int)pord->valuedouble : 0;
                        if (ord_val == po) {
                            const cJSON *rdid = cJSON_GetObjectItemCaseSensitive(r, "DeviceId-");
                            if (!rdid) rdid = cJSON_GetObjectItemCaseSensitive(r, "DeviceId");
                            char rd_str[32] = {0};
                            if (rdid && cJSON_IsNumber(rdid)) snprintf(rd_str, sizeof(rd_str), "%ld", (long)rdid->valuedouble);
                            else if (rdid && cJSON_IsString(rdid) && rdid->valuestring) strncpy(rd_str, rdid->valuestring, sizeof(rd_str) - 1);
                            if (rd_str[0] && strcmp(rd_str, "0") != 0) {
                                int dup = 0;
                                const cJSON *ex = NULL;
                                cJSON_ArrayForEach(ex, dev_ids) {
                                    if (ex->valuestring && strcmp(ex->valuestring, rd_str) == 0) { dup = 1; break; }
                                }
                                if (!dup) cJSON_AddItemToArray(dev_ids, cJSON_CreateString(rd_str));
                            }
                        }
                    }
                }
                const cJSON *r = NULL;
                cJSON_ArrayForEach(r, roles) {
                    const cJSON *pord = cJSON_GetObjectItemCaseSensitive(r, "PowerOnOrder");
                    if (!pord || !cJSON_IsNumber(pord) || (int)pord->valuedouble <= 0) {
                        const cJSON *rdid = cJSON_GetObjectItemCaseSensitive(r, "DeviceId-");
                        if (!rdid) rdid = cJSON_GetObjectItemCaseSensitive(r, "DeviceId");
                        char rd_str[32] = {0};
                        if (rdid && cJSON_IsNumber(rdid)) snprintf(rd_str, sizeof(rd_str), "%ld", (long)rdid->valuedouble);
                        else if (rdid && cJSON_IsString(rdid) && rdid->valuestring) strncpy(rd_str, rdid->valuestring, sizeof(rd_str) - 1);
                        if (rd_str[0] && strcmp(rd_str, "0") != 0) {
                            int dup = 0;
                            const cJSON *ex = NULL;
                            cJSON_ArrayForEach(ex, dev_ids) {
                                if (ex->valuestring && strcmp(ex->valuestring, rd_str) == 0) { dup = 1; break; }
                            }
                            if (!dup) cJSON_AddItemToArray(dev_ids, cJSON_CreateString(rd_str));
                        }
                    }
                }
            }
            if (cJSON_GetArraySize(dev_ids) == 0) {
                const cJSON *seqs[2];
                seqs[0] = cJSON_GetObjectItemCaseSensitive(a, "StartSequence");
                if (!seqs[0]) seqs[0] = cJSON_GetObjectItemCaseSensitive(a, "EnterActions");
                seqs[1] = cJSON_GetObjectItemCaseSensitive(a, "StopSequence");
                if (!seqs[1]) seqs[1] = cJSON_GetObjectItemCaseSensitive(a, "LeaveActions");
                for (int si = 0; si < 2; si++) {
                    if (seqs[si] && cJSON_IsArray(seqs[si])) {
                        const cJSON *st = NULL;
                        cJSON_ArrayForEach(st, seqs[si]) {
                            const cJSON *sdid = cJSON_GetObjectItemCaseSensitive(st, "DeviceId-");
                            if (!sdid) sdid = cJSON_GetObjectItemCaseSensitive(st, "DeviceId");
                            char rd_str[32] = {0};
                            if (sdid && cJSON_IsNumber(sdid)) snprintf(rd_str, sizeof(rd_str), "%ld", (long)sdid->valuedouble);
                            else if (sdid && cJSON_IsString(sdid) && sdid->valuestring) strncpy(rd_str, sdid->valuestring, sizeof(rd_str) - 1);
                            if (rd_str[0] && strcmp(rd_str, "0") != 0) {
                                int dup = 0;
                                const cJSON *ex = NULL;
                                cJSON_ArrayForEach(ex, dev_ids) {
                                    if (ex->valuestring && strcmp(ex->valuestring, rd_str) == 0) { dup = 1; break; }
                                }
                                if (!dup) cJSON_AddItemToArray(dev_ids, cJSON_CreateString(rd_str));
                            }
                        }
                    }
                }
            }

            /* startSequence */
            cJSON *start_seq = cJSON_AddArrayToObject(nact, "startSequence");
            const cJSON *s_arr = cJSON_GetObjectItemCaseSensitive(a, "StartSequence");
            if (!s_arr) s_arr = cJSON_GetObjectItemCaseSensitive(a, "EnterActions");
            if (s_arr && cJSON_IsArray(s_arr)) {
                const cJSON *step = NULL;
                cJSON_ArrayForEach(step, s_arr) {
                    cJSON *nstep = cJSON_CreateObject();
                    const cJSON *stype = cJSON_GetObjectItemCaseSensitive(step, "Type");
                    const cJSON *st_type = cJSON_GetObjectItemCaseSensitive(step, "__type");
                    const char *tstr = (stype && stype->valuestring) ? stype->valuestring : ((st_type && strstr(st_type->valuestring, "Delay")) ? "Delay" : "IRCommand");
                    cJSON_AddStringToObject(nstep, "type", tstr);
                    const cJSON *sdid = cJSON_GetObjectItemCaseSensitive(step, "DeviceId-");
                    if (!sdid) sdid = cJSON_GetObjectItemCaseSensitive(step, "DeviceId");
                    char sd_str[32] = {0};
                    if (sdid && cJSON_IsNumber(sdid)) snprintf(sd_str, sizeof(sd_str), "%ld", (long)sdid->valuedouble);
                    else if (sdid && cJSON_IsString(sdid) && sdid->valuestring) strncpy(sd_str, sdid->valuestring, sizeof(sd_str) - 1);
                    cJSON_AddStringToObject(nstep, "deviceId", sd_str);
                    const cJSON *scmd = cJSON_GetObjectItemCaseSensitive(step, "CommandName");
                    if (!scmd) scmd = cJSON_GetObjectItemCaseSensitive(step, "IRCommandName");
                    if (!scmd) scmd = cJSON_GetObjectItemCaseSensitive(step, "Command");
                    cJSON_AddStringToObject(nstep, "command", (scmd && scmd->valuestring) ? scmd->valuestring : "");
                    const cJSON *sdel = cJSON_GetObjectItemCaseSensitive(step, "Duration");
                    if (!sdel) sdel = cJSON_GetObjectItemCaseSensitive(step, "Delay");
                    cJSON_AddNumberToObject(nstep, "delay", (sdel && cJSON_IsNumber(sdel)) ? (int)sdel->valuedouble : 0);
                    cJSON_AddItemToArray(start_seq, nstep);
                }
            }

            /* stopSequence */
            cJSON *stop_seq = cJSON_AddArrayToObject(nact, "stopSequence");
            const cJSON *p_arr = cJSON_GetObjectItemCaseSensitive(a, "StopSequence");
            if (!p_arr) p_arr = cJSON_GetObjectItemCaseSensitive(a, "LeaveActions");
            if (p_arr && cJSON_IsArray(p_arr)) {
                const cJSON *step = NULL;
                cJSON_ArrayForEach(step, p_arr) {
                    cJSON *nstep = cJSON_CreateObject();
                    const cJSON *stype = cJSON_GetObjectItemCaseSensitive(step, "Type");
                    const cJSON *st_type = cJSON_GetObjectItemCaseSensitive(step, "__type");
                    const char *tstr = (stype && stype->valuestring) ? stype->valuestring : ((st_type && strstr(st_type->valuestring, "Delay")) ? "Delay" : "IRCommand");
                    cJSON_AddStringToObject(nstep, "type", tstr);
                    const cJSON *sdid = cJSON_GetObjectItemCaseSensitive(step, "DeviceId-");
                    if (!sdid) sdid = cJSON_GetObjectItemCaseSensitive(step, "DeviceId");
                    char sd_str[32] = {0};
                    if (sdid && cJSON_IsNumber(sdid)) snprintf(sd_str, sizeof(sd_str), "%ld", (long)sdid->valuedouble);
                    else if (sdid && cJSON_IsString(sdid) && sdid->valuestring) strncpy(sd_str, sdid->valuestring, sizeof(sd_str) - 1);
                    cJSON_AddStringToObject(nstep, "deviceId", sd_str);
                    const cJSON *scmd = cJSON_GetObjectItemCaseSensitive(step, "CommandName");
                    if (!scmd) scmd = cJSON_GetObjectItemCaseSensitive(step, "IRCommandName");
                    if (!scmd) scmd = cJSON_GetObjectItemCaseSensitive(step, "Command");
                    cJSON_AddStringToObject(nstep, "command", (scmd && scmd->valuestring) ? scmd->valuestring : "");
                    const cJSON *sdel = cJSON_GetObjectItemCaseSensitive(step, "Duration");
                    if (!sdel) sdel = cJSON_GetObjectItemCaseSensitive(step, "Delay");
                    cJSON_AddNumberToObject(nstep, "delay", (sdel && cJSON_IsNumber(sdel)) ? (int)sdel->valuedouble : 0);
                    cJSON_AddItemToArray(stop_seq, nstep);
                }
            }

            cJSON_AddItemToArray(acts_arr, nact);
        }
    }
    cJSON_Delete(stock);

    char *json_out = cJSON_Print(clean_root);
    if (json_out) {
        write_file_atomic(HARMONY_ACTIVITIES_FILE, json_out);
        free(json_out);
    }
    cJSON_Delete(clean_root);
    printf("[+] Migrated stock ActivityList.json -> %s\n", HARMONY_ACTIVITIES_FILE);
}
