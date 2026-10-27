#define _GNU_SOURCE
#include "hub_mqtt_ha.h"
#include "config_store.h"
#include "mqtt_client.h"
#include "harmony_version.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>

void safe_id(const char *in, char *out, size_t outlen) {
    if (!in || !out || outlen == 0) return;
    size_t i = 0;
    while (*in && i < outlen - 1) {
        char c = *in++;
        if (isalnum((unsigned char)c) || c == '-' || c == '_') {
            out[i++] = (char)tolower((unsigned char)c);
        } else {
            out[i++] = '_';
        }
    }
    out[i] = '\0';
}

int load_daemon_activities(struct daemon_activity_list *list) {
    if (!list) return -1;
    memset(list, 0, sizeof(*list));

    const char *path = HARMONY_ACTIVITIES_FILE;
    char *raw = read_file_text(path, 1024 * 1024);
    if (!raw) return -1;

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return -1;

    cJSON *acts = cJSON_GetObjectItemCaseSensitive(root, "activities");
    if (!acts) acts = cJSON_GetObjectItemCaseSensitive(root, "Activities");
    if (!acts || !cJSON_IsArray(acts)) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, acts) {
        if (list->count >= 32) break;

        cJSON *act_obj = item;
        cJSON *inner = cJSON_GetObjectItemCaseSensitive(item, "Activity");
        if (inner && cJSON_IsObject(inner)) act_obj = inner;

        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(act_obj, "Id-");
        if (!j_id) j_id = cJSON_GetObjectItemCaseSensitive(act_obj, "Id");
        if (!j_id) j_id = cJSON_GetObjectItemCaseSensitive(act_obj, "id");

        char act_id[32] = {0};
        if (j_id) {
            if (cJSON_IsNumber(j_id)) snprintf(act_id, sizeof(act_id), "%ld", (long)j_id->valuedouble);
            else if (cJSON_IsString(j_id) && j_id->valuestring) strncpy(act_id, j_id->valuestring, sizeof(act_id) - 1);
        }

        cJSON *j_name = cJSON_GetObjectItemCaseSensitive(act_obj, "name");
        if (!j_name) j_name = cJSON_GetObjectItemCaseSensitive(act_obj, "Name");
        const char *act_name = (j_name && cJSON_IsString(j_name) && j_name->valuestring) ? j_name->valuestring : "";

        int act_order = 999;
        cJSON *j_order = cJSON_GetObjectItemCaseSensitive(act_obj, "ActivityOrder");
        if (!j_order) j_order = cJSON_GetObjectItemCaseSensitive(act_obj, "Order");
        if (j_order && cJSON_IsNumber(j_order)) act_order = (int)j_order->valuedouble;

        if (act_id[0] && act_name[0] && strcmp(act_name, "PowerOff") != 0 && strcmp(act_id, "-1") != 0) {
            strncpy(list->items[list->count].id, act_id, sizeof(list->items[0].id) - 1);
            strncpy(list->items[list->count].name, act_name, sizeof(list->items[0].name) - 1);
            list->items[list->count].order = act_order;
            list->count++;
        }
    }

    cJSON_Delete(root);

    for (int i = 0; i < list->count - 1; i++) {
        for (int j = i + 1; j < list->count; j++) {
            if (list->items[j].order < list->items[i].order) {
                struct daemon_activity tmp = list->items[i];
                list->items[i] = list->items[j];
                list->items[j] = tmp;
            }
        }
    }

    return 0;
}

void get_local_ip(const char *mqtt_host, char *buf, size_t buflen) {
    if (!buf || buflen == 0) return;
    buf[0] = '\0';

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct sockaddr_in target;
        memset(&target, 0, sizeof(target));
        target.sin_family = AF_INET;
        target.sin_port = htons(80);
        if (mqtt_host && mqtt_host[0] && inet_pton(AF_INET, mqtt_host, &target.sin_addr) > 0) {
            /* target set to mqtt host */
        } else {
            inet_pton(AF_INET, "1.1.1.1", &target.sin_addr);
        }
        if (connect(s, (struct sockaddr *)&target, sizeof(target)) == 0) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            if (getsockname(s, (struct sockaddr *)&local, &len) == 0) {
                if (inet_ntop(AF_INET, &local.sin_addr, buf, buflen)) {
                    if (buf[0] && strcmp(buf, "0.0.0.0") != 0 && strcmp(buf, "127.0.0.1") != 0) {
                        close(s);
                        return;
                    }
                }
            }
        }
        close(s);
    }

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        const char *ifaces[] = {"ath0", "eth1", "eth0", "wlan0", "br-lan", NULL};
        for (int i = 0; ifaces[i]; i++) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifaces[i], IFNAMSIZ - 1);
            if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
                struct sockaddr_in *sa = (struct sockaddr_in *)&ifr.ifr_addr;
                if (inet_ntop(AF_INET, &sa->sin_addr, buf, buflen)) {
                    if (buf[0] && strcmp(buf, "0.0.0.0") != 0 && strcmp(buf, "127.0.0.1") != 0) {
                        close(s);
                        return;
                    }
                }
            }
        }
        close(s);
    }
}

void get_device_and_command_counts(int *dev_count, int *cmd_count) {
    if (dev_count) *dev_count = 0;
    if (cmd_count) *cmd_count = 0;

    const char *path = HARMONY_DEVICES_FILE;
    char *buf = read_file_text(path, 2 * 1024 * 1024);
    if (!buf) return;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (devs && cJSON_IsArray(devs)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, devs) {
            if (dev_count) (*dev_count)++;
            cJSON *cmds = cJSON_GetObjectItemCaseSensitive(item, "commands");
            if (cmds && cJSON_IsArray(cmds) && cmd_count) {
                *cmd_count += cJSON_GetArraySize(cmds);
            }
        }
    }
    cJSON_Delete(root);
}

static cJSON *create_ha_origin_obj(const char *ip) {
    cJSON *orig = cJSON_CreateObject();
    if (!orig) return NULL;
    cJSON_AddStringToObject(orig, "name", "HarmonyNG");
    cJSON_AddStringToObject(orig, "sw_version", HARMONY_VERSION);
    char sup[96];
    if (ip && ip[0]) {
        snprintf(sup, sizeof(sup), "http://%s:8089/", ip);
    } else {
        strncpy(sup, "http://harmony-hub.local:8089/", sizeof(sup) - 1);
        sup[sizeof(sup) - 1] = '\0';
    }
    cJSON_AddStringToObject(orig, "support_url", sup);
    return orig;
}

static cJSON *create_ha_device_obj(const char *ident, const char *device_name) {
    cJSON *dev = cJSON_CreateObject();
    if (!dev) return NULL;
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(ident));
    cJSON_AddItemToObject(dev, "identifiers", ids);
    cJSON_AddStringToObject(dev, "name", device_name && device_name[0] ? device_name : "Harmony Hub");
    cJSON_AddStringToObject(dev, "manufacturer", "Logitech");
    cJSON_AddStringToObject(dev, "model", "Harmony Hub");
    return dev;
}

void publish_ha_discovery(const char *cid, const char *name, const char *base_topic,
                          const char *discovery_prefix, const char *state_topic,
                          const char *status_topic, const char *mqtt_host,
                          int ha_discovery_enabled) {
    if (!mqtt_is_connected() || !ha_discovery_enabled) return;

    char ident[64];
    safe_id(cid, ident, sizeof(ident));

    struct daemon_activity_list act_list;
    load_daemon_activities(&act_list);

    char top[256];
    char uid[128];
    char cmd_top[160];
    snprintf(cmd_top, sizeof(cmd_top), "%s/activity/set", base_topic);

    /* 1. select/<ident>_activity/config */
    snprintf(top, sizeof(top), "%s/select/%s_activity/config", discovery_prefix, ident);
    cJSON *c1 = cJSON_CreateObject();
    if (c1) {
        cJSON_AddStringToObject(c1, "name", "Activity");
        snprintf(uid, sizeof(uid), "%s_activity", ident);
        cJSON_AddStringToObject(c1, "unique_id", uid);
        cJSON_AddStringToObject(c1, "command_topic", cmd_top);
        cJSON_AddStringToObject(c1, "state_topic", state_topic);
        cJSON_AddStringToObject(c1, "value_template", "{{ value_json.activity }}");
        cJSON *opts = cJSON_CreateArray();
        cJSON_AddItemToArray(opts, cJSON_CreateString("PowerOff"));
        for (int i = 0; i < act_list.count; i++) {
            cJSON_AddItemToArray(opts, cJSON_CreateString(act_list.items[i].name));
        }
        cJSON_AddItemToObject(c1, "options", opts);
        cJSON_AddStringToObject(c1, "availability_topic", status_topic);
        cJSON_AddStringToObject(c1, "payload_available", "online");
        cJSON_AddStringToObject(c1, "payload_not_available", "offline");
        cJSON_AddStringToObject(c1, "icon", "mdi:remote-tv");
        cJSON_AddItemToObject(c1, "device", create_ha_device_obj(ident, name));
        char *p1 = cJSON_PrintUnformatted(c1);
        cJSON_Delete(c1);
        if (p1) {
            mqtt_publish(top, p1, 1);
            free(p1);
        }
    }

    /* 2. sensor/<ident>_activity_id/config */
    snprintf(top, sizeof(top), "%s/sensor/%s_activity_id/config", discovery_prefix, ident);
    cJSON *c2 = cJSON_CreateObject();
    if (c2) {
        cJSON_AddStringToObject(c2, "name", "Activity ID");
        snprintf(uid, sizeof(uid), "%s_activity_id", ident);
        cJSON_AddStringToObject(c2, "unique_id", uid);
        cJSON_AddStringToObject(c2, "state_topic", state_topic);
        cJSON_AddStringToObject(c2, "value_template", "{{ value_json.activityId }}");
        cJSON_AddStringToObject(c2, "availability_topic", status_topic);
        cJSON_AddStringToObject(c2, "payload_available", "online");
        cJSON_AddStringToObject(c2, "payload_not_available", "offline");
        cJSON_AddStringToObject(c2, "icon", "mdi:identifier");
        cJSON_AddStringToObject(c2, "entity_category", "diagnostic");
        cJSON_AddItemToObject(c2, "device", create_ha_device_obj(ident, name));
        char *p2 = cJSON_PrintUnformatted(c2);
        cJSON_Delete(c2);
        if (p2) {
            mqtt_publish(top, p2, 1);
            free(p2);
        }
    }

    char local_ip[64] = {0};
    get_local_ip(mqtt_host, local_ip, sizeof(local_ip));

    /* 3. sensor/<ident>_ip/config */
    snprintf(top, sizeof(top), "%s/sensor/%s_ip/config", discovery_prefix, ident);
    cJSON *c_ip = cJSON_CreateObject();
    if (c_ip) {
        cJSON_AddStringToObject(c_ip, "name", "IP Address");
        snprintf(uid, sizeof(uid), "%s_ip", ident);
        cJSON_AddStringToObject(c_ip, "unique_id", uid);
        cJSON_AddStringToObject(c_ip, "state_topic", state_topic);
        cJSON_AddStringToObject(c_ip, "value_template", "{{ value_json.ip }}");
        cJSON_AddStringToObject(c_ip, "json_attributes_topic", state_topic);
        cJSON_AddStringToObject(c_ip, "availability_topic", status_topic);
        cJSON_AddStringToObject(c_ip, "payload_available", "online");
        cJSON_AddStringToObject(c_ip, "payload_not_available", "offline");
        cJSON_AddStringToObject(c_ip, "icon", "mdi:ip-network");
        cJSON_AddStringToObject(c_ip, "entity_category", "diagnostic");
        cJSON_AddItemToObject(c_ip, "origin", create_ha_origin_obj(local_ip));
        cJSON_AddItemToObject(c_ip, "device", create_ha_device_obj(ident, name));
        char *p_ip = cJSON_PrintUnformatted(c_ip);
        cJSON_Delete(c_ip);
        if (p_ip) {
            mqtt_publish(top, p_ip, 1);
            free(p_ip);
        }
    }

    /* 4. sensor/<ident>_firmware/config */
    snprintf(top, sizeof(top), "%s/sensor/%s_firmware/config", discovery_prefix, ident);
    cJSON *c_fw = cJSON_CreateObject();
    if (c_fw) {
        cJSON_AddStringToObject(c_fw, "name", "Firmware");
        snprintf(uid, sizeof(uid), "%s_firmware", ident);
        cJSON_AddStringToObject(c_fw, "unique_id", uid);
        cJSON_AddStringToObject(c_fw, "state_topic", state_topic);
        cJSON_AddStringToObject(c_fw, "value_template", "{{ value_json.firmware }}");
        cJSON_AddStringToObject(c_fw, "availability_topic", status_topic);
        cJSON_AddStringToObject(c_fw, "payload_available", "online");
        cJSON_AddStringToObject(c_fw, "payload_not_available", "offline");
        cJSON_AddStringToObject(c_fw, "icon", "mdi:chip");
        cJSON_AddStringToObject(c_fw, "entity_category", "diagnostic");
        cJSON_AddItemToObject(c_fw, "origin", create_ha_origin_obj(local_ip));
        cJSON_AddItemToObject(c_fw, "device", create_ha_device_obj(ident, name));
        char *p_fw = cJSON_PrintUnformatted(c_fw);
        cJSON_Delete(c_fw);
        if (p_fw) {
            mqtt_publish(top, p_fw, 1);
            free(p_fw);
        }
    }

    /* 5. sensor/<ident>_ir_devices/config */
    snprintf(top, sizeof(top), "%s/sensor/%s_ir_devices/config", discovery_prefix, ident);
    cJSON *c_devs = cJSON_CreateObject();
    if (c_devs) {
        cJSON_AddStringToObject(c_devs, "name", "IR Devices");
        snprintf(uid, sizeof(uid), "%s_ir_devices", ident);
        cJSON_AddStringToObject(c_devs, "unique_id", uid);
        cJSON_AddStringToObject(c_devs, "state_topic", state_topic);
        cJSON_AddStringToObject(c_devs, "value_template", "{{ value_json.deviceCount }}");
        cJSON_AddStringToObject(c_devs, "availability_topic", status_topic);
        cJSON_AddStringToObject(c_devs, "payload_available", "online");
        cJSON_AddStringToObject(c_devs, "payload_not_available", "offline");
        cJSON_AddStringToObject(c_devs, "icon", "mdi:remote");
        cJSON_AddStringToObject(c_devs, "entity_category", "diagnostic");
        cJSON_AddItemToObject(c_devs, "origin", create_ha_origin_obj(local_ip));
        cJSON_AddItemToObject(c_devs, "device", create_ha_device_obj(ident, name));
        char *p_devs = cJSON_PrintUnformatted(c_devs);
        cJSON_Delete(c_devs);
        if (p_devs) {
            mqtt_publish(top, p_devs, 1);
            free(p_devs);
        }
    }

    /* 6. sensor/<ident>_ir_commands/config */
    snprintf(top, sizeof(top), "%s/sensor/%s_ir_commands/config", discovery_prefix, ident);
    cJSON *c_cmds = cJSON_CreateObject();
    if (c_cmds) {
        cJSON_AddStringToObject(c_cmds, "name", "IR Commands");
        snprintf(uid, sizeof(uid), "%s_ir_commands", ident);
        cJSON_AddStringToObject(c_cmds, "unique_id", uid);
        cJSON_AddStringToObject(c_cmds, "state_topic", state_topic);
        cJSON_AddStringToObject(c_cmds, "value_template", "{{ value_json.commandCount }}");
        cJSON_AddStringToObject(c_cmds, "availability_topic", status_topic);
        cJSON_AddStringToObject(c_cmds, "payload_available", "online");
        cJSON_AddStringToObject(c_cmds, "payload_not_available", "offline");
        cJSON_AddStringToObject(c_cmds, "icon", "mdi:counter");
        cJSON_AddStringToObject(c_cmds, "entity_category", "diagnostic");
        cJSON_AddItemToObject(c_cmds, "origin", create_ha_origin_obj(local_ip));
        cJSON_AddItemToObject(c_cmds, "device", create_ha_device_obj(ident, name));
        char *p_cmds = cJSON_PrintUnformatted(c_cmds);
        cJSON_Delete(c_cmds);
        if (p_cmds) {
            mqtt_publish(top, p_cmds, 1);
            free(p_cmds);
        }
    }

    /* 7. button/<ident>_power_off/config */
    snprintf(top, sizeof(top), "%s/button/%s_power_off/config", discovery_prefix, ident);
    cJSON *c3 = cJSON_CreateObject();
    if (c3) {
        cJSON_AddStringToObject(c3, "name", "Power Off");
        snprintf(uid, sizeof(uid), "%s_power_off", ident);
        cJSON_AddStringToObject(c3, "unique_id", uid);
        cJSON_AddStringToObject(c3, "command_topic", cmd_top);
        cJSON_AddStringToObject(c3, "payload_press", "PowerOff");
        cJSON_AddStringToObject(c3, "availability_topic", status_topic);
        cJSON_AddStringToObject(c3, "payload_available", "online");
        cJSON_AddStringToObject(c3, "payload_not_available", "offline");
        cJSON_AddStringToObject(c3, "icon", "mdi:power");
        cJSON_AddItemToObject(c3, "device", create_ha_device_obj(ident, name));
        char *p3 = cJSON_PrintUnformatted(c3);
        cJSON_Delete(c3);
        if (p3) {
            mqtt_publish(top, p3, 1);
            free(p3);
        }
    }

    /* 8. button/<ident>_sync/config */
    snprintf(top, sizeof(top), "%s/button/%s_sync/config", discovery_prefix, ident);
    cJSON *c4 = cJSON_CreateObject();
    if (c4) {
        cJSON_AddStringToObject(c4, "name", "Sync");
        snprintf(uid, sizeof(uid), "%s_sync", ident);
        cJSON_AddStringToObject(c4, "unique_id", uid);
        char sync_top[160];
        snprintf(sync_top, sizeof(sync_top), "%s/sync", base_topic);
        cJSON_AddStringToObject(c4, "command_topic", sync_top);
        cJSON_AddStringToObject(c4, "payload_press", "PRESS");
        cJSON_AddStringToObject(c4, "availability_topic", status_topic);
        cJSON_AddStringToObject(c4, "payload_available", "online");
        cJSON_AddStringToObject(c4, "payload_not_available", "offline");
        cJSON_AddStringToObject(c4, "icon", "mdi:sync");
        cJSON_AddStringToObject(c4, "entity_category", "config");
        cJSON_AddItemToObject(c4, "device", create_ha_device_obj(ident, name));
        char *p4 = cJSON_PrintUnformatted(c4);
        cJSON_Delete(c4);
        if (p4) {
            mqtt_publish(top, p4, 1);
            free(p4);
        }
    }

    /* 9. button/<ident>_remote_1_sleep/config & button/<ident>_remote_2_sleep/config */
    for (int s = 1; s <= 2; s++) {
        char sleep_top[160];
        snprintf(sleep_top, sizeof(sleep_top), "%s/remote/%d/sleep", base_topic, s);
        snprintf(top, sizeof(top), "%s/button/%s_remote_%d_sleep/config", discovery_prefix, ident, s);
        cJSON *cs = cJSON_CreateObject();
        if (cs) {
            char bname[64];
            snprintf(bname, sizeof(bname), "Remote %d Sleep", s);
            cJSON_AddStringToObject(cs, "name", bname);
            snprintf(uid, sizeof(uid), "%s_remote_%d_sleep", ident, s);
            cJSON_AddStringToObject(cs, "unique_id", uid);
            cJSON_AddStringToObject(cs, "command_topic", sleep_top);
            cJSON_AddStringToObject(cs, "payload_press", "PRESS");
            cJSON_AddStringToObject(cs, "availability_topic", status_topic);
            cJSON_AddStringToObject(cs, "payload_available", "online");
            cJSON_AddStringToObject(cs, "payload_not_available", "offline");
            cJSON_AddStringToObject(cs, "icon", "mdi:sleep");
            cJSON_AddStringToObject(cs, "entity_category", "config");
            cJSON_AddItemToObject(cs, "device", create_ha_device_obj(ident, name));
            char *ps = cJSON_PrintUnformatted(cs);
            cJSON_Delete(cs);
            if (ps) {
                mqtt_publish(top, ps, 1);
                free(ps);
            }
        }
    }
}
