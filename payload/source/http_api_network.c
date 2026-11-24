#include "http_api_network.h"
#include "hub_network_util.h"
#include "http_util.h"
#include "config_store.h"
#include "cJSON.h"
#include "wpa_conf.h"
#include "wifi_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>

static int parse_ipv4(const char *s, uint32_t *out) {
    struct in_addr a;
    if (!s || !is_safe_ipv4(s) || inet_pton(AF_INET, s, &a) != 1) return 0;
    *out = ntohl(a.s_addr);
    return 1;
}

/* NULL when a static address, mask, gateway (optional) and DNS list (optional, space or comma
 * separated) can work together, otherwise the reason */
static const char *static_address_error(const char *ip_s, const char *mask_s, const char *gw_s, const char *dns_s) {
    uint32_t ip, mask, gw;
    if (!parse_ipv4(ip_s, &ip)) return "Static mode needs a valid IPv4 address";
    if (!parse_ipv4(mask_s, &mask) || mask == 0 || ((~mask) & ((~mask) + 1)) != 0) return "Invalid netmask";
    uint32_t net = ip & mask, host = ip & ~mask;
    if ((ip >> 24) == 0 || (ip >> 24) == 127 || (ip >> 24) >= 224) return "The address cannot be used by a host";
    if (~mask > 1 && (host == 0 || host == ~mask)) return "The address is the network or broadcast address of its subnet";
    if (gw_s && gw_s[0]) {
        if (!parse_ipv4(gw_s, &gw)) return "Invalid gateway address";
        if ((gw & mask) != net || gw == ip) return "The gateway is not in the same subnet as the address";
    }
    if (dns_s && dns_s[0]) {
        char buf[64], *save = NULL;
        if (strlen(dns_s) >= sizeof(buf)) return "Invalid DNS server list";
        snprintf(buf, sizeof(buf), "%s", dns_s);
        for (char *tok = strtok_r(buf, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
            uint32_t d;
            if (!parse_ipv4(tok, &d)) return "Invalid DNS server address";
        }
    }
    return NULL;
}

/* USB Ethernet driver state, written by usb_eth_driver.sh scan and install (RAM files) */
static const char *eth_driver_path(char *buf, size_t sz, const char *name) {
    const char *dir = "/tmp";
#ifdef TEST_HARNESS
    const char *t = getenv("HARMONY_TEST_TMP_DIR");
    if (t && t[0]) dir = t;
#endif
    snprintf(buf, sz, "%s/%s", dir, name);
    return buf;
}

/* A script of /data/harmony run in the background: <tag>_running exists while it runs, and
 * <tag>.rc holds its exit code after, <tag>.log its output. args is put into the command as it
 * is: callers pass only fixed words and checked names. */
static void start_bg_job(const char *tag, const char *args) {
    char flag[256], rc_path[256], log_path[256], name[64], cmd[1024];
    snprintf(name, sizeof(name), "%s_running", tag);
    eth_driver_path(flag, sizeof(flag), name);
    snprintf(name, sizeof(name), "%s.rc", tag);
    eth_driver_path(rc_path, sizeof(rc_path), name);
    snprintf(name, sizeof(name), "%s.log", tag);
    eth_driver_path(log_path, sizeof(log_path), name);
    unlink(rc_path);
    FILE *ff = fopen(flag, "w");
    if (ff) fclose(ff);
    snprintf(cmd, sizeof(cmd),
             "/bin/sh -c 'sh /data/harmony/%s >%s 2>&1; echo $? >%s; rm -f %s' >/dev/null 2>&1 &",
             args, log_path, rc_path, flag);
#ifndef TEST_HARNESS
    system(cmd);
#else
    printf("[*] background job (test build, not run): %s\n", cmd);
#endif
}

/* 1 while the job runs; *rc: its exit code, -1 when it never ran or still runs */
static int bg_job_state(const char *tag, int *rc) {
    char path[256], name[64], line[32];
    *rc = -1;
    snprintf(name, sizeof(name), "%s_running", tag);
    if (access(eth_driver_path(path, sizeof(path), name), F_OK) == 0) return 1;
    snprintf(name, sizeof(name), "%s.rc", tag);
    FILE *f = fopen(eth_driver_path(path, sizeof(path), name), "r");
    if (f) {
        if (fgets(line, sizeof(line), f)) *rc = atoi(line);
        fclose(f);
    }
    return 0;
}

/* A module name usb_eth_driver.sh and update.sh accept: [a-z0-9_], 1-31 characters */
static int eth_module_name_ok(const char *s) {
    if (!s || !s[0] || strlen(s) > 31) return 0;
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    }
    return 1;
}

/* The "eth_driver" object of /api/network-status: the adapter found on USB, the driver it needs,
 * the modules that are not installed, whether the driver is loaded, and a running or failed
 * download. NULL (no object) when no adapter was found. */
static cJSON *eth_driver_json(void) {
    char path[256], line[160];
    cJSON *o = NULL;
    FILE *f = fopen(eth_driver_path(path, sizeof(path), "eth_driver_state"), "r");
    if (f) {
        o = cJSON_CreateObject();
        cJSON *missing = cJSON_CreateArray();
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *v = strchr(line, '=');
            if (!v) continue;
            *v++ = '\0';
            if (strcmp(line, "vid") == 0 || strcmp(line, "pid") == 0 || strcmp(line, "driver") == 0) {
                cJSON_AddStringToObject(o, line, v);
            } else if (strcmp(line, "missing") == 0) {
                char *save = NULL;
                for (char *m = strtok_r(v, " ", &save); m; m = strtok_r(NULL, " ", &save))
                    if (eth_module_name_ok(m)) cJSON_AddItemToArray(missing, cJSON_CreateString(m));
            } else if (strcmp(line, "loaded") == 0 || strcmp(line, "unsupported") == 0 ||
                       strcmp(line, "slow") == 0) {
                cJSON_AddBoolToObject(o, line, atoi(v) == 1);
            }
        }
        fclose(f);
        cJSON_AddItemToObject(o, "missing", missing);
    }
    int rc, installing = bg_job_state("eth_driver", &rc);
    if (!o && !installing && rc < 0) return NULL;
    if (!o) o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "installing", installing);
    if (!installing && rc > 0) cJSON_AddBoolToObject(o, "install_failed", 1);
    return o;
}

/* GET /api/usb-storage: the state usb_storage.sh scan wrote, the job running, and the last line
 * the last job printed (its result or why it failed) */
static cJSON *usb_storage_json(void) {
    char path[256], line[256];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON *missing = cJSON_CreateArray();
    int present = 0, mounted = 0, offload = 0;
    long total = -1, avail = -1;
    char device[32] = "", fs[16] = "";
    FILE *f = fopen(eth_driver_path(path, sizeof(path), "usb_storage_state"), "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *v = strchr(line, '=');
            if (!v) continue;
            *v++ = '\0';
            if (strcmp(line, "present") == 0) present = atoi(v) == 1;
            else if (strcmp(line, "mounted") == 0) mounted = atoi(v) == 1;
            else if (strcmp(line, "offload") == 0) offload = atoi(v) == 1;
            else if (strcmp(line, "device") == 0 && is_safe_param(v, "")) snprintf(device, sizeof(device), "%s", v);
            else if (strcmp(line, "fs") == 0 && is_safe_param(v, "")) snprintf(fs, sizeof(fs), "%s", v);
            else if (strcmp(line, "total_kb") == 0 && v[0]) total = atol(v);
            else if (strcmp(line, "free_kb") == 0 && v[0]) avail = atol(v);
            else if (strcmp(line, "missing") == 0) {
                char *save = NULL;
                for (char *m = strtok_r(v, " ", &save); m; m = strtok_r(NULL, " ", &save))
                    if (is_safe_param(m, "")) cJSON_AddItemToArray(missing, cJSON_CreateString(m));
            }
        }
        fclose(f);
    }
    cJSON_AddBoolToObject(o, "present", present);
    cJSON_AddBoolToObject(o, "mounted", mounted);
    cJSON_AddStringToObject(o, "device", device);
    cJSON_AddStringToObject(o, "fs", fs);
    cJSON_AddItemToObject(o, "missing", missing);
    if (total >= 0) cJSON_AddNumberToObject(o, "total_kb", (double)total);
    if (avail >= 0) cJSON_AddNumberToObject(o, "free_kb", (double)avail);
    /* the flag file is the setting; the state file only mirrors it at the last scan */
    cJSON_AddBoolToObject(o, "offload", offload || access(config_store_path("config/usb_offload"), F_OK) == 0);
    int rc, busy = bg_job_state("usb_storage", &rc);
    cJSON_AddBoolToObject(o, "busy", busy);
    if (!busy && rc > 0) cJSON_AddBoolToObject(o, "failed", 1);
    char last[256] = "";
    f = fopen(eth_driver_path(path, sizeof(path), "usb_storage.log"), "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0]) snprintf(last, sizeof(last), "%s", line);
        }
        fclose(f);
    }
    cJSON_AddStringToObject(o, "message", last);
    return o;
}

int http_api_network_handle(int client_fd, const char *method, const char *clean_path,
                            const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    /* Route: GET /api/network-status */
    if (strcmp(clean_path, "/api/network-status") == 0 && strcmp(method, "GET") == 0) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);

        int usb_host = net_get_usb_host_mode();
        cJSON_AddBoolToObject(st, "usb_host_mode", usb_host);

        char eth_if[32] = {0};
        int eth_onboard = 0;
        int eth_present = net_find_usb_eth_interface_ex(eth_if, sizeof(eth_if), &eth_onboard);
        int eth_carrier = eth_present ? net_get_iface_carrier(eth_if) : 0;
        char eth_ip[32] = {0}, eth_mask[32] = {0}, eth_mac[32] = {0};
        if (eth_present) {
            net_get_iface_ip_mask_mac(eth_if, eth_ip, sizeof(eth_ip), eth_mask, sizeof(eth_mask), eth_mac, sizeof(eth_mac));
        }

        char wifi_ip[32] = {0}, wifi_mask[32] = {0}, wifi_mac[32] = {0}, wifi_ssid[64] = {0};
        net_get_iface_ip_mask_mac("ath0", wifi_ip, sizeof(wifi_ip), wifi_mask, sizeof(wifi_mask), wifi_mac, sizeof(wifi_mac));
        int wifi_connected = (wifi_ip[0] != '\0');
        if (wifi_connected) {
            net_get_wifi_ssid(wifi_ssid, sizeof(wifi_ssid));
        }

        char gw[64] = "", default_iface[32] = "";
        char mask_dummy[64] = "", mac_dummy[64] = "";
        net_get_network_details(default_iface, sizeof(default_iface), gw, sizeof(gw), mask_dummy, sizeof(mask_dummy), mac_dummy, sizeof(mac_dummy));

        const char *active_if = default_iface[0] ? default_iface : (eth_carrier && eth_ip[0] ? eth_if : "ath0");
        cJSON_AddStringToObject(st, "active_interface", active_if);

        const char *conn_type = "Disconnected";
        if (eth_carrier && eth_ip[0] && wifi_connected) {
            conn_type = (strcmp(active_if, eth_if) == 0)
                ? (eth_onboard ? "Dual (Onboard Ethernet Active + Wi-Fi Standby)" : "Dual (Ethernet Active + Wi-Fi Standby)")
                : (eth_onboard ? "Dual (Wi-Fi Active + Onboard Ethernet Standby)" : "Dual (Wi-Fi Active + Ethernet Standby)");
        } else if (eth_carrier && eth_ip[0]) {
            conn_type = eth_onboard ? "Onboard Ethernet (PoE/RJ45)" : "Ethernet";
        } else if (wifi_connected && eth_carrier) {
            conn_type = eth_onboard ? "Dual (Wi-Fi Active + Onboard Ethernet Link Up)" : "Dual (Wi-Fi Active + Ethernet Link Up)";
        } else if (wifi_connected && eth_present) {
            conn_type = eth_onboard ? "Dual (Wi-Fi Active + Onboard Ethernet Standby)" : "Dual (Wi-Fi Active + Ethernet Standby)";
        } else if (wifi_connected) {
            conn_type = "Wi-Fi";
        } else if (eth_carrier) {
            conn_type = eth_onboard ? "Onboard Ethernet (Link Up, No IP)" : "Ethernet (Link Up, No IP)";
        }
        cJSON_AddStringToObject(st, "connection_type", conn_type);
        cJSON_AddStringToObject(st, "mode", (strncmp(active_if, "eth", 3) == 0) ? "ethernet" : "wifi");

        if (strcmp(active_if, eth_if) == 0 && eth_ip[0]) {
            cJSON_AddStringToObject(st, "ip", eth_ip);
            cJSON_AddStringToObject(st, "netmask", eth_mask);
            cJSON_AddStringToObject(st, "mac", eth_mac);
        } else {
            char local_ip[64];
            net_get_local_ip(client_fd, local_ip, sizeof(local_ip));
            cJSON_AddStringToObject(st, "ip", wifi_ip[0] ? wifi_ip : local_ip);
            cJSON_AddStringToObject(st, "netmask", wifi_mask[0] ? wifi_mask : "255.255.255.0");
            cJSON_AddStringToObject(st, "mac", wifi_mac[0] ? wifi_mac : mac_dummy);
        }
        cJSON_AddStringToObject(st, "gateway", gw);

        cJSON_AddBoolToObject(st, "eth_present", eth_present);
        cJSON_AddBoolToObject(st, "eth_onboard", eth_onboard);
        cJSON_AddBoolToObject(st, "eth_carrier", eth_carrier);
        cJSON_AddStringToObject(st, "eth_interface", eth_if[0] ? eth_if : "none");
        cJSON_AddStringToObject(st, "eth_ip", eth_ip);
        cJSON_AddStringToObject(st, "eth_mask", eth_mask);
        cJSON_AddStringToObject(st, "eth_mac", eth_mac);

        cJSON_AddBoolToObject(st, "wifi_connected", wifi_connected);
        cJSON_AddStringToObject(st, "wifi_interface", "ath0");
        cJSON_AddStringToObject(st, "wifi_ip", wifi_ip);
        cJSON_AddStringToObject(st, "wifi_mask", wifi_mask);
        cJSON_AddStringToObject(st, "wifi_mac", wifi_mac);
        cJSON_AddStringToObject(st, "wifi_ssid", wifi_ssid);

        cJSON *cfg = cJSON_CreateObject();
        int usb_wifi = 1; /* Wi-Fi setup over USB (hub_util usbwifi): on unless USB_WIFI_SETUP=0 */
        FILE *cff = fopen(config_store_path("ethernet.conf"), "r");
        if (cff) {
            char cline[128];
            while (fgets(cline, sizeof(cline), cff)) {
                char *eq = strchr(cline, '=');
                if (!eq) continue;
                *eq = '\0';
                char *k = cline;
                char *v = eq + 1;
                while (*k == ' ' || *k == '\t') k++;
                char *ken = k + strlen(k) - 1;
                while (ken > k && (*ken == ' ' || *ken == '\t' || *ken == '\r' || *ken == '\n')) *ken-- = '\0';
                while (*v == ' ' || *v == '\t' || *v == '"') v++;
                char *ven = v + strlen(v) - 1;
                while (ven >= v && (*ven == ' ' || *ven == '\t' || *ven == '\r' || *ven == '\n' || *ven == '"')) *ven-- = '\0';
                if (strcmp(k, "ETH_ENABLED") == 0) cJSON_AddBoolToObject(cfg, "eth_enabled", atoi(v) == 1);
                else if (strcmp(k, "ETH_FALLBACK_WIFI") == 0) cJSON_AddBoolToObject(cfg, "eth_fallback_wifi", atoi(v) == 1);
                else if (strcmp(k, "ETH_MODE") == 0) cJSON_AddStringToObject(cfg, "eth_mode", v);
                else if (strcmp(k, "ETH_IP") == 0) cJSON_AddStringToObject(cfg, "eth_ip", v);
                else if (strcmp(k, "ETH_NETMASK") == 0) cJSON_AddStringToObject(cfg, "eth_netmask", v);
                else if (strcmp(k, "ETH_GATEWAY") == 0) cJSON_AddStringToObject(cfg, "eth_gateway", v);
                else if (strcmp(k, "ETH_DNS") == 0) cJSON_AddStringToObject(cfg, "eth_dns", v);
                else if (strcmp(k, "USB_WIFI_SETUP") == 0) usb_wifi = strcmp(v, "0") != 0;
            }
            fclose(cff);
        }
        cJSON_AddBoolToObject(cfg, "usb_wifi_setup", usb_wifi);
        cJSON_AddItemToObject(st, "config", cfg);

        char ifconfig_buf[8192] = {0};
        FILE *pif = popen("/sbin/ifconfig 2>/dev/null", "r");
        if (pif) {
            size_t r = fread(ifconfig_buf, 1, sizeof(ifconfig_buf) - 1, pif);
            ifconfig_buf[r] = '\0';
            pclose(pif);
        }
        cJSON_AddStringToObject(st, "ifconfig", ifconfig_buf);
        cJSON *drv = eth_driver_json();
        if (drv) cJSON_AddItemToObject(st, "eth_driver", drv);

        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/wifi-scan */
    if (strcmp(clean_path, "/api/wifi-scan") == 0 && strcmp(method, "GET") == 0) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON *networks = cJSON_CreateArray();
        net_scan_wifi_networks(networks);
        cJSON_AddItemToObject(resp, "networks", networks);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/wifi-test */
    if (strcmp(clean_path, "/api/wifi-test") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        if (params) {
            cJSON *p_ssid = cJSON_GetObjectItemCaseSensitive(params, "ssid");
            cJSON *p_pass = cJSON_GetObjectItemCaseSensitive(params, "password");
            cJSON *p_open = cJSON_GetObjectItemCaseSensitive(params, "open");
            cJSON *p_hid = cJSON_GetObjectItemCaseSensitive(params, "hidden");
            cJSON *p_keep = cJSON_GetObjectItemCaseSensitive(params, "keep_password");

            const char *raw_ssid = (p_ssid && cJSON_IsString(p_ssid)) ? p_ssid->valuestring : "";
            char pass_buf[128] = {0};
            int is_open = (p_open && ((cJSON_IsBool(p_open) && cJSON_IsTrue(p_open)) || (cJSON_IsNumber(p_open) && p_open->valueint == 1) || (cJSON_IsString(p_open) && strcmp(p_open->valuestring, "1") == 0)));
            int is_hidden = (p_hid && ((cJSON_IsBool(p_hid) && cJSON_IsTrue(p_hid)) || (cJSON_IsNumber(p_hid) && p_hid->valueint == 1) || (cJSON_IsString(p_hid) && strcmp(p_hid->valuestring, "1") == 0)));
            int keep_pass = (p_keep && ((cJSON_IsBool(p_keep) && cJSON_IsTrue(p_keep)) || (cJSON_IsNumber(p_keep) && p_keep->valueint == 1) || (cJSON_IsString(p_keep) && strcmp(p_keep->valuestring, "1") == 0)));

            if (p_pass && cJSON_IsString(p_pass) && p_pass->valuestring[0]) {
                strncpy(pass_buf, p_pass->valuestring, sizeof(pass_buf) - 1);
            } else if (keep_pass && !is_open) {
                net_get_current_wifi_psk(pass_buf, sizeof(pass_buf));
            }

            char err_buf[256] = {0};
            const char *bad = wpa_conf_value_error(raw_ssid, 0);
            if (!bad && !is_open) bad = wpa_conf_value_error(pass_buf, 1);
            int res = bad ? -1 : net_test_wifi_connection(raw_ssid, pass_buf, is_open, is_hidden, err_buf, sizeof(err_buf));
            if (bad) snprintf(err_buf, sizeof(err_buf), "%s", bad);
            cJSON_Delete(params);

            cJSON *resp = cJSON_CreateObject();
            if (res == 0) {
                cJSON_AddBoolToObject(resp, "ok", 1);
                cJSON_AddStringToObject(resp, "message", "Wi-Fi test connection successful");
                send_cjson_resp(client_fd, 200, resp);
            } else {
                cJSON_AddBoolToObject(resp, "ok", 0);
                cJSON_AddStringToObject(resp, "error", err_buf[0] ? err_buf : "Wi-Fi test connection failed");
                send_cjson_resp(client_fd, 400, resp);
            }
            cJSON_Delete(resp);
            close(client_fd);
            return 1;
        }
        send_json_error(client_fd, 400, "Invalid parameters");
        close(client_fd);
        return 1;
    }

    /* Route: POST /wifi or POST /api/wifi-save */
    if ((strcmp(clean_path, "/wifi") == 0 || strcmp(clean_path, "/api/wifi-save") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        if (params) {
            cJSON *p_ssid = cJSON_GetObjectItemCaseSensitive(params, "ssid");
            cJSON *p_pass = cJSON_GetObjectItemCaseSensitive(params, "password");
            cJSON *p_open = cJSON_GetObjectItemCaseSensitive(params, "open");
            cJSON *p_hid = cJSON_GetObjectItemCaseSensitive(params, "hidden");
            cJSON *p_keep = cJSON_GetObjectItemCaseSensitive(params, "keep_password");
            cJSON *p_force = cJSON_GetObjectItemCaseSensitive(params, "force");
            cJSON *p_app = cJSON_GetObjectItemCaseSensitive(params, "apply");

            const char *raw_ssid = (p_ssid && cJSON_IsString(p_ssid)) ? p_ssid->valuestring : "";
            if (raw_ssid[0]) {
                /* written as typed: wpa_supplicant reads a quoted value without escapes */
                char esc_pass[128] = {0};

                int is_open = (p_open && ((cJSON_IsBool(p_open) && cJSON_IsTrue(p_open)) || (cJSON_IsNumber(p_open) && p_open->valueint == 1) || (cJSON_IsString(p_open) && strcmp(p_open->valuestring, "1") == 0)));
                int is_hidden = (p_hid && ((cJSON_IsBool(p_hid) && cJSON_IsTrue(p_hid)) || (cJSON_IsNumber(p_hid) && p_hid->valueint == 1) || (cJSON_IsString(p_hid) && strcmp(p_hid->valuestring, "1") == 0)));
                int keep_pass = (p_keep && ((cJSON_IsBool(p_keep) && cJSON_IsTrue(p_keep)) || (cJSON_IsNumber(p_keep) && p_keep->valueint == 1) || (cJSON_IsString(p_keep) && strcmp(p_keep->valuestring, "1") == 0)));
                int force_save = (p_force && ((cJSON_IsBool(p_force) && cJSON_IsTrue(p_force)) || (cJSON_IsNumber(p_force) && p_force->valueint == 1) || (cJSON_IsString(p_force) && strcmp(p_force->valuestring, "1") == 0)));

                const char *app_mode = (p_app && cJSON_IsString(p_app)) ? p_app->valuestring : "save";
                int should_apply = (strcmp(app_mode, "reconfigure") == 0 || strcmp(app_mode, "apply") == 0 || strcmp(app_mode, "1") == 0 || strcmp(app_mode, "reboot") == 0);

                if (p_pass && cJSON_IsString(p_pass) && p_pass->valuestring[0]) {
                    snprintf(esc_pass, sizeof(esc_pass), "%s", p_pass->valuestring);
                } else if (keep_pass && !is_open) {
                    net_get_current_wifi_psk(esc_pass, sizeof(esc_pass));
                }
                const char *bad = wpa_conf_value_error(raw_ssid, 0);
                if (!bad && !is_open) bad = wpa_conf_value_error(esc_pass, 1);
                if (bad) {
                    cJSON_Delete(params);
                    send_json_error(client_fd, 400, bad);
                    close(client_fd);
                    return 1;
                }

                if (should_apply && !force_save) {
                    char err_buf[256] = {0};
                    int res = net_test_wifi_connection(raw_ssid, esc_pass, is_open, is_hidden, err_buf, sizeof(err_buf));
                    if (res != 0) {
                        cJSON_Delete(params);
                        cJSON *resp = cJSON_CreateObject();
                        cJSON_AddBoolToObject(resp, "ok", 0);
                        char full_err[300];
                        snprintf(full_err, sizeof(full_err), "Wi-Fi test connection failed: %s. Settings not changed.", err_buf[0] ? err_buf : "handshake timeout");
                        cJSON_AddStringToObject(resp, "error", full_err);
                        send_cjson_resp(client_fd, 400, resp);
                        cJSON_Delete(resp);
                        close(client_fd);
                        return 1;
                    }
                }

                /* both files or neither (wifi_config.c); a failed save keeps the old ones */
                int reboot_after = strcmp(app_mode, "reboot") == 0;
                if (wifi_config_save(raw_ssid, esc_pass, is_open, is_hidden, should_apply && !reboot_after) != 0) {
                    cJSON_Delete(params);
                    send_json_error(client_fd, 500, "Could not save the Wi-Fi settings (flash full?)");
                    close(client_fd);
                    return 1;
                }

                if (reboot_after) {
                    system("/data/harmony/bin/safe_reboot.sh 2>/dev/null || (killall -9 hub_rf hub_btstack 2>/dev/null; /data/harmony/bin/hub_sync 2>/dev/null; sleep 2; reboot -f) &");
                }

                cJSON_Delete(params);
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "ok", 1);
                cJSON_AddStringToObject(resp, "message", should_apply ? "Wi-Fi settings verified and applied" : "Wi-Fi settings saved");
                send_cjson_resp(client_fd, 200, resp);
                cJSON_Delete(resp);
                close(client_fd);
                return 1;
            }
            cJSON_Delete(params);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 0);
            cJSON_AddStringToObject(resp, "error", "Missing or empty SSID");
            send_cjson_resp(client_fd, 400, resp);
            cJSON_Delete(resp);
            close(client_fd);
            return 1;
        }
        send_json_error(client_fd, 400, "Invalid parameters");
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/usb-storage: a USB stick for non-essential files (icons) */
    if (strcmp(clean_path, "/api/usb-storage") == 0 && strcmp(method, "GET") == 0) {
        cJSON *st = usb_storage_json();
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/usb-storage {action}: scan, install (download the USB storage modules),
     * eject, offload (move the icons to the stick), restore (move them back). Runs
     * usb_storage.sh in the background; GET shows busy until it ends. */
    if (strcmp(clean_path, "/api/usb-storage") == 0 && strcmp(method, "POST") == 0) {
        static const char *actions[] = {"scan", "install", "eject", "offload", "restore"};
        cJSON *params = parse_request_params(body);
        cJSON *ja = params ? cJSON_GetObjectItemCaseSensitive(params, "action") : NULL;
        const char *action = NULL;
        for (size_t i = 0; cJSON_IsString(ja) && i < sizeof(actions) / sizeof(actions[0]); i++)
            if (strcmp(ja->valuestring, actions[i]) == 0) action = actions[i];
        cJSON_Delete(params);
        if (!action) {
            send_json_error(client_fd, 400, "action must be scan, install, eject, offload or restore");
            close(client_fd);
            return 1;
        }
        int rc;
        if (bg_job_state("usb_storage", &rc)) {
            send_json_error(client_fd, 409, "A USB storage action is already running");
            close(client_fd);
            return 1;
        }
        char args[64];
        snprintf(args, sizeof(args), "usb_storage.sh %s", action);
        start_bg_job("usb_storage", args);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "status", "running");
        cJSON_AddStringToObject(resp, "action", action);
        send_cjson_resp(client_fd, 202, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/ethernet-driver-install {module}: download the USB Ethernet driver the
     * plugged adapter needs (and the modules it depends on) from the update source, checked
     * against the signed release manifest, then load it. Runs in the background; progress is
     * the eth_driver object of /api/network-status. Only the driver usb_eth_driver.sh asked for
     * is accepted. */
    if (strcmp(clean_path, "/api/ethernet-driver-install") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        cJSON *jm = params ? cJSON_GetObjectItemCaseSensitive(params, "module") : NULL;
        char module[32] = "";
        if (cJSON_IsString(jm) && eth_module_name_ok(jm->valuestring))
            snprintf(module, sizeof(module), "%s", jm->valuestring);
        cJSON_Delete(params);
        cJSON *drv = eth_driver_json();
        cJSON *jd = drv ? cJSON_GetObjectItemCaseSensitive(drv, "driver") : NULL;
        int wanted = module[0] && cJSON_IsString(jd) && strcmp(jd->valuestring, module) == 0;
        int busy = drv && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(drv, "installing"));
        cJSON_Delete(drv);
        if (!wanted) {
            send_json_error(client_fd, 400, "module is not the driver the USB adapter needs");
            close(client_fd);
            return 1;
        }
        if (busy) {
            send_json_error(client_fd, 409, "A driver download is already running");
            close(client_fd);
            return 1;
        }
        char args[64];
        /* module is [a-z0-9_] only (checked above): safe inside the command */
        snprintf(args, sizeof(args), "usb_eth_driver.sh install %s", module);
        start_bg_job("eth_driver", args);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "status", "installing");
        cJSON_AddStringToObject(resp, "module", module);
        send_cjson_resp(client_fd, 202, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /ethernet or POST /api/ethernet-save */
    if ((strcmp(clean_path, "/ethernet") == 0 || strcmp(clean_path, "/api/ethernet-save") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        if (params) {
            char conf_buf[1024];
            cJSON *p_en = cJSON_GetObjectItemCaseSensitive(params, "eth_enabled");
            cJSON *p_fb = cJSON_GetObjectItemCaseSensitive(params, "eth_fallback_wifi");
            cJSON *p_mode = cJSON_GetObjectItemCaseSensitive(params, "eth_mode");
            cJSON *p_ip = cJSON_GetObjectItemCaseSensitive(params, "eth_ip");
            cJSON *p_mask = cJSON_GetObjectItemCaseSensitive(params, "eth_netmask");
            cJSON *p_gw = cJSON_GetObjectItemCaseSensitive(params, "eth_gateway");
            cJSON *p_dns = cJSON_GetObjectItemCaseSensitive(params, "eth_dns");
            cJSON *p_usb = cJSON_GetObjectItemCaseSensitive(params, "usb_wifi_setup");

            int eth_en = 1;
            if (p_en) {
                if (cJSON_IsBool(p_en)) eth_en = cJSON_IsTrue(p_en);
                else if (cJSON_IsNumber(p_en)) eth_en = (p_en->valueint != 0);
                else if (cJSON_IsString(p_en)) eth_en = (strcmp(p_en->valuestring, "0") != 0 && strcasecmp(p_en->valuestring, "false") != 0);
            }
            int eth_fb = 1;
            if (p_fb) {
                if (cJSON_IsBool(p_fb)) eth_fb = cJSON_IsTrue(p_fb);
                else if (cJSON_IsNumber(p_fb)) eth_fb = (p_fb->valueint != 0);
                else if (cJSON_IsString(p_fb)) eth_fb = (strcmp(p_fb->valuestring, "0") != 0 && strcasecmp(p_fb->valuestring, "false") != 0);
            }

            /* Unlike the other fields, usb_wifi_setup left out keeps the stored value: the web UIs
             * send it only after reading the stored value, and an Ethernet save from an older
             * client must not turn the USB setup back on */
            int usb_wifi = wifi_usb_setup_setting(config_store_path("ethernet.conf"));
            if (p_usb) {
                if (cJSON_IsBool(p_usb)) usb_wifi = cJSON_IsTrue(p_usb);
                else if (cJSON_IsNumber(p_usb)) usb_wifi = (p_usb->valueint != 0);
                else if (cJSON_IsString(p_usb)) usb_wifi = (strcmp(p_usb->valuestring, "0") != 0 && strcasecmp(p_usb->valuestring, "false") != 0);
            }

            const char *mode_val = (p_mode && cJSON_IsString(p_mode) && strcasecmp(p_mode->valuestring, "static") == 0) ? "static" : "dhcp";
            const char *raw_ip = (p_ip && cJSON_IsString(p_ip)) ? p_ip->valuestring : "";
            const char *raw_mask = (p_mask && cJSON_IsString(p_mask)) ? p_mask->valuestring : "255.255.255.0";
            const char *raw_gw = (p_gw && cJSON_IsString(p_gw)) ? p_gw->valuestring : "";
            const char *raw_dns = (p_dns && cJSON_IsString(p_dns)) ? p_dns->valuestring : "";

            const char *valid_ip = is_safe_ipv4(raw_ip) ? raw_ip : "";
            const char *valid_mask = is_safe_ipv4(raw_mask) ? raw_mask : "255.255.255.0";
            const char *valid_gw = is_safe_ipv4(raw_gw) ? raw_gw : "";
            const char *valid_dns = (is_safe_param(raw_dns, ". ,") && strlen(raw_dns) < 64) ? raw_dns : "";

            /* A static address is applied to Ethernet and Wi-Fi with no way back: refuse values
             * that cannot work instead of saving them (dhcp mode keeps the old lenient handling) */
            if (strcmp(mode_val, "static") == 0) {
                const char *bad = static_address_error(raw_ip, raw_mask, raw_gw, raw_dns);
                if (bad) {
                    cJSON_Delete(params);
                    send_json_error(client_fd, 400, bad);
                    close(client_fd);
                    return 1;
                }
            }

            snprintf(conf_buf, sizeof(conf_buf),
                "ETH_ENABLED=%d\n"
                "ETH_FALLBACK_WIFI=%d\n"
                "ETH_MODE=\"%s\"\n"
                "ETH_IP=\"%s\"\n"
                "ETH_NETMASK=\"%s\"\n"
                "ETH_GATEWAY=\"%s\"\n"
                "ETH_DNS=\"%s\"\n"
                "USB_WIFI_SETUP=%d\n",
                eth_en, eth_fb, mode_val, valid_ip, valid_mask, valid_gw, valid_dns, usb_wifi);

            if (write_file_content(config_store_path("ethernet.conf"), conf_buf) != 0) {
                cJSON_Delete(params);
                send_json_error(client_fd, 500, "Could not save the Ethernet settings (flash full?)");
                close(client_fd);
                return 1;
            }

            cJSON *p_app = cJSON_GetObjectItemCaseSensitive(params, "apply");
            if (p_app && cJSON_IsString(p_app) && (strcmp(p_app->valuestring, "apply") == 0 || strcmp(p_app->valuestring, "1") == 0 || strcmp(p_app->valuestring, "reconfigure") == 0 || strcmp(p_app->valuestring, "restart") == 0)) {
                /* the installers (deploy_hub.py, update.sh) put the scripts in /data/harmony/, as
                 * init.sh expects: /data/harmony/bin/ holds no network_manager.sh, so the apply
                 * did nothing while the request answered "saved" */
                system("/data/harmony/network_manager.sh apply 2>/dev/null &");
            }
            cJSON_Delete(params);

            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "message", "Ethernet configuration saved");
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
            close(client_fd);
            return 1;
        }
        send_json_error(client_fd, 400, "Invalid parameters");
        close(client_fd);
        return 1;
    }

    return 0;
}
