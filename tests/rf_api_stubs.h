/* Link stubs for the parts of hub_rf that rf_state.c and rf_api.c call (shared by the unit tests
 * that link those two files: test_rf_status_json.c, test_rf_model.c, rf_status_sample.c) */
#ifndef RF_API_STUBS_H
#define RF_API_STUBS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool is_ota_busy(void) { return false; }
bool is_slot_direct_mode(int slot) { (void)slot; return false; }
const char *get_current_activity(void) { return "-1"; }
void start_pairing_mode(int slot) { (void)slot; }
void stop_pairing_mode(void) {}
void unpair_device(int slot) { (void)slot; }
uint8_t g_tx_seq = 0;
static int g_radio_writes __attribute__((unused)) = 0;
void rf_write_cmd(const uint8_t *b, size_t l) { (void)b; (void)l; g_radio_writes++; }
int rf_send_frame(uint8_t pipe, uint8_t msg_type, uint8_t flags, const uint8_t *payload, size_t len) {
    (void)pipe; (void)msg_type; (void)flags; (void)payload; (void)len; return 0;
}
int rf_send_frame_acked(uint8_t pipe, uint8_t msg_type, uint8_t flags, const uint8_t *payload, size_t len, int retries) {
    (void)pipe; (void)msg_type; (void)flags; (void)payload; (void)len; (void)retries; return 0;
}
void rf_send_hub_activity_sync(void) {}
void rf_send_settings_push_slot(uint8_t slot, int bright, int timeout, int touch, int gyro,
                               int nav_key, int nav_mode, int confirm_act, int touch_sens,
                               int remote_mode, int gyro_rate, int btn_backlight,
                               int night_start, int night_end, int usbeth, int no_screen,
                               int cap_buttons, int power_debug, int language, int theme, int layout) {
    (void)slot; (void)bright; (void)timeout; (void)touch; (void)gyro;
    (void)nav_key; (void)nav_mode; (void)confirm_act; (void)touch_sens;
    (void)remote_mode; (void)gyro_rate; (void)btn_backlight;
    (void)night_start; (void)night_end; (void)usbeth; (void)no_screen;
    (void)cap_buttons; (void)power_debug; (void)language; (void)theme; (void)layout;
}
void rf_notify_update_available(int slot) { (void)slot; }
void rf_push_custom_icon(const char *id, const char *type) { (void)id; (void)type; }
void record_config_diff(void) {}
void rf_sync_cache_invalidate(void) {}
int rf_wake_remote(uint8_t slot, int attempts) { (void)slot; (void)attempts; return 0; }
void *config_push_worker(void *arg) { (void)arg; return NULL; }
void *rf_ota_sender_worker(void *arg) { (void)arg; return NULL; }
void rf_keymap_invalidate_devices_cache(void) {}
void rf_keymap_invalidate_activities_cache(void) {}
void rf_keymap_invalidate_map(uint8_t slot) { (void)slot; }

#endif
