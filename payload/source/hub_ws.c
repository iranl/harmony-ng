#define _GNU_SOURCE
#include "hub_ws.h"
#include "ws_server.h"
#include "hub_events.h"
#include "hub_info.h"
#include "http_auth.h"
#include "hub_cmd.h"
#include "state_watch.h"
#include "config_store.h"
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
cJSON *hub_ws_snapshot_object(int msg_id) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "type", "snapshot");
    if (msg_id) cJSON_AddNumberToObject(root, "msgId", msg_id);

    cJSON *hub = cJSON_CreateObject();
    struct hub_settings cs;
    config_store_get_settings(&cs);
    cJSON_AddStringToObject(hub, "hub_id", hub_info_id());
    cJSON_AddStringToObject(hub, "hub_name", cs.hub_name[0] ? cs.hub_name : "Harmony Hub");
    hub_info_add_status_fields(hub, &cs);
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
    const cJSON *mq = hub_events_last(HUB_EV_MQTT_STATUS);
    if (mq) cJSON_AddItemToObject(root, "mqtt_status", cJSON_Duplicate(mq, 1));
    else cJSON_AddNullToObject(root, "mqtt_status");

    cJSON *cfg = cJSON_CreateObject();
    cJSON *files = state_watch_config_mtimes();
    if (files) cJSON_AddItemToObject(cfg, "files", files);
    cJSON_AddItemToObject(root, "config", cfg);

    cJSON_AddNumberToObject(root, "seq", (double)hub_events_last_seq());
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    return root;
}

static void send_snapshot(int cfd, int msg_id) {
    cJSON *root = hub_ws_snapshot_object(msg_id);
    if (!root) return;
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
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

    /* WebSocket clients and MQTT get it from the bus (their sinks) */
    hub_events_emit(HUB_EV_ACTIVITY_PROGRESS, root);
}

void handle_ws_client_message(int cfd) {
    char buf[4096];
    int opcode = 0;
    int len = ws_read_frame(cfd, buf, sizeof(buf), &opcode);
    if (len == WS_READ_AGAIN) return; /* nothing to read after all: keep the client */
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

    /* Every action runs through the shared executor (same actions and codes as the MQTT
     * command channel); get_snapshot is answered here so the frame goes to this client only */
    cJSON *resp = hub_cmd_execute(root, ws_get_client_role(cfd));
    cJSON_Delete(root);
    if (!resp) return;
    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (str) {
        if (ws_send_text(cfd, str) != 0) ws_remove_client(cfd);
        free(str);
    }
}
