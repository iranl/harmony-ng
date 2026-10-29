#include "http_api_remotes.h"
#include "http_util.h"
#include "config_store.h"
#include "bt_common.h"
#include "hub_mqtt.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int http_api_remotes_handle(int client_fd, const char *method, const char *clean_path,
                            const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    /* Route: GET /api/remote-mapping */
    if (strcmp(clean_path, "/api/remote-mapping") == 0 && strcmp(method, "GET") == 0) {
        FILE *f = fopen(config_store_path("bt_remote_map.json"), "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 200000) {
                char *buf = (char *)malloc((size_t)sz + 1);
                if (buf) {
                    size_t nr = fread(buf, 1, (size_t)sz, f);
                    buf[nr] = '\0';
                    cJSON *mj = cJSON_Parse(buf);
                    free(buf);
                    if (mj) {
                        send_cjson_resp(client_fd, 200, mj);
                        cJSON_Delete(mj);
                        fclose(f);
                        close(client_fd);
                        return 1;
                    }
                }
            }
            fclose(f);
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON *rem = cJSON_CreateObject();
        cJSON_AddStringToObject(rem, "name", "Homatics B25");
        cJSON_AddStringToObject(rem, "bdaddr", "");
        cJSON_AddItemToObject(st, "remote", rem);
        cJSON_AddItemToObject(st, "activities", cJSON_CreateObject());
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/elite-mapping */
    if (strcmp(clean_path, "/api/elite-mapping") == 0 && strcmp(method, "GET") == 0) {
        int slot = (query && strstr(query, "slot=2")) ? 2 : 1;
        char fname[64];
        snprintf(fname, sizeof(fname), "remote_slot%d_map.json", slot);
        const char *path = config_store_path(fname);
        FILE *f = fopen(path, "r");
        if (!f && slot == 1) {
            f = fopen(config_store_path("elite_remote_map.json"), "r");
        }
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 200000) {
                char *buf = (char *)malloc((size_t)sz + 1);
                if (buf) {
                    size_t nr = fread(buf, 1, (size_t)sz, f);
                    buf[nr] = '\0';
                    cJSON *mj = cJSON_Parse(buf);
                    free(buf);
                    if (mj) {
                        send_cjson_resp(client_fd, 200, mj);
                        cJSON_Delete(mj);
                        fclose(f);
                        close(client_fd);
                        return 1;
                    }
                }
            }
            fclose(f);
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddArrayToObject(st, "mappings");
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/remote-status or GET /api/remote-pair-status */
    if ((strcmp(clean_path, "/api/remote-status") == 0 || strcmp(clean_path, "/api/remote-pair-status") == 0) &&
        strcmp(method, "GET") == 0) {
        cJSON *st = NULL;
        FILE *f = fopen("/tmp/hub_btstack_status.json", "r");
        if (f) {
            char stat_buf[2048] = {0};
            fread(stat_buf, 1, sizeof(stat_buf) - 1, f);
            fclose(f);
            if (stat_buf[0]) st = cJSON_Parse(stat_buf);
        }
        if (!st) {
            st = cJSON_CreateObject();
            cJSON_AddStringToObject(st, "state", "idle");
        }
        cJSON_AddBoolToObject(st, "ok", 1);

        int running = is_proc_running("hub_btstack");
        cJSON_AddBoolToObject(st, "running", running);

        if (!cJSON_GetObjectItemCaseSensitive(st, "bdaddr")) {
            FILE *mf = fopen(config_store_path("bt_remote_map.json"), "r");
            if (mf) {
                char mbuf[2048] = {0};
                fread(mbuf, 1, sizeof(mbuf) - 1, mf);
                fclose(mf);
                cJSON *mj = cJSON_Parse(mbuf);
                if (mj) {
                    cJSON *rem = cJSON_GetObjectItemCaseSensitive(mj, "remote");
                    cJSON *addr = rem ? cJSON_GetObjectItemCaseSensitive(rem, "bdaddr") : cJSON_GetObjectItemCaseSensitive(mj, "bdaddr");
                    if (addr && cJSON_IsString(addr) && addr->valuestring[0]) {
                        cJSON_AddStringToObject(st, "bdaddr", addr->valuestring);
                        cJSON_AddStringToObject(st, "name", "Homatics B25");
                        cJSON_AddBoolToObject(st, "connected", running);
                    }
                    cJSON_Delete(mj);
                }
            }
        }
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/remote-scan */
    if (strcmp(clean_path, "/api/remote-scan") == 0 && strcmp(method, "POST") == 0) {
        FILE *cf = fopen("/tmp/hub_btstack_cmd", "w");
        if (cf) {
            fputs("SCAN\n", cf);
            fclose(cf);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "state", "scanning");
        cJSON_AddStringToObject(resp, "message", "Bluetooth remote scan started");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/remote-pair */
    if (strcmp(clean_path, "/api/remote-pair") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        char bdaddr[32] = {0};
        if (params) {
            cJSON *ja = cJSON_GetObjectItemCaseSensitive(params, "bdaddr");
            if (ja && cJSON_IsString(ja)) strncpy(bdaddr, ja->valuestring, sizeof(bdaddr) - 1);
            cJSON_Delete(params);
        }
        if (bdaddr[0]) {
            FILE *cf = fopen("/tmp/hub_btstack_cmd", "w");
            if (cf) {
                fprintf(cf, "PAIR %s\n", bdaddr);
                fclose(cf);
            }
            FILE *mf = fopen(config_store_path("bt_remote_map.json"), "r");
            if (mf) {
                fseek(mf, 0, SEEK_END);
                long sz = ftell(mf);
                fseek(mf, 0, SEEK_SET);
                if (sz > 0 && sz < 200000) {
                    char *b = (char *)malloc((size_t)sz + 1);
                    if (b) {
                        size_t nr = fread(b, 1, (size_t)sz, mf);
                        b[nr] = '\0';
                        cJSON *mj = cJSON_Parse(b);
                        free(b);
                        if (mj) {
                            cJSON *rem = cJSON_GetObjectItemCaseSensitive(mj, "remote");
                            if (!rem) {
                                rem = cJSON_CreateObject();
                                cJSON_AddItemToObject(mj, "remote", rem);
                            }
                            cJSON_DeleteItemFromObject(rem, "bdaddr");
                            cJSON_AddStringToObject(rem, "bdaddr", bdaddr);
                            char *up = cJSON_Print(mj);
                            if (up) {
                                write_file_atomic(config_store_path("bt_remote_map.json"), up);
                                free(up);
                            }
                            cJSON_Delete(mj);
                        }
                    }
                }
                fclose(mf);
            }
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "message", "Pair command issued to BTstack");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: /api/rf/... proxy to RF daemon (127.0.0.1:8092) */
    if (strncmp(clean_path, "/api/rf", 7) == 0) {
        int rfsock = socket(AF_INET, SOCK_STREAM, 0);
        if (rfsock >= 0) {
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(8092);
            sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };
            setsockopt(rfsock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(rfsock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            if (connect(rfsock, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
                char proxy_req[512];
                int body_len = (body && *body) ? (int)strlen(body) : 0;
                if (body_len > 0) {
                    int prlen = snprintf(proxy_req, sizeof(proxy_req),
                        "%s %s HTTP/1.1\r\nHost: 127.0.0.1:8092\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                        method, clean_path, body_len);
                    if (prlen > 0) write(rfsock, proxy_req, (size_t)prlen);
                    write(rfsock, body, (size_t)body_len);
                } else {
                    int prlen = snprintf(proxy_req, sizeof(proxy_req),
                        "%s %s HTTP/1.1\r\nHost: 127.0.0.1:8092\r\nConnection: close\r\n\r\n",
                        method, clean_path);
                    if (prlen > 0) write(rfsock, proxy_req, (size_t)prlen);
                }

                char resp_buf[1024];
                ssize_t rn;
                while ((rn = read(rfsock, resp_buf, sizeof(resp_buf))) > 0) {
                    write(client_fd, resp_buf, (size_t)rn);
                }
                close(rfsock);
                close(client_fd);
                return 1;
            }
            close(rfsock);
        }
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", 1);
        cJSON_AddBoolToObject(err, "running", 0);
        cJSON_AddStringToObject(err, "state", "stopped");
        cJSON_AddBoolToObject(err, "connected", 0);
        cJSON_AddBoolToObject(err, "pairing_active", 0);
        cJSON_AddBoolToObject(err, "paired", 0);
        cJSON_AddStringToObject(err, "rf_address", "None");
        cJSON_AddStringToObject(err, "hub_rf_address", "None");
        cJSON *slots = cJSON_AddArrayToObject(err, "slots");
        cJSON *s1 = cJSON_CreateObject();
        cJSON_AddNumberToObject(s1, "slot", 1);
        cJSON_AddBoolToObject(s1, "active", 0);
        cJSON_AddStringToObject(s1, "address", "None");
        cJSON_AddStringToObject(s1, "rf_address", "None");
        cJSON_AddItemToArray(slots, s1);
        cJSON *s2 = cJSON_CreateObject();
        cJSON_AddNumberToObject(s2, "slot", 2);
        cJSON_AddBoolToObject(s2, "active", 0);
        cJSON_AddStringToObject(s2, "address", "None");
        cJSON_AddStringToObject(s2, "rf_address", "None");
        cJSON_AddItemToArray(slots, s2);
        send_cjson_resp(client_fd, 200, err);
        cJSON_Delete(err);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/button-press or POST /api/mqtt-publish */
    if ((strcmp(clean_path, "/api/button-press") == 0 || strcmp(clean_path, "/api/mqtt-publish") == 0) &&
        strcmp(method, "POST") == 0) {
        char dev_id[64] = {0};
        char dev_name[128] = {0};
        char cmd_name[128] = {0};
        char req_source[32] = {0};
        char req_action[32] = {0};
        int req_pulse_ms = 0;
        char req_topic[256] = {0};
        int req_slot = 1;

        cJSON *req_j = body ? cJSON_Parse(body) : NULL;
        if (req_j) {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(req_j, "deviceId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(req_j, "id");
            if (jid && cJSON_IsString(jid) && jid->valuestring) strncpy(dev_id, jid->valuestring, sizeof(dev_id) - 1);
            else if (jid && cJSON_IsNumber(jid)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jid->valuedouble);

            cJSON *jdev = cJSON_GetObjectItemCaseSensitive(req_j, "device");
            if (jdev && cJSON_IsString(jdev) && jdev->valuestring) strncpy(dev_name, jdev->valuestring, sizeof(dev_name) - 1);

            cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "command");
            if (!jcmd) jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "button");
            if (jcmd && cJSON_IsString(jcmd) && jcmd->valuestring) strncpy(cmd_name, jcmd->valuestring, sizeof(cmd_name) - 1);

            cJSON *jsrc = cJSON_GetObjectItemCaseSensitive(req_j, "source");
            if (jsrc && cJSON_IsString(jsrc) && jsrc->valuestring) strncpy(req_source, jsrc->valuestring, sizeof(req_source) - 1);

            cJSON *jact = cJSON_GetObjectItemCaseSensitive(req_j, "action");
            if (jact && cJSON_IsString(jact) && jact->valuestring) strncpy(req_action, jact->valuestring, sizeof(req_action) - 1);

            cJSON *jtop = cJSON_GetObjectItemCaseSensitive(req_j, "topic");
            if (!jtop) jtop = cJSON_GetObjectItemCaseSensitive(req_j, "mqttTopic");
            if (jtop && cJSON_IsString(jtop) && jtop->valuestring[0]) strncpy(req_topic, jtop->valuestring, sizeof(req_topic) - 1);

            cJSON *jp = cJSON_GetObjectItemCaseSensitive(req_j, "pulseMs");
            if (!jp) jp = cJSON_GetObjectItemCaseSensitive(req_j, "pulse_ms");
            cJSON *js = cJSON_GetObjectItemCaseSensitive(req_j, "slot");
            if (js && cJSON_IsNumber(js) && js->valueint >= 1 && js->valueint <= 2) req_slot = js->valueint;
            cJSON_Delete(req_j);
        }

        if (!cmd_name[0]) {
            send_json_error(client_fd, 400, "Missing command parameter");
            close(client_fd);
            return 1;
        }

        char final_topic[256] = {0};
        int final_pulse_ms = 0;
        int mrc = -1;

        if (req_source[0]) {
            mrc = mqtt_dispatch_remote_button(req_source, req_action, cmd_name, req_slot, req_topic[0] ? req_topic : NULL, req_pulse_ms,
                                              final_topic, sizeof(final_topic), &final_pulse_ms);
        } else {
            if (!dev_name[0]) {
                if (dev_id[0]) strncpy(dev_name, dev_id, sizeof(dev_name) - 1);
                else strncpy(dev_name, "device", sizeof(dev_name) - 1);
            }
            mrc = mqtt_dispatch_button_pulse_topic(dev_id, dev_name, cmd_name, req_topic[0] ? req_topic : NULL, req_pulse_ms,
                                                   final_topic, sizeof(final_topic), &final_pulse_ms);
        }

        if (mrc == 0) {
            cJSON *resp_j = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp_j, "ok", 1);
            cJSON_AddBoolToObject(resp_j, "mirrored", 1);
            cJSON_AddNumberToObject(resp_j, "pulseMs", final_pulse_ms);
            cJSON_AddStringToObject(resp_j, "topic", final_topic);
            cJSON_AddStringToObject(resp_j, "reply", "ok");
            send_cjson_resp(client_fd, 200, resp_j);
            cJSON_Delete(resp_j);
        } else if (mrc == 1) {
            cJSON *resp_j = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp_j, "ok", 1);
            cJSON_AddBoolToObject(resp_j, "mirrored", 0);
            cJSON_AddStringToObject(resp_j, "message", "MQTT button mirroring disabled for source");
            send_cjson_resp(client_fd, 200, resp_j);
            cJSON_Delete(resp_j);
        } else {
            send_json_error(client_fd, 503, "MQTT broker not connected");
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/remote-mapping-save (Homatics B25 BT remote) */
    if (strcmp(clean_path, "/api/remote-mapping-save") == 0 && strcmp(method, "POST") == 0) {
        if (body && body[0]) {
            system("mkdir -p /data/harmony 2>/dev/null");
            write_file_atomic(config_store_path("bt_remote_map.json"), body);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/elite-mapping-save (Harmony RF remotes) */
    if (strcmp(clean_path, "/api/elite-mapping-save") == 0 && strcmp(method, "POST") == 0) {
        if (body && body[0]) {
            int slot = (query && strstr(query, "slot=2")) ? 2 : 1;
            cJSON *bj = cJSON_Parse(body);
            if (bj) {
                cJSON *js = cJSON_GetObjectItemCaseSensitive(bj, "slot");
                if (js && cJSON_IsNumber(js) && js->valueint == 2) slot = 2;
                cJSON_Delete(bj);
            }
            char fname[64];
            snprintf(fname, sizeof(fname), "remote_slot%d_map.json", slot);
            system("mkdir -p /data/harmony 2>/dev/null");
            write_file_atomic(config_store_path(fname), body);
            if (slot == 1) {
                write_file_atomic(config_store_path("elite_remote_map.json"), body);
            }
            notify_rf_push_config();
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/remote-mapping-copy */
    if (strcmp(clean_path, "/api/remote-mapping-copy") == 0 && strcmp(method, "POST") == 0) {
        int from_slot = 1;
        int to_slot = 2;
        if (query) {
            if (strstr(query, "from=2")) from_slot = 2;
            if (strstr(query, "to=1")) to_slot = 1;
        }
        if (body && body[0]) {
            cJSON *bj = cJSON_Parse(body);
            if (bj) {
                cJSON *jf = cJSON_GetObjectItemCaseSensitive(bj, "from");
                cJSON *jt = cJSON_GetObjectItemCaseSensitive(bj, "to");
                if (jf && cJSON_IsNumber(jf) && jf->valueint >= 1 && jf->valueint <= 2) from_slot = jf->valueint;
                if (jt && cJSON_IsNumber(jt) && jt->valueint >= 1 && jt->valueint <= 2) to_slot = jt->valueint;
                cJSON_Delete(bj);
            }
        }
        char src_name[64], dst_name[64];
        snprintf(src_name, sizeof(src_name), "remote_slot%d_map.json", from_slot);
        snprintf(dst_name, sizeof(dst_name), "remote_slot%d_map.json", to_slot);
        size_t flen = 0;
        char *content = read_file_text_alloc(config_store_path(src_name), 500000, &flen);
        if (!content && from_slot == 1) {
            content = read_file_text_alloc(config_store_path("elite_remote_map.json"), 500000, &flen);
        }
        if (content) {
            system("mkdir -p /data/harmony 2>/dev/null");
            write_file_atomic(config_store_path(dst_name), content);
            if (to_slot == 1) {
                write_file_atomic(config_store_path("elite_remote_map.json"), content);
            }
            free(content);
            notify_rf_push_config();
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddNumberToObject(resp, "from", from_slot);
            cJSON_AddNumberToObject(resp, "to", to_slot);
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        } else {
            send_json_error(client_fd, 404, "Source slot mapping not found");
        }
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/ui-remote-layout and POST /api/ui-remote-layout */
    if (strcmp(clean_path, "/api/ui-remote-layout") == 0) {
        if (strcmp(method, "POST") == 0) {
            if (body && body[0]) {
                system("mkdir -p /data/harmony 2>/dev/null");
                write_file_atomic(config_store_path("web_remote_layouts.json"), body);
            }
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
            close(client_fd);
            return 1;
        } else if (strcmp(method, "GET") == 0) {
            size_t flen = 0;
            char *buf = read_file_text_alloc(config_store_path("web_remote_layouts.json"), 200000, &flen);
            if (buf) {
                cJSON *mj = cJSON_Parse(buf);
                free(buf);
                if (mj) {
                    send_cjson_resp(client_fd, 200, mj);
                    cJSON_Delete(mj);
                    close(client_fd);
                    return 1;
                }
            }
            cJSON *def = cJSON_CreateObject();
            cJSON_AddBoolToObject(def, "ok", 1);
            cJSON_AddItemToObject(def, "layouts", cJSON_CreateObject());
            send_cjson_resp(client_fd, 200, def);
            cJSON_Delete(def);
            close(client_fd);
            return 1;
        }
    }

    /* Route: GET /api/bt-status */
    if (strcmp(clean_path, "/api/bt-status") == 0 && strcmp(method, "GET") == 0) {
        FILE *bf = fopen("/tmp/hub_btstack_status.json", "r");
        if (bf) {
            fseek(bf, 0, SEEK_END);
            long sz = ftell(bf);
            fseek(bf, 0, SEEK_SET);
            if (sz > 0 && sz < 32768) {
                char *buf = (char *)malloc((size_t)sz + 1);
                if (buf) {
                    size_t rd = fread(buf, 1, (size_t)sz, bf);
                    buf[rd] = '\0';
                    fclose(bf);
                    cJSON *bj = cJSON_Parse(buf);
                    free(buf);
                    if (bj) {
                        send_cjson_resp(client_fd, 200, bj);
                        cJSON_Delete(bj);
                        close(client_fd);
                        return 1;
                    }
                } else {
                    fclose(bf);
                }
            } else {
                fclose(bf);
            }
        }
        int bt_running = (access("/tmp/hub_btstack.pid", F_OK) == 0 || is_proc_running("hub_btstack"));
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddBoolToObject(st, "running", bt_running);
        cJSON_AddStringToObject(st, "state", bt_running ? "running" : "stopped");
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/services */
    if (strcmp(clean_path, "/api/services") == 0 && strcmp(method, "GET") == 0) {
        int rf_disabled = (access(config_store_path("rf_disabled.conf"), F_OK) == 0);
        int rf_running = is_proc_running("hub_rf");
        int bt_running = (access("/tmp/hub_btstack.pid", F_OK) == 0 || is_proc_running("hub_btstack"));

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddBoolToObject(st, "rf_enabled", !rf_disabled);
        cJSON_AddBoolToObject(st, "rf_running", rf_running);
        cJSON_AddBoolToObject(st, "bt_enabled", bt_running);
        cJSON_AddBoolToObject(st, "bt_running", bt_running);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/services/rf-toggle */
    if ((strcmp(clean_path, "/api/services/rf-toggle") == 0 || strcmp(clean_path, "/api/rf-toggle") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int enable = 0;
        if (params) {
            cJSON *item = cJSON_GetObjectItem(params, "enable");
            if (item) {
                if (cJSON_IsBool(item)) enable = cJSON_IsTrue(item);
                else if (cJSON_IsNumber(item)) enable = item->valueint;
                else if (cJSON_IsString(item)) enable = (atoi(item->valuestring) != 0 || strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0);
            }
            cJSON_Delete(params);
        }
        if (enable) {
            unlink(config_store_path("rf_disabled.conf"));
            system("killall -9 hub_rf 2>/dev/null; /data/harmony/bin/hub_rf &");
        } else {
            write_file_content(config_store_path("rf_disabled.conf"), "1\n");
            system("killall -9 hub_rf 2>/dev/null");
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddBoolToObject(st, "rf_enabled", enable);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/services/bt-toggle */
    if ((strcmp(clean_path, "/api/services/bt-toggle") == 0 || strcmp(clean_path, "/api/bt-toggle") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int enable = 0;
        if (params) {
            cJSON *item = cJSON_GetObjectItem(params, "enable");
            if (item) {
                if (cJSON_IsBool(item)) enable = cJSON_IsTrue(item);
                else if (cJSON_IsNumber(item)) enable = item->valueint;
                else if (cJSON_IsString(item)) enable = (atoi(item->valuestring) != 0 || strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0);
            }
            cJSON_Delete(params);
        }
        if (enable) {
            system("killall -9 hub_btstack 2>/dev/null; /data/harmony/bin/hub_btstack &");
        } else {
            system("killall -9 hub_btstack 2>/dev/null");
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddBoolToObject(st, "bt_running", enable);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/bt-pairing */
    if (strcmp(clean_path, "/api/bt-pairing") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int enable = 0;
        char name[64] = "Harmony Keyboard";
        char target_dev[64] = {0};
        if (params) {
            cJSON *je = cJSON_GetObjectItem(params, "enable");
            if (je) {
                if (cJSON_IsBool(je)) enable = cJSON_IsTrue(je);
                else if (cJSON_IsNumber(je)) enable = je->valueint;
                else if (cJSON_IsString(je)) enable = (atoi(je->valuestring) != 0 || strcmp(je->valuestring, "true") == 0 || strcmp(je->valuestring, "1") == 0);
            }
            cJSON *jn = cJSON_GetObjectItem(params, "name");
            if (jn && cJSON_IsString(jn) && jn->valuestring[0]) {
                strncpy(name, jn->valuestring, sizeof(name) - 1);
            }
            cJSON *jt = cJSON_GetObjectItem(params, "targetDev");
            if (!jt) jt = cJSON_GetObjectItem(params, "deviceId");
            if (jt) {
                if (cJSON_IsString(jt) && jt->valuestring[0]) strncpy(target_dev, jt->valuestring, sizeof(target_dev) - 1);
                else if (cJSON_IsNumber(jt)) snprintf(target_dev, sizeof(target_dev), "%ld", (long)jt->valuedouble);
            }
            cJSON_Delete(params);
        }
        if (enable && !target_dev[0]) {
            send_json_error(client_fd, 400, "Target device is required for Bluetooth pairing");
            close(client_fd);
            return 1;
        }
        if (enable && target_dev[0]) {
            FILE *pf = fopen("/tmp/bt_pending_pair_device", "w");
            if (pf) {
                fprintf(pf, "%s\n", target_dev);
                fclose(pf);
            }
        } else if (!enable) {
            unlink("/tmp/bt_pending_pair_device");
        }
        char pipe_cmd[128];
        if (enable) snprintf(pipe_cmd, sizeof(pipe_cmd), "pair_host_on %s\n", name);
        else snprintf(pipe_cmd, sizeof(pipe_cmd), "pair_host_off\n");
        hub_send_btstack_cmd(pipe_cmd);
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddBoolToObject(st, "pairing", enable);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/bt-connect */
    if (strcmp(clean_path, "/api/bt-connect") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        char bdaddr[32] = {0};
        if (params) {
            cJSON *ja = cJSON_GetObjectItem(params, "bdaddr");
            if (ja && cJSON_IsString(ja)) strncpy(bdaddr, ja->valuestring, sizeof(bdaddr) - 1);
            cJSON_Delete(params);
        }
        char pipe_cmd[128];
        if (bdaddr[0]) snprintf(pipe_cmd, sizeof(pipe_cmd), "connect_host %s\n", bdaddr);
        else snprintf(pipe_cmd, sizeof(pipe_cmd), "connect_host\n");
        hub_send_btstack_cmd(pipe_cmd);
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/bt-disconnect */
    if (strcmp(clean_path, "/api/bt-disconnect") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        char bdaddr[32] = {0};
        if (params) {
            cJSON *ja = cJSON_GetObjectItem(params, "bdaddr");
            if (ja && cJSON_IsString(ja)) strncpy(bdaddr, ja->valuestring, sizeof(bdaddr) - 1);
            cJSON_Delete(params);
        }
        char pipe_cmd[128];
        if (bdaddr[0]) snprintf(pipe_cmd, sizeof(pipe_cmd), "disconnect_host %s\n", bdaddr);
        else snprintf(pipe_cmd, sizeof(pipe_cmd), "disconnect_host\n");
        hub_send_btstack_cmd(pipe_cmd);
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/bt-link-device */
    if (strcmp(clean_path, "/api/bt-link-device") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        char dev_id[64] = {0};
        char bdaddr[32] = {0};
        if (params) {
            cJSON *jd = cJSON_GetObjectItem(params, "deviceId");
            if (!jd) jd = cJSON_GetObjectItem(params, "id");
            if (jd) {
                if (cJSON_IsString(jd)) strncpy(dev_id, jd->valuestring, sizeof(dev_id) - 1);
                else if (cJSON_IsNumber(jd)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jd->valuedouble);
            }
            cJSON *ja = cJSON_GetObjectItem(params, "bdaddr");
            if (ja && cJSON_IsString(ja)) strncpy(bdaddr, ja->valuestring, sizeof(bdaddr) - 1);
            cJSON_Delete(params);
        }
        int success = 0;
        if (dev_id[0]) {
            cJSON *root = config_store_get_devices_json();
            if (root) {
                cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                if (devs && cJSON_IsArray(devs)) {
                    cJSON *dev_item = NULL;
                    cJSON_ArrayForEach(dev_item, devs) {
                        cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
                        if (!d) d = dev_item;
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
                            if (bdaddr[0]) {
                                cJSON_AddStringToObject(d, "bdaddr", bdaddr);
                            }
                            config_store_save_devices(root);
                            success = 1;
                            break;
                        }
                    }
                }
            }
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", success);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/bt-mouse or /api/v1/bt/mouse */
    if ((strcmp(clean_path, "/api/bt-mouse") == 0 || strcmp(clean_path, "/api/v1/bt/mouse") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int dx = 0, dy = 0, buttons = 0, wheel = 0;
        char action[32] = {0};
        char button[32] = {0};
        char target[32] = {0};
        if (params) {
            cJSON *j_dx = cJSON_GetObjectItem(params, "dx");
            if (j_dx && cJSON_IsNumber(j_dx)) dx = j_dx->valueint;
            cJSON *j_dy = cJSON_GetObjectItem(params, "dy");
            if (j_dy && cJSON_IsNumber(j_dy)) dy = j_dy->valueint;
            cJSON *j_btn = cJSON_GetObjectItem(params, "buttons");
            if (j_btn && cJSON_IsNumber(j_btn)) buttons = j_btn->valueint;
            cJSON *j_wh = cJSON_GetObjectItem(params, "wheel");
            if (j_wh && cJSON_IsNumber(j_wh)) wheel = j_wh->valueint;
            cJSON *j_act = cJSON_GetObjectItem(params, "action");
            if (j_act && cJSON_IsString(j_act)) strncpy(action, j_act->valuestring, sizeof(action) - 1);
            cJSON *j_bname = cJSON_GetObjectItem(params, "button");
            if (j_bname && cJSON_IsString(j_bname)) strncpy(button, j_bname->valuestring, sizeof(button) - 1);
            cJSON *j_tgt = cJSON_GetObjectItem(params, "target");
            if (j_tgt && cJSON_IsString(j_tgt)) strncpy(target, j_tgt->valuestring, sizeof(target) - 1);
            cJSON_Delete(params);
        }
        int ok = 0;
        int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
        if (bfd >= 0) {
            if (target[0]) {
                char thdr[64];
                snprintf(thdr, sizeof(thdr), "TARGET %s\n", target);
                write(bfd, thdr, strlen(thdr));
            }
            char pipe_cmd[128];
            if (strcasecmp(action, "click") == 0) {
                const char *b = button[0] ? button : "left";
                snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_CLICK %s\n", b);
            } else if (strcasecmp(action, "down") == 0) {
                const char *b = button[0] ? button : "left";
                snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_DOWN %s\n", b);
            } else if (strcasecmp(action, "up") == 0) {
                const char *b = button[0] ? button : "left";
                snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_UP %s\n", b);
            } else if (strcasecmp(action, "wheel") == 0) {
                snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_WHEEL %d\n", wheel);
            } else {
                snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE %d %d %d %d\n", buttons, dx, dy, wheel);
            }
            if (write(bfd, pipe_cmd, strlen(pipe_cmd)) > 0) ok = 1;
            close(bfd);
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", ok);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/bt/reconnect or POST /bt/reconnect */
    if ((strcmp(clean_path, "/api/bt/reconnect") == 0 || strcmp(clean_path, "/bt/reconnect") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = body ? parse_request_params(body) : NULL;
        char tgt[64] = {0};
        int dur = 30;
        int dly = 0;
        if (p) {
            cJSON *jt = cJSON_GetObjectItemCaseSensitive(p, "target");
            if (!jt) jt = cJSON_GetObjectItemCaseSensitive(p, "bdaddr");
            if (!jt) jt = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (jt && cJSON_IsString(jt) && jt->valuestring) strncpy(tgt, jt->valuestring, sizeof(tgt) - 1);

            cJSON *jdur = cJSON_GetObjectItemCaseSensitive(p, "duration");
            if (jdur && cJSON_IsNumber(jdur) && jdur->valueint > 0) dur = jdur->valueint;

            cJSON *jdly = cJSON_GetObjectItemCaseSensitive(p, "delay");
            if (jdly && cJSON_IsNumber(jdly) && jdly->valueint >= 0) dly = jdly->valueint;
            cJSON_Delete(p);
        }
        char sh_cmd[256];
        if (tgt[0]) {
            snprintf(sh_cmd, sizeof(sh_cmd), "/data/harmony/bin/bt_reconnect.sh %s %d %d >/dev/null 2>&1 &",
                     tgt, dur, dly);
        } else {
            snprintf(sh_cmd, sizeof(sh_cmd), "/data/harmony/bin/bt_reconnect.sh \"\" %d %d >/dev/null 2>&1 &",
                     dur, dly);
        }
        system(sh_cmd);
        if (dly == 0) {
            char cmd[128];
            if (tgt[0]) snprintf(cmd, sizeof(cmd), "connect_host %s %d\n", tgt, dur);
            else snprintf(cmd, sizeof(cmd), "connect_host %d\n", dur);
            hub_send_btstack_cmd(cmd);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "message", "Bluetooth reconnect triggered");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    return 0;
}
