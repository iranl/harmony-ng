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
int rf_send_frame(rf_frame_t *frame);
int rf_send_frame_acked(rf_frame_t *frame, int timeout_ms);
void rf_signal_ack(uint8_t seq, uint8_t status);
void query_paired_devices(void);
void *rx_worker(void *arg);

/* Top-level frame handler defined in rf_main.c */
void handle_rf_frame(const rf_frame_t *f);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RF_SPI_H */
