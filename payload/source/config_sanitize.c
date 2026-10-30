#define _GNU_SOURCE
#include "config_sanitize.h"
#include "config_store.h"
#include "cJSON.h"
#include "bt_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

/* Sanitize devices JSON:
 * 1. Ensure canonical {"devices": [...]} structure
 * 2. Normalize and validate fields to clean camelCase canonical schema
 * 3. Normalize commands and power sequences
 */
void sanitize_devices_json(cJSON *root) {
    if (!root) return;

    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs || !cJSON_IsArray(devs)) return;

    cJSON *clean_devs = cJSON_CreateArray();

    cJSON *src_d = NULL;
    cJSON_ArrayForEach(src_d, devs) {
        if (!src_d || !cJSON_IsObject(src_d)) continue;

        cJSON *jid = cJSON_GetObjectItemCaseSensitive(src_d, "id");
        char id_str[64] = {0};
        if (jid && cJSON_IsNumber(jid)) snprintf(id_str, sizeof(id_str), "%ld", (long)jid->valuedouble);
        else if (jid && cJSON_IsString(jid) && jid->valuestring) strncpy(id_str, jid->valuestring, sizeof(id_str) - 1);
        if (!id_str[0]) continue;

        cJSON *cdev = cJSON_CreateObject();
        cJSON_AddStringToObject(cdev, "id", id_str);

        cJSON *jname = cJSON_GetObjectItemCaseSensitive(src_d, "name");
        const char *name_str = (jname && jname->valuestring && jname->valuestring[0]) ? jname->valuestring : "Device";
        cJSON_AddStringToObject(cdev, "name", name_str);

        cJSON *jmfg = cJSON_GetObjectItemCaseSensitive(src_d, "manufacturer");
        const char *mfg_str = (jmfg && jmfg->valuestring) ? jmfg->valuestring : "";
        cJSON_AddStringToObject(cdev, "manufacturer", mfg_str);

        cJSON *jmdl = cJSON_GetObjectItemCaseSensitive(src_d, "model");
        const char *mdl_str = (jmdl && jmdl->valuestring) ? jmdl->valuestring : "";
        cJSON_AddStringToObject(cdev, "model", mdl_str);

        cJSON *jtype = cJSON_GetObjectItemCaseSensitive(src_d, "type");
        const char *type_str = (jtype && jtype->valuestring) ? jtype->valuestring : "IR";

        cJSON *jmqtt_en = cJSON_GetObjectItemCaseSensitive(src_d, "mqttEnabled");
        int is_mqtt = (strcasecmp(type_str, "MQTT") == 0 ||
                       (jmqtt_en && (cJSON_IsTrue(jmqtt_en) || (cJSON_IsNumber(jmqtt_en) && jmqtt_en->valueint == 1))));

        cJSON *jtrans = cJSON_GetObjectItemCaseSensitive(src_d, "transport");
        int is_bt = (!is_mqtt) && ((jtrans && cJSON_IsNumber(jtrans) && jtrans->valueint == 32) ||
                       strcasecmp(type_str, "BT") == 0 || strcasecmp(type_str, "Bluetooth") == 0 ||
                       strcasestr(type_str, "bluetooth") != NULL || strcasestr(mfg_str, "bluetooth") != NULL ||
                       strcasecmp(type_str, "BTHID") == 0);

        const char *final_type = "IR";
        if (is_mqtt) {
            final_type = (type_str && type_str[0]) ? type_str : "MQTT";
        } else if (is_bt) {
            final_type = (type_str && type_str[0]) ? type_str : "BT";
        } else if (type_str && type_str[0]) {
            final_type = type_str;
        }
        cJSON_AddStringToObject(cdev, "type", final_type);

        cJSON *jicon = cJSON_GetObjectItemCaseSensitive(src_d, "icon");
        if (jicon) {
            if (cJSON_IsString(jicon) && jicon->valuestring) cJSON_AddStringToObject(cdev, "icon", jicon->valuestring);
            else if (cJSON_IsNumber(jicon)) cJSON_AddNumberToObject(cdev, "icon", jicon->valueint);
        }

        if (!is_bt && !is_mqtt) {
            cJSON *jcp = cJSON_GetObjectItemCaseSensitive(src_d, "controlPort");
            int cp_val = (jcp && cJSON_IsNumber(jcp)) ? jcp->valueint : 7;
            cJSON_AddNumberToObject(cdev, "controlPort", (cp_val >= 0 && cp_val <= 7) ? cp_val : 7);
        }

        /* Timings */
        cJSON *jidd = cJSON_GetObjectItemCaseSensitive(src_d, "interDeviceDelay");
        int idd = (jidd && cJSON_IsNumber(jidd)) ? jidd->valueint : (jidd && cJSON_IsString(jidd) ? atoi(jidd->valuestring) : 500);
        cJSON_AddNumberToObject(cdev, "interDeviceDelay", idd >= 0 ? idd : 500);

        cJSON *jikd = cJSON_GetObjectItemCaseSensitive(src_d, "interKeyDelay");
        int ikd = (jikd && cJSON_IsNumber(jikd)) ? jikd->valueint : (jikd && cJSON_IsString(jikd) ? atoi(jikd->valuestring) : 100);
        cJSON_AddNumberToObject(cdev, "interKeyDelay", ikd >= 0 ? ikd : 100);

        cJSON *jhikd = cJSON_GetObjectItemCaseSensitive(src_d, "holdInterKeyDelay");
        int hikd = (jhikd && cJSON_IsNumber(jhikd)) ? jhikd->valueint : (jhikd && cJSON_IsString(jhikd) ? atoi(jhikd->valuestring) : 200);
        cJSON_AddNumberToObject(cdev, "holdInterKeyDelay", hikd >= 0 ? hikd : 200);

        cJSON *jdidd = cJSON_GetObjectItemCaseSensitive(src_d, "defaultInterDeviceDelay");
        int didd = (jdidd && cJSON_IsNumber(jdidd)) ? jdidd->valueint : 500;
        cJSON_AddNumberToObject(cdev, "defaultInterDeviceDelay", didd >= 0 ? didd : 500);

        cJSON *jpod = cJSON_GetObjectItemCaseSensitive(src_d, "powerOnDelay");
        int pod = (jpod && cJSON_IsNumber(jpod)) ? jpod->valueint : (jpod && cJSON_IsString(jpod) ? atoi(jpod->valuestring) : 1500);
        cJSON_AddNumberToObject(cdev, "powerOnDelay", pod >= 0 ? pod : 1500);

        cJSON *jpao = cJSON_GetObjectItemCaseSensitive(src_d, "isPowerAlwaysOn");
        int pao = (jpao && (cJSON_IsTrue(jpao) || (cJSON_IsNumber(jpao) && jpao->valuedouble > 0) ||
                   (cJSON_IsString(jpao) && (strcmp(jpao->valuestring, "1") == 0 || strcasecmp(jpao->valuestring, "true") == 0))));
        cJSON_AddBoolToObject(cdev, "isPowerAlwaysOn", pao ? 1 : 0);

        /* BT Reconnect */
        if (is_bt) {
            cJSON *jbd = cJSON_GetObjectItemCaseSensitive(src_d, "bdaddr");
            cJSON_AddStringToObject(cdev, "bdaddr", (jbd && jbd->valuestring) ? jbd->valuestring : "");

            cJSON *jrec = cJSON_GetObjectItemCaseSensitive(src_d, "btAutoReconnect");
            int rec = (jrec && cJSON_IsNumber(jrec)) ? jrec->valueint : (jrec && cJSON_IsBool(jrec) ? (cJSON_IsTrue(jrec) ? 1 : 0) : 1);
            cJSON_AddNumberToObject(cdev, "btAutoReconnect", rec);

            cJSON *jrdur = cJSON_GetObjectItemCaseSensitive(src_d, "btReconnectDurationSec");
            cJSON_AddNumberToObject(cdev, "btReconnectDurationSec", (jrdur && cJSON_IsNumber(jrdur)) ? jrdur->valueint : 0);

            cJSON *jrdel = cJSON_GetObjectItemCaseSensitive(src_d, "btReconnectDelaySec");
            cJSON_AddNumberToObject(cdev, "btReconnectDelaySec", (jrdel && cJSON_IsNumber(jrdel)) ? jrdel->valueint : -1);

            cJSON *j_btd = cJSON_GetObjectItemCaseSensitive(src_d, "btTriggerDeviceId");
            if (j_btd && cJSON_IsString(j_btd) && j_btd->valuestring) {
                cJSON_AddStringToObject(cdev, "btTriggerDeviceId", j_btd->valuestring);
            }
            cJSON *j_btc = cJSON_GetObjectItemCaseSensitive(src_d, "btTriggerCommandName");
            if (j_btc && cJSON_IsString(j_btc) && j_btc->valuestring) {
                cJSON_AddStringToObject(cdev, "btTriggerCommandName", j_btc->valuestring);
            }
        }

        /* MQTT */
        if (is_mqtt) {
            cJSON_AddBoolToObject(cdev, "mqttEnabled", 1);
            cJSON *jtop = cJSON_GetObjectItemCaseSensitive(src_d, "mqttTopic");
            cJSON_AddStringToObject(cdev, "mqttTopic", (jtop && jtop->valuestring && jtop->valuestring[0]) ?
                                    jtop->valuestring : "{base_topic}/button/{device}/{command}");
            cJSON *jpul = cJSON_GetObjectItemCaseSensitive(src_d, "mqttPulseMs");
            cJSON_AddNumberToObject(cdev, "mqttPulseMs", (jpul && cJSON_IsNumber(jpul)) ? jpul->valueint : 1000);
        }

        /* Power Sequences */
        cJSON *po = cJSON_GetObjectItemCaseSensitive(src_d, "powerOnSequence");
        if (po && cJSON_IsArray(po)) {
            cJSON *po_dup = cJSON_Duplicate(po, 1);
            sanitize_activity_steps(po_dup);
            cJSON_AddItemToObject(cdev, "powerOnSequence", po_dup);
        } else {
            cJSON_AddArrayToObject(cdev, "powerOnSequence");
        }

        cJSON *pf = cJSON_GetObjectItemCaseSensitive(src_d, "powerOffSequence");
        if (pf && cJSON_IsArray(pf)) {
            cJSON *pf_dup = cJSON_Duplicate(pf, 1);
            sanitize_activity_steps(pf_dup);
            cJSON_AddItemToObject(cdev, "powerOffSequence", pf_dup);
        } else {
            cJSON_AddArrayToObject(cdev, "powerOffSequence");
        }

        /* Commands */
        cJSON *clean_cmds = cJSON_AddArrayToObject(cdev, "commands");
        cJSON *cmds = cJSON_GetObjectItemCaseSensitive(src_d, "commands");
        int has_connect = 0, has_disconnect = 0;

        if (cmds && cJSON_IsArray(cmds)) {
            cJSON *c = NULL;
            cJSON_ArrayForEach(c, cmds) {
                cJSON *cname = cJSON_GetObjectItemCaseSensitive(c, "name");
                if (!cname || !cname->valuestring || !cname->valuestring[0]) continue;

                if (strcasecmp(cname->valuestring, "Connect") == 0) has_connect = 1;
                if (strcasecmp(cname->valuestring, "Disconnect") == 0) has_disconnect = 1;

                cJSON *ccmd = cJSON_CreateObject();
                cJSON_AddStringToObject(ccmd, "name", cname->valuestring);

                cJSON *ckc = cJSON_GetObjectItemCaseSensitive(c, "keyCode");
                const char *kc_str = (ckc && ckc->valuestring) ? ckc->valuestring : "";
                cJSON_AddStringToObject(ccmd, "keyCode", kc_str);

                cJSON *craw = cJSON_GetObjectItemCaseSensitive(c, "raw");
                const char *raw_str = (craw && craw->valuestring) ? craw->valuestring : "";
                cJSON_AddStringToObject(ccmd, "raw", raw_str[0] ? raw_str : kc_str);

                if (!is_bt && !is_mqtt) {
                    cJSON *cp_item = cJSON_GetObjectItemCaseSensitive(c, "port");
                    cJSON *dev_cp = cJSON_GetObjectItemCaseSensitive(cdev, "controlPort");
                    int port_val = (cp_item && cJSON_IsNumber(cp_item)) ? cp_item->valueint : (dev_cp ? dev_cp->valueint : 7);
                    cJSON_AddNumberToObject(ccmd, "port", port_val);

                    cJSON *crep = cJSON_GetObjectItemCaseSensitive(c, "repeats");
                    int rep_val = (crep && cJSON_IsNumber(crep)) ? crep->valueint : 3;
                    cJSON_AddNumberToObject(ccmd, "repeats", rep_val);
                }

                cJSON *cmacro = cJSON_GetObjectItemCaseSensitive(c, "isMacro");
                if (cmacro && cJSON_IsTrue(cmacro)) cJSON_AddBoolToObject(ccmd, "isMacro", 1);

                cJSON_AddItemToArray(clean_cmds, ccmd);
            }
        }

        if (is_bt) {
            if (!has_connect) {
                cJSON *cc = cJSON_CreateObject();
                cJSON_AddStringToObject(cc, "name", "Connect");
                cJSON_AddStringToObject(cc, "keyCode", "CONNECT");
                cJSON_AddStringToObject(cc, "raw", "CONNECT");
                cJSON_AddItemToArray(clean_cmds, cc);
            }
            if (!has_disconnect) {
                cJSON *dc = cJSON_CreateObject();
                cJSON_AddStringToObject(dc, "name", "Disconnect");
                cJSON_AddStringToObject(dc, "keyCode", "DISCONNECT");
                cJSON_AddStringToObject(dc, "raw", "DISCONNECT");
                cJSON_AddItemToArray(clean_cmds, dc);
            }
        }

        cJSON_AddItemToArray(clean_devs, cdev);
    }

    /* Replace elements in devs with clean_devs */
    while (cJSON_GetArraySize(devs) > 0) {
        cJSON_DeleteItemFromArray(devs, 0);
    }
    while (cJSON_GetArraySize(clean_devs) > 0) {
        cJSON *item = cJSON_DetachItemFromArray(clean_devs, 0);
        cJSON_AddItemToArray(devs, item);
    }
    cJSON_Delete(clean_devs);
}

/* Sanitize activity step arrays (PowerOn/PowerOff sequences, startSequence, stopSequence):
 * 1. Validate fields against canonical schema (type, command, deviceId, delay, order)
 * 2. Prune invalid steps without command or delay
 * 3. Normalize step types to 'Command', 'Delay', 'Switched', 'BTCommand'
 */
void sanitize_activity_steps(cJSON *steps) {
    if (!steps || !cJSON_IsArray(steps)) return;

    cJSON *new_arr = cJSON_CreateArray();
    cJSON *s = NULL;
    cJSON_ArrayForEach(s, steps) {
        if (!s || !cJSON_IsObject(s)) continue;

        cJSON *type_item = cJSON_GetObjectItemCaseSensitive(s, "type");
        const char *tstr = type_item ? type_item->valuestring : "";

        cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(s, "command");
        const char *cmd = cmd_item ? cmd_item->valuestring : "";

        cJSON *dev_item = cJSON_GetObjectItemCaseSensitive(s, "deviceId");
        const char *dev_id = dev_item ? dev_item->valuestring : "";

        cJSON *delay_item = cJSON_GetObjectItemCaseSensitive(s, "delay");
        int delay_val = 0;
        if (delay_item) {
            if (cJSON_IsNumber(delay_item)) delay_val = delay_item->valueint;
            else if (cJSON_IsString(delay_item) && delay_item->valuestring) delay_val = atoi(delay_item->valuestring);
        }

        /* Check for Switched pseudo-step */
        if (strcasecmp(tstr, "Switched") == 0 || strcasecmp(cmd, "Switched") == 0) {
            cJSON *clean_step = cJSON_CreateObject();
            cJSON_AddStringToObject(clean_step, "type", "Switched");
            cJSON_AddStringToObject(clean_step, "command", "Switched");
            cJSON_AddStringToObject(clean_step, "deviceId", "0");
            cJSON_AddNumberToObject(clean_step, "delay", 0);
            cJSON_AddItemToArray(new_arr, clean_step);
            continue;
        }

        /* Check for delay step */
        if (strcasecmp(tstr, "Delay") == 0 || (delay_val > 0 && (!cmd || !cmd[0]))) {
            cJSON *clean_step = cJSON_CreateObject();
            cJSON_AddStringToObject(clean_step, "type", "Delay");
            cJSON_AddStringToObject(clean_step, "command", "");
            cJSON_AddStringToObject(clean_step, "deviceId", "0");
            cJSON_AddNumberToObject(clean_step, "delay", delay_val > 0 ? delay_val : 1000);
            cJSON_AddItemToArray(new_arr, clean_step);
            continue;
        }

        /* Check for BT or IR command step */
        if (cmd && cmd[0]) {
            cJSON *clean_step = cJSON_CreateObject();
            if (strcasecmp(tstr, "BTCommand") == 0 || strncmp(cmd, "HID:", 4) == 0 || strncmp(cmd, "BT:", 3) == 0) {
                cJSON_AddStringToObject(clean_step, "type", "BTCommand");
            } else {
                cJSON_AddStringToObject(clean_step, "type", "Command");
            }
            cJSON_AddStringToObject(clean_step, "command", cmd);
            cJSON_AddStringToObject(clean_step, "deviceId", (dev_id && dev_id[0]) ? dev_id : "0");
            if (delay_val > 0) {
                cJSON_AddNumberToObject(clean_step, "delay", delay_val);
            }
            cJSON_AddItemToArray(new_arr, clean_step);
        }
    }

    /* Replace elements in steps with new_arr */
    while (cJSON_GetArraySize(steps) > 0) {
        cJSON_DeleteItemFromArray(steps, 0);
    }
    while (cJSON_GetArraySize(new_arr) > 0) {
        cJSON *item = cJSON_DetachItemFromArray(new_arr, 0);
        cJSON_AddItemToArray(steps, item);
    }
    cJSON_Delete(new_arr);
}

void sanitize_activities_json(cJSON *root) {
    if (!root) return;
    cJSON *acts = cJSON_GetObjectItemCaseSensitive(root, "activities");
    if (!acts || !cJSON_IsArray(acts)) return;

    cJSON *a = NULL;
    cJSON_ArrayForEach(a, acts) {
        if (!a || !cJSON_IsObject(a)) continue;

        cJSON *start_seq = cJSON_GetObjectItemCaseSensitive(a, "startSequence");
        if (start_seq) sanitize_activity_steps(start_seq);

        cJSON *stop_seq = cJSON_GetObjectItemCaseSensitive(a, "stopSequence");
        if (stop_seq) sanitize_activity_steps(stop_seq);
    }
}
