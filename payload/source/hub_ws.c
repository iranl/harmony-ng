#define _GNU_SOURCE
#include "hub_ws.h"
#include "ws_server.h"
#include "hub_events.h"
#include "hub_info.h"
#include "http_auth.h"
#include "state_watch.h"
#include "config_store.h"
#include "hub_mqtt.h"
#include "orchestrator.h"
#include "hw_action.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <strings.h>
#include <time.h>

/* Bus sink: every event goes to every WebSocket client as it is (type, seq, ts added by the bus) */
void hub_ws_event_sink(const hub_event_t *ev, void *user) {
    (void)user;
    if (!ev || !ev->payload) return;
    char *txt = cJSON_PrintUnformatted(ev->payload);
    if (!txt) return;
    ws_broadcast_text(txt);
    free(txt);
}

/* The activity_state object as the bus last emitted it, or built from the orchestrator */
static cJSON *snapshot_activity_state(void) {
    const cJSON *last = hub_events_last(HUB_EV_ACTIVITY_STATE);
    if (last) return cJSON_Duplicate(last, 1);
    cJSON *st = cJSON_CreateObject();
    if (!st) return NULL;
    orch_state_t s = orch_get_state();
    cJSON_AddStringToObject(st, "type", "activity_state");
    cJSON_AddStringToObject(st, "activity", orch_get_current_activity());
    cJSON_AddNumberToObject(st, "state", (int)s);
    cJSON_AddStringToObject(st, "stateName", s == ORCH_STATE_STARTING ? "starting" :
                                             s == ORCH_STATE_RUNNING ? "running" :
                                             s == ORCH_STATE_STOPPING ? "stopping" : "idle");
    cJSON_AddBoolToObject(st, "busy", orch_is_busy() ? 1 : 0);
    cJSON_AddBoolToObject(st, "sending", orch_is_sending() ? 1 : 0);
    return st;
}

/* Full state snapshot (spec 3.9): hub identity, the last activity_state / bt_status / rf_status
 * payloads seen on the bus (null before the first one), the config file mtimes and the bus seq.
 * Main loop only (hub_events_last). */
static char *build_snapshot(int msg_id) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "type", "snapshot");
    if (msg_id) cJSON_AddNumberToObject(root, "msgId", msg_id);

    cJSON *hub = cJSON_CreateObject();
    struct hub_settings cs;
    config_store_get_settings(&cs);
    cJSON_AddStringToObject(hub, "hub_id", hub_info_id());
    cJSON_AddStringToObject(hub, "hub_name", cs.hub_name[0] ? cs.hub_name : "Harmony Hub");
    hub_info_add_status_fields(hub);
    cJSON_DeleteItemFromObjectCaseSensitive(hub, "commit");
    int auth_on = 0;
    load_user_accounts(NULL, 0, &auth_on);
    cJSON_AddBoolToObject(hub, "auth_enabled", auth_on);
    FILE *uf = fopen("/proc/uptime", "r");
    double up_sec = 0;
    if (uf) {
        if (fscanf(uf, "%lf", &up_sec) != 1) up_sec = 0;
        fclose(uf);
    }
    cJSON_AddNumberToObject(hub, "uptime_sec", (double)(long)up_sec);
    cJSON_AddItemToObject(root, "hub", hub);

    cJSON *act = snapshot_activity_state();
    if (act) cJSON_AddItemToObject(root, "activity_state", act);
    const cJSON *bt = hub_events_last(HUB_EV_BT_STATUS);
    if (bt) cJSON_AddItemToObject(root, "bt_status", cJSON_Duplicate(bt, 1));
    else cJSON_AddNullToObject(root, "bt_status");
    const cJSON *rf = hub_events_last(HUB_EV_RF_STATUS);
    if (rf) cJSON_AddItemToObject(root, "rf_status", cJSON_Duplicate(rf, 1));
    else cJSON_AddNullToObject(root, "rf_status");

    cJSON *cfg = cJSON_CreateObject();
    cJSON *files = state_watch_config_mtimes();
    if (files) cJSON_AddItemToObject(cfg, "files", files);
    cJSON_AddItemToObject(root, "config", cfg);

    cJSON_AddNumberToObject(root, "seq", (double)hub_events_last_seq());
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return txt;
}

static void send_snapshot(int cfd, int msg_id) {
    char *txt = build_snapshot(msg_id);
    if (!txt) return;
    if (ws_send_text(cfd, txt) != 0) ws_remove_client(cfd);
    free(txt);
}

void hub_ws_tick(void) {
    int fds[WS_MAX_CLIENTS];
    int n = ws_take_pending_clients(fds, WS_MAX_CLIENTS);
    for (int i = 0; i < n; i++) send_snapshot(fds[i], 0);
}

void on_orch_progress(const char *activity_id, orch_state_t state,
                      int current_step, int total_steps,
                      const char *step_desc, void *user_data) {
    (void)user_data;
    const char *act_id = (activity_id && activity_id[0]) ? activity_id : "-1";
    const char *st_str = (state == ORCH_STATE_STARTING) ? "starting" :
                         (state == ORCH_STATE_STOPPING) ? "stopping" :
                         (state == ORCH_STATE_RUNNING) ? "running" : "idle";
    const char *desc = step_desc ? step_desc : "";

    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "activity_progress");
    cJSON_AddStringToObject(root, "activity", act_id);
    cJSON_AddStringToObject(root, "activityId", act_id);
    cJSON_AddStringToObject(root, "state", st_str);
    cJSON_AddNumberToObject(root, "stateCode", (int)state);
    cJSON_AddNumberToObject(root, "step", current_step);
    cJSON_AddNumberToObject(root, "currentStep", current_step);
    cJSON_AddNumberToObject(root, "total", total_steps);
    cJSON_AddNumberToObject(root, "totalSteps", total_steps);
    cJSON_AddStringToObject(root, "desc", desc);
    cJSON_AddStringToObject(root, "stepDesc", desc);

    /* MQTT keeps the message as it was until milestone 3 moves it to a bus sink; WebSocket
     * clients get it from the bus (hub_ws_event_sink) */
    char *json_msg = cJSON_PrintUnformatted(root);
    if (json_msg) {
        hub_mqtt_publish_progress(json_msg);
        free(json_msg);
    }
    hub_events_emit(HUB_EV_ACTIVITY_PROGRESS, root);
}

void handle_ws_client_message(int cfd) {
    char buf[4096];
    int opcode = 0;
    int len = ws_read_frame(cfd, buf, sizeof(buf), &opcode);
    if (len < 0 || opcode == WS_OP_CLOSE) {
        ws_remove_client(cfd);  /* closes the socket (it used to be closed twice here) */
        return;
    }

    if (opcode == WS_OP_PING) {
        ws_send_pong(cfd);
        return;
    }

    if (opcode != WS_OP_TEXT || len == 0) return; /* pongs and other control frames: traffic only */

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;

    cJSON *act_j = cJSON_GetObjectItemCaseSensitive(root, "action");
    const char *action = (act_j && cJSON_IsString(act_j)) ? act_j->valuestring : "";

    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(root, "msgId");
    int msg_id = (id_j && cJSON_IsNumber(id_j)) ? id_j->valueint : 0;

    /* A user-role token follows the permission table of the REST equivalents; ping and the
     * state queries are always allowed */
    if (ws_get_client_role(cfd) == (int)ROLE_USER) {
        const char *rest_path = NULL;
        if (strcmp(action, "send_command") == 0 || strcmp(action, "ir_send") == 0) rest_path = "/api/ir-send";
        else if (strcmp(action, "start_activity") == 0) rest_path = "/api/activity";
        else if (strcmp(action, "stop_activity") == 0 || strcmp(action, "power_off") == 0) rest_path = "/api/poweroff";
        else if (strcmp(action, "cancel_activity") == 0 || strcmp(action, "reset_interlock") == 0) rest_path = "/api/activity-cancel";
        else if (strcmp(action, "bt_key") == 0) rest_path = "/api/bt-key";
        else if (strcmp(action, "bt_text") == 0) rest_path = "/api/bt-text";
        else if (strcmp(action, "bt_mouse") == 0) rest_path = "/api/bt-mouse";
        if (rest_path && !http_auth_is_user_role_permitted("POST", rest_path)) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "type", "ack");
            cJSON_AddStringToObject(resp, "action", action);
            cJSON_AddBoolToObject(resp, "ok", 0);
            cJSON_AddStringToObject(resp, "error", "Forbidden: Admin access required");
            if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
            char *str = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
            if (str) {
                ws_send_text(cfd, str);
                free(str);
            }
            cJSON_Delete(root);
            return;
        }
    }

    if (strcmp(action, "ping") == 0) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "pong");
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        cJSON_AddStringToObject(resp, "activity", orch_get_current_activity());
        cJSON_AddNumberToObject(resp, "state", (int)orch_get_state());
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "get_state") == 0) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "activity_state");
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        cJSON_AddStringToObject(resp, "activity", orch_get_current_activity());
        cJSON_AddNumberToObject(resp, "state", (int)orch_get_state());
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "get_snapshot") == 0) {
        send_snapshot(cfd, msg_id);
    }
    else if (strcmp(action, "send_command") == 0 || strcmp(action, "ir_send") == 0) {
        cJSON *dev_j = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
        if (!dev_j) dev_j = cJSON_GetObjectItemCaseSensitive(root, "device_id");
        if (!dev_j) dev_j = cJSON_GetObjectItemCaseSensitive(root, "device");
        cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(root, "command");
        char dev_buf[64] = {0};
        if (dev_j) {
            if (cJSON_IsString(dev_j) && dev_j->valuestring) strncpy(dev_buf, dev_j->valuestring, sizeof(dev_buf) - 1);
            else if (cJSON_IsNumber(dev_j)) snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)dev_j->valuedouble);
        }
        const char *cmd = (cmd_j && cJSON_IsString(cmd_j)) ? cmd_j->valuestring : "";

        int btn_action = HW_BTN_ACTION_TAP;
        cJSON *act_j = cJSON_GetObjectItemCaseSensitive(root, "cmdAction");
        if (!act_j) act_j = cJSON_GetObjectItemCaseSensitive(root, "action_type");
        if (!act_j) act_j = cJSON_GetObjectItemCaseSensitive(root, "btnAction");
        if (act_j && cJSON_IsString(act_j) && act_j->valuestring) {
            if (strcasecmp(act_j->valuestring, "press") == 0 || strcasecmp(act_j->valuestring, "down") == 0) {
                btn_action = HW_BTN_ACTION_PRESS;
            } else if (strcasecmp(act_j->valuestring, "hold") == 0 || strcasecmp(act_j->valuestring, "repeat") == 0) {
                btn_action = HW_BTN_ACTION_REPEAT;
            } else if (strcasecmp(act_j->valuestring, "release") == 0 || strcasecmp(act_j->valuestring, "up") == 0) {
                btn_action = HW_BTN_ACTION_RELEASE;
            }
        }

        int ok = 0;
        if (dev_buf[0] && cmd[0]) {
            ok = (hw_device_command_send_action(dev_buf, 0, cmd, btn_action) == 0);
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", action);
        cJSON_AddBoolToObject(resp, "ok", ok);
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "start_activity") == 0) {
        cJSON *target_j = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (!target_j) target_j = cJSON_GetObjectItemCaseSensitive(root, "activity");
        const char *target = (target_j && cJSON_IsString(target_j)) ? target_j->valuestring : "";

        int rc = -1;
        if (target[0]) {
            rc = orch_switch_activity(target, 0, NULL, 0);
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", "start_activity");
        cJSON_AddBoolToObject(resp, "ok", rc == 0);
        if (rc != 0) cJSON_AddStringToObject(resp, "error", "Interlock busy");
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "stop_activity") == 0 || strcmp(action, "power_off") == 0) {
        int rc = orch_power_off(0, NULL, 0);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", "stop_activity");
        cJSON_AddBoolToObject(resp, "ok", rc == 0);
        if (rc != 0) cJSON_AddStringToObject(resp, "error", "Interlock busy");
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "cancel_activity") == 0 || strcmp(action, "reset_interlock") == 0) {
        orch_cancel();
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", action);
        cJSON_AddBoolToObject(resp, "ok", 1);
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "bt_key") == 0) {
        cJSON *k_j = cJSON_GetObjectItemCaseSensitive(root, "key");
        cJSON *a_j = cJSON_GetObjectItemCaseSensitive(root, "keyAction");
        cJSON *tgt_j = cJSON_GetObjectItemCaseSensitive(root, "target");
        if (!tgt_j) tgt_j = cJSON_GetObjectItemCaseSensitive(root, "bdaddr");
        const char *k = (k_j && cJSON_IsString(k_j)) ? k_j->valuestring : "";
        const char *ka = (a_j && cJSON_IsString(a_j)) ? a_j->valuestring : "tap";
        const char *tgt = (tgt_j && cJSON_IsString(tgt_j)) ? tgt_j->valuestring : "";
        int ok = 0;
        if (k[0]) {
            char line[160];
            if (strcasecmp(k, "release_all") == 0 || strcasecmp(ka, "up") == 0 || strcasecmp(ka, "release") == 0) {
                if (strcasecmp(k, "release_all") == 0) snprintf(line, sizeof(line), "RELEASE\n");
                else snprintf(line, sizeof(line), "KEYUP %s\n", k);
            } else if (strcasecmp(ka, "down") == 0 || strcasecmp(ka, "press") == 0) {
                snprintf(line, sizeof(line), "KEYDOWN %s\n", k);
            } else {
                snprintf(line, sizeof(line), "KEY %s\n", k);
            }
            int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
            if (bfd >= 0) {
                if (tgt[0]) {
                    char thdr[64];
                    snprintf(thdr, sizeof(thdr), "TARGET %s\n", tgt);
                    write(bfd, thdr, strlen(thdr));
                }
                if (write(bfd, line, strlen(line)) > 0) ok = 1;
                close(bfd);
            }
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", "bt_key");
        cJSON_AddBoolToObject(resp, "ok", ok);
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "bt_text") == 0) {
        cJSON *t_j = cJSON_GetObjectItemCaseSensitive(root, "text");
        cJSON *tgt_j = cJSON_GetObjectItemCaseSensitive(root, "target");
        if (!tgt_j) tgt_j = cJSON_GetObjectItemCaseSensitive(root, "bdaddr");
        const char *t = (t_j && cJSON_IsString(t_j)) ? t_j->valuestring : "";
        const char *tgt = (tgt_j && cJSON_IsString(tgt_j)) ? tgt_j->valuestring : "";
        int ok = 0;
        if (t[0]) {
            int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
            if (bfd >= 0) {
                if (tgt[0]) {
                    char thdr[64];
                    snprintf(thdr, sizeof(thdr), "TARGET %s\n", tgt);
                    write(bfd, thdr, strlen(thdr));
                }
                char prefix[] = "TEXT ";
                write(bfd, prefix, strlen(prefix));
                if (write(bfd, t, strlen(t)) > 0) {
                    write(bfd, "\n", 1);
                    ok = 1;
                }
                close(bfd);
            }
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", "bt_text");
        cJSON_AddBoolToObject(resp, "ok", ok);
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }
    else if (strcmp(action, "bt_mouse") == 0) {
        cJSON *dx_j = cJSON_GetObjectItemCaseSensitive(root, "dx");
        cJSON *dy_j = cJSON_GetObjectItemCaseSensitive(root, "dy");
        cJSON *btn_j = cJSON_GetObjectItemCaseSensitive(root, "buttons");
        cJSON *wh_j = cJSON_GetObjectItemCaseSensitive(root, "wheel");
        cJSON *act_j = cJSON_GetObjectItemCaseSensitive(root, "mouseAction");
        if (!act_j) act_j = cJSON_GetObjectItemCaseSensitive(root, "action");
        cJSON *bname_j = cJSON_GetObjectItemCaseSensitive(root, "button");
        cJSON *tgt_j = cJSON_GetObjectItemCaseSensitive(root, "target");
        if (!tgt_j) tgt_j = cJSON_GetObjectItemCaseSensitive(root, "bdaddr");

        int dx = (dx_j && cJSON_IsNumber(dx_j)) ? dx_j->valueint : 0;
        int dy = (dy_j && cJSON_IsNumber(dy_j)) ? dy_j->valueint : 0;
        int buttons = (btn_j && cJSON_IsNumber(btn_j)) ? btn_j->valueint : 0;
        int wheel = (wh_j && cJSON_IsNumber(wh_j)) ? wh_j->valueint : 0;
        const char *act = (act_j && cJSON_IsString(act_j)) ? act_j->valuestring : "";
        const char *bname = (bname_j && cJSON_IsString(bname_j)) ? bname_j->valuestring : "left";
        const char *tgt = (tgt_j && cJSON_IsString(tgt_j)) ? tgt_j->valuestring : "";

        int ok = 0;
        int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
        if (bfd >= 0) {
            if (tgt[0]) {
                char thdr[64];
                snprintf(thdr, sizeof(thdr), "TARGET %s\n", tgt);
                write(bfd, thdr, strlen(thdr));
            }
            char pipe_cmd[128];
            if (strcasecmp(act, "click") == 0) {
                snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_CLICK %s\n", bname[0] ? bname : "left");
            } else if (strcasecmp(act, "down") == 0) {
                snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_DOWN %s\n", bname[0] ? bname : "left");
            } else if (strcasecmp(act, "up") == 0) {
                snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_UP %s\n", bname[0] ? bname : "left");
            } else if (strcasecmp(act, "wheel") == 0) {
                snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE_WHEEL %d\n", wheel);
            } else {
                snprintf(pipe_cmd, sizeof(pipe_cmd), "MOUSE %d %d %d %d\n", buttons, dx, dy, wheel);
            }
            if (write(bfd, pipe_cmd, strlen(pipe_cmd)) > 0) ok = 1;
            close(bfd);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "action", "bt_mouse");
        cJSON_AddBoolToObject(resp, "ok", ok);
        if (msg_id) cJSON_AddNumberToObject(resp, "msgId", msg_id);
        char *str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (str) {
            ws_send_text(cfd, str);
            free(str);
        }
    }

    cJSON_Delete(root);
}
