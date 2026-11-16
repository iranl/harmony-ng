#include "http_api_ota.h"
#include "http_util.h"
#include "config_store.h"
#include "harmony_version.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

/* The flags and latest_version live on flash: written only when they change (an update check
 * with remote_version could be repeated without limit, each one rewriting them) */
static void set_flag_file(const char *path, int on) {
    if (on) {
        if (access(path, F_OK) == 0) return;
        FILE *f = fopen(path, "w");
        if (f) fclose(f);
    } else if (access(path, F_OK) == 0) {
        unlink(path);
    }
}

static void write_latest_version(const char *ver) {
    const char *path = config_store_path("latest_version");
    char cur[64] = {0};
    FILE *rf = fopen(path, "r");
    if (rf) {
        if (fgets(cur, sizeof(cur), rf)) cur[strcspn(cur, "\r\n")] = '\0';
        fclose(rf);
        if (strcmp(cur, ver) == 0) return;
    }
    FILE *wf = fopen(path, "w");
    if (wf) {
        fprintf(wf, "%s\n", ver);
        fclose(wf);
    }
}

/* /tmp/hub_updating.flag: set while update.sh apply runs (its exit removes it). The test build
 * can point it elsewhere (HARMONY_TEST_UPDATING_FLAG) so a host test never touches /tmp. */
static const char *updating_flag_path(void) {
#ifdef TEST_HARNESS
    const char *p = getenv("HARMONY_TEST_UPDATING_FLAG");
    if (p && p[0]) return p;
#endif
    return "/tmp/hub_updating.flag";
}

/* GET /api/update-status object */
cJSON *ota_update_status(void) {
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
                if (harmony_version_compare(lbuf, HARMONY_VERSION) > 0) {
                    snprintf(remote_ver, sizeof(remote_ver), "%s", lbuf);
                    has_update = 1;
                } else {
                    has_update = 0;
                    unlink("/data/harmony/update_available.flag");
                }
            }
        }
        fclose(rf);
    }
    if (has_update && harmony_version_compare(remote_ver, HARMONY_VERSION) <= 0) {
        has_update = 0;
        unlink("/data/harmony/update_available.flag");
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
    return st;
}

/* POST /api/update-check: runs update.sh check (or fetches the published VERSION), which can take
 * several seconds: callers on the main loop run it on a thread (hub_cmd.c). params: repo, token,
 * remote_version (all optional), or NULL. */
cJSON *ota_update_check(const cJSON *params) {
    const cJSON *p = params;
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
    }

    if (repo[0] && !is_safe_param(repo, "/.:-_@~")) repo[0] = '\0';
    if (token[0] && !is_safe_param(token, "-_")) token[0] = '\0';

    if (client_remote_ver[0]) {
        char clean_ver[64] = {0};
        const char *src = client_remote_ver;
        if (src[0] == 'v' || src[0] == 'V') src++;
        strncpy(clean_ver, src, sizeof(clean_ver) - 1);
        const char *save_ver = (harmony_version_compare(clean_ver, HARMONY_VERSION) > 0) ? clean_ver : HARMONY_VERSION;
        write_latest_version(save_ver);
        set_flag_file("/data/harmony/update_available.flag", harmony_version_compare(clean_ver, HARMONY_VERSION) > 0);
    } else {
        char cmd[512] = {0};
        if (access("/data/harmony/update.sh", X_OK) == 0) {
            snprintf(cmd, sizeof(cmd), "/data/harmony/update.sh check '%s' '%s' >/tmp/update_check.log 2>&1", repo, token);
        } else {
            snprintf(cmd, sizeof(cmd),
                "/cache/bin/busybox wget --no-check-certificate -q -T 6 -O /data/harmony/latest_version 'https://raw.githubusercontent.com/iranl/harmony-ng/ota/v2/VERSION' 2>/dev/null");
        }
#ifndef TEST_HARNESS
        system(cmd);
#else
        /* test build: no network; HARMONY_TEST_UPDATE_CHECK_MS simulates a slow check */
        const char *delay = getenv("HARMONY_TEST_UPDATE_CHECK_MS");
        if (delay && atoi(delay) > 0) usleep((useconds_t)atoi(delay) * 1000);
#endif
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
                has_update = harmony_version_compare(clean, HARMONY_VERSION) > 0;
                set_flag_file("/data/harmony/update_available.flag", has_update);
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
    return st;
}

/* POST /api/update-apply: starts update.sh apply in the background and returns at once */
cJSON *ota_update_apply(const cJSON *params) {
    const cJSON *p = params;
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
    }

    if (repo[0] && !is_safe_param(repo, "/.:-_@~")) repo[0] = '\0';
    if (token[0] && !is_safe_param(token, "-_")) token[0] = '\0';

    /* One update at a time: a second apply while one runs (a double click, two browser tabs)
     * started a second update.sh in the same work directory. A flag older than 30 minutes is
     * left from a run that never ended and does not block, nor does one dated in the future
     * (the clock was set back after it was written): it would have blocked until then. */
    struct stat fst;
    time_t now = time(NULL);
    if (stat(updating_flag_path(), &fst) == 0 && now >= fst.st_mtime && now - fst.st_mtime < 1800) {
        cJSON *busy = cJSON_CreateObject();
        cJSON_AddBoolToObject(busy, "ok", 0);
        cJSON_AddStringToObject(busy, "status", "busy");
        cJSON_AddStringToObject(busy, "error", "A software update is already running");
        return busy;
    }
#ifdef TEST_HARNESS
    if (getenv("HARMONY_TEST_UPDATING_FLAG"))
#endif
    {
        FILE *uf = fopen(updating_flag_path(), "w");
        if (uf) fclose(uf);
    }

    char cmd[512] = {0};
    snprintf(cmd, sizeof(cmd),
        "/bin/sh -c 'sleep 1; if [ -x /data/harmony/update.sh ]; then /data/harmony/update.sh apply \"%s\" \"%s\"; else /data/harmony/scripts/init.sh restart 2>/dev/null; fi' >/tmp/update_apply.log 2>&1 &",
        repo, token);
#ifndef TEST_HARNESS
    system(cmd);
#else
    printf("[*] update-apply (test build, not run): %s\n", cmd); /* never run the updater on a test host */
#endif

    cJSON *st = cJSON_CreateObject();
    cJSON_AddBoolToObject(st, "ok", 1);
    cJSON_AddStringToObject(st, "status", "applying");
    cJSON_AddStringToObject(st, "message", "Software update triggered. Hub services downloading and applying in background.");
    return st;
}

int http_api_ota_handle(int client_fd, const char *method, const char *clean_path,
                        const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    /* Route: GET /api/update-status */
    if (strcmp(clean_path, "/api/update-status") == 0 && strcmp(method, "GET") == 0) {
        cJSON *st = ota_update_status();
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: GET/POST /api/update-check-state or /api/update-check */
    if ((strcmp(clean_path, "/api/update-check-state") == 0 || strcmp(clean_path, "/api/update-check") == 0) &&
        (strcmp(method, "POST") == 0 || strcmp(method, "GET") == 0)) {
        cJSON *p = parse_request_params(body);
        cJSON *st = ota_update_check(p);
        cJSON_Delete(p);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/update-apply */
    if (strcmp(clean_path, "/api/update-apply") == 0 && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        cJSON *st = ota_update_apply(p);
        cJSON_Delete(p);
        send_cjson_resp(client_fd, cJSON_IsTrue(cJSON_GetObjectItem(st, "ok")) ? 200 : 409, st);
        cJSON_Delete(st);
        close(client_fd);
        return 1;
    }

    return 0;
}
