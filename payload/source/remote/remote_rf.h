#ifndef HARMONY_REMOTE_RF_H
#define HARMONY_REMOTE_RF_H

#include "remote_state.h"

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
void cc2544_clear_bonding(void);
void rf_perform_unpair_cleanup(bool notify_hub);
void rf_unpair(void);
void send_battery_status_rf(void);
void send_version_report_rf(void);
/* The remote's factory unit id (mfg partition), read once at start; empty when unreadable */
extern char g_remote_unit_id[32];
extern uint8_t g_remote_skin_id;   /* "skin" record of the mfg partition, 0 when missing */
/* RF_MSG_UNIT_ID with g_remote_unit_id; sent after every version report */
void send_unit_id_rf(void);
void send_remote_config_backup_rf(void);
void rf_send_activity_start(const char *act_id);
void rf_send_device_command(const char *dev_id, const char *cmd);
void rf_send_help_request(const char *act_id);
void rf_send_help_answer(const char *dev_id, bool on);
void rf_send_screen_button_press(const char *act_id, uint8_t btn_idx, const char *label);
int build_combined_activity_sequence(const char *from_id, const char *to_id, rf_act_step_t *out_steps, int max_steps, uint32_t *out_total_ms);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_RF_H */
