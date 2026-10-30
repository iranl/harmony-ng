#define _GNU_SOURCE
#include "hub_mqtt.h"
#include "hub_mqtt_ha.h"
#include "config_store.h"
#include "orchestrator.h"
#include "mqtt_client.h"
#include "hw_action.h"
#include "harmony_version.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <strings.h>

/* MQTT configuration globals */
static char g_mqtt_host[128] = "";
static int  g_mqtt_port = 1883;
char g_mqtt_base_topic[128] = "harmony/hub";
static char g_mqtt_discovery_prefix[64] = "homeassistant";
static char g_mqtt_name[64] = "Harmony Hub";
static char g_mqtt_cid[64] = "harmony-local-mqtt";
static char g_mqtt_user[256] = "";
static char g_mqtt_pass[256] = "";
static int  g_mqtt_ha_discovery = 1;
static int  g_mqtt_enabled = 0;
static int  g_mqtt_bt_events = 0;
static char g_mqtt_bt_topic[160] = "";
static int  g_mqtt_elite_events = 1;
static char g_mqtt_elite_topic[160] = "";
static int  g_mqtt_slot1_events = -1;
static char g_mqtt_slot1_topic[160] = "";
static int  g_mqtt_slot2_events = -1;
static char g_mqtt_slot2_topic[160] = "";
static char g_status_topic[160] = "harmony/hub/status";
static char g_state_topic[160] = "harmony/hub/state";

/* Forward declarations */
static void publish_ha_discovery_wrapper(void);
static void publish_mqtt_state_ex(int force);
static void publish_mqtt_state(void);
static void on_mqtt_command(const char *topic, const char *payload, size_t len, void *ud);
static void subscribe_mqtt_topics(void);

void daemon_get_mqtt_info(char *host, size_t hlen, int *port, char *base, size_t blen, int *connected) {
    if (host && hlen > 0) {
        strncpy(host, g_mqtt_host, hlen - 1);
        host[hlen - 1] = '\0';
    }
    if (port) *port = g_mqtt_port;
    if (base && blen > 0) {
        strncpy(base, g_mqtt_base_topic, blen - 1);
        base[blen - 1] = '\0';
    }
    if (connected) *connected = mqtt_is_connected();
}

static int load_mqtt_config(char *host, int *port, char *cid, char *user, char *pass) {
    cJSON *root = config_store_get_mqtt_json();
    if (!root) return 0;

    cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (!en || !cJSON_IsTrue(en)) {
        cJSON_Delete(root);
        return 0;
    }

    cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (h && cJSON_IsString(h) && h->valuestring && h->valuestring[0]) {
        strncpy(host, h->valuestring, 127);
        host[127] = '\0';
    } else {
        cJSON_Delete(root);
        return 0;
    }

    cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "port");
    if (p && cJSON_IsNumber(p) && p->valueint > 0) {
        *port = p->valueint;
    } else {
        *port = 1883;
    }

    cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "clientId");
    if (c && cJSON_IsString(c) && c->valuestring && c->valuestring[0]) {
        strncpy(cid, c->valuestring, 63);
        cid[63] = '\0';
    } else {
        strncpy(cid, "harmony-local-mqtt", 63);
        cid[63] = '\0';
    }

    cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "username");
    if (u && cJSON_IsString(u) && u->valuestring) {
        strncpy(user, u->valuestring, 255);
        user[255] = '\0';
    } else {
        user[0] = '\0';
    }

    cJSON *pw = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (pw && cJSON_IsString(pw) && pw->valuestring) {
        strncpy(pass, pw->valuestring, 255);
        pass[255] = '\0';
    } else {
        pass[0] = '\0';
    }

    cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "baseTopic");
    if (b && cJSON_IsString(b) && b->valuestring && b->valuestring[0]) {
        strncpy(g_mqtt_base_topic, b->valuestring, sizeof(g_mqtt_base_topic) - 1);
        g_mqtt_base_topic[sizeof(g_mqtt_base_topic) - 1] = '\0';
        size_t blen = strlen(g_mqtt_base_topic);
        while (blen > 0 && g_mqtt_base_topic[blen - 1] == '/') {
            g_mqtt_base_topic[blen - 1] = '\0';
            blen--;
        }
    } else {
        strncpy(g_mqtt_base_topic, "harmony/hub", sizeof(g_mqtt_base_topic) - 1);
        g_mqtt_base_topic[sizeof(g_mqtt_base_topic) - 1] = '\0';
    }

    cJSON *dp = cJSON_GetObjectItemCaseSensitive(root, "discoveryPrefix");
    if (dp && cJSON_IsString(dp) && dp->valuestring && dp->valuestring[0]) {
        strncpy(g_mqtt_discovery_prefix, dp->valuestring, sizeof(g_mqtt_discovery_prefix) - 1);
        g_mqtt_discovery_prefix[sizeof(g_mqtt_discovery_prefix) - 1] = '\0';
    } else {
        strncpy(g_mqtt_discovery_prefix, "homeassistant", sizeof(g_mqtt_discovery_prefix) - 1);
        g_mqtt_discovery_prefix[sizeof(g_mqtt_discovery_prefix) - 1] = '\0';
    }

    cJSON *nm = cJSON_GetObjectItemCaseSensitive(root, "deviceName");
    if (nm && cJSON_IsString(nm) && nm->valuestring && nm->valuestring[0]) {
        strncpy(g_mqtt_name, nm->valuestring, sizeof(g_mqtt_name) - 1);
        g_mqtt_name[sizeof(g_mqtt_name) - 1] = '\0';
    } else {
        strncpy(g_mqtt_name, "Harmony Hub", sizeof(g_mqtt_name) - 1);
        g_mqtt_name[sizeof(g_mqtt_name) - 1] = '\0';
    }

    cJSON *ha = cJSON_GetObjectItemCaseSensitive(root, "haDiscovery");
    if (ha) {
        g_mqtt_ha_discovery = cJSON_IsTrue(ha) ? 1 : 0;
    } else {
        g_mqtt_ha_discovery = 1;
    }

    cJSON *bte = cJSON_GetObjectItemCaseSensitive(root, "btRemoteEvents");
    g_mqtt_bt_events = (bte && (cJSON_IsTrue(bte) || (cJSON_IsNumber(bte) && bte->valueint > 0))) ? 1 : 0;

    cJSON *btt = cJSON_GetObjectItemCaseSensitive(root, "btRemoteTopic");
    if (btt && cJSON_IsString(btt) && btt->valuestring) {
        strncpy(g_mqtt_bt_topic, btt->valuestring, sizeof(g_mqtt_bt_topic) - 1);
        g_mqtt_bt_topic[sizeof(g_mqtt_bt_topic) - 1] = '\0';
    } else {
        g_mqtt_bt_topic[0] = '\0';
    }

    cJSON *ele = cJSON_GetObjectItemCaseSensitive(root, "eliteRemoteEvents");
    if (ele) {
        g_mqtt_elite_events = (cJSON_IsTrue(ele) || (cJSON_IsNumber(ele) && ele->valueint > 0)) ? 1 : 0;
    } else {
        g_mqtt_elite_events = 0;
    }

    cJSON *elt = cJSON_GetObjectItemCaseSensitive(root, "eliteRemoteTopic");
    if (elt && cJSON_IsString(elt) && elt->valuestring) {
        strncpy(g_mqtt_elite_topic, elt->valuestring, sizeof(g_mqtt_elite_topic) - 1);
        g_mqtt_elite_topic[sizeof(g_mqtt_elite_topic) - 1] = '\0';
    } else {
        g_mqtt_elite_topic[0] = '\0';
    }

    cJSON *s1e = cJSON_GetObjectItemCaseSensitive(root, "slot1RemoteEvents");
    g_mqtt_slot1_events = s1e ? ((cJSON_IsTrue(s1e) || (cJSON_IsNumber(s1e) && s1e->valueint > 0)) ? 1 : 0) : g_mqtt_elite_events;

    cJSON *s1t = cJSON_GetObjectItemCaseSensitive(root, "slot1RemoteTopic");
    if (s1t && cJSON_IsString(s1t) && s1t->valuestring) {
        strncpy(g_mqtt_slot1_topic, s1t->valuestring, sizeof(g_mqtt_slot1_topic) - 1);
        g_mqtt_slot1_topic[sizeof(g_mqtt_slot1_topic) - 1] = '\0';
    } else {
        g_mqtt_slot1_topic[0] = '\0';
    }

    cJSON *s2e = cJSON_GetObjectItemCaseSensitive(root, "slot2RemoteEvents");
    g_mqtt_slot2_events = s2e ? ((cJSON_IsTrue(s2e) || (cJSON_IsNumber(s2e) && s2e->valueint > 0)) ? 1 : 0) : g_mqtt_elite_events;

    cJSON *s2t = cJSON_GetObjectItemCaseSensitive(root, "slot2RemoteTopic");
    if (s2t && cJSON_IsString(s2t) && s2t->valuestring) {
        strncpy(g_mqtt_slot2_topic, s2t->valuestring, sizeof(g_mqtt_slot2_topic) - 1);
        g_mqtt_slot2_topic[sizeof(g_mqtt_slot2_topic) - 1] = '\0';
    } else {
        g_mqtt_slot2_topic[0] = '\0';
    }

    snprintf(g_status_topic, sizeof(g_status_topic), "%s/status", g_mqtt_base_topic);
    snprintf(g_state_topic, sizeof(g_state_topic), "%s/state", g_mqtt_base_topic);

    cJSON_Delete(root);
    return 1;
}

void hub_mqtt_publish_progress(const char *msg) {
    if (!mqtt_is_connected() || !msg || !msg[0]) return;
    char top[160];
    snprintf(top, sizeof(top), "%s/progress", g_mqtt_base_topic);
    mqtt_publish(top, msg, 0);

    cJSON *root = cJSON_Parse(msg);
    if (root) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        if (type && cJSON_IsString(type) && strcmp(type->valuestring, "activity_change") == 0) {
            publish_mqtt_state_ex(1);
        }
        cJSON_Delete(root);
    }
}

static char g_last_state_payload[2048] = {0};

static void publish_mqtt_state_ex(int force) {
    if (!mqtt_is_connected()) return;

    const char *cur_id = orch_get_current_activity();
    if (!cur_id || !cur_id[0]) cur_id = "-1";

    struct daemon_activity_list act_list;
    load_daemon_activities(&act_list);

    char act_name[64] = "PowerOff";
    char actual_cur_id[32] = "-1";
    strncpy(actual_cur_id, cur_id, sizeof(actual_cur_id) - 1);
    actual_cur_id[sizeof(actual_cur_id) - 1] = '\0';

    int is_on = 0;
    if (strcmp(actual_cur_id, "-1") != 0 && strcmp(actual_cur_id, "PowerOff") != 0) {
        int found = 0;
        for (int i = 0; i < act_list.count; i++) {
            if (strcmp(actual_cur_id, act_list.items[i].id) == 0) {
                strncpy(act_name, act_list.items[i].name, sizeof(act_name) - 1);
                is_on = 1;
                found = 1;
                break;
            }
        }
        if (!found) {
            for (int i = 0; i < act_list.count; i++) {
                if (strcasecmp(actual_cur_id, act_list.items[i].name) == 0) {
                    strncpy(actual_cur_id, act_list.items[i].id, sizeof(actual_cur_id) - 1);
                    strncpy(act_name, act_list.items[i].name, sizeof(act_name) - 1);
                    is_on = 1;
                    found = 1;
                    break;
                }
            }
        }
        if (!found) {
            strncpy(act_name, "PowerOff", sizeof(act_name) - 1);
            strncpy(actual_cur_id, "-1", sizeof(actual_cur_id) - 1);
            is_on = 0;
        }
    }

    char fw[32] = "4.15.600";
    FILE *ffw = fopen("/etc/version", "r");
    if (ffw) {
        if (fgets(fw, sizeof(fw), ffw)) {
            char *nl = strchr(fw, '\n'); if (nl) *nl = 0;
            nl = strchr(fw, '\r'); if (nl) *nl = 0;
        }
        fclose(ffw);
    }

    char local_ip[64] = {0};
    get_local_ip(g_mqtt_host, local_ip, sizeof(local_ip));
    int dev_count = 0, cmd_count = 0;
    get_device_and_command_counts(&dev_count, &cmd_count);

    cJSON *st = cJSON_CreateObject();
    if (st) {
        cJSON_AddStringToObject(st, "state", is_on ? "on" : "off");
        cJSON_AddStringToObject(st, "current_activity", act_name);
        cJSON_AddStringToObject(st, "activity", act_name);
        cJSON_AddStringToObject(st, "activityId", actual_cur_id);

        cJSON *act_arr = cJSON_CreateArray();
        cJSON_AddItemToArray(act_arr, cJSON_CreateString("PowerOff"));
        for (int i = 0; i < act_list.count; i++) {
            cJSON_AddItemToArray(act_arr, cJSON_CreateString(act_list.items[i].name));
        }
        cJSON_AddItemToObject(st, "activity_list", act_arr);
        cJSON_AddStringToObject(st, "firmware", fw);
        cJSON_AddStringToObject(st, "ip", local_ip);
        cJSON_AddNumberToObject(st, "deviceCount", dev_count);
        cJSON_AddNumberToObject(st, "commandCount", cmd_count);
        cJSON_AddStringToObject(st, "baseTopic", g_mqtt_base_topic);

        char *payload = cJSON_PrintUnformatted(st);
        cJSON_Delete(st);
        if (payload) {
            if (!force && strcmp(payload, g_last_state_payload) == 0) {
                free(payload);
                return;
            }
            strncpy(g_last_state_payload, payload, sizeof(g_last_state_payload) - 1);
            g_last_state_payload[sizeof(g_last_state_payload) - 1] = '\0';
            printf("[*] MQTT state published (retain=1): %s\n", payload);
            mqtt_publish(g_state_topic, payload, 1);
            free(payload);

            char sub_top[160];
            snprintf(sub_top, sizeof(sub_top), "%s/current_activity", g_mqtt_base_topic);
            mqtt_publish(sub_top, act_name, 1);
            snprintf(sub_top, sizeof(sub_top), "%s/activity", g_mqtt_base_topic);
            mqtt_publish(sub_top, act_name, 1);
            snprintf(sub_top, sizeof(sub_top), "%s/activity_id", g_mqtt_base_topic);
            mqtt_publish(sub_top, actual_cur_id, 1);
            snprintf(sub_top, sizeof(sub_top), "%s/status", g_mqtt_base_topic);
            mqtt_publish(sub_top, "online", 1);
        }
    }
}

static void publish_mqtt_state(void) {
    publish_mqtt_state_ex(0);
}

void daemon_publish_mqtt_state(void) {
    publish_mqtt_state_ex(1);
}

static void publish_ha_discovery_wrapper(void) {
    publish_ha_discovery(g_mqtt_cid, g_mqtt_name, g_mqtt_base_topic,
                         g_mqtt_discovery_prefix, g_state_topic,
                         g_status_topic, g_mqtt_host, g_mqtt_ha_discovery);
}

void daemon_sync_mqtt(void) {
    char host[128] = {0};
    int port = 1883;
    char cid[64] = "harmony-local-mqtt";
    char user[256] = {0};
    char pass[256] = {0};

    int was_enabled = g_mqtt_enabled;
    g_mqtt_enabled = load_mqtt_config(host, &port, cid, user, pass);

    if (!g_mqtt_enabled) {
        if (was_enabled && mqtt_is_connected()) {
            mqtt_disconnect();
            g_last_state_payload[0] = '\0';
        }
        return;
    }

    g_mqtt_port = port;
    strncpy(g_mqtt_host, host, sizeof(g_mqtt_host) - 1);
    g_mqtt_host[sizeof(g_mqtt_host) - 1] = '\0';
    strncpy(g_mqtt_cid, cid, sizeof(g_mqtt_cid) - 1);
    g_mqtt_cid[sizeof(g_mqtt_cid) - 1] = '\0';
    strncpy(g_mqtt_user, user, sizeof(g_mqtt_user) - 1);
    g_mqtt_user[sizeof(g_mqtt_user) - 1] = '\0';
    strncpy(g_mqtt_pass, pass, sizeof(g_mqtt_pass) - 1);
    g_mqtt_pass[sizeof(g_mqtt_pass) - 1] = '\0';

    if (mqtt_is_connected()) {
        mqtt_disconnect();
        g_last_state_payload[0] = '\0';
    }

    mqtt_init(g_mqtt_host, g_mqtt_port, g_mqtt_cid, g_mqtt_user, g_mqtt_pass);
    mqtt_set_lwt(g_status_topic, "offline", 1);
    mqtt_set_birth(g_status_topic, "online", 1);

    if (mqtt_connect() == 0) {
        printf("[+] Reconnected to MQTT broker %s:%d\n", g_mqtt_host, g_mqtt_port);
        publish_ha_discovery_wrapper();
        subscribe_mqtt_topics();
        publish_mqtt_state_ex(1);
    } else {
        printf("[*] Connecting to MQTT broker in background...\n");
    }
}

static void handle_mqtt_activity_set(const char *payload) {
    if (!payload || !payload[0]) return;

    char trimmed[128];
    strncpy(trimmed, payload, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';

    char *start = trimmed;
    while (*start && isspace((unsigned char)*start)) start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';

    if (!*start) return;

    if (strcasecmp(start, "poweroff") == 0 || strcmp(start, "-1") == 0) {
        printf("[*] MQTT command: PowerOff\n");
        orch_power_off(0, NULL, 0);
        return;
    }

    struct daemon_activity_list act_list;
    load_daemon_activities(&act_list);

    for (int i = 0; i < act_list.count; i++) {
        if (strcmp(start, act_list.items[i].id) == 0) {
            printf("[*] MQTT command: start activity %s (%s)\n", act_list.items[i].name, act_list.items[i].id);
            orch_switch_activity(act_list.items[i].id, 0, NULL, 0);
            return;
        }
    }

    for (int i = 0; i < act_list.count; i++) {
        if (strcasecmp(start, act_list.items[i].name) == 0) {
            printf("[*] MQTT command: start activity %s (%s)\n", act_list.items[i].name, act_list.items[i].id);
            orch_switch_activity(act_list.items[i].id, 0, NULL, 0);
            return;
        }
    }

    printf("[-] MQTT command: activity '%s' not found\n", start);
}

struct mqtt_pulse_entry {
    char topic[256];
    char off_payload[16];
    uint64_t off_deadline_ms;
    int active;
};
static struct mqtt_pulse_entry g_pulse_queue[16];

typedef struct {
    int active;
    char dev[128];
    char cmd_list[32][64];
    int cmd_count;
    int num_repeats;
    int cur_repeat;
    int cur_cmd;
    uint32_t hold_ms;
    uint32_t delay_ms;
    uint64_t next_exec_ms;
} mqtt_cmd_seq_t;
static mqtt_cmd_seq_t g_mqtt_seq = {0};

static void queue_pulse_off_ex(const char *topic, const char *off_val, int pulse_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    uint64_t deadline = now_ms + (pulse_ms > 0 ? (uint64_t)pulse_ms : 50ULL);

    for (int i = 0; i < 16; i++) {
        if (!g_pulse_queue[i].active) {
            strncpy(g_pulse_queue[i].topic, topic, sizeof(g_pulse_queue[i].topic) - 1);
            g_pulse_queue[i].topic[sizeof(g_pulse_queue[i].topic) - 1] = '\0';
            strncpy(g_pulse_queue[i].off_payload, off_val ? off_val : "0", sizeof(g_pulse_queue[i].off_payload) - 1);
            g_pulse_queue[i].off_payload[sizeof(g_pulse_queue[i].off_payload) - 1] = '\0';
            g_pulse_queue[i].off_deadline_ms = deadline;
            g_pulse_queue[i].active = 1;
            return;
        }
    }
    mqtt_publish(topic, off_val ? off_val : "0", 0);
}

void mqtt_cmd_tick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;

    for (int i = 0; i < 16; i++) {
        if (g_pulse_queue[i].active && now_ms >= g_pulse_queue[i].off_deadline_ms) {
            mqtt_publish(g_pulse_queue[i].topic, g_pulse_queue[i].off_payload, 0);
            g_pulse_queue[i].active = 0;
        }
    }

    if (g_mqtt_seq.active && now_ms >= g_mqtt_seq.next_exec_ms) {
        printf("[*] Executing command [%d/%d] (repeat %d/%d): dev='%s' cmd='%s'\n",
               g_mqtt_seq.cur_cmd + 1, g_mqtt_seq.cmd_count,
               g_mqtt_seq.cur_repeat + 1, g_mqtt_seq.num_repeats,
               g_mqtt_seq.dev, g_mqtt_seq.cmd_list[g_mqtt_seq.cur_cmd]);
        hw_device_command_send(g_mqtt_seq.dev, g_mqtt_seq.cmd_list[g_mqtt_seq.cur_cmd]);

        g_mqtt_seq.cur_cmd++;
        if (g_mqtt_seq.cur_cmd >= g_mqtt_seq.cmd_count) {
            g_mqtt_seq.cur_cmd = 0;
            g_mqtt_seq.cur_repeat++;
        }

        if (g_mqtt_seq.cur_repeat >= g_mqtt_seq.num_repeats) {
            g_mqtt_seq.active = 0;
        } else {
            uint32_t wait_ms = g_mqtt_seq.hold_ms + g_mqtt_seq.delay_ms;
            g_mqtt_seq.next_exec_ms = now_ms + (wait_ms > 0 ? wait_ms : 50);
        }
    }
}

static void handle_mqtt_send_command(const char *payload) {
    if (!payload || !payload[0]) return;

    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        printf("[-] MQTT send_command: invalid JSON payload\n");
        return;
    }

    cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
    if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(root, "dev");

    char dev[128] = {0};
    if (j_dev) {
        if (cJSON_IsString(j_dev) && j_dev->valuestring) {
            strncpy(dev, j_dev->valuestring, sizeof(dev) - 1);
        } else if (cJSON_IsNumber(j_dev)) {
            snprintf(dev, sizeof(dev), "%ld", (long)j_dev->valuedouble);
        }
    }

    if (!dev[0]) {
        printf("[-] MQTT send_command: missing deviceId or device in payload\n");
        cJSON_Delete(root);
        return;
    }

    /* Extract command list: string, array, or number */
    char cmd_list[32][64];
    int cmd_count = 0;

    cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!j_cmd) j_cmd = cJSON_GetObjectItemCaseSensitive(root, "commands");
    if (!j_cmd) j_cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");

    if (j_cmd) {
        if (cJSON_IsArray(j_cmd)) {
            cJSON *elem = NULL;
            cJSON_ArrayForEach(elem, j_cmd) {
                if (cmd_count >= 32) break;
                if (cJSON_IsString(elem) && elem->valuestring && elem->valuestring[0]) {
                    strncpy(cmd_list[cmd_count], elem->valuestring, sizeof(cmd_list[0]) - 1);
                    cmd_list[cmd_count][sizeof(cmd_list[0]) - 1] = '\0';
                    cmd_count++;
                } else if (cJSON_IsNumber(elem)) {
                    snprintf(cmd_list[cmd_count], sizeof(cmd_list[0]), "%ld", (long)elem->valuedouble);
                    cmd_count++;
                }
            }
        } else if (cJSON_IsString(j_cmd) && j_cmd->valuestring && j_cmd->valuestring[0]) {
            strncpy(cmd_list[0], j_cmd->valuestring, sizeof(cmd_list[0]) - 1);
            cmd_list[0][sizeof(cmd_list[0]) - 1] = '\0';
            cmd_count = 1;
        } else if (cJSON_IsNumber(j_cmd)) {
            snprintf(cmd_list[0], sizeof(cmd_list[0]), "%ld", (long)j_cmd->valuedouble);
            cmd_count = 1;
        }
    }

    if (cmd_count == 0) {
        printf("[-] MQTT send_command: no commands parsed for device '%s'\n", dev);
        cJSON_Delete(root);
        return;
    }

    cJSON *act_j = cJSON_GetObjectItemCaseSensitive(root, "action");
    const char *action = (act_j && cJSON_IsString(act_j)) ? act_j->valuestring : "tap";

    double delay_secs = -1.0;
    cJSON *j_delay = cJSON_GetObjectItemCaseSensitive(root, "delay_secs");
    if (!j_delay) j_delay = cJSON_GetObjectItemCaseSensitive(root, "delaySecs");
    if (!j_delay) j_delay = cJSON_GetObjectItemCaseSensitive(root, "delay");
    if (j_delay && cJSON_IsNumber(j_delay)) {
        delay_secs = j_delay->valuedouble;
    } else {
        cJSON *j_delay_ms = cJSON_GetObjectItemCaseSensitive(root, "delay_ms");
        if (!j_delay_ms) j_delay_ms = cJSON_GetObjectItemCaseSensitive(root, "delayMs");
        if (j_delay_ms && cJSON_IsNumber(j_delay_ms)) {
            delay_secs = j_delay_ms->valuedouble / 1000.0;
        }
    }

    double hold_secs = 0.0;
    cJSON *j_hold = cJSON_GetObjectItemCaseSensitive(root, "hold_secs");
    if (!j_hold) j_hold = cJSON_GetObjectItemCaseSensitive(root, "holdSecs");
    if (!j_hold) j_hold = cJSON_GetObjectItemCaseSensitive(root, "hold");
    if (j_hold && cJSON_IsNumber(j_hold)) {
        hold_secs = j_hold->valuedouble;
    } else {
        cJSON *j_hold_ms = cJSON_GetObjectItemCaseSensitive(root, "hold_ms");
        if (!j_hold_ms) j_hold_ms = cJSON_GetObjectItemCaseSensitive(root, "holdMs");
        if (j_hold_ms && cJSON_IsNumber(j_hold_ms)) {
            hold_secs = j_hold_ms->valuedouble / 1000.0;
        }
    }
    if (strcasecmp(action, "hold") == 0 && hold_secs <= 0.001) {
        hold_secs = 0.5;
    }

    int num_repeats = 1;
    cJSON *j_rep = cJSON_GetObjectItemCaseSensitive(root, "num_repeats");
    if (!j_rep) j_rep = cJSON_GetObjectItemCaseSensitive(root, "numRepeats");
    if (!j_rep) j_rep = cJSON_GetObjectItemCaseSensitive(root, "repeats");
    if (j_rep && cJSON_IsNumber(j_rep)) num_repeats = (int)j_rep->valuedouble;

    cJSON_Delete(root);

    if (num_repeats < 1) num_repeats = 1;
    if (num_repeats > 50) num_repeats = 50;

    int is_multi = (cmd_count > 1) || (num_repeats > 1);
    if (delay_secs < 0) {
        delay_secs = is_multi ? 0.25 : 0.0;
    }
    if (delay_secs > 60.0) delay_secs = 60.0;
    if (hold_secs < 0) hold_secs = 0;
    if (hold_secs > 10.0) hold_secs = 10.0;

    printf("[*] MQTT send_command: dev='%s' cmds=%d repeats=%d action=%s delay=%.2fs hold=%.2fs\n",
           dev, cmd_count, num_repeats, action, delay_secs, hold_secs);

    if (cmd_count == 1 && num_repeats == 1 && hold_secs <= 0.001) {
        printf("[*] Executing command [1/1] (repeat 1/1): dev='%s' cmd='%s'\n", dev, cmd_list[0]);
        hw_device_command_send(dev, cmd_list[0]);
        return;
    }

    snprintf(g_mqtt_seq.dev, sizeof(g_mqtt_seq.dev), "%s", dev);
    g_mqtt_seq.cmd_count = cmd_count;
    for (int i = 0; i < cmd_count; i++) {
        snprintf(g_mqtt_seq.cmd_list[i], sizeof(g_mqtt_seq.cmd_list[i]), "%s", cmd_list[i]);
    }
    g_mqtt_seq.num_repeats = num_repeats;
    g_mqtt_seq.cur_repeat = 0;
    g_mqtt_seq.cur_cmd = 0;
    g_mqtt_seq.hold_ms = (uint32_t)(hold_secs * 1000.0);
    g_mqtt_seq.delay_ms = (uint32_t)(delay_secs * 1000.0);
    g_mqtt_seq.next_exec_ms = 0;
    g_mqtt_seq.active = 1;

    mqtt_cmd_tick();
}


int mqtt_dispatch_button_pulse_topic(const char *dev_id, const char *dev_name_in, const char *cmd_name,
                                     const char *topic_in, int req_pulse_ms,
                                     char *out_topic, size_t out_topic_len, int *out_pulse_ms) {
    if (!cmd_name || !cmd_name[0]) return -1;

    char dev_name[128] = {0};
    if (dev_name_in && dev_name_in[0]) {
        strncpy(dev_name, dev_name_in, sizeof(dev_name) - 1);
    } else if (dev_id && dev_id[0]) {
        strncpy(dev_name, dev_id, sizeof(dev_name) - 1);
    } else {
        strncpy(dev_name, "device", sizeof(dev_name) - 1);
    }

    char base_topic[128] = "harmony/hub";
    if (g_mqtt_base_topic[0]) {
        strncpy(base_topic, g_mqtt_base_topic, sizeof(base_topic) - 1);
    }

    char topic_pattern[256] = "{root}/button/{device}/{command}";
    int pulse_ms = 1000;

    if (topic_in && topic_in[0]) {
        strncpy(topic_pattern, topic_in, sizeof(topic_pattern) - 1);
    } else if (dev_id && dev_id[0]) {
        cJSON *droot = config_store_get_devices_json();
        if (droot) {
            cJSON *darr = cJSON_GetObjectItemCaseSensitive(droot, "devices");
            if (!darr) darr = cJSON_GetObjectItemCaseSensitive(droot, "DevicesWithFeatures");
            if (darr && cJSON_IsArray(darr)) {
                cJSON *di = NULL;
                cJSON_ArrayForEach(di, darr) {
                    cJSON *d = cJSON_GetObjectItemCaseSensitive(di, "Device");
                    if (!d) d = di;
                    cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                    if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                    if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id");
                    char d_id_str[64] = {0};
                    if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                    else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);

                    cJSON *jname = cJSON_GetObjectItemCaseSensitive(d, "name");
                    if (!jname) jname = cJSON_GetObjectItemCaseSensitive(d, "Name");
                    const char *d_name_str = (jname && cJSON_IsString(jname)) ? jname->valuestring : "";

                    if ((d_id_str[0] && strcmp(d_id_str, dev_id) == 0) ||
                        (d_name_str[0] && (strcmp(d_name_str, dev_id) == 0 || (dev_name_in && strcmp(d_name_str, dev_name_in) == 0)))) {
                        if ((!dev_name_in || !dev_name_in[0]) && d_name_str[0]) {
                            strncpy(dev_name, d_name_str, sizeof(dev_name) - 1);
                        }
                        cJSON *jtop = cJSON_GetObjectItemCaseSensitive(d, "mqttTopic");
                        if (!jtop) jtop = cJSON_GetObjectItemCaseSensitive(d, "MQTTTopic");
                        if (jtop && cJSON_IsString(jtop) && jtop->valuestring && jtop->valuestring[0]) {
                            strncpy(topic_pattern, jtop->valuestring, sizeof(topic_pattern) - 1);
                        }
                        cJSON *jp = cJSON_GetObjectItemCaseSensitive(d, "mqttPulseMs");
                        if (!jp) jp = cJSON_GetObjectItemCaseSensitive(d, "MQTTPulseMs");
                        if (!jp) jp = cJSON_GetObjectItemCaseSensitive(d, "mqttPulse");
                        if (jp && cJSON_IsNumber(jp) && jp->valueint > 0) pulse_ms = jp->valueint;
                        break;
                    }
                }
            }
        }
    }

    if (req_pulse_ms > 0) {
        pulse_ms = req_pulse_ms;
    }

    char final_topic[256];
    char temp[256];
    strncpy(temp, topic_pattern, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    /* Replace {base_topic}, {baseTopic}, {base}, or {root} */
    char *bt_pos = strstr(temp, "{base_topic}");
    int bt_len = 12;
    if (!bt_pos) {
        bt_pos = strstr(temp, "{baseTopic}");
        bt_len = 11;
    }
    if (!bt_pos) {
        bt_pos = strstr(temp, "{base}");
        bt_len = 6;
    }
    if (!bt_pos) {
        bt_pos = strstr(temp, "{root}");
        bt_len = 6;
    }
    if (bt_pos) {
        char buf[256];
        *bt_pos = '\0';
        snprintf(buf, sizeof(buf), "%s%s%s", temp, base_topic, bt_pos + bt_len);
        strncpy(temp, buf, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
    } else if (temp[0] == '/') {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s%s", base_topic, temp);
        strncpy(temp, buf, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
    }

    /* Replace {device_id}, {deviceId}, or {device} */
    char *d_pos = strstr(temp, "{device_id}");
    int d_len = 11;
    if (!d_pos) {
        d_pos = strstr(temp, "{deviceId}");
        d_len = 10;
    }
    if (!d_pos) {
        d_pos = strstr(temp, "{device}");
        d_len = 8;
    }
    if (!d_pos) {
        d_pos = strstr(temp, "{deviceName}");
        d_len = 12;
    }
    if (d_pos) {
        char buf[256];
        *d_pos = '\0';
        snprintf(buf, sizeof(buf), "%s%s%s", temp, dev_name, d_pos + d_len);
        strncpy(temp, buf, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
    }

    /* Replace {command} or {button} */
    char *c_pos = strstr(temp, "{command}");
    int c_len = 9;
    if (!c_pos) {
        c_pos = strstr(temp, "{button}");
        c_len = 8;
    }
    if (c_pos) {
        char buf[256];
        *c_pos = '\0';
        snprintf(buf, sizeof(buf), "%s%s%s", temp, cmd_name, c_pos + c_len);
        strncpy(final_topic, buf, sizeof(final_topic) - 1);
        final_topic[sizeof(final_topic) - 1] = '\0';
    } else {
        strncpy(final_topic, temp, sizeof(final_topic) - 1);
        final_topic[sizeof(final_topic) - 1] = '\0';
    }

    if (out_topic && out_topic_len > 0) {
        strncpy(out_topic, final_topic, out_topic_len - 1);
        out_topic[out_topic_len - 1] = '\0';
    }
    if (out_pulse_ms) {
        *out_pulse_ms = pulse_ms;
    }

    if (!mqtt_is_connected()) return -1;

    mqtt_publish(final_topic, "1", 0);
    queue_pulse_off_ex(final_topic, "0", pulse_ms);
    return 0;
}

static void format_button_topic(const char *tpl, const char *default_prefix, const char *safe_cmd, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (tpl && tpl[0]) {
        const char *pbtn = strstr(tpl, "{button}");
        if (!pbtn) pbtn = strstr(tpl, "{command}");
        if (pbtn) {
            size_t pre_len = (size_t)(pbtn - tpl);
            const char *post = pbtn + (strstr(tpl, "{button}") ? 8 : 9);
            snprintf(out, out_sz, "%.*s%s%s", (int)pre_len, tpl, safe_cmd, post);
            return;
        }
        size_t tlen = strlen(tpl);
        if (tpl[tlen - 1] == '/') {
            snprintf(out, out_sz, "%s%s", tpl, safe_cmd);
        } else {
            snprintf(out, out_sz, "%s/%s", tpl, safe_cmd);
        }
    } else {
        snprintf(out, out_sz, "%s/%s/%s", g_mqtt_base_topic, default_prefix, safe_cmd);
    }
}

int mqtt_dispatch_remote_button(const char *source, const char *action, const char *cmd_name, int slot,
                                const char *topic_in, int req_pulse_ms,
                                char *out_topic, size_t out_topic_len, int *out_pulse_ms) {
    if (!mqtt_is_connected()) return -1;
    if (!cmd_name || !cmd_name[0]) return -1;

    /* Ignore button releases to prevent duplicate pulses */
    if (action && (strcasecmp(action, "release") == 0 || strcasecmp(action, "up") == 0)) {
        return 0;
    }

    int is_rf = (source && (strcasecmp(source, "elite_rf") == 0 || strcasecmp(source, "elite") == 0 || strcasecmp(source, "rf_remote") == 0));
    int is_bt = (source && (strcasecmp(source, "bt_remote") == 0 || strcasecmp(source, "bt") == 0));

    int rf_events_enabled = g_mqtt_elite_events;
    const char *rf_custom_topic = g_mqtt_elite_topic;
    char default_rf_subpath[32] = "remotes/slot1";
    if (slot == 2) {
        if (g_mqtt_slot2_events >= 0) rf_events_enabled = g_mqtt_slot2_events;
        if (g_mqtt_slot2_topic[0]) rf_custom_topic = g_mqtt_slot2_topic;
        snprintf(default_rf_subpath, sizeof(default_rf_subpath), "remotes/slot2");
    } else {
        if (g_mqtt_slot1_events >= 0) rf_events_enabled = g_mqtt_slot1_events;
        if (g_mqtt_slot1_topic[0]) rf_custom_topic = g_mqtt_slot1_topic;
    }

    if (is_rf && !rf_events_enabled) {
        return 1; /* disabled */
    }
    if (is_bt && !g_mqtt_bt_events) {
        return 1; /* disabled */
    }

    char safe_cmd[128] = {0};
    safe_id(cmd_name, safe_cmd, sizeof(safe_cmd));

    char topic[256];
    if (topic_in && topic_in[0]) {
        strncpy(topic, topic_in, sizeof(topic) - 1);
        topic[sizeof(topic) - 1] = '\0';
    } else if (is_rf) {
        format_button_topic(rf_custom_topic, default_rf_subpath, safe_cmd, topic, sizeof(topic));
    } else if (is_bt) {
        format_button_topic(g_mqtt_bt_topic, "remotes/bt", safe_cmd, topic, sizeof(topic));
    } else {
        snprintf(topic, sizeof(topic), "%s/button/device/%s", g_mqtt_base_topic, safe_cmd);
    }

    if (out_topic && out_topic_len > 0) {
        strncpy(out_topic, topic, out_topic_len - 1);
        out_topic[out_topic_len - 1] = '\0';
    }
    if (out_pulse_ms) {
        *out_pulse_ms = (req_pulse_ms > 0) ? req_pulse_ms : 1000;
    }

    mqtt_publish(topic, "1", 0);
    queue_pulse_off_ex(topic, "0", (req_pulse_ms > 0) ? req_pulse_ms : 1000);
    return 0;
}

static int mqtt_dispatch_button_pulse(const char *device_id, const char *dev_name, const char *command,
                                      int press, char *out_topic, size_t out_len, int *out_pulse) {
    (void)press;
    int pulse_ms = 0;
    int rc = mqtt_dispatch_button_pulse_topic(device_id, dev_name, command, NULL, 0, out_topic, out_len, &pulse_ms);
    if (out_pulse) *out_pulse = (rc == 0) ? 1 : 0;
    return rc;
}

static void handle_mqtt_command_dispatch(const char *topic, const char *payload) {
    const char *prefix = "/command/";
    const char *p = strstr(topic, prefix);
    if (!p) return;
    p += strlen(prefix);

    const char *slash = strchr(p, '/');
    if (!slash) return;

    char dev[64] = {0};
    size_t dev_len = (size_t)(slash - p);
    if (dev_len >= sizeof(dev)) dev_len = sizeof(dev) - 1;
    strncpy(dev, p, dev_len);

    const char *cmd = slash + 1;
    if (!dev[0] || !cmd[0]) return;

    printf("[*] MQTT command topic dispatch: dev=%s cmd=%s payload=%s\n", dev, cmd, payload ? payload : "");
    hw_device_command_send(dev, cmd);
}

static void trigger_remote_sleep(int slot) {
    if (slot != 1 && slot != 2) return;
    printf("[*] Triggering sleep for remote slot %d via RF proxy...\n", slot);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(8092);
    inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);

    if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) == 0) {
        char req[128];
        snprintf(req, sizeof(req), "POST /api/rf/remote/%d/sleep HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n", slot);
        write(sock, req, strlen(req));
    }
    close(sock);
}

static void on_mqtt_command(const char *topic, const char *payload, size_t len, void *ud) {
    (void)len;
    (void)ud;

    char act_top[160];
    snprintf(act_top, sizeof(act_top), "%s/activity/set", g_mqtt_base_topic);

    char cmd_top[160];
    snprintf(cmd_top, sizeof(cmd_top), "%s/send_command", g_mqtt_base_topic);

    char sync_top[160];
    snprintf(sync_top, sizeof(sync_top), "%s/sync", g_mqtt_base_topic);

    char ha_status_top[160];
    snprintf(ha_status_top, sizeof(ha_status_top), "%s/status", g_mqtt_discovery_prefix);

    if (strcmp(topic, act_top) == 0) {
        handle_mqtt_activity_set(payload);
    } else if (strcmp(topic, cmd_top) == 0) {
        handle_mqtt_send_command(payload);
    } else if (strcmp(topic, sync_top) == 0) {
        printf("[*] MQTT command: Hub Sync requested\n");
        publish_mqtt_state_ex(1);
    } else if (strcmp(topic, ha_status_top) == 0) {
        if (payload && strcmp(payload, "online") == 0) {
            printf("[*] Home Assistant birth message received, rediscovering...\n");
            publish_ha_discovery_wrapper();
        }
    } else if (strcmp(topic, "harmony/ir/pronto") == 0) {
        if (payload && payload[0]) {
            printf("[*] MQTT Pronto code received\n");
            hw_ir_send_pronto(payload, IR_PORT_ALL, 1);
        }
    } else if (strstr(topic, "/remote/1/sleep")) {
        trigger_remote_sleep(1);
    } else if (strstr(topic, "/remote/2/sleep")) {
        trigger_remote_sleep(2);
    } else if (strstr(topic, "/command/")) {
        handle_mqtt_command_dispatch(topic, payload);
    }
}

static void subscribe_mqtt_topics(void) {
    char sub_buf[256];

    snprintf(sub_buf, sizeof(sub_buf), "%s/activity/set", g_mqtt_base_topic);
    mqtt_subscribe(sub_buf, on_mqtt_command, NULL);

    snprintf(sub_buf, sizeof(sub_buf), "%s/command/#", g_mqtt_base_topic);
    mqtt_subscribe(sub_buf, on_mqtt_command, NULL);

    snprintf(sub_buf, sizeof(sub_buf), "%s/command", g_mqtt_base_topic);
    mqtt_subscribe(sub_buf, on_mqtt_command, NULL);

    snprintf(sub_buf, sizeof(sub_buf), "%s/send_command", g_mqtt_base_topic);
    mqtt_subscribe(sub_buf, on_mqtt_command, NULL);

    snprintf(sub_buf, sizeof(sub_buf), "%s/sync", g_mqtt_base_topic);
    mqtt_subscribe(sub_buf, on_mqtt_command, NULL);

    snprintf(sub_buf, sizeof(sub_buf), "%s/remote/+/sleep", g_mqtt_base_topic);
    mqtt_subscribe(sub_buf, on_mqtt_command, NULL);

    snprintf(sub_buf, sizeof(sub_buf), "%s/remote/sleep", g_mqtt_base_topic);
    mqtt_subscribe(sub_buf, on_mqtt_command, NULL);

    snprintf(sub_buf, sizeof(sub_buf), "%s/status", g_mqtt_discovery_prefix);
    mqtt_subscribe(sub_buf, on_mqtt_command, NULL);

    mqtt_subscribe("harmony/ir/pronto", on_mqtt_command, NULL);
}

int daemon_mqtt_button_handler(const char *device_id, const char *dev_name, const char *command) {
    char topic[256] = {0};
    int pulse = 0;
    return mqtt_dispatch_button_pulse(device_id, dev_name, command, 0, topic, sizeof(topic), &pulse);
}

static time_t g_last_mqtt_retry = 0;
static time_t g_last_state_poll = 0;
static int g_last_mqtt_connected = 0;
static int g_mqtt_retry_interval = 5;

int hub_mqtt_init(void) {
    g_mqtt_enabled = load_mqtt_config(g_mqtt_host, &g_mqtt_port, g_mqtt_cid, g_mqtt_user, g_mqtt_pass);
    if (!g_mqtt_enabled) {
        printf("[*] MQTT disabled or config missing\n");
        return 0;
    }
    printf("[+] MQTT config loaded: %s:%d (user=%s, client=%s, base=%s)\n",
           g_mqtt_host, g_mqtt_port, g_mqtt_user[0] ? g_mqtt_user : "none", g_mqtt_cid, g_mqtt_base_topic);
    mqtt_init(g_mqtt_host, g_mqtt_port, g_mqtt_cid, g_mqtt_user, g_mqtt_pass);
    mqtt_set_lwt(g_status_topic, "offline", 1);
    mqtt_set_birth(g_status_topic, "online", 1);
    printf("[*] Connecting to MQTT broker %s:%d in background...\n", g_mqtt_host, g_mqtt_port);
    mqtt_connect_async();
    return 1;
}

void hub_mqtt_tick(void) {
    if (!g_mqtt_enabled) return;

    if (mqtt_is_connected()) {
        if (!g_last_mqtt_connected) {
            g_last_mqtt_connected = 1;
            g_mqtt_retry_interval = 5;
            printf("[+] Connected to MQTT broker %s:%d\n", g_mqtt_host, g_mqtt_port);
            publish_ha_discovery_wrapper();
            subscribe_mqtt_topics();
            publish_mqtt_state_ex(1);
        }
        mqtt_tick();

        time_t now_tick = time(NULL);
        if (now_tick - g_last_state_poll >= 30) {
            g_last_state_poll = now_tick;
            publish_mqtt_state();
        }
    } else {
        if (g_last_mqtt_connected) {
            g_last_mqtt_connected = 0;
            printf("[-] MQTT connection lost, will reconnect with backoff...\n");
        }
        time_t now = time(NULL);
        if (now - g_last_mqtt_retry >= g_mqtt_retry_interval) {
            g_last_mqtt_retry = now;
            if (!mqtt_is_connecting()) {
                mqtt_connect_async();
                g_mqtt_retry_interval = (g_mqtt_retry_interval * 2 > 60) ? 60 : (g_mqtt_retry_interval * 2);
            }
        }
    }
}

void hub_mqtt_shutdown(void) {
    if (g_mqtt_enabled) {
        mqtt_disconnect();
    }
}
