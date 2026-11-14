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
/* ota_replace_file for the binary, except that bak_path is kept as it is when self_exe (the
 * running binary, /proc/self/exe) is that backup file */
int ota_install_binary(const char *new_path, const char *path, const char *bak_path, const char *self_exe);
/* Files of a package installed next to the binary (rcS.local, *.sh, remote_sync): each target
 * gets <dst>.new by ota_stage_files (all or none: a failure removes the ones already staged) and
 * then ota_commit_files renames them over their targets, keeping the old one as <dst>.bak.
 * ota_package_files lists them (returns the count, -1 on error). */
#define OTA_PATH_MAX 160
#define OTA_MAX_PKG_FILES 24
typedef struct {
    char src[OTA_PATH_MAX];
    char dst[OTA_PATH_MAX];
} ota_file_t;
int ota_package_files(const char *pkg_dir, const char *data_dir, const char *initd_dir, ota_file_t *files, int max);
int ota_stage_files(const ota_file_t *files, int n, uint64_t free_margin);
int ota_commit_files(const ota_file_t *files, int n);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_RF_OTA_H */
