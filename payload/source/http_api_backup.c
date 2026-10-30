#include "http_api_backup.h"
#include "http_util.h"
#include "http_auth.h"
#include "config_store.h"
#include "resource_cache.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>

int http_api_backup_handle(int client_fd, const char *method, const char *clean_path,
                           const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    /* Route: GET /export/<target> */
    if (strncmp(clean_path, "/export/", 8) == 0 && strcmp(method, "GET") == 0) {
        const char *tgt = clean_path + 8;
        if (strcmp(tgt, "bundle") == 0) {
            cJSON *bundle = cJSON_CreateObject();
            cJSON_AddNumberToObject(bundle, "version", 1);
            cJSON_AddNumberToObject(bundle, "timestamp", (double)time(NULL));

            static const struct { const char *key; const char *path; } b_files[] = {
                { "devices", "/data/harmony/config/devices.json" },
                { "activities", "/data/harmony/config/activities.json" },
                { "settings", "/data/harmony/config/settings.json" },
                { "mqtt", "/data/harmony/mqtt.json" },
                { "network", "/data/harmony/ethernet.conf" },
                { "wifi", "/etc/wpa_supplicant.conf" },
                { "bluetooth", "/data/harmony/bt-devices.json" },
                { "bt_remote_map", "/data/harmony/bt_remote_map.json" },
                { "remote_slot1_map", "/data/harmony/remote_slot1_map.json" },
                { "remote_slot2_map", "/data/harmony/remote_slot2_map.json" },
                { "ui_remote_layout", "/data/harmony/web_remote_layouts.json" },
                { "auth", "/data/harmony/webui_auth.conf" },
                { "users", "/data/harmony/users.json" },
                { "hub_id", "/data/harmony/hub_id" },
                { NULL, NULL }
            };

            for (int i = 0; b_files[i].key; i++) {
                const char *use_path = b_files[i].path;
                size_t flen = 0;
                char *txt = read_file_text_alloc(use_path, 2000000, &flen);
                if (txt) {
                    cJSON *parsed = cJSON_Parse(txt);
                    if (parsed) {
                        cJSON_AddItemToObject(bundle, b_files[i].key, parsed);
                    } else {
                        cJSON_AddStringToObject(bundle, b_files[i].key, txt);
                    }
                    free(txt);
                }
            }

            FILE *kf = fopen("/data/harmony/btstack_keys.bin", "rb");
            if (!kf) kf = fopen("/data/harmony/btstack_keys.tlv", "rb");
            if (kf) {
                fseek(kf, 0, SEEK_END);
                long ksz = ftell(kf);
                fseek(kf, 0, SEEK_SET);
                if (ksz > 0 && ksz < 200000) {
                    unsigned char *kbuf = (unsigned char *)malloc((size_t)ksz);
                    if (kbuf) {
                        size_t kr = fread(kbuf, 1, (size_t)ksz, kf);
                        char *b64 = b64_encode(kbuf, kr);
                        if (b64) {
                            cJSON_AddStringToObject(bundle, "btstack_keys", b64);
                            free(b64);
                        }
                        free(kbuf);
                    }
                }
                fclose(kf);
            }

            char *json_out = cJSON_Print(bundle);
            cJSON_Delete(bundle);
            if (json_out) {
                size_t jlen = strlen(json_out);
                uint32_t crc = crc32_simple((const unsigned char *)json_out, jlen);
                char header[512];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json; charset=utf-8\r\n"
                    "Content-Disposition: attachment; filename=\"harmony_backup_bundle.json\"\r\n"
                    "X-Checksum-CRC32: %08x\r\n"
                    "Content-Length: %zu\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n",
                    crc, jlen);
                send(client_fd, header, (size_t)hlen, 0);
                send(client_fd, json_out, jlen, 0);
                free(json_out);
                close(client_fd);
                return 1;
            }
        }

        const char *file_to_send = NULL;
        const char *download_name = "backup.json";
        const char *ctype = "application/json; charset=utf-8";

        if (strcmp(tgt, "devices") == 0) {
            file_to_send = config_store_path("config/devices.json");
            download_name = "devices.json";
        } else if (strcmp(tgt, "activities") == 0) {
            file_to_send = config_store_path("config/activities.json");
            download_name = "activities.json";
        } else if (strcmp(tgt, "settings") == 0) {
            file_to_send = config_store_path("config/settings.json");
            download_name = "settings.json";
        } else if (strcmp(tgt, "mqtt") == 0) {
            file_to_send = config_store_path("mqtt.json");
            download_name = "mqtt.json";
        } else if (strcmp(tgt, "network") == 0 || strcmp(tgt, "ethernet") == 0) {
            file_to_send = config_store_path("ethernet.conf");
            download_name = "ethernet.conf";
            ctype = "text/plain; charset=utf-8";
        } else if (strcmp(tgt, "wifi") == 0) {
            file_to_send = "/etc/wpa_supplicant.conf";
            download_name = "wpa_supplicant.conf";
            ctype = "text/plain; charset=utf-8";
        } else if (strcmp(tgt, "bluetooth") == 0) {
            file_to_send = config_store_path("bt-devices.json");
            download_name = "bt-devices.json";
        } else if (strcmp(tgt, "remote-mapping") == 0 || strcmp(tgt, "bt_remote_map") == 0) {
            file_to_send = config_store_path("bt_remote_map.json");
            download_name = "bt_remote_map.json";
        } else if (strcmp(tgt, "remote_slot1_map") == 0) {
            file_to_send = config_store_path("remote_slot1_map.json");
            download_name = "remote_slot1_map.json";
        } else if (strcmp(tgt, "remote_slot2_map") == 0) {
            file_to_send = config_store_path("remote_slot2_map.json");
            download_name = "remote_slot2_map.json";
        } else if (strcmp(tgt, "ui-remote-layout") == 0 || strcmp(tgt, "ui_remote_layout") == 0) {
            file_to_send = config_store_path("web_remote_layouts.json");
            download_name = "web_remote_layouts.json";
        } else if (strcmp(tgt, "auth") == 0) {
            file_to_send = config_store_path("webui_auth.conf");
            download_name = "webui_auth.conf";
            ctype = "text/plain; charset=utf-8";
        } else if (strcmp(tgt, "users") == 0) {
            file_to_send = config_store_path("users.json");
            download_name = "users.json";
        } else if (strcmp(tgt, "hub-id") == 0 || strcmp(tgt, "hub_id") == 0) {
            file_to_send = config_store_path("hub_id");
            download_name = "hub_id";
            ctype = "text/plain; charset=utf-8";
        }

        if (file_to_send) {
            size_t flen = 0;
            char *buf = read_file_text_alloc(file_to_send, 2000000, &flen);
            if (buf) {
                char header[512];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Disposition: attachment; filename=\"%s\"\r\n"
                    "Content-Length: %zu\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n",
                    ctype, download_name, flen);
                send(client_fd, header, (size_t)hlen, 0);
                send(client_fd, buf, flen, 0);
                free(buf);
                close(client_fd);
                return 1;
            }
        }
        send_json_error(client_fd, 404, "Export target not found");
        close(client_fd);
        return 1;
    }

    /* Route: POST /import */
    if (strcmp(clean_path, "/import") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int success = 0;
        if (params) {
            cJSON *p_tgt = cJSON_GetObjectItemCaseSensitive(params, "target");
            cJSON *p_pay = cJSON_GetObjectItemCaseSensitive(params, "payload");
            const char *tgt = p_tgt ? (cJSON_IsString(p_tgt) ? p_tgt->valuestring : "") : "";

            cJSON *p_net = cJSON_GetObjectItemCaseSensitive(params, "restore_network");
            int allow_network = (p_net && (cJSON_IsTrue(p_net) || (cJSON_IsNumber(p_net) && p_net->valueint > 0)));

            if (strcmp(tgt, "bundle") == 0) {
                cJSON *bundle = NULL;
                if (p_pay && cJSON_IsString(p_pay)) bundle = cJSON_Parse(p_pay->valuestring);
                else if (p_pay && cJSON_IsObject(p_pay)) bundle = p_pay;
                else bundle = params;

                if (bundle) {
                    static const struct { const char *key; const char *path; int is_net; } b_files[] = {
                        { "devices", "/data/harmony/config/devices.json", 0 },
                        { "activities", "/data/harmony/config/activities.json", 0 },
                        { "settings", "/data/harmony/config/settings.json", 0 },
                        { "mqtt", "/data/harmony/mqtt.json", 0 },
                        { "network", "/data/harmony/ethernet.conf", 1 },
                        { "wifi", "/etc/wpa_supplicant.conf", 1 },
                        { "bluetooth", "/data/harmony/bt-devices.json", 0 },
                        { "bt_remote_map", "/data/harmony/bt_remote_map.json", 0 },
                        { "remote_slot1_map", "/data/harmony/remote_slot1_map.json", 0 },
                        { "remote_slot2_map", "/data/harmony/remote_slot2_map.json", 0 },
                        { "ui_remote_layout", "/data/harmony/web_remote_layouts.json", 0 },
                        { "auth", "/data/harmony/webui_auth.conf", 0 },
                        { "users", "/data/harmony/users.json", 0 },
                        { "hub_id", "/data/harmony/hub_id", 0 },
                        { NULL, NULL, 0 }
                    };

                    for (int i = 0; b_files[i].key; i++) {
                        if (b_files[i].is_net && !allow_network) {
                            continue;
                        }
                        cJSON *sec = cJSON_GetObjectItemCaseSensitive(bundle, b_files[i].key);
                        if (sec) {
                            if (strcmp(b_files[i].key, "mqtt") == 0 && cJSON_IsObject(sec)) {
                                if (!cJSON_GetObjectItemCaseSensitive(sec, "enabled")) {
                                    cJSON *h = cJSON_GetObjectItemCaseSensitive(sec, "host");
                                    if (!h) {
                                        cJSON *bk = cJSON_GetObjectItemCaseSensitive(sec, "broker");
                                        if (bk) h = cJSON_GetObjectItemCaseSensitive(bk, "host");
                                    }
                                    if (h && cJSON_IsString(h) && h->valuestring && h->valuestring[0]) {
                                        cJSON_AddBoolToObject(sec, "enabled", 1);
                                    }
                                }
                            }
                            if (cJSON_IsString(sec)) {
                                write_file_atomic(b_files[i].path, sec->valuestring);
                            } else {
                                char *pstr = cJSON_PrintUnformatted(sec);
                                if (pstr) {
                                    write_file_atomic(b_files[i].path, pstr);
                                    free(pstr);
                                }
                            }
                        }
                    }

                    cJSON *j_keys = cJSON_GetObjectItemCaseSensitive(bundle, "btstack_keys");
                    if (j_keys && cJSON_IsString(j_keys) && j_keys->valuestring[0]) {
                        size_t klen = 0;
                        unsigned char *raw_keys = b64_decode(j_keys->valuestring, &klen);
                        if (raw_keys) {
                            write_file_atomic_sized("/data/harmony/btstack_keys.tlv", (const char *)raw_keys, klen);
                            write_file_atomic_sized("/data/harmony/btstack_keys.bin", (const char *)raw_keys, klen);
                            free(raw_keys);
                        }
                    }

                    sync();
                    invalidate_device_list_cache();
                    config_store_init();
                    notify_rf_push_config();
                    struct user_account tmp_u[MAX_HTTP_USERS];
                    int tmp_en = 0;
                    int tmp_c = load_user_accounts(tmp_u, MAX_HTTP_USERS, &tmp_en);
                    if (tmp_c > 0) save_user_accounts(tmp_u, tmp_c, tmp_en);
                    success = 1;
                }
                if (bundle && bundle != p_pay && bundle != params) cJSON_Delete(bundle);
            } else if (tgt[0] && p_pay) {
                const char *file_to_write = NULL;
                if (strcmp(tgt, "devices") == 0) { file_to_write = "/data/harmony/config/devices.json"; }
                else if (strcmp(tgt, "activities") == 0) { file_to_write = "/data/harmony/config/activities.json"; }
                else if (strcmp(tgt, "settings") == 0) { file_to_write = "/data/harmony/config/settings.json"; }
                else if (strcmp(tgt, "mqtt") == 0) { file_to_write = "/data/harmony/mqtt.json"; }
                else if (strcmp(tgt, "network") == 0 || strcmp(tgt, "ethernet") == 0) { file_to_write = "/data/harmony/ethernet.conf"; }
                else if (strcmp(tgt, "wifi") == 0) { file_to_write = "/etc/wpa_supplicant.conf"; }
                else if (strcmp(tgt, "bluetooth") == 0) { file_to_write = "/data/harmony/bt-devices.json"; }
                else if (strcmp(tgt, "remote-mapping") == 0 || strcmp(tgt, "bt_remote_map") == 0) { file_to_write = "/data/harmony/bt_remote_map.json"; }
                else if (strcmp(tgt, "remote_slot1_map") == 0) { file_to_write = "/data/harmony/remote_slot1_map.json"; }
                else if (strcmp(tgt, "remote_slot2_map") == 0) { file_to_write = "/data/harmony/remote_slot2_map.json"; }
                else if (strcmp(tgt, "ui-remote-layout") == 0 || strcmp(tgt, "ui_remote_layout") == 0) { file_to_write = "/data/harmony/web_remote_layouts.json"; }
                else if (strcmp(tgt, "auth") == 0) { file_to_write = "/data/harmony/webui_auth.conf"; }
                else if (strcmp(tgt, "users") == 0) { file_to_write = "/data/harmony/users.json"; }
                else if (strcmp(tgt, "hub_id") == 0 || strcmp(tgt, "hub-id") == 0) { file_to_write = "/data/harmony/hub_id"; }

                if (file_to_write) {
                    if (cJSON_IsString(p_pay)) {
                        write_file_atomic(file_to_write, p_pay->valuestring);
                        success = 1;
                    } else {
                        char *pstr = cJSON_PrintUnformatted(p_pay);
                        if (pstr) {
                            write_file_atomic(file_to_write, pstr);
                            free(pstr);
                            success = 1;
                        }
                    }
                    if (success) {
                        invalidate_device_list_cache();
                        config_store_init();
                        if (strcmp(tgt, "devices") == 0 || strcmp(tgt, "activities") == 0 || strcmp(tgt, "remote_slot1_map") == 0 || strcmp(tgt, "remote_slot2_map") == 0) {
                            notify_rf_push_config();
                        }
                        if (strcmp(tgt, "auth") == 0 || strcmp(tgt, "users") == 0) {
                            struct user_account tmp_u[MAX_HTTP_USERS];
                            int tmp_en = 0;
                            int tmp_c = load_user_accounts(tmp_u, MAX_HTTP_USERS, &tmp_en);
                            if (tmp_c > 0) save_user_accounts(tmp_u, tmp_c, tmp_en);
                        }
                    }
                }
            }
            cJSON_Delete(params);
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        cJSON_AddStringToObject(resp, "message", success ? "Restored successfully" : "Import failed");
        send_cjson_resp(client_fd, success ? 200 : 400, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    return 0;
}
