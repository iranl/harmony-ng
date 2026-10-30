#include "elite_state.h"

/* UI Views & Remote Modes */
ui_view_t g_current_view = VIEW_ACTIVITIES;
settings_subview_t g_settings_subview = SETTINGS_VIEW_CATEGORIES;
bool g_no_screen_mode = false;
int g_log_level = LOG_LEVEL_INFO;

/* RF Link & Pairing State */
volatile bool g_rf_link_connected = false;
volatile int g_pairing_state = 0; /* 0=idle, 1=pairing, 2=paired, 3=failed */
uint8_t g_paired_hub_addr[4] = {0};
uint8_t g_remote_own_addr[4] = {0};
bool g_unpair_confirm_active = false;

/* Hub & OTA Update Notification State */
bool g_update_config_available = false;
bool g_update_ota_available = false;
uint8_t g_hub_config_version = 0;
char g_hub_ota_version[16] = "";

/* Activities & Devices */
activity_item_t g_activities[MAX_ACTIVITIES];
int g_activity_count = 0;
int g_selected_activity = 0;
char g_active_activity_id[32] = "-1";
char g_active_activity_name[48] = "PowerOff";
bool g_initial_sync_received = false;

int g_selected_device_cmd = 0;
rf_activity_t g_rf_activities[MAX_RF_ACTIVITIES];
int g_rf_activity_count = 0;
rf_device_t g_rf_devices[MAX_RF_DEVICES];
int g_rf_device_count = 0;
rf_dev_cmd_t g_rf_dev_cmds[MAX_RF_DEV_CMDS];
int g_rf_dev_cmd_count = 0;
int g_browse_dev = -1;
int g_browse_sel = 0;

/* Store or replace a command; a retried chunk must not add a duplicate */
void store_rf_dev_cmd(uint8_t dev_idx, uint8_t cmd_idx, const char *name) {
    int i = 0;
    while (i < g_rf_dev_cmd_count &&
           (g_rf_dev_cmds[i].dev_idx != dev_idx || g_rf_dev_cmds[i].cmd_idx != cmd_idx)) i++;
    if (i >= MAX_RF_DEV_CMDS) return;
    g_rf_dev_cmds[i].dev_idx = dev_idx;
    g_rf_dev_cmds[i].cmd_idx = cmd_idx;
    strncpy(g_rf_dev_cmds[i].name, name, sizeof(g_rf_dev_cmds[0].name) - 1);
    g_rf_dev_cmds[i].name[sizeof(g_rf_dev_cmds[0].name) - 1] = '\0';
    if (i == g_rf_dev_cmd_count) g_rf_dev_cmd_count++;
}
rf_button_t g_rf_buttons[MAX_RF_BUTTONS];
int g_rf_button_count = 0;
rf_act_step_t g_rf_steps[MAX_RF_STEPS];
int g_rf_step_count = 0;

uint8_t g_config_version = 0;
uint8_t g_remote_slot = 0x01;
bool g_has_hub_config = false;
uint8_t g_tx_seq = 0;

/* Activity switching lock overlay state */
volatile bool g_is_switching_activity = false;
char g_switching_target_id[16] = "-1";
char g_switching_target_name[48] = "PowerOff";
uint64_t g_switching_start_ms = 0;
uint32_t g_switching_total_ms = 3500;

/* Config Synchronization & RF OTA Lock overlay states */
volatile bool g_is_syncing_config = false;
uint64_t g_sync_start_ms = 0;
uint64_t g_sync_last_packet_ms = 0;
uint64_t g_sync_failed_ms = 0;
int g_sync_chunk_count = 0;
int g_sync_total = 0;
pthread_mutex_t g_sync_lock = PTHREAD_MUTEX_INITIALIZER;

volatile bool g_is_ota_updating = false;
uint64_t g_ota_start_ms = 0;
uint64_t g_ota_last_packet_ms = 0;
uint32_t g_ota_total_bytes = 0;
uint32_t g_ota_received_bytes = 0;
uint16_t g_ota_total_chunks = 0;
uint16_t g_ota_received_chunks = 0;
FILE *g_ota_file = NULL;

FILE *g_icon_rx_file = NULL;
uint32_t g_icon_rx_bytes = 0;
uint32_t g_icon_rx_tot_bytes = 0;
uint16_t g_icon_rx_chunks = 0;
uint8_t g_icon_rx_type = 0;
char g_icon_rx_id[32] = {0};

/* Settings & Navigation */
int g_selected_category = 0;
int g_selected_sub_item = 0;
int g_factory_reset_stage = 0;
uint64_t g_suspend_countdown_start_ms = 0;
int g_selected_pair_opt = 0;
bool g_touch_enabled = true;
bool g_cap_buttons_enabled = true;
int g_touch_sensitivity = 2;   /* 1 = Low, 2 = Normal, 3 = High */
uint64_t g_last_switch_completed_ms = 0;
char g_last_switch_target_id[16] = "";

activity_trigger_btn_t g_act_triggers[MAX_ACT_TRIGGERS];
int g_act_trigger_count = 0;

bool g_gyro_enabled = true;
int g_gyro_rate_hz = 25;       /* 10, 25, 50 Hz */
int g_btn_backlight_mode = 2;  /* 0 = OFF, 1 = ON, 2 = NIGHT_SCHEDULE */
int g_night_start_hour = 22;   /* 10 PM */
int g_night_end_hour = 7;      /* 7 AM */
int g_timeout_seconds = 10;
int g_brightness_val = 2; /* 1..5 */
bool g_confirm_activity = true;
int g_confirm_pending_idx = -1;
uint64_t g_confirm_pending_deadline_ms = 0;
int g_confirm_pending_rf_code = 0;
uint64_t g_confirm_pending_rf_deadline_ms = 0;
bool g_settings_dirty = false;

elite_battery_info_t g_battery_info = { 100, 0, 4100, "battery" };
bool g_screen_on = true;
bool g_screen_nav_mode = true;  /* Default true: bottom buttons navigate screen UI on boot */
int g_nav_toggle_key = 139;     /* Default: 139 (Menu), also 358 (Info), 395 (Guide), 389 (DVR), 174 (Exit) */
int g_nav_toggle_mode = 1;      /* 0 = Short Press, 1 = Long Press (Hold >= 500ms) */
bool g_usbeth_enabled = false;  /* Dynamic USB Ethernet mode */
bool g_usb_connected = false;   /* Dynamic physical USB cable detection */

volatile bool g_screen_dirty = true;
char g_notice_msg[64] = {0};
uint64_t g_notice_until_ms = 0;

uint64_t g_toggle_key_down_ms = 0;
bool g_toggle_key_triggered = false;

volatile bool g_running = true;
int g_fb_fd = -1;
uint16_t *g_fb_mem = NULL;
size_t g_fb_size = FB_SIZE;
int g_btn_fd = -1;
int g_touch_fd = -1;
int g_gyro_fd = -1;
int g_rf_fd = -1;
time_t g_last_activity_time = 0;
uint64_t s_last_blank_time_ms = 0;
pthread_mutex_t g_render_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_rf_write_mutex = PTHREAD_MUTEX_INITIALIZER;

int g_touch_cur_x = 0;
int g_touch_cur_y = 0;
int g_touch_down_x = 0;
int g_touch_down_y = 0;
bool g_touch_is_down = false;
bool g_touch_need_down_latch = false;
uint64_t g_touch_down_ms = 0;

uint64_t s_last_wake_ms = 0;
uint64_t s_last_reconnect_ms = 0;
volatile uint64_t g_last_hub_frame_ms = 0;

/* Power Debug Logging */
#define POWER_LOG_TMP "/tmp/power_debug.log"
static pthread_mutex_t g_power_log_mutex = PTHREAD_MUTEX_INITIALIZER;
bool g_power_debug = false;

uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

bool is_remote_paired(void) {
    if (g_rf_link_connected) return true;
    if (g_pairing_state == 2) return true;
    if (g_paired_hub_addr[0] || g_paired_hub_addr[1] || g_paired_hub_addr[2] || g_paired_hub_addr[3]) return true;
    if (access(RF_CONFIG_FILE, F_OK) == 0) return true;
    return false;
}


void log_power_debug(const char *fmt, ...) {
    if (!g_power_debug) return;
    pthread_mutex_lock(&g_power_log_mutex);
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_buf[32] = {0};
    if (tm) {
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);
    } else {
        snprintf(time_buf, sizeof(time_buf), "epoch=%ld", (long)now);
    }

    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    printf("[PWR %s] %s", time_buf, msg);
    fflush(stdout);

    FILE *ftmp = fopen(POWER_LOG_TMP, "a");
    if (ftmp) {
        fprintf(ftmp, "[%s] %s", time_buf, msg);
        fclose(ftmp);
    }
    pthread_mutex_unlock(&g_power_log_mutex);
}

bool has_configured_activities(void) {
    for (int i = 0; i < g_activity_count; i++) {
        if (strcmp(g_activities[i].id, "-1") != 0 && g_activities[i].id[0] != '\0') {
            return true;
        }
    }
    return false;
}

int get_active_dev_cmd_count(void) {
    if (!g_has_hub_config || g_rf_button_count == 0) {
        return 0;
    }
    const char *target_ctx = g_active_activity_id;
    int count = 0;
    for (int i = 0; i < g_rf_button_count; i++) {
        if (g_rf_buttons[i].context_type != 0x01) {
            continue;
        }
        if (strcmp(g_rf_buttons[i].context_id, target_ctx) == 0) {
            count++;
        }
    }
    return count;
}

int get_active_dev_cmd_item(int filtered_idx, char *out_label, size_t label_len, uint8_t *out_btn_index, uint8_t *out_atype, char *out_icon, size_t icon_len) {
    if (!g_has_hub_config || g_rf_button_count == 0) {
        return -1;
    }
    const char *target_ctx = g_active_activity_id;
    int cur = 0;
    for (int i = 0; i < g_rf_button_count; i++) {
        if (g_rf_buttons[i].context_type != 0x01) {
            continue;
        }
        if (strcmp(g_rf_buttons[i].context_id, target_ctx) == 0) {
            if (cur == filtered_idx) {
                if (out_label && label_len > 0) {
                    strncpy(out_label, g_rf_buttons[i].label, label_len - 1);
                    out_label[label_len - 1] = '\0';
                }
                if (out_btn_index) *out_btn_index = g_rf_buttons[i].btn_index;
                if (out_atype) *out_atype = g_rf_buttons[i].action_type;
                if (out_icon && icon_len > 0) {
                    strncpy(out_icon, g_rf_buttons[i].icon_key, icon_len - 1);
                    out_icon[icon_len - 1] = '\0';
                }
                return 0;
            }
            cur++;
        }
    }
    return -1;
}
