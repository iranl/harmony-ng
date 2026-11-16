#define _GNU_SOURCE
#include "http_api_plugins.h"
#include "plugin_mgr.h"
#include "http_util.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

int http_api_plugins_handle(int client_fd, const char *method, const char *path,
                            const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    if (strncmp(path, "/api/plugins", 12) != 0) return 0;

    /* 1. GET /api/plugins - List installed plugins */
    if (strcmp(path, "/api/plugins") == 0 && strcmp(method, "GET") == 0) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON *plugs = plugin_mgr_get_installed_json();
        cJSON_AddItemToObject(resp, "plugins", plugs ? plugs : cJSON_CreateArray());
        cJSON *storage = plugin_mgr_get_storage_json();
        if (storage) cJSON_AddItemToObject(resp, "storage", storage);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* 2. GET /api/plugins/marketplace - List available repo plugins */
    if (strcmp(path, "/api/plugins/marketplace") == 0 && strcmp(method, "GET") == 0) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON *cat = plugin_mgr_get_marketplace_catalog();
        cJSON_AddItemToObject(resp, "marketplace", cat ? cat : cJSON_CreateArray());
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* 3. GET /api/plugins/repositories - List repository URLs */
    if (strcmp(path, "/api/plugins/repositories") == 0 && strcmp(method, "GET") == 0) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON *repos = plugin_mgr_get_repositories();
        cJSON_AddItemToObject(resp, "repositories", repos ? repos : cJSON_CreateArray());
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* 4. POST /api/plugins/repositories - Add or remove repository URL */
    if (strcmp(path, "/api/plugins/repositories") == 0 && strcmp(method, "POST") == 0) {
        cJSON *json = body ? cJSON_Parse(body) : NULL;
        if (!json) {
            send_json_error(client_fd, 400, "Invalid JSON body");
            close(client_fd);
            return 1;
        }
        cJSON *j_act = cJSON_GetObjectItemCaseSensitive(json, "action");
        cJSON *j_url = cJSON_GetObjectItemCaseSensitive(json, "url");
        const char *act = (j_act && cJSON_IsString(j_act)) ? j_act->valuestring : "";
        const char *url = (j_url && cJSON_IsString(j_url)) ? j_url->valuestring : "";

        int rc = -1;
        if (strcmp(act, "add") == 0) {
            rc = plugin_mgr_add_repository(url);
        } else if (strcmp(act, "remove") == 0) {
            rc = plugin_mgr_remove_repository(url);
        }
        cJSON_Delete(json);

        if (rc == 0) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddItemToObject(resp, "repositories", plugin_mgr_get_repositories());
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        } else {
            send_json_error(client_fd, 400, "Failed to update repositories");
        }
        close(client_fd);
        return 1;
    }

    /* 5. POST /api/plugins/install - Install from URL or archive */
    if (strcmp(path, "/api/plugins/install") == 0 && strcmp(method, "POST") == 0) {
        char err[128] = "Unknown error";
        int rc = -1;

        cJSON *json = body ? cJSON_Parse(body) : NULL;
        if (json) {
            cJSON *j_url = cJSON_GetObjectItemCaseSensitive(json, "url");
            cJSON *j_b64 = cJSON_GetObjectItemCaseSensitive(json, "archive_base64");
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(json, "dev");
            int is_dev = (j_dev && (cJSON_IsTrue(j_dev) || (cJSON_IsNumber(j_dev) && j_dev->valueint == 1)));

            if (j_url && cJSON_IsString(j_url) && j_url->valuestring[0]) {
                const char *u = j_url->valuestring;
                if (!is_dev && (strstr(u, "manifest.json") != NULL || strstr(u, "raw.githubusercontent.com") != NULL)) {
                    if (!strstr(u, ".tar.gz") && !strstr(u, ".tgz")) {
                        is_dev = 1;
                    }
                }
                if (is_dev) {
                    rc = plugin_mgr_install_dev_raw(u, err, sizeof(err));
                } else {
                    rc = plugin_mgr_install_from_url(u, err, sizeof(err));
                }
            } else if (j_b64 && cJSON_IsString(j_b64)) {
                size_t out_len = 0;
                unsigned char *data = b64_decode(j_b64->valuestring, &out_len);
                if (data && out_len > 0) {
                    char tmp_tar[128];
                    snprintf(tmp_tar, sizeof(tmp_tar), "/tmp/upload_%ld.tar.gz", (long)time(NULL));
                    int fd = open(tmp_tar, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd >= 0) {
                        write(fd, data, out_len);
                        close(fd);
                        rc = plugin_mgr_install_archive(tmp_tar, err, sizeof(err));
                        unlink(tmp_tar);
                    }
                    free(data);
                } else {
                    snprintf(err, sizeof(err), "Failed to decode base64 archive");
                }
            }
            cJSON_Delete(json);
        } else if (body && body[0]) {
            /* Direct binary stream upload */
            const char *hdr_cl = strcasestr(req, "Content-Length:");
            size_t clen = hdr_cl ? (size_t)atoi(hdr_cl + 15) : strlen(body);
            if (clen > 0) {
                char tmp_tar[128];
                snprintf(tmp_tar, sizeof(tmp_tar), "/tmp/upload_%ld.tar.gz", (long)time(NULL));
                int fd = open(tmp_tar, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    write(fd, body, clen);
                    close(fd);
                    rc = plugin_mgr_install_archive(tmp_tar, err, sizeof(err));
                    unlink(tmp_tar);
                }
            }
        }

        if (rc == 0) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "message", "Plugin installed successfully");
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        } else {
            send_json_error(client_fd, 400, err);
        }
        close(client_fd);
        return 1;
    }

    /* 5b. POST /api/plugins/pull - Pull latest committed files directly from repo */
    if (strcmp(path, "/api/plugins/pull") == 0 && strcmp(method, "POST") == 0) {
        char err[128] = "Unknown error";
        int rc = -1;
        cJSON *json = body ? cJSON_Parse(body) : NULL;
        if (json) {
            cJSON *j_id = cJSON_GetObjectItemCaseSensitive(json, "id");
            const char *id = (j_id && cJSON_IsString(j_id)) ? j_id->valuestring : "";
            if (id[0]) {
                rc = plugin_mgr_pull(id, err, sizeof(err));
            } else {
                snprintf(err, sizeof(err), "Missing plugin id");
            }
            cJSON_Delete(json);
        } else {
            snprintf(err, sizeof(err), "Invalid JSON body");
        }

        if (rc == 0) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "message", "Plugin files updated directly from repository");
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        } else {
            send_json_error(client_fd, 400, err);
        }
        close(client_fd);
        return 1;
    }

    /* 5c. POST /api/plugins/pair - Pair plugin with external device */
    if (strcmp(path, "/api/plugins/pair") == 0 && strcmp(method, "POST") == 0) {
        cJSON *json = body ? cJSON_Parse(body) : NULL;
        if (!json) {
            send_json_error(client_fd, 400, "Invalid JSON body");
            close(client_fd);
            return 1;
        }

        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(json, "id");
        cJSON *j_act = cJSON_GetObjectItemCaseSensitive(json, "action");
        cJSON *j_host = cJSON_GetObjectItemCaseSensitive(json, "host");
        cJSON *j_pin = cJSON_GetObjectItemCaseSensitive(json, "pin");
        cJSON *j_port = cJSON_GetObjectItemCaseSensitive(json, "port");

        const char *id = (j_id && cJSON_IsString(j_id)) ? j_id->valuestring : "";
        const char *act = (j_act && cJSON_IsString(j_act)) ? j_act->valuestring : "start";
        const char *host = (j_host && cJSON_IsString(j_host)) ? j_host->valuestring : "";
        const char *pin = (j_pin && cJSON_IsString(j_pin)) ? j_pin->valuestring : "";
        int port = (j_port && cJSON_IsNumber(j_port)) ? j_port->valueint : 6467;

        if (!id[0] || !host[0]) {
            cJSON_Delete(json);
            send_json_error(client_fd, 400, "Missing plugin id or host");
            close(client_fd);
            return 1;
        }

        /* Ensure plugin daemon is running */
        if (!plugin_mgr_is_running(id)) {
            plugin_mgr_start(id);
            usleep(200000);
        }

        /* Forward pairing command to plugin unix socket */
        char sock_path[128];
        snprintf(sock_path, sizeof(sock_path), "/tmp/plugins/%s.sock", id);

        int s = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (s >= 0) {
            struct sockaddr_un sun;
            memset(&sun, 0, sizeof(sun));
            sun.sun_family = AF_UNIX;
            strncpy(sun.sun_path, sock_path, sizeof(sun.sun_path) - 1);

            int is_pin = (strcasecmp(act, "pin") == 0 || strcasecmp(act, "finish_pair") == 0);
            const char *fwd_action = is_pin ? "finish_pair" : "start_pair";

            cJSON *cmd_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(cmd_obj, "plugin_id", id);
            cJSON_AddStringToObject(cmd_obj, "command", "pair");
            cJSON_AddStringToObject(cmd_obj, "action", fwd_action);
            cJSON_AddStringToObject(cmd_obj, "host", host);
            cJSON_AddNumberToObject(cmd_obj, "port", port);
            if (pin[0]) cJSON_AddStringToObject(cmd_obj, "pin", pin);

            char *cmd_str = cJSON_PrintUnformatted(cmd_obj);
            if (cmd_str) {
                sendto(s, cmd_str, strlen(cmd_str), 0, (struct sockaddr *)&sun, sizeof(sun));
                free(cmd_str);
            }
            cJSON_Delete(cmd_obj);
            close(s);
        }

        cJSON_Delete(json);

        /* Wait up to 5 seconds for status file update */
        char stat_path[128];
        snprintf(stat_path, sizeof(stat_path), "/tmp/plugins/%s.status", id);

        int is_pin = (strcasecmp(act, "pin") == 0 || strcasecmp(act, "finish_pair") == 0);
        cJSON *res_obj = NULL;
        for (int i = 0; i < 50; i++) {
            usleep(100000); /* 100ms * 50 = 5s max */
            size_t slen = 0;
            char *stxt = read_file_text_alloc(stat_path, 4096, &slen);
            if (stxt) {
                cJSON *sj = cJSON_Parse(stxt);
                free(stxt);
                if (sj) {
                    cJSON *sj_act = cJSON_GetObjectItemCaseSensitive(sj, "action");
                    const char *sa = (sj_act && cJSON_IsString(sj_act)) ? sj_act->valuestring : "";
                    if (!sa[0] || strcasecmp(sa, act) == 0 ||
                        (!is_pin && (strcasecmp(sa, "start") == 0 || strcasecmp(sa, "start_pair") == 0)) ||
                        (is_pin && (strcasecmp(sa, "pin") == 0 || strcasecmp(sa, "finish_pair") == 0))) {
                        res_obj = sj;
                        break;
                    }
                    cJSON_Delete(sj);
                }
            }
        }

        if (!res_obj) {
            res_obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(res_obj, "ok", 0);
            cJSON_AddStringToObject(res_obj, "stage", "error");
            cJSON_AddStringToObject(res_obj, "message", "Device did not respond or pairing timed out");
        }

        int resp_code = 200;
        cJSON *j_ok = cJSON_GetObjectItemCaseSensitive(res_obj, "ok");
        if (j_ok && !cJSON_IsTrue(j_ok)) {
            resp_code = 400;
        }

        send_cjson_resp(client_fd, resp_code, res_obj);
        cJSON_Delete(res_obj);
        close(client_fd);
        return 1;
    }

    /* 6. POST /api/plugins/action - Control plugin lifecycle */
    if (strcmp(path, "/api/plugins/action") == 0 && strcmp(method, "POST") == 0) {
        cJSON *json = body ? cJSON_Parse(body) : NULL;
        if (!json) {
            send_json_error(client_fd, 400, "Invalid JSON body");
            close(client_fd);
            return 1;
        }
        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(json, "id");
        cJSON *j_act = cJSON_GetObjectItemCaseSensitive(json, "action");
        const char *id = (j_id && cJSON_IsString(j_id)) ? j_id->valuestring : "";
        const char *act = (j_act && cJSON_IsString(j_act)) ? j_act->valuestring : "";

        int rc = -1;
        if (strcmp(act, "start") == 0) rc = plugin_mgr_start(id);
        else if (strcmp(act, "stop") == 0) rc = plugin_mgr_stop(id);
        else if (strcmp(act, "restart") == 0) rc = plugin_mgr_restart(id);
        else if (strcmp(act, "enable") == 0) rc = plugin_mgr_set_enabled(id, 1);
        else if (strcmp(act, "disable") == 0) rc = plugin_mgr_set_enabled(id, 0);

        cJSON_Delete(json);

        if (rc == 0) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "id", id);
            cJSON_AddStringToObject(resp, "action", act);
            cJSON_AddBoolToObject(resp, "running", plugin_mgr_is_running(id));
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        } else {
            send_json_error(client_fd, 400, "Action failed or invalid plugin ID");
        }
        close(client_fd);
        return 1;
    }

    /* 7. DELETE /api/plugins or POST /api/plugins/uninstall - Uninstall plugin */
    if ((strcmp(path, "/api/plugins") == 0 && strcmp(method, "DELETE") == 0) ||
        (strcmp(path, "/api/plugins/uninstall") == 0 && strcmp(method, "POST") == 0)) {
        char target_id[64] = {0};
        cJSON *json = body ? cJSON_Parse(body) : NULL;
        if (json) {
            cJSON *j_id = cJSON_GetObjectItemCaseSensitive(json, "id");
            if (j_id && cJSON_IsString(j_id)) {
                strncpy(target_id, j_id->valuestring, sizeof(target_id) - 1);
            }
            cJSON_Delete(json);
        }
        if (!target_id[0] && query) {
            const char *qid = strstr(query, "id=");
            if (qid) sscanf(qid + 3, "%63[^&]", target_id);
        }

        if (target_id[0]) {
            char in_use_dev[64] = {0};
            if (plugin_mgr_is_in_use_by_device(target_id, in_use_dev, sizeof(in_use_dev))) {
                char err_buf[160];
                snprintf(err_buf, sizeof(err_buf), "Cannot delete plugin '%s': currently in use by device '%s'",
                         target_id, in_use_dev[0] ? in_use_dev : "configured device");
                send_json_error(client_fd, 400, err_buf);
                close(client_fd);
                return 1;
            }
        }

        if (target_id[0] && plugin_mgr_uninstall(target_id) == 0) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "id", target_id);
            cJSON_AddStringToObject(resp, "message", "Plugin uninstalled");
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        } else {
            send_json_error(client_fd, 400, "Failed to uninstall plugin");
        }
        close(client_fd);
        return 1;
    }

    return 0;
}
