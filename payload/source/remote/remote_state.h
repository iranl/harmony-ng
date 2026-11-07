#ifndef HARMONY_REMOTE_STATE_H
#define HARMONY_REMOTE_STATE_H

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
#ifdef __linux__ /* the host and WebAssembly renderer builds have no device headers */
#include <linux/input.h>
#include <linux/fb.h>
#include <linux/i2c-dev.h>
#endif

#include "cJSON.h"
#include "rf_proto.h"
#include "font_aa.h"
#include "remote_theme.h"
#include "harmony_version.h"
#include "remote_stock_icons.h"
#include "remote_i18n.h"

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
#define RF_CONFIG_FILE            "/data/remote_rf_config.json"
#define SETTINGS_CONFIG_FILE      "/data/remote_settings.conf"
#define FORCE_SYNC_FILE           "/data/force_sync"

#define FB_WIDTH            240
#define FB_HEIGHT           320
#define FB_SIZE             (FB_WIDTH * FB_HEIGHT * 2)

/* Colours come from the theme's palette (remote_theme.h); these names are the roles' older
   names, kept so the drawing code reads as before */
#define RGB_BLACK           (g_theme.pal.on_accent)
#define RGB_BG              (g_theme.pal.bg)
#define RGB_CARD            (g_theme.pal.card)
#define RGB_CARD_SEL        (g_theme.pal.card_sel)
#define RGB_CARD_BORDER     (g_theme.pal.card_border)
#define RGB_WHITE           (g_theme.pal.text)
#define RGB_TEXT_MUTED      (g_theme.pal.text_muted)
#define RGB_CYAN            (g_theme.pal.accent)
#define RGB_TEAL            (g_theme.pal.accent)
#define RGB_GREEN           (g_theme.pal.success)
#define RGB_RED             (g_theme.pal.danger)
#define RGB_YELLOW          (g_theme.pal.warning)
#define RGB_BLUE            (g_theme.pal.info)
#define RGB_HEADER          (g_theme.pal.header)
#define RGB_DIVIDER         (g_theme.pal.divider)

/* Text is drawn with the theme's fonts */
#define g_font_main         (*g_theme.font_main)
#define g_font_title        (*g_theme.font_title)

enum { HOME_LAYOUT_THEME = 0, HOME_LAYOUT_LIST, HOME_LAYOUT_GRID, HOME_LAYOUT_CAROUSEL, HOME_LAYOUT_COUNT };
enum { ACTIVITY_ORDER_YOURS = 0, ACTIVITY_ORDER_MOST_USED };

/* UI Views */
typedef enum {
    VIEW_ACTIVITIES = 0,
    VIEW_DEVICES,
    VIEW_SETTINGS,
    VIEW_PAIRING,
    VIEW_NOT_PAIRED,
    VIEW_DEVICE_BROWSER, /* all Hub devices, then one device's commands */
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
    char name[RF_NAME_SIZE];
    char icon[24];
    uint16_t icon_id;
    uint8_t dev_count;            /* devices the activity uses, from RF_MSG_ACT_DEVICES_CHUNK */
    uint8_t devs[MAX_RF_DEVICES]; /* indexes into g_rf_devices */
} rf_activity_t;

typedef struct {
    char id[8];
    char name[RF_NAME_SIZE];
    char icon[24];
    uint16_t icon_id;
} rf_device_t;

/* Device command names pushed by the Hub, for browsing devices on the remote */
#define MAX_RF_DEV_CMDS 512
typedef struct {
    uint8_t dev_idx;  /* index into g_rf_devices */
    uint8_t cmd_idx;  /* index into the device's commands on the Hub */
    char name[23];
} rf_dev_cmd_t;

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
} remote_battery_info_t;

/* Log levels */
#define LOG_LEVEL_ERR   0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3

/* Shared Globals */
extern ui_view_t g_current_view;
extern settings_subview_t g_settings_subview;
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
extern rf_dev_cmd_t g_rf_dev_cmds[MAX_RF_DEV_CMDS];
extern int g_rf_dev_cmd_count;
extern int g_browse_dev;
extern int g_browse_sel;
void store_rf_dev_cmd(uint8_t dev_idx, uint8_t cmd_idx, const char *name);
extern rf_button_t g_rf_buttons[MAX_RF_BUTTONS];
extern int g_rf_button_count;
extern rf_act_step_t g_rf_steps[MAX_RF_STEPS];
extern int g_rf_step_count;

extern uint8_t g_config_version;
extern uint8_t g_remote_slot;
extern bool g_has_hub_config;
extern uint8_t g_tx_seq;

extern volatile bool g_is_switching_activity;
extern char g_switching_target_id[16];
extern char g_switching_target_name[48];
extern uint64_t g_switching_start_ms;
extern uint32_t g_switching_total_ms;

extern volatile bool g_is_syncing_config;
extern uint64_t g_sync_start_ms;
extern uint64_t g_sync_last_packet_ms;
extern uint64_t g_sync_failed_ms;         /* when last config sync stalled, for retry backoff */
extern int g_sync_chunk_count;
extern int g_sync_total;                  /* chunk frames in this push, from CONFIG_END clear; 0 if unknown */
extern pthread_mutex_t g_sync_lock;

/* Smart backoff: 10s if we already have a working config, 3s if unconfigured */
static inline bool sync_backoff_active(uint64_t now_ms) {
    if (g_sync_failed_ms == 0) return false;
    uint64_t backoff_duration = g_has_hub_config ? 10000 : 3000;
    return (now_ms - g_sync_failed_ms < backoff_duration);
}

extern volatile bool g_is_ota_updating;
extern uint64_t g_ota_start_ms;
extern volatile uint64_t g_ota_last_packet_ms;
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
extern int g_deep_standby_min;
extern int g_btn_backlight_mode;
extern int g_night_start_hour;
extern int g_night_end_hour;
extern int g_timeout_seconds;
extern int g_brightness_val;
extern bool g_confirm_activity;
extern int g_home_layout;            /* HOME_LAYOUT_*: the theme's, or the user's choice */
extern int g_activity_order;         /* ACTIVITY_ORDER_*: the web UI's order, or most used first */
extern int g_confirm_pending_idx;
extern uint64_t g_confirm_pending_deadline_ms;
extern int g_confirm_pending_rf_code;
extern uint64_t g_confirm_pending_rf_deadline_ms;
extern bool g_settings_dirty;

extern remote_battery_info_t g_battery_info;
extern bool g_screen_on;
extern bool g_usb_connected;

extern volatile bool g_screen_dirty;
extern char g_notice_msg[64];
extern uint64_t g_notice_until_ms;
/* Low 32 bits of the monotonic ms until which the charging page shows after docking, 0 when off.
   The power manager thread sets it, so it is one word wide. */
extern volatile uint32_t g_charging_page_until_ms;

extern uint64_t g_act_key_down_ms;   /* Activities key press time, 0 when released */
extern bool g_act_key_long_fired;    /* the current press already opened the menu */
extern bool g_menu_open;
extern int g_menu_sel;
extern bool g_key_hint_pending;      /* show the one-time Activities/Devices/Off key hint */
extern bool g_link_panel_open;       /* RF link details, opened from the status bar indicator */

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
extern uint8_t g_rf_pm; /* CC2544 power mode, in byte 1 of every frame sent (RF_PM_ACTIVE / RF_PM_TRANSFER) */

extern int g_touch_cur_x;
extern int g_touch_cur_y;
extern int g_touch_down_x;
extern int g_touch_down_y;
extern bool g_touch_is_down;
extern bool g_touch_need_down_latch;
extern uint64_t g_touch_down_ms;

/* Utility / helper functions */
uint64_t get_monotonic_ms(void);
uint64_t get_monotonic_us(void);

/* Milliseconds from then to now, 0 if then is later. Timestamps written by another thread (the RF
 * receiver stamps each frame) can be a few ms newer than a now_ms read earlier, and the unsigned
 * difference would wrap to a huge age. */
static inline uint64_t ms_since(uint64_t now_ms, uint64_t then_ms) {
    return now_ms > then_ms ? now_ms - then_ms : 0;
}

/* Files the renderer and config loader read are opened through remote_fs_path(). On the
   remote g_fs_root is empty; the host renderer points it at a directory laid out like the
   remote's filesystem. */
extern const char *g_fs_root;
const char *remote_fs_path(const char *path, char *buf, size_t len);
bool is_remote_paired(void);
void reset_idle_timer(void);
void show_notice(const char *msg, int duration_ms);
void log_power_debug(const char *fmt, ...);
bool has_configured_activities(void);
int get_active_dev_cmd_count(void);
int get_active_dev_cmd_item(int filtered_idx, char *out_label, size_t label_len, uint8_t *out_btn_index, uint8_t *out_atype, char *out_icon, size_t icon_len);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_STATE_H */
