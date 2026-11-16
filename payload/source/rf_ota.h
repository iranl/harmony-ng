#ifndef HARMONY_RF_OTA_H
#define HARMONY_RF_OTA_H

#include "rf_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void ota_set_progress(uint32_t sent_bytes, uint16_t sent_chunks);
void ota_set_status(int status, bool in_prog, const char *err);
void ota_note_chunk_ack(uint8_t slot, const uint8_t *payload); /* every RF_MSG_ACK; uses windowed OTA chunk ACKs of the remote being updated only */
void *rf_ota_sender_worker(void *arg);
bool rf_ota_verify_manifest(const uint8_t *digest, char *err_buf, size_t err_sz);
/* The update source for a remote image download: source= of update_state.conf when it fits
 * out_sz and has only URL characters the update API accepts, else the project's default */
void get_ota_repo_source(char *out_repo, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RF_OTA_H */
