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
 * 1. Switch device type to 'IR', 'BT', or 'MQTT'
 * 2. Ensure IR timings and BT reconnect settings exist
 * 3. Convert legacy G:HID codes to clean HID:<page>:<usage>
 * 4. Add undeletable Connect & Disconnect commands for BT devices
 * 5. Deduplicate MQTT settings and strip IR fields from BT/MQTT
 */
void sanitize_devices_json(cJSON *root) {
    if (!root) return;
    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
    if (!devs || !cJSON_IsArray(devs)) return;

    cJSON *dev_item = NULL;
    cJSON_ArrayForEach(dev_item, devs) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
        if (!d) d = dev_item;

        cJSON *dev_type_item = cJSON_GetObjectItemCaseSensitive(d, "deviceType");
        if (!dev_type_item) dev_type_item = cJSON_GetObjectItemCaseSensitive(d, "type");
        const char *orig_type = dev_type_item ? dev_type_item->valuestring : "";

        /* Determine transport category: BT, MQTT, or IR */
        int is_bt = 0;
        int is_mqtt = 0;
        cJSON *mqtt_en = cJSON_GetObjectItemCaseSensitive(d, "mqtt_enabled");
        if (mqtt_en && (cJSON_IsTrue(mqtt_en) || (cJSON_IsNumber(mqtt_en) && mqtt_en->valueint == 1))) {
            is_mqtt = 1;
        }

        if (strcasecmp(orig_type, "BT") == 0 ||
            strcasecmp(orig_type, "Bluetooth") == 0 ||
            strcasecmp(orig_type, "BTKeyboard") == 0 ||
            strcasecmp(orig_type, "BTHID") == 0 ||
            strcasecmp(orig_type, "AppleTV") == 0 ||
            strcasecmp(orig_type, "FireTV") == 0 ||
            strcasecmp(orig_type, "Shield") == 0 ||
            strcasecmp(orig_type, "PlayStation") == 0 ||
            strcasecmp(orig_type, "PS4") == 0) {
            is_bt = 1;
        }

        /* Sanitize type string */
        if (is_mqtt) {
            cJSON_DeleteItemFromObject(d, "deviceType");
            cJSON_DeleteItemFromObject(d, "type");
            cJSON_AddStringToObject(d, "type", "MQTT");
        } else if (is_bt) {
            cJSON_DeleteItemFromObject(d, "deviceType");
            cJSON_DeleteItemFromObject(d, "type");
            cJSON_AddStringToObject(d, "type", "BT");
        } else {
            cJSON_DeleteItemFromObject(d, "deviceType");
            cJSON_DeleteItemFromObject(d, "type");
            cJSON_AddStringToObject(d, "type", "IR");
        }

        /* Clean up unused protocol/transport fields */
        if (is_bt || is_mqtt) {
            cJSON_DeleteItemFromObject(d, "controlPort");
            cJSON_DeleteItemFromObject(d, "ControlPort");
            cJSON_DeleteItemFromObject(d, "protocolId");
            cJSON_DeleteItemFromObject(d, "transport");
        }

        /* Standardize IR timings if IR */
        if (!is_bt && !is_mqtt) {
            if (!cJSON_GetObjectItemCaseSensitive(d, "interDeviceDelay")) {
                cJSON_AddNumberToObject(d, "interDeviceDelay", 500);
            }
            if (!cJSON_GetObjectItemCaseSensitive(d, "interKeyDelay")) {
                cJSON_AddNumberToObject(d, "interKeyDelay", 100);
            }
            if (!cJSON_GetObjectItemCaseSensitive(d, "holdInterKeyDelay")) {
                cJSON_AddNumberToObject(d, "holdInterKeyDelay", 200);
            }
        }

        /* Standardize Bluetooth reconnect settings */
        if (is_bt) {
            if (!cJSON_GetObjectItemCaseSensitive(d, "bt_auto_reconnect")) {
                cJSON_AddNumberToObject(d, "bt_auto_reconnect", 1);
            }
            if (!cJSON_GetObjectItemCaseSensitive(d, "bt_reconnect_duration_sec")) {
                cJSON_AddNumberToObject(d, "bt_reconnect_duration_sec", 30);
            }
            if (!cJSON_GetObjectItemCaseSensitive(d, "bt_reconnect_delay_sec")) {
                cJSON_AddNumberToObject(d, "bt_reconnect_delay_sec", 2);
            }
        }

        /* Sanitize commands array */
        cJSON *cmds = cJSON_GetObjectItemCaseSensitive(d, "commands");
        if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(d, "Commands");
        if (cmds && cJSON_IsArray(cmds)) {
            int has_connect = 0;
            int has_disconnect = 0;
            cJSON *cmd_item = NULL;
            cJSON_ArrayForEach(cmd_item, cmds) {
                cJSON *c = cJSON_GetObjectItemCaseSensitive(cmd_item, "Command");
                if (!c) c = cmd_item;

                cJSON *cname = cJSON_GetObjectItemCaseSensitive(c, "name");
                if (!cname) cname = cJSON_GetObjectItemCaseSensitive(c, "Name");
                const char *cn = cname ? cname->valuestring : "";

                if (strcasecmp(cn, "Connect") == 0) has_connect = 1;
                if (strcasecmp(cn, "Disconnect") == 0) has_disconnect = 1;

                /* Sanitize keycode/action for BT */
                if (is_bt) {
                    cJSON *kc = cJSON_GetObjectItemCaseSensitive(c, "action");
                    if (!kc) kc = cJSON_GetObjectItemCaseSensitive(c, "keycode");
                    if (!kc) kc = cJSON_GetObjectItemCaseSensitive(c, "IRCode");
                    if (kc && kc->valuestring) {
                        char clean[128] = {0};
                        convert_legacy_hid_code(kc->valuestring, clean, sizeof(clean));
                        cJSON_DeleteItemFromObject(c, "action");
                        cJSON_DeleteItemFromObject(c, "keycode");
                        cJSON_DeleteItemFromObject(c, "IRCode");
                        cJSON_AddStringToObject(c, "action", clean);
                    }
                    /* Strip raw IR data if mistakenly attached */
                    cJSON_DeleteItemFromObject(c, "raw");
                    cJSON_DeleteItemFromObject(c, "ir_raw");
                }
            }

            /* Add built-in Connect/Disconnect helper commands if missing on BT device */
            if (is_bt) {
                if (!has_connect) {
                    cJSON *ccmd = cJSON_CreateObject();
                    cJSON_AddStringToObject(ccmd, "name", "Connect");
                    cJSON_AddStringToObject(ccmd, "action", "BT:CONNECT");
                    cJSON_AddItemToArray(cmds, ccmd);
                }
                if (!has_disconnect) {
                    cJSON *dcmd = cJSON_CreateObject();
                    dcmd = cJSON_CreateObject();
                    cJSON_AddStringToObject(dcmd, "name", "Disconnect");
                    cJSON_AddStringToObject(dcmd, "action", "BT:DISCONNECT");
                    cJSON_AddItemToArray(cmds, dcmd);
                }
            }
        }
    }
}

/* Sanitize activity step arrays (PowerOn/PowerOff sequences, startSequence, stopSequence):
 * 1. Convert string delay_ms to integers
 * 2. Prune invalid steps without command or delay
 * 3. Normalize step types to 'DeviceCommand', 'Delay', 'BTCommand'
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
        const char *cmd = cmd_item ? cmd_item->valuestring : "";

        cJSON *dev_item = cJSON_GetObjectItemCaseSensitive(s, "deviceId");
        if (!dev_item) dev_item = cJSON_GetObjectItemCaseSensitive(s, "device_id");
        if (!dev_item) dev_item = cJSON_GetObjectItemCaseSensitive(s, "DeviceId");
        const char *dev_id = dev_item ? dev_item->valuestring : "";

        cJSON *delay_item = cJSON_GetObjectItemCaseSensitive(s, "delay_ms");
        if (!delay_item) delay_item = cJSON_GetObjectItemCaseSensitive(s, "delayMs");
        if (!delay_item) delay_item = cJSON_GetObjectItemCaseSensitive(s, "Delay");
        int delay_val = 0;
        if (delay_item) {
            if (cJSON_IsNumber(delay_item)) delay_val = delay_item->valueint;
            else if (cJSON_IsString(delay_item)) delay_val = atoi(delay_item->valuestring);
        }

        /* Check for delay step */
        if (strcasecmp(tstr, "Delay") == 0 || (delay_val > 0 && (!cmd || !cmd[0]))) {
            cJSON *clean_step = cJSON_CreateObject();
            cJSON_AddStringToObject(clean_step, "type", "Delay");
            cJSON_AddNumberToObject(clean_step, "delay_ms", delay_val > 0 ? delay_val : 100);
            cJSON_AddItemToArray(new_arr, clean_step);
            continue;
        }

        /* Check for BT or IR command step */
        if (cmd && cmd[0]) {
            cJSON *clean_step = cJSON_CreateObject();
            if (strcasecmp(tstr, "BTCommand") == 0 || strncmp(cmd, "HID:", 4) == 0 || strncmp(cmd, "BT:", 3) == 0) {
                cJSON_AddStringToObject(clean_step, "type", "BTCommand");
            } else {
                cJSON_AddStringToObject(clean_step, "type", "DeviceCommand");
            }
            cJSON_AddStringToObject(clean_step, "command", cmd);
            if (dev_id && dev_id[0]) {
                cJSON_AddStringToObject(clean_step, "deviceId", dev_id);
            }
            if (delay_val > 0) {
                cJSON_AddNumberToObject(clean_step, "delay_ms", delay_val);
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
