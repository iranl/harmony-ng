#include "http_api_network.h"
#include "hub_network_util.h"
#include "http_util.h"
#include "config_store.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

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
            }
            fclose(cff);
        }
        cJSON_AddItemToObject(st, "config", cfg);

        char ifconfig_buf[8192] = {0};
        FILE *pif = popen("/sbin/ifconfig 2>/dev/null", "r");
        if (pif) {
            size_t r = fread(ifconfig_buf, 1, sizeof(ifconfig_buf) - 1, pif);
            ifconfig_buf[r] = '\0';
            pclose(pif);
        }
        cJSON_AddStringToObject(st, "ifconfig", ifconfig_buf);

        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/wifi-scan or GET /api/wifi/scan */
    if ((strcmp(clean_path, "/api/wifi-scan") == 0 || strcmp(clean_path, "/api/wifi/scan") == 0) && strcmp(method, "GET") == 0) {
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
            int res = net_test_wifi_connection(raw_ssid, pass_buf, is_open, is_hidden, err_buf, sizeof(err_buf));
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
                char esc_ssid[128] = {0};
                char esc_pass[128] = {0};
                escape_wpa_str(raw_ssid, esc_ssid, sizeof(esc_ssid));

                int is_open = (p_open && ((cJSON_IsBool(p_open) && cJSON_IsTrue(p_open)) || (cJSON_IsNumber(p_open) && p_open->valueint == 1) || (cJSON_IsString(p_open) && strcmp(p_open->valuestring, "1") == 0)));
                int is_hidden = (p_hid && ((cJSON_IsBool(p_hid) && cJSON_IsTrue(p_hid)) || (cJSON_IsNumber(p_hid) && p_hid->valueint == 1) || (cJSON_IsString(p_hid) && strcmp(p_hid->valuestring, "1") == 0)));
                int keep_pass = (p_keep && ((cJSON_IsBool(p_keep) && cJSON_IsTrue(p_keep)) || (cJSON_IsNumber(p_keep) && p_keep->valueint == 1) || (cJSON_IsString(p_keep) && strcmp(p_keep->valuestring, "1") == 0)));
                int force_save = (p_force && ((cJSON_IsBool(p_force) && cJSON_IsTrue(p_force)) || (cJSON_IsNumber(p_force) && p_force->valueint == 1) || (cJSON_IsString(p_force) && strcmp(p_force->valuestring, "1") == 0)));

                const char *app_mode = (p_app && cJSON_IsString(p_app)) ? p_app->valuestring : "save";
                int should_apply = (strcmp(app_mode, "reconfigure") == 0 || strcmp(app_mode, "apply") == 0 || strcmp(app_mode, "1") == 0 || strcmp(app_mode, "reboot") == 0);

                if (p_pass && cJSON_IsString(p_pass) && p_pass->valuestring[0]) {
                    escape_wpa_str(p_pass->valuestring, esc_pass, sizeof(esc_pass));
                } else if (keep_pass && !is_open) {
                    net_get_current_wifi_psk(esc_pass, sizeof(esc_pass));
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

                char wpa_buf[1024];
                if (is_open) {
                    snprintf(wpa_buf, sizeof(wpa_buf),
                        "ctrl_interface=/var/run/wpa_supplicant\n"
                        "update_config=1\n\n"
                        "network={\n"
                        "\tssid=\"%s\"\n"
                        "\tkey_mgmt=NONE\n"
                        "%s"
                        "}\n",
                        esc_ssid,
                        is_hidden ? "\tscan_ssid=1\n" : "");
                } else {
                    snprintf(wpa_buf, sizeof(wpa_buf),
                        "ctrl_interface=/var/run/wpa_supplicant\n"
                        "update_config=1\n\n"
                        "network={\n"
                        "\tssid=\"%s\"\n"
                        "\tpsk=\"%s\"\n"
                        "%s"
                        "}\n",
                        esc_ssid, esc_pass,
                        is_hidden ? "\tscan_ssid=1\n" : "");
                }

                write_file_content("/etc/wpa_supplicant.conf", wpa_buf);
                write_file_content("/etc/wpa_supplicant.ath0.conf", wpa_buf);

                if (strcmp(app_mode, "reboot") == 0) {
                    system("/data/harmony/bin/safe_reboot.sh 2>/dev/null || (killall -9 hub_rf hub_btstack 2>/dev/null; /data/harmony/bin/hub_sync 2>/dev/null; sleep 2; reboot -f) &");
                } else if (should_apply) {
                    system("/usr/sbin/wpa_cli -i ath0 reconfigure 2>/dev/null || killall -HUP wpa_supplicant 2>/dev/null &");
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

            const char *mode_val = (p_mode && cJSON_IsString(p_mode) && strcasecmp(p_mode->valuestring, "static") == 0) ? "static" : "dhcp";
            const char *raw_ip = (p_ip && cJSON_IsString(p_ip)) ? p_ip->valuestring : "";
            const char *raw_mask = (p_mask && cJSON_IsString(p_mask)) ? p_mask->valuestring : "255.255.255.0";
            const char *raw_gw = (p_gw && cJSON_IsString(p_gw)) ? p_gw->valuestring : "";
            const char *raw_dns = (p_dns && cJSON_IsString(p_dns)) ? p_dns->valuestring : "";

            const char *valid_ip = is_safe_ipv4(raw_ip) ? raw_ip : "";
            const char *valid_mask = is_safe_ipv4(raw_mask) ? raw_mask : "255.255.255.0";
            const char *valid_gw = is_safe_ipv4(raw_gw) ? raw_gw : "";
            const char *valid_dns = (is_safe_param(raw_dns, ". ,") && strlen(raw_dns) < 64) ? raw_dns : "";

            snprintf(conf_buf, sizeof(conf_buf),
                "ETH_ENABLED=%d\n"
                "ETH_FALLBACK_WIFI=%d\n"
                "ETH_MODE=\"%s\"\n"
                "ETH_IP=\"%s\"\n"
                "ETH_NETMASK=\"%s\"\n"
                "ETH_GATEWAY=\"%s\"\n"
                "ETH_DNS=\"%s\"\n",
                eth_en, eth_fb, mode_val, valid_ip, valid_mask, valid_gw, valid_dns);

            write_file_content(config_store_path("ethernet.conf"), conf_buf);

            cJSON *p_app = cJSON_GetObjectItemCaseSensitive(params, "apply");
            if (p_app && cJSON_IsString(p_app) && (strcmp(p_app->valuestring, "apply") == 0 || strcmp(p_app->valuestring, "1") == 0 || strcmp(p_app->valuestring, "reconfigure") == 0 || strcmp(p_app->valuestring, "restart") == 0)) {
                system("/data/harmony/bin/network_manager.sh apply 2>/dev/null &");
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
