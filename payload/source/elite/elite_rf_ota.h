#ifndef HARMONY_ELITE_RF_OTA_H
#define HARMONY_ELITE_RF_OTA_H

#include "elite_state.h"

#ifdef __cplusplus
extern "C" {
#endif

bool handle_ota_frame(const rf_frame_t *f);
/* A raw 32-byte RF_OTA_DATA frame (header 0x60) from the Hub, windowed transfer; OTA mode only */
void handle_ota_data_frame(const uint8_t *pkt);
/* Enter or leave OTA mode: sets g_is_ota_updating and the CC2544 power mode */
void ota_set_updating(bool on);
bool handle_icon_frame(const rf_frame_t *f);
void render_ota_overlay(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_ELITE_RF_OTA_H */
