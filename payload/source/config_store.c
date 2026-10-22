#define _GNU_SOURCE
#include "config_store.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <strings.h>
#include <time.h>
#include "codex_bt_common.h"

extern int g_dry_run;

static cJSON *g_devices_json_cache = NULL;
static cJSON *g_activities_json_cache = NULL;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;

static char *read_file_text(const char *path, size_t max_bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || (size_t)sz > max_bytes) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';
    return buf;
}

static int write_file_atomic(const char *path, const char *content) {
    if (!path || !content) return -1;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return rename(tmp, path);
}

static void convert_legacy_hid_code(const char *in, char *out, size_t outsz) {
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
static void sanitize_devices_json(cJSON *root) {
    if (!root) return;
    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
    if (!devs || !cJSON_IsArray(devs)) return;

    cJSON *dev_item = NULL;
    cJSON_ArrayForEach(dev_item, devs) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
        if (!d) d = dev_item;

        /* Deduplicate MQTT settings */
        cJSON *mq_obj = cJSON_GetObjectItemCaseSensitive(d, "mqtt");
        cJSON *mq_en = cJSON_GetObjectItemCaseSensitive(d, "mqttEnabled");
        if (!mq_en) mq_en = cJSON_GetObjectItemCaseSensitive(d, "MQTTEnabled");
        cJSON *mq_top = cJSON_GetObjectItemCaseSensitive(d, "mqttTopic");
        if (!mq_top) mq_top = cJSON_GetObjectItemCaseSensitive(d, "MQTTTopic");
        cJSON *mq_pls = cJSON_GetObjectItemCaseSensitive(d, "mqttPulseMs");
        if (!mq_pls) mq_pls = cJSON_GetObjectItemCaseSensitive(d, "MQTTPulseMs");

        if (mq_obj && cJSON_IsObject(mq_obj)) {
            if (!mq_en) {
                cJSON *sub_en = cJSON_GetObjectItemCaseSensitive(mq_obj, "enabled");
                int val = (sub_en && (cJSON_IsTrue(sub_en) || (cJSON_IsNumber(sub_en) && sub_en->valueint > 0))) ? 1 : 0;
                cJSON_AddNumberToObject(d, "mqttEnabled", val);
            }
            if (!mq_top) {
                cJSON *sub_top = cJSON_GetObjectItemCaseSensitive(mq_obj, "topic");
                cJSON_AddStringToObject(d, "mqttTopic", (sub_top && sub_top->valuestring) ? sub_top->valuestring : "");
            }
            if (!mq_pls) {
                cJSON *sub_pls = cJSON_GetObjectItemCaseSensitive(mq_obj, "pulseMs");
                cJSON_AddNumberToObject(d, "mqttPulseMs", (sub_pls && cJSON_IsNumber(sub_pls)) ? sub_pls->valueint : 1000);
            }
            cJSON_DeleteItemFromObjectCaseSensitive(d, "mqtt");
        }

        /* Check device type */
        cJSON *dtype = cJSON_GetObjectItemCaseSensitive(d, "type");
        if (!dtype) dtype = cJSON_GetObjectItemCaseSensitive(d, "DeviceTypeDisplayName");
        const char *tstr = (dtype && dtype->valuestring) ? dtype->valuestring : "";
        cJSON *dmfg = cJSON_GetObjectItemCaseSensitive(d, "manufacturer");
        if (!dmfg) dmfg = cJSON_GetObjectItemCaseSensitive(d, "Manufacturer");
        const char *mstr = (dmfg && dmfg->valuestring) ? dmfg->valuestring : "";
        cJSON *dtr = cJSON_GetObjectItemCaseSensitive(d, "transport");
        if (!dtr) dtr = cJSON_GetObjectItemCaseSensitive(d, "Transport");
        int tr_val = (dtr && cJSON_IsNumber(dtr)) ? dtr->valueint : 1;

        int dev_is_bt = (tr_val == 32 || strcasecmp(tstr, "BT") == 0 || strcasestr(tstr, "bluetooth") != NULL || strcasestr(tstr, "bt") != NULL || strcasestr(mstr, "bluetooth") != NULL);
        int dev_is_mqtt = (strcasecmp(tstr, "MQTT") == 0 || strcasestr(tstr, "mqtt") != NULL || strcasestr(mstr, "mqtt") != NULL);

        /* Map type to strictly 'IR', 'BT', or 'MQTT' */
        const char *target_type = dev_is_bt ? "BT" : (dev_is_mqtt ? "MQTT" : "IR");
        if (!dtype) {
            cJSON_AddStringToObject(d, "type", target_type);
        } else if (strcmp(tstr, "IR") != 0 && strcmp(tstr, "BT") != 0 && strcmp(tstr, "MQTT") != 0) {
            cJSON_SetValuestring(dtype, target_type);
        }

        /* IR delays and power settings */
        if (!cJSON_GetObjectItemCaseSensitive(d, "interDeviceDelay")) {
            cJSON *old_idd = cJSON_GetObjectItemCaseSensitive(d, "InterDeviceDelay");
            cJSON_AddNumberToObject(d, "interDeviceDelay", old_idd ? old_idd->valueint : 500);
        }
        if (!cJSON_GetObjectItemCaseSensitive(d, "interKeyDelay")) {
            cJSON *old_ikd = cJSON_GetObjectItemCaseSensitive(d, "InterKeyDelay");
            cJSON_AddNumberToObject(d, "interKeyDelay", old_ikd ? old_ikd->valueint : 100);
        }
        if (!cJSON_GetObjectItemCaseSensitive(d, "holdInterKeyDelay")) {
            cJSON *old_hikd = cJSON_GetObjectItemCaseSensitive(d, "HoldInterKeyDelay");
            cJSON_AddNumberToObject(d, "holdInterKeyDelay", old_hikd ? old_hikd->valueint : 200);
        }
        if (!cJSON_GetObjectItemCaseSensitive(d, "defaultInterDeviceDelay")) {
            cJSON *old_didd = cJSON_GetObjectItemCaseSensitive(d, "DefaultInterDeviceDelay");
            cJSON_AddNumberToObject(d, "defaultInterDeviceDelay", old_didd ? old_didd->valueint : 500);
        }
        if (!cJSON_GetObjectItemCaseSensitive(d, "powerOnDelay")) {
            cJSON *old_pod = cJSON_GetObjectItemCaseSensitive(d, "PowerOnDelay");
            cJSON_AddNumberToObject(d, "powerOnDelay", old_pod ? old_pod->valueint : 1500);
        }
        cJSON *old_pao = cJSON_GetObjectItemCaseSensitive(d, "isPowerAlwaysOn");
        if (!old_pao) old_pao = cJSON_GetObjectItemCaseSensitive(d, "is_power_always_on");
        int pao_val = (old_pao && (cJSON_IsTrue(old_pao) || (cJSON_IsNumber(old_pao) && old_pao->valueint > 0))) ? 1 : 0;
        cJSON_DeleteItemFromObjectCaseSensitive(d, "isPowerAlwaysOn");
        cJSON_DeleteItemFromObjectCaseSensitive(d, "is_power_always_on");
        cJSON_AddNumberToObject(d, "isPowerAlwaysOn", pao_val);
        cJSON_AddBoolToObject(d, "is_power_always_on", pao_val);

        /* Per-device BT reconnect settings */
        if (!cJSON_GetObjectItemCaseSensitive(d, "btAutoReconnect")) {
            cJSON_AddNumberToObject(d, "btAutoReconnect", -1); /* -1 = inherit */
        }
        if (!cJSON_GetObjectItemCaseSensitive(d, "btReconnectDuration")) {
            cJSON_AddNumberToObject(d, "btReconnectDuration", 0); /* 0 = inherit */
        }
        if (!cJSON_GetObjectItemCaseSensitive(d, "btReconnectDelay")) {
            cJSON_AddNumberToObject(d, "btReconnectDelay", -1); /* -1 = inherit */
        }

        /* MQTT default topic */
        cJSON *mq_t = cJSON_GetObjectItemCaseSensitive(d, "mqttTopic");
        if (!mq_t || !mq_t->valuestring || !mq_t->valuestring[0]) {
            cJSON_DeleteItemFromObjectCaseSensitive(d, "mqttTopic");
            cJSON_AddStringToObject(d, "mqttTopic", "{base_topic}/button/{device}/{command}");
        }

        cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "commands");
        if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "Commands");
        if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(d, "commands");
        if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(d, "Commands");

        int has_connect = 0;
        int has_disconnect = 0;

        if (cmds && cJSON_IsArray(cmds)) {
            cJSON *c_item = NULL;
            cJSON_ArrayForEach(c_item, cmds) {
                cJSON *cname = cJSON_GetObjectItemCaseSensitive(c_item, "name");
                if (!cname) cname = cJSON_GetObjectItemCaseSensitive(c_item, "Name");
                const char *cnstr = (cname && cname->valuestring) ? cname->valuestring : "";
                if (strcasecmp(cnstr, "Connect") == 0) has_connect = 1;
                if (strcasecmp(cnstr, "Disconnect") == 0) has_disconnect = 1;

                cJSON *jkey = cJSON_GetObjectItemCaseSensitive(c_item, "keyCode");
                if (!jkey) jkey = cJSON_GetObjectItemCaseSensitive(c_item, "KeyCode");
                const char *kstr = (jkey && jkey->valuestring) ? jkey->valuestring : "";

                cJSON *jraw = cJSON_GetObjectItemCaseSensitive(c_item, "raw");
                if (!jraw) jraw = cJSON_GetObjectItemCaseSensitive(c_item, "Raw");
                const char *rstr = (jraw && jraw->valuestring) ? jraw->valuestring : "";

                /* Convert legacy G:HID codes */
                if (strncmp(kstr, "G:HID", 5) == 0) {
                    char clean_hid[64] = {0};
                    convert_legacy_hid_code(kstr, clean_hid, sizeof(clean_hid));
                    cJSON_DeleteItemFromObjectCaseSensitive(c_item, "keyCode");
                    cJSON_DeleteItemFromObjectCaseSensitive(c_item, "KeyCode");
                    cJSON_AddStringToObject(c_item, "keyCode", clean_hid);
                    kstr = clean_hid;
                }
                if (strncmp(rstr, "G:HID", 5) == 0) {
                    char clean_hid[64] = {0};
                    convert_legacy_hid_code(rstr, clean_hid, sizeof(clean_hid));
                    cJSON_DeleteItemFromObjectCaseSensitive(c_item, "raw");
                    cJSON_DeleteItemFromObjectCaseSensitive(c_item, "Raw");
                    cJSON_AddStringToObject(c_item, "raw", clean_hid);
                }

                int cmd_is_bt = dev_is_bt || (strncmp(kstr, "HID:", 4) == 0);
                int cmd_is_mqtt = dev_is_mqtt || (strncmp(kstr, "G:MQTT", 6) == 0);

                if (cmd_is_bt || cmd_is_mqtt) {
                    cJSON_DeleteItemFromObjectCaseSensitive(c_item, "port");
                    cJSON_DeleteItemFromObjectCaseSensitive(c_item, "Port");
                    cJSON_DeleteItemFromObjectCaseSensitive(c_item, "controlPort");
                    cJSON_DeleteItemFromObjectCaseSensitive(c_item, "ControlPort");
                    cJSON_DeleteItemFromObjectCaseSensitive(c_item, "repeats");
                    cJSON_DeleteItemFromObjectCaseSensitive(c_item, "Repeats");
                }
            }

            /* Add undeletable Connect & Disconnect buttons to BT devices */
            if (dev_is_bt) {
                if (!has_connect) {
                    cJSON *cc = cJSON_CreateObject();
                    cJSON_AddStringToObject(cc, "name", "Connect");
                    cJSON_AddStringToObject(cc, "keyCode", "CONNECT");
                    cJSON_AddStringToObject(cc, "raw", "CONNECT");
                    cJSON_AddItemToArray(cmds, cc);
                }
                if (!has_disconnect) {
                    cJSON *dc = cJSON_CreateObject();
                    cJSON_AddStringToObject(dc, "name", "Disconnect");
                    cJSON_AddStringToObject(dc, "keyCode", "DISCONNECT");
                    cJSON_AddStringToObject(dc, "raw", "DISCONNECT");
                    cJSON_AddItemToArray(cmds, dc);
                }
            }
        }
    }
}

/* Sanitize activities JSON:
 * 1. Rename 'IRCommand' -> 'Command'
 * 2. Split commands with wait/delay into a Command step + separate Delay step
 * 3. Support 'Switched' / 'ActivitySwitched'
 */
static void sanitize_activity_steps(cJSON *steps) {
    if (!steps || !cJSON_IsArray(steps)) return;

    cJSON *new_arr = cJSON_CreateArray();
    if (!new_arr) return;

    cJSON *s = NULL;
    cJSON_ArrayForEach(s, steps) {
        if (!cJSON_IsObject(s)) continue;

        cJSON *stype = cJSON_GetObjectItemCaseSensitive(s, "type");
        if (!stype) stype = cJSON_GetObjectItemCaseSensitive(s, "Type");
        cJSON *st_type = cJSON_GetObjectItemCaseSensitive(s, "__type");
        const char *raw_type = (stype && stype->valuestring) ? stype->valuestring :
                               ((st_type && strstr(st_type->valuestring, "Delay")) ? "Delay" : "Command");

        cJSON *scmd = cJSON_GetObjectItemCaseSensitive(s, "command");
        if (!scmd) scmd = cJSON_GetObjectItemCaseSensitive(s, "CommandName");
        if (!scmd) scmd = cJSON_GetObjectItemCaseSensitive(s, "Command");
        if (!scmd) scmd = cJSON_GetObjectItemCaseSensitive(s, "IRCommandName");
        char cmd_buf[128] = "";
        if (scmd && scmd->valuestring) strncpy(cmd_buf, scmd->valuestring, sizeof(cmd_buf) - 1);

        const char *tval = "Command";
        if (strcasecmp(raw_type, "Delay") == 0) tval = "Delay";
        else if (strcasecmp(raw_type, "Switched") == 0 || strcasecmp(raw_type, "ActivitySwitched") == 0 || strcasecmp(cmd_buf, "Switched") == 0) tval = "Switched";

        cJSON *sdid = cJSON_GetObjectItemCaseSensitive(s, "deviceId");
        if (!sdid) sdid = cJSON_GetObjectItemCaseSensitive(s, "DeviceId");
        if (!sdid) sdid = cJSON_GetObjectItemCaseSensitive(s, "DeviceId-");
        char did_buf[32] = "0";
        if (sdid && cJSON_IsString(sdid) && sdid->valuestring) strncpy(did_buf, sdid->valuestring, sizeof(did_buf) - 1);
        else if (sdid && cJSON_IsNumber(sdid)) snprintf(did_buf, sizeof(did_buf), "%ld", (long)sdid->valuedouble);

        cJSON *sdel = cJSON_GetObjectItemCaseSensitive(s, "delay");
        if (!sdel) sdel = cJSON_GetObjectItemCaseSensitive(s, "Delay");
        if (!sdel) sdel = cJSON_GetObjectItemCaseSensitive(s, "Duration");
        int del_val = (sdel && cJSON_IsNumber(sdel)) ? (int)sdel->valuedouble : 0;

        if (strcmp(tval, "Command") == 0) {
            cJSON *c_step = cJSON_CreateObject();
            cJSON_AddStringToObject(c_step, "type", "Command");
            cJSON_AddStringToObject(c_step, "deviceId", did_buf);
            cJSON_AddStringToObject(c_step, "command", cmd_buf);
            cJSON_AddNumberToObject(c_step, "delay", 0);
            cJSON_AddItemToArray(new_arr, c_step);

            if (del_val > 0) {
                cJSON *d_step = cJSON_CreateObject();
                cJSON_AddStringToObject(d_step, "type", "Delay");
                cJSON_AddStringToObject(d_step, "deviceId", "0");
                cJSON_AddStringToObject(d_step, "command", "");
                cJSON_AddNumberToObject(d_step, "delay", del_val);
                cJSON_AddItemToArray(new_arr, d_step);
            }
        } else if (strcmp(tval, "Delay") == 0) {
            cJSON *d_step = cJSON_CreateObject();
            cJSON_AddStringToObject(d_step, "type", "Delay");
            cJSON_AddStringToObject(d_step, "deviceId", "0");
            cJSON_AddStringToObject(d_step, "command", "");
            cJSON_AddNumberToObject(d_step, "delay", del_val > 0 ? del_val : 100);
            cJSON_AddItemToArray(new_arr, d_step);
        } else if (strcmp(tval, "Switched") == 0) {
            cJSON *sw_step = cJSON_CreateObject();
            cJSON_AddStringToObject(sw_step, "type", "Switched");
            cJSON_AddStringToObject(sw_step, "deviceId", "0");
            cJSON_AddStringToObject(sw_step, "command", "Switched");
            cJSON_AddNumberToObject(sw_step, "delay", 0);
            cJSON_AddItemToArray(new_arr, sw_step);
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

static void sanitize_activities_json(cJSON *root) {
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

/* Migrate stock DeviceList.json -> /data/codex/config/devices.json without touching original */
static void migrate_stock_devices(void) {
    if (access(CODEX_DEVICES_FILE, F_OK) == 0) return;

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
        write_file_atomic(CODEX_DEVICES_FILE, json_out);
        free(json_out);
    }
    cJSON_Delete(clean_root);
    printf("[+] Migrated stock DeviceList.json -> %s\n", CODEX_DEVICES_FILE);
}

/* Migrate stock ActivityList.json -> /data/codex/config/activities.json without touching original */
static void migrate_stock_activities(void) {
    if (access(CODEX_ACTIVITIES_FILE, F_OK) == 0) return;

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
        write_file_atomic(CODEX_ACTIVITIES_FILE, json_out);
        free(json_out);
    }
    cJSON_Delete(clean_root);
    printf("[+] Migrated stock ActivityList.json -> %s\n", CODEX_ACTIVITIES_FILE);
}

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

static void migrate_activity_ids_to_short(cJSON *root) {
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
        write_file_atomic(CODEX_ACTIVITIES_FILE, out);
        free(out);
    }

    /* Remap /data/codex/current_activity */
    FILE *cf = fopen("/data/codex/current_activity", "r");
    if (cf) {
        char cur_id[32] = {0};
        if (fscanf(cf, "%31s", cur_id) == 1) {
            fclose(cf);
            for (int i = 0; i < map_count; i++) {
                if (strcmp(cur_id, map[i].old_id) == 0) {
                    char new_act_buf[16];
                    snprintf(new_act_buf, sizeof(new_act_buf), "%s\n", map[i].new_id);
                    write_file_atomic("/data/codex/current_activity", new_act_buf);
                    sync();
                    break;
                }
            }
        } else {
            fclose(cf);
        }
    }

    /* Helper loop to remap JSON mapping files */
    const char *map_files[] = {
        "/data/codex/elite_remote_map.json",
        "/data/codex/bt_remote_map.json",
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

int config_store_init(void) {
    mkdir("/data/codex", 0755);
    mkdir(CODEX_CONFIG_DIR, 0755);

    /* Stock import on first boot only: skip if import flag already present */
    if (access(STOCK_IMPORT_FLAG, F_OK) != 0) {
        migrate_stock_devices();
        migrate_stock_activities();
        if (access(CODEX_DEVICES_FILE, F_OK) == 0 && access(CODEX_ACTIVITIES_FILE, F_OK) == 0) {
            FILE *flag = fopen(STOCK_IMPORT_FLAG, "w");
            if (flag) {
                fprintf(flag, "imported %ld\n", (long)time(NULL));
                fclose(flag);
            }
        }
    }

    pthread_mutex_lock(&g_cfg_lock);
    if (g_devices_json_cache) { cJSON_Delete(g_devices_json_cache); g_devices_json_cache = NULL; }
    if (g_activities_json_cache) { cJSON_Delete(g_activities_json_cache); g_activities_json_cache = NULL; }

    char *dev_raw = read_file_text(CODEX_DEVICES_FILE, 4 * 1024 * 1024);
    if (dev_raw) {
        g_devices_json_cache = cJSON_Parse(dev_raw);
        free(dev_raw);
        if (g_devices_json_cache) {
            sanitize_devices_json(g_devices_json_cache);
        }
    }
    if (!g_devices_json_cache) {
        g_devices_json_cache = cJSON_CreateObject();
        cJSON_AddArrayToObject(g_devices_json_cache, "devices");
    }

    char *act_raw = read_file_text(CODEX_ACTIVITIES_FILE, 2 * 1024 * 1024);
    if (act_raw) {
        g_activities_json_cache = cJSON_Parse(act_raw);
        free(act_raw);
        if (g_activities_json_cache) {
            sanitize_activities_json(g_activities_json_cache);
            migrate_activity_ids_to_short(g_activities_json_cache);
        }
    }
    if (!g_activities_json_cache) {
        g_activities_json_cache = cJSON_CreateObject();
        cJSON_AddArrayToObject(g_activities_json_cache, "activities");
    }
    pthread_mutex_unlock(&g_cfg_lock);
    return 0;
}


cJSON *config_store_get_devices_json(void) {
    pthread_mutex_lock(&g_cfg_lock);
    if (!g_devices_json_cache) {
        pthread_mutex_unlock(&g_cfg_lock);
        config_store_init();
        pthread_mutex_lock(&g_cfg_lock);
    }
    cJSON *res = g_devices_json_cache;
    pthread_mutex_unlock(&g_cfg_lock);
    return res;
}

cJSON *config_store_get_activities_json(void) {
    pthread_mutex_lock(&g_cfg_lock);
    if (!g_activities_json_cache) {
        pthread_mutex_unlock(&g_cfg_lock);
        config_store_init();
        pthread_mutex_lock(&g_cfg_lock);
    }
    cJSON *res = g_activities_json_cache;
    pthread_mutex_unlock(&g_cfg_lock);
    return res;
}

int config_store_save_devices(cJSON *devices_json) {
    if (!devices_json) return -1;
    sanitize_devices_json(devices_json);
    char *out = cJSON_Print(devices_json);
    if (!out) return -1;
    pthread_mutex_lock(&g_cfg_lock);
    write_file_atomic(CODEX_DEVICES_FILE, out);
    free(out);
    if (g_devices_json_cache && g_devices_json_cache != devices_json) {
        cJSON_Delete(g_devices_json_cache);
    }
    g_devices_json_cache = devices_json;
    pthread_mutex_unlock(&g_cfg_lock);
    notify_rf_push_config();
    return 0;
}

int config_store_save_activities(cJSON *activities_json) {
    if (!activities_json) return -1;
    sanitize_activities_json(activities_json);
    char *out = cJSON_Print(activities_json);
    if (!out) return -1;
    pthread_mutex_lock(&g_cfg_lock);
    write_file_atomic(CODEX_ACTIVITIES_FILE, out);
    free(out);
    if (g_activities_json_cache && g_activities_json_cache != activities_json) {
        cJSON_Delete(g_activities_json_cache);
    }
    g_activities_json_cache = activities_json;
    pthread_mutex_unlock(&g_cfg_lock);
    notify_rf_push_config();
    return 0;
}

void notify_rf_push_config(void) {
    FILE *f = fopen("/data/codex/rf_sync_pending.flag", "w");
    if (f) {
        fprintf(f, "%ld\n", (long)time(NULL));
        fclose(f);
    }
}

void config_store_invalidate_cache(void) {
    pthread_mutex_lock(&g_cfg_lock);
    if (g_devices_json_cache) { cJSON_Delete(g_devices_json_cache); g_devices_json_cache = NULL; }
    if (g_activities_json_cache) { cJSON_Delete(g_activities_json_cache); g_activities_json_cache = NULL; }
    pthread_mutex_unlock(&g_cfg_lock);
}

static void parse_steps(const cJSON *arr, const char *dev_id, struct activity_step **steps_out, int *count_out) {
    *steps_out = NULL;
    *count_out = 0;
    if (!arr || !cJSON_IsArray(arr)) return;
    int sz = cJSON_GetArraySize(arr);
    if (sz <= 0) return;
    struct activity_step *steps = (struct activity_step *)calloc((size_t)sz, sizeof(*steps));
    if (!steps) return;
    int cnt = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (dev_id && dev_id[0]) {
            strncpy(steps[cnt].device_id, dev_id, sizeof(steps[cnt].device_id) - 1);
        }
        const cJSON *order = cJSON_GetObjectItemCaseSensitive(item, "Order");
        if (order && cJSON_IsNumber(order)) steps[cnt].order = order->valueint;
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(item, "Command");
        if (cmd && cJSON_IsString(cmd) && cmd->valuestring) {
            strncpy(steps[cnt].command, cmd->valuestring, sizeof(steps[cnt].command) - 1);
        }
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "Type");
        if (type && cJSON_IsString(type) && type->valuestring) {
            strncpy(steps[cnt].type, type->valuestring, sizeof(steps[cnt].type) - 1);
        } else {
            strncpy(steps[cnt].type, "IRCommand", sizeof(steps[cnt].type) - 1);
        }
        const cJSON *del = cJSON_GetObjectItemCaseSensitive(item, "Delay");
        if (del && cJSON_IsNumber(del)) steps[cnt].delay_ms = del->valueint;
        cnt++;
    }
    *steps_out = steps;
    *count_out = cnt;
}

int load_ir_inventory(struct ir_inventory *inv) {
    if (!inv) return -1;
    memset(inv, 0, sizeof(*inv));

    cJSON *root = config_store_get_devices_json();
    if (!root) return -1;

    /* Check clean format: { "devices": [ ... ] } */
    cJSON *clean_devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (clean_devs && cJSON_IsArray(clean_devs)) {
        const cJSON *d = NULL;
        cJSON_ArrayForEach(d, clean_devs) {
            if (inv->device_count >= MAX_IR_DEVICES) break;
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id");
            long id = (jid && cJSON_IsNumber(jid)) ? (long)jid->valuedouble : 0;
            if (id <= 0 && jid && cJSON_IsString(jid) && jid->valuestring) id = atol(jid->valuestring);
            if (id <= 0) continue;

            struct ir_device *dev = &inv->devices[inv->device_count];
            snprintf(dev->id, sizeof(dev->id), "%ld", id);
            if (id > inv->max_device_id) inv->max_device_id = id;

            const cJSON *name = cJSON_GetObjectItemCaseSensitive(d, "name");
            if (name && cJSON_IsString(name) && name->valuestring) {
                strncpy(dev->name, name->valuestring, sizeof(dev->name) - 1);
            }
            const cJSON *mfg = cJSON_GetObjectItemCaseSensitive(d, "manufacturer");
            if (mfg && cJSON_IsString(mfg) && mfg->valuestring) {
                strncpy(dev->manufacturer, mfg->valuestring, sizeof(dev->manufacturer) - 1);
            }
            const cJSON *model = cJSON_GetObjectItemCaseSensitive(d, "model");
            if (model && cJSON_IsString(model) && model->valuestring) {
                strncpy(dev->model, model->valuestring, sizeof(dev->model) - 1);
            }
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(d, "type");
            if (type && cJSON_IsString(type) && type->valuestring) {
                strncpy(dev->type, type->valuestring, sizeof(dev->type) - 1);
            }

            const cJSON *cp = cJSON_GetObjectItemCaseSensitive(d, "controlPort");
            dev->control_port = (cp && cJSON_IsNumber(cp)) ? cp->valueint : 7;
            const cJSON *tr = cJSON_GetObjectItemCaseSensitive(d, "transport");
            dev->transport = (tr && cJSON_IsNumber(tr)) ? tr->valueint : 1;
            const cJSON *idd = cJSON_GetObjectItemCaseSensitive(d, "interDeviceDelay");
            dev->inter_device_delay = (idd && cJSON_IsNumber(idd)) ? idd->valueint : 500;
            const cJSON *ikd = cJSON_GetObjectItemCaseSensitive(d, "interKeyDelay");
            dev->inter_key_delay = (ikd && cJSON_IsNumber(ikd)) ? ikd->valueint : 100;
            const cJSON *hikd = cJSON_GetObjectItemCaseSensitive(d, "holdInterKeyDelay");
            dev->hold_inter_key_delay = (hikd && cJSON_IsNumber(hikd)) ? hikd->valueint : 200;
            const cJSON *didd = cJSON_GetObjectItemCaseSensitive(d, "defaultInterDeviceDelay");
            dev->default_inter_device_delay = (didd && cJSON_IsNumber(didd)) ? didd->valueint : 500;
            const cJSON *pod = cJSON_GetObjectItemCaseSensitive(d, "powerOnDelay");
            dev->power_on_delay = (pod && cJSON_IsNumber(pod)) ? pod->valueint : 1500;
            const cJSON *pao = cJSON_GetObjectItemCaseSensitive(d, "isPowerAlwaysOn");
            if (!pao) pao = cJSON_GetObjectItemCaseSensitive(d, "is_power_always_on");
            dev->is_power_always_on = (pao && (cJSON_IsTrue(pao) || (cJSON_IsNumber(pao) && pao->valueint > 0))) ? 1 : 0;

            const cJSON *bar = cJSON_GetObjectItemCaseSensitive(d, "bt_auto_reconnect");
            if (!bar) bar = cJSON_GetObjectItemCaseSensitive(d, "btAutoReconnect");
            if (bar) {
                if (cJSON_IsNumber(bar)) dev->bt_auto_reconnect = bar->valueint;
                else if (cJSON_IsBool(bar)) dev->bt_auto_reconnect = cJSON_IsTrue(bar) ? 1 : 0;
                else dev->bt_auto_reconnect = -1;
            } else {
                dev->bt_auto_reconnect = -1;
            }

            const cJSON *brdur = cJSON_GetObjectItemCaseSensitive(d, "bt_reconnect_duration_sec");
            if (!brdur) brdur = cJSON_GetObjectItemCaseSensitive(d, "btReconnectDurationSec");
            if (!brdur) brdur = cJSON_GetObjectItemCaseSensitive(d, "btReconnectDuration");
            dev->bt_reconnect_duration_sec = (brdur && cJSON_IsNumber(brdur)) ? brdur->valueint : 0;

            const cJSON *brdel = cJSON_GetObjectItemCaseSensitive(d, "bt_reconnect_delay_sec");
            if (!brdel) brdel = cJSON_GetObjectItemCaseSensitive(d, "btReconnectDelaySec");
            if (!brdel) brdel = cJSON_GetObjectItemCaseSensitive(d, "btReconnectDelay");
            dev->bt_reconnect_delay_sec = (brdel && cJSON_IsNumber(brdel)) ? brdel->valueint : -1;

            const cJSON *j_btd = cJSON_GetObjectItemCaseSensitive(d, "bt_trigger_device_id");
            if (!j_btd) j_btd = cJSON_GetObjectItemCaseSensitive(d, "btTriggerDeviceId");
            if (j_btd && cJSON_IsString(j_btd) && j_btd->valuestring) {
                strncpy(dev->bt_trigger_device_id, j_btd->valuestring, sizeof(dev->bt_trigger_device_id) - 1);
            }
            const cJSON *j_btc = cJSON_GetObjectItemCaseSensitive(d, "bt_trigger_command_name");
            if (!j_btc) j_btc = cJSON_GetObjectItemCaseSensitive(d, "btTriggerCommandName");
            if (!j_btc) j_btc = cJSON_GetObjectItemCaseSensitive(d, "bt_trigger_command");
            if (!j_btc) j_btc = cJSON_GetObjectItemCaseSensitive(d, "btTriggerCommand");
            if (j_btc && cJSON_IsString(j_btc) && j_btc->valuestring) {
                const char *colon = strchr(j_btc->valuestring, ':');
                if (colon && !dev->bt_trigger_device_id[0]) {
                    size_t did_len = (size_t)(colon - j_btc->valuestring);
                    if (did_len >= sizeof(dev->bt_trigger_device_id)) did_len = sizeof(dev->bt_trigger_device_id) - 1;
                    strncpy(dev->bt_trigger_device_id, j_btc->valuestring, did_len);
                    dev->bt_trigger_device_id[did_len] = '\0';
                    strncpy(dev->bt_trigger_command_name, colon + 1, sizeof(dev->bt_trigger_command_name) - 1);
                } else {
                    strncpy(dev->bt_trigger_command_name, j_btc->valuestring, sizeof(dev->bt_trigger_command_name) - 1);
                }
            }

            const cJSON *po = cJSON_GetObjectItemCaseSensitive(d, "powerOnSequence");
            if (!po) po = cJSON_GetObjectItemCaseSensitive(d, "PowerOnActions");
            if (po) parse_steps(po, dev->id, &dev->power_on_steps, &dev->power_on_count);

            const cJSON *pf = cJSON_GetObjectItemCaseSensitive(d, "powerOffSequence");
            if (!pf) pf = cJSON_GetObjectItemCaseSensitive(d, "PowerOffActions");
            if (pf) parse_steps(pf, dev->id, &dev->power_off_steps, &dev->power_off_count);

            const cJSON *mqe = cJSON_GetObjectItemCaseSensitive(d, "mqttEnabled");
            if (!mqe) mqe = cJSON_GetObjectItemCaseSensitive(d, "MQTTEnabled");
            const cJSON *mqt = cJSON_GetObjectItemCaseSensitive(d, "mqttTopic");
            if (!mqt) mqt = cJSON_GetObjectItemCaseSensitive(d, "MQTTTopic");
            const cJSON *mqp = cJSON_GetObjectItemCaseSensitive(d, "mqttPulseMs");
            if (!mqp) mqp = cJSON_GetObjectItemCaseSensitive(d, "MQTTPulseMs");
            if (mqe || mqt || mqp) {
                dev->mqtt_enabled = (mqe && (cJSON_IsTrue(mqe) || (cJSON_IsNumber(mqe) && mqe->valueint > 0))) ? 1 : 0;
                if (mqt && cJSON_IsString(mqt) && mqt->valuestring) strncpy(dev->mqtt_topic, mqt->valuestring, sizeof(dev->mqtt_topic) - 1);
                dev->mqtt_pulse_ms = (mqp && cJSON_IsNumber(mqp) && mqp->valueint > 0) ? mqp->valueint : 1000;
            } else {
                const cJSON *mq = cJSON_GetObjectItemCaseSensitive(d, "mqtt");
                if (mq && cJSON_IsObject(mq)) {
                    const cJSON *sub_en = cJSON_GetObjectItemCaseSensitive(mq, "enabled");
                    dev->mqtt_enabled = (sub_en && (cJSON_IsTrue(sub_en) || (cJSON_IsNumber(sub_en) && sub_en->valueint > 0))) ? 1 : 0;
                    const cJSON *sub_top = cJSON_GetObjectItemCaseSensitive(mq, "topic");
                    if (sub_top && cJSON_IsString(sub_top) && sub_top->valuestring) strncpy(dev->mqtt_topic, sub_top->valuestring, sizeof(dev->mqtt_topic) - 1);
                    const cJSON *sub_pls = cJSON_GetObjectItemCaseSensitive(mq, "pulseMs");
                    dev->mqtt_pulse_ms = (sub_pls && cJSON_IsNumber(sub_pls) && sub_pls->valueint > 0) ? sub_pls->valueint : 1000;
                }
            }

            const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(d, "commands");
            if (cmds && cJSON_IsArray(cmds)) {
                int csz = cJSON_GetArraySize(cmds);
                if (csz > 0) {
                    dev->commands = (struct ir_command *)calloc((size_t)csz, sizeof(struct ir_command));
                    if (dev->commands) {
                        dev->command_cap = csz;
                        const cJSON *c = NULL;
                        cJSON_ArrayForEach(c, cmds) {
                            if (dev->command_count >= dev->command_cap) break;
                            struct ir_command *cmd = &dev->commands[dev->command_count];
                            const cJSON *cname = cJSON_GetObjectItemCaseSensitive(c, "name");
                            if (cname && cJSON_IsString(cname) && cname->valuestring) {
                                strncpy(cmd->name, cname->valuestring, sizeof(cmd->name) - 1);
                            }
                            const cJSON *ckey = cJSON_GetObjectItemCaseSensitive(c, "keyCode");
                            if (ckey && cJSON_IsString(ckey) && ckey->valuestring) {
                                strncpy(cmd->keycode, ckey->valuestring, sizeof(cmd->keycode) - 1);
                            }
                            const cJSON *craw = cJSON_GetObjectItemCaseSensitive(c, "raw");
                            if (craw && cJSON_IsString(craw) && craw->valuestring) {
                                strncpy(cmd->raw, craw->valuestring, sizeof(cmd->raw) - 1);
                                cmd->has_raw = 1;
                            }
                            dev->command_count++;
                        }
                    }
                }
            }

            inv->device_count++;
        }
        return 0;
    }

    /* Fallback stock DevicesWithFeatures parser */
    cJSON *dwf = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
    if (dwf && cJSON_IsArray(dwf)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, dwf) {
            if (inv->device_count >= MAX_IR_DEVICES) break;
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(item, "Device");
            if (!d) continue;
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id");
            long id = (jid && cJSON_IsNumber(jid)) ? (long)jid->valuedouble : 0;
            if (id <= 0 && jid && cJSON_IsString(jid) && jid->valuestring) id = atol(jid->valuestring);
            if (id <= 0) continue;

            struct ir_device *dev = &inv->devices[inv->device_count];
            snprintf(dev->id, sizeof(dev->id), "%ld", id);
            if (id > inv->max_device_id) inv->max_device_id = id;

            const cJSON *name = cJSON_GetObjectItemCaseSensitive(d, "Name");
            if (name && cJSON_IsString(name) && name->valuestring) {
                strncpy(dev->name, name->valuestring, sizeof(dev->name) - 1);
            }
            const cJSON *mfg = cJSON_GetObjectItemCaseSensitive(d, "Manufacturer");
            if (mfg && cJSON_IsString(mfg) && mfg->valuestring) {
                strncpy(dev->manufacturer, mfg->valuestring, sizeof(dev->manufacturer) - 1);
            }
            const cJSON *model = cJSON_GetObjectItemCaseSensitive(d, "Model");
            if (model && cJSON_IsString(model) && model->valuestring) {
                strncpy(dev->model, model->valuestring, sizeof(dev->model) - 1);
            }
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(d, "DeviceTypeDisplayName");
            if (type && cJSON_IsString(type) && type->valuestring) {
                strncpy(dev->type, type->valuestring, sizeof(dev->type) - 1);
            }

            const cJSON *cp = cJSON_GetObjectItemCaseSensitive(d, "ControlPort");
            dev->control_port = (cp && cJSON_IsNumber(cp)) ? cp->valueint : 7;
            const cJSON *tr = cJSON_GetObjectItemCaseSensitive(d, "Transport");
            dev->transport = (tr && cJSON_IsNumber(tr)) ? tr->valueint : 1;
            const cJSON *idd = cJSON_GetObjectItemCaseSensitive(d, "InterDeviceDelay");
            dev->inter_device_delay = (idd && cJSON_IsNumber(idd)) ? idd->valueint : 100;

            const cJSON *mqe = cJSON_GetObjectItemCaseSensitive(d, "MQTTEnabled");
            if (!mqe) mqe = cJSON_GetObjectItemCaseSensitive(d, "mqtt_enabled");
            if (mqe) {
                if (cJSON_IsTrue(mqe)) dev->mqtt_enabled = 1;
                else if (cJSON_IsNumber(mqe)) dev->mqtt_enabled = (int)mqe->valuedouble;
                else if (cJSON_IsString(mqe)) dev->mqtt_enabled = atoi(mqe->valuestring);
            }
            const cJSON *mqt = cJSON_GetObjectItemCaseSensitive(d, "MQTTTopic");
            if (!mqt) mqt = cJSON_GetObjectItemCaseSensitive(d, "mqtt_topic");
            if (mqt && cJSON_IsString(mqt) && mqt->valuestring) {
                strncpy(dev->mqtt_topic, mqt->valuestring, sizeof(dev->mqtt_topic) - 1);
            }
            const cJSON *mqp = cJSON_GetObjectItemCaseSensitive(d, "MQTTPulseMs");
            if (!mqp) mqp = cJSON_GetObjectItemCaseSensitive(d, "mqtt_pulse_ms");
            if (mqp && cJSON_IsNumber(mqp) && mqp->valueint > 0) {
                dev->mqtt_pulse_ms = mqp->valueint;
            }

            const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(item, "Commands");
            if (cmds && cJSON_IsArray(cmds)) {
                int csz = cJSON_GetArraySize(cmds);
                if (csz > 0) {
                    dev->commands = (struct ir_command *)calloc((size_t)csz, sizeof(struct ir_command));
                    if (dev->commands) {
                        dev->command_cap = csz;
                        const cJSON *c = NULL;
                        cJSON_ArrayForEach(c, cmds) {
                            if (dev->command_count >= dev->command_cap) break;
                            struct ir_command *cmd = &dev->commands[dev->command_count];
                            const cJSON *cname = cJSON_GetObjectItemCaseSensitive(c, "Name");
                            if (cname && cJSON_IsString(cname) && cname->valuestring) {
                                strncpy(cmd->name, cname->valuestring, sizeof(cmd->name) - 1);
                            }
                            const cJSON *ckey = cJSON_GetObjectItemCaseSensitive(c, "KeyCode");
                            if (ckey && cJSON_IsString(ckey) && ckey->valuestring) {
                                strncpy(cmd->keycode, ckey->valuestring, sizeof(cmd->keycode) - 1);
                            }
                            dev->command_count++;
                        }
                    }
                }
            }

            const cJSON *po = cJSON_GetObjectItemCaseSensitive(item, "PowerOnActions");
            if (po) parse_steps(po, dev->id, &dev->power_on_steps, &dev->power_on_count);
            const cJSON *pf = cJSON_GetObjectItemCaseSensitive(item, "PowerOffActions");
            if (pf) parse_steps(pf, dev->id, &dev->power_off_steps, &dev->power_off_count);

            inv->device_count++;
        }
    }
    return 0;
}

void free_ir_inventory(struct ir_inventory *inv) {
    if (!inv) return;
    for (int i = 0; i < inv->device_count; i++) {
        if (inv->devices[i].commands) free(inv->devices[i].commands);
        if (inv->devices[i].power_on_steps) free(inv->devices[i].power_on_steps);
        if (inv->devices[i].power_off_steps) free(inv->devices[i].power_off_steps);
    }
    memset(inv, 0, sizeof(*inv));
}

static void parse_activity_steps_array(const cJSON *arr, struct activity_step *steps, int max_steps, int *count_out) {
    *count_out = 0;
    if (!arr || !cJSON_IsArray(arr) || !steps || max_steps <= 0) return;
    int cnt = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (cnt >= max_steps) break;
        struct activity_step *st = &steps[cnt];
        memset(st, 0, sizeof(*st));

        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "DeviceId-");
        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "DeviceId");
        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "deviceId");
        if (jid) {
            if (cJSON_IsNumber(jid)) snprintf(st->device_id, sizeof(st->device_id), "%ld", (long)jid->valuedouble);
            else if (cJSON_IsString(jid) && jid->valuestring) strncpy(st->device_id, jid->valuestring, sizeof(st->device_id) - 1);
        }

        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(item, "CommandName");
        if (!cmd) cmd = cJSON_GetObjectItemCaseSensitive(item, "IRCommandName");
        if (!cmd) cmd = cJSON_GetObjectItemCaseSensitive(item, "Command");
        if (!cmd) cmd = cJSON_GetObjectItemCaseSensitive(item, "command");
        if (cmd && cJSON_IsString(cmd) && cmd->valuestring) {
            strncpy(st->command, cmd->valuestring, sizeof(st->command) - 1);
        }

        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "Type");
        if (!type) type = cJSON_GetObjectItemCaseSensitive(item, "type");
        const cJSON *ttype = cJSON_GetObjectItemCaseSensitive(item, "__type");
        if (type && cJSON_IsString(type) && type->valuestring) {
            strncpy(st->type, type->valuestring, sizeof(st->type) - 1);
        } else if (ttype && cJSON_IsString(ttype) && ttype->valuestring) {
            if (strstr(ttype->valuestring, "Delay")) strncpy(st->type, "Delay", sizeof(st->type) - 1);
            else if (strstr(ttype->valuestring, "Switched")) strncpy(st->type, "Switched", sizeof(st->type) - 1);
            else strncpy(st->type, "IRCommand", sizeof(st->type) - 1);
        } else {
            strncpy(st->type, "IRCommand", sizeof(st->type) - 1);
        }
        if (strcasecmp(st->type, "Switched") == 0 || strcasecmp(st->type, "ActivitySwitched") == 0 || strcasecmp(st->command, "Switched") == 0) {
            strncpy(st->type, "Switched", sizeof(st->type) - 1);
            if (!st->command[0]) strncpy(st->command, "Switched", sizeof(st->command) - 1);
        }

        const cJSON *del = cJSON_GetObjectItemCaseSensitive(item, "Duration");
        if (!del) del = cJSON_GetObjectItemCaseSensitive(item, "Delay");
        if (!del) del = cJSON_GetObjectItemCaseSensitive(item, "delay");
        if (del && cJSON_IsNumber(del)) st->delay_ms = (int)del->valuedouble;

        const cJSON *order = cJSON_GetObjectItemCaseSensitive(item, "Order");
        if (!order) order = cJSON_GetObjectItemCaseSensitive(item, "ActionOrder");
        if (order && cJSON_IsNumber(order)) st->order = (int)order->valuedouble;

        cnt++;
    }
    *count_out = cnt;
}

int load_activity_inventory(struct activity_inventory *inv) {
    if (!inv) return -1;
    memset(inv, 0, sizeof(*inv));
    strncpy(inv->current_id, "-1", sizeof(inv->current_id) - 1);
    strncpy(inv->current_name, "PowerOff", sizeof(inv->current_name) - 1);

    FILE *cf = fopen("/data/codex/current_activity", "r");
    if (cf) {
        if (fscanf(cf, "%31s", inv->current_id) != 1) strncpy(inv->current_id, "-1", sizeof(inv->current_id) - 1);
        fclose(cf);
    }

    cJSON *root = config_store_get_activities_json();
    if (!root) return -1;

    cJSON *arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "activities");
    if (!arr) arr = cJSON_GetObjectItemCaseSensitive(root, "Activities");
    if (arr && cJSON_IsArray(arr)) {
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, arr) {
            if (inv->count >= MAX_ACTIVITIES) break;
            const cJSON *a = entry;
            const cJSON *inner = cJSON_GetObjectItemCaseSensitive(entry, "Activity");
            if (inner && cJSON_IsObject(inner)) a = inner;

            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(a, "id");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(a, "Id-");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(a, "Id");
            long id = (jid && cJSON_IsNumber(jid)) ? (long)jid->valuedouble : 0;
            if (id == 0 && jid && cJSON_IsString(jid) && jid->valuestring) id = atol(jid->valuestring);
            if (id == 0) continue;

            struct activity_item *act = &inv->items[inv->count];
            snprintf(act->id, sizeof(act->id), "%ld", id);
            if (id > 0 && id > inv->max_id) inv->max_id = id;

            const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
            if (!name) name = cJSON_GetObjectItemCaseSensitive(a, "Name");
            if (name && cJSON_IsString(name) && name->valuestring) {
                strncpy(act->name, name->valuestring, sizeof(act->name) - 1);
            } else if (id == -1) {
                strncpy(act->name, "PowerOff", sizeof(act->name) - 1);
            }
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(a, "type");
            if (!type) type = cJSON_GetObjectItemCaseSensitive(a, "Type");
            if (!type) type = cJSON_GetObjectItemCaseSensitive(a, "SuggestedDisplay");
            if (type && cJSON_IsString(type) && type->valuestring) {
                strncpy(act->type, type->valuestring, sizeof(act->type) - 1);
            }

            const cJSON *devs = cJSON_GetObjectItemCaseSensitive(a, "deviceIds");
            if (!devs) devs = cJSON_GetObjectItemCaseSensitive(a, "Devices");
            if (devs && cJSON_IsArray(devs)) {
                const cJSON *d = NULL;
                cJSON_ArrayForEach(d, devs) {
                    if (act->device_count >= 16) break;
                    if (cJSON_IsString(d) && d->valuestring) {
                        strncpy(act->device_ids[act->device_count++], d->valuestring, 31);
                    } else if (cJSON_IsNumber(d)) {
                        snprintf(act->device_ids[act->device_count++], 31, "%ld", (long)d->valuedouble);
                    }
                }
            }

            /* Extract devices from Roles */
            const cJSON *roles = cJSON_GetObjectItemCaseSensitive(a, "Roles");
            if (roles && cJSON_IsArray(roles)) {
                const cJSON *r = NULL;
                cJSON_ArrayForEach(r, roles) {
                    if (act->device_count >= 16) break;
                    const cJSON *rdid = cJSON_GetObjectItemCaseSensitive(r, "DeviceId-");
                    if (!rdid) rdid = cJSON_GetObjectItemCaseSensitive(r, "DeviceId");
                    if (!rdid) rdid = cJSON_GetObjectItemCaseSensitive(r, "deviceId");
                    char dstr[32] = {0};
                    if (rdid && cJSON_IsNumber(rdid)) snprintf(dstr, sizeof(dstr), "%ld", (long)rdid->valuedouble);
                    else if (rdid && cJSON_IsString(rdid) && rdid->valuestring) strncpy(dstr, rdid->valuestring, sizeof(dstr) - 1);
                    if (dstr[0] && strcmp(dstr, "0") != 0) {
                        int dup = 0;
                        for (int di = 0; di < act->device_count; di++) {
                            if (strcmp(act->device_ids[di], dstr) == 0) { dup = 1; break; }
                        }
                        if (!dup) strncpy(act->device_ids[act->device_count++], dstr, 31);
                    }
                }
            }

            /* Parse StartSequence or EnterActions */
            const cJSON *start_arr = cJSON_GetObjectItemCaseSensitive(a, "startSequence");
            if (!start_arr) start_arr = cJSON_GetObjectItemCaseSensitive(a, "StartSequence");
            if (!start_arr || !cJSON_IsArray(start_arr) || cJSON_GetArraySize(start_arr) == 0) {
                start_arr = cJSON_GetObjectItemCaseSensitive(a, "EnterActions");
            }
            parse_activity_steps_array(start_arr, act->start_steps, MAX_ACTIVITY_STEPS, &act->start_count);

            /* Parse StopSequence or LeaveActions */
            const cJSON *stop_arr = cJSON_GetObjectItemCaseSensitive(a, "stopSequence");
            if (!stop_arr) stop_arr = cJSON_GetObjectItemCaseSensitive(a, "StopSequence");
            if (!stop_arr || !cJSON_IsArray(stop_arr) || cJSON_GetArraySize(stop_arr) == 0) {
                stop_arr = cJSON_GetObjectItemCaseSensitive(a, "LeaveActions");
            }
            parse_activity_steps_array(stop_arr, act->stop_steps, MAX_ACTIVITY_STEPS, &act->stop_count);

            /* Extract any devices present in actions if not yet recorded */
            for (int s = 0; s < act->start_count && act->device_count < 16; s++) {
                if (act->start_steps[s].device_id[0] && strcmp(act->start_steps[s].device_id, "0") != 0) {
                    int dup = 0;
                    for (int di = 0; di < act->device_count; di++) {
                        if (strcmp(act->device_ids[di], act->start_steps[s].device_id) == 0) { dup = 1; break; }
                    }
                    if (!dup) strncpy(act->device_ids[act->device_count++], act->start_steps[s].device_id, 31);
                }
            }

            if (strcmp(act->id, inv->current_id) == 0 && act->name[0]) {
                strncpy(inv->current_name, act->name, sizeof(inv->current_name) - 1);
            }

            inv->count++;
        }
    }
    return 0;
}

int run_bt_saved_script(const char *type, const char *bdaddr, const char *script,
                        int gap_ms, char *out, size_t outlen) {
    (void)type;
    if (out && outlen) out[0] = '\0';
    if (!script || !script[0]) return -1;

    if (g_dry_run) {
        printf("[DRY-RUN] BT Send simulated: type=%s bdaddr=%s script=%s\n", type ? type : "", bdaddr ? bdaddr : "", script);
        if (out && outlen) snprintf(out, outlen, "[DRY-RUN] Sent key %s", script);
        return 0;
    }

    int fd = open(BT_TEXT_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        /* User rule: Still always try to reconnect when a BT button is sent and not connected */
        if (bdaddr && bdaddr[0]) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "connect_host %s\n", bdaddr);
            codex_send_btstack_cmd(cmd);
        } else {
            codex_send_btstack_cmd("connect_host\n");
        }
        if (out && outlen) snprintf(out, outlen, "Bluetooth host not connected; initiating connection");
        return -1;
    }

    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "KEY %s\n", script);
    write(fd, buf, (size_t)len);
    close(fd);

    if (gap_ms > 0) usleep((useconds_t)gap_ms * 1000);
    if (out && outlen) snprintf(out, outlen, "Sent key %s", script);
    return 0;
}

int save_bthid_target(const char *type, const char *bdaddr) {
    if (!bdaddr || !bdaddr[0]) return -1;
    FILE *f = fopen("/data/codex/bthid_target", "w");
    if (!f) return -1;
    fprintf(f, "%s %s\n", (type && type[0]) ? type : "btkeyboard", bdaddr);
    fclose(f);
    return 0;
}

int load_bt_inventory(struct bt_inventory *inv) {
    if (!inv) return -1;
    memset(inv, 0, sizeof(*inv));
    char *raw = read_file_text("/data/codex/bt-devices.json", 1024 * 1024);
    if (!raw) return 0;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return 0;

    cJSON *devs = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (devs && cJSON_IsArray(devs)) {
        const cJSON *d = NULL;
        cJSON_ArrayForEach(d, devs) {
            if (inv->device_count >= 12) break;
            struct bt_saved_device *bdev = &inv->devices[inv->device_count];
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
            if (jid && cJSON_IsString(jid) && jid->valuestring) {
                strncpy(bdev->id, jid->valuestring, sizeof(bdev->id) - 1);
            }
            const cJSON *jname = cJSON_GetObjectItemCaseSensitive(d, "name");
            if (jname && cJSON_IsString(jname) && jname->valuestring) {
                strncpy(bdev->name, jname->valuestring, sizeof(bdev->name) - 1);
            }
            const cJSON *jaddr = cJSON_GetObjectItemCaseSensitive(d, "bdaddr");
            if (!jaddr) jaddr = cJSON_GetObjectItemCaseSensitive(d, "address");
            if (jaddr && cJSON_IsString(jaddr) && jaddr->valuestring) {
                strncpy(bdev->bdaddr, jaddr->valuestring, sizeof(bdev->bdaddr) - 1);
            }
            const cJSON *jtype = cJSON_GetObjectItemCaseSensitive(d, "type");
            if (jtype && cJSON_IsString(jtype) && jtype->valuestring) {
                strncpy(bdev->type, jtype->valuestring, sizeof(bdev->type) - 1);
            }
            inv->device_count++;
        }
    }
    cJSON_Delete(root);
    return 0;
}

int config_store_get_settings(struct codex_settings *settings) {
    if (!settings) return -1;
    settings->bt_auto_reconnect = 1;
    settings->bt_reconnect_on_boot = 1;
    settings->bt_reconnect_duration_sec = 30;
    settings->bt_reconnect_delay_sec = 2;
    settings->dry_run = 0;
    strncpy(settings->hub_name, "Harmony Hub", sizeof(settings->hub_name) - 1);
    settings->hub_name[sizeof(settings->hub_name) - 1] = '\0';
    settings->ntp_server[0] = '\0';
    strncpy(settings->timezone, "UTC", sizeof(settings->timezone) - 1);
    strncpy(settings->led_mode, "always_on", sizeof(settings->led_mode) - 1);

    char *raw = read_file_text(CODEX_SETTINGS_FILE, 64 * 1024);
    if (!raw) return 0;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return 0;

    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "bt_auto_reconnect");
    if (item && cJSON_IsBool(item)) settings->bt_auto_reconnect = cJSON_IsTrue(item) ? 1 : 0;
    else if (item && cJSON_IsNumber(item)) settings->bt_auto_reconnect = (int)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(root, "bt_reconnect_on_boot");
    if (item && cJSON_IsBool(item)) settings->bt_reconnect_on_boot = cJSON_IsTrue(item) ? 1 : 0;
    else if (item && cJSON_IsNumber(item)) settings->bt_reconnect_on_boot = (int)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(root, "bt_reconnect_duration_sec");
    if (item && cJSON_IsNumber(item)) settings->bt_reconnect_duration_sec = (int)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(root, "bt_reconnect_delay_sec");
    if (item && cJSON_IsNumber(item)) settings->bt_reconnect_delay_sec = (int)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(root, "dry_run");
    if (item && cJSON_IsBool(item)) settings->dry_run = cJSON_IsTrue(item) ? 1 : 0;
    else if (item && cJSON_IsNumber(item)) settings->dry_run = (int)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(root, "hub_name");
    if (item && cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        strncpy(settings->hub_name, item->valuestring, sizeof(settings->hub_name) - 1);
        settings->hub_name[sizeof(settings->hub_name) - 1] = '\0';
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "ntp_server");
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(settings->ntp_server, item->valuestring, sizeof(settings->ntp_server) - 1);
        settings->ntp_server[sizeof(settings->ntp_server) - 1] = '\0';
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(settings->timezone, item->valuestring, sizeof(settings->timezone) - 1);
        settings->timezone[sizeof(settings->timezone) - 1] = '\0';
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "led_mode");
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(settings->led_mode, item->valuestring, sizeof(settings->led_mode) - 1);
        settings->led_mode[sizeof(settings->led_mode) - 1] = '\0';
    }

    cJSON_Delete(root);
    return 0;
}

int config_store_save_settings(const struct codex_settings *settings) {
    if (!settings) return -1;
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddBoolToObject(root, "bt_auto_reconnect", settings->bt_auto_reconnect ? 1 : 0);
    cJSON_AddBoolToObject(root, "bt_reconnect_on_boot", settings->bt_reconnect_on_boot ? 1 : 0);
    cJSON_AddNumberToObject(root, "bt_reconnect_duration_sec", settings->bt_reconnect_duration_sec);
    cJSON_AddNumberToObject(root, "bt_reconnect_delay_sec", settings->bt_reconnect_delay_sec);
    cJSON_AddBoolToObject(root, "dry_run", settings->dry_run ? 1 : 0);
    cJSON_AddStringToObject(root, "hub_name", settings->hub_name[0] ? settings->hub_name : "Harmony Hub");
    cJSON_AddStringToObject(root, "ntp_server", settings->ntp_server);
    cJSON_AddStringToObject(root, "timezone", settings->timezone[0] ? settings->timezone : "UTC");
    cJSON_AddStringToObject(root, "led_mode", settings->led_mode[0] ? settings->led_mode : "always_on");

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) return -1;

    int rc = write_file_atomic(CODEX_SETTINGS_FILE, out);
    free(out);
    return rc;
}

