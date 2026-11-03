#define _GNU_SOURCE
#include "hw_action.h"
#include "resource_cache.h"
#include "ir_encoder.h"
#include "ir_i2s.h"
#include "config_store.h"
#include "bt_common.h"
#include "ble_adv_payload.h"
#include "mantra_codec.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

static hw_mqtt_button_handler_t g_mqtt_button_handler = NULL;

/* Every Mantra broadcast gets a new counter, as the app's do; the counter also seeds the whitening */
static pthread_mutex_t s_mantra_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t s_mantra_counter = 0;

/* The fan sets brightness and colour temperature together, so each device's last values are kept */
#define MANTRA_STATE_MAX 16
static struct {
    char device_id[64];
    int br_pct;
    int ct_pct;
} s_mantra_state[MANTRA_STATE_MAX];
static int s_mantra_state_count = 0;

static uint16_t mantra_next_counter(void) {
    pthread_mutex_lock(&s_mantra_lock);
    if (s_mantra_counter == 0) s_mantra_counter = (uint16_t)(time(NULL) ^ getpid());
    uint16_t c = ++s_mantra_counter;
    pthread_mutex_unlock(&s_mantra_lock);
    return c;
}

static int mantra_send_adv(const uint8_t *adv, int len, int ms) {
    char cmd[16 + 2 * MANTRA_ADV_LEN];
    int n = snprintf(cmd, sizeof(cmd), "adv ");
    for (int i = 0; i < len; i++) n += snprintf(cmd + n, sizeof(cmd) - n, "%02X", adv[i]);
    if (ms > 0) n += snprintf(cmd + n, sizeof(cmd) - n, " %d", ms);
    snprintf(cmd + n, sizeof(cmd) - n, "\n");
    return hub_send_btstack_cmd(cmd);
}

/* Reads the device's mantraId (hex string) and mantraVersion. Returns 0, or -2 if the id is missing or invalid. */
static int mantra_device_params(const cJSON *dev, uint16_t *id, int *version) {
    const cJSON *j_id = cJSON_GetObjectItemCaseSensitive(dev, "mantraId");
    if (!j_id || !cJSON_IsString(j_id) || mantra_parse_id(j_id->valuestring, id) != 0) return -2;
    const cJSON *j_ver = cJSON_GetObjectItemCaseSensitive(dev, "mantraVersion");
    *version = (j_ver && cJSON_IsNumber(j_ver) && j_ver->valueint == 1) ? 1 : 0;
    return 0;
}

static int mantra_send_command(const cJSON *dev, const char *device_id, const char *action) {
    uint16_t id;
    int version;
    if (mantra_device_params(dev, &id, &version) != 0) return -2;

    pthread_mutex_lock(&s_mantra_lock);
    int idx = -1;
    for (int i = 0; i < s_mantra_state_count; i++) {
        if (strcmp(s_mantra_state[i].device_id, device_id) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        idx = (s_mantra_state_count < MANTRA_STATE_MAX) ? s_mantra_state_count++ : MANTRA_STATE_MAX - 1;
        snprintf(s_mantra_state[idx].device_id, sizeof(s_mantra_state[idx].device_id), "%s", device_id);
        s_mantra_state[idx].br_pct = 100;
        s_mantra_state[idx].ct_pct = 50;
    }
    uint8_t cmd7[7];
    int rc = mantra_build_command(version, action, &s_mantra_state[idx].br_pct, &s_mantra_state[idx].ct_pct, cmd7);
    pthread_mutex_unlock(&s_mantra_lock);
    if (rc != 0) return -2;

    uint8_t adv[MANTRA_ADV_LEN];
    int len = mantra_encode_adv(version, id, mantra_next_counter(), cmd7, adv);
    return mantra_send_adv(adv, len, 0);
}

void hw_set_mqtt_button_handler(hw_mqtt_button_handler_t handler) {
    g_mqtt_button_handler = handler;
}


static char *build_button_press_json(const char *device_id, const char *dev_name, const char *command, int action) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddStringToObject(j, "deviceId", device_id ? device_id : "");
    cJSON_AddStringToObject(j, "device", (dev_name && dev_name[0]) ? dev_name : "Dummy");
    cJSON_AddStringToObject(j, "command", command ? command : "");
    const char *act_str = (action == HW_BTN_ACTION_RELEASE) ? "release" : ((action == HW_BTN_ACTION_REPEAT) ? "hold" : "press");
    cJSON_AddStringToObject(j, "action", act_str);
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return out;
}

int g_dry_run = 0;

int hw_ir_send_pronto(const char *pronto_hex, uint8_t ports, uint8_t repeats) {
    if (!pronto_hex || !pronto_hex[0]) return -1;
    if (ports == 0) return 0; /* 0 = No blasters / IR disabled */

    uint8_t *bin = NULL;
    size_t bin_len = 0;
    int rc = ir_encode_pronto(pronto_hex, repeats ? repeats : 3, 500, &bin, &bin_len);
    if (rc != 0 || !bin || bin_len == 0) {
        return -2;
    }

    uint8_t port_mask = ports;
    if (g_dry_run) {
        printf("[DRY-RUN] IR Pronto simulated: %zu bytes, port_mask 0x%02x\n", bin_len, port_mask);
        ir_free_encoded(bin);
        return 0;
    }
    int ret = ir_i2s_blast_blob(bin, bin_len, port_mask);

    ir_free_encoded(bin);
    return ret;
}

int hw_ir_send_raw(uint32_t freq_hz, const uint32_t *timings_us, size_t count, uint8_t ports, uint8_t repeats) {
    if (!timings_us || count == 0) return -1;
    if (ports == 0) return 0; /* 0 = No blasters / IR disabled */

    uint8_t *bin = NULL;
    size_t bin_len = 0;
    int rc = ir_encode_raw(freq_hz, timings_us, count, repeats ? repeats : 3, &bin, &bin_len);
    if (rc != 0 || !bin || bin_len == 0) {
        return -2;
    }

    uint8_t port_mask = ports;
    if (g_dry_run) {
        printf("[DRY-RUN] IR Raw simulated: %zu timings (%u Hz), port_mask 0x%02x\n", count, freq_hz, port_mask);
        ir_free_encoded(bin);
        return 0;
    }
    int ret = ir_i2s_blast_blob(bin, bin_len, port_mask);

    ir_free_encoded(bin);
    return ret;
}

int hw_ir_cancel(uint8_t ports) {
    (void)ports;
    return 0;
}

int hw_ir_send_harmony_keycode(const char *keycode, uint8_t ports, uint8_t repeats) {
    if (!keycode || !keycode[0]) return -1;
    if (ports == 0) return 0; /* 0 = No blasters / IR disabled */

    uint8_t *bin = NULL;
    size_t bin_len = 0;
    int rc = ir_encode_harmony_keycode(keycode, repeats ? repeats : 3, &bin, &bin_len);
    if (rc != 0 || !bin || bin_len == 0) {
        return -2;
    }

    uint8_t port_mask = ports;
    if (g_dry_run) {
        printf("[DRY-RUN] IR Keycode simulated: %s (%zu bytes), port_mask 0x%02x\n", keycode, bin_len, port_mask);
        ir_free_encoded(bin);
        return 0;
    }
    int ret = ir_i2s_blast_blob(bin, bin_len, port_mask);

    ir_free_encoded(bin);
    return ret;
}

static int is_bt_device_connected(const char *target_bdaddr) {
    FILE *f = fopen("/tmp/bthid_status", "r");
    if (!f) return 0;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (!strstr(buf, "\"state\":\"listening\"")) return 0;
    if (target_bdaddr && target_bdaddr[0]) {
        return (strcasestr(buf, target_bdaddr) != NULL);
    }
    return 1;
}

static void check_and_trigger_bt_reconnect_for_command(cJSON *devs, const char *executed_dev_id, const char *executed_dev_name, const char *executed_cmd) {
    if (!devs || !executed_cmd || !executed_cmd[0]) return;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devs) {
        cJSON *dev = item;

        int is_bt = 0;
        cJSON *j_type = cJSON_GetObjectItemCaseSensitive(dev, "type");
        if (j_type && cJSON_IsString(j_type) && j_type->valuestring) {
            if (strcasecmp(j_type->valuestring, "BT") == 0 ||
                strcasecmp(j_type->valuestring, "Bluetooth") == 0 ||
                strcasecmp(j_type->valuestring, "BTHID") == 0 ||
                strcasestr(j_type->valuestring, "bluetooth") != NULL) {
                is_bt = 1;
            }
        }

        cJSON *j_bta = cJSON_GetObjectItemCaseSensitive(dev, "bdaddr");
        if (!j_bta) j_bta = cJSON_GetObjectItemCaseSensitive(dev, "btHost");
        if (j_bta && cJSON_IsString(j_bta) && j_bta->valuestring && hub_valid_bdaddr(j_bta->valuestring)) {
            is_bt = 1;
        }

        if (cJSON_GetObjectItemCaseSensitive(dev, "btAutoReconnect") ||
            cJSON_GetObjectItemCaseSensitive(dev, "btTriggerCommandName")) {
            is_bt = 1;
        }

        if (!is_bt) continue;

        cJSON *j_rec = cJSON_GetObjectItemCaseSensitive(dev, "btAutoReconnect");
        if (j_rec) {
            int rec_val = cJSON_IsNumber(j_rec) ? (int)j_rec->valuedouble : (cJSON_IsBool(j_rec) ? (cJSON_IsTrue(j_rec) ? 1 : 0) : (j_rec->valuestring ? atoi(j_rec->valuestring) : 1));
            if (rec_val == 0) continue;
        }

        char cur_id[64] = {0};
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(dev, "id");
        if (jid) {
            if (cJSON_IsNumber(jid)) snprintf(cur_id, sizeof(cur_id), "%ld", (long)jid->valuedouble);
            else if (cJSON_IsString(jid) && jid->valuestring) strncpy(cur_id, jid->valuestring, sizeof(cur_id) - 1);
        }

        char cur_name[64] = {0};
        cJSON *jnm = cJSON_GetObjectItemCaseSensitive(dev, "name");
        if (jnm && cJSON_IsString(jnm) && jnm->valuestring) {
            strncpy(cur_name, jnm->valuestring, sizeof(cur_name) - 1);
        }

        int target_dev_delay = -1;
        cJSON *jd_dly = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDelaySec");
        if (jd_dly) {
            if (cJSON_IsNumber(jd_dly) && jd_dly->valuedouble >= 0) target_dev_delay = (int)jd_dly->valuedouble;
            else if (cJSON_IsString(jd_dly) && jd_dly->valuestring && jd_dly->valuestring[0]) target_dev_delay = atoi(jd_dly->valuestring);
        }

        int dur = 0;
        cJSON *jd_dur = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDurationSec");
        if (jd_dur) {
            if (cJSON_IsNumber(jd_dur) && jd_dur->valuedouble > 0) dur = (int)jd_dur->valuedouble;
            else if (cJSON_IsString(jd_dur) && jd_dur->valuestring) dur = atoi(jd_dur->valuestring);
        }
        if (dur <= 0) dur = 30;

        char trig_did_buf[64] = {0};
        cJSON *j_trig_dev = cJSON_GetObjectItemCaseSensitive(dev, "btTriggerDeviceId");
        if (j_trig_dev) {
            if (cJSON_IsString(j_trig_dev) && j_trig_dev->valuestring) {
                strncpy(trig_did_buf, j_trig_dev->valuestring, sizeof(trig_did_buf) - 1);
            } else if (cJSON_IsNumber(j_trig_dev)) {
                snprintf(trig_did_buf, sizeof(trig_did_buf), "%ld", (long)j_trig_dev->valuedouble);
            }
        }
        const char *trig_did = trig_did_buf;

        char trig_cmd_buf[64] = {0};
        cJSON *j_trig_cmd = cJSON_GetObjectItemCaseSensitive(dev, "btTriggerCommandName");
        if (j_trig_cmd) {
            if (cJSON_IsString(j_trig_cmd) && j_trig_cmd->valuestring) {
                strncpy(trig_cmd_buf, j_trig_cmd->valuestring, sizeof(trig_cmd_buf) - 1);
            } else if (cJSON_IsNumber(j_trig_cmd)) {
                snprintf(trig_cmd_buf, sizeof(trig_cmd_buf), "%ld", (long)j_trig_cmd->valuedouble);
            }
        }
        const char *trig_cmd = trig_cmd_buf;

        /* If colon is in trig_cmd (e.g. "deviceId:PowerOn" or "MQTT:PowerOn"), separate them */
        const char *colon = strchr(trig_cmd, ':');
        if (colon) {
            if (!trig_did[0]) {
                size_t did_len = (size_t)(colon - trig_cmd);
                if (did_len >= sizeof(trig_did_buf)) did_len = sizeof(trig_did_buf) - 1;
                strncpy(trig_did_buf, trig_cmd, did_len);
                trig_did_buf[did_len] = '\0';
                trig_did = trig_did_buf;
            }
            trig_cmd = colon + 1;
        }

        int should_reconnect = 0;
        int dly = 0;

        if (trig_cmd[0]) {
            if (strcasecmp(executed_cmd, trig_cmd) == 0) {
                int match_dev = 1;
                if (trig_did && trig_did[0]) {
                    match_dev = 0;
                    if (executed_dev_id && (strcmp(executed_dev_id, trig_did) == 0 || strcasecmp(executed_dev_id, trig_did) == 0)) match_dev = 1;
                    if (!match_dev && executed_dev_name && (strcmp(executed_dev_name, trig_did) == 0 || strcasecmp(executed_dev_name, trig_did) == 0)) match_dev = 1;
                }
                if (match_dev) {
                    should_reconnect = 1;
                    dly = (target_dev_delay >= 0) ? target_dev_delay : 2;
                }
            }
        }

        /* Fallback: any button press sent directly to this BT device */
        if (!should_reconnect) {
            int is_target_dev = 0;
            if (executed_dev_id && cur_id[0] && (strcmp(executed_dev_id, cur_id) == 0 || strcasecmp(executed_dev_id, cur_id) == 0)) is_target_dev = 1;
            if (!is_target_dev && executed_dev_name && cur_name[0] && (strcmp(executed_dev_name, cur_name) == 0 || strcasecmp(executed_dev_name, cur_name) == 0)) is_target_dev = 1;
            if (is_target_dev) {
                should_reconnect = 1;
                dly = 0; /* Button press: 0 delay for immediate reconnect */
            }
        }

        if (!should_reconnect) continue;

        char target_bdaddr[32] = {0};
        if (j_bta && cJSON_IsString(j_bta) && j_bta->valuestring && hub_valid_bdaddr(j_bta->valuestring)) {
            strncpy(target_bdaddr, j_bta->valuestring, sizeof(target_bdaddr) - 1);
        }

        if (!target_bdaddr[0]) {
            FILE *bf = fopen("/data/harmony/bt-devices.json", "r");
            if (bf) {
                fseek(bf, 0, SEEK_END);
                long sz = ftell(bf);
                fseek(bf, 0, SEEK_SET);
                if (sz > 0 && sz < 65536) {
                    char *buf = malloc(sz + 1);
                    if (buf) {
                        size_t r = fread(buf, 1, sz, bf);
                        buf[r] = 0;
                        cJSON *bt_root = cJSON_Parse(buf);
                        if (bt_root) {
                            cJSON *bdevs = cJSON_GetObjectItemCaseSensitive(bt_root, "devices");
                            if (bdevs && cJSON_IsArray(bdevs)) {
                                cJSON *bitem = NULL;
                                cJSON_ArrayForEach(bitem, bdevs) {
                                    cJSON *b_id = cJSON_GetObjectItemCaseSensitive(bitem, "id");
                                    char b_id_str[64] = {0};
                                    if (b_id) {
                                        if (cJSON_IsNumber(b_id)) snprintf(b_id_str, sizeof(b_id_str), "%ld", (long)b_id->valuedouble);
                                        else if (cJSON_IsString(b_id) && b_id->valuestring) strncpy(b_id_str, b_id->valuestring, sizeof(b_id_str) - 1);
                                    }
                                    if (cur_id[0] && b_id_str[0] && strcmp(cur_id, b_id_str) == 0) {
                                        cJSON *b_mac = cJSON_GetObjectItemCaseSensitive(bitem, "bdaddr");
                                        if (!b_mac) b_mac = cJSON_GetObjectItemCaseSensitive(bitem, "btHost");
                                        if (b_mac && cJSON_IsString(b_mac) && b_mac->valuestring && hub_valid_bdaddr(b_mac->valuestring)) {
                                            strncpy(target_bdaddr, b_mac->valuestring, sizeof(target_bdaddr) - 1);
                                            break;
                                        }
                                    }
                                }
                            }
                            cJSON_Delete(bt_root);
                        }
                        free(buf);
                    }
                }
                fclose(bf);
            }
        }

        printf("[*] BT reconnect trigger: device='%s' (id=%s, target=%s) delay=%ds dur=%ds (triggered by cmd='%s' on dev='%s' id='%s')\n",
               cur_name[0] ? cur_name : "BT-Device", cur_id,
               target_bdaddr[0] ? target_bdaddr : "auto",
               dly, dur, executed_cmd,
               executed_dev_name ? executed_dev_name : "",
               executed_dev_id ? executed_dev_id : "");

        if (dly > 0) {
            char sh_cmd[256];
            snprintf(sh_cmd, sizeof(sh_cmd),
                     "/data/harmony/bt_reconnect.sh %s %d %d >/dev/null 2>&1 &",
                     target_bdaddr[0] ? target_bdaddr : "\"\"", dur, dly);
            system(sh_cmd);
        } else {
            if (target_bdaddr[0] && is_bt_device_connected(target_bdaddr)) {
                printf("[*] BT device '%s' (%s) already connected, skipping immediate reconnect\n",
                       cur_name[0] ? cur_name : "BT-Device", target_bdaddr);
            } else {
                char cmd[128];
                if (target_bdaddr[0]) snprintf(cmd, sizeof(cmd), "connect_host %s %d\n", target_bdaddr, dur);
                else snprintf(cmd, sizeof(cmd), "connect_host %d\n", dur);
                hub_send_btstack_cmd(cmd);
            }
        }
    }
}

static int hw_device_command_send_match(const char *target, int match_mode, const char *command, int action) {
    if (!target || !command) return -1;

    /* BORROWED from resource_cache - DO NOT free or cJSON_Delete */
    cJSON *root = get_device_list_cached();
    if (!root) return -4;

    int ret = -1;

    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs || !cJSON_IsArray(devs)) {
        return -5;
    }

    cJSON *target_item = NULL;
    cJSON *target_dev = NULL;
    char target_id_str[64] = {0};
    cJSON *item = NULL;

    /* Match device: match_mode 1 = ID only, 2 = Name only, 0 = ID or Name */
    cJSON_ArrayForEach(item, devs) {
        cJSON *dev = item;

        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(dev, "id");
        char id_str[64] = {0};
        if (j_id) {
            if (cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%ld", (long)j_id->valuedouble);
            else if (cJSON_IsString(j_id) && j_id->valuestring) strncpy(id_str, j_id->valuestring, sizeof(id_str) - 1);
        }

        cJSON *j_name = cJSON_GetObjectItemCaseSensitive(dev, "name");
        const char *name_str = (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : "";

        int matched = 0;
        if (match_mode == 1) {
            matched = (id_str[0] && strcmp(id_str, target) == 0);
        } else if (match_mode == 2) {
            matched = (name_str && name_str[0] && (strcmp(name_str, target) == 0 || strcasecmp(name_str, target) == 0));
        } else {
            matched = ((id_str[0] && strcmp(id_str, target) == 0) ||
                       (name_str && name_str[0] && (strcmp(name_str, target) == 0 || strcasecmp(name_str, target) == 0)));
        }

        if (matched) {
            target_item = item;
            target_dev = dev;
            strncpy(target_id_str, id_str, sizeof(target_id_str) - 1);
            break;
        }
    }

    if (!target_item || !target_dev) {
        return -5;
    }

    int is_bt = 0;
    char bdaddr[32] = {0};

    cJSON *j_type = cJSON_GetObjectItemCaseSensitive(target_dev, "type");
    if (j_type && cJSON_IsString(j_type) && (strcasecmp(j_type->valuestring, "BT") == 0 || strcasestr(j_type->valuestring, "bluetooth"))) {
        is_bt = 1;
    }

    char dev_name[64] = {0};
    cJSON *j_dname = cJSON_GetObjectItemCaseSensitive(target_dev, "name");
    if (j_dname && cJSON_IsString(j_dname) && j_dname->valuestring) {
        strncpy(dev_name, j_dname->valuestring, sizeof(dev_name) - 1);
    }

    cJSON *j_bta = cJSON_GetObjectItemCaseSensitive(target_dev, "bdaddr");
    if (!j_bta) j_bta = cJSON_GetObjectItemCaseSensitive(target_dev, "btHost");
    if (j_bta && cJSON_IsString(j_bta) && j_bta->valuestring) {
        strncpy(bdaddr, j_bta->valuestring, sizeof(bdaddr) - 1);
    }

    if (!bdaddr[0]) {
        FILE *bf = fopen("/data/harmony/bt-devices.json", "r");
        if (bf) {
            fseek(bf, 0, SEEK_END);
            long sz = ftell(bf);
            fseek(bf, 0, SEEK_SET);
            if (sz > 0 && sz < 65536) {
                char *buf = malloc(sz + 1);
                if (buf) {
                    size_t r = fread(buf, 1, sz, bf);
                    buf[r] = 0;
                    cJSON *bj = cJSON_Parse(buf);
                    free(buf);
                    if (bj) {
                        cJSON *devs = cJSON_GetObjectItemCaseSensitive(bj, "devices");
                        if (devs && cJSON_IsArray(devs)) {
                            cJSON *di = NULL;
                            cJSON_ArrayForEach(di, devs) {
                                cJSON *d_id = cJSON_GetObjectItemCaseSensitive(di, "id");
                                cJSON *d_name = cJSON_GetObjectItemCaseSensitive(di, "name");
                                cJSON *d_addr = cJSON_GetObjectItemCaseSensitive(di, "bdaddr");
                                if (d_addr && cJSON_IsString(d_addr) && d_addr->valuestring[0]) {
                                    if ((d_id && cJSON_IsString(d_id) && ((target_id_str[0] && strcmp(d_id->valuestring, target_id_str) == 0) || strcmp(d_id->valuestring, target) == 0)) ||
                                        (d_name && cJSON_IsString(d_name) && strcasecmp(d_name->valuestring, target) == 0) ||
                                        (dev_name[0] && d_name && cJSON_IsString(d_name) && strcasecmp(d_name->valuestring, dev_name) == 0)) {
                                        strncpy(bdaddr, d_addr->valuestring, sizeof(bdaddr) - 1);
                                        break;
                                    }
                                }
                            }
                        }
                        cJSON_Delete(bj);
                    }
                }
            }
            fclose(bf);
        }
    }

    if (!bdaddr[0]) {
        FILE *tf = fopen("/data/harmony/bthid_target", "r");
        if (tf) {
            char line[128];
            while (fgets(line, sizeof(line), tf)) {
                if (strncmp(line, "bdaddr=", 7) == 0) {
                    sscanf(line + 7, "%31s", bdaddr);
                }
            }
            fclose(tf);
        }
    }

    int is_mqtt = 0;
    cJSON *j_men2 = cJSON_GetObjectItemCaseSensitive(target_dev, "mqttEnabled");
    if (j_men2) {
        if (cJSON_IsTrue(j_men2)) is_mqtt = 1;
        else if (cJSON_IsNumber(j_men2) && j_men2->valueint > 0) is_mqtt = 1;
        else if (cJSON_IsString(j_men2) && (strcmp(j_men2->valuestring, "1") == 0 || strcasecmp(j_men2->valuestring, "true") == 0)) is_mqtt = 1;
    }
    cJSON *j_dtype = cJSON_GetObjectItemCaseSensitive(target_dev, "type");
    if (j_dtype && cJSON_IsString(j_dtype) && strcasestr(j_dtype->valuestring, "mqtt")) is_mqtt = 1;

    cJSON *j_dmfg = cJSON_GetObjectItemCaseSensitive(target_dev, "manufacturer");
    if (j_dmfg && cJSON_IsString(j_dmfg) && strcasestr(j_dmfg->valuestring, "mqtt")) is_mqtt = 1;

    cJSON *j_mtop = cJSON_GetObjectItemCaseSensitive(target_dev, "mqttTopic");
    if (j_mtop && cJSON_IsString(j_mtop) && j_mtop->valuestring && j_mtop->valuestring[0]) is_mqtt = 1;

    /* Find Command */
    cJSON *cmds = cJSON_GetObjectItemCaseSensitive(target_dev, "commands");
    cJSON *target_cmd = NULL;

    const char *alt = NULL;
    if (strcasecmp(command, "OK") == 0) alt = "Select";
    else if (strcasecmp(command, "Select") == 0) alt = "OK";
    else if (strcasecmp(command, "Enter") == 0) alt = "Select";
    else if (strcasecmp(command, "Back") == 0) alt = "Return";
    else if (strcasecmp(command, "Return") == 0) alt = "Back";
    else if (strcasecmp(command, "Stop") == 0) alt = "Pause";
    else if (strcasecmp(command, "ChannelUp") == 0) alt = "PageUp";
    else if (strcasecmp(command, "ChannelDown") == 0) alt = "PageDown";

    if (cmds && cJSON_IsArray(cmds)) {
        cJSON *c_elem = NULL;
        cJSON_ArrayForEach(c_elem, cmds) {
            cJSON *c_name = cJSON_GetObjectItemCaseSensitive(c_elem, "name");
            if (c_name && cJSON_IsString(c_name) && c_name->valuestring) {
                if (strcmp(c_name->valuestring, command) == 0 ||
                    strcasecmp(c_name->valuestring, command) == 0) {
                    target_cmd = c_elem;
                    break;
                }
                if (!target_cmd && alt && (strcmp(c_name->valuestring, alt) == 0 ||
                                          strcasecmp(c_name->valuestring, alt) == 0)) {
                    target_cmd = c_elem;
                }
            }
        }
    }

    if (!target_cmd && !is_mqtt) {
        return -6;
    }

    check_and_trigger_bt_reconnect_for_command(devs, target_id_str[0] ? target_id_str : target, dev_name, command);

    char keycode_str[4096] = {0};
    if (target_cmd) {
        cJSON *j_kc = cJSON_GetObjectItemCaseSensitive(target_cmd, "keyCode");
        if (j_kc && cJSON_IsString(j_kc) && j_kc->valuestring && j_kc->valuestring[0]) {
            strncpy(keycode_str, j_kc->valuestring, sizeof(keycode_str) - 1);
        } else {
            cJSON *j_raw = cJSON_GetObjectItemCaseSensitive(target_cmd, "raw");
            if (j_raw && cJSON_IsString(j_raw) && j_raw->valuestring && j_raw->valuestring[0]) {
                strncpy(keycode_str, j_raw->valuestring, sizeof(keycode_str) - 1);
            }
        }
    }

    if (strncmp(keycode_str, "G:MQTT", 6) == 0) is_mqtt = 1;

    uint8_t ir_ports = IR_PORT_ALL;
    cJSON *j_cp = cJSON_GetObjectItemCaseSensitive(target_dev, "controlPort");
    if (j_cp && cJSON_IsNumber(j_cp)) {
        int cp_val = (int)j_cp->valuedouble;
        if (cp_val >= 0 && cp_val <= 7) {
            ir_ports = (uint8_t)cp_val;
        }
    }

    if (strncmp(keycode_str, "HID:", 4) == 0 || strncmp(keycode_str, "hid:", 4) == 0 ||
        strcasecmp(command, "Connect") == 0 || strcasecmp(command, "Disconnect") == 0 ||
        strcasecmp(command, "BT_CONNECT") == 0 || strcasecmp(command, "BT_DISCONNECT") == 0) {
        is_bt = 1;
    }

    int inter_device_delay = 500;
    int inter_key_delay = 100;
    int hold_inter_key_delay = 200;

    cJSON *j_idd = cJSON_GetObjectItemCaseSensitive(target_dev, "interDeviceDelay");
    if (j_idd && cJSON_IsNumber(j_idd) && j_idd->valueint >= 0) inter_device_delay = j_idd->valueint;

    cJSON *j_ikd = cJSON_GetObjectItemCaseSensitive(target_dev, "interKeyDelay");
    if (j_ikd && cJSON_IsNumber(j_ikd) && j_ikd->valueint >= 0) inter_key_delay = j_ikd->valueint;

    cJSON *j_hikd = cJSON_GetObjectItemCaseSensitive(target_dev, "holdInterKeyDelay");
    if (j_hikd && cJSON_IsNumber(j_hikd) && j_hikd->valueint >= 0) hold_inter_key_delay = j_hikd->valueint;

    static pthread_mutex_t s_hw_cmd_lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t s_last_cmd_time_ms = 0;
    static char s_last_device_id[64] = {0};
    static char s_last_cmd_name[64] = {0};

    pthread_mutex_lock(&s_hw_cmd_lock);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    const char *actual_dev_id = target_id_str[0] ? target_id_str : target;

    /* 90ms debounce rejection: drop rapid duplicate press/tap pulses on the same button */
    if ((action == HW_BTN_ACTION_TAP || action == HW_BTN_ACTION_PRESS) &&
        s_last_cmd_time_ms > 0 &&
        strcmp(s_last_device_id, actual_dev_id) == 0 &&
        strcmp(s_last_cmd_name, command) == 0) {
        uint64_t elapsed = (now_ms > s_last_cmd_time_ms) ? (now_ms - s_last_cmd_time_ms) : 0;
        if (elapsed < 90) {
            pthread_mutex_unlock(&s_hw_cmd_lock);
            return 0;
        }
    }

    if (action != HW_BTN_ACTION_RELEASE && action != HW_BTN_ACTION_REPEAT && s_last_cmd_time_ms > 0) {
        uint64_t elapsed = (now_ms > s_last_cmd_time_ms) ? (now_ms - s_last_cmd_time_ms) : 0;
        int req_delay = 0;
        if (strcmp(s_last_device_id, actual_dev_id) != 0) {
            req_delay = inter_device_delay;
        } else {
            if (strcmp(s_last_cmd_name, command) == 0) {
                req_delay = hold_inter_key_delay;
            } else {
                req_delay = inter_key_delay;
            }
        }
        if (req_delay > 0 && elapsed < (uint64_t)req_delay) {
            uint64_t wait_ms = (uint64_t)req_delay - elapsed;
            usleep((useconds_t)(wait_ms * 1000));
        }
    }

    int is_ble_adv = (strncmp(keycode_str, BLE_ADV_KEYCODE_PREFIX, BLE_ADV_KEYCODE_PREFIX_LEN) == 0);
    int is_mantra = (strncmp(keycode_str, MANTRA_KEYCODE_PREFIX, MANTRA_KEYCODE_PREFIX_LEN) == 0);

    if (is_mantra) {
        /* One broadcast per press, like other BLE broadcast commands */
        if (action == HW_BTN_ACTION_RELEASE || action == HW_BTN_ACTION_REPEAT) {
            ret = 0;
        } else {
            ret = mantra_send_command(target_dev, actual_dev_id, keycode_str + MANTRA_KEYCODE_PREFIX_LEN);
            if (ret != 0) {
                printf("[!] Mantra command '%s' (%s) skipped: Device '%s' (%s) needs a valid mantraId and action\n",
                       command, keycode_str, dev_name[0] ? dev_name : "Mantra", actual_dev_id);
            }
        }
    } else if (is_ble_adv) {
        /* One broadcast per press: the receiver acts on the packet, not on how long it is held */
        if (action == HW_BTN_ACTION_RELEASE || action == HW_BTN_ACTION_REPEAT) {
            ret = 0;
        } else {
            const char *hex = keycode_str + BLE_ADV_KEYCODE_PREFIX_LEN;
            uint8_t adv[BLE_ADV_MAX_LEN];
            int adv_len = ble_adv_parse_hex(hex, adv, NULL);
            if (adv_len < 0) {
                printf("[!] BLE broadcast '%s' skipped: Device '%s' (%s) has an invalid payload\n",
                       command, dev_name[0] ? dev_name : "BLE-Device", actual_dev_id);
                ret = -2;
            } else {
                char adv_cmd[8 + 2 * BLE_ADV_MAX_LEN];
                snprintf(adv_cmd, sizeof(adv_cmd), "adv %.*s\n", 2 * adv_len, hex);
                ret = hub_send_btstack_cmd(adv_cmd);
            }
        }
    } else if (is_bt) {
        if (!bdaddr[0]) {
            pthread_mutex_unlock(&s_hw_cmd_lock);
            printf("[!] BT command '%s' skipped: Device '%s' (%s) has no host paired\n",
                   command, dev_name[0] ? dev_name : "BT-Device", actual_dev_id);
            return 0; /* Skip gracefully without failing activity switch */
        }

        char pipe_cmd[128] = {0};
        unsigned int page = 0, usage = 0;

        if (strcasecmp(command, "Connect") == 0 || strcasecmp(command, "BT_CONNECT") == 0) {
            snprintf(pipe_cmd, sizeof(pipe_cmd), "CONNECT %s\n", bdaddr);
        } else if (strcasecmp(command, "Disconnect") == 0 || strcasecmp(command, "BT_DISCONNECT") == 0) {
            snprintf(pipe_cmd, sizeof(pipe_cmd), "DISCONNECT\n");
        } else if (sscanf(keycode_str, "HID:%x:%x", &page, &usage) == 2 ||
                   sscanf(keycode_str, "hid:%x:%x", &page, &usage) == 2) {
            if (page == 0x07) {
                if (action == HW_BTN_ACTION_PRESS) {
                    snprintf(pipe_cmd, sizeof(pipe_cmd), "A1010000%02X0000000000\n", (unsigned char)usage);
                } else if (action == HW_BTN_ACTION_RELEASE) {
                    snprintf(pipe_cmd, sizeof(pipe_cmd), "RELEASE\n");
                } else if (action == HW_BTN_ACTION_REPEAT) {
                    pipe_cmd[0] = '\0';
                    ret = 0;
                } else {
                    snprintf(pipe_cmd, sizeof(pipe_cmd), "A1010000%02X0000000000|A1010000000000000000\n", (unsigned char)usage);
                }
            } else if (page == 0x0C) {
                if (action == HW_BTN_ACTION_PRESS) {
                    snprintf(pipe_cmd, sizeof(pipe_cmd), "CONSUMER_DOWN %X\n", usage);
                } else if (action == HW_BTN_ACTION_RELEASE) {
                    snprintf(pipe_cmd, sizeof(pipe_cmd), "CONSUMER_UP\n");
                } else if (action == HW_BTN_ACTION_REPEAT) {
                    pipe_cmd[0] = '\0';
                    ret = 0;
                } else {
                    snprintf(pipe_cmd, sizeof(pipe_cmd), "CONSUMER %X\n", usage);
                }
            }
        } else {
            if (action == HW_BTN_ACTION_PRESS) {
                snprintf(pipe_cmd, sizeof(pipe_cmd), "KEYDOWN %s\n", command);
            } else if (action == HW_BTN_ACTION_RELEASE) {
                snprintf(pipe_cmd, sizeof(pipe_cmd), "KEYUP %s\n", command);
            } else if (action == HW_BTN_ACTION_REPEAT) {
                pipe_cmd[0] = '\0';
                ret = 0;
            } else {
                snprintf(pipe_cmd, sizeof(pipe_cmd), "KEY %s\n", command);
            }
        }

        if (pipe_cmd[0]) {
            int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
            if (bfd >= 0) {
                char target_hdr[64];
                snprintf(target_hdr, sizeof(target_hdr), "TARGET %s\n", bdaddr);
                write(bfd, target_hdr, strlen(target_hdr));
                write(bfd, pipe_cmd, strlen(pipe_cmd));
                close(bfd);
                ret = 0;
            } else {
                ret = -1;
            }

            if (is_debug_log_enabled()) {
                struct stat st;
                if (stat("/tmp/bt_sent.log", &st) == 0 && st.st_size > 65536) {
                    unlink("/tmp/bt_sent.log.1");
                    rename("/tmp/bt_sent.log", "/tmp/bt_sent.log.1");
                }
                FILE *bf = fopen("/tmp/bt_sent.log", "a");
                if (bf) {
                    time_t now = time(NULL);
                    char tbuf[32];
                    ctime_r(&now, tbuf);
                    char *nl = strchr(tbuf, '\n');
                    if (nl) *nl = '\0';
                    fprintf(bf, "[%s] Dev: %s (%s) | Cmd: %s | Action: %d | Page: 0x%02X Usage: 0x%04X | KeyCode: %s | ret=%d | Pipe: %s",
                            tbuf, dev_name[0] ? dev_name : "BT-Device", actual_dev_id, command, action, page, usage, keycode_str, ret, pipe_cmd);
                    fclose(bf);
                }
            }
        }
    } else if (is_mqtt || strncmp(keycode_str, "G:MQTT", 6) == 0) {
        const char *actual_dev_id_str = target_id_str[0] ? target_id_str : target;
        if (g_mqtt_button_handler) {
            ret = g_mqtt_button_handler(actual_dev_id_str, dev_name, command);
        } else {
            /* Dispatch MQTT button to hub_daemon on 127.0.0.1:8089 */
            int mfd = socket(AF_INET, SOCK_STREAM, 0);
            if (mfd >= 0) {
                struct sockaddr_in sin;
                memset(&sin, 0, sizeof(sin));
                sin.sin_family = AF_INET;
                sin.sin_port = htons(8089);
                sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
                setsockopt(mfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(mfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                if (connect(mfd, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
                    char *body = build_button_press_json(actual_dev_id_str, dev_name, command, action);
                    char req[512];
                    snprintf(req, sizeof(req),
                             "POST /api/button-press HTTP/1.1\r\n"
                             "Host: 127.0.0.1:8089\r\n"
                             "Content-Length: %zu\r\n"
                             "Content-Type: application/json\r\n"
                             "Connection: close\r\n\r\n%s",
                             body ? strlen(body) : 0, body ? body : "");
                    if (body) free(body);
                    send(mfd, req, strlen(req), MSG_NOSIGNAL);
                    char resp[256] = {0};
                    recv(mfd, resp, sizeof(resp) - 1, 0);
                    if (strstr(resp, "200 OK") != NULL || strstr(resp, "\"ok\":true") != NULL) {
                        ret = 0;
                    }
                }
                close(mfd);
            }
        }
    } else if (keycode_str[0]) {
        if (action == HW_BTN_ACTION_RELEASE) {
            hw_ir_cancel(ir_ports);
            ret = 0;
        } else {
            uint8_t repeats = 1;
            cJSON *j_rep = cJSON_GetObjectItemCaseSensitive(target_cmd, "repeats");
            if (j_rep && cJSON_IsNumber(j_rep) && j_rep->valuedouble > 0 && j_rep->valuedouble <= 20) {
                repeats = (uint8_t)j_rep->valuedouble;
            }
            ret = hw_ir_send_harmony_keycode(keycode_str, ir_ports, repeats);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    s_last_cmd_time_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    strncpy(s_last_device_id, target_id_str[0] ? target_id_str : target, sizeof(s_last_device_id) - 1);
    s_last_device_id[sizeof(s_last_device_id) - 1] = '\0';
    strncpy(s_last_cmd_name, command, sizeof(s_last_cmd_name) - 1);
    s_last_cmd_name[sizeof(s_last_cmd_name) - 1] = '\0';
    pthread_mutex_unlock(&s_hw_cmd_lock);

    return ret;
}

int hw_device_command_send_action(const char *target, int match_mode, const char *command, int action) {
    return hw_device_command_send_match(target, match_mode, command, action);
}

int hw_device_command_send(const char *device_id, const char *command) {
    return hw_device_command_send_match(device_id, 0, command, HW_BTN_ACTION_TAP);
}

int hw_device_command_send_by_id(const char *device_id, const char *command) {
    return hw_device_command_send_match(device_id, 1, command, HW_BTN_ACTION_TAP);
}

int hw_device_command_send_by_name(const char *device_name, const char *command) {
    return hw_device_command_send_match(device_name, 2, command, HW_BTN_ACTION_TAP);
}

int hw_mantra_pair(const char *device_id) {
    if (!device_id || !device_id[0]) return -5;
    /* BORROWED from resource_cache - DO NOT free or cJSON_Delete */
    cJSON *root = get_device_list_cached();
    cJSON *devs = root ? cJSON_GetObjectItemCaseSensitive(root, "devices") : NULL;
    cJSON *dev = NULL;
    cJSON_ArrayForEach(dev, devs) {
        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(dev, "id");
        char id_str[64] = {0};
        if (j_id && cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%ld", (long)j_id->valuedouble);
        else if (j_id && cJSON_IsString(j_id) && j_id->valuestring) snprintf(id_str, sizeof(id_str), "%s", j_id->valuestring);
        if (strcmp(id_str, device_id) == 0) break;
    }
    if (!dev) return -5;

    uint16_t id;
    int version;
    if (mantra_device_params(dev, &id, &version) != 0) return -2;

    /* The app sends a search, then a bind a few seconds later; hub_btstack queues them in order */
    uint8_t adv[MANTRA_ADV_LEN];
    int len = mantra_encode_pair_adv(0, id, mantra_next_counter(), adv);
    int rc = mantra_send_adv(adv, len, 2000);
    len = mantra_encode_pair_adv(1, id, mantra_next_counter(), adv);
    if (rc == 0) rc = mantra_send_adv(adv, len, 3000);
    printf("[*] Mantra pairing broadcast for device %s, fan id %04X: %s\n", device_id, id, rc == 0 ? "queued" : "failed");
    return rc;
}

int hw_bthid_send_report(const uint8_t *report, size_t len) {
    if (!report || len == 0) return -1;

    char hex[256] = {0};
    size_t hlen = 0;
    for (size_t i = 0; i < len && (hlen + 3) < sizeof(hex); i++) {
        snprintf(hex + hlen, sizeof(hex) - hlen, "%02X", report[i]);
        hlen += 2;
    }
    snprintf(hex + hlen, sizeof(hex) - hlen, "\n");

    int fd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        write(fd, hex, strlen(hex));
        close(fd);
        return 0;
    }
    return -1;
}
