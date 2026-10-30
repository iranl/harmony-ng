#define _GNU_SOURCE
#include "rf_keymap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static cJSON *g_remote_map_cache[2] = {NULL, NULL};
static time_t g_remote_map_mtime[2] = {0, 0};
pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;

void get_current_activity(char *out, size_t outlen) {
    FILE *f = fopen(CURRENT_ACT_FILE, "r");
    if (f) {
        if (fgets(out, outlen, f)) {
            fclose(f);
            char *p = out + strlen(out) - 1;
            while (p >= out && (*p == '\r' || *p == '\n' || *p == ' ')) *p-- = '\0';
            if (out[0]) return;
        } else {
            fclose(f);
        }
    }
    snprintf(out, outlen, "-1");
}

static cJSON *get_remote_map_unlocked(uint8_t slot) {
    if (slot < 1 || slot > 2) slot = 1;
    int idx = slot - 1;

    char slot_path[64];
    snprintf(slot_path, sizeof(slot_path), "/data/harmony/remote_slot%d_map.json", slot);

    const char *map_file = slot_path;
    struct stat st;
    if (stat(map_file, &st) != 0) return NULL;

    if (st.st_mtime != g_remote_map_mtime[idx] || !g_remote_map_cache[idx]) {
        if (g_remote_map_cache[idx]) {
            cJSON_Delete(g_remote_map_cache[idx]);
            g_remote_map_cache[idx] = NULL;
        }
        FILE *mf = fopen(map_file, "r");
        if (mf) {
            fseek(mf, 0, SEEK_END);
            long sz = ftell(mf);
            fseek(mf, 0, SEEK_SET);
            if (sz > 0 && sz < 500000) {
                char *buf = malloc(sz + 1);
                if (buf) {
                    size_t n = fread(buf, 1, sz, mf);
                    buf[n] = '\0';
                    g_remote_map_cache[idx] = cJSON_Parse(buf);
                    free(buf);
                }
            }
            fclose(mf);
        }
        g_remote_map_mtime[idx] = st.st_mtime;
        printf("[+] (Re)loaded remote slot %d map cache (%s, size=%ld bytes)\n", slot, map_file, (long)st.st_size);
    }
    return g_remote_map_cache[idx];
}

cJSON *get_remote_map(uint8_t slot) {
    pthread_mutex_lock(&g_map_lock);
    cJSON *res = get_remote_map_unlocked(slot);
    pthread_mutex_unlock(&g_map_lock);
    return res;
}

bool get_remote_screen_button(uint8_t slot, const char *act_or_dev, int btn_idx, char *out_dev, size_t dev_sz, char *out_cmd, size_t cmd_sz) {
    if (!act_or_dev || !out_dev || !out_cmd || dev_sz == 0 || cmd_sz == 0) return false;
    out_dev[0] = '\0';
    out_cmd[0] = '\0';
    bool handled = false;

    pthread_mutex_lock(&g_map_lock);
    cJSON *map = get_remote_map_unlocked(slot);
    if (map) {
        cJSON *acts = cJSON_GetObjectItemCaseSensitive(map, "activities");
        if (acts && cJSON_IsObject(acts)) {
            cJSON *act_obj = cJSON_GetObjectItemCaseSensitive(acts, act_or_dev);
            if (!act_obj && strcmp(act_or_dev, "-1") == 0) {
                act_obj = cJSON_GetObjectItemCaseSensitive(acts, "-1");
            }
            if (act_obj && cJSON_IsObject(act_obj)) {
                cJSON *sbtns = cJSON_GetObjectItemCaseSensitive(act_obj, "screen_buttons");
                if (sbtns && cJSON_IsArray(sbtns)) {
                    cJSON *item = cJSON_GetArrayItem(sbtns, btn_idx);
                    if (item && cJSON_IsObject(item)) {
                        cJSON *jdev = cJSON_GetObjectItemCaseSensitive(item, "deviceId");
                        cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(item, "command");
                        if (jdev && jdev->valuestring && jcmd && jcmd->valuestring) {
                            strncpy(out_dev, jdev->valuestring, dev_sz - 1);
                            strncpy(out_cmd, jcmd->valuestring, cmd_sz - 1);
                            handled = true;
                        }
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&g_map_lock);
    return handled;
}

cJSON *get_elite_map(void) {
    return get_remote_map(1);
}

int is_slot_direct_mode(uint8_t slot) {
    (void)slot;
    return 0;
}

cJSON *get_elite_direct_map(int slot) {
    if (slot < 1 || slot > 2) slot = 1;
    char path[128];
    snprintf(path, sizeof(path), "/data/harmony/elite_slot%d_direct_buttons.json", slot);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 500000) { fclose(f); return NULL; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0';
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

void dispatch_elite_button(uint8_t slot, const char *button_name, const char *action_state, const char *dev_id) {
    int is_press = (strcmp(action_state, "release") != 0);

    /* Forward raw button event to daemon for MQTT (fire and forget) */
    char mqtt_body[256];
    if (dev_id && dev_id[0]) {
        snprintf(mqtt_body, sizeof(mqtt_body),
                 "{\"command\":\"%s\",\"action\":\"%s\",\"source\":\"elite_rf\",\"slot\":%d,\"deviceId\":\"%s\"}",
                 button_name, action_state, slot, dev_id);
    } else {
        snprintf(mqtt_body, sizeof(mqtt_body),
                 "{\"command\":\"%s\",\"action\":\"%s\",\"source\":\"elite_rf\",\"slot\":%d}",
                 button_name, action_state, slot);
    }
    http_post_local(DAEMON_PORT, "/api/button-press", mqtt_body);

    if (!is_press) return;

    if (strcmp(button_name, "PowerOffActivity") == 0) {
        printf("[*] RF Dispatch: PowerOffActivity -> Stopping activity\n");
        http_post_local(DAEMON_PORT, "/api/poweroff", "{}");
        return;
    }

    /* If button pressed while browsing a specific device, dispatch to that device */
    if (dev_id && dev_id[0]) {
        cJSON *map = get_remote_map(slot);
        if (map) {
            pthread_mutex_lock(&g_map_lock);
            cJSON *devs_map = cJSON_GetObjectItemCaseSensitive(map, "devices");
            if (devs_map && cJSON_IsObject(devs_map)) {
                cJSON *d_obj = cJSON_GetObjectItemCaseSensitive(devs_map, dev_id);
                if (d_obj && cJSON_IsObject(d_obj)) {
                    cJSON *b_cfg = cJSON_GetObjectItemCaseSensitive(d_obj, button_name);
                    if (b_cfg && cJSON_IsObject(b_cfg)) {
                        cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(b_cfg, "command");
                        if (j_cmd && cJSON_IsString(j_cmd) && j_cmd->valuestring[0]) {
                            char body[256];
                            snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"command\":\"%s\"}", dev_id, j_cmd->valuestring);
                            printf("[*] RF Dispatch Device Button Map: %s -> dev=%s cmd=%s\n", button_name, dev_id, j_cmd->valuestring);
                            http_post_local(WEBUI_PORT, "/api/ir-send", body);
                            pthread_mutex_unlock(&g_map_lock);
                            return;
                        }
                    }
                }
            }
            pthread_mutex_unlock(&g_map_lock);
        }

        /* Default fallback: auto-match button_name against device commands in devices.json */
        FILE *fd = fopen("/data/harmony/config/devices.json", "r");
        if (fd) {
            fseek(fd, 0, SEEK_END);
            long sz = ftell(fd);
            fseek(fd, 0, SEEK_SET);
            char *buf = (sz > 0 && sz < 1000000) ? malloc(sz + 1) : NULL;
            if (buf) {
                size_t n = fread(buf, 1, sz, fd);
                buf[n] = '\0';
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *devs_arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "devices");
                    cJSON *d = NULL;
                    const char *matched_cmd = NULL;
                    cJSON_ArrayForEach(d, devs_arr) {
                        cJSON *jid = cJSON_GetObjectItem(d, "id");
                        if (jid && cJSON_IsString(jid) && strcmp(jid->valuestring, dev_id) == 0) {
                            cJSON *cmds = cJSON_GetObjectItem(d, "commands");
                            cJSON *c = NULL;
                            cJSON_ArrayForEach(c, cmds) {
                                cJSON *jn = cJSON_IsString(c) ? c : cJSON_GetObjectItem(c, "name");
                                if (jn && cJSON_IsString(jn) && strcasecmp(jn->valuestring, button_name) == 0) {
                                    matched_cmd = jn->valuestring;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                    if (matched_cmd) {
                        char body[256];
                        snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"command\":\"%s\"}", dev_id, matched_cmd);
                        printf("[*] RF Dispatch Device Button Match: %s -> dev=%s cmd=%s\n", button_name, dev_id, matched_cmd);
                        http_post_local(WEBUI_PORT, "/api/ir-send", body);
                        cJSON_Delete(root);
                        fclose(fd);
                        return;
                    }
                    cJSON_Delete(root);
                }
            }
            fclose(fd);
        }
        return;
    }

    /* Standard Activity-Based mapping */
    cJSON *map = get_remote_map(slot);
    if (!map) return;

    char act_id[32] = "-1";
    get_current_activity(act_id, sizeof(act_id));

    char act_type_buf[32] = {0};
    char dev_id_buf[64] = {0};
    char cmd_buf[64] = {0};
    char target_act_id_buf[64] = {0};

    pthread_mutex_lock(&g_map_lock);
    cJSON *acts = cJSON_GetObjectItemCaseSensitive(map, "activities");
    if (!acts || !cJSON_IsObject(acts)) acts = map;

    cJSON *btn_cfg = NULL;
    if (act_id[0] && strcmp(act_id, "-1") != 0) {
        cJSON *act_obj = cJSON_GetObjectItemCaseSensitive(acts, act_id);
        if (act_obj && cJSON_IsObject(act_obj)) {
            btn_cfg = cJSON_GetObjectItemCaseSensitive(act_obj, button_name);
        }
    }
    if (!btn_cfg) {
        cJSON *def_obj = cJSON_GetObjectItemCaseSensitive(acts, "-1");
        if (!def_obj) def_obj = cJSON_GetObjectItemCaseSensitive(acts, "default");
        if (!def_obj) def_obj = cJSON_GetObjectItemCaseSensitive(acts, "53591842");
        if (!def_obj && acts->child) def_obj = acts->child;
        if (def_obj && cJSON_IsObject(def_obj)) {
            btn_cfg = cJSON_GetObjectItemCaseSensitive(def_obj, button_name);
        }
    }

    if (btn_cfg && cJSON_IsObject(btn_cfg)) {
        cJSON *j_act = cJSON_GetObjectItemCaseSensitive(btn_cfg, "action");
        if (!j_act) j_act = cJSON_GetObjectItemCaseSensitive(btn_cfg, "type");
        if (j_act && cJSON_IsString(j_act)) strncpy(act_type_buf, j_act->valuestring, sizeof(act_type_buf) - 1);

        cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(btn_cfg, "targetDevice");
        if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(btn_cfg, "deviceId");
        if (j_dev && cJSON_IsString(j_dev)) strncpy(dev_id_buf, j_dev->valuestring, sizeof(dev_id_buf) - 1);

        cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(btn_cfg, "command");
        if (j_cmd && cJSON_IsString(j_cmd)) strncpy(cmd_buf, j_cmd->valuestring, sizeof(cmd_buf) - 1);

        cJSON *j_target_act = cJSON_GetObjectItemCaseSensitive(btn_cfg, "activityId");
        if (!j_target_act) j_target_act = cJSON_GetObjectItemCaseSensitive(btn_cfg, "targetActivity");
        if (j_target_act && cJSON_IsString(j_target_act)) {
            strncpy(target_act_id_buf, j_target_act->valuestring, sizeof(target_act_id_buf) - 1);
        } else if (dev_id_buf[0]) {
            strncpy(target_act_id_buf, dev_id_buf, sizeof(target_act_id_buf) - 1);
        }
    }
    pthread_mutex_unlock(&g_map_lock);

    int is_cmd = (strcmp(act_type_buf, "device_cmd") == 0 || strcmp(act_type_buf, "command") == 0 ||
                  (act_type_buf[0] == '\0' && dev_id_buf[0] && cmd_buf[0]));
    if (is_cmd && dev_id_buf[0] && cmd_buf[0]) {
        char body[256];
        snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"command\":\"%s\"}", dev_id_buf, cmd_buf);
        printf("[*] RF Dispatch: IR/BT send -> dev=%s cmd=%s\n", dev_id_buf, cmd_buf);
        http_post_local(WEBUI_PORT, "/api/ir-send", body);
    } else if ((strcmp(act_type_buf, "activity_start") == 0 || strcmp(act_type_buf, "change_activity") == 0) && target_act_id_buf[0]) {
        char body[64];
        snprintf(body, sizeof(body), "{\"activity\":\"%s\"}", target_act_id_buf);
        printf("[*] RF Dispatch: Start activity -> id=%s\n", target_act_id_buf);
        http_post_local(DAEMON_PORT, "/api/activity", body);
    } else if (strcmp(act_type_buf, "activity_stop") == 0 || strcmp(act_type_buf, "poweroff") == 0) {
        printf("[*] RF Dispatch: Stop activity (PowerOff)\n");
        http_post_local(DAEMON_PORT, "/api/poweroff", "{}");
    }
}

typedef enum {
    RF_EV_NONE = 0,
    RF_EV_BUTTON,
    RF_EV_DEVICE_CMD,
    RF_EV_ACTIVITY
} rf_ev_kind_t;

typedef struct {
    rf_ev_kind_t kind;
    uint8_t slot;
    char param1[64];
    char param2[64];
    char param3[16];
} rf_event_t;

#define RF_QUEUE_SIZE 64
static rf_event_t g_rf_queue[RF_QUEUE_SIZE];
static int g_rf_q_head = 0;
static int g_rf_q_tail = 0;
static pthread_mutex_t g_rf_q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rf_q_cond = PTHREAD_COND_INITIALIZER;

void enqueue_elite_button(uint8_t slot, const char *name, const char *action, const char *dev_id) {
    pthread_mutex_lock(&g_rf_q_mutex);
    if (action && strcmp(action, "release") == 0) {
        /* On button release, purge any pending queued repeats for this button to stop runaway execution */
        int purged = 0;
        int i = g_rf_q_tail;
        while (i != g_rf_q_head) {
            if (g_rf_queue[i].kind == RF_EV_BUTTON && g_rf_queue[i].slot == slot &&
                strcmp(g_rf_queue[i].param1, name ? name : "") == 0) {
                g_rf_queue[i].kind = RF_EV_NONE;
                purged++;
            }
            i = (i + 1) % RF_QUEUE_SIZE;
        }
        if (purged > 0) {
            printf("[*] Purged %d queued repeated presses on release of %s\n", purged, name ? name : "");
        }
    }
    int next = (g_rf_q_head + 1) % RF_QUEUE_SIZE;
    if (next != g_rf_q_tail) {
        g_rf_queue[g_rf_q_head].kind = RF_EV_BUTTON;
        g_rf_queue[g_rf_q_head].slot = slot;
        strncpy(g_rf_queue[g_rf_q_head].param1, name ? name : "", sizeof(g_rf_queue[0].param1) - 1);
        g_rf_queue[g_rf_q_head].param1[sizeof(g_rf_queue[0].param1) - 1] = '\0';
        strncpy(g_rf_queue[g_rf_q_head].param2, action ? action : "", sizeof(g_rf_queue[0].param2) - 1);
        g_rf_queue[g_rf_q_head].param2[sizeof(g_rf_queue[0].param2) - 1] = '\0';
        strncpy(g_rf_queue[g_rf_q_head].param3, dev_id ? dev_id : "", sizeof(g_rf_queue[0].param3) - 1);
        g_rf_queue[g_rf_q_head].param3[sizeof(g_rf_queue[0].param3) - 1] = '\0';
        g_rf_q_head = next;
        pthread_cond_signal(&g_rf_q_cond);
    } else {
        printf("[-] Warning: RF dispatch queue overflow!\n");
    }
    pthread_mutex_unlock(&g_rf_q_mutex);
}

void enqueue_elite_device_cmd(const char *dev_id, const char *cmd) {
    if (!dev_id || !cmd) return;
    pthread_mutex_lock(&g_rf_q_mutex);
    int next = (g_rf_q_head + 1) % RF_QUEUE_SIZE;
    if (next != g_rf_q_tail) {
        g_rf_queue[g_rf_q_head].kind = RF_EV_DEVICE_CMD;
        g_rf_queue[g_rf_q_head].slot = 1;
        strncpy(g_rf_queue[g_rf_q_head].param1, dev_id, sizeof(g_rf_queue[0].param1) - 1);
        g_rf_queue[g_rf_q_head].param1[sizeof(g_rf_queue[0].param1) - 1] = '\0';
        strncpy(g_rf_queue[g_rf_q_head].param2, cmd, sizeof(g_rf_queue[0].param2) - 1);
        g_rf_queue[g_rf_q_head].param2[sizeof(g_rf_queue[0].param2) - 1] = '\0';
        g_rf_q_head = next;
        pthread_cond_signal(&g_rf_q_cond);
    } else {
        printf("[-] Warning: RF dispatch queue overflow on dev_cmd!\n");
    }
    pthread_mutex_unlock(&g_rf_q_mutex);
}

void enqueue_elite_activity(const char *act_id) {
    if (!act_id) return;
    pthread_mutex_lock(&g_rf_q_mutex);
    int next = (g_rf_q_head + 1) % RF_QUEUE_SIZE;
    if (next != g_rf_q_tail) {
        g_rf_queue[g_rf_q_head].kind = RF_EV_ACTIVITY;
        g_rf_queue[g_rf_q_head].slot = 1;
        strncpy(g_rf_queue[g_rf_q_head].param1, act_id, sizeof(g_rf_queue[0].param1) - 1);
        g_rf_queue[g_rf_q_head].param1[sizeof(g_rf_queue[0].param1) - 1] = '\0';
        g_rf_queue[g_rf_q_head].param2[0] = '\0';
        g_rf_q_head = next;
        pthread_cond_signal(&g_rf_q_cond);
    } else {
        printf("[-] Warning: RF dispatch queue overflow on activity!\n");
    }
    pthread_mutex_unlock(&g_rf_q_mutex);
}

static struct {
    uint16_t current_key;
    char current_name[32];
    uint64_t press_start_ms;
    uint64_t last_repeat_ms;
    bool hold_sent;
} s_companion_state[MAX_PAIRED_REMOTES];

void check_companion_timeouts(void) {
    uint64_t now_ms = get_monotonic_ms();
    for (int s = 0; s < MAX_PAIRED_REMOTES; s++) {
        if (s_companion_state[s].current_key != 0) {
            /* Trigger HOLD when held longer than 300ms */
            if (!s_companion_state[s].hold_sent && (now_ms - s_companion_state[s].press_start_ms >= 300)) {
                s_companion_state[s].hold_sent = true;
                printf("[*] Companion button HOLD: slot=%d code=0x%04X name=%s (held %llums)\n",
                       s + 1, s_companion_state[s].current_key, s_companion_state[s].current_name,
                       (unsigned long long)(now_ms - s_companion_state[s].press_start_ms));
                enqueue_elite_button(s + 1, s_companion_state[s].current_name, "hold", NULL);
            }
            /* Safety timeout: only release if link lost or stuck for >30 seconds */
            else if (now_ms - s_companion_state[s].press_start_ms > 30000) {
                printf("[*] Companion button AUTO-RELEASE (safety timeout): slot=%d name=%s\n",
                       s + 1, s_companion_state[s].current_name);
                enqueue_elite_button(s + 1, s_companion_state[s].current_name, "release", NULL);
                s_companion_state[s].current_key = 0;
                s_companion_state[s].current_name[0] = '\0';
                s_companion_state[s].hold_sent = false;
            }
        }
    }
}

void handle_companion_packet(const uint8_t *pkt, size_t len) {
    if (len < 5) return;
    uint8_t slot = pkt[1] & 0x0F;
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) slot = 1;
    mark_remote_slot_active(slot);
    if (g_paired_remotes[slot - 1].model != REMOTE_MODEL_SMART_CONTROL &&
        g_paired_remotes[slot - 1].model != REMOTE_MODEL_COMPANION) {
        g_paired_remotes[slot - 1].model = REMOTE_MODEL_COMPANION;
    }

    uint8_t sub_id = pkt[2];
    uint16_t key_code = 0;

    if (sub_id == 0x03) {
        /* Consumer Control / Media / Activity */
        key_code = ((uint16_t)pkt[4] << 8) | pkt[3];
    } else if (sub_id == 0x01) {
        /* Keyboard */
        key_code = pkt[4];
    } else {
        printf("[*] Companion RX (slot=%d sub=0x%02X unknown): len=%zu\n", slot, sub_id, len);
        return;
    }

    int sidx = slot - 1;
    uint64_t now_ms = get_monotonic_ms();

    printf("[*] Companion RX (slot=%d sub=0x%02X): raw=[%02X %02X %02X %02X %02X %02X %02X] -> key=0x%04X\n",
           slot, sub_id, pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], (len > 5 ? pkt[5] : 0), (len > 6 ? pkt[6] : 0), key_code);

    if (key_code == 0) {
        if (s_companion_state[sidx].current_key != 0) {
            uint64_t held_ms = now_ms - s_companion_state[sidx].press_start_ms;
            printf("[*] Companion button RELEASE: slot=%d name=%s (held %llums, hold_sent=%d)\n",
                   slot, s_companion_state[sidx].current_name, (unsigned long long)held_ms, s_companion_state[sidx].hold_sent);
            enqueue_elite_button(slot, s_companion_state[sidx].current_name, "release", NULL);
            s_companion_state[sidx].current_key = 0;
            s_companion_state[sidx].current_name[0] = '\0';
            s_companion_state[sidx].hold_sent = false;
        }
        return;
    }

    const char *name = map_button_code(key_code >> 8, key_code & 0xFF);
    if (!name) name = "Unknown";

    if (s_companion_state[sidx].current_key != key_code) {
        if (s_companion_state[sidx].current_key != 0) {
            enqueue_elite_button(slot, s_companion_state[sidx].current_name, "release", NULL);
        }
        s_companion_state[sidx].current_key = key_code;
        strncpy(s_companion_state[sidx].current_name, name, sizeof(s_companion_state[sidx].current_name) - 1);
        s_companion_state[sidx].current_name[sizeof(s_companion_state[sidx].current_name) - 1] = '\0';
        s_companion_state[sidx].press_start_ms = now_ms;
        s_companion_state[sidx].last_repeat_ms = now_ms;
        s_companion_state[sidx].hold_sent = false;

        printf("[*] Companion button PRESS: slot=%d code=0x%04X name=%s\n", slot, key_code, name);
        enqueue_elite_button(slot, name, "press", NULL);
    } else {
        s_companion_state[sidx].last_repeat_ms = now_ms;
        if (!s_companion_state[sidx].hold_sent && (now_ms - s_companion_state[sidx].press_start_ms >= 300)) {
            s_companion_state[sidx].hold_sent = true;
            printf("[*] Companion button HOLD: slot=%d code=0x%04X name=%s (held %llums)\n",
                   slot, key_code, name, (unsigned long long)(now_ms - s_companion_state[sidx].press_start_ms));
            enqueue_elite_button(slot, name, "hold", NULL);
        }
    }
}

void *dispatch_worker(void *arg) {
    (void)arg;
    while (g_running) {
        check_companion_timeouts();

        rf_event_t ev;
        bool has_event = false;

        pthread_mutex_lock(&g_rf_q_mutex);
        if (g_rf_q_head != g_rf_q_tail) {
            ev = g_rf_queue[g_rf_q_tail];
            g_rf_q_tail = (g_rf_q_tail + 1) % RF_QUEUE_SIZE;
            has_event = true;
        }
        pthread_mutex_unlock(&g_rf_q_mutex);

        if (!has_event) {
            usleep(20000);
            continue;
        }

        if (ev.kind == RF_EV_NONE) {
            continue;
        } else if (ev.kind == RF_EV_DEVICE_CMD) {
            char body[256];
            snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"command\":\"%s\"}",
                     ev.param1, ev.param2);
            http_post_local(WEBUI_PORT, "/api/ir-send", body);
        } else if (ev.kind == RF_EV_ACTIVITY) {
            dispatch_activity_to_daemon(ev.param1);
        } else {
            dispatch_elite_button(ev.slot, ev.param1, ev.param2, ev.param3);
        }
    }
    return NULL;
}

void dispatch_activity_to_daemon(const char *act_id) {
    if (strcmp(act_id, "-1") == 0) {
        printf("[*] RF Dispatching Activity Stop (PowerOff) to Hub\n");
        http_post_local(DAEMON_PORT, "/api/poweroff", "{}");
    } else {
        printf("[*] RF Dispatching Activity Start: %s to Hub\n", act_id);
        char body[64];
        snprintf(body, sizeof(body), "{\"activity\":\"%s\"}", act_id);
        http_post_local(DAEMON_PORT, "/api/activity", body);
    }
}

void dispatch_device_command_to_webui(const char *payload) {
    char buf[128] = {0};
    strncpy(buf, payload, sizeof(buf) - 1);
    char *colon = strchr(buf, ':');
    if (!colon) return;
    *colon = '\0';
    const char *dev = buf;
    const char *cmd = colon + 1;
    char body[256];
    snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"command\":\"%s\"}", dev, cmd);
    printf("[*] RF Dispatch Device Cmd: dev=%s cmd=%s\n", dev, cmd);
    http_post_local(WEBUI_PORT, "/api/ir-send", body);
}

const char *map_button_code(uint8_t k0, uint8_t k1) {
    uint16_t key = ((uint16_t)k0 << 8) | k1;
    switch (key) {
        case 0x01EC: return "PowerOffActivity";
        case 0x01ED: return "WatchTVActivity";
        case 0x01E8: return "MusicActivity";
        case 0x01E9: return "MovieActivity";
        case 0x004B: return "UpArrow";
        case 0x004E: return "DownArrow";
        case 0x0052: return "DirectionUp";
        case 0x0051: return "DirectionDown";
        case 0x0050: return "DirectionLeft";
        case 0x004F: return "DirectionRight";
        case 0x0058: return "Select";
        case 0x0028: return "NumberEnter";
        case 0x0046: return "Back";
        case 0x0225: return "Back";
        case 0x0065: return "Menu";
        case 0x0094: return "Exit";
        case 0x01FF: return "Info";
        case 0x008D: return "Guide";
        case 0x0089: return "Live";
        case 0x009A: return "Dvr";
        case 0x00E9: return "VolumeUp";
        case 0x00EA: return "VolumeDown";
        case 0x00E2: return "VolumeMute";
        case 0x009C: return "ChannelUp";
        case 0x009D: return "ChannelDown";
        case 0x0224: return "Back";
        case 0x00B0: return "Play";
        case 0x00B1: return "Pause";
        case 0x00B2: return "Record";
        case 0x00B3: return "FastForward";
        case 0x00B4: return "Rewind";
        case 0x00B7: return "Stop";
        case 0x01F7: return "Red";
        case 0x01F6: return "Green";
        case 0x01F5: return "Yellow";
        case 0x01F4: return "Blue";
        case 0x001E: return "Number1";
        case 0x001F: return "Number2";
        case 0x0020: return "Number3";
        case 0x0021: return "Number4";
        case 0x0022: return "Number5";
        case 0x0023: return "Number6";
        case 0x0024: return "Number7";
        case 0x0025: return "Number8";
        case 0x0026: return "Number9";
        case 0x0027: return "Number0";
        case 0x0056: return "NumberPlus";
        case 0x0FF2: return "Ha1";
        case 0x0FF3: return "Ha2";
        case 0x0FF4: return "Ha3";
        case 0x0FF5: return "Ha4";
        case 0x0FF0: return "RockerUp";
        case 0x0FF1: return "RockerDown";
        default: return NULL;
    }
}

void get_activity_name_by_id(const char *act_id, char *out_name, size_t out_len) {
    if (!act_id || strcmp(act_id, "-1") == 0) {
        snprintf(out_name, out_len, "PowerOff");
        return;
    }
    FILE *f = fopen("/data/harmony/config/activities.json", "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000) {
            char *buf = malloc(sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, sz, f);
                buf[n] = '\0';
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *acts_arr = cJSON_GetObjectItem(root, "activities");
                    if (!acts_arr) acts_arr = cJSON_GetObjectItem(root, "Activities");
                    if (acts_arr && cJSON_IsArray(acts_arr)) {
                        cJSON *elem = NULL;
                        cJSON_ArrayForEach(elem, acts_arr) {
                            cJSON *j_name = cJSON_GetObjectItem(elem, "name");
                            if (!j_name) j_name = cJSON_GetObjectItem(elem, "Name");
                            cJSON *j_id = cJSON_GetObjectItem(elem, "id");
                            if (!j_id) j_id = cJSON_GetObjectItem(elem, "Id");
                            if (!j_id) j_id = cJSON_GetObjectItem(elem, "Id-");
                            if (j_name && cJSON_IsString(j_name) && j_id) {
                                char id_str[32] = {0};
                                if (cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%d", j_id->valueint);
                                else if (cJSON_IsString(j_id)) strncpy(id_str, j_id->valuestring, sizeof(id_str) - 1);
                                if (strcmp(id_str, act_id) == 0) {
                                    strncpy(out_name, j_name->valuestring, out_len - 1);
                                    out_name[out_len - 1] = '\0';
                                    cJSON_Delete(root);
                                    fclose(f);
                                    return;
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(f);
    }
    strncpy(out_name, act_id, out_len - 1);
    out_name[out_len - 1] = '\0';
}
