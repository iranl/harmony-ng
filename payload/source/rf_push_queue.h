#ifndef RF_PUSH_QUEUE_H
#define RF_PUSH_QUEUE_H

#include "rf_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One remote's config push, built in full before sending so that CONFIG_END clear can
   tell the remote how many chunks to expect. */
typedef struct {
    rf_frame_t *frames;
    int count, cap;
    bool oom;
} push_queue_t;

void push_queue_add(push_queue_t *q, const rf_frame_t *f);
int push_queue_send(uint8_t slot, uint8_t config_ver, push_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif /* RF_PUSH_QUEUE_H */
