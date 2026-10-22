#include "hw_action.h"
#include "resource_cache.h"
#include "ir_encoder.h"
#include "ir_i2s.h"
#include "config_store.h"
#include "codex_bt_common.h"
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

static hw_mqtt_button_handler_t g_mqtt_button_handler = NULL;

void hw_set_mqtt_button_handler(hw_mqtt_button_handler_t handler) {
    g_mqtt_button_handler = handler;
}

static char *build_ir_send_json(int enable, unsigned int ports, int key_latency) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddBoolToObject(j, "enable", enable ? 1 : 0);
    if (key_latency >= 0) {
        cJSON_AddNumberToObject(j, "keyLatency", key_latency);
    }
    cJSON_AddNumberToObject(j, "ports", ports);
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return out;
}

static char *build_bthid_json(const char *bdaddr) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddStringToObject(j, "type", "btkeyboard");
    cJSON_AddStringToObject(j, "bdaddr", bdaddr ? bdaddr : "");
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return out;
}

static char *build_button_press_json(const char *device_id, const char *dev_name, const char *command) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddStringToObject(j, "deviceId", device_id ? device_id : "");
    cJSON_AddStringToObject(j, "device", (dev_name && dev_name[0]) ? dev_name : "Dummy");
    cJSON_AddStringToObject(j, "command", command ? command : "");
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

static void check_and_trigger_bt_reconnect_for_command(cJSON *devs, const char *executed_dev_id, const char *executed_dev_name, const char *executed_cmd) {
    if (!devs || !executed_cmd || !executed_cmd[0]) return;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devs) {
        cJSON *dev = cJSON_GetObjectItemCaseSensitive(item, "Device");
        if (!dev) dev = item;

        int is_bt = 0;
        cJSON *j_trans = cJSON_GetObjectItemCaseSensitive(dev, "transport");
        if (!j_trans) j_trans = cJSON_GetObjectItemCaseSensitive(dev, "Transport");
        if (j_trans && cJSON_IsNumber(j_trans) && (int)j_trans->valuedouble == 32) is_bt = 1;
        cJSON *j_isbt = cJSON_GetObjectItemCaseSensitive(dev, "isBt");
        if (j_isbt && cJSON_IsTrue(j_isbt)) is_bt = 1;
        if (!is_bt) continue;

        cJSON *j_rec = cJSON_GetObjectItemCaseSensitive(dev, "bt_auto_reconnect");
        if (!j_rec) j_rec = cJSON_GetObjectItemCaseSensitive(dev, "btAutoReconnect");
        if (j_rec) {
            int rec_val = cJSON_IsNumber(j_rec) ? (int)j_rec->valuedouble : (cJSON_IsBool(j_rec) ? (cJSON_IsTrue(j_rec) ? 1 : 0) : atoi(j_rec->valuestring));
            if (rec_val == 0) continue;
        }

        cJSON *j_trig_dev = cJSON_GetObjectItemCaseSensitive(dev, "bt_trigger_device_id");
        if (!j_trig_dev) j_trig_dev = cJSON_GetObjectItemCaseSensitive(dev, "btTriggerDeviceId");
        const char *trig_did = (j_trig_dev && cJSON_IsString(j_trig_dev)) ? j_trig_dev->valuestring : "";

        cJSON *j_trig_cmd = cJSON_GetObjectItemCaseSensitive(dev, "bt_trigger_command_name");
        if (!j_trig_cmd) j_trig_cmd = cJSON_GetObjectItemCaseSensitive(dev, "btTriggerCommandName");
        if (!j_trig_cmd) j_trig_cmd = cJSON_GetObjectItemCaseSensitive(dev, "bt_trigger_command");
        if (!j_trig_cmd) j_trig_cmd = cJSON_GetObjectItemCaseSensitive(dev, "btTriggerCommand");
        const char *trig_cmd = (j_trig_cmd && cJSON_IsString(j_trig_cmd)) ? j_trig_cmd->valuestring : "";
        if (!trig_cmd[0]) continue;

        char trig_did_buf[64] = {0};
        char trig_cmd_buf[64] = {0};
        const char *colon = strchr(trig_cmd, ':');
        if (colon && !trig_did[0]) {
            size_t did_len = (size_t)(colon - trig_cmd);
            if (did_len >= sizeof(trig_did_buf)) did_len = sizeof(trig_did_buf) - 1;
            strncpy(trig_did_buf, trig_cmd, did_len);
            trig_did = trig_did_buf;
            strncpy(trig_cmd_buf, colon + 1, sizeof(trig_cmd_buf) - 1);
            trig_cmd = trig_cmd_buf;
        }

        if (strcasecmp(executed_cmd, trig_cmd) != 0) continue;

        if (trig_did && trig_did[0]) {
            int match_dev = 0;
            if (executed_dev_id && (strcmp(executed_dev_id, trig_did) == 0 || strcasecmp(executed_dev_id, trig_did) == 0)) match_dev = 1;
            if (!match_dev && executed_dev_name && (strcmp(executed_dev_name, trig_did) == 0 || strcasecmp(executed_dev_name, trig_did) == 0)) match_dev = 1;
            if (!match_dev) continue;
        }

        char target_bdaddr[32] = {0};
        cJSON *j_bta = cJSON_GetObjectItemCaseSensitive(dev, "BTAddress");
        if (!j_bta) j_bta = cJSON_GetObjectItemCaseSensitive(dev, "bt_host");
        if (!j_bta) j_bta = cJSON_GetObjectItemCaseSensitive(dev, "bdaddr");
        if (j_bta && cJSON_IsString(j_bta) && j_bta->valuestring && codex_valid_bdaddr(j_bta->valuestring)) {
            strncpy(target_bdaddr, j_bta->valuestring, sizeof(target_bdaddr) - 1);
        }

        struct codex_settings g_settings;
        config_store_get_settings(&g_settings);

        int dur = 0;
        cJSON *jd_dur = cJSON_GetObjectItemCaseSensitive(dev, "bt_reconnect_duration_sec");
        if (!jd_dur) jd_dur = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDurationSec");
        if (!jd_dur) jd_dur = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDuration");
        if (jd_dur && cJSON_IsNumber(jd_dur) && jd_dur->valueint > 0) dur = jd_dur->valueint;
        if (dur <= 0) dur = g_settings.bt_reconnect_duration_sec > 0 ? g_settings.bt_reconnect_duration_sec : 30;

        int dly = -1;
        cJSON *jd_dly = cJSON_GetObjectItemCaseSensitive(dev, "bt_reconnect_delay_sec");
        if (!jd_dly) jd_dly = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDelaySec");
        if (!jd_dly) jd_dly = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDelay");
        if (jd_dly && cJSON_IsNumber(jd_dly) && jd_dly->valueint >= 0) dly = jd_dly->valueint;
        if (dly < 0) dly = g_settings.bt_reconnect_delay_sec >= 0 ? g_settings.bt_reconnect_delay_sec : 2;

        char sh_cmd[256];
        if (target_bdaddr[0]) {
            snprintf(sh_cmd, sizeof(sh_cmd), "/data/codex/bt_reconnect.sh %s %d %d >/dev/null 2>&1 &", target_bdaddr, dur, dly);
        } else {
            snprintf(sh_cmd, sizeof(sh_cmd), "/data/codex/bt_reconnect.sh \"\" %d %d >/dev/null 2>&1 &", dur, dly);
        }
        system(sh_cmd);

        if (dly == 0) {
            char cmd[128];
            if (target_bdaddr[0]) snprintf(cmd, sizeof(cmd), "connect_host %s %d\n", target_bdaddr, dur);
            else snprintf(cmd, sizeof(cmd), "connect_host %d\n", dur);
            codex_send_btstack_cmd(cmd);
        }
    }
}

int hw_device_command_send(const char *device_id, const char *command) {
    if (!device_id || !command) return -1;

    /* BORROWED from resource_cache - DO NOT free or cJSON_Delete */
    cJSON *root = get_device_list_cached();
    if (!root) return -4;

    int ret = -1;

    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
    if (!devs || !cJSON_IsArray(devs)) {
        return -5;
    }

    cJSON *target_item = NULL;
    cJSON *target_dev = NULL;
    cJSON *item = NULL;

    /* Match device by ID or Name */
    cJSON_ArrayForEach(item, devs) {
        cJSON *dev = cJSON_GetObjectItemCaseSensitive(item, "Device");
        if (!dev) dev = item;

        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(dev, "id");
        if (!j_id) j_id = cJSON_GetObjectItemCaseSensitive(dev, "Id-");
        if (!j_id) j_id = cJSON_GetObjectItemCaseSensitive(dev, "Id");

        char id_str[64] = {0};
        if (j_id) {
            if (cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%ld", (long)j_id->valuedouble);
            else if (cJSON_IsString(j_id) && j_id->valuestring) strncpy(id_str, j_id->valuestring, sizeof(id_str) - 1);
        }

        cJSON *j_name = cJSON_GetObjectItemCaseSensitive(dev, "name");
        if (!j_name) j_name = cJSON_GetObjectItemCaseSensitive(dev, "Name");
        const char *name_str = (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : "";

        if ((id_str[0] && strcmp(id_str, device_id) == 0) ||
            (name_str && name_str[0] && (strcmp(name_str, device_id) == 0 || strcasecmp(name_str, device_id) == 0))) {
            target_item = item;
            target_dev = dev;
            break;
        }
    }

    if (!target_item || !target_dev) {
        return -5;
    }

    int is_bt = 0;
    char bdaddr[32] = {0};

    cJSON *j_trans = cJSON_GetObjectItemCaseSensitive(target_dev, "transport");
    if (!j_trans) j_trans = cJSON_GetObjectItemCaseSensitive(target_dev, "Transport");
    if (!j_trans) j_trans = cJSON_GetObjectItemCaseSensitive(target_dev, "TransportType");
    if (j_trans && cJSON_IsNumber(j_trans) && (int)j_trans->valuedouble == 32) {
        is_bt = 1;
    }
    cJSON *j_isbt = cJSON_GetObjectItemCaseSensitive(target_dev, "isBt");
    if (j_isbt && cJSON_IsTrue(j_isbt)) is_bt = 1;

    char dev_name[64] = {0};
    cJSON *j_dname = cJSON_GetObjectItemCaseSensitive(target_dev, "name");
    if (!j_dname) j_dname = cJSON_GetObjectItemCaseSensitive(target_dev, "Name");
    if (j_dname && cJSON_IsString(j_dname) && j_dname->valuestring) {
        strncpy(dev_name, j_dname->valuestring, sizeof(dev_name) - 1);
    }

    cJSON *j_bta = cJSON_GetObjectItemCaseSensitive(target_dev, "bdaddr");
    if (!j_bta) j_bta = cJSON_GetObjectItemCaseSensitive(target_dev, "BTAddress");
    if (j_bta && cJSON_IsString(j_bta) && j_bta->valuestring) {
        strncpy(bdaddr, j_bta->valuestring, sizeof(bdaddr) - 1);
    }

    if (!bdaddr[0]) {
        FILE *bf = fopen("/data/codex/bt-devices.json", "r");
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
                                    if ((d_id && cJSON_IsString(d_id) && strcmp(d_id->valuestring, device_id) == 0) ||
                                        (d_name && cJSON_IsString(d_name) && strcasecmp(d_name->valuestring, device_id) == 0) ||
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
        FILE *tf = fopen("/data/codex/bthid_target", "r");
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

    /* Find Command */
    cJSON *cmds = cJSON_GetObjectItemCaseSensitive(target_item, "commands");
    if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(target_item, "Commands");
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
            if (!c_name) c_name = cJSON_GetObjectItemCaseSensitive(c_elem, "Name");
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

    if (!target_cmd) {
        return -6;
    }

    check_and_trigger_bt_reconnect_for_command(devs, device_id, dev_name, command);

    char keycode_str[4096] = {0};
    cJSON *j_kc = cJSON_GetObjectItemCaseSensitive(target_cmd, "keyCode");
    if (!j_kc) j_kc = cJSON_GetObjectItemCaseSensitive(target_cmd, "KeyCode");
    if (j_kc && cJSON_IsString(j_kc) && j_kc->valuestring && j_kc->valuestring[0]) {
        strncpy(keycode_str, j_kc->valuestring, sizeof(keycode_str) - 1);
    } else {
        cJSON *j_raw = cJSON_GetObjectItemCaseSensitive(target_cmd, "raw");
        if (!j_raw) j_raw = cJSON_GetObjectItemCaseSensitive(target_cmd, "Raw");
        if (j_raw && cJSON_IsString(j_raw) && j_raw->valuestring && j_raw->valuestring[0]) {
            strncpy(keycode_str, j_raw->valuestring, sizeof(keycode_str) - 1);
        }
    }

    uint8_t ir_ports = IR_PORT_ALL;
    cJSON *j_cp = cJSON_GetObjectItemCaseSensitive(target_dev, "controlPort");
    if (!j_cp) j_cp = cJSON_GetObjectItemCaseSensitive(target_dev, "ControlPort");
    if (j_cp && cJSON_IsNumber(j_cp)) {
        int cp_val = (int)j_cp->valuedouble;
        if (cp_val >= 0 && cp_val <= 7) {
            ir_ports = (uint8_t)cp_val;
        }
    }

    /* Check if target device or command is MQTT */
    int is_mqtt = 0;
    cJSON *j_mqtt = cJSON_GetObjectItemCaseSensitive(target_dev, "mqtt");
    if (j_mqtt && cJSON_IsObject(j_mqtt)) {
        cJSON *j_men = cJSON_GetObjectItemCaseSensitive(j_mqtt, "enabled");
        if (j_men && cJSON_IsTrue(j_men)) is_mqtt = 1;
    }
    cJSON *j_men2 = cJSON_GetObjectItemCaseSensitive(target_dev, "mqttEnabled");
    if (!j_men2) j_men2 = cJSON_GetObjectItemCaseSensitive(target_dev, "MQTTEnabled");
    if (j_men2 && (cJSON_IsTrue(j_men2) || (cJSON_IsNumber(j_men2) && j_men2->valueint > 0))) is_mqtt = 1;

    if (strncmp(keycode_str, "G:MQTT", 6) == 0) is_mqtt = 1;

    if (strncmp(keycode_str, "HID:", 4) == 0 || strncmp(keycode_str, "hid:", 4) == 0 ||
        strcasecmp(command, "Connect") == 0 || strcasecmp(command, "Disconnect") == 0 ||
        strcasecmp(command, "BT_CONNECT") == 0 || strcasecmp(command, "BT_DISCONNECT") == 0) {
        is_bt = 1;
    }

    if (is_bt) {
        if (!bdaddr[0]) {
            printf("[!] BT command '%s' skipped: Device '%s' (%s) has no host paired\n",
                   command, dev_name[0] ? dev_name : "BT-Device", device_id);
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
                snprintf(pipe_cmd, sizeof(pipe_cmd), "A1010000%02X0000000000|A1010000000000000000\n", (unsigned char)usage);
            } else if (page == 0x0C) {
                snprintf(pipe_cmd, sizeof(pipe_cmd), "CONSUMER %X\n", usage);
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
                    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
                    fprintf(bf, "[%s] Dev: %s (%s) | Cmd: %s | Page: 0x%02X Usage: 0x%04X | KeyCode: %s | ret=%d | Pipe: %s",
                            tbuf, dev_name[0] ? dev_name : "BT-Device", device_id, command, page, usage, keycode_str, ret, pipe_cmd);
                    fclose(bf);
                }
            }
        }
    } else if (is_mqtt || strncmp(keycode_str, "G:MQTT", 6) == 0) {
        if (g_mqtt_button_handler) {
            ret = g_mqtt_button_handler(device_id, dev_name, command);
        } else {
            /* Dispatch MQTT button to codex_daemon on 127.0.0.1:8089 */
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
                    char *body = build_button_press_json(device_id, dev_name, command);
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
        uint8_t repeats = 3;
        cJSON *j_rep = cJSON_GetObjectItemCaseSensitive(target_cmd, "Repeats");
        if (!j_rep) j_rep = cJSON_GetObjectItemCaseSensitive(target_cmd, "repeats");
        if (j_rep && cJSON_IsNumber(j_rep) && j_rep->valuedouble > 0 && j_rep->valuedouble <= 20) {
            repeats = (uint8_t)j_rep->valuedouble;
        }
        ret = hw_ir_send_harmony_keycode(keycode_str, ir_ports, repeats);
    }

    return ret;
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
