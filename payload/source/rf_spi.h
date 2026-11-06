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
int rf_send_frame_acked(rf_frame_t *frame, int timeout_ms);
int rf_wake_remote(uint8_t slot, int attempts);
void rf_signal_ack(uint8_t seq, uint8_t status, const uint8_t *payload);
void query_paired_devices(void);
void *rx_worker(void *arg);

/* Top-level frame handler defined in rf_main.c */
void handle_rf_frame(const rf_frame_t *f);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RF_SPI_H */
