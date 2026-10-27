#define _GNU_SOURCE
#include "hub_ws.h"
#include "ws_server.h"
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

void on_orch_progress(const char *activity_id, orch_state_t state,
                      int current_step, int total_steps,
                      const char *step_desc, void *user_data) {
    (void)user_data;
    char json_msg[512];
    const char *st_str = (state == ORCH_STATE_STARTING) ? "starting" :
                         (state == ORCH_STATE_STOPPING) ? "stopping" :
                         (state == ORCH_STATE_RUNNING) ? "running" : "idle";
    snprintf(json_msg, sizeof(json_msg),
             "{\"type\":\"activity_progress\",\"activityId\":\"%s\",\"state\":\"%s\","
             "\"currentStep\":%d,\"totalSteps\":%d,\"stepDesc\":\"%s\"}",
             activity_id ? activity_id : "-1", st_str, current_step, total_steps,
             step_desc ? step_desc : "");
    ws_broadcast_text(json_msg);
    hub_mqtt_publish_progress(json_msg);
}

void handle_ws_client_message(int cfd) {
    char buf[4096];
    int opcode = 0;
    int len = ws_read_frame(cfd, buf, sizeof(buf), &opcode);
    if (len <= 0 || opcode == WS_OP_CLOSE) {
        close(cfd);
        ws_remove_client(cfd);
        return;
    }

    if (opcode == WS_OP_PING) {
        ws_send_pong(cfd);
        return;
    }

    if (opcode != WS_OP_TEXT) return;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;

    cJSON *act_j = cJSON_GetObjectItemCaseSensitive(root, "action");
    const char *action = (act_j && cJSON_IsString(act_j)) ? act_j->valuestring : "";

    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(root, "msgId");
    int msg_id = (id_j && cJSON_IsNumber(id_j)) ? id_j->valueint : 0;

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
    else if (strcmp(action, "send_command") == 0 || strcmp(action, "ir_send") == 0) {
        cJSON *dev_j = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
        cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(root, "command");
        const char *dev = (dev_j && cJSON_IsString(dev_j)) ? dev_j->valuestring : "";
        const char *cmd = (cmd_j && cJSON_IsString(cmd_j)) ? cmd_j->valuestring : "";

        int ok = 0;
        if (dev[0] && cmd[0]) {
            ok = (hw_device_command_send(dev, cmd) == 0);
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
