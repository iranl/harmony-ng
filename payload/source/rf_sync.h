#ifndef RF_SYNC_H
#define RF_SYNC_H

#include "rf_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void publish_remote_battery_mqtt(uint8_t slot, uint8_t pct, uint8_t chg, uint16_t mv);
void record_config_diff(int slot, const char *old_json_str, cJSON *norm);
void save_remote_config_backup(uint8_t slot, const uint8_t *p);
void rf_build_settings_push(rf_frame_t *f, uint8_t slot, int bright, int timeout, int touch, int gyro,
                            int nav_key, int nav_mode, int confirm_act, int touch_sens,
                            int remote_mode, int gyro_rate, int btn_backlight,
                            int night_start, int night_end, int usbeth, int no_screen,
                            int cap_buttons, int power_debug);
void rf_send_settings_push_slot(uint8_t slot, int bright, int timeout, int touch, int gyro,
                               int nav_key, int nav_mode, int confirm_act, int touch_sens,
                               int remote_mode, int gyro_rate, int btn_backlight,
                               int night_start, int night_end, int usbeth, int no_screen,
                               int cap_buttons, int power_debug);
void rf_send_hub_activity_sync_slot(const char *act_id, uint8_t seq, uint8_t slot, uint8_t is_query_reply);
void rf_send_hub_activity_sync_seq(const char *act_id, uint8_t seq);
void rf_send_hub_activity_sync(const char *act_id);
uint8_t rf_check_update_flags(uint8_t slot);
void rf_notify_update_available(uint8_t target_slot);
void rf_send_hub_time_sync_slot(uint8_t slot);
void rf_send_hub_time_sync(void);
int rf_push_custom_icon(int target_slot, const char *type_str, const char *id_str);
void *activity_sync_worker(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* RF_SYNC_H */
