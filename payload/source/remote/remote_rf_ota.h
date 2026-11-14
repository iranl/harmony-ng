#ifndef HARMONY_REMOTE_RF_OTA_H
#define HARMONY_REMOTE_RF_OTA_H

#include "remote_state.h"

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
/* Install helpers (exposed for tests). ota_stage_file copies src to dst, a new file on the
 * file system of the install target, after checking that dst's directory keeps free_margin bytes
 * free, and fsyncs it; ota_replace_file renames new_path over path (with bak_path, when given,
 * as a hard link to the old file). Both return 0 on success, -1 on failure (dst removed). */
int ota_sync_file(const char *path);
int ota_stage_file(const char *src, const char *dst, uint64_t free_margin);
int ota_replace_file(const char *new_path, const char *path, const char *bak_path);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_RF_OTA_H */
