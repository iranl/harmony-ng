#include "http_api_ota.h"
#include "http_util.h"
#include "config_store.h"
#include "harmony_version.h"
#include "orchestrator.h"
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
 * points it, the state file, the apply log and the uptime file elsewhere (HARMONY_TEST_*), and
 * without them at a path that does not exist: a host test never reads or writes the host's /tmp. */
#ifdef TEST_HARNESS
#define OTA_PATH(env, path) do { const char *p = getenv(env); return p && p[0] ? p : "/nonexistent" path; } while (0)
#else
#define OTA_PATH(env, path) return path
#endif
static const char *updating_flag_path(void) { OTA_PATH("HARMONY_TEST_UPDATING_FLAG", "/tmp/hub_updating.flag"); }

/* /tmp/hub_update.state (update.sh): state=running|restarting|failed, version=<new> when
 * restarting, since=<seconds since boot> when written. /tmp/update_apply.log: the output of the
 * apply. */
static const char *update_state_path(void) { OTA_PATH("HARMONY_TEST_UPDATE_STATE", "/tmp/hub_update.state"); }
static const char *update_log_path(void) { OTA_PATH("HARMONY_TEST_UPDATE_LOG", "/tmp/update_apply.log"); }
static const char *uptime_path(void) { OTA_PATH("HARMONY_TEST_UPTIME_FILE", "/proc/uptime"); }

/* Seconds since boot (SNTP never steps this clock), -1 when unknown */
static long uptime_secs(void) {
    double up = -1;
    FILE *f = fopen(uptime_path(), "r");
    if (f) {
        if (fscanf(f, "%lf", &up) != 1) up = -1;
        fclose(f);
    }
    return (long)up;
}

static void read_update_state(char *state, size_t state_sz, char *ver, size_t ver_sz, long *since) {
    char line[128];
    state[0] = ver[0] = '\0';
    *since = -1;
    FILE *f = fopen(update_state_path(), "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "state=", 6) == 0) snprintf(state, state_sz, "%s", line + 6);
        else if (strncmp(line, "version=", 8) == 0) snprintf(ver, ver_sz, "%s", line + 8);
        else if (strncmp(line, "since=", 6) == 0) *since = strtol(line + 6, NULL, 10);
    }
    fclose(f);
}

/* Last non-empty line of the apply log, and the first error line ("[-] ...": the cause; update.sh
 * adds a last one when it ends with an error) */
static void read_update_log(char *last, size_t last_sz, char *err, size_t err_sz) {
    char line[256];
    last[0] = err[0] = '\0';
    FILE *f = fopen(update_log_path(), "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        snprintf(last, last_sz, "%s", line);
        if (!err[0] && strncmp(line, "[-]", 3) == 0) snprintf(err, err_sz, "%s", line);
    }
    fclose(f);
}

/* An update that runs longer than 30 minutes is left from a run that never ended (update.sh
 * killed with its parent). Counted on the uptime clock from since= of the state file: a flag
 * written before SNTP set the clock looked "old" at the clock step. Without since= (an update.sh
 * older than the state file) from the flag's date, where a date in the future (the clock was set
 * back after it was written) does not count either. */
static int update_run_active(const char *state, long since) {
    struct stat fst;
    if (stat(updating_flag_path(), &fst) != 0) return 0;
    long up = uptime_secs();
    if (strcmp(state, "running") == 0 && since >= 0 && up >= 0) return up >= since && up - since < 1800;
    time_t now = time(NULL);
    return now >= fst.st_mtime && now - fst.st_mtime < 1800;
}

/* idle, running, restarting (installed, the services restart: update.sh removes the flag at its
 * exit, before the old daemon stops) or failed (the last apply ended with an error; wins over a
 * flag a killed update.sh left behind). target: the version installed, set only when restarting.
 * A restart takes seconds and the new daemon ends the state at its start: "restarting" older
 * than 10 minutes of uptime is over (it would block updates and reboots until a reboot). */
static const char *update_state_now(char *target, size_t target_sz) {
    char state[16];
    long since;
    read_update_state(state, sizeof(state), target, target_sz, &since);
    long up = since >= 0 ? uptime_secs() : -1;
    int restart_over = up >= since && up - since > 600;
    if (strcmp(state, "restarting") == 0 && !restart_over &&
        (!target[0] || harmony_version_compare(target, HARMONY_VERSION) > 0))
        return "restarting";
    target[0] = '\0';
    if (strcmp(state, "failed") == 0) return "failed";
    return update_run_active(state, since) ? "running" : "idle";
}

int ota_update_busy(void) {
    char target[64];
    const char *s = update_state_now(target, sizeof(target));
    return strcmp(s, "running") == 0 || strcmp(s, "restarting") == 0;
}

/* The daemon that starts is the restart "restarting" waited for (or a deploy, or a restart that
 * did not load the new files): from now on the version it reports tells the result */
void ota_update_state_init(void) {
    char state[16], target[64];
    long since;
    read_update_state(state, sizeof(state), target, sizeof(target), &since);
    if (strcmp(state, "restarting") == 0) unlink(update_state_path());
}

/* state=running for an apply this daemon starts, on the uptime clock */
static void write_running_state(void) {
    FILE *sf = fopen(update_state_path(), "w");
    if (!sf) return;
    long up = uptime_secs();
    if (up >= 0) fprintf(sf, "state=running\nsince=%ld\n", up);
    else fputs("state=running\n", sf);
    fclose(sf);
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

    char target[64], last[256], err[256];
    const char *ust = update_state_now(target, sizeof(target));
    cJSON_AddBoolToObject(st, "updating", strcmp(ust, "running") == 0 || strcmp(ust, "restarting") == 0);
    cJSON_AddStringToObject(st, "update_state", ust);
    if (target[0]) cJSON_AddStringToObject(st, "update_target", target);
    if (strcmp(ust, "running") == 0 || strcmp(ust, "failed") == 0) {
        read_update_log(last, sizeof(last), err, sizeof(err));
        if (strcmp(ust, "running") == 0 && last[0]) cJSON_AddStringToObject(st, "update_step", last);
        if (strcmp(ust, "failed") == 0) cJSON_AddStringToObject(st, "update_error", err[0] ? err : last);
    }
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
                "LD_LIBRARY_PATH=/data/harmony/lib /data/harmony/bin/hub_fetch -T 6 -o /data/harmony/latest_version 'https://raw.githubusercontent.com/iranl/harmony-ng/ota/v2/VERSION' 2>/dev/null");
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
     * started a second update.sh in the same work directory, and one while the services restart
     * after an update overwrote its state. A run that never ended does not block
     * (update_run_active). */
    if (ota_update_busy()) {
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
#ifdef TEST_HARNESS
    if (getenv("HARMONY_TEST_UPDATE_STATE"))
#endif
    write_running_state(); /* replaces the state of an earlier run; an older update.sh writes none */

    /* The fallback (no update.sh) installs nothing and only restarts the services: it removes the
     * flag first ("init.sh stop" kills this shell, whose command line names init.sh), and the
     * services get /dev/null, not the apply log, as their output */
    char cmd[640] = {0};
    snprintf(cmd, sizeof(cmd),
        "/bin/sh -c 'sleep 1; if [ -x /data/harmony/update.sh ]; then /data/harmony/update.sh apply \"%s\" \"%s\"; else rm -f /tmp/hub_updating.flag /tmp/hub_update.state; /data/harmony/scripts/init.sh restart >/dev/null 2>&1 </dev/null; fi' >/tmp/update_apply.log 2>&1 &",
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

/* update.sh restarts the Hub services at the end of an update (and so does the fallback): one
 * that came while an activity switch ran cut it in the middle and left the devices half switched.
 * A request is refused while a switch runs, one is queued, or the steps after the switch point are
 * still being sent; update.sh itself also waits for the switch before its restart. */
cJSON *ota_update_apply_blocked(void) {
    char queued[32] = "";
    int has_queued = orch_get_queued_activity(queued, sizeof(queued));
    int busy = orch_is_busy();
    int sending = orch_is_sending();
    if (!busy && !sending && !has_queued) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "status", "activity_switch");
    cJSON_AddStringToObject(o, "error", "An activity switch is running: start the update again when it has finished");
    cJSON_AddBoolToObject(o, "busy", busy);
    cJSON_AddBoolToObject(o, "sending", sending);
    if (has_queued) cJSON_AddStringToObject(o, "queued", queued);
    else cJSON_AddNullToObject(o, "queued");
    return o;
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
        cJSON *blocked = ota_update_apply_blocked();
        if (blocked) {
            send_cjson_resp(client_fd, 409, blocked);
            cJSON_Delete(blocked);
            close(client_fd);
            return 1;
        }

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
