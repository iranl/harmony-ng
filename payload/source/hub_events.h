#ifndef HUB_EVENTS_H
#define HUB_EVENTS_H

/*
 * Event bus of hub_daemon.
 *
 * Any thread emits (the orchestrator on the main loop, HTTP workers, the state watcher thread);
 * the main loop dispatches once per iteration, so every sink runs single-threaded, which is what
 * ws_broadcast_text and the MQTT client expect. The bus adds "type", "seq" and "ts" to each
 * payload before the sinks see it and keeps the last payload per type for the snapshot.
 */

#include <stdint.h>
#include <time.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HUB_EV_ACTIVITY_STATE,
    HUB_EV_ACTIVITY_PROGRESS,
    HUB_EV_CONFIG_CHANGED,
    HUB_EV_BT_STATUS,
    HUB_EV_RF_STATUS,
    HUB_EV_BUTTON,
    HUB_EV_COUNT
} hub_event_type_t;

typedef struct {
    hub_event_type_t type;
    uint32_t seq;        /* monotonically increasing per daemon run, starts at 1 */
    time_t ts;
    cJSON *payload;      /* object; owned by the bus, valid during dispatch only */
} hub_event_t;

typedef void (*hub_event_sink_fn)(const hub_event_t *ev, void *user);

#define HUB_EVENTS_MAX_SINKS 4
#define HUB_EVENTS_RING 32

int  hub_events_init(void);
/* Up to HUB_EVENTS_MAX_SINKS sinks, called in subscription order. Returns 0 or -1 when full. */
int  hub_events_subscribe(hub_event_sink_fn fn, void *user);
/* Queues an event; takes ownership of payload (an object, or NULL for an empty object). Any thread.
 * On a full ring the oldest queued event is dropped and counted. */
void hub_events_emit(hub_event_type_t type, cJSON *payload);
/* Pops every queued event and calls the sinks. Main loop only. */
void hub_events_dispatch(void);
const char *hub_event_type_name(hub_event_type_t type);         /* "activity_state", ... */
uint32_t hub_events_last_seq(void);
/* Last dispatched payload of a type (with "type", "seq", "ts"), or NULL. Main loop only. */
const cJSON *hub_events_last(hub_event_type_t type);
/* Events dropped so far because the ring was full */
uint32_t hub_events_overflows(void);

#ifdef __cplusplus
}
#endif

#endif /* HUB_EVENTS_H */
