/* Shared command executor, see hub_cmd.h. Runs on the main loop (WebSocket) or on the MQTT
 * tick (also the main loop); the hub_rf calls go over its loopback HTTP API. */
#define _GNU_SOURCE
#include "hub_cmd.h"
#include "hub_ws.h"
#include "orchestrator.h"
#include "hw_action.h"
#include "http_auth.h"
#include "config_store.h"
#include "hub_events.h"
#include "resource_cache.h"
#include "http_api_ota.h"
#include "http_api_activities.h"
#include "hub_mqtt.h"
#include "ws_server.h"
#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define RF_API_PORT 8092

/* ------------------------------------------------------------------------------------------ */

static const char *json_str(const cJSON *o, const char *k1, const char *k2, const char *k3) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k1);
    if (!j && k2) j = cJSON_GetObjectItemCaseSensitive(o, k2);
    if (!j && k3) j = cJSON_GetObjectItemCaseSensitive(o, k3);
    return (j && cJSON_IsString(j) && j->valuestring) ? j->valuestring : NULL;
}

/* True when id names a configured activity, or is PowerOff's "-1" */
static int activity_id_known(const char *id) {
    if (!id || !id[0]) return 0;
    if (strcmp(id, "-1") == 0) return 1;
    cJSON *root = config_store_get_activities_json();
    const cJSON *arr = root ? cJSON_GetObjectItemCaseSensitive(root, "activities") : NULL;
    const cJSON *a = NULL;
    cJSON_ArrayForEach(a, arr) {
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(a, "id");
        char buf[32];
        const char *aid = NULL;
        if (jid && cJSON_IsString(jid)) aid = jid->valuestring;
        else if (jid && cJSON_IsNumber(jid)) {
            snprintf(buf, sizeof(buf), "%ld", (long)jid->valuedouble);
            aid = buf;
        }
        if (aid && strcmp(aid, id) == 0) return 1;
    }
    return 0;
}

static cJSON *reply_base(const cJSON *req, const char *type, const char *action) {
    cJSON *r = cJSON_CreateObject();
    if (!r) return NULL;
    cJSON_AddStringToObject(r, "type", type);
    if (action) cJSON_AddStringToObject(r, "action", action);
    const cJSON *mid = cJSON_GetObjectItemCaseSensitive(req, "msgId");
    if (mid && cJSON_IsNumber(mid) && mid->valueint) cJSON_AddNumberToObject(r, "msgId", mid->valueint);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    if (id && cJSON_IsString(id) && id->valuestring[0]) {
        char idbuf[65];
        snprintf(idbuf, sizeof(idbuf), "%s", id->valuestring);
        cJSON_AddStringToObject(r, "id", idbuf);
    }
    return r;
}

static cJSON *ack(const cJSON *req, const char *action, int ok, int code, const char *err) {
    cJSON *r = reply_base(req, "ack", action);
    if (!r) return NULL;
    cJSON_AddBoolToObject(r, "ok", ok ? 1 : 0);
    if (!ok) {
        cJSON_AddNumberToObject(r, "code", code);
        cJSON_AddStringToObject(r, "error", err ? err : "error");
    }
    return r;
}

/* Request to hub_rf on the loopback; returns the HTTP status or -1 (hub_rf not running) */
static int rf_request(const char *method, const char *path, const char *body) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 600000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(RF_API_PORT);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(s);
        return -1;
    }
    char req[640];
    int blen = body ? (int)strlen(body) : 0;
    int n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", method, path, RF_API_PORT, blen);
    if (n <= 0 || send(s, req, (size_t)n, MSG_NOSIGNAL) != n || (blen && send(s, body, (size_t)blen, MSG_NOSIGNAL) != blen)) {
        close(s);
        return -1;
    }
    char resp[256] = {0};
    ssize_t r = recv(s, resp, sizeof(resp) - 1, 0);
    close(s);
    int status = -1;
    if (r > 0 && sscanf(resp, "HTTP/%*s %d", &status) == 1) return status;
    return -1;
}

static int pipe_write(const char *target, const char *line) {
    int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
    if (bfd < 0) return 0;
    if (target && target[0]) {
        char thdr[64];
        snprintf(thdr, sizeof(thdr), "TARGET %s\n", target);
        if (write(bfd, thdr, strlen(thdr)) < 0) { /* ignore */ }
    }
    int ok = write(bfd, line, strlen(line)) > 0;
    close(bfd);
    return ok;
}

/* ------------------------------------------------------------------------------------------ */
/* ir_raw                                                                                      */

int hub_cmd_ir_raw(const cJSON *req, cJSON **reply) {
    *reply = NULL;
    if (!req || !cJSON_IsObject(req)) {
        *reply = cJSON_CreateObject();
        cJSON_AddBoolToObject(*reply, "ok", 0);
        cJSON_AddStringToObject(*reply, "error", "body: JSON object required");
        return 400;
    }
    const char *err = NULL;
    uint32_t freq = 38000;
    int port = 7, repeats = 1;
    const cJSON *jf = cJSON_GetObjectItemCaseSensitive(req, "frequency");
    if (jf) {
        if (!cJSON_IsNumber(jf) || jf->valuedouble < 0 || jf->valuedouble != (double)(long)jf->valuedouble) err = "frequency: integer Hz required";
        else if (jf->valuedouble != 0 && (jf->valuedouble < 20000 || jf->valuedouble > 60000)) err = "frequency: 0 or 20000 to 60000 Hz";
        else freq = (uint32_t)jf->valuedouble;
    }
    const cJSON *jp = cJSON_GetObjectItemCaseSensitive(req, "port");
    if (!err && jp) {
        if (!cJSON_IsNumber(jp) || jp->valueint < 1 || jp->valueint > 7) err = "port: bitmask 1 to 7";
        else port = jp->valueint;
    }
    const cJSON *jr = cJSON_GetObjectItemCaseSensitive(req, "repeats");
    if (!err && jr) {
        if (!cJSON_IsNumber(jr) || jr->valueint < 1 || jr->valueint > 10) err = "repeats: 1 to 10";
        else repeats = jr->valueint;
    }
    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(req, "timings");
    size_t count = 0;
    uint32_t *timings = NULL;
    if (!err) {
        if (!jt || !cJSON_IsArray(jt)) err = "timings: array required";
        else {
            int n = cJSON_GetArraySize(jt);
            if (n < 2 || n > 2048) err = "timings: 2 to 2048 values";
            else {
                timings = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
                if (!timings) err = "timings: out of memory";
                int signed_form = 0, i = 0;
                const cJSON *it = NULL;
                cJSON_ArrayForEach(it, jt) {
                    if (err) break;
                    if (!cJSON_IsNumber(it) || it->valuedouble != (double)(long)it->valuedouble) { err = "timings: integers required"; break; }
                    long v = (long)it->valuedouble;
                    if (i == 0) {
                        if (v <= 0) { err = "timings: first value must be a positive mark"; break; }
                    } else if (i == 1) {
                        signed_form = (v < 0);
                    }
                    if (signed_form) {
                        if ((i % 2 == 0 && v <= 0) || (i % 2 == 1 && v >= 0)) { err = "timings: signed form must alternate mark (+) and space (-)"; break; }
                    } else if (v <= 0) {
                        err = "timings: values must be positive (or use the signed form)";
                        break;
                    }
                    long a = v < 0 ? -v : v;
                    if (a > 65535) { err = "timings: each value 1 to 65535 us"; break; }
                    timings[i++] = (uint32_t)a;
                }
                count = (size_t)n;
            }
        }
    }
    if (err) {
        free(timings);
        *reply = cJSON_CreateObject();
        cJSON_AddBoolToObject(*reply, "ok", 0);
        cJSON_AddStringToObject(*reply, "error", err);
        return 400;
    }
    uint64_t sum_us = 0;
    for (size_t i = 0; i < count; i++) sum_us += timings[i];
    int rc = hw_ir_send_raw(freq, timings, count, (uint8_t)port, (uint8_t)repeats);
    free(timings);
    *reply = cJSON_CreateObject();
    if (rc == -EBUSY) {
        cJSON_AddBoolToObject(*reply, "ok", 0);
        cJSON_AddStringToObject(*reply, "error", "busy");
        return 409;
    }
    if (rc != 0) {
        cJSON_AddBoolToObject(*reply, "ok", 0);
        cJSON_AddStringToObject(*reply, "error", "IR transmit failed");
        return 500;
    }
    cJSON_AddBoolToObject(*reply, "ok", 1);
    cJSON_AddNumberToObject(*reply, "count", (double)count);
    cJSON_AddNumberToObject(*reply, "duration_ms", (double)((sum_us * (uint64_t)repeats + 999) / 1000));
    return 200;
}

/* ------------------------------------------------------------------------------------------ */

static int btn_action_from(const char *s) {
    if (!s) return HW_BTN_ACTION_TAP;
    if (strcasecmp(s, "press") == 0 || strcasecmp(s, "down") == 0) return HW_BTN_ACTION_PRESS;
    if (strcasecmp(s, "hold") == 0 || strcasecmp(s, "repeat") == 0) return HW_BTN_ACTION_REPEAT;
    if (strcasecmp(s, "release") == 0 || strcasecmp(s, "up") == 0) return HW_BTN_ACTION_RELEASE;
    return HW_BTN_ACTION_TAP;
}

/* An ack carrying the fields of a REST reply (minus its own ok/success flag) */
static cJSON *ack_with(const cJSON *req, const char *action, int code, const cJSON *rest) {
    int ok = (code >= 200 && code < 300);
    const char *err = NULL;
    if (!ok && rest) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(rest, "error");
        if (e && cJSON_IsString(e)) err = e->valuestring;
    }
    cJSON *r = ack(req, action, ok, code, err);
    if (!r || !ok || !rest) return r;
    const cJSON *it;
    cJSON_ArrayForEach(it, rest) {
        if (!it->string || strcmp(it->string, "ok") == 0 || strcmp(it->string, "success") == 0) continue;
        if (cJSON_GetObjectItemCaseSensitive(r, it->string)) continue;
        cJSON_AddItemToObject(r, it->string, cJSON_Duplicate(it, 1));
    }
    return r;
}

/* update_check: the check, then the GET /api/update-status fields (spec 5.3) */
static cJSON *update_check_reply(const cJSON *req) {
    cJSON *chk = ota_update_check(req);
    cJSON *st = ota_update_status();
    if (st && chk) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(chk, "message");
        if (msg && cJSON_IsString(msg)) cJSON_AddStringToObject(st, "message", msg->valuestring);
    }
    cJSON *r = ack_with(req, "update_check", st ? 200 : 500, st);
    cJSON_Delete(chk);
    cJSON_Delete(st);
    return r;
}

/* Replies of slow actions, produced on a thread, delivered by hub_cmd_poll on the main loop */
typedef struct deferred {
    int origin;
    cJSON *req;
    cJSON *reply;
    struct deferred *next;
} deferred_t;
static pthread_mutex_t g_deferred_lock = PTHREAD_MUTEX_INITIALIZER;
static deferred_t *g_deferred_head;
static int g_update_check_running;

static void *update_check_thread(void *arg) {
    deferred_t *d = (deferred_t *)arg;
    d->reply = update_check_reply(d->req);
    pthread_mutex_lock(&g_deferred_lock);
    d->next = g_deferred_head;
    g_deferred_head = d;
    g_update_check_running = 0;
    pthread_mutex_unlock(&g_deferred_lock);
    return NULL;
}

void hub_cmd_poll(void) {
    pthread_mutex_lock(&g_deferred_lock);
    deferred_t *list = g_deferred_head;
    g_deferred_head = NULL;
    pthread_mutex_unlock(&g_deferred_lock);
    while (list) {
        deferred_t *d = list;
        list = list->next;
        if (d->reply) {
            if (d->origin == HUB_CMD_ORIGIN_MQTT) {
                hub_mqtt_deliver_reply(d->req, d->reply);
            } else if (d->origin >= 0 && ws_is_client(d->origin)) {
                char *txt = cJSON_PrintUnformatted(d->reply);
                if (txt) {
                    if (ws_send_text(d->origin, txt) != 0) ws_remove_client(d->origin);
                    free(txt);
                }
            }
        }
        cJSON_Delete(d->reply);
        cJSON_Delete(d->req);
        free(d);
    }
}

cJSON *hub_cmd_execute(const cJSON *req, int role) {
    return hub_cmd_execute_from(req, role, HUB_CMD_ORIGIN_NONE);
}

cJSON *hub_cmd_execute_from(const cJSON *req, int role, int origin) {
    if (!req || !cJSON_IsObject(req)) return NULL;
    const char *action = json_str(req, "action", NULL, NULL);
    if (!action) action = "";

    /* A user-role token follows the permission table of the REST equivalents; ping and the
     * state queries are always allowed */
    if (role == (int)ROLE_USER) {
        const char *rest_path = NULL;
        if (strcmp(action, "send_command") == 0 || strcmp(action, "ir_send") == 0) rest_path = "/api/ir-send";
        else if (strcmp(action, "start_activity") == 0) rest_path = "/api/activity";
        else if (strcmp(action, "stop_activity") == 0 || strcmp(action, "power_off") == 0) rest_path = "/api/poweroff";
        else if (strcmp(action, "cancel_activity") == 0 || strcmp(action, "reset_interlock") == 0) rest_path = "/api/activity-cancel";
        else if (strcmp(action, "bt_key") == 0) rest_path = "/api/bt-key";
        else if (strcmp(action, "bt_text") == 0) rest_path = "/api/bt-text";
        else if (strcmp(action, "bt_mouse") == 0) rest_path = "/api/bt-mouse";
        else if (strcmp(action, "ir_raw") == 0) rest_path = "/api/ir-raw";
        else if (strcmp(action, "rf_wake") == 0) rest_path = "/api/rf/wake";
        else if (strcmp(action, "rf_sleep") == 0) rest_path = "/api/rf/sleep";
        else if (strcmp(action, "sync") == 0) rest_path = "/api/sync";
        else if (strcmp(action, "update_check") == 0) rest_path = "/api/update-check";
        else if (strcmp(action, "update_apply") == 0) rest_path = "/api/update-apply";
        else if (strcmp(action, "activity_help") == 0) rest_path = "/api/activity/help";
        else if (strcmp(action, "activity_help_answer") == 0) rest_path = "/api/activity/help/answer";
        if (rest_path && !http_auth_is_user_role_permitted("POST", rest_path)) {
            return ack(req, action, 0, 403, "Forbidden: Admin access required");
        }
    }

    if (strcmp(action, "ping") == 0) {
        cJSON *r = reply_base(req, "pong", NULL);
        cJSON_AddStringToObject(r, "activity", orch_get_current_activity());
        cJSON_AddNumberToObject(r, "state", (int)orch_get_state());
        return r;
    }
    if (strcmp(action, "get_state") == 0) {
        cJSON *r = reply_base(req, "activity_state", NULL);
        orch_state_t st = orch_get_state();
        cJSON_AddStringToObject(r, "activity", orch_get_current_activity());
        cJSON_AddNumberToObject(r, "state", (int)st);
        cJSON_AddStringToObject(r, "stateName", st == ORCH_STATE_STARTING ? "starting" : st == ORCH_STATE_RUNNING ? "running" :
                                                st == ORCH_STATE_STOPPING ? "stopping" : "idle");
        cJSON_AddBoolToObject(r, "busy", orch_is_busy() ? 1 : 0);
        cJSON_AddBoolToObject(r, "sending", orch_is_sending() ? 1 : 0);
        return r;
    }
    if (strcmp(action, "get_snapshot") == 0) {
        const cJSON *mid = cJSON_GetObjectItemCaseSensitive(req, "msgId");
        cJSON *snap = hub_ws_snapshot_object((mid && cJSON_IsNumber(mid)) ? mid->valueint : 0);
        if (!snap) return ack(req, action, 0, 500, "snapshot failed");
        const char *id = json_str(req, "id", NULL, NULL);
        if (id && id[0]) cJSON_AddStringToObject(snap, "id", id);
        return snap;
    }
    if (strcmp(action, "send_command") == 0 || strcmp(action, "ir_send") == 0) {
        char dev_buf[64] = {0};
        const cJSON *dev_j = cJSON_GetObjectItemCaseSensitive(req, "deviceId");
        if (!dev_j) dev_j = cJSON_GetObjectItemCaseSensitive(req, "device_id");
        if (!dev_j) dev_j = cJSON_GetObjectItemCaseSensitive(req, "device");
        if (dev_j) {
            if (cJSON_IsString(dev_j) && dev_j->valuestring) snprintf(dev_buf, sizeof(dev_buf), "%s", dev_j->valuestring);
            else if (cJSON_IsNumber(dev_j)) snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)dev_j->valuedouble);
        }
        const char *cmd = json_str(req, "command", NULL, NULL);
        if (!dev_buf[0] || !cmd || !cmd[0]) return ack(req, action, 0, 400, "deviceId and command required");
        int btn_action = btn_action_from(json_str(req, "cmdAction", "status", "action_type"));
        if (btn_action == HW_BTN_ACTION_TAP) btn_action = btn_action_from(json_str(req, "btnAction", NULL, NULL));
        int ok = (hw_device_command_send_action(dev_buf, 0, cmd, btn_action) == 0);
        return ok ? ack(req, action, 1, 200, NULL) : ack(req, action, 0, 500, "IR transmit failed");
    }
    if (strcmp(action, "start_activity") == 0) {
        /* "activity" names the activity. The web UI sends it as "id" (its request id is msgId),
         * but on MQTT and in the WebSocket API "id" is the request id: "id" counts only when no
         * "activity" is given and it names a configured activity. */
        const char *target = json_str(req, "activity", NULL, NULL);
        if (!target) {
            const char *id = json_str(req, "id", NULL, NULL);
            if (activity_id_known(id)) target = id;
        }
        if (!target || !target[0]) return ack(req, action, 0, 400, "activity required");
        int rc = orch_switch_activity(target, 0, NULL, 0);
        return rc == 0 ? ack(req, action, 1, 200, NULL) : ack(req, action, 0, 409, "busy");
    }
    if (strcmp(action, "stop_activity") == 0 || strcmp(action, "power_off") == 0) {
        int rc = orch_power_off(0, NULL, 0);
        cJSON *r = rc == 0 ? ack(req, "stop_activity", 1, 200, NULL) : ack(req, "stop_activity", 0, 409, "busy");
        return r;
    }
    if (strcmp(action, "cancel_activity") == 0 || strcmp(action, "reset_interlock") == 0) {
        orch_cancel();
        return ack(req, action, 1, 200, NULL);
    }
    if (strcmp(action, "bt_key") == 0) {
        const char *k = json_str(req, "key", NULL, NULL);
        const char *ka = json_str(req, "keyAction", NULL, NULL);
        const char *tgt = json_str(req, "target", "bdaddr", NULL);
        if (!k || !k[0]) return ack(req, action, 0, 400, "key required");
        if (!ka) ka = "tap";
        char line[160];
        if (strcasecmp(k, "release_all") == 0 || strcasecmp(ka, "up") == 0 || strcasecmp(ka, "release") == 0) {
            if (strcasecmp(k, "release_all") == 0) snprintf(line, sizeof(line), "RELEASE\n");
            else snprintf(line, sizeof(line), "KEYUP %s\n", k);
        } else if (strcasecmp(ka, "down") == 0 || strcasecmp(ka, "press") == 0) {
            snprintf(line, sizeof(line), "KEYDOWN %s\n", k);
        } else {
            snprintf(line, sizeof(line), "KEY %s\n", k);
        }
        int ok = pipe_write(tgt, line);
        return ok ? ack(req, action, 1, 200, NULL) : ack(req, action, 0, 503, "Bluetooth stack not running");
    }
    if (strcmp(action, "bt_text") == 0) {
        const char *t = json_str(req, "text", NULL, NULL);
        const char *tgt = json_str(req, "target", "bdaddr", NULL);
        if (!t || !t[0]) return ack(req, action, 0, 400, "text required");
        char *line = (char *)malloc(strlen(t) + 8);
        if (!line) return ack(req, action, 0, 500, "out of memory");
        sprintf(line, "TEXT %s\n", t);
        /* one pipe line per command: a line break in the text would start another command */
        for (char *c = line + 5; *c && c[1]; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        int ok = pipe_write(tgt, line);
        free(line);
        return ok ? ack(req, action, 1, 200, NULL) : ack(req, action, 0, 503, "Bluetooth stack not running");
    }
    if (strcmp(action, "bt_mouse") == 0) {
        const cJSON *dx_j = cJSON_GetObjectItemCaseSensitive(req, "dx");
        const cJSON *dy_j = cJSON_GetObjectItemCaseSensitive(req, "dy");
        const cJSON *btn_j = cJSON_GetObjectItemCaseSensitive(req, "buttons");
        const cJSON *wh_j = cJSON_GetObjectItemCaseSensitive(req, "wheel");
        const char *act = json_str(req, "mouseAction", NULL, NULL);
        const char *bname = json_str(req, "button", NULL, NULL);
        const char *tgt = json_str(req, "target", "bdaddr", NULL);
        int dx = (dx_j && cJSON_IsNumber(dx_j)) ? dx_j->valueint : 0;
        int dy = (dy_j && cJSON_IsNumber(dy_j)) ? dy_j->valueint : 0;
        int buttons = (btn_j && cJSON_IsNumber(btn_j)) ? btn_j->valueint : 0;
        int wheel = (wh_j && cJSON_IsNumber(wh_j)) ? wh_j->valueint : 0;
        if (!act) act = "";
        if (!bname || !bname[0]) bname = "left";
        char pipe_cmd[128];
        if (strcasecmp(act, "click") == 0) snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_CLICK %s\n", bname);
        else if (strcasecmp(act, "down") == 0) snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_DOWN %s\n", bname);
        else if (strcasecmp(act, "up") == 0) snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_UP %s\n", bname);
        else if (strcasecmp(act, "wheel") == 0) snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_WHEEL %d\n", wheel);
        else snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE %d %d %d %d\n", buttons, dx, dy, wheel);
        int ok = pipe_write(tgt, pipe_cmd);
        return ok ? ack(req, action, 1, 200, NULL) : ack(req, action, 0, 503, "Bluetooth stack not running");
    }
    if (strcmp(action, "rf_wake") == 0 || strcmp(action, "rf_sleep") == 0) {
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(req, "slot");
        int slot = (js && cJSON_IsNumber(js)) ? js->valueint : 1;
        if (slot < 1 || slot > 2) return ack(req, action, 0, 400, "slot: 1 or 2");
        char path[64];
        snprintf(path, sizeof(path), "/api/rf/%s?slot=%d", strcmp(action, "rf_wake") == 0 ? "wake" : "sleep", slot);
        int st = rf_request("POST", path, "{}");
        if (st < 0) return ack(req, action, 0, 503, "hub_rf not running");
        if (st >= 200 && st < 300) return ack(req, action, 1, 200, NULL);
        return ack(req, action, 0, st, st == 409 ? "busy" : "hub_rf refused");
    }
    if (strcmp(action, "sync") == 0) {
        invalidate_device_list_cache();
        config_store_init();
        notify_rf_push_config();
        return ack(req, action, 1, 200, NULL);
    }
    if (strcmp(action, "ir_raw") == 0) {
        cJSON *body = NULL;
        int code = hub_cmd_ir_raw(req, &body);
        cJSON *r = ack(req, action, code == 200, code, NULL);
        if (body) {
            if (code == 200) {
                cJSON *c = cJSON_GetObjectItemCaseSensitive(body, "count");
                cJSON *d = cJSON_GetObjectItemCaseSensitive(body, "duration_ms");
                if (c) cJSON_AddNumberToObject(r, "count", c->valuedouble);
                if (d) cJSON_AddNumberToObject(r, "duration_ms", d->valuedouble);
            } else {
                cJSON *e = cJSON_GetObjectItemCaseSensitive(body, "error");
                cJSON_DeleteItemFromObjectCaseSensitive(r, "error");
                cJSON_AddStringToObject(r, "error", (e && cJSON_IsString(e)) ? e->valuestring : "error");
            }
            cJSON_Delete(body);
        }
        return r;
    }
    if (strcmp(action, "update_check") == 0) {
        if (origin == HUB_CMD_ORIGIN_NONE) return update_check_reply(req);
        pthread_mutex_lock(&g_deferred_lock);
        int busy = g_update_check_running;
        if (!busy) g_update_check_running = 1;
        pthread_mutex_unlock(&g_deferred_lock);
        if (busy) return ack(req, action, 0, 409, "an update check is already running");
        deferred_t *d = (deferred_t *)calloc(1, sizeof(*d));
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        int started = 0;
        if (d) {
            d->origin = origin;
            d->req = cJSON_Duplicate(req, 1);
            started = d->req && pthread_create(&tid, &attr, update_check_thread, d) == 0;
        }
        pthread_attr_destroy(&attr);
        if (!started) {
            if (d) cJSON_Delete(d->req);
            free(d);
            pthread_mutex_lock(&g_deferred_lock);
            g_update_check_running = 0;
            pthread_mutex_unlock(&g_deferred_lock);
            return ack(req, action, 0, 500, "could not start the update check");
        }
        return NULL; /* hub_cmd_poll delivers the reply */
    }
    if (strcmp(action, "update_apply") == 0) {
        cJSON *rest = ota_update_apply(req);
        cJSON *r = ack_with(req, action, rest ? 200 : 500, rest);
        cJSON_Delete(rest);
        return r;
    }
    if (strcmp(action, "activity_help") == 0 || strcmp(action, "activity_help_answer") == 0) {
        cJSON *rest = NULL;
        int code = strcmp(action, "activity_help") == 0 ? activity_help_start(req, &rest) : activity_help_answer(req, &rest);
        cJSON *r = ack_with(req, action, code, rest);
        cJSON_Delete(rest);
        return r;
    }
    return ack(req, action[0] ? action : "unknown", 0, 400, "unknown action");
}
