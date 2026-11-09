#ifndef HUB_CMD_H
#define HUB_CMD_H

/*
 * Command executor shared by the WebSocket actions (hub_ws.c) and the MQTT command channel
 * (hub_mqtt.c, spec 5.3): one request object in, one reply object out, same actions, fields,
 * codes and errors on both transports.
 */

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Executes req->action. role: enum http_role value (1 user, 2 admin); a user role is limited to
 * the actions its REST equivalents allow. Returns a new reply object: "type" is "pong",
 * "activity_state", "snapshot" or "ack"; an ack carries "action", "ok", and on failure "code"
 * (HTTP-style) and "error". The request's "msgId" (number) or "id" (string) is echoed. */
cJSON *hub_cmd_execute(const cJSON *req, int role);

/* Where a request came from, for a reply that comes later: HUB_CMD_ORIGIN_MQTT, a WebSocket
 * client's socket, or HUB_CMD_ORIGIN_NONE (the caller waits: slow actions run inline). */
#define HUB_CMD_ORIGIN_NONE  (-2)
#define HUB_CMD_ORIGIN_MQTT  (-1)

/* As hub_cmd_execute, but a slow action (update_check, which runs update.sh or a download for
 * seconds) runs on a thread and returns NULL: its reply is delivered by hub_cmd_poll() to the
 * origin. Only one update check runs at a time; another request meanwhile gets 409. */
cJSON *hub_cmd_execute_from(const cJSON *req, int role, int origin);

/* Main loop: delivers the replies of finished slow actions (MQTT <base>/cmd/response, or the
 * WebSocket client if it is still connected) */
void hub_cmd_poll(void);

/* POST /api/ir-raw body -> hw_ir_send_raw. Returns the HTTP code and the reply object in *reply. */
int hub_cmd_ir_raw(const cJSON *req, cJSON **reply);

/* POST /api/ir-capture body {"enabled": bool} -> the IR capture mode (spec 3.14): on, every
 * learned signal is emitted as an ir_received event. Returns the HTTP code and the reply. */
int hub_cmd_ir_capture(const cJSON *req, cJSON **reply);
/* ir_i2s capture-state listener: emits HUB_EV_IR_CAPTURE (registered by hub_daemon at start) */
void hub_cmd_ir_capture_state_changed(void *user);

#ifdef __cplusplus
}
#endif

#endif /* HUB_CMD_H */
