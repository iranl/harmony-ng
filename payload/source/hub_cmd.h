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

/* POST /api/ir-raw body -> hw_ir_send_raw. Returns the HTTP code and the reply object in *reply. */
int hub_cmd_ir_raw(const cJSON *req, cJSON **reply);

#ifdef __cplusplus
}
#endif

#endif /* HUB_CMD_H */
