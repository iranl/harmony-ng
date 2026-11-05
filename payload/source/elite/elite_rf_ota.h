#ifndef HARMONY_ELITE_RF_OTA_H
#define HARMONY_ELITE_RF_OTA_H

#include "elite_state.h"

#ifdef __cplusplus
extern "C" {
#endif

bool handle_ota_frame(const rf_frame_t *f);
void *ota_poll_worker(void *arg);
bool handle_icon_frame(const rf_frame_t *f);
void render_ota_overlay(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_ELITE_RF_OTA_H */
