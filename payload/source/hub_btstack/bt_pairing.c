#include "bt_pairing.h"

discovered_remote_t g_discovered[MAX_DISCOVERED];
int g_discovered_count = 0;
bool g_user_scan_active = false;
time_t g_user_scan_end = 0;

uint16_t get_target_host_hid_cid(void) {
    /* Once a target host is chosen (TARGET line), its keys go to it or nowhere: falling back to
     * another connected host sent e.g. Shield keys to a TV. Without a target (g_active_host_idx
     * -1, e.g. the web UI's "Auto") any connected host is used, as before. */
    if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS) {
        if (g_hosts[g_active_host_idx].connected && g_hosts[g_active_host_idx].hid_cid) {
            return g_hosts[g_active_host_idx].hid_cid;
        }
        return 0;
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

/* Every host with an address is connected (a null-target reconnect loop connects them all);
 * false with no known host, so such a loop still tries the fallbacks in try_connect_host() */
bool all_known_hosts_connected(void) {
    bool any = false;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (is_null_bd_addr(g_hosts[i].addr)) continue;
        any = true;
        if (!g_hosts[i].connected || !g_hosts[i].hid_cid) return false;
    }
    return any;
}

bool is_host_connected(const bd_addr_t addr) {
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (memcmp(g_hosts[i].addr, addr, 6) == 0 && g_hosts[i].connected && g_hosts[i].hid_cid) return true;
    }
    return false;
}

bool host_manual_disconnect(const bd_addr_t addr) {
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (!is_null_bd_addr(g_hosts[i].addr) && memcmp(g_hosts[i].addr, addr, 6) == 0) {
            return g_hosts[i].manual_disconnect;
        }
    }
    return false;
}

void set_host_manual_disconnect(const bd_addr_t addr, bool on) {
    bool all = is_null_bd_addr(addr);
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (is_null_bd_addr(g_hosts[i].addr)) continue;
        if (all || memcmp(g_hosts[i].addr, addr, 6) == 0) g_hosts[i].manual_disconnect = on;
    }
}

/* True when the requested host is connected; for a null address (a loop over every known host),
 * when all of them are: with two hosts the first to connect used to end the loop for the other */
static bool host_target_connected(const bd_addr_t target) {
    return is_null_bd_addr(target) ? all_known_hosts_connected() : is_host_connected(target);
}

bool get_reconnect_target(bd_addr_t out) {
    /* The host of the current activity (last TARGET) first, then the pinned one */
    if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && !is_null_bd_addr(g_hosts[g_active_host_idx].addr)) {
        memcpy(out, g_hosts[g_active_host_idx].addr, 6);
        return true;
    }
    return get_pinned_host_target(out) != 0;
}

/* Names that only stand in until the host's device name is known */
static bool is_placeholder_host_name(const char *name) {
    return !name || !name[0] || strcmp(name, "Harmony Keyboard") == 0 || strncmp(name, "Host ", 5) == 0;
}

host_conn_t * register_host_target(const bd_addr_t addr, const char *name) {
    if (is_null_bd_addr(addr)) return NULL;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (memcmp(g_hosts[i].addr, addr, 6) == 0) {
            if (name && name[0] && is_placeholder_host_name(g_hosts[i].name)) {
                strncpy(g_hosts[i].name, name, sizeof(g_hosts[i].name) - 1);
                g_hosts[i].name[sizeof(g_hosts[i].name) - 1] = '\0';
            }
            return &g_hosts[i];
        }
    }
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (is_null_bd_addr(g_hosts[i].addr)) {
            memcpy(g_hosts[i].addr, addr, 6);
            if (name && name[0]) {
                strncpy(g_hosts[i].name, name, sizeof(g_hosts[i].name) - 1);
                g_hosts[i].name[sizeof(g_hosts[i].name) - 1] = '\0';
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
    /* hub_daemon and bt_reconnect.sh read "listening" plus a matching "target" as "this host is
     * connected", so name the chosen host only while it really is */
    const char *target = "";
    if (ah && ah->connected && ah->hid_cid) {
        target = bd_addr_to_str(ah->addr);
    } else if (!ah && any_conn) {
        target = bd_addr_to_str(g_host_addr);
    }
    cJSON_AddStringToObject(root, "target", target);
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
            /* paired = BTstack has a link key for it; hosts known only from devices.json
             * (e.g. after a reset, or migrated from the stock config) have none */
            link_key_t lk;
            link_key_type_t lkt;
            bd_addr_t ha;
            memcpy(ha, g_hosts[i].addr, 6);
            bool paired = gap_get_link_key_for_bd_addr(ha, lk, &lkt);
            fprintf(f, "    {\"addr\": \"%s\", \"name\": \"%s\", \"connected\": %s, \"paired\": %s, \"cid\": %u}",
                    bd_addr_to_str(g_hosts[i].addr), g_hosts[i].name,
                    g_hosts[i].connected ? "true" : "false", paired ? "true" : "false", g_hosts[i].hid_cid);
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
    const char *files[] = {"/data/harmony/bthid_host_target", "/data/harmony/config/devices.json", NULL};
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

/* Name of the device with this id in config/devices.json; out is "" when not found */
static void device_name_for_id(const char *dev_id, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!dev_id) return;
    FILE *f = fopen("/data/harmony/config/devices.json", "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (sz > 0 && sz < 1000000) ? (char *)malloc((size_t)sz + 1) : NULL;
    if (buf) {
        size_t r = fread(buf, 1, (size_t)sz, f);
        buf[r] = 0;
        cJSON *root = cJSON_Parse(buf);
        free(buf);
        cJSON *devs = root ? cJSON_GetObjectItemCaseSensitive(root, "devices") : NULL;
        cJSON *item = NULL;
        if (devs && cJSON_IsArray(devs)) {
            cJSON_ArrayForEach(item, devs) {
                cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
                char id_str[32] = {0};
                if (jid && cJSON_IsNumber(jid)) snprintf(id_str, sizeof(id_str), "%ld", (long)jid->valuedouble);
                else if (jid && cJSON_IsString(jid)) strncpy(id_str, jid->valuestring, sizeof(id_str) - 1);
                cJSON *jn = cJSON_GetObjectItemCaseSensitive(item, "name");
                if (strcmp(id_str, dev_id) == 0 && jn && cJSON_IsString(jn) && jn->valuestring[0]) {
                    strncpy(out, jn->valuestring, out_len - 1);
                    out[out_len - 1] = '\0';
                    break;
                }
            }
        }
        cJSON_Delete(root);
    }
    fclose(f);
}

/* Ask hub_daemon to link the device (POST /api/bt-link-device on loopback, which it trusts).
 * hub_daemon owns devices.json and keeps it cached: a file written behind its back is not seen
 * by its command sending and is overwritten by the next device edit in the web UI. */
static bool link_via_daemon(const char *dev_id, const char *addr_str) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(DAEMON_PORT);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = false;
    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
        char body[160], req[400];
        int blen = snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"bdaddr\":\"%s\"}", dev_id, addr_str);
        int rlen = snprintf(req, sizeof(req),
                            "POST /api/bt-link-device HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                            "Content-Length: %d\r\nConnection: close\r\n\r\n%s", blen, body);
        if (blen > 0 && blen < (int)sizeof(body) && rlen > 0 && rlen < (int)sizeof(req) &&
            send(s, req, (size_t)rlen, MSG_NOSIGNAL) == rlen) {
            char resp[512];
            size_t got = 0;
            ssize_t n;
            while (got < sizeof(resp) - 1 && (n = recv(s, resp + got, sizeof(resp) - 1 - got, 0)) > 0) got += (size_t)n;
            resp[got] = '\0';
            ok = strncmp(resp, "HTTP/1.1 200", 12) == 0 && strstr(resp, "\"ok\":true") != NULL;
        }
    }
    close(s);
    return ok;
}

void link_bt_device_to_addr(const char *dev_id, const char *addr_str) {
    if (!dev_id || !dev_id[0] || !addr_str || !addr_str[0]) return;
    char msg[128];
    snprintf(msg, sizeof(msg), "Linking device %s to Bluetooth address %s", dev_id, addr_str);
    log_msg(msg);
    char dev_name[64];
    device_name_for_id(dev_id, dev_name, sizeof(dev_name));

    /* config/devices.json is the only device-to-host mapping; bt-devices.json, which used to
     * hold a second copy, is no longer written (hub_daemon folds old copies in, bt_migrate.c).
     * hub_daemon writes it; editing the file directly is the fallback when it does not answer. */
    bool linked = link_via_daemon(dev_id, addr_str);
    if (linked) {
        log_msg("Device link saved by hub_daemon");
    } else {
        log_msg("hub_daemon did not confirm the device link, writing devices.json directly");
    }
    const char *dpath = "/data/harmony/config/devices.json";
    FILE *df = linked ? NULL : fopen(dpath, "r");
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
                    if (ddevs && cJSON_IsArray(ddevs)) {
                        cJSON *ditem = NULL;
                        cJSON_ArrayForEach(ditem, ddevs) {
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(ditem, "id");
                            char d_id_str[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON_DeleteItemFromObjectCaseSensitive(ditem, "btHost");
                                cJSON_DeleteItemFromObjectCaseSensitive(ditem, "bdaddr");
                                cJSON_AddStringToObject(ditem, "bdaddr", addr_str);
                                char *dout = cJSON_Print(droot);
                                if (dout) {
                                    char tmp_dpath[128];
                                    snprintf(tmp_dpath, sizeof(tmp_dpath), "%s.tmp", dpath);
                                    FILE *dwf = fopen(tmp_dpath, "w");
                                    if (dwf) {
                                        fputs(dout, dwf);
                                        fputc('\n', dwf);
                                        fclose(dwf);
                                        linked = (rename(tmp_dpath, dpath) == 0);
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
    if (!linked) {
        snprintf(msg, sizeof(msg), "Device %s not saved in devices.json: host %s not linked", dev_id, addr_str);
        log_msg(msg);
    }

    /* Show the linked device's name for the host right away; until hub_btstack restarted, a
     * host paired from the web UI was listed as "Host <bdaddr>" */
    bd_addr_t ba;
    if (dev_name[0] && parse_bd_addr(addr_str, ba)) {
        host_conn_t *h = register_host_target(ba, NULL);
        if (h) {
            strncpy(h->name, dev_name, sizeof(h->name) - 1);
            h->name[sizeof(h->name) - 1] = '\0';
        }
        update_status(app_state == APP_READY ? "ready" : "standby");
    }
}

static cJSON *load_json_file(const char *path, long max_size) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    cJSON *root = NULL;
    if (sz > 0 && sz < max_size) {
        char *buf = (char *)malloc(sz + 1);
        if (buf) {
            size_t r = fread(buf, 1, sz, f);
            buf[r] = 0;
            root = cJSON_Parse(buf);
            free(buf);
        }
    }
    fclose(f);
    return root;
}

/* The host of an activity: the first of its devices (config/activities.json deviceIds) that is a
 * Bluetooth device with an address in config/devices.json. With two hosts paired, the pinned host
 * (the last one paired) is not necessarily the one the running activity uses. */
/* An "id" as text: string ids as they are, numeric ids (older or hand-written files) printed */
static bool json_id_str(const cJSON *j, char *out, size_t out_len) {
    if (!j || !out || out_len == 0) return false;
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
        snprintf(out, out_len, "%s", j->valuestring);
        return true;
    }
    if (cJSON_IsNumber(j)) {
        snprintf(out, out_len, "%ld", (long)j->valuedouble);
        return true;
    }
    return false;
}

bool get_activity_host_target(const char *act_id, bd_addr_t out) {
    if (!act_id || !act_id[0]) return false;
    bool found = false;
    cJSON *acts = load_json_file("/data/harmony/config/activities.json", 1000000);
    cJSON *devs = load_json_file("/data/harmony/config/devices.json", 1000000);
    cJSON *alist = acts ? cJSON_GetObjectItemCaseSensitive(acts, "activities") : NULL;
    cJSON *dlist = devs ? cJSON_GetObjectItemCaseSensitive(devs, "devices") : NULL;
    cJSON *act = NULL;
    if (alist && dlist && cJSON_IsArray(alist) && cJSON_IsArray(dlist)) {
        cJSON_ArrayForEach(act, alist) {
            char aid[64];
            if (!json_id_str(cJSON_GetObjectItemCaseSensitive(act, "id"), aid, sizeof(aid)) || strcmp(aid, act_id) != 0) continue;
            cJSON *ids = cJSON_GetObjectItemCaseSensitive(act, "deviceIds");
            cJSON *did = NULL;
            if (ids && cJSON_IsArray(ids)) {
                cJSON_ArrayForEach(did, ids) {
                    char want[64];
                    if (!json_id_str(did, want, sizeof(want))) continue;
                    cJSON *dev = NULL;
                    cJSON_ArrayForEach(dev, dlist) {
                        char have[64];
                        if (!json_id_str(cJSON_GetObjectItemCaseSensitive(dev, "id"), have, sizeof(have)) || strcmp(have, want) != 0) continue;
                        /* ids are unique: this is the device, BT or not */
                        cJSON *jtype = cJSON_GetObjectItemCaseSensitive(dev, "type");
                        cJSON *jaddr = cJSON_GetObjectItemCaseSensitive(dev, "bdaddr");
                        if (jtype && cJSON_IsString(jtype) && strcasecmp(jtype->valuestring, "BT") == 0 &&
                            jaddr && cJSON_IsString(jaddr) && parse_bd_addr(jaddr->valuestring, out) && !is_null_bd_addr(out)) {
                            found = true;
                        }
                        break;
                    }
                    if (found) break;
                }
            }
            break;
        }
    }
    if (acts) cJSON_Delete(acts);
    if (devs) cJSON_Delete(devs);
    return found;
}

void init_known_hosts(void) {
    bd_addr_t def_target;
    /* Register the pinned host without a name: the devices below name it by address.
     * It used to get the first BT device's name in devices.json, whichever host that was. */
    if (get_pinned_host_target(def_target)) {
        register_host_target(def_target, NULL);
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
                            /* Host names come only from the device mapped to the host here */
                            cJSON *ja = cJSON_GetObjectItemCaseSensitive(item, "bdaddr");
                            if (!ja) ja = cJSON_GetObjectItemCaseSensitive(item, "btHost");
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
        fclose(df);
    }
}

void try_connect_host_addr(const bd_addr_t addr) {
    if (is_null_bd_addr(addr)) return;
    set_host_manual_disconnect(addr, false);
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (g_hosts[i].connected && g_hosts[i].hid_cid != 0 && memcmp(g_hosts[i].addr, addr, 6) == 0) {
            return;
        }
    }
    if (g_host_connecting && (time(NULL) - g_host_connecting_start) < 5) {
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Initiating connection to Classic HID Host: %s", bd_addr_to_str(addr));
    log_msg(msg);
    g_host_connecting = true;
    g_host_connecting_start = time(NULL);

    // Suspend LE scanning during Classic ACL paging to dedicate CSR8811 radio to Host connection
    if (app_state == APP_SCANNING && !g_user_scan_active) {
        gap_stop_scan();
    }

    uint16_t cid = 0;
    uint8_t status = hid_device_connect((uint8_t*)addr, &cid);
    if (status != ERROR_CODE_SUCCESS || cid == 0) {
        g_host_connecting = false;
        if (app_state == APP_SCANNING && !g_user_scan_active) {
            gap_start_scan();
        }
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
                register_host_target(target, NULL);
                try_connect_host_addr(target);
            }
        }
    }
}

static void host_reconnect_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (!g_host_reconnect_active) return;

    if (host_target_connected(g_host_reconnect_target)) {
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

    if (g_host_connecting && (time(NULL) - g_host_connecting_start) < 5) {
        btstack_run_loop_set_timer(&g_host_reconnect_timer, 1500);
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

    uint32_t next_ms = (g_host_reconnect_attempts <= 3) ? 2000 : 4000;
    btstack_run_loop_set_timer(&g_host_reconnect_timer, next_ms);
    btstack_run_loop_add_timer(&g_host_reconnect_timer);
}

void start_host_reconnect(const bd_addr_t target, int max_attempts) {
    (void)max_attempts;
    set_host_manual_disconnect(target, false);
    /* Another connected host must not block this one (several HID hosts are supported) */
    if (host_target_connected(target)) {
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
    snprintf(msg, sizeof(msg), "Starting host reconnect loop for %s (fast retry: 2s interval)",
             is_null_bd_addr(g_host_reconnect_target) ? "targets" : bd_addr_to_str(g_host_reconnect_target));
    log_msg(msg);

    if (!is_null_bd_addr(g_host_reconnect_target)) {
        try_connect_host_addr(g_host_reconnect_target);
    } else {
        try_connect_host();
    }

    btstack_run_loop_remove_timer(&g_host_reconnect_timer);
    btstack_run_loop_set_timer(&g_host_reconnect_timer, 2000);
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
        const char *addr_str = bd_addr_to_str(g_discovered[i].addr);
        cJSON_AddStringToObject(d, "addr", addr_str);
        cJSON_AddStringToObject(d, "bdaddr", addr_str);
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
        if (is_null_bd_addr(remote_device.addr)) {
            memcpy(remote_device.addr, pinned, 6);
        }
        update_status("standby");
        gap_set_scan_parameters(0, 96, 24); // passive scanning (25% duty cycle): no SCAN_REQ delay, avoids Classic BT paging collision
    } else {
        update_status("scanning");
        gap_set_scan_parameters(1, 96, 24); // active scanning for discovery (25% duty cycle)
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

void stop_user_scan(void) {
    if (g_user_scan_active) {
        g_user_scan_active = false;
        log_msg("User stopped BLE remote scan");
        write_scan_results(false);
        gap_stop_scan();
        hog_start_connect();
    }
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
