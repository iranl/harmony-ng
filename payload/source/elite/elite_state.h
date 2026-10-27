#ifndef HARMONY_ELITE_STATE_H
#define HARMONY_ELITE_STATE_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/reboot.h>
#include <stdarg.h>
#include <linux/input.h>
#include <linux/fb.h>
#include <linux/i2c-dev.h>

#include "cJSON.h"
#include "rf_proto.h"
#include "font_aa.h"
#include "harmony_version.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FB_DEV                  "/dev/fb0"
#define FB_BLANK_SYSFS          "/sys/class/graphics/fb0/blank"
#define BACKLIGHT_SYSFS         "/sys/class/backlight/ap3156_backlight/brightness"
#define BTN_LED_SYSFS           "/sys/class/leds/tps6507x:white/brightness"
#define BTN_EVENT_DEV           "/dev/input/event0"
#define GYRO_EVENT_DEV          "/dev/input/event1"
#define TOUCH_EVENT_DEV         "/dev/input/event2"
#define GYRO_DATA_RATE_SYS      "/sys/devices/platform/i2c_davinci.1/i2c-1/1-0019/data_rate"
#define GYRO_INT1_THRESHOLD_SYS "/sys/devices/platform/i2c_davinci.1/i2c-1/1-0019/int1_threshold"
#define GYRO_INT1_DURATION_SYS  "/sys/devices/platform/i2c_davinci.1/i2c-1/1-0019/int1_duration"
#define GYRO_XYZ_SYS            "/sys/devices/platform/i2c_davinci.1/i2c-1/1-0019/xyz"
#define RFSPI_DEV               "/dev/rfspi"
#define ACTIVITY_JSON_FILE        "/data/activities.json"
#define STOCK_ACTIVITY_JSON_FILE  "/data/resources/ActivityList.json"
#define RF_CONFIG_FILE            "/data/elite_ui.conf"

#define FB_WIDTH            240
#define FB_HEIGHT           320
#define FB_SIZE             (FB_WIDTH * FB_HEIGHT * 2)

/* Logitech Elite RGB565 Palette - High Contrast */
#define RGB_BLACK           0x0000
#define RGB_BG              0x0000  /* Pure deep black background for OLED/LCD contrast */
#define RGB_CARD            0x10A2  /* Dark slate card body #10141A */
#define RGB_CARD_SEL        0x1945  /* Highlighted card surface #182835 */
#define RGB_CARD_BORDER     0x2965  /* Crisp card border #282C34 */
#define RGB_WHITE           0xFFFF  /* Pure white for primary titles */
#define RGB_TEXT_MUTED      0xD6DA  /* Crisp light gray for secondary info */
#define RGB_CYAN            0x067A  /* Vibrant Logitech Teal #00CFB4 */
#define RGB_TEAL            0x067A  /* Vibrant Logitech Teal #00CFB4 */
#define RGB_GREEN           0x27E8  /* Crisp success green #20FA80 */
#define RGB_RED             0xFA08  /* Vivid alert / power red #F84040 */
#define RGB_YELLOW          0xFFE0  /* Crisp warning yellow */
#define RGB_BLUE            0x2D7F  /* Clean highlight blue */
#define RGB_HEADER          0x0861  /* Sleek top header bar */
#define RGB_DIVIDER         0x2965  /* Crisp divider line */
#define RGB_TAB_BG          0x0841  /* Bottom dock background */
#define RGB_TAB_ACTIVE      0x1945  /* Active bottom tab surface */

/* UI Views */
typedef enum {
    VIEW_ACTIVITIES = 0,
    VIEW_DEVICES,
    VIEW_SETTINGS,
    VIEW_PAIRING,
    VIEW_NOT_PAIRED,
    VIEW_COUNT
} ui_view_t;

typedef enum {
    SETTINGS_VIEW_CATEGORIES = 0,
    SETTINGS_VIEW_ACTIVITIES,
    SETTINGS_VIEW_DISPLAY,
    SETTINGS_VIEW_TOUCH,
    SETTINGS_VIEW_POWER,
    SETTINGS_VIEW_SYSTEM,
    SETTINGS_VIEW_UPDATE,
    SETTINGS_VIEW_FACTORY_RESET,
    SETTINGS_VIEW_REBOOT,
    SETTINGS_VIEW_SUSPEND
} settings_subview_t;

#define PAIR_OPT_COUNT 2

/* Activity item */
typedef struct {
    char id[32];
    char name[48];
    char type_str[24];
    char icon_path[128];
} activity_item_t;

#define MAX_ACTIVITIES 16
#define MAX_RF_ACTIVITIES 16
#define MAX_RF_DEVICES    16
#define MAX_RF_BUTTONS    48
#define MAX_RF_STEPS      64
#define MAX_ACT_TRIGGERS  16

typedef struct {
    char id[13];
    char name[24];
    char icon[24];
} rf_activity_t;

typedef struct {
    char id[13];
    char name[15];
} rf_device_t;

typedef struct {
    char context_id[13];  /* activity context ID, e.g. "53591842" or "-1" */
    char label[16];       /* short button label */
    uint8_t context_type; /* 0=physical, 1=screen/dev button */
    uint8_t action_type;  /* 1=device_cmd, 2=activity_start, 3=activity_stop */
    uint8_t btn_index;    /* index in activity screen_buttons */
    char icon_key[24];    /* optional icon key / function id / semantic name */
} rf_button_t;

typedef struct {
    char act_id[12];
    uint8_t step_idx;
    uint8_t total_steps;
    uint16_t delay_ms;
    char name[32];
} rf_act_step_t;

typedef struct {
    char button_name[16];
    uint16_t rf_key_code;
    uint8_t atype; /* 0x02 = start, 0x03 = stop */
    char target_act_id[12];
} activity_trigger_btn_t;

typedef struct {
    uint8_t percent;
    uint8_t charging;
    uint16_t voltage_mv;
    char powersource[16];
} elite_battery_info_t;

/* Log levels */
#define LOG_LEVEL_ERR   0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3

/* Shared Globals */
extern ui_view_t g_current_view;
extern settings_subview_t g_settings_subview;
extern int g_remote_mode;
extern bool g_no_screen_mode;
extern int g_log_level;

extern volatile bool g_rf_link_connected;
extern volatile int g_pairing_state;
extern uint8_t g_paired_hub_addr[4];
extern uint8_t g_remote_own_addr[4];
extern bool g_unpair_confirm_active;

extern bool g_update_config_available;
extern bool g_update_ota_available;
extern uint8_t g_hub_config_version;
extern char g_hub_ota_version[16];

extern activity_item_t g_activities[MAX_ACTIVITIES];
extern int g_activity_count;
extern int g_selected_activity;
extern char g_active_activity_id[32];
extern char g_active_activity_name[48];
extern bool g_initial_sync_received;

extern int g_selected_device_cmd;
extern rf_activity_t g_rf_activities[MAX_RF_ACTIVITIES];
extern int g_rf_activity_count;
extern rf_device_t g_rf_devices[MAX_RF_DEVICES];
extern int g_rf_device_count;
extern rf_button_t g_rf_buttons[MAX_RF_BUTTONS];
extern int g_rf_button_count;
extern rf_act_step_t g_rf_steps[MAX_RF_STEPS];
extern int g_rf_step_count;

extern uint8_t g_config_version;
extern uint8_t g_remote_slot;
extern bool g_has_hub_config;
extern uint8_t g_tx_seq;

extern bool g_is_switching_activity;
extern char g_switching_target_id[16];
extern char g_switching_target_name[48];
extern uint64_t g_switching_start_ms;
extern uint32_t g_switching_total_ms;

extern bool g_is_syncing_config;
extern uint64_t g_sync_start_ms;
extern uint64_t g_sync_last_packet_ms;
extern int g_sync_chunk_count;

extern bool g_is_ota_updating;
extern uint64_t g_ota_start_ms;
extern uint64_t g_ota_last_packet_ms;
extern uint32_t g_ota_total_bytes;
extern uint32_t g_ota_received_bytes;
extern uint16_t g_ota_total_chunks;
extern uint16_t g_ota_received_chunks;
extern FILE *g_ota_file;

extern FILE *g_icon_rx_file;
extern uint32_t g_icon_rx_bytes;
extern uint32_t g_icon_rx_tot_bytes;
extern uint16_t g_icon_rx_chunks;
extern uint8_t g_icon_rx_type;
extern char g_icon_rx_id[32];

extern int g_selected_category;
extern int g_selected_sub_item;
extern int g_factory_reset_stage;
extern uint64_t g_suspend_countdown_start_ms;
extern int g_selected_pair_opt;
extern bool g_touch_enabled;
extern bool g_cap_buttons_enabled;
extern int g_touch_sensitivity;
extern uint64_t g_last_switch_completed_ms;
extern char g_last_switch_target_id[16];

extern activity_trigger_btn_t g_act_triggers[MAX_ACT_TRIGGERS];
extern int g_act_trigger_count;

extern bool g_gyro_enabled;
extern int g_gyro_rate_hz;
extern int g_btn_backlight_mode;
extern int g_night_start_hour;
extern int g_night_end_hour;
extern int g_timeout_seconds;
extern int g_brightness_val;
extern bool g_confirm_activity;
extern int g_confirm_pending_idx;
extern uint64_t g_confirm_pending_deadline_ms;
extern int g_confirm_pending_rf_code;
extern uint64_t g_confirm_pending_rf_deadline_ms;
extern bool g_settings_dirty;

extern elite_battery_info_t g_battery_info;
extern bool g_screen_on;
extern bool g_screen_nav_mode;
extern int g_nav_toggle_key;
extern int g_nav_toggle_mode;
extern bool g_usbeth_enabled;
extern bool g_usb_connected;

extern bool g_screen_dirty;
extern char g_notice_msg[64];
extern uint64_t g_notice_until_ms;

extern uint64_t g_toggle_key_down_ms;
extern bool g_toggle_key_triggered;

extern volatile bool g_running;
extern int g_fb_fd;
extern uint16_t *g_fb_mem;
extern size_t g_fb_size;
extern int g_btn_fd;
extern int g_touch_fd;
extern int g_gyro_fd;
extern int g_rf_fd;
extern time_t g_last_activity_time;
extern uint64_t s_last_blank_time_ms;
extern uint64_t s_last_wake_ms;
extern uint64_t s_last_reconnect_ms;
extern volatile uint64_t g_last_hub_frame_ms;
extern bool g_power_debug;
extern pthread_mutex_t g_render_mutex;
extern pthread_mutex_t g_rf_write_mutex;

extern int g_touch_cur_x;
extern int g_touch_cur_y;
extern int g_touch_down_x;
extern int g_touch_down_y;
extern bool g_touch_is_down;
extern bool g_touch_need_down_latch;
extern uint64_t g_touch_down_ms;

/* Utility / helper functions */
uint64_t get_monotonic_ms(void);
bool is_remote_paired(void);
void reset_idle_timer(void);
void show_notice(const char *msg, int duration_ms);
void log_power_debug(const char *fmt, ...);
int get_active_dev_cmd_count(void);
int get_active_dev_cmd_item(int filtered_idx, char *out_label, size_t label_len, uint8_t *out_btn_index, uint8_t *out_atype, char *out_icon, size_t icon_len);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_ELITE_STATE_H */
