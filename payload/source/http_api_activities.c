#define _GNU_SOURCE
#include "http_api_activities.h"
#include "http_util.h"
#include "config_store.h"
#include "config_sanitize.h"
#include "orchestrator.h"
#include "hw_action.h"
#include "ws_server.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>

static cJSON *build_activity_steps_cjson(const char *seq_text) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    if (!seq_text || !seq_text[0]) return arr;

    const char *p = seq_text;
    char line[512];

    while (*p) {
        size_t l = 0;
        while (*p && *p != '\n' && *p != '\r' && l < sizeof(line) - 1) {
            line[l++] = *p++;
        }
        line[l] = '\0';
        while (*p == '\n' || *p == '\r') p++;
        if (!line[0]) continue;

        char type[32] = {0}, dev_id[32] = {0}, cmd[80] = {0}, del_str[32] = {0};
        char *f[4] = { type, dev_id, cmd, del_str };
        int fi = 0;
        char *lp = line;
        while (fi < 4 && *lp) {
            char *sep = strchr(lp, '|');
            if (sep) {
                *sep = '\0';
                strncpy(f[fi], lp, (fi == 2) ? 79 : 31);
                lp = sep + 1;
            } else {
                strncpy(f[fi], lp, (fi == 2) ? 79 : 31);
                break;
            }
            fi++;
        }

        if (strcasecmp(type, "Delay") == 0) {
            if (!del_str[0] && dev_id[0] && atoi(dev_id) > 0) {
                strncpy(del_str, dev_id, sizeof(del_str) - 1);
            } else if (!del_str[0] && cmd[0] && atoi(cmd) > 0) {
                strncpy(del_str, cmd, sizeof(del_str) - 1);
            }
        }
        int delay_ms = atoi(del_str);

        cJSON *step = cJSON_CreateObject();
        if (strcasecmp(type, "Delay") == 0) {
            int dms = delay_ms > 0 ? delay_ms : 1000;
            cJSON_AddStringToObject(step, "type", "Delay");
            cJSON_AddStringToObject(step, "deviceId", "0");
            cJSON_AddStringToObject(step, "command", "");
            cJSON_AddNumberToObject(step, "delay", dms);
        } else if (strcasecmp(type, "Switched") == 0 || strcasecmp(type, "ActivitySwitched") == 0 || strcasecmp(cmd, "Switched") == 0) {
            cJSON_AddStringToObject(step, "type", "Switched");
            cJSON_AddStringToObject(step, "deviceId", "0");
            cJSON_AddStringToObject(step, "command", "Switched");
            cJSON_AddNumberToObject(step, "delay", 0);
        } else if (strcasecmp(type, "BTCommand") == 0) {
            cJSON_AddStringToObject(step, "type", "BTCommand");
            cJSON_AddStringToObject(step, "deviceId", dev_id[0] ? dev_id : "0");
            cJSON_AddStringToObject(step, "command", cmd);
            cJSON_AddNumberToObject(step, "delay", delay_ms >= 0 ? delay_ms : 0);
        } else {
            cJSON_AddStringToObject(step, "type", "Command");
            cJSON_AddStringToObject(step, "deviceId", dev_id);
            cJSON_AddStringToObject(step, "command", cmd);
            cJSON_AddNumberToObject(step, "delay", delay_ms >= 0 ? delay_ms : 0);
        }
        cJSON_AddItemToArray(arr, step);
    }
    return arr;
}

/* Help: the devices the last Help asked about, so answers toggle only those */
static orch_toggle_t s_help_toggles[16];
static size_t s_help_toggle_count;
static pthread_mutex_t s_help_lock = PTHREAD_MUTEX_INITIALIZER;

static void device_name(const char *id, char *out, size_t len) {
    snprintf(out, len, "%s", id);
    cJSON *root = config_store_get_devices_json();
    cJSON *devs = root ? cJSON_GetObjectItemCaseSensitive(root, "devices") : NULL;
    cJSON *d = NULL;
    cJSON_ArrayForEach(d, devs) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
        cJSON *jname = cJSON_GetObjectItemCaseSensitive(d, "name");
        char d_id[32] = {0};
        if (cJSON_IsNumber(jid)) snprintf(d_id, sizeof(d_id), "%ld", (long)jid->valuedouble);
        else if (cJSON_IsString(jid)) snprintf(d_id, sizeof(d_id), "%s", jid->valuestring);
        if (strcmp(d_id, id) == 0 && cJSON_IsString(jname)) {
            snprintf(out, len, "%s", jname->valuestring);
            return;
        }
    }
}

int http_api_activities_handle(int client_fd, const char *method, const char *clean_path,
                               const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    /* Route: GET /api/activities */
    if (strcmp(clean_path, "/api/activities") == 0 && strcmp(method, "GET") == 0) {
        cJSON *act_j = config_store_get_activities_json();
        if (act_j) {
            cJSON *resp = cJSON_Duplicate(act_j, 1);
            if (resp) {
                cJSON_AddStringToObject(resp, "currentId", orch_get_current_activity());
                cJSON_AddNumberToObject(resp, "state", (int)orch_get_state());
                send_cjson_resp(client_fd, 200, resp);
                cJSON_Delete(resp);
                close(client_fd);
                return 1;
            }
        }
        send_json_error(client_fd, 404, "Activities not found");
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/activity or GET /api/current_activity */
    if ((strcmp(clean_path, "/api/activity") == 0 || strcmp(clean_path, "/api/current_activity") == 0) &&
        strcmp(method, "GET") == 0) {
        cJSON *aj = cJSON_CreateObject();
        cJSON_AddStringToObject(aj, "activity", orch_get_current_activity());
        cJSON_AddNumberToObject(aj, "state", (int)orch_get_state());
        cJSON_AddBoolToObject(aj, "busy", orch_is_busy() ? 1 : 0);
        cJSON_AddBoolToObject(aj, "sending", orch_is_sending() ? 1 : 0);
        send_cjson_resp(client_fd, 200, aj);
        cJSON_Delete(aj);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/activity-progress */
    if (strcmp(clean_path, "/api/activity-progress") == 0 && strcmp(method, "POST") == 0) {
        if (body && body[0]) {
            ws_broadcast_text(body);
        }
        cJSON *ok = cJSON_CreateObject();
        cJSON_AddBoolToObject(ok, "ok", 1);
        send_cjson_resp(client_fd, 200, ok);
        cJSON_Delete(ok);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/activity or POST /api/activity-start */
    if ((strcmp(clean_path, "/api/activity") == 0 || strcmp(clean_path, "/api/activity-start") == 0) && strcmp(method, "POST") == 0) {
        char act_id[32] = {0};
        int dry_run = 0;
        cJSON *req_j = body ? cJSON_Parse(body) : NULL;
        if (req_j) {
            cJSON *ja = cJSON_GetObjectItemCaseSensitive(req_j, "activity");
            if (!ja) ja = cJSON_GetObjectItemCaseSensitive(req_j, "id");
            if (ja && cJSON_IsString(ja) && ja->valuestring) strncpy(act_id, ja->valuestring, sizeof(act_id) - 1);
            else if (ja && cJSON_IsNumber(ja)) snprintf(act_id, sizeof(act_id), "%ld", (long)ja->valuedouble);

            cJSON *jd = cJSON_GetObjectItemCaseSensitive(req_j, "dry_run");
            if (jd) {
                if (cJSON_IsTrue(jd)) dry_run = 1;
                else if (cJSON_IsNumber(jd) && jd->valuedouble > 0) dry_run = 1;
                else if (cJSON_IsString(jd) && (strcmp(jd->valuestring, "true") == 0 || strcmp(jd->valuestring, "1") == 0)) dry_run = 1;
            }
            cJSON_Delete(req_j);
        }

        if (!act_id[0]) {
            send_json_error(client_fd, 400, "Missing activity parameter");
            close(client_fd);
            return 1;
        }

        if (dry_run) {
            char preview[4096] = {0};
            int rc = orch_switch_activity(act_id, 1, preview, sizeof(preview));
            if (rc == 0) {
                cJSON *pj = preview[0] ? cJSON_Parse(preview) : NULL;
                if (!pj) {
                    pj = cJSON_CreateObject();
                    cJSON_AddStringToObject(pj, "target", act_id);
                    cJSON_AddArrayToObject(pj, "steps");
                }
                send_cjson_resp(client_fd, 200, pj);
                cJSON_Delete(pj);
            } else {
                send_json_error(client_fd, 409, "Interlock busy");
            }
        } else {
            int rc = orch_switch_activity(act_id, 0, NULL, 0);
            if (rc == 0) {
                cJSON *suc = cJSON_CreateObject();
                cJSON_AddBoolToObject(suc, "success", 1);
                cJSON_AddStringToObject(suc, "state", "starting");
                send_cjson_resp(client_fd, 200, suc);
                cJSON_Delete(suc);
            } else {
                send_json_error(client_fd, 409, "Interlock busy");
            }
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/activity/help: re-send the running activity's commands except power
       toggles, and list the devices with toggles to ask about */
    if (strcmp(clean_path, "/api/activity/help") == 0 && strcmp(method, "POST") == 0) {
        char act_id[32] = {0};
        int dry_run = 0;
        cJSON *req_j = body ? cJSON_Parse(body) : NULL;
        cJSON *ja = req_j ? cJSON_GetObjectItemCaseSensitive(req_j, "activity") : NULL;
        if (cJSON_IsString(ja)) snprintf(act_id, sizeof(act_id), "%s", ja->valuestring);
        else if (cJSON_IsNumber(ja)) snprintf(act_id, sizeof(act_id), "%ld", (long)ja->valuedouble);
        cJSON *jd = req_j ? cJSON_GetObjectItemCaseSensitive(req_j, "dry_run") : NULL;
        if (cJSON_IsTrue(jd) || (cJSON_IsNumber(jd) && jd->valuedouble > 0)) dry_run = 1;
        cJSON_Delete(req_j);
        if (!act_id[0]) snprintf(act_id, sizeof(act_id), "%s", orch_get_current_activity());

        orch_toggle_t toggles[16];
        size_t n = 0;
        char preview[8192] = {0};
        int rc = orch_repair_activity(act_id, dry_run, toggles, 16, &n, preview, sizeof(preview));
        if (rc == -2) {
            send_json_error(client_fd, 409, "Interlock busy");
        } else if (rc == -3) {
            send_json_error(client_fd, 409, "Activity is not running");
        } else if (rc != 0) {
            send_json_error(client_fd, 500, "Could not build the activity's commands");
        } else {
            if (!dry_run) {
                pthread_mutex_lock(&s_help_lock);
                memcpy(s_help_toggles, toggles, n * sizeof(toggles[0]));
                s_help_toggle_count = n;
                pthread_mutex_unlock(&s_help_lock);
            }
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "success", 1);
            cJSON_AddStringToObject(resp, "activity", act_id);
            cJSON *arr = cJSON_AddArrayToObject(resp, "toggleDevices");
            for (size_t i = 0; i < n; i++) {
                char name[128];
                device_name(toggles[i].device_id, name, sizeof(name));
                cJSON *d = cJSON_CreateObject();
                cJSON_AddStringToObject(d, "id", toggles[i].device_id);
                cJSON_AddStringToObject(d, "name", name);
                cJSON_AddStringToObject(d, "command", toggles[i].command);
                cJSON_AddItemToArray(arr, d);
            }
            cJSON *steps = dry_run && preview[0] ? cJSON_Parse(preview) : NULL;
            if (steps) cJSON_AddItemToObject(resp, "preview", steps);
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/activity/help/answer: {"device": id, "on": false} sends the device's
       power toggle; only devices the last Help listed, once each */
    if (strcmp(clean_path, "/api/activity/help/answer") == 0 && strcmp(method, "POST") == 0) {
        char dev_id[32] = {0};
        int on = 1;
        cJSON *req_j = body ? cJSON_Parse(body) : NULL;
        cJSON *jdev = req_j ? cJSON_GetObjectItemCaseSensitive(req_j, "device") : NULL;
        if (cJSON_IsString(jdev)) snprintf(dev_id, sizeof(dev_id), "%s", jdev->valuestring);
        else if (cJSON_IsNumber(jdev)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jdev->valuedouble);
        cJSON *jon = req_j ? cJSON_GetObjectItemCaseSensitive(req_j, "on") : NULL;
        if (cJSON_IsFalse(jon) || (cJSON_IsNumber(jon) && jon->valuedouble == 0)) on = 0;
        cJSON_Delete(req_j);

        char cmd[64] = {0};
        pthread_mutex_lock(&s_help_lock);
        for (size_t i = 0; i < s_help_toggle_count; i++) {
            if (strcmp(s_help_toggles[i].device_id, dev_id) != 0) continue;
            /* One answer per device, so a repeated "No" cannot toggle it back off */
            snprintf(cmd, sizeof(cmd), "%s", s_help_toggles[i].command);
            s_help_toggles[i] = s_help_toggles[--s_help_toggle_count];
            break;
        }
        pthread_mutex_unlock(&s_help_lock);
        if (!cmd[0]) {
            send_json_error(client_fd, 404, "Device is not waiting for a Help answer");
        } else {
            if (!on) hw_device_command_send(dev_id, cmd);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "success", 1);
            cJSON_AddBoolToObject(resp, "sent", !on);
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/poweroff or POST /api/activity-stop */
    if ((strcmp(clean_path, "/api/poweroff") == 0 || strcmp(clean_path, "/api/activity-stop") == 0) && strcmp(method, "POST") == 0) {
        int dry_run = 0;
        cJSON *req_j = body ? cJSON_Parse(body) : NULL;
        if (req_j) {
            cJSON *jd = cJSON_GetObjectItemCaseSensitive(req_j, "dry_run");
            if (jd) {
                if (cJSON_IsTrue(jd)) dry_run = 1;
                else if (cJSON_IsNumber(jd) && jd->valuedouble > 0) dry_run = 1;
                else if (cJSON_IsString(jd) && (strcmp(jd->valuestring, "true") == 0 || strcmp(jd->valuestring, "1") == 0)) dry_run = 1;
            }
            cJSON_Delete(req_j);
        }

        if (dry_run) {
            char preview[4096] = {0};
            int rc = orch_power_off(1, preview, sizeof(preview));
            if (rc == 0) {
                cJSON *pj = preview[0] ? cJSON_Parse(preview) : NULL;
                if (!pj) {
                    pj = cJSON_CreateObject();
                    cJSON_AddStringToObject(pj, "target", ORCH_POWEROFF_ID);
                    cJSON_AddArrayToObject(pj, "steps");
                }
                send_cjson_resp(client_fd, 200, pj);
                cJSON_Delete(pj);
            } else {
                send_json_error(client_fd, 409, "busy");
            }
        } else {
            int rc = orch_power_off(0, NULL, 0);
            if (rc == 0) {
                cJSON *suc = cJSON_CreateObject();
                cJSON_AddBoolToObject(suc, "success", 1);
                cJSON_AddStringToObject(suc, "state", "stopping");
                send_cjson_resp(client_fd, 200, suc);
                cJSON_Delete(suc);
            } else {
                send_json_error(client_fd, 409, "busy");
            }
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/activity-cancel or POST /api/activity-reset or POST /api/interlock-reset */
    if ((strcmp(clean_path, "/api/activity-cancel") == 0 ||
         strcmp(clean_path, "/api/activity-reset") == 0 ||
         strcmp(clean_path, "/api/interlock-reset") == 0) && strcmp(method, "POST") == 0) {
        orch_cancel();
        cJSON *suc = cJSON_CreateObject();
        cJSON_AddBoolToObject(suc, "success", 1);
        cJSON_AddStringToObject(suc, "message", "Activity cancelled / interlock reset");
        send_cjson_resp(client_fd, 200, suc);
        cJSON_Delete(suc);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/activity-save */
    if (strcmp(clean_path, "/api/activity-save") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int success = 0;
        if (params) {
            cJSON *p_id = cJSON_GetObjectItemCaseSensitive(params, "id");
            if (!p_id) p_id = cJSON_GetObjectItemCaseSensitive(params, "activityId");
            const char *act_id_str = p_id ? (cJSON_IsString(p_id) ? p_id->valuestring : "") : "";
            char id_buf[32] = {0};
            if (!act_id_str[0] && p_id && cJSON_IsNumber(p_id)) {
                snprintf(id_buf, sizeof(id_buf), "%ld", (long)p_id->valuedouble);
                act_id_str = id_buf;
            }

            cJSON *p_name = cJSON_GetObjectItemCaseSensitive(params, "name");
            const char *name_str = (p_name && cJSON_IsString(p_name)) ? p_name->valuestring : "";

            cJSON *p_type = cJSON_GetObjectItemCaseSensitive(params, "type");
            const char *type_str = (p_type && cJSON_IsString(p_type)) ? p_type->valuestring : "Custom";

            cJSON *p_icon = cJSON_GetObjectItemCaseSensitive(params, "icon");
            const char *icon_str = (p_icon && cJSON_IsString(p_icon)) ? p_icon->valuestring : "Default";

            cJSON *p_order = cJSON_GetObjectItemCaseSensitive(params, "order");
            int order_val = (p_order && cJSON_IsNumber(p_order)) ? p_order->valueint :
                            (p_order && cJSON_IsString(p_order) ? atoi(p_order->valuestring) : 1);

            cJSON *p_enter = cJSON_GetObjectItemCaseSensitive(params, "startSequence");
            cJSON *p_leave = cJSON_GetObjectItemCaseSensitive(params, "stopSequence");

            cJSON *enter_steps = NULL;
            if (p_enter && cJSON_IsArray(p_enter)) {
                enter_steps = cJSON_Duplicate(p_enter, 1);
                sanitize_activity_steps(enter_steps);
            } else if (p_enter && cJSON_IsString(p_enter)) {
                enter_steps = build_activity_steps_cjson(p_enter->valuestring);
            } else {
                enter_steps = cJSON_CreateArray();
            }

            cJSON *leave_steps = NULL;
            if (p_leave && cJSON_IsArray(p_leave)) {
                leave_steps = cJSON_Duplicate(p_leave, 1);
                sanitize_activity_steps(leave_steps);
            } else if (p_leave && cJSON_IsString(p_leave)) {
                leave_steps = build_activity_steps_cjson(p_leave->valuestring);
            } else {
                leave_steps = cJSON_CreateArray();
            }

            cJSON *dev_ids_arr = cJSON_CreateArray();
            cJSON *p_devs = cJSON_GetObjectItemCaseSensitive(params, "deviceIds");
            if (!p_devs) p_devs = cJSON_GetObjectItemCaseSensitive(params, "devices");
            int dev_ids_provided = (p_devs != NULL);
            if (p_devs && cJSON_IsArray(p_devs)) {
                const cJSON *ditem = NULL;
                cJSON_ArrayForEach(ditem, p_devs) {
                    char dstr[32] = {0};
                    if (cJSON_IsString(ditem) && ditem->valuestring) strncpy(dstr, ditem->valuestring, sizeof(dstr) - 1);
                    else if (cJSON_IsNumber(ditem)) snprintf(dstr, sizeof(dstr), "%ld", (long)ditem->valuedouble);
                    if (dstr[0] && strcmp(dstr, "0") != 0) {
                        cJSON_AddItemToArray(dev_ids_arr, cJSON_CreateString(dstr));
                    }
                }
            } else if (p_devs && cJSON_IsString(p_devs) && p_devs->valuestring[0]) {
                char dev_buf[512];
                strncpy(dev_buf, p_devs->valuestring, sizeof(dev_buf) - 1);
                dev_buf[sizeof(dev_buf) - 1] = '\0';
                char *tok = strtok(dev_buf, ",; ");
                while (tok) {
                    if (tok[0] && strcmp(tok, "0") != 0) {
                        cJSON_AddItemToArray(dev_ids_arr, cJSON_CreateString(tok));
                    }
                    tok = strtok(NULL, ",; ");
                }
            }
            if (!dev_ids_provided && cJSON_GetArraySize(dev_ids_arr) == 0) {
                cJSON *arrs[2] = { enter_steps, leave_steps };
                for (int ai_idx = 0; ai_idx < 2; ai_idx++) {
                    const cJSON *st = NULL;
                    cJSON_ArrayForEach(st, arrs[ai_idx]) {
                        const cJSON *did = cJSON_GetObjectItemCaseSensitive(st, "deviceId");
                        if (did && cJSON_IsString(did) && did->valuestring[0] && strcmp(did->valuestring, "0") != 0) {
                            int exists = 0;
                            const cJSON *ex = NULL;
                            cJSON_ArrayForEach(ex, dev_ids_arr) {
                                if (ex->valuestring && strcmp(ex->valuestring, did->valuestring) == 0) { exists = 1; break; }
                            }
                            if (!exists) cJSON_AddItemToArray(dev_ids_arr, cJSON_CreateString(did->valuestring));
                        }
                    }
                }
            }

            cJSON *cached = config_store_get_activities_json();
            cJSON *root = cached ? cJSON_Duplicate(cached, 1) : cJSON_CreateObject();
            if (root) {
                cJSON *acts = cJSON_GetObjectItemCaseSensitive(root, "activities");
                if (!acts) acts = cJSON_AddArrayToObject(root, "activities");
                if (acts && cJSON_IsArray(acts)) {
                    cJSON *matched = NULL;
                    cJSON *ai = NULL;
                    cJSON_ArrayForEach(ai, acts) {
                        cJSON *jid = cJSON_GetObjectItemCaseSensitive(ai, "id");
                        char cur_id[32] = {0};
                        if (jid && cJSON_IsNumber(jid)) snprintf(cur_id, sizeof(cur_id), "%ld", (long)jid->valuedouble);
                        else if (jid && cJSON_IsString(jid)) strncpy(cur_id, jid->valuestring, sizeof(cur_id) - 1);
                        cJSON *jname = cJSON_GetObjectItemCaseSensitive(ai, "name");
                        const char *cur_name = (jname && cJSON_IsString(jname)) ? jname->valuestring : "";

                        if ((act_id_str[0] && strcmp(cur_id, act_id_str) == 0) ||
                            (strcmp(act_id_str, "-1") == 0 && strcasecmp(cur_name, "PowerOff") == 0) ||
                            (name_str[0] && strcasecmp(cur_name, name_str) == 0)) {
                            matched = ai;
                            break;
                        }
                    }

                    if (matched) {
                        if (name_str[0]) {
                            cJSON_DeleteItemFromObject(matched, "name");
                            cJSON_AddStringToObject(matched, "name", name_str);
                        }
                        if (type_str[0]) {
                            cJSON_DeleteItemFromObject(matched, "type");
                            cJSON_AddStringToObject(matched, "type", type_str);
                        }
                        if (icon_str[0]) {
                            cJSON_DeleteItemFromObject(matched, "icon");
                            cJSON_AddStringToObject(matched, "icon", icon_str);
                        }
                        cJSON_DeleteItemFromObject(matched, "order");
                        cJSON_AddNumberToObject(matched, "order", order_val);

                        cJSON_DeleteItemFromObject(matched, "deviceIds");
                        cJSON_AddItemToObject(matched, "deviceIds", dev_ids_arr);

                        cJSON_DeleteItemFromObject(matched, "startSequence");
                        cJSON_AddItemToObject(matched, "startSequence", enter_steps);

                        cJSON_DeleteItemFromObject(matched, "stopSequence");
                        cJSON_AddItemToObject(matched, "stopSequence", leave_steps);
                    } else {
                        matched = cJSON_CreateObject();
                        char new_id_str[16];
                        if (act_id_str[0]) {
                            strncpy(new_id_str, act_id_str, sizeof(new_id_str) - 1);
                            new_id_str[sizeof(new_id_str) - 1] = '\0';
                        } else {
                            int cand = 1;
                            for (; cand <= 99; cand++) {
                                char cand_s[8];
                                snprintf(cand_s, sizeof(cand_s), "%d", cand);
                                int used = 0;
                                cJSON *chk = NULL;
                                cJSON_ArrayForEach(chk, acts) {
                                    cJSON *cid = cJSON_GetObjectItemCaseSensitive(chk, "id");
                                    if (cid && cJSON_IsString(cid) && cid->valuestring) {
                                        if (strcmp(cid->valuestring, cand_s) == 0) { used = 1; break; }
                                    }
                                }
                                if (!used) break;
                            }
                            snprintf(new_id_str, sizeof(new_id_str), "%d", cand);
                        }
                        cJSON_AddStringToObject(matched, "id", new_id_str);
                        cJSON_AddStringToObject(matched, "name", name_str[0] ? name_str : "Custom Activity");
                        cJSON_AddStringToObject(matched, "type", type_str);
                        cJSON_AddStringToObject(matched, "icon", icon_str);
                        cJSON_AddNumberToObject(matched, "order", order_val);
                        cJSON_AddItemToObject(matched, "deviceIds", dev_ids_arr);
                        cJSON_AddItemToObject(matched, "startSequence", enter_steps);
                        cJSON_AddItemToObject(matched, "stopSequence", leave_steps);
                        cJSON_AddItemToArray(acts, matched);
                    }

                    if (config_store_save_activities(root) == 0) {
                        success = 1;
                    } else {
                        cJSON_Delete(root);
                    }
                } else {
                    cJSON_Delete(dev_ids_arr);
                    cJSON_Delete(enter_steps);
                    cJSON_Delete(leave_steps);
                    cJSON_Delete(root);
                }
            } else {
                cJSON_Delete(dev_ids_arr);
                cJSON_Delete(enter_steps);
                cJSON_Delete(leave_steps);
            }

            cJSON_Delete(params);
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        cJSON_AddStringToObject(resp, "message", success ? "Activity saved successfully" : "Failed to save activity");
        send_cjson_resp(client_fd, success ? 200 : 400, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/activity-test-sequence */
    if (strcmp(clean_path, "/api/activity-test-sequence") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int executed = 0;
        if (params) {
            cJSON *p_steps = cJSON_GetObjectItemCaseSensitive(params, "steps");
            if (!p_steps) p_steps = cJSON_GetObjectItemCaseSensitive(params, "sequence");
            cJSON *step_arr = NULL;
            if (p_steps && cJSON_IsArray(p_steps)) step_arr = p_steps;
            else if (p_steps && cJSON_IsString(p_steps)) step_arr = build_activity_steps_cjson(p_steps->valuestring);

            if (step_arr && cJSON_IsArray(step_arr)) {
                cJSON *st = NULL;
                cJSON_ArrayForEach(st, step_arr) {
                    cJSON *jtype = cJSON_GetObjectItemCaseSensitive(st, "type");
                    const char *type = (jtype && cJSON_IsString(jtype)) ? jtype->valuestring : "";

                    if (strcasecmp(type, "Switched") == 0 || strcasecmp(type, "ActivitySwitched") == 0) {
                        executed++;
                        continue;
                    }

                    cJSON *jdelay = cJSON_GetObjectItemCaseSensitive(st, "delay");
                    int dms = (jdelay && cJSON_IsNumber(jdelay)) ? (int)jdelay->valuedouble : 0;

                    if (strcasecmp(type, "Delay") == 0 || dms > 0) {
                        if (dms > 3000) dms = 3000;
                        if (dms > 0) usleep(dms * 1000);
                        executed++;
                        if (strcasecmp(type, "Delay") == 0) continue;
                    }

                    cJSON *jdev = cJSON_GetObjectItemCaseSensitive(st, "deviceId");
                    char dev_buf[64] = {0};
                    if (jdev && cJSON_IsNumber(jdev)) snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)jdev->valuedouble);
                    else if (jdev && cJSON_IsString(jdev)) strncpy(dev_buf, jdev->valuestring, sizeof(dev_buf) - 1);

                    cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(st, "command");
                    const char *cmd = (jcmd && cJSON_IsString(jcmd)) ? jcmd->valuestring : "";

                    if (dev_buf[0] && cmd[0]) {
                        hw_device_command_send(dev_buf, cmd);
                        executed++;
                    }
                }
            }
            if (step_arr && step_arr != p_steps) cJSON_Delete(step_arr);
            cJSON_Delete(params);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddNumberToObject(resp, "executed", executed);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/activity-delete */
    if (strcmp(clean_path, "/api/activity-delete") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int success = 0;
        if (params) {
            cJSON *p_id = cJSON_GetObjectItemCaseSensitive(params, "id");
            if (p_id) {
                const char *act_id_str = cJSON_IsString(p_id) ? p_id->valuestring : "";
                char id_buf[32] = {0};
                if (!act_id_str[0] && cJSON_IsNumber(p_id)) {
                    snprintf(id_buf, sizeof(id_buf), "%ld", (long)p_id->valuedouble);
                    act_id_str = id_buf;
                }
                cJSON *root = config_store_get_activities_json();
                if (root) {
                    cJSON *acts = cJSON_GetObjectItemCaseSensitive(root, "activities");
                    if (acts && cJSON_IsArray(acts)) {
                        int count = cJSON_GetArraySize(acts);
                        for (int i = 0; i < count; i++) {
                            cJSON *item = cJSON_GetArrayItem(acts, i);
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
                            char item_id[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(item_id, sizeof(item_id), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(item_id, jid->valuestring, sizeof(item_id) - 1);
                            if (strcmp(item_id, act_id_str) == 0) {
                                cJSON_DeleteItemFromArray(acts, i);
                                config_store_save_activities(root);
                                success = 1;
                                break;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(params);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    return 0;
}
