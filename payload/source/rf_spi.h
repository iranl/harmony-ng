#ifndef HARMONY_RF_SPI_H
#define HARMONY_RF_SPI_H

#include "rf_state.h"

#ifdef __cplusplus
extern "C" {
#endif

int rf_spi_open(void);
void rf_spi_close(void);
int flash_cc2544(void);
int rf_write_cmd(const uint8_t *cmd, size_t len);
/* One RF_OTA_DATA frame (windowed OTA): 26-byte chunk idx, ack_req sets the ACK request bit */
int rf_send_ota_data(uint8_t slot, uint32_t idx, bool ack_req, const uint8_t *data, size_t len);
/* CC2544 power mode (RF_PM_ACTIVE / RF_PM_TRANSFER); writes only on change */
int rf_set_power_mode(uint8_t mode);
int rf_send_frame(rf_frame_t *frame);
/* Sends the frame and waits for the remote's ACK (0) or the timeout (-1). One ACKed send runs at
 * a time; the ACK's status and payload are in g_ack_status and g_ack_payload of the calling thread. */
int rf_send_frame_acked(rf_frame_t *frame, int timeout_ms);
int rf_wake_remote(uint8_t slot, int attempts);
/* An ACK received from slot (0 = not known) for the frame with sequence number seq */
void rf_signal_ack_from(uint8_t slot, uint8_t seq, uint8_t status, const uint8_t *payload);
void rf_signal_ack(uint8_t seq, uint8_t status, const uint8_t *payload);
void query_paired_devices(void);
void *rx_worker(void *arg);
/* Read errors in a row (100 ms apart) after which rx_worker() sets g_rf_read_failed and stops
 * hub_rf (g_running false): main() then exits with 1 and the supervisor restarts it */
#define RF_READ_ERRORS_MAX 50
extern volatile bool g_rf_read_failed;

/* Top-level frame handler defined in rf_main.c */
void handle_rf_frame(const rf_frame_t *f);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RF_SPI_H */
