#define _GNU_SOURCE
#include "config_sanitize.h"
#include "config_store.h"
#include "cJSON.h"
#include "bt_common.h"
#include "json_number.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

/* Names end up in HTML (both web UIs, Home Assistant cards, other clients): keep '<', '>'
 * and control characters out of them, so a name can never become markup. Every other
 * character is kept. Returns out. */
static const char *clean_display_name(const char *in, char *out, size_t out_sz) {
    size_t j = 0;
    if (!out || out_sz == 0) return "";
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && j + 1 < out_sz; p++) {
        if (*p == '<' || *p == '>' || *p < 0x20 || *p == 0x7f) continue;
        out[j++] = (char)*p;
    }
    out[j] = '\0';
    return out;
}

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
        if (jid && cJSON_IsNumber(jid)) json_number_text(jid, id_str, sizeof(id_str));
        else if (jid && cJSON_IsString(jid) && jid->valuestring) strncpy(id_str, jid->valuestring, sizeof(id_str) - 1);
        if (!id_str[0]) {
            /* not addressable without an id; say so, the next save no longer has it */
            cJSON *jn = cJSON_GetObjectItemCaseSensitive(src_d, "name");
            fprintf(stderr, "[!] Config: device without an id left out (name \"%s\")\n",
                    cJSON_IsString(jn) && jn->valuestring ? jn->valuestring : "");
            continue;
        }
        {
            /* every lookup takes the first device of an id: a second one cannot be reached */
            cJSON *seen = NULL;
            cJSON_ArrayForEach(seen, clean_devs) {
                const char *sid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(seen, "id"));
                if (sid && strcmp(sid, id_str) == 0) {
                    fprintf(stderr, "[!] Config: device id \"%s\" is used twice: only the first is used\n", id_str);
                    break;
                }
            }
        }

        cJSON *cdev = cJSON_CreateObject();
        cJSON_AddStringToObject(cdev, "id", id_str);

        cJSON *jname = cJSON_GetObjectItemCaseSensitive(src_d, "name");
        char name_buf[256];
        const char *name_str = (jname && jname->valuestring && jname->valuestring[0]) ? jname->valuestring : "Device";
        name_str = clean_display_name(name_str, name_buf, sizeof(name_buf));
        cJSON_AddStringToObject(cdev, "name", name_str[0] ? name_str : "Device");

        cJSON *jmfg = cJSON_GetObjectItemCaseSensitive(src_d, "manufacturer");
        char mfg_buf[256];
        const char *mfg_str = clean_display_name((jmfg && jmfg->valuestring) ? jmfg->valuestring : "", mfg_buf, sizeof(mfg_buf));
        cJSON_AddStringToObject(cdev, "manufacturer", mfg_str);

        cJSON *jmdl = cJSON_GetObjectItemCaseSensitive(src_d, "model");
        char mdl_buf[256];
        const char *mdl_str = clean_display_name((jmdl && jmdl->valuestring) ? jmdl->valuestring : "", mdl_buf, sizeof(mdl_buf));
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
            /* A device that is Bluetooth only by "transport": 32 must say so in its type: the
             * output has no transport field, and the next pass would take it for IR and drop its
             * Bluetooth settings */
            int type_says_bt = strcasecmp(type_str, "BT") == 0 || strcasestr(type_str, "bluetooth") != NULL ||
                               strcasecmp(type_str, "BTHID") == 0 || strcasestr(mfg_str, "bluetooth") != NULL;
            final_type = (type_str && type_str[0] && type_says_bt) ? type_str : "BT";
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
        if (idd > DEVICE_DELAY_MAX_MS) idd = DEVICE_DELAY_MAX_MS;
        cJSON_AddNumberToObject(cdev, "interDeviceDelay", idd >= 0 ? idd : 500);

        cJSON *jikd = cJSON_GetObjectItemCaseSensitive(src_d, "interKeyDelay");
        int ikd = (jikd && cJSON_IsNumber(jikd)) ? jikd->valueint : (jikd && cJSON_IsString(jikd) ? atoi(jikd->valuestring) : 100);
        if (ikd > DEVICE_DELAY_MAX_MS) ikd = DEVICE_DELAY_MAX_MS;
        cJSON_AddNumberToObject(cdev, "interKeyDelay", ikd >= 0 ? ikd : 100);

        cJSON *jhikd = cJSON_GetObjectItemCaseSensitive(src_d, "holdInterKeyDelay");
        int hikd = (jhikd && cJSON_IsNumber(jhikd)) ? jhikd->valueint : (jhikd && cJSON_IsString(jhikd) ? atoi(jhikd->valuestring) : 200);
        if (hikd > DEVICE_DELAY_MAX_MS) hikd = DEVICE_DELAY_MAX_MS;
        cJSON_AddNumberToObject(cdev, "holdInterKeyDelay", hikd >= 0 ? hikd : 200);

        cJSON *jdidd = cJSON_GetObjectItemCaseSensitive(src_d, "defaultInterDeviceDelay");
        int didd = (jdidd && cJSON_IsNumber(jdidd)) ? json_number_int(jdidd, -1, DEVICE_DELAY_MAX_MS, 500) : 500;
        cJSON_AddNumberToObject(cdev, "defaultInterDeviceDelay", didd >= 0 ? didd : 500);

        cJSON *jpod = cJSON_GetObjectItemCaseSensitive(src_d, "powerOnDelay");
        int pod = (jpod && cJSON_IsNumber(jpod)) ? json_number_int(jpod, -1, POWER_ON_DELAY_MAX_MS, 1500)
                                                 : (jpod && cJSON_IsString(jpod) ? atoi(jpod->valuestring) : 1500);
        if (pod > POWER_ON_DELAY_MAX_MS) pod = POWER_ON_DELAY_MAX_MS;
        cJSON_AddNumberToObject(cdev, "powerOnDelay", pod >= 0 ? pod : 1500);

        cJSON *jpao = cJSON_GetObjectItemCaseSensitive(src_d, "isPowerAlwaysOn");
        int pao = (jpao && (cJSON_IsTrue(jpao) || (cJSON_IsNumber(jpao) && jpao->valuedouble > 0) ||
                   (cJSON_IsString(jpao) && (strcmp(jpao->valuestring, "1") == 0 || strcasecmp(jpao->valuestring, "true") == 0))));
        cJSON_AddBoolToObject(cdev, "isPowerAlwaysOn", pao ? 1 : 0);

        /* The device's Bluetooth host: bdaddr, else the older btHost key. The rebuilt object
         * keeps only bdaddr, so a btHost-only device would otherwise lose its host on save. */
        const char *host_addr = NULL;
        {
            const char *keys[] = {"bdaddr", "btHost", NULL};
            for (int k = 0; keys[k] && !host_addr; k++) {
                cJSON *jh = cJSON_GetObjectItemCaseSensitive(src_d, keys[k]);
                if (jh && cJSON_IsString(jh) && jh->valuestring && jh->valuestring[0]) host_addr = jh->valuestring;
            }
        }

        /* A device of another transport can still be linked to a Bluetooth host (a TV driven by
         * IR that pairs as a keyboard host): keep its address, it names the host and is the
         * only place the link lives (POST /api/bt-link-device). Keys still go by type. */
        if (!is_bt && host_addr) {
            cJSON_AddStringToObject(cdev, "bdaddr", host_addr);
        }

        /* BT Reconnect */
        if (is_bt) {
            cJSON_AddStringToObject(cdev, "bdaddr", host_addr ? host_addr : "");

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

                char cmd_name[128];
                clean_display_name(cname->valuestring, cmd_name, sizeof(cmd_name));
                if (!cmd_name[0]) continue;
                cJSON *ccmd = cJSON_CreateObject();
                cJSON_AddStringToObject(ccmd, "name", cmd_name);

                cJSON *ckc = cJSON_GetObjectItemCaseSensitive(c, "keyCode");
                const char *kc_str = (ckc && ckc->valuestring) ? ckc->valuestring : "";
                cJSON_AddStringToObject(ccmd, "keyCode", kc_str);

                cJSON *craw = cJSON_GetObjectItemCaseSensitive(c, "raw");
                const char *raw_str = (craw && craw->valuestring) ? craw->valuestring : "";
                cJSON_AddStringToObject(ccmd, "raw", raw_str[0] ? raw_str : kc_str);

                if (!is_bt && !is_mqtt) {
                    cJSON *cp_item = cJSON_GetObjectItemCaseSensitive(c, "port");
                    cJSON *dev_cp = cJSON_GetObjectItemCaseSensitive(cdev, "controlPort");
                    /* ports are the same 0..7 as controlPort; the sender keeps repeats in 8 bits */
                    int dev_port = dev_cp ? dev_cp->valueint : 7;
                    int port_val = (cp_item && cJSON_IsNumber(cp_item)) ? json_number_int(cp_item, -1, 8, dev_port) : dev_port;
                    if (port_val < 0 || port_val > 7) port_val = dev_port;
                    cJSON_AddNumberToObject(ccmd, "port", port_val);

                    cJSON *crep = cJSON_GetObjectItemCaseSensitive(c, "repeats");
                    int rep_val = (crep && cJSON_IsNumber(crep)) ? json_number_int(crep, -1, 256, 3) : 3;
                    if (rep_val < 0 || rep_val > 255) rep_val = 3;
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

        /* cJSON_GetStringValue() is NULL for a missing key and for any type but a string
         * (null, a number, a bool...), whose valuestring is NULL too */
        const char *tstr = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s, "type"));
        if (!tstr) tstr = "";

        const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s, "command"));
        if (!cmd) cmd = "";

        cJSON *dev_item = cJSON_GetObjectItemCaseSensitive(s, "deviceId");
        const char *dev_id = cJSON_GetStringValue(dev_item);
        char dev_num[32];
        if (!dev_id && cJSON_IsNumber(dev_item)) {
            /* A numeric device id is a valid id: keep it instead of dropping it to "0" */
            if (json_number_text(dev_item, dev_num, sizeof(dev_num))) dev_id = dev_num;
        }
        if (!dev_id) dev_id = "";

        cJSON *delay_item = cJSON_GetObjectItemCaseSensitive(s, "delay");
        int delay_val = 0;
        if (delay_item) {
            if (cJSON_IsNumber(delay_item)) delay_val = json_number_int(delay_item, -1, STEP_DELAY_MAX_MS, 0);
            else if (cJSON_IsString(delay_item) && delay_item->valuestring) delay_val = atoi(delay_item->valuestring);
        }
        if (delay_val > STEP_DELAY_MAX_MS) delay_val = STEP_DELAY_MAX_MS;

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

        /* A start request finds the first activity of an id: say when one cannot be reached */
        char aid[32] = "";
        cJSON *jaid = cJSON_GetObjectItemCaseSensitive(a, "id");
        if (cJSON_IsString(jaid) && jaid->valuestring) snprintf(aid, sizeof(aid), "%s", jaid->valuestring);
        else json_number_text(jaid, aid, sizeof(aid));
        if (!aid[0]) {
            fprintf(stderr, "[!] Config: activity without an id cannot be started\n");
        } else {
            cJSON *prev = NULL;
            cJSON_ArrayForEach(prev, acts) {
                if (prev == a) break;
                char pid[32] = "";
                cJSON *jp = cJSON_GetObjectItemCaseSensitive(prev, "id");
                if (cJSON_IsString(jp) && jp->valuestring) snprintf(pid, sizeof(pid), "%s", jp->valuestring);
                else json_number_text(jp, pid, sizeof(pid));
                if (strcmp(pid, aid) == 0) {
                    fprintf(stderr, "[!] Config: activity id \"%s\" is used twice: only the first is used\n", aid);
                    break;
                }
            }
        }

        cJSON *aname = cJSON_GetObjectItemCaseSensitive(a, "name");
        if (cJSON_IsString(aname) && aname->valuestring) {
            char clean[256];
            clean_display_name(aname->valuestring, clean, sizeof(clean));
            if (strcmp(clean, aname->valuestring) != 0) {
                cJSON_ReplaceItemInObjectCaseSensitive(a, "name", cJSON_CreateString(clean[0] ? clean : "Activity"));
            }
        }

        cJSON *start_seq = cJSON_GetObjectItemCaseSensitive(a, "startSequence");
        if (start_seq) sanitize_activity_steps(start_seq);

        cJSON *stop_seq = cJSON_GetObjectItemCaseSensitive(a, "stopSequence");
        if (stop_seq) sanitize_activity_steps(stop_seq);
    }
}
