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

            if (j_url && cJSON_IsString(j_url) && j_url->valuestring[0]) {
                rc = plugin_mgr_install_from_url(j_url->valuestring, err, sizeof(err));
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
