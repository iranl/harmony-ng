#include "bt_remote.h"
#include "bt_pairing.h"
#include "bt_hid.h"

static cJSON *g_cached_map_json = NULL;
static time_t g_map_mtime = 0;
static btstack_timer_source_t g_remote_watchdog_timer;
static bool g_remote_watchdog_active = false;
static char g_active_remote_btn[64] = "";

static btstack_timer_source_t g_remote_wake_grace_timer;
static bool g_remote_wake_grace_active = true;

const char *parse_button(const uint8_t *data, uint16_t len) {
    if (len < 1) return NULL;

    /* Report ID 0x01: Keyboard report [0x01, mod, res, k1, ...] */
    if (data[0] == 0x01 && len >= 3) {
        uint8_t k = (len >= 4 && data[1] == 0) ? data[3] : data[2];
        if (k == 0x00) return NULL; // Release
        switch (k) {
            case 0x52: return "up";
            case 0x51: return "down";
            case 0x50: return "left";
            case 0x4F: return "right";
            case 0x28: return "select";
            case 0x29: return "back";
            case 0x4A: return "home";
            case 0x80: return "vol_up";
            case 0x81: return "vol_down";
            case 0x1E: return "1"; case 0x1F: return "2"; case 0x20: return "3";
            case 0x21: return "4"; case 0x22: return "5"; case 0x23: return "6";
            case 0x24: return "7"; case 0x25: return "8"; case 0x26: return "9";
            case 0x27: return "0";
            default: return NULL;
        }
    }

    /* Raw keyboard report without Report ID [mod, res, k1, ...] (len == 8) */
    if (len == 8) {
        uint8_t k = (data[1] == 0 && data[3] != 0) ? data[3] : data[2];
        if (k != 0x00) {
            switch (k) {
                case 0x52: return "up";
                case 0x51: return "down";
                case 0x50: return "left";
                case 0x4F: return "right";
                case 0x28: return "select";
                case 0x29: return "back";
                case 0x4A: return "home";
                case 0x80: return "vol_up";
                case 0x81: return "vol_down";
                case 0x1E: return "1"; case 0x1F: return "2"; case 0x20: return "3";
                case 0x21: return "4"; case 0x22: return "5"; case 0x23: return "6";
                case 0x24: return "7"; case 0x25: return "8"; case 0x26: return "9";
                case 0x27: return "0";
                default: break;
            }
        }
    }

    /* Report ID 0x02 or direct 16-bit Consumer Control */
    uint16_t cc = 0;
    if (data[0] == 0x02 && len >= 3) {
        cc = data[1] | (data[2] << 8);
    } else if (len >= 2) {
        cc = data[0] | (data[1] << 8);
    }
    if (cc == 0x0000) return NULL; // Release

    switch (cc) {
        case 0x0042: return "up";
        case 0x0043: return "down";
        case 0x0044: return "left";
        case 0x0045: return "right";
        case 0x0041: return "select";
        case 0x0223: return "home";
        case 0x0224: return "back";
        case 0x0030: return "power";
        case 0x01BB: return "input";
        case 0x00E2: return "mute";
        case 0x00E9: return "vol_up";
        case 0x00EA: return "vol_down";
        case 0x0069: return "red";
        case 0x006A: return "green";
        case 0x006C: return "yellow";
        case 0x006B:
        case 0x006D: return "blue";
        case 0x0061: return "guide";
        case 0x008D: return "live_tv";
        case 0x01BD: return "info";
        case 0x009C: return "ch_up";
        case 0x009D: return "ch_down";
        case 0x0077: return "youtube";
        case 0x0078: return "netflix";
        case 0x0079: return "prime_video";
        case 0x007A: return "google_play";
        case 0x0096: return "settings";
        case 0x0221: return "assistant";
        case 0x022A: return "watchlist";
        default: return NULL;
    }
}

void trigger_http_post(const char *endpoint, const char *json_body) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_in addr;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DAEMON_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }
    char req[512];
    size_t blen = json_body ? strlen(json_body) : 0;
    snprintf(req, sizeof(req),
             "POST %s HTTP/1.1\r\n"
             "Host: 127.0.0.1:%d\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n%s",
             endpoint, DAEMON_PORT, blen, json_body ? json_body : "");
    ssize_t w = write(fd, req, strlen(req));
    (void)w;
    close(fd);
}

char *read_entire_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 500000) { fclose(f); return NULL; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = 0;
    fclose(f);
    return buf;
}

cJSON *get_cached_map(void) {
    struct stat st;
    if (stat(MAP_FILE, &st) != 0) return NULL;
    if (g_cached_map_json && st.st_mtime == g_map_mtime) {
        return g_cached_map_json;
    }
    if (g_cached_map_json) {
        cJSON_Delete(g_cached_map_json);
        g_cached_map_json = NULL;
    }
    char *buf = read_entire_file(MAP_FILE);
    if (buf) {
        g_cached_map_json = cJSON_Parse(buf);
        free(buf);
    }
    g_map_mtime = st.st_mtime;
    return g_cached_map_json;
}

void read_current_activity(char *out, size_t outlen) {
    FILE *f = fopen(CURRENT_ACT_FILE, "r");
    if (f) {
        if (fgets(out, outlen, f)) {
            fclose(f);
            char *p = out + strlen(out) - 1;
            while (p >= out && (*p == '\r' || *p == '\n' || *p == ' ')) *p-- = 0;
            if (out[0]) return;
        } else {
            fclose(f);
        }
    }
    snprintf(out, outlen, "-1");
}

int is_bt_device_id(const char *target) {
    if (!target || !target[0]) return 0;
    if (strncmp(target, "bt", 2) == 0 || strcasecmp(target, "bluetooth") == 0 || strcasecmp(target, "bthid") == 0) return 1;

    static char s_cached_id[64] = "";
    static int s_cached_is_bt = 0;
    if (strcmp(s_cached_id, target) == 0) return s_cached_is_bt;

    int is_bt = 0;
    const char *dpath = "/data/harmony/config/devices.json";
    FILE *f = fopen(dpath, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz <= 1000000) {
            char *buf = (char *)malloc(sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, sz, f);
                buf[n] = 0;
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *item = NULL;
                        cJSON_ArrayForEach(item, devs) {
                            cJSON *j_id = cJSON_GetObjectItemCaseSensitive(item, "id");
                            if (!j_id) j_id = cJSON_GetObjectItemCaseSensitive(item, "Id");
                            char id_str[64] = {0};
                            if (j_id) {
                                if (cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%ld", (long)j_id->valuedouble);
                                else if (cJSON_IsString(j_id) && j_id->valuestring) strncpy(id_str, j_id->valuestring, sizeof(id_str) - 1);
                            }
                            cJSON *j_name = cJSON_GetObjectItemCaseSensitive(item, "name");
                            if (!j_name) j_name = cJSON_GetObjectItemCaseSensitive(item, "Name");
                            const char *name_str = (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : "";
                            if ((id_str[0] && strcmp(id_str, target) == 0) ||
                                (name_str && name_str[0] && strcasecmp(name_str, target) == 0)) {
                                cJSON *j_type = cJSON_GetObjectItemCaseSensitive(item, "type");
                                cJSON *j_cp = cJSON_GetObjectItemCaseSensitive(item, "controlPort");
                                if (j_type && cJSON_IsString(j_type) && strcasecmp(j_type->valuestring, "BT") == 0) {
                                    is_bt = 1;
                                } else if (j_cp && cJSON_IsNumber(j_cp) && (int)j_cp->valuedouble == 32) {
                                    is_bt = 1;
                                }
                                break;
                            }
                        }
                    } else {
                        cJSON *dwf = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                        if (dwf && cJSON_IsArray(dwf)) {
                            cJSON *item = NULL;
                            cJSON_ArrayForEach(item, dwf) {
                                cJSON *dev = cJSON_GetObjectItemCaseSensitive(item, "Device");
                                if (!dev) continue;
                                cJSON *j_id = cJSON_GetObjectItemCaseSensitive(dev, "Id-");
                                if (!j_id) j_id = cJSON_GetObjectItemCaseSensitive(dev, "Id");
                                if (!j_id) j_id = cJSON_GetObjectItemCaseSensitive(dev, "id");
                                char id_str[64] = {0};
                                if (j_id) {
                                    if (cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%ld", (long)j_id->valuedouble);
                                    else if (cJSON_IsString(j_id) && j_id->valuestring) strncpy(id_str, j_id->valuestring, sizeof(id_str) - 1);
                                }
                                cJSON *j_name = cJSON_GetObjectItemCaseSensitive(dev, "Name");
                                const char *name_str = (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : "";
                                if ((id_str[0] && strcmp(id_str, target) == 0) ||
                                    (name_str && name_str[0] && strcasecmp(name_str, target) == 0)) {
                                    cJSON *j_trans = cJSON_GetObjectItemCaseSensitive(dev, "Transport");
                                    if (!j_trans) j_trans = cJSON_GetObjectItemCaseSensitive(dev, "TransportType");
                                    if (j_trans && cJSON_IsNumber(j_trans) && (int)j_trans->valuedouble == 32) {
                                        is_bt = 1;
                                    }
                                    break;
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

    strncpy(s_cached_id, target, sizeof(s_cached_id) - 1);
    s_cached_is_bt = is_bt;
    return is_bt;
}

bool is_remote_release_packet(const uint8_t *data, uint16_t len) {
    if (!data || len < 1) return false;
    if (data[0] == 0x01 && len >= 3) {
        uint8_t k = (len >= 4 && data[1] == 0) ? data[3] : data[2];
        return (k == 0x00 && data[1] == 0);
    }
    if (len == 8) {
        uint8_t k = (data[1] == 0 && data[3] != 0) ? data[3] : data[2];
        return (k == 0x00 && data[0] == 0);
    }
    if (data[0] == 0x02 && len >= 3) {
        uint16_t cc = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
        return (cc == 0x0000);
    }
    if (len >= 2 && len < 8 && data[0] != 0x01) {
        uint16_t cc = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        return (cc == 0x0000);
    }
    return false;
}

static void remote_wake_grace_timeout_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    g_remote_wake_grace_active = false;
    hub_bt_log_debug("Remote wake grace window closed; full hold-mode active");
}

static void remote_watchdog_timeout_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    g_remote_wake_grace_active = false;
    g_remote_watchdog_active = false;
    if (g_active_remote_btn[0]) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Remote hold watchdog expired (auto-release): %s", g_active_remote_btn);
        hub_bt_log_debug(msg);
        dispatch_button_event(g_active_remote_btn, 0 /* release */);
        g_active_remote_btn[0] = 0;
    }
}

void dispatch_button_event(const char *btn, int is_press) {
    if (!btn || !btn[0]) return;
    if (is_press) {
        strncpy(g_last_btn, btn, sizeof(g_last_btn) - 1);
        g_last_seen = time(NULL);
        update_status("ready");

        struct stat est;
        if (stat(BT_EVENT_FILE, &est) == 0 && est.st_size > LOG_MAX_BYTES) {
            unlink(BT_EVENT_FILE ".1");
            rename(BT_EVENT_FILE, BT_EVENT_FILE ".1");
        }
        FILE *ef = fopen(BT_EVENT_FILE, "a");
        if (ef) {
            fprintf(ef, "{\"timestamp\":%ld,\"button\":\"%s\",\"bdaddr\":\"%s\",\"event\":\"down\"}\n",
                    (long)g_last_seen, btn, bd_addr_to_str(remote_device.addr));
            fclose(ef);
        }

        char msg[160];
        snprintf(msg, sizeof(msg), "Button pressed: %s", btn);
        hub_bt_log_debug(msg);

        /* Forward raw button event to daemon for MQTT (fire and forget) */
        char mqtt_body[256];
        snprintf(mqtt_body, sizeof(mqtt_body),
                 "{\"command\":\"%s\",\"action\":\"press\",\"source\":\"bt_remote\"}",
                 btn);
        trigger_http_post("/api/button-press", mqtt_body);
    } else {
        FILE *ef = fopen(BT_EVENT_FILE, "a");
        if (ef) {
            fprintf(ef, "{\"timestamp\":%ld,\"button\":\"%s\",\"bdaddr\":\"%s\",\"event\":\"up\"}\n",
                    (long)time(NULL), btn, bd_addr_to_str(remote_device.addr));
            fclose(ef);
        }
        char msg[160];
        snprintf(msg, sizeof(msg), "Button released: %s", btn);
        hub_bt_log_debug(msg);
        if (g_remote_wake_grace_active) {
            g_remote_wake_grace_active = false;
            btstack_run_loop_remove_timer(&g_remote_wake_grace_timer);
            hub_bt_log_debug("Remote first button released; wake grace window ended");
        }
    }

    char act_id[32] = "-1";
    read_current_activity(act_id, sizeof(act_id));

    cJSON *map = get_cached_map();
    if (!map) {
        if (!is_press) send_host_release_all();
        log_msg("Mapping file not available or invalid JSON");
        return;
    }

    cJSON *activities = cJSON_GetObjectItemCaseSensitive(map, "activities");
    if (!activities || !cJSON_IsObject(activities)) activities = map;

    cJSON *btn_obj = NULL;
    if (act_id[0] && strcmp(act_id, "-1") != 0) {
        cJSON *act_obj = cJSON_GetObjectItemCaseSensitive(activities, act_id);
        if (act_obj && cJSON_IsObject(act_obj)) {
            btn_obj = cJSON_GetObjectItemCaseSensitive(act_obj, btn);
        }
    }
    if (!btn_obj) {
        cJSON *def_obj = cJSON_GetObjectItemCaseSensitive(activities, "-1");
        if (!def_obj) def_obj = cJSON_GetObjectItemCaseSensitive(activities, "default");
        if (def_obj && cJSON_IsObject(def_obj)) {
            btn_obj = cJSON_GetObjectItemCaseSensitive(def_obj, btn);
        }
    }

    if (!btn_obj || !cJSON_IsObject(btn_obj)) {
        if (!is_press) send_host_release_all();
        char msg[160];
        snprintf(msg, sizeof(msg), "Button '%s' unmapped for activity %s", btn, act_id);
        hub_bt_log_debug(msg);
        return;
    }

    cJSON *j_act = cJSON_GetObjectItemCaseSensitive(btn_obj, "action");
    if (!j_act) j_act = cJSON_GetObjectItemCaseSensitive(btn_obj, "type");
    const char *action = (j_act && cJSON_IsString(j_act)) ? j_act->valuestring : "";

    cJSON *j_tgt = cJSON_GetObjectItemCaseSensitive(btn_obj, "targetDevice");
    if (!j_tgt) j_tgt = cJSON_GetObjectItemCaseSensitive(btn_obj, "deviceId");
    if (!j_tgt) j_tgt = cJSON_GetObjectItemCaseSensitive(btn_obj, "activityId");
    const char *target = (j_tgt && cJSON_IsString(j_tgt)) ? j_tgt->valuestring : "";

    cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(btn_obj, "command");
    const char *cmd = (j_cmd && cJSON_IsString(j_cmd)) ? j_cmd->valuestring : "";

    char msg[160];
    if (strcmp(action, "passthrough_bt") == 0 && cmd[0]) {
        if (!g_host_connected || !g_host_hid_cid) {
            if (!g_host_reconnect_active) {
                bd_addr_t tgt;
                if (get_pinned_host_target(tgt)) start_host_reconnect(tgt, 20);
                else try_connect_host();
            }
        }
        if (g_host_connected && g_host_hid_cid) {
            if (is_press) {
                int hold_mode = g_remote_wake_grace_active ? 0 : 1;
                send_host_named_key_ex(cmd, hold_mode);
                snprintf(msg, sizeof(msg), "Dispatched passthrough_bt %s: %s%s", hold_mode ? "hold" : "tap", cmd, g_remote_wake_grace_active ? " (wake-grace tap)" : "");
            } else {
                send_host_release_all();
                snprintf(msg, sizeof(msg), "Dispatched passthrough_bt release: %s", cmd);
            }
        } else {
            snprintf(msg, sizeof(msg), "passthrough_bt dropped (host connecting/offline): %s", cmd);
        }
        hub_bt_log_debug(msg);
    } else if (strcmp(action, "device_cmd") == 0 && target[0] && cmd[0]) {
        if (is_bt_device_id(target)) {
            if (!g_host_connected || !g_host_hid_cid) {
                if (!g_host_reconnect_active) {
                    bd_addr_t tgt;
                    if (get_pinned_host_target(tgt)) start_host_reconnect(tgt, 20);
                    else try_connect_host();
                }
            }
            if (g_host_connected && g_host_hid_cid) {
                if (is_press) {
                    int hold_mode = g_remote_wake_grace_active ? 0 : 1;
                    if (send_host_named_key_ex(cmd, hold_mode)) {
                        snprintf(msg, sizeof(msg), "Dispatched device_cmd %s to BT host: dev=%s cmd=%s%s", hold_mode ? "hold" : "tap", target, cmd, g_remote_wake_grace_active ? " (wake-grace tap)" : "");
                        hub_bt_log_debug(msg);
                        return;
                    }
                } else {
                    send_host_release_all();
                    snprintf(msg, sizeof(msg), "Dispatched device_cmd release to BT host: dev=%s cmd=%s", target, cmd);
                    hub_bt_log_debug(msg);
                    return;
                }
            } else {
                snprintf(msg, sizeof(msg), "device_cmd for BT host dev=%s cmd=%s dropped (host connecting/offline)", target, cmd);
                hub_bt_log_debug(msg);
                return;
            }
        }
        if (is_press) {
            char body[256];
            snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"command\":\"%s\"}", target, cmd);
            trigger_http_post("/api/ir-send", body);
            snprintf(msg, sizeof(msg), "Dispatched device_cmd: dev=%s cmd=%s", target, cmd);
            hub_bt_log_debug(msg);
        }
    } else if (is_press) {
        if (strcmp(action, "activity_start") == 0 && target[0]) {
            char body[64];
            snprintf(body, sizeof(body), "{\"id\":\"%s\"}", target);
            trigger_http_post("/api/activity-start", body);
            snprintf(msg, sizeof(msg), "Dispatched activity_start: %s", target);
            hub_bt_log_debug(msg);
        } else if (strcmp(action, "activity_stop") == 0) {
            trigger_http_post("/api/activity-stop", "{}");
            hub_bt_log_debug("Dispatched activity_stop");
        } else {
            snprintf(msg, sizeof(msg), "Unhandled mapping: act=%s tgt=%s cmd=%s", action, target, cmd);
            hub_bt_log_debug(msg);
        }
    }
}

void dispatch_button(const char *btn) {
    dispatch_button_event(btn, 1);
}

void handle_gatt_notification_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    uint16_t val_len = gatt_event_notification_get_value_length(packet);
    const uint8_t *val = gatt_event_notification_get_value(packet);
    if (!val || val_len == 0) return;

    if (access(BT_DEBUG_FLAG, F_OK) == 0) {
        char hex_buf[128];
        int hpos = snprintf(hex_buf, sizeof(hex_buf), "GATT notify len=%u: ", val_len);
        for (uint16_t i = 0; i < val_len && i < 16; i++) {
            hpos += snprintf(hex_buf + hpos, sizeof(hex_buf) - hpos, "%02X ", val[i]);
        }
        log_msg(hex_buf);
    }

    if (is_remote_release_packet(val, val_len)) {
        if (g_remote_watchdog_active) {
            btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
            g_remote_watchdog_active = false;
        }
        if (g_active_remote_btn[0]) {
            dispatch_button_event(g_active_remote_btn, 0 /* release */);
            g_active_remote_btn[0] = 0;
        } else {
            send_host_release_all();
        }
        return;
    }

    const char *btn = parse_button(val, val_len);
    if (btn) {
        if (app_state != APP_READY) {
            app_state = APP_READY;
            update_status("ready");
        }
        if (g_active_remote_btn[0] && strcmp(g_active_remote_btn, btn) == 0) {
            if (g_remote_watchdog_active) {
                btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
            }
            btstack_run_loop_set_timer_handler(&g_remote_watchdog_timer, remote_watchdog_timeout_handler);
            btstack_run_loop_set_timer(&g_remote_watchdog_timer, 10000);
            btstack_run_loop_add_timer(&g_remote_watchdog_timer);
            g_remote_watchdog_active = true;
            return;
        }
        if (g_active_remote_btn[0]) {
            dispatch_button_event(g_active_remote_btn, 0);
            g_active_remote_btn[0] = 0;
        }
        strncpy(g_active_remote_btn, btn, sizeof(g_active_remote_btn) - 1);
        dispatch_button_event(btn, 1 /* press/hold */);

        if (g_remote_watchdog_active) {
            btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
        }
        btstack_run_loop_set_timer_handler(&g_remote_watchdog_timer, remote_watchdog_timeout_handler);
        btstack_run_loop_set_timer(&g_remote_watchdog_timer, 10000);
        btstack_run_loop_add_timer(&g_remote_watchdog_timer);
        g_remote_watchdog_active = true;
    }
}

void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                log_msg("HIDS client connected successfully");
                g_remote_wake_grace_active = true;
                btstack_run_loop_remove_timer(&g_remote_wake_grace_timer);
                btstack_run_loop_set_timer_handler(&g_remote_wake_grace_timer, remote_wake_grace_timeout_handler);
                btstack_run_loop_set_timer(&g_remote_wake_grace_timer, 2500);
                btstack_run_loop_add_timer(&g_remote_wake_grace_timer);
                app_state = APP_READY;
                update_status("ready");
                if (tlv_impl) {
                    tlv_impl->store_tag(&tlv_context, TLV_TAG_HOGD, (const uint8_t *)&remote_device, sizeof(remote_device));
                }
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "HIDS client connection failed (status=0x%02x)", status);
                log_msg(msg);
                gap_disconnect(connection_handle);
                schedule_reconnect(2000);
            }
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            log_msg("HIDS client disconnected");
            g_remote_wake_grace_active = true;
            btstack_run_loop_remove_timer(&g_remote_wake_grace_timer);
            if (g_remote_watchdog_active) {
                btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
                g_remote_watchdog_active = false;
            }
            if (g_active_remote_btn[0]) {
                dispatch_button_event(g_active_remote_btn, 0);
                g_active_remote_btn[0] = 0;
            } else {
                send_host_release_all();
            }
            app_state = APP_IDLE;
            update_status("disconnected");
            schedule_reconnect(1000);
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            const uint8_t *report = gattservice_subevent_hid_report_get_report(packet);
            uint16_t report_len = gattservice_subevent_hid_report_get_report_len(packet);
            if (access(BT_DEBUG_FLAG, F_OK) == 0) {
                char hex_buf[128];
                int hpos = snprintf(hex_buf, sizeof(hex_buf), "HID report len=%u: ", report_len);
                for (uint16_t i = 0; i < report_len && i < 16; i++) {
                    hpos += snprintf(hex_buf + hpos, sizeof(hex_buf) - hpos, "%02X ", report[i]);
                }
                log_msg(hex_buf);
            }

            if (is_remote_release_packet(report, report_len)) {
                if (g_remote_watchdog_active) {
                    btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
                    g_remote_watchdog_active = false;
                }
                if (g_active_remote_btn[0]) {
                    dispatch_button_event(g_active_remote_btn, 0 /* release */);
                    g_active_remote_btn[0] = 0;
                } else {
                    send_host_release_all();
                }
                break;
            }

            const char *btn = parse_button(report, report_len);
            if (btn) {
                if (g_active_remote_btn[0]) {
                    dispatch_button_event(g_active_remote_btn, 0);
                    g_active_remote_btn[0] = 0;
                }
                strncpy(g_active_remote_btn, btn, sizeof(g_active_remote_btn) - 1);
                dispatch_button(btn);
            }
            break;
        }
        default:
            break;
    }
}

void bt_remote_on_disconnect(void) {
    g_remote_wake_grace_active = true;
    btstack_run_loop_remove_timer(&g_remote_wake_grace_timer);
    if (g_remote_watchdog_active) {
        btstack_run_loop_remove_timer(&g_remote_watchdog_timer);
        g_remote_watchdog_active = false;
    }
    if (g_active_remote_btn[0]) {
        dispatch_button_event(g_active_remote_btn, 0);
        g_active_remote_btn[0] = 0;
    } else {
        send_host_release_all();
    }
}
