#ifndef HUB_WS_H
#define HUB_WS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "orchestrator.h"
#include "hub_events.h"

/* Event bus sink that broadcasts every event to the WebSocket clients */
void hub_ws_event_sink(const hub_event_t *ev, void *user);
/* Main loop, once per iteration after hub_events_dispatch(): sends the snapshot to clients that
 * upgraded since the last call (their first frame) */
void hub_ws_tick(void);

void on_orch_progress(const char *activity_id, orch_state_t state,
                      int current_step, int total_steps,
                      const char *step_desc, void *user_data);
void handle_ws_client_message(int cfd);

#ifdef __cplusplus
}
#endif

#endif /* HUB_WS_H */
