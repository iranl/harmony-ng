/* Host implementation of the remote's hardware interfaces for the screen renderer.
 * Drawing code only reads state; the RF, power and display calls it can reach do nothing here. */
#include "remote_state.h"
#include "remote_power.h"
#include "remote_rf.h"
#include "remote_present.h"

char g_timezone_str[64] = "UTC0";
time_t g_host_wall_time = 0;

time_t ui_wall_time(void) { return g_host_wall_time ? g_host_wall_time : time(NULL); }
void read_remote_battery(void) {}
bool is_remote_docked(void) { return strcmp(g_battery_info.powersource, "usb") == 0; }
void expand_timezone(const char *in, char *out, size_t out_sz) { snprintf(out, out_sz, "%s", in); }
void fb_set_blank(bool blank) { (void)blank; }
bool fb_present(const uint16_t *frame, bool wait_vsync) { (void)frame; (void)wait_vsync; return false; }
bool fb_can_animate(void) { return false; }
bool frame_timing_enabled(void) { return false; }
void frame_timing_record(const frame_timing_t *t) { (void)t; }
void set_backlight_level(int level) { (void)level; }
void trigger_manual_suspend_to_ram(void) {}
void init_gyro_interrupt(void) {}

int build_combined_activity_sequence(const char *from_id, const char *to_id, rf_act_step_t *out_steps,
                                     int max_steps, uint32_t *out_total_ms) {
    (void)from_id; (void)to_id; (void)out_steps; (void)max_steps;
    if (out_total_ms) *out_total_ms = 0;
    return 0;
}
int rf_send_frame(rf_frame_t *frame) { (void)frame; return 0; }
void rf_send_activity_start(const char *act_id) { (void)act_id; }
void rf_send_help_request(const char *act_id) { (void)act_id; }
void rf_send_help_answer(const char *dev_id, bool on) { (void)dev_id; (void)on; }
void rf_send_full_sync_request(void) {}
void rf_send_screen_button_press(const char *act_id, uint8_t btn_idx, const char *label) {
    (void)act_id; (void)btn_idx; (void)label;
}
void rf_send_sync_query(void) {}
void rf_start_pairing(void) {}
void rf_stop_pairing(void) {}
void rf_unpair(void) {}
void send_remote_config_backup_rf(void) {}
void send_version_report_rf(void) {}
