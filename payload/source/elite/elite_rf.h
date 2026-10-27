#ifndef HARMONY_ELITE_RF_H
#define HARMONY_ELITE_RF_H

#include "elite_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void rf_wake_and_reconnect(void);
int rf_send_frame(rf_frame_t *frame);
void rf_send_button_event(uint16_t key_code, int value);
void rf_send_sync_query(void);
void rf_send_full_sync_request(void);
void rf_start_pairing(void);
void rf_stop_pairing(void);
void rf_unpair(void);
void send_battery_status_rf(void);
void send_version_report_rf(void);
void send_remote_config_backup_rf(void);
void rf_send_activity_start(const char *act_id);
void rf_send_device_command(const char *dev_id, const char *cmd);
void rf_send_screen_button_press(const char *act_id, uint8_t btn_idx, const char *label);
int build_combined_activity_sequence(const char *from_id, const char *to_id, rf_act_step_t *out_steps, int max_steps, uint32_t *out_total_ms);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_ELITE_RF_H */
