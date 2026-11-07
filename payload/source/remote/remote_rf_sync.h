#ifndef HARMONY_REMOTE_RF_SYNC_H
#define HARMONY_REMOTE_RF_SYNC_H

#include "remote_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void handle_hub_frame_remote(const rf_frame_t *f);
void *rf_rx_worker(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_RF_SYNC_H */
