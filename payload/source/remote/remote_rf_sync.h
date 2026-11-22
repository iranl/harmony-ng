#ifndef HARMONY_REMOTE_RF_SYNC_H
#define HARMONY_REMOTE_RF_SYNC_H

#include "remote_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void handle_hub_frame_remote(const rf_frame_t *f);
/* Main loop: ends a streamed config push that went quiet (radio back to RF_PM_ACTIVE) */
void remote_stream_tick(uint64_t now_ms);
void rf_handle_pairing_event(const uint8_t pkt[7]);
void *rf_rx_worker(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_RF_SYNC_H */
