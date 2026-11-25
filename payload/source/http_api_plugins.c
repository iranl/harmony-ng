#define _GNU_SOURCE
#include "hw_action.h"
#include "http_api_plugins.h"
#include "plugin_mgr.h"
#include "http_util.h"
#include "http_auth.h"
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
#include <pthread.h>

/* The largest archive an upload may carry (base64 in JSON, 4/3 of this on the wire) */
#define PLUGIN_UPLOAD_MAX_BYTES (1L * 1024 * 1024)

/* A plugin runs as root: installing one is running code as root. While authentication is off
 * every LAN client is an admin, so these routes are refused until it is on. */
static int refuse_without_auth(int client_fd) {
    if (http_auth_is_enabled()) return 0;
    send_json_error(client_fd, 403, "Enable authentication before installing plugins or changing repositories: a plugin runs as root on the Hub");
    close(client_fd);
    return 1;
}

/* 409: another install runs; 502: the Hub could not download (the browser may try); else 400 */
static int install_status(int rc) {
    return rc == PLUGIN_ERR_BUSY ? 409 : (rc == PLUGIN_ERR_DOWNLOAD ? 502 : 400);
}

/* A pairing status may carry "config": values the plugin learned (an address, an id) for the
 * UI to put in the device's fields. Only the keys the manifest's config_fields declare stay. */
static void plugin_pair_filter_config(cJSON *status, const cJSON *dev_def) {
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(status, "config");
    if (!cfg) return;
    if (!cJSON_IsObject(cfg)) {
        cJSON_DeleteItemFromObjectCaseSensitive(status, "config");
        return;
    }
    const cJSON *fields = dev_def ? cJSON_GetObjectItemCaseSensitive(dev_def, "config_fields") : NULL;
    cJSON *item = cfg->child;
    while (item) {
        cJSON *next = item->next;
        int known = 0;
        const cJSON *f = NULL;
        cJSON_ArrayForEach(f, (cJSON_IsArray(fields) ? fields : NULL)) {
            const cJSON *k = cJSON_GetObjectItemCaseSensitive(f, "key");
            if (cJSON_IsString(k) && item->string && strcmp(k->valuestring, item->string) == 0) known = 1;
        }
        if (!known || !(cJSON_IsString(item) || cJSON_IsNumber(item) || cJSON_IsBool(item))) {
            cJSON_Delete(cJSON_DetachItemViaPointer(cfg, item));
        }
        item = next;
    }
}

/* The status the plugin wrote, when it answers this request (any action when want is NULL) */
static cJSON *read_pair_status(const char *stat_path, const char *want, int is_pin) {
    size_t slen = 0;
    char *stxt = read_file_text_alloc(stat_path, 4096, &slen);
    if (!stxt) return NULL;
    cJSON *sj = cJSON_Parse(stxt);
    free(stxt);
    if (!sj || !want) return sj;
    cJSON *sj_act = cJSON_GetObjectItemCaseSensitive(sj, "action");
    const char *sa = (sj_act && cJSON_IsString(sj_act)) ? sj_act->valuestring : "";
    if (!sa[0] || strcasecmp(sa, want) == 0 ||
        (!is_pin && (strcasecmp(sa, "start") == 0 || strcasecmp(sa, "start_pair") == 0)) ||
        (is_pin && (strcasecmp(sa, "pin") == 0 || strcasecmp(sa, "finish_pair") == 0))) {
        return sj;
    }
    cJSON_Delete(sj);
    return NULL;
}

int http_api_plugins_handle(int client_fd, const char *method, const char *path,
                            const char *query, const char *body, const char *req) {
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
        int force = (query && (strstr(query, "refresh=1") || strstr(query, "forceReload=1"))) ? 1 : 0;
        cJSON *cat = plugin_mgr_get_marketplace_catalog(force);
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
        if (refuse_without_auth(client_fd)) return 1;
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

    /* 5. POST /api/plugins/install - Install from URL or base64 archive. Every install that is
     * not marked dev needs a SHA-256 (given, or from the marketplace entry of the URL). */
    if (strcmp(path, "/api/plugins/install") == 0 && strcmp(method, "POST") == 0) {
        if (refuse_without_auth(client_fd)) return 1;
        char err[192] = "Unknown error";
        int rc = -1;
        int status = 400;

        cJSON *json = body ? cJSON_Parse(body) : NULL;
        if (json) {
            cJSON *j_url = cJSON_GetObjectItemCaseSensitive(json, "url");
            cJSON *j_b64 = cJSON_GetObjectItemCaseSensitive(json, "archive_base64");
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(json, "dev");
            cJSON *j_sha = cJSON_GetObjectItemCaseSensitive(json, "sha256");
            const char *sha = (cJSON_IsString(j_sha) && j_sha->valuestring[0]) ? j_sha->valuestring : NULL;
            int is_dev = (j_dev && (cJSON_IsTrue(j_dev) || (cJSON_IsNumber(j_dev) && j_dev->valueint == 1)));
            int has_url = (j_url && cJSON_IsString(j_url) && j_url->valuestring[0]);

            if (sha && strlen(sha) != 64) {
                snprintf(err, sizeof(err), "sha256 must be 64 hex characters");
            } else if (is_dev && sha && has_url) {
                /* the dev path downloads loose files: nothing it could check the value against */
                snprintf(err, sizeof(err), "sha256 cannot be checked on a dev install (raw files); leave it out or install the release archive");
            } else if (has_url) {
                if (is_dev) rc = plugin_mgr_install_dev_raw(j_url->valuestring, err, sizeof(err));
                else rc = plugin_mgr_install_from_url(j_url->valuestring, sha, err, sizeof(err));
                status = install_status(rc);
            } else if (j_b64 && cJSON_IsString(j_b64)) {
                if (!sha && !is_dev) {
                    snprintf(err, sizeof(err), "An uploaded archive needs sha256, or dev: true for a local build");
                } else if (strlen(j_b64->valuestring) > (size_t)(PLUGIN_UPLOAD_MAX_BYTES / 3 * 4 + 4)) {
                    snprintf(err, sizeof(err), "Archive larger than 1 MB");
                    status = 413;
                } else {
                    size_t out_len = 0;
                    unsigned char *data = b64_decode(j_b64->valuestring, &out_len);
                    if (data && out_len > 0) {
                        char tmp_tar[128];
                        snprintf(tmp_tar, sizeof(tmp_tar), "/tmp/upload_%ld_%lx.tar.gz", (long)time(NULL), (unsigned long)pthread_self());
                        int fd = open(tmp_tar, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (fd >= 0) {
                            ssize_t w = write(fd, data, out_len);
                            close(fd);
                            if (w == (ssize_t)out_len) {
                                rc = plugin_mgr_install_archive(tmp_tar, sha, err, sizeof(err));
                                status = install_status(rc);
                            } else {
                                snprintf(err, sizeof(err), "Failed to store the uploaded archive (no RAM left in /tmp?)");
                            }
                            unlink(tmp_tar);
                        }
                    } else {
                        snprintf(err, sizeof(err), "Failed to decode base64 archive");
                    }
                    free(data);
                }
            } else {
                snprintf(err, sizeof(err), "Give url or archive_base64");
            }
            cJSON_Delete(json);
        } else {
            /* uploads are base64 in JSON only: the body reader stops at the first NUL byte */
            snprintf(err, sizeof(err), "Invalid JSON body (upload the archive as archive_base64)");
        }

        if (rc == 0) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "message", "Plugin installed successfully");
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        } else {
            send_json_error(client_fd, status, err);
        }
        close(client_fd);
        return 1;
    }

    /* 5b. POST /api/plugins/pull - Pull latest committed files directly from repo */
    if (strcmp(path, "/api/plugins/pull") == 0 && strcmp(method, "POST") == 0) {
        if (refuse_without_auth(client_fd)) return 1;
        char err[192] = "Unknown error";
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
            send_json_error(client_fd, install_status(rc), err);
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

        /* Copy strings to local buffers before freeing JSON (avoid use-after-free) */
        char id[64] = "", act[32] = "start", host[64] = "", pin[32] = "";
        if (j_id && cJSON_IsString(j_id)) strncpy(id, j_id->valuestring, sizeof(id) - 1);
        if (j_act && cJSON_IsString(j_act)) strncpy(act, j_act->valuestring, sizeof(act) - 1);
        if (j_host && cJSON_IsString(j_host)) strncpy(host, j_host->valuestring, sizeof(host) - 1);
        if (j_pin && cJSON_IsString(j_pin)) strncpy(pin, j_pin->valuestring, sizeof(pin) - 1);
        /* optional: without it the plugin uses its own port (the Hub knows no plugin's default) */
        int port = (j_port && cJSON_IsNumber(j_port)) ? j_port->valueint : 0;
        cJSON_Delete(json);

        /* The manifest's pairing block. Two pairings return at once and the UI polls action
         * "status": "listen" (the plugin hears its device, no address to give, no host) and
         * "confirm" (the user accepts a prompt on the device, e.g. a TV; host, no PIN) */
        cJSON *pairing = plugin_mgr_is_safe_id(id) ? plugin_mgr_get_manifest_item(id, "pairing") : NULL;
        const cJSON *j_ptype = pairing ? cJSON_GetObjectItemCaseSensitive(pairing, "type") : NULL;
        int is_listen = cJSON_IsString(j_ptype) && strcmp(j_ptype->valuestring, "listen") == 0;
        int is_confirm = cJSON_IsString(j_ptype) && strcmp(j_ptype->valuestring, "confirm") == 0;
        int is_async = is_listen || is_confirm;
        const cJSON *j_rh = pairing ? cJSON_GetObjectItemCaseSensitive(pairing, "requires_host") : NULL;
        int needs_host = j_rh ? cJSON_IsTrue(j_rh) : !is_listen;
        const cJSON *j_to = pairing ? cJSON_GetObjectItemCaseSensitive(pairing, "timeout_s") : NULL;
        int timeout_s = cJSON_IsNumber(j_to) ? j_to->valueint : 15;
        if (timeout_s < 1) timeout_s = 1;
        if (timeout_s > 60) timeout_s = 60;
        cJSON_Delete(pairing);

        int is_status = strcasecmp(act, "status") == 0;
        if (!plugin_mgr_is_safe_id(id) || (needs_host && !is_status && !host[0]) || port < 0 || port > 65535) {
            send_json_error(client_fd, 400, "Missing or invalid plugin id, host or port");
            close(client_fd);
            return 1;
        }

        char stat_path[128];
        snprintf(stat_path, sizeof(stat_path), HARMONY_PLUGINS_RUN_DIR "/%s.status", id);
        int is_pin = (strcasecmp(act, "pin") == 0 || strcasecmp(act, "finish_pair") == 0);
        cJSON *res_obj = NULL;

        if (is_status) {
            res_obj = read_pair_status(stat_path, NULL, 0);
            if (!res_obj) {
                res_obj = cJSON_CreateObject();
                cJSON_AddBoolToObject(res_obj, "ok", 1);
                cJSON_AddStringToObject(res_obj, "stage", "waiting");
            }
        } else {
            /* Ensure plugin daemon is running */
            if (!plugin_mgr_is_running(id)) {
                plugin_mgr_start(id);
                usleep(200000);
            }

            const char *fwd_action = is_pin ? "finish_pair" : "start_pair";

            /* Clear stale status file before new attempt */
            unlink(stat_path);

            /* Forward pairing command to plugin unix socket */
            int sent = 0;
            char sock_path[128];
            snprintf(sock_path, sizeof(sock_path), HARMONY_PLUGINS_RUN_DIR "/%s.sock", id);

            int s = socket(AF_UNIX, SOCK_DGRAM, 0);
            if (s >= 0) {
                struct sockaddr_un sun;
                memset(&sun, 0, sizeof(sun));
                sun.sun_family = AF_UNIX;
                strncpy(sun.sun_path, sock_path, sizeof(sun.sun_path) - 1);

                cJSON *cmd_obj = cJSON_CreateObject();
                char hub_name[64];
                hw_get_hub_name(hub_name, sizeof(hub_name));
                cJSON_AddStringToObject(cmd_obj, "plugin_id", id);
                cJSON_AddStringToObject(cmd_obj, "hub_name", hub_name);   /* the name a device shows */
                cJSON_AddStringToObject(cmd_obj, "command", "pair");
                cJSON_AddStringToObject(cmd_obj, "action", fwd_action);
                if (host[0]) cJSON_AddStringToObject(cmd_obj, "host", host);
                if (port > 0) cJSON_AddNumberToObject(cmd_obj, "port", port);
                if (pin[0]) cJSON_AddStringToObject(cmd_obj, "pin", pin);
                cJSON_AddNumberToObject(cmd_obj, "timeout_s", timeout_s);

                char *cmd_str = cJSON_PrintUnformatted(cmd_obj);
                if (cmd_str) {
                    /* A plugin with a full queue must not hold this request forever. One just
                     * started binds its socket a little later (the start removed the old one):
                     * up to 3 s for it */
                    for (int tries = 0; tries < 30 && !sent; tries++) {
                        sent = sendto(s, cmd_str, strlen(cmd_str), MSG_DONTWAIT, (struct sockaddr *)&sun, sizeof(sun)) >= 0;
                        if (!sent) usleep(100000);
                    }
                    free(cmd_str);
                }
                cJSON_Delete(cmd_obj);
                close(s);
            }

            /* Wait for the status file: up to timeout_s (default 15 s: the start of a pairing is
             * a connect (5 s timeout), a TLS handshake and three exchanges; 5 s reported a timeout
             * while the pairing went on and succeeded). A listen or confirm pairing waits 1 s at
             * most: it runs for timeout_s and the UI polls. A start or PIN step waits 15 s at
             * most whatever timeout_s says: both UIs give up on the request at 20 s, and the wait
             * holds one of the HTTP workers. */
            int polls = is_async ? 10 : (timeout_s < 15 ? timeout_s : 15) * 10;
            if (!sent) {
                /* not running, no socket or a full queue after 3 s of tries: nothing would answer */
                res_obj = cJSON_CreateObject();
                cJSON_AddBoolToObject(res_obj, "ok", 0);
                cJSON_AddStringToObject(res_obj, "stage", "error");
                cJSON_AddStringToObject(res_obj, "message", plugin_mgr_is_running(id)
                                        ? "The plugin does not take commands (no socket)"
                                        : "The plugin is not running");
                polls = 0;
            }
            for (int i = 0; i < polls && !res_obj; i++) {
                usleep(100000);
                res_obj = read_pair_status(stat_path, act, is_pin);
            }

            if (!res_obj && is_async) {
                res_obj = cJSON_CreateObject();
                cJSON_AddBoolToObject(res_obj, "ok", 1);
                cJSON_AddStringToObject(res_obj, "stage", is_listen ? "listening" : "confirm_on_device");
            } else if (!res_obj) {
                res_obj = cJSON_CreateObject();
                cJSON_AddBoolToObject(res_obj, "ok", 0);
                cJSON_AddStringToObject(res_obj, "stage", "error");
                cJSON_AddStringToObject(res_obj, "message", "Device did not respond or pairing timed out");
            }
            if (is_async && !cJSON_GetObjectItemCaseSensitive(res_obj, "timeout_s")) {
                cJSON_AddNumberToObject(res_obj, "timeout_s", timeout_s);
            }
        }

        cJSON *dev_def = plugin_mgr_get_device_definition(id);
        plugin_pair_filter_config(res_obj, dev_def);
        cJSON_Delete(dev_def);

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
        /* copies: both are used for the reply, after the JSON is deleted */
        char id[64] = "", act[16] = "";
        if (j_id && cJSON_IsString(j_id)) snprintf(id, sizeof(id), "%s", j_id->valuestring);
        if (j_act && cJSON_IsString(j_act)) snprintf(act, sizeof(act), "%s", j_act->valuestring);
        cJSON_Delete(json);

        int rc = -1;
        if (strcmp(act, "start") == 0) rc = plugin_mgr_start(id);
        else if (strcmp(act, "stop") == 0) rc = plugin_mgr_stop(id);
        else if (strcmp(act, "restart") == 0) rc = plugin_mgr_restart(id);
        else if (strcmp(act, "enable") == 0) rc = plugin_mgr_set_enabled(id, 1);
        else if (strcmp(act, "disable") == 0) rc = plugin_mgr_set_enabled(id, 0);

        if (rc == 0) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "id", id);
            cJSON_AddStringToObject(resp, "action", act);
            cJSON_AddBoolToObject(resp, "running", plugin_mgr_is_running(id));
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        } else {
            if (rc == PLUGIN_ERR_NOT_FOUND) send_json_error(client_fd, 404, "Plugin not installed");
            else if (rc == PLUGIN_ERR_BUSY) send_json_error(client_fd, 409, "The plugin is being started, stopped or replaced; try again");
            else send_json_error(client_fd, 400, "Action failed or invalid plugin ID");
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

        int rc = target_id[0] ? plugin_mgr_uninstall(target_id) : -1;
        if (rc == 0) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "id", target_id);
            cJSON_AddStringToObject(resp, "message", "Plugin uninstalled");
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        } else if (rc == PLUGIN_ERR_NOT_FOUND) {
            send_json_error(client_fd, 404, "Plugin not installed");
        } else if (rc == PLUGIN_ERR_BUSY) {
            send_json_error(client_fd, 409, "A plugin install is running");
        } else {
            send_json_error(client_fd, 400, "Failed to uninstall plugin");
        }
        close(client_fd);
        return 1;
    }

    return 0;
}
