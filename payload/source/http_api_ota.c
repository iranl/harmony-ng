#include "http_api_ota.h"
#include "http_util.h"
#include "config_store.h"
#include "harmony_version.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int http_api_ota_handle(int client_fd, const char *method, const char *clean_path,
                        const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    /* Route: GET /api/update-status */
    if (strcmp(clean_path, "/api/update-status") == 0 && strcmp(method, "GET") == 0) {
        char fw_str[128] = "4.15.600";
        FILE *vf = fopen("/etc/version", "r");
        if (vf) {
            if (fgets(fw_str, sizeof(fw_str), vf)) {
                char *nl = strchr(fw_str, '\n'); if (nl) *nl = '\0';
                nl = strchr(fw_str, '\r'); if (nl) *nl = '\0';
            }
            fclose(vf);
        }
        char remote_ver[64] = "v" HARMONY_VERSION;
        int has_update = (access("/data/harmony/update_available.flag", F_OK) == 0);
        FILE *rf = fopen(config_store_path("latest_version"), "r");
        if (rf) {
            char lbuf[64] = {0};
            if (fgets(lbuf, sizeof(lbuf), rf)) {
                char *nl = strchr(lbuf, '\n'); if (nl) *nl = '\0';
                nl = strchr(lbuf, '\r'); if (nl) *nl = '\0';
                if (strlen(lbuf) > 0) {
                    snprintf(remote_ver, sizeof(remote_ver), "%s", lbuf);
                    if (strcmp(lbuf, HARMONY_VERSION) != 0 && strcmp(lbuf, "v" HARMONY_VERSION) != 0) {
                        has_update = 1;
                    }
                }
            }
            fclose(rf);
        }

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddStringToObject(st, "current_version", HARMONY_VERSION);
        cJSON_AddStringToObject(st, "version", HARMONY_VERSION);
        cJSON_AddStringToObject(st, "local_version", "v" HARMONY_VERSION " (" HARMONY_COMMIT ")");
        cJSON_AddStringToObject(st, "remote_version", remote_ver);
        cJSON_AddStringToObject(st, "commit", HARMONY_COMMIT);
        cJSON_AddStringToObject(st, "build_time", __DATE__ " " __TIME__);
        cJSON_AddStringToObject(st, "fw_version", fw_str);
        cJSON_AddStringToObject(st, "firmware", fw_str);
        cJSON_AddStringToObject(st, "rf_version", "0x20 CC2544");
        cJSON_AddStringToObject(st, "bt_version", "BTstack v1.5 (UART)");
        cJSON_AddStringToObject(st, "arch", "mips (AR9344)");
        cJSON_AddBoolToObject(st, "update_available", has_update);
        cJSON_AddBoolToObject(st, "updating", access("/tmp/hub_updating.flag", F_OK) == 0);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: GET/POST /api/update-check-state or /api/update-check */
    if ((strcmp(clean_path, "/api/update-check-state") == 0 || strcmp(clean_path, "/api/update-check") == 0) &&
        (strcmp(method, "POST") == 0 || strcmp(method, "GET") == 0)) {
        cJSON *p = parse_request_params(body);
        char repo[256] = {0};
        char token[128] = {0};
        char client_remote_ver[64] = {0};
        if (p) {
            cJSON *j_repo = cJSON_GetObjectItemCaseSensitive(p, "repo");
            if (j_repo && cJSON_IsString(j_repo) && j_repo->valuestring && j_repo->valuestring[0]) {
                strncpy(repo, j_repo->valuestring, sizeof(repo) - 1);
            }
            cJSON *j_tok = cJSON_GetObjectItemCaseSensitive(p, "token");
            if (j_tok && cJSON_IsString(j_tok) && j_tok->valuestring && j_tok->valuestring[0]) {
                strncpy(token, j_tok->valuestring, sizeof(token) - 1);
            }
            cJSON *j_ver = cJSON_GetObjectItemCaseSensitive(p, "remote_version");
            if (!j_ver) j_ver = cJSON_GetObjectItemCaseSensitive(p, "version");
            if (j_ver && cJSON_IsString(j_ver) && j_ver->valuestring && j_ver->valuestring[0]) {
                strncpy(client_remote_ver, j_ver->valuestring, sizeof(client_remote_ver) - 1);
            }
            cJSON_Delete(p);
        }

        if (repo[0] && !is_safe_param(repo, "/.:-_@~")) repo[0] = '\0';
        if (token[0] && !is_safe_param(token, "-_")) token[0] = '\0';

        if (client_remote_ver[0]) {
            char clean_ver[64] = {0};
            const char *src = client_remote_ver;
            if (src[0] == 'v' || src[0] == 'V') src++;
            strncpy(clean_ver, src, sizeof(clean_ver) - 1);
            FILE *rf = fopen(config_store_path("latest_version"), "w");
            if (rf) {
                fprintf(rf, "%s\n", clean_ver);
                fclose(rf);
            }
            if (strcmp(clean_ver, HARMONY_VERSION) != 0) {
                FILE *ff = fopen("/data/harmony/update_available.flag", "w");
                if (ff) fclose(ff);
            } else {
                unlink("/data/harmony/update_available.flag");
            }
        } else {
            char cmd[512] = {0};
            if (access("/data/harmony/update.sh", X_OK) == 0) {
                snprintf(cmd, sizeof(cmd), "/data/harmony/update.sh check '%s' '%s' >/tmp/update_check.log 2>&1", repo, token);
            } else {
                snprintf(cmd, sizeof(cmd),
                    "/cache/bin/busybox wget --no-check-certificate -q -T 6 -O /data/harmony/latest_version 'https://raw.githubusercontent.com/iranl/harmony-ng/ota/v2/VERSION' 2>/dev/null");
            }
            system(cmd);
        }

        char remote_ver[64] = HARMONY_VERSION;
        int has_update = 0;
        FILE *rf = fopen(config_store_path("latest_version"), "r");
        if (rf) {
            char lbuf[64] = {0};
            if (fgets(lbuf, sizeof(lbuf), rf)) {
                char *nl = strchr(lbuf, '\n'); if (nl) *nl = '\0';
                nl = strchr(lbuf, '\r'); if (nl) *nl = '\0';
                if (strlen(lbuf) > 0) {
                    const char *clean = (lbuf[0] == 'v' || lbuf[0] == 'V') ? lbuf + 1 : lbuf;
                    snprintf(remote_ver, sizeof(remote_ver), "%s", clean);
                    if (strcmp(clean, HARMONY_VERSION) != 0) {
                        has_update = 1;
                        FILE *ff = fopen("/data/harmony/update_available.flag", "w");
                        if (ff) fclose(ff);
                    } else {
                        unlink("/data/harmony/update_available.flag");
                    }
                }
            }
            fclose(rf);
        }

        char msg[256];
        if (has_update) {
            snprintf(msg, sizeof(msg), "Update available: v%s (Current: v%s)", remote_ver, HARMONY_VERSION);
        } else {
            snprintf(msg, sizeof(msg), "Hub binaries and WebUI are up to date (v%s).", HARMONY_VERSION);
        }

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddStringToObject(st, "current_version", HARMONY_VERSION);
        cJSON_AddStringToObject(st, "remote_version", remote_ver);
        cJSON_AddStringToObject(st, "message", msg);
        cJSON_AddBoolToObject(st, "has_update", has_update);
        cJSON_AddBoolToObject(st, "update_available", has_update);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/update-apply */
    if (strcmp(clean_path, "/api/update-apply") == 0 && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        char repo[256] = {0};
        char token[128] = {0};
        if (p) {
            cJSON *j_repo = cJSON_GetObjectItemCaseSensitive(p, "repo");
            if (j_repo && cJSON_IsString(j_repo) && j_repo->valuestring && j_repo->valuestring[0]) {
                strncpy(repo, j_repo->valuestring, sizeof(repo) - 1);
            }
            cJSON *j_tok = cJSON_GetObjectItemCaseSensitive(p, "token");
            if (j_tok && cJSON_IsString(j_tok) && j_tok->valuestring && j_tok->valuestring[0]) {
                strncpy(token, j_tok->valuestring, sizeof(token) - 1);
            }
            cJSON_Delete(p);
        }

        if (repo[0] && !is_safe_param(repo, "/.:-_@~")) repo[0] = '\0';
        if (token[0] && !is_safe_param(token, "-_")) token[0] = '\0';

        FILE *uf = fopen("/tmp/hub_updating.flag", "w");
        if (uf) fclose(uf);

        char cmd[512] = {0};
        snprintf(cmd, sizeof(cmd),
            "/bin/sh -c 'sleep 1; if [ -x /data/harmony/update.sh ]; then /data/harmony/update.sh apply \"%s\" \"%s\"; else /data/harmony/scripts/init.sh restart 2>/dev/null; fi' >/tmp/update_apply.log 2>&1 &",
            repo, token);
        system(cmd);

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddStringToObject(st, "status", "applying");
        cJSON_AddStringToObject(st, "message", "Software update triggered. Hub services downloading and applying in background.");
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    return 0;
}
