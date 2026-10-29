#include "bt_pairing.h"

discovered_remote_t g_discovered[MAX_DISCOVERED];
int g_discovered_count = 0;
bool g_user_scan_active = false;
time_t g_user_scan_end = 0;

uint16_t get_target_host_hid_cid(void) {
    if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS) {
        if (g_hosts[g_active_host_idx].connected && g_hosts[g_active_host_idx].hid_cid) {
            return g_hosts[g_active_host_idx].hid_cid;
        }
    }
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (g_hosts[i].connected && g_hosts[i].hid_cid) {
            return g_hosts[i].hid_cid;
        }
    }
    return 0;
}

bool is_any_host_connected(void) {
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (g_hosts[i].connected && g_hosts[i].hid_cid) return true;
    }
    return false;
}

host_conn_t * register_host_target(const bd_addr_t addr, const char *name) {
    if (is_null_bd_addr(addr)) return NULL;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (memcmp(g_hosts[i].addr, addr, 6) == 0) {
            if (name && name[0] && (!g_hosts[i].name[0] || strcmp(g_hosts[i].name, "Harmony Keyboard") == 0)) {
                strncpy(g_hosts[i].name, name, sizeof(g_hosts[i].name) - 1);
            }
            return &g_hosts[i];
        }
    }
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (is_null_bd_addr(g_hosts[i].addr)) {
            memcpy(g_hosts[i].addr, addr, 6);
            if (name && name[0]) {
                strncpy(g_hosts[i].name, name, sizeof(g_hosts[i].name) - 1);
            } else {
                snprintf(g_hosts[i].name, sizeof(g_hosts[i].name), "Host %s", bd_addr_to_str(addr));
            }
            return &g_hosts[i];
        }
    }
    return NULL;
}

void update_bthid_status(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddBoolToObject(root, "runtime", 1);
    cJSON_AddNumberToObject(root, "pid", (double)getpid());
    cJSON_AddNumberToObject(root, "updated", (double)time(NULL));
    bool any_conn = is_any_host_connected();
    cJSON_AddStringToObject(root, "state", any_conn ? "listening" : (g_host_pairing ? "pairing" : "waiting_host"));
    host_conn_t *ah = (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS) ? &g_hosts[g_active_host_idx] : NULL;
    cJSON_AddStringToObject(root, "target", ah ? bd_addr_to_str(ah->addr) : (any_conn ? bd_addr_to_str(g_host_addr) : ""));
    cJSON_AddNumberToObject(root, "sent", (double)g_keys_sent);
    cJSON_AddNumberToObject(root, "skipped", 0);
    cJSON_AddStringToObject(root, "error", "");

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return;

    FILE *f = fopen(BTHID_STATUS_FILE ".new", "w");
    if (f) {
        fputs(out, f);
        fputc('\n', f);
        fclose(f);
        rename(BTHID_STATUS_FILE ".new", BTHID_STATUS_FILE);
    }
    free(out);
}

void update_status(const char *state_str) {
    FILE *f = fopen(STATUS_FILE, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"backend\": \"btstack\",\n");
        fprintf(f, "  \"pid\": %d,\n", (int)getpid());
        fprintf(f, "  \"state\": \"%s\",\n", state_str);
        fprintf(f, "  \"target\": \"%s\",\n", bd_addr_to_str(remote_device.addr));
        fprintf(f, "  \"last_button\": \"%s\",\n", g_last_btn);
        fprintf(f, "  \"last_seen\": %ld,\n", (long)g_last_seen);
        fprintf(f, "  \"remote\": {\n");
        fprintf(f, "    \"connected\": %s,\n", (connection_handle != HCI_CON_HANDLE_INVALID) ? "true" : "false");
        fprintf(f, "    \"addr\": \"%s\"\n", bd_addr_to_str(remote_device.addr));
        fprintf(f, "  },\n");
        fprintf(f, "  \"host\": {\n");
        uint16_t active_cid = get_target_host_hid_cid();
        host_conn_t *ah = (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS) ? &g_hosts[g_active_host_idx] : NULL;
        bool host_conn = (active_cid != 0);
        fprintf(f, "    \"connected\": %s,\n", host_conn ? "true" : "false");
        fprintf(f, "    \"addr\": \"%s\",\n", ah ? bd_addr_to_str(ah->addr) : (g_host_connected ? bd_addr_to_str(g_host_addr) : ""));
        fprintf(f, "    \"name\": \"%s\",\n", (ah && ah->name[0]) ? ah->name : g_host_name);
        fprintf(f, "    \"pairing\": %s\n", g_host_pairing ? "true" : "false");
        fprintf(f, "  },\n");
        fprintf(f, "  \"hosts\": [\n");
        bool first = true;
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (is_null_bd_addr(g_hosts[i].addr)) continue;
            if (!first) fprintf(f, ",\n");
            first = false;
            fprintf(f, "    {\"addr\": \"%s\", \"name\": \"%s\", \"connected\": %s, \"cid\": %u}",
                    bd_addr_to_str(g_hosts[i].addr), g_hosts[i].name,
                    g_hosts[i].connected ? "true" : "false", g_hosts[i].hid_cid);
        }
        fprintf(f, "\n  ]\n");
        fprintf(f, "}\n");
        fclose(f);
    }
    update_bthid_status();
}

int get_pinned_target(bd_addr_t addr) {
    FILE *f = fopen(TARGET_FILE, "r");
    if (!f) f = fopen(MAP_FILE, "r");
    if (!f) return 0;
    char buf[1024];
    size_t rn = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rn] = 0;
    if (parse_bd_addr(buf, addr)) return 1;
    char *p = strstr(buf, "bdaddr");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            char mac[32] = "";
            int i = 0;
            while (*p && i < 20) {
                if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') || *p == ':') {
                    mac[i++] = *p;
                } else if (i > 0) {
                    break;
                }
                p++;
            }
            mac[i] = 0;
            if (parse_bd_addr(mac, addr)) return 1;
        }
    }
    return 0;
}

int get_pinned_host_target(bd_addr_t addr) {
    const char *files[] = {"/data/harmony/bthid_host_target", "/data/harmony/bt-devices.json", "/data/harmony/config/devices.json", NULL};
    for (int fi = 0; files[fi]; fi++) {
        FILE *f = fopen(files[fi], "r");
        if (!f) continue;
        char buf[4096];
        size_t rn = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[rn] = 0;
        for (char *p = buf; *p; p++) {
            if (isxdigit((unsigned char)*p) && p[2] == ':' && p[5] == ':' && p[8] == ':' && p[11] == ':' && p[14] == ':') {
                char mac[18];
                strncpy(mac, p, 17);
                mac[17] = 0;
                if (parse_bd_addr(mac, addr) && !is_null_bd_addr(addr)) return 1;
            }
        }
    }
    return 0;
}

void link_bt_device_to_addr(const char *dev_id, const char *addr_str) {
    if (!dev_id || !dev_id[0] || !addr_str || !addr_str[0]) return;
    char msg[128];
    snprintf(msg, sizeof(msg), "Linking device %s to Bluetooth address %s", dev_id, addr_str);
    log_msg(msg);

    FILE *bf = fopen("/data/harmony/bt-devices.json", "r");
    cJSON *root = NULL;
    if (bf) {
        fseek(bf, 0, SEEK_END);
        long sz = ftell(bf);
        fseek(bf, 0, SEEK_SET);
        if (sz > 0 && sz < 65536) {
            char *buf = (char *)malloc(sz + 1);
            if (buf) {
                size_t r = fread(buf, 1, sz, bf);
                buf[r] = 0;
                root = cJSON_Parse(buf);
                free(buf);
            }
        }
        fclose(bf);
    }
    if (!root) {
        root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "version", 1);
        cJSON_AddItemToObject(root, "devices", cJSON_CreateArray());
    }
    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs || !cJSON_IsArray(devs)) {
        cJSON_DeleteItemFromObject(root, "devices");
        devs = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "devices", devs);
    }

    cJSON *found = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devs) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (jid && cJSON_IsString(jid) && strcmp(jid->valuestring, dev_id) == 0) {
            found = item;
            break;
        }
    }
    if (found) {
        cJSON_ReplaceItemInObject(found, "bdaddr", cJSON_CreateString(addr_str));
    } else {
        cJSON *new_d = cJSON_CreateObject();
        cJSON_AddStringToObject(new_d, "id", dev_id);
        cJSON_AddStringToObject(new_d, "name", dev_id);
        cJSON_AddStringToObject(new_d, "type", "btkeyboard");
        cJSON_AddStringToObject(new_d, "bdaddr", addr_str);
        cJSON_AddItemToObject(new_d, "commands", cJSON_CreateArray());
        cJSON_AddItemToArray(devs, new_d);
    }

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (out) {
        FILE *wf = fopen("/data/harmony/bt-devices.json.tmp", "w");
        if (wf) {
            fputs(out, wf);
            fputc('\n', wf);
            fclose(wf);
            rename("/data/harmony/bt-devices.json.tmp", "/data/harmony/bt-devices.json");
        }
        free(out);
    }

    /* Also persist to canonical config/devices.json */
    const char *dpath = "/data/harmony/config/devices.json";
    FILE *df = fopen(dpath, "r");
    if (df) {
        fseek(df, 0, SEEK_END);
        long dsz = ftell(df);
        fseek(df, 0, SEEK_SET);
        if (dsz > 0 && dsz < 1000000) {
            char *dbuf = (char *)malloc(dsz + 1);
            if (dbuf) {
                size_t dr = fread(dbuf, 1, dsz, df);
                dbuf[dr] = 0;
                cJSON *droot = cJSON_Parse(dbuf);
                free(dbuf);
                if (droot) {
                    cJSON *ddevs = cJSON_GetObjectItemCaseSensitive(droot, "devices");
                    if (!ddevs) ddevs = cJSON_GetObjectItemCaseSensitive(droot, "DevicesWithFeatures");
                    if (ddevs && cJSON_IsArray(ddevs)) {
                        cJSON *ditem = NULL;
                        cJSON_ArrayForEach(ditem, ddevs) {
                            cJSON *d = cJSON_GetObjectItemCaseSensitive(ditem, "Device");
                            if (!d) d = ditem;
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                            char d_id_str[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON_DeleteItemFromObjectCaseSensitive(d, "BTAddress");
                                cJSON_DeleteItemFromObjectCaseSensitive(d, "bt_host");
                                cJSON_DeleteItemFromObjectCaseSensitive(d, "btHost");
                                cJSON_DeleteItemFromObjectCaseSensitive(d, "bdaddr");
                                cJSON_AddStringToObject(d, "bdaddr", addr_str);
                                char *dout = cJSON_Print(droot);
                                if (dout) {
                                    char tmp_dpath[128];
                                    snprintf(tmp_dpath, sizeof(tmp_dpath), "%s.tmp", dpath);
                                    FILE *dwf = fopen(tmp_dpath, "w");
                                    if (dwf) {
                                        fputs(dout, dwf);
                                        fputc('\n', dwf);
                                        fclose(dwf);
                                        rename(tmp_dpath, dpath);
                                    }
                                    free(dout);
                                }
                                break;
                            }
                        }
                    }
                    cJSON_Delete(droot);
                }
            }
        }
        fclose(df);
    }
}

const char *get_default_host_name(void) {
    static char s_host_name[64] = "";
    s_host_name[0] = '\0';
    FILE *f = fopen("/data/harmony/config/devices.json", "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz < 200000) {
        char *buf = (char *)malloc((size_t)sz + 1);
        if (buf) {
            size_t r = fread(buf, 1, sz, f);
            buf[r] = 0;
            cJSON *root = cJSON_Parse(buf);
            free(buf);
            if (root) {
                cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                if (devs && cJSON_IsArray(devs)) {
                    cJSON *di = NULL;
                    cJSON_ArrayForEach(di, devs) {
                        cJSON *d = cJSON_GetObjectItemCaseSensitive(di, "Device");
                        if (!d) d = di;
                        cJSON *t = cJSON_GetObjectItemCaseSensitive(d, "type");
                        cJSON *n = cJSON_GetObjectItemCaseSensitive(d, "name");
                        cJSON *tr = cJSON_GetObjectItemCaseSensitive(d, "transport");
                        const char *tstr = (t && t->valuestring) ? t->valuestring : "";
                        const char *nstr = (n && n->valuestring) ? n->valuestring : "";
                        int transport = (tr && cJSON_IsNumber(tr)) ? tr->valueint : 0;
                        if (transport == 32 || strcasestr(tstr, "game") || strcasestr(tstr, "bt") || strcasestr(tstr, "bluetooth")) {
                            if (nstr[0]) {
                                strncpy(s_host_name, nstr, sizeof(s_host_name) - 1);
                                s_host_name[sizeof(s_host_name) - 1] = 0;
                                cJSON_Delete(root);
                                fclose(f);
                                return s_host_name;
                            }
                        }
                    }
                }
                cJSON_Delete(root);
            }
        }
    }
    fclose(f);
    return s_host_name[0] ? s_host_name : NULL;
}

void init_known_hosts(void) {
    bd_addr_t def_target;
    if (get_pinned_host_target(def_target)) {
        register_host_target(def_target, get_default_host_name());
    }

    FILE *bf = fopen("/data/harmony/bt-devices.json", "r");
    if (bf) {
        fseek(bf, 0, SEEK_END);
        long sz = ftell(bf);
        fseek(bf, 0, SEEK_SET);
        if (sz > 0 && sz < 65536) {
            char *buf = (char *)malloc(sz + 1);
            if (buf) {
                size_t r = fread(buf, 1, sz, bf);
                buf[r] = 0;
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *item = NULL;
                        cJSON_ArrayForEach(item, devs) {
                            cJSON *ja = cJSON_GetObjectItemCaseSensitive(item, "bdaddr");
                            cJSON *jn = cJSON_GetObjectItemCaseSensitive(item, "name");
                            if (ja && cJSON_IsString(ja) && ja->valuestring[0]) {
                                bd_addr_t ba;
                                if (parse_bd_addr(ja->valuestring, ba)) {
                                    const char *nm = (jn && cJSON_IsString(jn)) ? jn->valuestring : NULL;
                                    register_host_target(ba, nm);
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(bf);
    }

    const char *dpath = "/data/harmony/config/devices.json";
    FILE *df = fopen(dpath, "r");
    if (df) {
        fseek(df, 0, SEEK_END);
        long sz = ftell(df);
        fseek(df, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000) {
            char *buf = (char *)malloc(sz + 1);
            if (buf) {
                size_t r = fread(buf, 1, sz, df);
                buf[r] = 0;
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *item = NULL;
                        cJSON_ArrayForEach(item, devs) {
                            cJSON *ja = cJSON_GetObjectItemCaseSensitive(item, "bdaddr");
                            if (!ja) ja = cJSON_GetObjectItemCaseSensitive(item, "BTAddress");
                            cJSON *jn = cJSON_GetObjectItemCaseSensitive(item, "name");
                            if (!jn) jn = cJSON_GetObjectItemCaseSensitive(item, "Name");
                            if (ja && cJSON_IsString(ja) && ja->valuestring[0]) {
                                bd_addr_t ba;
                                if (parse_bd_addr(ja->valuestring, ba)) {
                                    const char *nm = (jn && cJSON_IsString(jn)) ? jn->valuestring : NULL;
                                    register_host_target(ba, nm);
                                }
                            }
                        }
                    } else {
                        cJSON *dwf = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                        if (dwf && cJSON_IsArray(dwf)) {
                            cJSON *item = NULL;
                            cJSON_ArrayForEach(item, dwf) {
                                cJSON *dev = cJSON_GetObjectItemCaseSensitive(item, "Device");
                                if (!dev) continue;
                                cJSON *ja = cJSON_GetObjectItemCaseSensitive(dev, "BTAddress");
                                cJSON *jn = cJSON_GetObjectItemCaseSensitive(dev, "Name");
                                if (ja && cJSON_IsString(ja) && ja->valuestring[0]) {
                                    bd_addr_t ba;
                                    if (parse_bd_addr(ja->valuestring, ba)) {
                                        const char *nm = (jn && cJSON_IsString(jn)) ? jn->valuestring : NULL;
                                        register_host_target(ba, nm);
                                    }
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(df);
    }
}

void try_connect_host_addr(const bd_addr_t addr) {
    if (is_null_bd_addr(addr)) return;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (g_hosts[i].connected && g_hosts[i].hid_cid != 0 && memcmp(g_hosts[i].addr, addr, 6) == 0) {
            return;
        }
    }
    if (g_host_connecting && (time(NULL) - g_host_connecting_start) < 15) {
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Initiating connection to Classic HID Host: %s", bd_addr_to_str(addr));
    log_msg(msg);
    g_host_connecting = true;
    g_host_connecting_start = time(NULL);
    uint16_t cid = 0;
    uint8_t status = hid_device_connect((uint8_t*)addr, &cid);
    if (status != ERROR_CODE_SUCCESS || cid == 0) {
        g_host_connecting = false;
        snprintf(msg, sizeof(msg), "Classic HID Host connect request failed (status=0x%02x, cid=0x%04x)", status, cid);
        log_msg(msg);
        return;
    }
    host_conn_t *h = register_host_target(addr, NULL);
    if (h) {
        h->hid_cid = cid;
    }
}

void try_connect_host(void) {
    bool found = false;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (!is_null_bd_addr(g_hosts[i].addr)) {
            found = true;
            if (!g_hosts[i].connected) {
                try_connect_host_addr(g_hosts[i].addr);
            }
        }
    }
    if (!found) {
        if (!is_null_bd_addr(g_host_addr)) {
            try_connect_host_addr(g_host_addr);
        } else {
            bd_addr_t target;
            if (get_pinned_host_target(target)) {
                register_host_target(target, get_default_host_name());
                try_connect_host_addr(target);
            }
        }
    }
}

static void host_reconnect_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (!g_host_reconnect_active) return;

    if (is_any_host_connected()) {
        log_msg("Host reconnect: Host connected, stopping retry loop");
        g_host_reconnect_active = false;
        g_host_connecting = false;
        return;
    }

    if (g_host_reconnect_attempts >= MAX_HOST_RECONNECT_ATTEMPTS) {
        log_msg("Host reconnect: Maximum retry attempts reached");
        g_host_reconnect_active = false;
        g_host_connecting = false;
        return;
    }

    if (g_host_connecting && (time(NULL) - g_host_connecting_start) < 15) {
        btstack_run_loop_set_timer(&g_host_reconnect_timer, 4000);
        btstack_run_loop_add_timer(&g_host_reconnect_timer);
        return;
    }

    g_host_reconnect_attempts++;
    char msg[128];
    snprintf(msg, sizeof(msg), "Host reconnect: Attempt %d/%d to %s",
             g_host_reconnect_attempts, MAX_HOST_RECONNECT_ATTEMPTS,
             is_null_bd_addr(g_host_reconnect_target) ? "targets" : bd_addr_to_str(g_host_reconnect_target));
    log_msg(msg);

    if (!is_null_bd_addr(g_host_reconnect_target)) {
        try_connect_host_addr(g_host_reconnect_target);
    } else {
        try_connect_host();
    }

    btstack_run_loop_set_timer(&g_host_reconnect_timer, 7000);
    btstack_run_loop_add_timer(&g_host_reconnect_timer);
}

void start_host_reconnect(const bd_addr_t target, int max_attempts) {
    (void)max_attempts;
    if (is_any_host_connected()) {
        log_msg("Host reconnect: Host already connected");
        return;
    }

    if (g_host_reconnect_active && !is_null_bd_addr(target) && memcmp(g_host_reconnect_target, target, 6) == 0 && g_host_connecting) {
        return;
    }

    if (!is_null_bd_addr(target)) {
        memcpy(g_host_reconnect_target, target, 6);
    } else {
        memset(g_host_reconnect_target, 0, 6);
    }
    g_host_reconnect_attempts = 0;
    g_host_reconnect_active = true;

    char msg[128];
    snprintf(msg, sizeof(msg), "Starting host reconnect loop for %s (105s window, 7s interval)",
             is_null_bd_addr(g_host_reconnect_target) ? "targets" : bd_addr_to_str(g_host_reconnect_target));
    log_msg(msg);

    if (!is_null_bd_addr(g_host_reconnect_target)) {
        try_connect_host_addr(g_host_reconnect_target);
    } else {
        try_connect_host();
    }

    btstack_run_loop_remove_timer(&g_host_reconnect_timer);
    btstack_run_loop_set_timer(&g_host_reconnect_timer, 7000);
    btstack_run_loop_set_timer_handler(&g_host_reconnect_timer, host_reconnect_timer_handler);
    btstack_run_loop_add_timer(&g_host_reconnect_timer);
}

void stop_host_reconnect(void) {
    if (g_host_reconnect_active) {
        g_host_reconnect_active = false;
        btstack_run_loop_remove_timer(&g_host_reconnect_timer);
        log_msg("Host reconnect: Loop stopped");
    }
    g_host_connecting = false;
}

static void hog_reconnect_timeout(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (app_state == APP_READY) return;
    if (app_state == APP_CONNECTING) {
        log_msg("Connection attempt timed out (10s); cancelling and resuming scan");
        gap_connect_cancel();
        hog_start_scan();
        return;
    }
    log_msg("Reconnect timer fired: starting scan");
    hog_start_scan();
}

void schedule_reconnect(uint32_t delay_ms) {
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_set_timer(&reconnect_timer, delay_ms);
    btstack_run_loop_set_timer_handler(&reconnect_timer, &hog_reconnect_timeout);
    btstack_run_loop_add_timer(&reconnect_timer);
}

void write_scan_results(bool scanning) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddBoolToObject(root, "scanning", scanning);
    cJSON *devs = cJSON_CreateArray();
    for (int i = 0; i < g_discovered_count; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "addr", bd_addr_to_str(g_discovered[i].addr));
        cJSON_AddStringToObject(d, "name", g_discovered[i].name[0] ? g_discovered[i].name : "Bluetooth Remote");
        cJSON_AddNumberToObject(d, "rssi", g_discovered[i].rssi);
        cJSON_AddBoolToObject(d, "pairing", 1);
        cJSON_AddItemToArray(devs, d);
    }
    cJSON_AddItemToObject(root, "devices", devs);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return;

    FILE *f = fopen(SCAN_RESULT_FILE ".tmp", "w");
    if (f) {
        fputs(out, f);
        fputc('\n', f);
        fclose(f);
        rename(SCAN_RESULT_FILE ".tmp", SCAN_RESULT_FILE);
    }
    free(out);
}

void hog_start_scan(void) {
    bd_addr_t pinned;
    if (get_pinned_target(pinned)) {
        update_status("standby");
        gap_set_scan_parameters(0, 48, 48); // passive scanning: no SCAN_REQ delay/collision
    } else {
        update_status("scanning");
        gap_set_scan_parameters(1, 48, 48); // active scanning for discovery
    }
    app_state = APP_SCANNING;
    gap_start_scan();
}

void hog_connect(void) {
    log_msg("Connecting to target BLE remote...");
    app_state = APP_CONNECTING;
    update_status("connecting");
    gap_stop_scan();
    gap_connect(remote_device.addr, remote_device.addr_type);
    schedule_reconnect(10000); // 10s connection timeout
}

void hog_start_connect(void) {
    hog_start_scan();
}

void start_user_scan(void) {
    log_msg("User requested BLE scan for remotes in pairing mode...");
    g_discovered_count = 0;
    memset(g_discovered, 0, sizeof(g_discovered));
    g_user_scan_active = true;
    g_user_scan_end = time(NULL) + 8;

    btstack_run_loop_remove_timer(&reconnect_timer);
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(connection_handle);
    }
    write_scan_results(true);

    app_state = APP_SCANNING;
    update_status("scanning");
    gap_set_scan_parameters(1, 48, 48);
    gap_start_scan();
}

void start_user_pair(const char *addr_str) {
    bd_addr_t new_addr;
    if (!parse_bd_addr(addr_str, new_addr)) {
        log_msg("Invalid pair address requested");
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "User pair requested for %s", addr_str);
    log_msg(msg);

    FILE *tf = fopen(TARGET_FILE, "w");
    if (tf) {
        fprintf(tf, "%s\n", addr_str);
        fclose(tf);
    }

    g_user_scan_active = false;
    btstack_run_loop_remove_timer(&reconnect_timer);
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(connection_handle);
    }
    gap_stop_scan();

    gap_delete_bonding(BD_ADDR_TYPE_LE_PUBLIC, new_addr);
    gap_delete_bonding(BD_ADDR_TYPE_LE_RANDOM, new_addr);

    memcpy(remote_device.addr, new_addr, 6);
    remote_device.addr_type = BD_ADDR_TYPE_LE_PUBLIC;
    for (int i = 0; i < g_discovered_count; i++) {
        if (memcmp(g_discovered[i].addr, new_addr, 6) == 0) {
            remote_device.addr_type = (bd_addr_type_t)g_discovered[i].addr_type;
            break;
        }
    }

    app_state = APP_SCANNING;
    update_status("pairing");
    gap_set_scan_parameters(1, 48, 48);
    gap_start_scan();
}
