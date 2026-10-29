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

void convert_legacy_hid_code(const char *in, char *out, size_t outsz) {
    if (!in || !out || outsz < 16) return;
    if (strncmp(in, "G:HID", 5) != 0) {
        strncpy(out, in, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    const char *p = in;
    unsigned int val = 0;
    int found = 0;
    while ((p = strchr(p, '(')) != NULL) {
        p++;
        if (isxdigit((unsigned char)*p)) {
            char hexbuf[16] = {0};
            int i = 0;
            while (isxdigit((unsigned char)*p) && i < 8) {
                hexbuf[i++] = *p++;
            }
            if (i > 0 && *p == ')') {
                val = (unsigned int)strtoul(hexbuf, NULL, 16);
                found = 1;
                break;
            }
        }
    }
    if (found) {
        unsigned int page = (val >> 24) & 0xFF;
        unsigned int usage = val & 0xFFFF;
        if (page == 0) page = 0x07;
        snprintf(out, outsz, "HID:%02X:%04X", page, usage);
    } else {
        strncpy(out, in, outsz - 1);
        out[outsz - 1] = '\0';
    }
}

/* Sanitize devices JSON:
 * 1. Convert root to canonical {"devices": [...]}
 * 2. Unwrap legacy stock {"Device": ..., "Commands": ...} structures
 * 3. Normalize all fields to clean camelCase canonical schema
 * 4. Deduplicate camelCase/snake_case/PascalCase fields and prune stock bloat
 * 5. Normalize commands and power sequences
 */
void sanitize_devices_json(cJSON *root) {
    if (!root) return;

    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs) {
        devs = cJSON_DetachItemFromObjectCaseSensitive(root, "DevicesWithFeatures");
        if (devs) {
            cJSON_AddItemToObject(root, "devices", devs);
        }
    }
    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs || !cJSON_IsArray(devs)) return;

    cJSON *clean_devs = cJSON_CreateArray();

    cJSON *dev_item = NULL;
    cJSON_ArrayForEach(dev_item, devs) {
        cJSON *src_d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
        if (!src_d || !cJSON_IsObject(src_d)) src_d = dev_item;

        cJSON *jid = cJSON_GetObjectItemCaseSensitive(src_d, "id");
        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(src_d, "Id-");
        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(src_d, "Id");
        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(src_d, "ContentProfileKey");
        char id_str[64] = {0};
        if (jid && cJSON_IsNumber(jid)) snprintf(id_str, sizeof(id_str), "%ld", (long)jid->valuedouble);
        else if (jid && cJSON_IsString(jid) && jid->valuestring) strncpy(id_str, jid->valuestring, sizeof(id_str) - 1);
        if (!id_str[0]) continue;

        cJSON *cdev = cJSON_CreateObject();
        cJSON_AddStringToObject(cdev, "id", id_str);

        cJSON *jname = cJSON_GetObjectItemCaseSensitive(src_d, "name");
        if (!jname) jname = cJSON_GetObjectItemCaseSensitive(src_d, "Name");
        if (!jname) jname = cJSON_GetObjectItemCaseSensitive(src_d, "FriendlyName");
        if (!jname) jname = cJSON_GetObjectItemCaseSensitive(src_d, "label");
        const char *name_str = (jname && jname->valuestring && jname->valuestring[0]) ? jname->valuestring : "Device";
        cJSON_AddStringToObject(cdev, "name", name_str);

        cJSON *jmfg = cJSON_GetObjectItemCaseSensitive(src_d, "manufacturer");
        if (!jmfg) jmfg = cJSON_GetObjectItemCaseSensitive(src_d, "Manufacturer");
        const char *mfg_str = (jmfg && jmfg->valuestring) ? jmfg->valuestring : "";
        cJSON_AddStringToObject(cdev, "manufacturer", mfg_str);

        cJSON *jmdl = cJSON_GetObjectItemCaseSensitive(src_d, "model");
        if (!jmdl) jmdl = cJSON_GetObjectItemCaseSensitive(src_d, "Model");
        if (!jmdl) jmdl = cJSON_GetObjectItemCaseSensitive(src_d, "ParentDeviceModel");
        const char *mdl_str = (jmdl && jmdl->valuestring) ? jmdl->valuestring : "";
        cJSON_AddStringToObject(cdev, "model", mdl_str);

        cJSON *jtype = cJSON_GetObjectItemCaseSensitive(src_d, "type");
        if (!jtype) jtype = cJSON_GetObjectItemCaseSensitive(src_d, "deviceType");
        if (!jtype) jtype = cJSON_GetObjectItemCaseSensitive(src_d, "DeviceTypeDisplayName");
        const char *type_str = (jtype && jtype->valuestring) ? jtype->valuestring : "";

        cJSON *jmqtt_en = cJSON_GetObjectItemCaseSensitive(src_d, "mqttEnabled");
        if (!jmqtt_en) jmqtt_en = cJSON_GetObjectItemCaseSensitive(src_d, "mqtt_enabled");
        if (!jmqtt_en) jmqtt_en = cJSON_GetObjectItemCaseSensitive(src_d, "MQTTEnabled");
        int is_mqtt = (strcasecmp(type_str, "MQTT") == 0 ||
                       (jmqtt_en && (cJSON_IsTrue(jmqtt_en) || (cJSON_IsNumber(jmqtt_en) && jmqtt_en->valueint == 1))));

        cJSON *jtrans = cJSON_GetObjectItemCaseSensitive(src_d, "transport");
        if (!jtrans) jtrans = cJSON_GetObjectItemCaseSensitive(src_d, "Transport");
        int is_bt = (!is_mqtt) && ((jtrans && cJSON_IsNumber(jtrans) && jtrans->valueint == 32) ||
                       strcasecmp(type_str, "BT") == 0 || strcasecmp(type_str, "Bluetooth") == 0 ||
                       strcasestr(type_str, "bluetooth") != NULL || strcasestr(mfg_str, "bluetooth") != NULL ||
                       strcasecmp(type_str, "BTHID") == 0 || strcasecmp(type_str, "AppleTV") == 0 ||
                       strcasecmp(type_str, "FireTV") == 0 || strcasecmp(type_str, "Shield") == 0 ||
                       strcasecmp(type_str, "PlayStation") == 0 || strcasecmp(type_str, "PS4") == 0);

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
        if (!jicon) jicon = cJSON_GetObjectItemCaseSensitive(src_d, "Icon");
        if (jicon) {
            if (cJSON_IsString(jicon) && jicon->valuestring) cJSON_AddStringToObject(cdev, "icon", jicon->valuestring);
            else if (cJSON_IsNumber(jicon)) cJSON_AddNumberToObject(cdev, "icon", jicon->valueint);
        }

        if (!is_bt && !is_mqtt) {
            cJSON *jcp = cJSON_GetObjectItemCaseSensitive(src_d, "controlPort");
            if (!jcp) jcp = cJSON_GetObjectItemCaseSensitive(src_d, "ControlPort");
            int cp_val = (jcp && cJSON_IsNumber(jcp)) ? jcp->valueint : 7;
            cJSON_AddNumberToObject(cdev, "controlPort", (cp_val >= 0 && cp_val <= 7) ? cp_val : 7);
        }

        /* Timings */
        cJSON *jidd = cJSON_GetObjectItemCaseSensitive(src_d, "interDeviceDelay");
        if (!jidd) jidd = cJSON_GetObjectItemCaseSensitive(src_d, "inter_device_delay");
        if (!jidd) jidd = cJSON_GetObjectItemCaseSensitive(src_d, "InterDeviceDelay");
        int idd = (jidd && cJSON_IsNumber(jidd)) ? jidd->valueint : (jidd && cJSON_IsString(jidd) ? atoi(jidd->valuestring) : 500);
        cJSON_AddNumberToObject(cdev, "interDeviceDelay", idd >= 0 ? idd : 500);

        cJSON *jikd = cJSON_GetObjectItemCaseSensitive(src_d, "interKeyDelay");
        if (!jikd) jikd = cJSON_GetObjectItemCaseSensitive(src_d, "inter_key_delay");
        if (!jikd) jikd = cJSON_GetObjectItemCaseSensitive(src_d, "InterKeyDelay");
        int ikd = (jikd && cJSON_IsNumber(jikd)) ? jikd->valueint : (jikd && cJSON_IsString(jikd) ? atoi(jikd->valuestring) : 100);
        cJSON_AddNumberToObject(cdev, "interKeyDelay", ikd >= 0 ? ikd : 100);

        cJSON *jhikd = cJSON_GetObjectItemCaseSensitive(src_d, "holdInterKeyDelay");
        if (!jhikd) jhikd = cJSON_GetObjectItemCaseSensitive(src_d, "hold_inter_key_delay");
        if (!jhikd) jhikd = cJSON_GetObjectItemCaseSensitive(src_d, "HoldInterKeyDelay");
        int hikd = (jhikd && cJSON_IsNumber(jhikd)) ? jhikd->valueint : (jhikd && cJSON_IsString(jhikd) ? atoi(jhikd->valuestring) : 200);
        cJSON_AddNumberToObject(cdev, "holdInterKeyDelay", hikd >= 0 ? hikd : 200);

        cJSON *jdidd = cJSON_GetObjectItemCaseSensitive(src_d, "defaultInterDeviceDelay");
        if (!jdidd) jdidd = cJSON_GetObjectItemCaseSensitive(src_d, "DefaultInterDeviceDelay");
        int didd = (jdidd && cJSON_IsNumber(jdidd)) ? jdidd->valueint : 500;
        cJSON_AddNumberToObject(cdev, "defaultInterDeviceDelay", didd >= 0 ? didd : 500);

        cJSON *jpod = cJSON_GetObjectItemCaseSensitive(src_d, "powerOnDelay");
        if (!jpod) jpod = cJSON_GetObjectItemCaseSensitive(src_d, "power_on_delay");
        if (!jpod) jpod = cJSON_GetObjectItemCaseSensitive(src_d, "PowerOnDelay");
        int pod = (jpod && cJSON_IsNumber(jpod)) ? jpod->valueint : (jpod && cJSON_IsString(jpod) ? atoi(jpod->valuestring) : 1500);
        cJSON_AddNumberToObject(cdev, "powerOnDelay", pod >= 0 ? pod : 1500);

        cJSON *jpao = cJSON_GetObjectItemCaseSensitive(src_d, "isPowerAlwaysOn");
        if (!jpao) jpao = cJSON_GetObjectItemCaseSensitive(src_d, "is_power_always_on");
        if (!jpao) jpao = cJSON_GetObjectItemCaseSensitive(src_d, "IsPowerAlwaysOn");
        int pao = (jpao && (cJSON_IsTrue(jpao) || (cJSON_IsNumber(jpao) && jpao->valuedouble > 0) ||
                   (cJSON_IsString(jpao) && (strcmp(jpao->valuestring, "1") == 0 || strcasecmp(jpao->valuestring, "true") == 0))));
        cJSON_AddBoolToObject(cdev, "isPowerAlwaysOn", pao ? 1 : 0);

        /* BT Reconnect */
        if (is_bt) {
            cJSON *jbd = cJSON_GetObjectItemCaseSensitive(src_d, "bdaddr");
            if (!jbd) jbd = cJSON_GetObjectItemCaseSensitive(src_d, "BTAddress");
            if (!jbd) jbd = cJSON_GetObjectItemCaseSensitive(src_d, "bt_host");
            if (!jbd) jbd = cJSON_GetObjectItemCaseSensitive(src_d, "btHost");
            cJSON_AddStringToObject(cdev, "bdaddr", (jbd && jbd->valuestring) ? jbd->valuestring : "");

            cJSON *jrec = cJSON_GetObjectItemCaseSensitive(src_d, "btAutoReconnect");
            if (!jrec) jrec = cJSON_GetObjectItemCaseSensitive(src_d, "bt_auto_reconnect");
            int rec = (jrec && cJSON_IsNumber(jrec)) ? jrec->valueint : (jrec && cJSON_IsBool(jrec) ? (cJSON_IsTrue(jrec) ? 1 : 0) : 1);
            cJSON_AddNumberToObject(cdev, "btAutoReconnect", rec);

            cJSON *jrdur = cJSON_GetObjectItemCaseSensitive(src_d, "btReconnectDurationSec");
            if (!jrdur) jrdur = cJSON_GetObjectItemCaseSensitive(src_d, "btReconnectDuration");
            if (!jrdur) jrdur = cJSON_GetObjectItemCaseSensitive(src_d, "bt_reconnect_duration_sec");
            int rdur = (jrdur && cJSON_IsNumber(jrdur)) ? jrdur->valueint : 30;
            cJSON_AddNumberToObject(cdev, "btReconnectDurationSec", rdur >= 0 ? rdur : 30);

            cJSON *jrdly = cJSON_GetObjectItemCaseSensitive(src_d, "btReconnectDelaySec");
            if (!jrdly) jrdly = cJSON_GetObjectItemCaseSensitive(src_d, "btReconnectDelay");
            if (!jrdly) jrdly = cJSON_GetObjectItemCaseSensitive(src_d, "bt_reconnect_delay_sec");
            int rdly = (jrdly && cJSON_IsNumber(jrdly)) ? jrdly->valueint : 2;
            cJSON_AddNumberToObject(cdev, "btReconnectDelaySec", rdly >= 0 ? rdly : 2);

            cJSON *jtd = cJSON_GetObjectItemCaseSensitive(src_d, "btTriggerDeviceId");
            if (!jtd) jtd = cJSON_GetObjectItemCaseSensitive(src_d, "bt_trigger_device_id");
            if (jtd && jtd->valuestring && jtd->valuestring[0]) {
                cJSON_AddStringToObject(cdev, "btTriggerDeviceId", jtd->valuestring);
            }

            cJSON *jtc = cJSON_GetObjectItemCaseSensitive(src_d, "btTriggerCommandName");
            if (!jtc) jtc = cJSON_GetObjectItemCaseSensitive(src_d, "bt_trigger_command_name");
            if (!jtc) jtc = cJSON_GetObjectItemCaseSensitive(src_d, "bt_trigger_command");
            if (!jtc) jtc = cJSON_GetObjectItemCaseSensitive(src_d, "btTriggerCommand");
            if (jtc && jtc->valuestring && jtc->valuestring[0]) {
                cJSON_AddStringToObject(cdev, "btTriggerCommandName", jtc->valuestring);
            }
        }

        /* MQTT */
        if (is_mqtt) {
            cJSON_AddBoolToObject(cdev, "mqttEnabled", 1);
            cJSON *jtop = cJSON_GetObjectItemCaseSensitive(src_d, "mqttTopic");
            if (!jtop) jtop = cJSON_GetObjectItemCaseSensitive(src_d, "mqtt_topic");
            if (!jtop) jtop = cJSON_GetObjectItemCaseSensitive(src_d, "MQTTTopic");
            cJSON_AddStringToObject(cdev, "mqttTopic", (jtop && jtop->valuestring && jtop->valuestring[0]) ? jtop->valuestring : "{base_topic}/button/{device}/{command}");

            cJSON *jpulse = cJSON_GetObjectItemCaseSensitive(src_d, "mqttPulseMs");
            if (!jpulse) jpulse = cJSON_GetObjectItemCaseSensitive(src_d, "mqtt_pulse_ms");
            if (!jpulse) jpulse = cJSON_GetObjectItemCaseSensitive(src_d, "MQTTPulseMs");
            if (!jpulse) jpulse = cJSON_GetObjectItemCaseSensitive(src_d, "mqttPulse");
            int pulse = (jpulse && cJSON_IsNumber(jpulse) && jpulse->valueint > 0) ? jpulse->valueint : 1000;
            cJSON_AddNumberToObject(cdev, "mqttPulseMs", pulse);
        }

        /* Power Sequences */
        cJSON *jpo = cJSON_GetObjectItemCaseSensitive(src_d, "powerOnSequence");
        if (!jpo) jpo = cJSON_GetObjectItemCaseSensitive(src_d, "power_on_sequence");
        if (!jpo) jpo = cJSON_GetObjectItemCaseSensitive(src_d, "PowerOnActions");
        if (!jpo) jpo = cJSON_GetObjectItemCaseSensitive(dev_item, "PowerOnActions");
        if (jpo && cJSON_IsArray(jpo)) {
            cJSON *po_dup = cJSON_Duplicate(jpo, 1);
            sanitize_activity_steps(po_dup);
            cJSON_AddItemToObject(cdev, "powerOnSequence", po_dup);
        }

        cJSON *jpf = cJSON_GetObjectItemCaseSensitive(src_d, "powerOffSequence");
        if (!jpf) jpf = cJSON_GetObjectItemCaseSensitive(src_d, "power_off_sequence");
        if (!jpf) jpf = cJSON_GetObjectItemCaseSensitive(src_d, "PowerOffActions");
        if (!jpf) jpf = cJSON_GetObjectItemCaseSensitive(dev_item, "PowerOffActions");
        if (jpf && cJSON_IsArray(jpf)) {
            cJSON *pf_dup = cJSON_Duplicate(jpf, 1);
            sanitize_activity_steps(pf_dup);
            cJSON_AddItemToObject(cdev, "powerOffSequence", pf_dup);
        }

        /* Commands */
        cJSON *cmds = cJSON_GetObjectItemCaseSensitive(src_d, "commands");
        if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(src_d, "Commands");
        if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "commands");
        if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "Commands");

        cJSON *clean_cmds = cJSON_AddArrayToObject(cdev, "commands");
        int has_connect = 0, has_disconnect = 0;

        if (cmds && cJSON_IsArray(cmds)) {
            cJSON *c_item = NULL;
            cJSON_ArrayForEach(c_item, cmds) {
                cJSON *c = cJSON_GetObjectItemCaseSensitive(c_item, "Command");
                if (!c) c = c_item;

                cJSON *cname = cJSON_GetObjectItemCaseSensitive(c, "name");
                if (!cname) cname = cJSON_GetObjectItemCaseSensitive(c, "Name");
                if (!cname || !cname->valuestring || !cname->valuestring[0]) continue;
                const char *cn = cname->valuestring;

                if (strcasecmp(cn, "Connect") == 0) has_connect = 1;
                if (strcasecmp(cn, "Disconnect") == 0) has_disconnect = 1;

                cJSON *ccmd = cJSON_CreateObject();
                cJSON_AddStringToObject(ccmd, "name", cn);

                cJSON *ckc = cJSON_GetObjectItemCaseSensitive(c, "keyCode");
                if (!ckc) ckc = cJSON_GetObjectItemCaseSensitive(c, "KeyCode");
                if (!ckc) ckc = cJSON_GetObjectItemCaseSensitive(c, "action");
                if (!ckc) ckc = cJSON_GetObjectItemCaseSensitive(c, "IRCode");
                const char *kc_str = (ckc && ckc->valuestring) ? ckc->valuestring : "";

                cJSON *craw = cJSON_GetObjectItemCaseSensitive(c, "raw");
                if (!craw) craw = cJSON_GetObjectItemCaseSensitive(c, "Raw");
                const char *raw_str = (craw && craw->valuestring) ? craw->valuestring : "";

                char clean_key[128] = {0};
                convert_legacy_hid_code(kc_str, clean_key, sizeof(clean_key));
                cJSON_AddStringToObject(ccmd, "keyCode", clean_key[0] ? clean_key : kc_str);

                char clean_raw[128] = {0};
                convert_legacy_hid_code(raw_str, clean_raw, sizeof(clean_raw));
                cJSON_AddStringToObject(ccmd, "raw", clean_raw[0] ? clean_raw : (raw_str[0] ? raw_str : clean_key));

                if (!is_bt && !is_mqtt) {
                    cJSON *cp_item = cJSON_GetObjectItemCaseSensitive(c, "port");
                    if (!cp_item) cp_item = cJSON_GetObjectItemCaseSensitive(c, "controlPort");
                    if (!cp_item) cp_item = cJSON_GetObjectItemCaseSensitive(c, "ControlPort");
                    cJSON *dev_cp = cJSON_GetObjectItemCaseSensitive(cdev, "controlPort");
                    int port_val = (cp_item && cJSON_IsNumber(cp_item)) ? cp_item->valueint : (dev_cp ? dev_cp->valueint : 7);
                    cJSON_AddNumberToObject(ccmd, "port", port_val);

                    cJSON *crep = cJSON_GetObjectItemCaseSensitive(c, "repeats");
                    if (!crep) crep = cJSON_GetObjectItemCaseSensitive(c, "Repeats");
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
 * 1. Convert string delay_ms/delay to integers
 * 2. Prune invalid steps without command or delay
 * 3. Normalize step types to 'Command', 'Delay', 'Switched', 'BTCommand'
 */
void sanitize_activity_steps(cJSON *steps) {
    if (!steps || !cJSON_IsArray(steps)) return;

    cJSON *new_arr = cJSON_CreateArray();
    cJSON *step = NULL;
    cJSON_ArrayForEach(step, steps) {
        cJSON *s = cJSON_GetObjectItemCaseSensitive(step, "Action");
        if (!s) s = step;

        cJSON *type_item = cJSON_GetObjectItemCaseSensitive(s, "type");
        if (!type_item) type_item = cJSON_GetObjectItemCaseSensitive(s, "Type");
        const char *tstr = type_item ? type_item->valuestring : "";

        cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(s, "command");
        if (!cmd_item) cmd_item = cJSON_GetObjectItemCaseSensitive(s, "Command");
        if (!cmd_item) cmd_item = cJSON_GetObjectItemCaseSensitive(s, "CommandName");
        if (!cmd_item) cmd_item = cJSON_GetObjectItemCaseSensitive(s, "IRCommandName");
        const char *cmd = cmd_item ? cmd_item->valuestring : "";

        cJSON *dev_item = cJSON_GetObjectItemCaseSensitive(s, "deviceId");
        if (!dev_item) dev_item = cJSON_GetObjectItemCaseSensitive(s, "device_id");
        if (!dev_item) dev_item = cJSON_GetObjectItemCaseSensitive(s, "DeviceId");
        if (!dev_item) dev_item = cJSON_GetObjectItemCaseSensitive(s, "DeviceId-");
        const char *dev_id = dev_item ? dev_item->valuestring : "";

        cJSON *delay_item = cJSON_GetObjectItemCaseSensitive(s, "delay");
        if (!delay_item) delay_item = cJSON_GetObjectItemCaseSensitive(s, "delay_ms");
        if (!delay_item) delay_item = cJSON_GetObjectItemCaseSensitive(s, "delayMs");
        if (!delay_item) delay_item = cJSON_GetObjectItemCaseSensitive(s, "Delay");
        if (!delay_item) delay_item = cJSON_GetObjectItemCaseSensitive(s, "Duration");
        int delay_val = 0;
        if (delay_item) {
            if (cJSON_IsNumber(delay_item)) delay_val = delay_item->valueint;
            else if (cJSON_IsString(delay_item) && delay_item->valuestring) delay_val = atoi(delay_item->valuestring);
        }

        /* Check for Switched pseudo-step */
        if (strcasecmp(tstr, "Switched") == 0 || strcasecmp(tstr, "ActivitySwitched") == 0 || strcasecmp(cmd, "Switched") == 0) {
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
    if (!acts) acts = cJSON_GetObjectItemCaseSensitive(root, "Activities");
    if (!acts || !cJSON_IsArray(acts)) return;

    cJSON *act_item = NULL;
    cJSON_ArrayForEach(act_item, acts) {
        cJSON *a = cJSON_GetObjectItemCaseSensitive(act_item, "Activity");
        if (!a) a = act_item;

        cJSON *start_seq = cJSON_GetObjectItemCaseSensitive(a, "startSequence");
        if (!start_seq) start_seq = cJSON_GetObjectItemCaseSensitive(a, "EnterActions");
        sanitize_activity_steps(start_seq);

        cJSON *stop_seq = cJSON_GetObjectItemCaseSensitive(a, "stopSequence");
        if (!stop_seq) stop_seq = cJSON_GetObjectItemCaseSensitive(a, "LeaveActions");
        sanitize_activity_steps(stop_seq);
    }
}
