#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <fcntl.h>

#include "ir_encoder.h"
#include "ir_i2s.h"
#include "hw_action.h"
#include "orchestrator.h"
#include "ws_server.h"
#include "http_server.h"
#include "config_store.h"
#include "mqtt_client.h"
#include "codex_ntp.h"
#include "cJSON.h"

#define PID_FILE "/tmp/codex_daemon.pid"
#define CODEX_VERSION "1.3.0"

static char g_ntp_server[128] = "pool.ntp.org";
static volatile int g_running = 1;

static void handle_sig(int sig) {
    (void)sig;
    g_running = 0;
}

static void safe_id(const char *in, char *out, size_t outlen) {
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
static char g_status_topic[160] = "harmony/hub/status";
static char g_state_topic[160] = "harmony/hub/state";

/* Forward declarations */
static void publish_ha_discovery(void);
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

/* Load MQTT settings from persistent configuration */
static int load_mqtt_config(char *host, int *port, char *cid, char *user, char *pass) {
    FILE *f = fopen("/data/codexmqtt/config.json", "r");
    if (!f) return 0;
    char buf[4096] = {0};
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return 0;

    cJSON *j_en = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (!j_en || !cJSON_IsTrue(j_en)) {
        cJSON_Delete(root);
        return 0;
    }

    cJSON *broker = cJSON_GetObjectItemCaseSensitive(root, "broker");

    /* Host */
    cJSON *j_host = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (!j_host && broker) j_host = cJSON_GetObjectItemCaseSensitive(broker, "host");
    if (j_host && cJSON_IsString(j_host) && j_host->valuestring) {
        strncpy(host, j_host->valuestring, 127);
        host[127] = '\0';
        strncpy(g_mqtt_host, host, sizeof(g_mqtt_host) - 1);
        g_mqtt_host[sizeof(g_mqtt_host) - 1] = '\0';
    }

    /* Port */
    *port = 1883;
    cJSON *j_port = cJSON_GetObjectItemCaseSensitive(root, "port");
    if (!j_port && broker) j_port = cJSON_GetObjectItemCaseSensitive(broker, "port");
    if (j_port && cJSON_IsNumber(j_port)) *port = (int)j_port->valuedouble;

    /* ClientId */
    cJSON *j_cid = cJSON_GetObjectItemCaseSensitive(root, "clientId");
    if (!j_cid && broker) j_cid = cJSON_GetObjectItemCaseSensitive(broker, "clientId");
    if (j_cid && cJSON_IsString(j_cid) && j_cid->valuestring && j_cid->valuestring[0]) {
        strncpy(cid, j_cid->valuestring, 63);
        cid[63] = '\0';
    } else {
        strcpy(cid, "harmony-local-mqtt");
    }
    strncpy(g_mqtt_cid, cid, sizeof(g_mqtt_cid) - 1);

    /* Username & Password */
    cJSON *j_user = cJSON_GetObjectItemCaseSensitive(root, "username");
    if (!j_user && broker) j_user = cJSON_GetObjectItemCaseSensitive(broker, "username");
    if (j_user && cJSON_IsString(j_user) && j_user->valuestring) {
        strncpy(user, j_user->valuestring, 255);
        user[255] = '\0';
    }

    cJSON *j_pass = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!j_pass && broker) j_pass = cJSON_GetObjectItemCaseSensitive(broker, "password");
    if (j_pass && cJSON_IsString(j_pass) && j_pass->valuestring) {
        strncpy(pass, j_pass->valuestring, 255);
        pass[255] = '\0';
    }

    /* BaseTopic */
    cJSON *j_bt = cJSON_GetObjectItemCaseSensitive(root, "baseTopic");
    if (j_bt && cJSON_IsString(j_bt) && j_bt->valuestring && j_bt->valuestring[0]) {
        strncpy(g_mqtt_base_topic, j_bt->valuestring, sizeof(g_mqtt_base_topic) - 1);
    } else {
        strcpy(g_mqtt_base_topic, "harmony/hub");
    }

    /* DiscoveryPrefix */
    cJSON *j_dp = cJSON_GetObjectItemCaseSensitive(root, "discoveryPrefix");
    if (j_dp && cJSON_IsString(j_dp) && j_dp->valuestring && j_dp->valuestring[0]) {
        strncpy(g_mqtt_discovery_prefix, j_dp->valuestring, sizeof(g_mqtt_discovery_prefix) - 1);
    } else {
        strcpy(g_mqtt_discovery_prefix, "homeassistant");
    }

    /* Name */
    cJSON *j_name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (j_name && cJSON_IsString(j_name) && j_name->valuestring && j_name->valuestring[0]) {
        strncpy(g_mqtt_name, j_name->valuestring, sizeof(g_mqtt_name) - 1);
    } else {
        strcpy(g_mqtt_name, "Harmony Hub");
    }

    /* haDiscovery */
    cJSON *j_hadisc = cJSON_GetObjectItemCaseSensitive(root, "haDiscovery");
    if (j_hadisc && cJSON_IsBool(j_hadisc)) {
        g_mqtt_ha_discovery = cJSON_IsTrue(j_hadisc);
    } else {
        g_mqtt_ha_discovery = 1;
    }

    /* NTP Server */
    cJSON *j_ntp = cJSON_GetObjectItemCaseSensitive(root, "ntpServer");
    if (!j_ntp) j_ntp = cJSON_GetObjectItemCaseSensitive(root, "ntp_server");
    if (j_ntp && cJSON_IsString(j_ntp) && j_ntp->valuestring && j_ntp->valuestring[0]) {
        strncpy(g_ntp_server, j_ntp->valuestring, sizeof(g_ntp_server) - 1);
    }

    snprintf(g_status_topic, sizeof(g_status_topic), "%s/status", g_mqtt_base_topic);
    snprintf(g_state_topic, sizeof(g_state_topic), "%s/state", g_mqtt_base_topic);

    cJSON_Delete(root);
    return (host[0] != '\0');
}

/* Orchestrator Progress Callback -> Broadcast to WebSocket + MQTT */
static void on_orch_progress(const char *act_id, orch_state_t state,
                             int cur_step, int total_steps,
                             const char *desc, void *ud) {
    (void)ud;
    cJSON *pj = cJSON_CreateObject();
    if (!pj) return;
    cJSON_AddStringToObject(pj, "type", "activity_progress");
    cJSON_AddStringToObject(pj, "activity", act_id ? act_id : "");
    cJSON_AddNumberToObject(pj, "state", (int)state);
    cJSON_AddNumberToObject(pj, "step", cur_step);
    cJSON_AddNumberToObject(pj, "total", total_steps);
    cJSON_AddStringToObject(pj, "desc", desc ? desc : "");
    char *msg = cJSON_PrintUnformatted(pj);
    cJSON_Delete(pj);

    if (msg) {
        ws_broadcast_text(msg);
        char act_status_top[160];
        snprintf(act_status_top, sizeof(act_status_top), "%s/activity/status", g_mqtt_base_topic);
        mqtt_publish(act_status_top, msg, 1);
        free(msg);
    }
        if (state == ORCH_STATE_IDLE || state == ORCH_STATE_RUNNING) {
            cJSON *sj = cJSON_CreateObject();
            if (sj) {
                cJSON_AddStringToObject(sj, "type", "activity_state");
                cJSON_AddStringToObject(sj, "activity", act_id ? act_id : "");
                cJSON_AddNumberToObject(sj, "state", (int)state);
                char *smsg = cJSON_PrintUnformatted(sj);
                cJSON_Delete(sj);
                if (smsg) {
                    ws_broadcast_text(smsg);
                    free(smsg);
                }
            }
            publish_mqtt_state();
        }
    }

struct daemon_activity {
    char id[32];
    char name[64];
    int order;
};

struct daemon_activity_list {
    struct daemon_activity items[32];
    int count;
};

static int load_daemon_activities(struct daemon_activity_list *list) {
    if (!list) return -1;
    memset(list, 0, sizeof(*list));

    const char *path = "/data/codex/config/activities.json";
    if (access(path, F_OK) != 0) path = "/data/codex/activities.json";
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 1024 * 1024) {
        fclose(f);
        return -1;
    }

    char *raw = (char *)malloc((size_t)fsize + 1);
    if (!raw) {
        fclose(f);
        return -1;
    }

    size_t rd = fread(raw, 1, (size_t)fsize, f);
    fclose(f);
    raw[rd] = '\0';

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

        /* Support either direct activity object or item.Activity */
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

static void get_local_ip(char *buf, size_t buflen) {
    if (!buf || buflen == 0) return;
    buf[0] = '\0';

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct sockaddr_in target;
        memset(&target, 0, sizeof(target));
        target.sin_family = AF_INET;
        target.sin_port = htons(80);
        if (g_mqtt_host[0] && inet_pton(AF_INET, g_mqtt_host, &target.sin_addr) > 0) {
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

static void get_device_and_command_counts(int *dev_count, int *cmd_count) {
    if (dev_count) *dev_count = 0;
    if (cmd_count) *cmd_count = 0;

    const char *path = "/data/codex/config/devices.json";
    if (access(path, F_OK) != 0) path = "/data/codex/devices.json";
    FILE *f = fopen(path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 2 * 1024 * 1024) {
        fclose(f);
        return;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

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
    } else {
        cJSON *dwf = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
        if (dwf && cJSON_IsArray(dwf)) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, dwf) {
                cJSON *d = cJSON_GetObjectItemCaseSensitive(item, "Device");
                if (d && dev_count) (*dev_count)++;

                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(item, "Commands");
                if (cmds && cJSON_IsArray(cmds) && cmd_count) {
                    *cmd_count += cJSON_GetArraySize(cmds);
                }
            }
        }
    }
    cJSON_Delete(root);
}


static cJSON *create_ha_origin_obj(const char *ip) {
    cJSON *orig = cJSON_CreateObject();
    if (!orig) return NULL;
    cJSON_AddStringToObject(orig, "name", "codexmqtt");

    char sw_ver[32] = CODEX_VERSION;
    FILE *vf = fopen("/data/codex/VERSION", "r");
    if (!vf) vf = fopen("/etc/codex_version", "r");
    if (vf) {
        if (fgets(sw_ver, sizeof(sw_ver), vf)) {
            char *nl = strchr(sw_ver, '\n'); if (nl) *nl = 0;
            nl = strchr(sw_ver, '\r'); if (nl) *nl = 0;
        }
        fclose(vf);
    }
    cJSON_AddStringToObject(orig, "sw_version", sw_ver[0] ? sw_ver : CODEX_VERSION);
    char sup[96];
    if (ip && ip[0]) {
        snprintf(sup, sizeof(sup), "http://%s:8080/", ip);
    } else {
        strcpy(sup, "http://harmony-hub.local:8080/");
    }
    cJSON_AddStringToObject(orig, "support_url", sup);
    return orig;
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
            strcpy(act_name, "PowerOff");
            strcpy(actual_cur_id, "-1");
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
    get_local_ip(local_ip, sizeof(local_ip));
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
        publish_ha_discovery();
        subscribe_mqtt_topics();
        publish_mqtt_state_ex(1);
    } else {
        printf("[*] Connecting to MQTT broker in background...\n");
    }
}

static cJSON *create_ha_device_obj(const char *ident) {
    cJSON *dev = cJSON_CreateObject();
    if (!dev) return NULL;
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(ident));
    cJSON_AddItemToObject(dev, "identifiers", ids);
    cJSON_AddStringToObject(dev, "name", g_mqtt_name);
    cJSON_AddStringToObject(dev, "manufacturer", "Logitech");
    cJSON_AddStringToObject(dev, "model", "Harmony Hub");
    return dev;
}

static void publish_ha_discovery(void) {
    if (!mqtt_is_connected() || !g_mqtt_ha_discovery) return;

    char ident[64];
    safe_id(g_mqtt_cid, ident, sizeof(ident));

    struct daemon_activity_list act_list;
    load_daemon_activities(&act_list);

    char top[256];
    char uid[128];
    char cmd_top[160];
    snprintf(cmd_top, sizeof(cmd_top), "%s/activity/set", g_mqtt_base_topic);

    /* 1. select/<ident>_activity/config */
    snprintf(top, sizeof(top), "%s/select/%s_activity/config", g_mqtt_discovery_prefix, ident);
    cJSON *c1 = cJSON_CreateObject();
    if (c1) {
        cJSON_AddStringToObject(c1, "name", "Activity");
        snprintf(uid, sizeof(uid), "%s_activity", ident);
        cJSON_AddStringToObject(c1, "unique_id", uid);
        cJSON_AddStringToObject(c1, "command_topic", cmd_top);
        cJSON_AddStringToObject(c1, "state_topic", g_state_topic);
        cJSON_AddStringToObject(c1, "value_template", "{{ value_json.activity }}");
        cJSON *opts = cJSON_CreateArray();
        cJSON_AddItemToArray(opts, cJSON_CreateString("PowerOff"));
        for (int i = 0; i < act_list.count; i++) {
            cJSON_AddItemToArray(opts, cJSON_CreateString(act_list.items[i].name));
        }
        cJSON_AddItemToObject(c1, "options", opts);
        cJSON_AddStringToObject(c1, "availability_topic", g_status_topic);
        cJSON_AddStringToObject(c1, "payload_available", "online");
        cJSON_AddStringToObject(c1, "payload_not_available", "offline");
        cJSON_AddStringToObject(c1, "icon", "mdi:remote-tv");
        cJSON_AddItemToObject(c1, "device", create_ha_device_obj(ident));
        char *p1 = cJSON_PrintUnformatted(c1);
        cJSON_Delete(c1);
        if (p1) {
            mqtt_publish(top, p1, 1);
            free(p1);
        }
    }

    /* 2. sensor/<ident>_activity_id/config */
    snprintf(top, sizeof(top), "%s/sensor/%s_activity_id/config", g_mqtt_discovery_prefix, ident);
    cJSON *c2 = cJSON_CreateObject();
    if (c2) {
        cJSON_AddStringToObject(c2, "name", "Activity ID");
        snprintf(uid, sizeof(uid), "%s_activity_id", ident);
        cJSON_AddStringToObject(c2, "unique_id", uid);
        cJSON_AddStringToObject(c2, "state_topic", g_state_topic);
        cJSON_AddStringToObject(c2, "value_template", "{{ value_json.activityId }}");
        cJSON_AddStringToObject(c2, "availability_topic", g_status_topic);
        cJSON_AddStringToObject(c2, "payload_available", "online");
        cJSON_AddStringToObject(c2, "payload_not_available", "offline");
        cJSON_AddStringToObject(c2, "icon", "mdi:identifier");
        cJSON_AddStringToObject(c2, "entity_category", "diagnostic");
        cJSON_AddItemToObject(c2, "device", create_ha_device_obj(ident));
        char *p2 = cJSON_PrintUnformatted(c2);
        cJSON_Delete(c2);
        if (p2) {
            mqtt_publish(top, p2, 1);
            free(p2);
        }
    }

    char local_ip[64] = {0};
    get_local_ip(local_ip, sizeof(local_ip));

    /* 3. sensor/<ident>_ip/config */
    snprintf(top, sizeof(top), "%s/sensor/%s_ip/config", g_mqtt_discovery_prefix, ident);
    cJSON *c_ip = cJSON_CreateObject();
    if (c_ip) {
        cJSON_AddStringToObject(c_ip, "name", "IP Address");
        snprintf(uid, sizeof(uid), "%s_ip", ident);
        cJSON_AddStringToObject(c_ip, "unique_id", uid);
        cJSON_AddStringToObject(c_ip, "state_topic", g_state_topic);
        cJSON_AddStringToObject(c_ip, "value_template", "{{ value_json.ip }}");
        cJSON_AddStringToObject(c_ip, "json_attributes_topic", g_state_topic);
        cJSON_AddStringToObject(c_ip, "availability_topic", g_status_topic);
        cJSON_AddStringToObject(c_ip, "payload_available", "online");
        cJSON_AddStringToObject(c_ip, "payload_not_available", "offline");
        cJSON_AddStringToObject(c_ip, "icon", "mdi:ip-network");
        cJSON_AddStringToObject(c_ip, "entity_category", "diagnostic");
        cJSON_AddItemToObject(c_ip, "origin", create_ha_origin_obj(local_ip));
        cJSON_AddItemToObject(c_ip, "device", create_ha_device_obj(ident));
        char *p_ip = cJSON_PrintUnformatted(c_ip);
        cJSON_Delete(c_ip);
        if (p_ip) {
            mqtt_publish(top, p_ip, 1);
            free(p_ip);
        }
    }

    /* 4. sensor/<ident>_firmware/config */
    snprintf(top, sizeof(top), "%s/sensor/%s_firmware/config", g_mqtt_discovery_prefix, ident);
    cJSON *c_fw = cJSON_CreateObject();
    if (c_fw) {
        cJSON_AddStringToObject(c_fw, "name", "Firmware");
        snprintf(uid, sizeof(uid), "%s_firmware", ident);
        cJSON_AddStringToObject(c_fw, "unique_id", uid);
        cJSON_AddStringToObject(c_fw, "state_topic", g_state_topic);
        cJSON_AddStringToObject(c_fw, "value_template", "{{ value_json.firmware }}");
        cJSON_AddStringToObject(c_fw, "availability_topic", g_status_topic);
        cJSON_AddStringToObject(c_fw, "payload_available", "online");
        cJSON_AddStringToObject(c_fw, "payload_not_available", "offline");
        cJSON_AddStringToObject(c_fw, "icon", "mdi:chip");
        cJSON_AddStringToObject(c_fw, "entity_category", "diagnostic");
        cJSON_AddItemToObject(c_fw, "origin", create_ha_origin_obj(local_ip));
        cJSON_AddItemToObject(c_fw, "device", create_ha_device_obj(ident));
        char *p_fw = cJSON_PrintUnformatted(c_fw);
        cJSON_Delete(c_fw);
        if (p_fw) {
            mqtt_publish(top, p_fw, 1);
            free(p_fw);
        }
    }

    /* 5. sensor/<ident>_ir_devices/config */
    snprintf(top, sizeof(top), "%s/sensor/%s_ir_devices/config", g_mqtt_discovery_prefix, ident);
    cJSON *c_devs = cJSON_CreateObject();
    if (c_devs) {
        cJSON_AddStringToObject(c_devs, "name", "IR Devices");
        snprintf(uid, sizeof(uid), "%s_ir_devices", ident);
        cJSON_AddStringToObject(c_devs, "unique_id", uid);
        cJSON_AddStringToObject(c_devs, "state_topic", g_state_topic);
        cJSON_AddStringToObject(c_devs, "value_template", "{{ value_json.deviceCount }}");
        cJSON_AddStringToObject(c_devs, "availability_topic", g_status_topic);
        cJSON_AddStringToObject(c_devs, "payload_available", "online");
        cJSON_AddStringToObject(c_devs, "payload_not_available", "offline");
        cJSON_AddStringToObject(c_devs, "icon", "mdi:remote");
        cJSON_AddStringToObject(c_devs, "entity_category", "diagnostic");
        cJSON_AddItemToObject(c_devs, "origin", create_ha_origin_obj(local_ip));
        cJSON_AddItemToObject(c_devs, "device", create_ha_device_obj(ident));
        char *p_devs = cJSON_PrintUnformatted(c_devs);
        cJSON_Delete(c_devs);
        if (p_devs) {
            mqtt_publish(top, p_devs, 1);
            free(p_devs);
        }
    }

    /* 6. sensor/<ident>_ir_commands/config */
    snprintf(top, sizeof(top), "%s/sensor/%s_ir_commands/config", g_mqtt_discovery_prefix, ident);
    cJSON *c_cmds = cJSON_CreateObject();
    if (c_cmds) {
        cJSON_AddStringToObject(c_cmds, "name", "IR Commands");
        snprintf(uid, sizeof(uid), "%s_ir_commands", ident);
        cJSON_AddStringToObject(c_cmds, "unique_id", uid);
        cJSON_AddStringToObject(c_cmds, "state_topic", g_state_topic);
        cJSON_AddStringToObject(c_cmds, "value_template", "{{ value_json.commandCount }}");
        cJSON_AddStringToObject(c_cmds, "availability_topic", g_status_topic);
        cJSON_AddStringToObject(c_cmds, "payload_available", "online");
        cJSON_AddStringToObject(c_cmds, "payload_not_available", "offline");
        cJSON_AddStringToObject(c_cmds, "icon", "mdi:counter");
        cJSON_AddStringToObject(c_cmds, "entity_category", "diagnostic");
        cJSON_AddItemToObject(c_cmds, "origin", create_ha_origin_obj(local_ip));
        cJSON_AddItemToObject(c_cmds, "device", create_ha_device_obj(ident));
        char *p_cmds = cJSON_PrintUnformatted(c_cmds);
        cJSON_Delete(c_cmds);
        if (p_cmds) {
            mqtt_publish(top, p_cmds, 1);
            free(p_cmds);
        }
    }

    /* 7. button/<ident>_power_off/config */
    snprintf(top, sizeof(top), "%s/button/%s_power_off/config", g_mqtt_discovery_prefix, ident);
    cJSON *c3 = cJSON_CreateObject();
    if (c3) {
        cJSON_AddStringToObject(c3, "name", "Power Off");
        snprintf(uid, sizeof(uid), "%s_power_off", ident);
        cJSON_AddStringToObject(c3, "unique_id", uid);
        cJSON_AddStringToObject(c3, "command_topic", cmd_top);
        cJSON_AddStringToObject(c3, "payload_press", "PowerOff");
        cJSON_AddStringToObject(c3, "availability_topic", g_status_topic);
        cJSON_AddStringToObject(c3, "payload_available", "online");
        cJSON_AddStringToObject(c3, "payload_not_available", "offline");
        cJSON_AddStringToObject(c3, "icon", "mdi:power");
        cJSON_AddItemToObject(c3, "device", create_ha_device_obj(ident));
        char *p3 = cJSON_PrintUnformatted(c3);
        cJSON_Delete(c3);
        if (p3) {
            mqtt_publish(top, p3, 1);
            free(p3);
        }
    }

    /* 8. button/<ident>_sync/config */
    snprintf(top, sizeof(top), "%s/button/%s_sync/config", g_mqtt_discovery_prefix, ident);
    cJSON *c4 = cJSON_CreateObject();
    if (c4) {
        cJSON_AddStringToObject(c4, "name", "Sync");
        snprintf(uid, sizeof(uid), "%s_sync", ident);
        cJSON_AddStringToObject(c4, "unique_id", uid);
        char sync_top[160];
        snprintf(sync_top, sizeof(sync_top), "%s/sync", g_mqtt_base_topic);
        cJSON_AddStringToObject(c4, "command_topic", sync_top);
        cJSON_AddStringToObject(c4, "payload_press", "PRESS");
        cJSON_AddStringToObject(c4, "availability_topic", g_status_topic);
        cJSON_AddStringToObject(c4, "payload_available", "online");
        cJSON_AddStringToObject(c4, "payload_not_available", "offline");
        cJSON_AddStringToObject(c4, "icon", "mdi:sync");
        cJSON_AddStringToObject(c4, "entity_category", "config");
        cJSON_AddItemToObject(c4, "device", create_ha_device_obj(ident));
        char *p4 = cJSON_PrintUnformatted(c4);
        cJSON_Delete(c4);
        if (p4) {
            mqtt_publish(top, p4, 1);
            free(p4);
        }
    }

    publish_mqtt_state_ex(1);
}

static void handle_mqtt_activity_set(const char *payload) {
    if (!payload || !payload[0]) return;

    char trimmed[128];
    strncpy(trimmed, payload, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';

    char *start = trimmed;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    char *end = start + strlen(start);
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t' || *(end - 1) == '\r' || *(end - 1) == '\n')) end--;
    *end = '\0';

    if (start[0] == '{') {
        cJSON *root = cJSON_Parse(start);
        if (root) {
            cJSON *j_act = cJSON_GetObjectItemCaseSensitive(root, "activityId");
            if (!j_act) j_act = cJSON_GetObjectItemCaseSensitive(root, "activity");
            if (!j_act) j_act = cJSON_GetObjectItemCaseSensitive(root, "id");
            if (!j_act) j_act = cJSON_GetObjectItemCaseSensitive(root, "name");
            if (j_act) {
                if (cJSON_IsString(j_act) && j_act->valuestring) {
                    strncpy(trimmed, j_act->valuestring, sizeof(trimmed) - 1);
                    trimmed[sizeof(trimmed) - 1] = '\0';
                    start = trimmed;
                } else if (cJSON_IsNumber(j_act)) {
                    snprintf(trimmed, sizeof(trimmed), "%ld", (long)j_act->valuedouble);
                    start = trimmed;
                }
            }
            cJSON_Delete(root);
        }
    }

    printf("[*] MQTT request: switch to activity '%s'\n", start);

    static time_t s_last_activity_cmd_time = 0;
    static char s_last_activity_cmd[64] = {0};

    time_t now = time(NULL);
    if (strcmp(start, s_last_activity_cmd) == 0 && (now - s_last_activity_cmd_time) < 2) {
        printf("[!] MQTT: duplicate activity '%s' received within %ld sec; dropping\n",
               start, (long)(now - s_last_activity_cmd_time));
        return;
    }
    s_last_activity_cmd_time = now;
    snprintf(s_last_activity_cmd, sizeof(s_last_activity_cmd), "%s", start);

    if (strcmp(start, "PowerOff") == 0 || strcasecmp(start, "poweroff") == 0 ||
        strcmp(start, "-1") == 0 || strcasecmp(start, "off") == 0 || strcmp(start, "0") == 0) {
        int rc = orch_power_off(0, NULL, 0);
        if (rc == -2) {
            printf("[!] MQTT: transition already in progress, PowerOff request dropped\n");
        }
        return;
    }

    struct daemon_activity_list act_list;
    if (load_daemon_activities(&act_list) != 0 || act_list.count == 0) {
        printf("[-] MQTT rejected: failed to load ActivityList.json\n");
        return;
    }

    const char *target_id = NULL;
    for (int i = 0; i < act_list.count; i++) {
        if (strcmp(start, act_list.items[i].id) == 0) {
            target_id = act_list.items[i].id;
            break;
        }
        if (strcasecmp(start, act_list.items[i].name) == 0) {
            target_id = act_list.items[i].id;
            break;
        }
    }

    if (!target_id) {
        printf("[-] MQTT rejected: unknown activity '%s' (not in ActivityList)\n", start);
        return;
    }

    printf("[+] MQTT validated activity '%s' -> ID %s, starting...\n", start, target_id);
    int rc = orch_switch_activity(target_id, 0, NULL, 0);
    if (rc == -2) {
        printf("[!] MQTT: transition already in progress, switch request dropped\n");
    }
}

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

static void mqtt_cmd_tick(void) {
    if (!g_mqtt_seq.active) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;

    if (now_ms < g_mqtt_seq.next_exec_ms) return;

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
        return;
    }

    uint32_t wait_ms = g_mqtt_seq.hold_ms + g_mqtt_seq.delay_ms;
    g_mqtt_seq.next_exec_ms = now_ms + wait_ms;
}

static void handle_mqtt_send_command(const char *payload) {
    if (!payload || payload[0] != '{') return;

    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        printf("[-] MQTT send_command: invalid JSON payload\n");
        return;
    }

    cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
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
        printf("[-] MQTT send_command: missing device in payload\n");
        cJSON_Delete(root);
        return;
    }

    /* Extract command list: string or array */
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

    double delay_secs = -1.0;
    cJSON *j_delay = cJSON_GetObjectItemCaseSensitive(root, "delay_secs");
    if (!j_delay) j_delay = cJSON_GetObjectItemCaseSensitive(root, "delaySecs");
    if (!j_delay) j_delay = cJSON_GetObjectItemCaseSensitive(root, "delay");
    if (j_delay && cJSON_IsNumber(j_delay)) delay_secs = j_delay->valuedouble;

    double hold_secs = 0.0;
    cJSON *j_hold = cJSON_GetObjectItemCaseSensitive(root, "hold_secs");
    if (!j_hold) j_hold = cJSON_GetObjectItemCaseSensitive(root, "holdSecs");
    if (!j_hold) j_hold = cJSON_GetObjectItemCaseSensitive(root, "hold");
    if (j_hold && cJSON_IsNumber(j_hold)) hold_secs = j_hold->valuedouble;

    int num_repeats = 1;
    cJSON *j_rep = cJSON_GetObjectItemCaseSensitive(root, "num_repeats");
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

    printf("[*] MQTT send_command: dev='%s' cmds=%d repeats=%d delay=%.2fs hold=%.2fs\n",
           dev, cmd_count, num_repeats, delay_secs, hold_secs);

    if (cmd_count == 1 && num_repeats == 1 && hold_secs <= 0.001) {
        printf("[*] Executing command [1/1] (repeat 1/1): dev='%s' cmd='%s'\n", dev, cmd_list[0]);
        hw_device_command_send(dev, cmd_list[0]);
        return;
    }

    strncpy(g_mqtt_seq.dev, dev, sizeof(g_mqtt_seq.dev) - 1);
    g_mqtt_seq.dev[sizeof(g_mqtt_seq.dev) - 1] = '\0';
    g_mqtt_seq.cmd_count = cmd_count;
    for (int i = 0; i < cmd_count; i++) {
        strncpy(g_mqtt_seq.cmd_list[i], cmd_list[i], sizeof(g_mqtt_seq.cmd_list[0]) - 1);
        g_mqtt_seq.cmd_list[i][sizeof(g_mqtt_seq.cmd_list[0]) - 1] = '\0';
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

static void handle_mqtt_command_dispatch(const char *topic, const char *payload) {
    char sub_prefix[160];
    snprintf(sub_prefix, sizeof(sub_prefix), "%s/command/", g_mqtt_base_topic);
    size_t plen = strlen(sub_prefix);
    if (strncmp(topic, sub_prefix, plen) == 0) {
        const char *rest = topic + plen;
        const char *slash = strchr(rest, '/');
        if (slash) {
            char dev[64] = {0};
            char cmd[64] = {0};
            size_t dlen = slash - rest;
            if (dlen >= sizeof(dev)) dlen = sizeof(dev) - 1;
            strncpy(dev, rest, dlen);
            strncpy(cmd, slash + 1, sizeof(cmd) - 1);
            printf("[*] MQTT direct command dispatch: dev='%s' cmd='%s'\n", dev, cmd);
            hw_device_command_send(dev, cmd);
            return;
        }
    }

    if (payload && payload[0] == '{') {
        handle_mqtt_send_command(payload);
    }
}

/* MQTT Incoming Message Handler */
static void on_mqtt_command(const char *topic, const char *payload, size_t len, void *ud) {
    (void)ud;
    (void)len;
    if (!topic) return;

    char act_set_topic[160];
    snprintf(act_set_topic, sizeof(act_set_topic), "%s/activity/set", g_mqtt_base_topic);
    char sync_topic[160];
    snprintf(sync_topic, sizeof(sync_topic), "%s/sync", g_mqtt_base_topic);
    char ha_status_topic[160];
    snprintf(ha_status_topic, sizeof(ha_status_topic), "%s/status", g_mqtt_discovery_prefix);

    if (strcmp(topic, act_set_topic) == 0) {
        handle_mqtt_activity_set(payload);
    } else if (strcmp(topic, sync_topic) == 0) {
        printf("[*] MQTT sync requested: re-publishing discovery & state\n");
        publish_ha_discovery();
    } else if (strcmp(topic, ha_status_topic) == 0) {
        if (payload && strcmp(payload, "online") == 0) {
            printf("[*] Home Assistant birth message received: re-publishing discovery & state\n");
            publish_ha_discovery();
        }
    } else if (strcmp(topic, "harmony/ir/pronto") == 0) {
        printf("[*] MQTT request: direct Pronto IR blast\n");
        hw_ir_send_pronto(payload, IR_PORT_ALL, 3);
    } else {
        handle_mqtt_command_dispatch(topic, payload);
    }
}

static void subscribe_mqtt_topics(void) {
    char sub_buf[160];
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

    snprintf(sub_buf, sizeof(sub_buf), "%s/status", g_mqtt_discovery_prefix);
    mqtt_subscribe(sub_buf, on_mqtt_command, NULL);

    mqtt_subscribe("harmony/ir/pronto", on_mqtt_command, NULL);
}

static void write_pid(void) {
    FILE *f = fopen(PID_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", (int)getpid());
        fclose(f);
    }
}

static int daemon_mqtt_button_handler(const char *device_id, const char *dev_name, const char *command) {
    char topic[256] = {0};
    int pulse = 0;
    return mqtt_dispatch_button_pulse(device_id, dev_name, command, 0, topic, sizeof(topic), &pulse);
}

static void handle_ws_client_message(int cfd) {
    char buf[4096];
    int opcode = 0;
    int len = ws_read_frame(cfd, buf, sizeof(buf), &opcode);
    if (len <= 0 || opcode == WS_OP_CLOSE) {
        close(cfd);
        ws_remove_client(cfd);
        return;
    }

    if (opcode == WS_OP_PING) {
        ws_send_pong(cfd);
        return;
    }

    if (opcode != WS_OP_TEXT) return;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;

    cJSON *act_j = cJSON_GetObjectItemCaseSensitive(root, "action");
    const char *action = (act_j && cJSON_IsString(act_j)) ? act_j->valuestring : "";

    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(root, "msgId");
    int msg_id = (id_j && cJSON_IsNumber(id_j)) ? id_j->valueint : 0;

    if (strcmp(action, "ping") == 0) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "pong");
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        cJSON_AddStringToObject(resp, "activity", orch_get_current_activity());
        cJSON_AddNumberToObject(resp, "state", (int)orch_get_state());
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "get_state") == 0) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "activity_state");
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        cJSON_AddStringToObject(resp, "activity", orch_get_current_activity());
        cJSON_AddNumberToObject(resp, "state", (int)orch_get_state());
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "send_command") == 0 || strcmp(action, "ir_send") == 0) {
        cJSON *dev_j = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
        cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(root, "command");
        const char *dev = (dev_j && cJSON_IsString(dev_j)) ? dev_j->valuestring : "";
        const char *cmd = (cmd_j && cJSON_IsString(cmd_j)) ? cmd_j->valuestring : "";

        int ok = 0;
        if (dev[0] && cmd[0]) {
            ok = (hw_device_command_send(dev, cmd) == 0);
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", action);
        cJSON_AddBoolToObject(resp, "ok", ok);
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "start_activity") == 0) {
        cJSON *target_j = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (!target_j) target_j = cJSON_GetObjectItemCaseSensitive(root, "activity");
        const char *target = (target_j && cJSON_IsString(target_j)) ? target_j->valuestring : "";

        int rc = -1;
        if (target[0]) {
            rc = orch_switch_activity(target, 0, NULL, 0);
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", "start_activity");
        cJSON_AddBoolToObject(resp, "ok", rc == 0);
        if (rc != 0) cJSON_AddStringToObject(resp, "error", "Interlock busy");
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "stop_activity") == 0 || strcmp(action, "power_off") == 0) {
        int rc = orch_power_off(0, NULL, 0);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", "stop_activity");
        cJSON_AddBoolToObject(resp, "ok", rc == 0);
        if (rc != 0) cJSON_AddStringToObject(resp, "error", "Interlock busy");
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "cancel_activity") == 0 || strcmp(action, "reset_interlock") == 0) {
        orch_cancel();
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", action);
        cJSON_AddBoolToObject(resp, "ok", 1);
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "bt_key") == 0) {
        cJSON *k_j = cJSON_GetObjectItemCaseSensitive(root, "key");
        cJSON *a_j = cJSON_GetObjectItemCaseSensitive(root, "keyAction");
        cJSON *tgt_j = cJSON_GetObjectItemCaseSensitive(root, "target");
        if (!tgt_j) tgt_j = cJSON_GetObjectItemCaseSensitive(root, "bdaddr");
        const char *k = (k_j && cJSON_IsString(k_j)) ? k_j->valuestring : "";
        const char *ka = (a_j && cJSON_IsString(a_j)) ? a_j->valuestring : "tap";
        const char *tgt = (tgt_j && cJSON_IsString(tgt_j)) ? tgt_j->valuestring : "";
        int ok = 0;
        if (k[0]) {
            char line[160];
            if (strcasecmp(k, "release_all") == 0 || strcasecmp(ka, "up") == 0 || strcasecmp(ka, "release") == 0) {
                if (strcasecmp(k, "release_all") == 0) snprintf(line, sizeof(line), "RELEASE\n");
                else snprintf(line, sizeof(line), "KEYUP %s\n", k);
            } else if (strcasecmp(ka, "down") == 0 || strcasecmp(ka, "press") == 0) {
                snprintf(line, sizeof(line), "KEYDOWN %s\n", k);
            } else {
                snprintf(line, sizeof(line), "KEY %s\n", k);
            }
            int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
            if (bfd >= 0) {
                if (tgt[0]) {
                    char thdr[64];
                    snprintf(thdr, sizeof(thdr), "TARGET %s\n", tgt);
                    write(bfd, thdr, strlen(thdr));
                }
                if (write(bfd, line, strlen(line)) > 0) ok = 1;
                close(bfd);
            }
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", "bt_key");
        cJSON_AddBoolToObject(resp, "ok", ok);
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "bt_text") == 0) {
        cJSON *t_j = cJSON_GetObjectItemCaseSensitive(root, "text");
        cJSON *tgt_j = cJSON_GetObjectItemCaseSensitive(root, "target");
        if (!tgt_j) tgt_j = cJSON_GetObjectItemCaseSensitive(root, "bdaddr");
        const char *t = (t_j && cJSON_IsString(t_j)) ? t_j->valuestring : "";
        const char *tgt = (tgt_j && cJSON_IsString(tgt_j)) ? tgt_j->valuestring : "";
        int ok = 0;
        if (t[0]) {
            int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
            if (bfd >= 0) {
                if (tgt[0]) {
                    char thdr[64];
                    snprintf(thdr, sizeof(thdr), "TARGET %s\n", tgt);
                    write(bfd, thdr, strlen(thdr));
                }
                char prefix[] = "TEXT ";
                write(bfd, prefix, strlen(prefix));
                if (write(bfd, t, strlen(t)) > 0) {
                    write(bfd, "\n", 1);
                    ok = 1;
                }
                close(bfd);
            }
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", "bt_text");
        cJSON_AddBoolToObject(resp, "ok", ok);
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }

    cJSON_Delete(root);
}

int main(int argc, char **argv) {
    int port = 8089;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            g_dry_run = 1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (isdigit((unsigned char)argv[i][0])) {
            port = atoi(argv[i]);
        }
    }
    if (port <= 0) port = 8089;

    signal(SIGTERM, handle_sig);
    signal(SIGINT, handle_sig);
    signal(SIGPIPE, SIG_IGN);

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    write_pid();

    printf("=========================================\n");
    printf("   codex_daemon — Unified Harmony Hub   \n");
    printf("=========================================\n");

    /* 1. Hardware, Config Store & Orchestrator Core */
    config_store_init();
    struct codex_settings settings;
    config_store_get_settings(&settings);
    if (settings.dry_run) g_dry_run = 1;
    if (g_dry_run) {
        printf("[!] DRY-RUN MODE ACTIVE: All physical IR, BT, and MQTT outputs will be simulated\n");
    }

    orch_init();
    ir_i2s_init();
    orch_set_progress_callback(on_orch_progress, NULL);
    hw_set_mqtt_button_handler(daemon_mqtt_button_handler);
    printf("[+] Hardware core & Activity Orchestrator initialized (current: %s)\n",
           orch_get_current_activity());

    /* 2. Web & WebSocket Server */
    ws_server_init();
    int srv_fd = http_server_init(port);
    if (srv_fd < 0) {
        fprintf(stderr, "[-] Could not bind HTTP server on port %d: %s\n", port, strerror(errno));
        return 1;
    }
    printf("[+] HTTP & WebSocket server listening on port %d\n", port);

    /* 3. MQTT Client */
    g_mqtt_enabled = load_mqtt_config(g_mqtt_host, &g_mqtt_port, g_mqtt_cid, g_mqtt_user, g_mqtt_pass);

    if (g_mqtt_enabled) {
        printf("[+] MQTT config loaded: %s:%d (user=%s, client=%s, base=%s)\n",
               g_mqtt_host, g_mqtt_port, g_mqtt_user[0] ? g_mqtt_user : "none", g_mqtt_cid, g_mqtt_base_topic);
        mqtt_init(g_mqtt_host, g_mqtt_port, g_mqtt_cid, g_mqtt_user, g_mqtt_pass);

        /* Configure LWT and Birth messages */
        mqtt_set_lwt(g_status_topic, "offline", 1);
        mqtt_set_birth(g_status_topic, "online", 1);

        if (mqtt_connect() == 0) {
            printf("[+] Connected to MQTT broker %s:%d\n", g_mqtt_host, g_mqtt_port);
            publish_ha_discovery();
            subscribe_mqtt_topics();
            publish_mqtt_state_ex(1);
        } else {
            printf("[*] Connecting to MQTT broker in background...\n");
        }
    } else {
        printf("[*] MQTT disabled or config missing\n");
    }

    printf("[+] Entering master non-blocking event loop...\n");

    /* Initial SNTP network time sync attempt */
    sntp_sync_time(g_ntp_server, 1);

    time_t last_mqtt_retry = 0;
    time_t last_state_poll = 0;

    while (g_running) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(srv_fd, &rset);
        int max_fd = srv_fd;

        int client_fds[WS_MAX_CLIENTS];
        int client_count = ws_get_clients(client_fds, WS_MAX_CLIENTS);
        for (int i = 0; i < client_count; i++) {
            FD_SET(client_fds[i], &rset);
            if (client_fds[i] > max_fd) max_fd = client_fds[i];
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 40000; /* 40 ms tick */

        int nready = select(max_fd + 1, &rset, NULL, NULL, &tv);
        if (nready > 0) {
            if (FD_ISSET(srv_fd, &rset)) {
                struct sockaddr_in caddr;
                socklen_t clen = sizeof(caddr);
                int cfd = accept(srv_fd, (struct sockaddr *)&caddr, &clen);
                if (cfd >= 0) {
                    struct timeval to;
                    to.tv_sec = 1;
                    to.tv_usec = 0;
                    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
                    to.tv_sec = 2;
                    to.tv_usec = 0;
                    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
                    int nodelay = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    http_handle_client(cfd);
                }
            }

            for (int i = 0; i < client_count; i++) {
                int cfd = client_fds[i];
                if (FD_ISSET(cfd, &rset)) {
                    handle_ws_client_message(cfd);
                }
            }
        }

        /* Tick Activity Orchestrator state machine & delays */
        orch_tick();
        pulse_tick();
        mqtt_cmd_tick();

        /* Tick SNTP network time sync */
        sntp_tick(g_ntp_server);

        /* Tick MQTT client packet reader, keepalive, and periodic state poll */
        if (g_mqtt_enabled) {
            if (mqtt_is_connected()) {
                mqtt_tick();

                time_t now_tick = time(NULL);
                if (now_tick - last_state_poll >= 30) {
                    last_state_poll = now_tick;
                    publish_mqtt_state();
                }
            } else {
                time_t now = time(NULL);
                if (now - last_mqtt_retry >= 10) {
                    last_mqtt_retry = now;
                    if (mqtt_connect() == 0) {
                        printf("[+] Reconnected to MQTT broker %s:%d\n", g_mqtt_host, g_mqtt_port);
                        publish_ha_discovery();
                        subscribe_mqtt_topics();
                        publish_mqtt_state_ex(1);
                    }
                }
            }
        }
    }

    printf("\n[*] Shutting down codex_daemon...\n");
    orch_cancel();
    ir_i2s_shutdown();
    ws_close_all();
    http_server_close(srv_fd);
    if (g_mqtt_enabled) mqtt_disconnect();
    unlink(PID_FILE);

    printf("[+] Shutdown complete.\n");
    return 0;
}
