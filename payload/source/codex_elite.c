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
#include "codex_rf_proto.h"

#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "font_aa.h"
#include "codex_version.h"

#define FB_DEV              "/dev/fb0"
#define FB_BLANK_SYSFS      "/sys/class/graphics/fb0/blank"
#define BACKLIGHT_SYSFS     "/sys/class/backlight/ap3156_backlight/brightness"
#define BTN_LED_SYSFS       "/sys/class/leds/tps6507x:white/brightness"
#define BTN_EVENT_DEV       "/dev/input/event0"
#define GYRO_EVENT_DEV      "/dev/input/event1"
#define TOUCH_EVENT_DEV     "/dev/input/event2"
#define GYRO_DATA_RATE_SYS      "/sys/devices/platform/i2c_davinci.1/i2c-1/1-0019/data_rate"
#define GYRO_INT1_THRESHOLD_SYS "/sys/devices/platform/i2c_davinci.1/i2c-1/1-0019/int1_threshold"
#define GYRO_INT1_DURATION_SYS  "/sys/devices/platform/i2c_davinci.1/i2c-1/1-0019/int1_duration"
#define GYRO_XYZ_SYS            "/sys/devices/platform/i2c_davinci.1/i2c-1/1-0019/xyz"
#define RFSPI_DEV                 "/dev/rfspi"
#define ACTIVITY_JSON_FILE        "/data/activities.json"
#define STOCK_ACTIVITY_JSON_FILE  "/data/resources/ActivityList.json"



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
    VIEW_COUNT
} ui_view_t;

static ui_view_t g_current_view = VIEW_ACTIVITIES;
static int g_remote_mode = 0;         /* 0 = Activities, 1 = Direct Device Mode */
static bool g_no_screen_mode = false;
#define LOG_LEVEL_ERR   0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3
static int g_log_level = LOG_LEVEL_INFO;
static void rf_send_full_sync_request(void);
static void rf_wake_and_reconnect(void);

/* Activity item */
typedef struct {
    char id[32];
    char name[48];
    char type_str[24];
    char icon_path[128];
} activity_item_t;

#define MAX_ACTIVITIES 16
static activity_item_t g_activities[MAX_ACTIVITIES];
static int g_activity_count = 0;
static int g_selected_activity = 0;
static char g_active_activity_id[32] = "-1";
static char g_active_activity_name[48] = "PowerOff";
static bool g_initial_sync_received = false;
static void reset_idle_timer(void);

static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int g_selected_device_cmd = 0;

#define MAX_RF_ACTIVITIES 16
#define MAX_RF_DEVICES    16
#define MAX_RF_BUTTONS    48

typedef struct {
    char id[13];
    char name[24];
    char icon[24];
} rf_activity_t;

static void resolve_activity_icon(const char *image_key, const char *clean_name, char *out_path, size_t max_len);

static void map_icon_type_to_str(uint8_t type, const char *name, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    switch (type) {
        case 1:  strncpy(out, "watchtv", out_len - 1); break;
        case 2:  strncpy(out, "movie", out_len - 1); break;
        case 3:  strncpy(out, "music", out_len - 1); break;
        case 4:  strncpy(out, "game", out_len - 1); break;
        case 5:  strncpy(out, "ps", out_len - 1); break;
        case 6:  strncpy(out, "xbox", out_len - 1); break;
        case 7:  strncpy(out, "switch", out_len - 1); break;
        case 8:  strncpy(out, "shield", out_len - 1); break;
        case 9:  strncpy(out, "apple", out_len - 1); break;
        case 10: strncpy(out, "roku", out_len - 1); break;
        case 11: strncpy(out, "firetv", out_len - 1); break;
        case 12: strncpy(out, "pc", out_len - 1); break;
        case 13: strncpy(out, "smarttv", out_len - 1); break;
        case 14: strncpy(out, "sonos", out_len - 1); break;
        case 15: strncpy(out, "receiver", out_len - 1); break;
        case 16: strncpy(out, "light", out_len - 1); break;
        case 17: strncpy(out, "power", out_len - 1); break;
        default:
            if (name && name[0]) {
                char low[64] = {0};
                for (int k = 0; name[k] && k < 63; k++) low[k] = tolower((unsigned char)name[k]);
                if (strstr(low, "switch") || strstr(low, "nintendo")) strncpy(out, "switch", out_len - 1);
                else strncpy(out, name, out_len - 1);
            } else {
                strncpy(out, "watchtv", out_len - 1);
            }
            break;
    }
    out[out_len - 1] = '\0';
}

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

static rf_activity_t g_rf_activities[MAX_RF_ACTIVITIES];
static int g_rf_activity_count = 0;
static rf_device_t g_rf_devices[MAX_RF_DEVICES];
static int g_rf_device_count = 0;
static rf_button_t g_rf_buttons[MAX_RF_BUTTONS];
static int g_rf_button_count = 0;

#define MAX_RF_STEPS 64
typedef struct {
    char act_id[12];
    uint8_t step_idx;
    uint8_t total_steps;
    uint16_t delay_ms;
    char name[32];
} rf_act_step_t;

static rf_act_step_t g_rf_steps[MAX_RF_STEPS];
static int g_rf_step_count = 0;

static uint8_t g_config_version = 0;
static uint8_t g_remote_slot = 0x01;
static bool g_has_hub_config = false;
static uint8_t g_tx_seq = 0;

/* Activity switching lock overlay state */
static bool g_is_switching_activity = false;
static char g_switching_target_id[16] = "-1";
static char g_switching_target_name[48] = "PowerOff";
static uint64_t g_switching_start_ms = 0;
static uint32_t g_switching_total_ms = 3500;

/* Config Synchronization & RF OTA Lock overlay states */
static bool g_is_syncing_config = false;
static uint64_t g_sync_start_ms = 0;
static uint64_t g_sync_last_packet_ms = 0;
static int g_sync_chunk_count = 0;

static bool g_is_ota_updating = false;
static uint64_t g_ota_start_ms = 0;
static uint64_t g_ota_last_packet_ms = 0;
static uint32_t g_ota_total_bytes = 0;
static uint32_t g_ota_received_bytes = 0;
static uint16_t g_ota_total_chunks = 0;
static uint16_t g_ota_received_chunks = 0;
static FILE *g_ota_file = NULL;

static int get_active_dev_cmd_count(void) {
    if (!g_has_hub_config || g_rf_button_count == 0) {
        return 0;
    }
    const char *target_ctx = g_active_activity_id;
    if (g_remote_mode == 1) {
        target_ctx = (g_current_view == VIEW_ACTIVITIES) ? "page1" : "page2";
    }
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

static int get_active_dev_cmd_item(int filtered_idx, char *out_label, size_t label_len, uint8_t *out_btn_index, uint8_t *out_atype, char *out_icon, size_t icon_len) {
    if (!g_has_hub_config || g_rf_button_count == 0) {
        return -1;
    }
    const char *target_ctx = g_active_activity_id;
    if (g_remote_mode == 1) {
        target_ctx = (g_current_view == VIEW_ACTIVITIES) ? "page1" : "page2";
    }
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

#define RF_CONFIG_FILE "/data/codex_elite_config.json"

static void save_rf_config(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddNumberToObject(root, "version", g_config_version);
    cJSON_AddNumberToObject(root, "slot", g_remote_slot);

    cJSON *acts = cJSON_AddArrayToObject(root, "activities");
    for (int i = 0; i < g_rf_activity_count; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "id", g_rf_activities[i].id);
        cJSON_AddStringToObject(a, "name", g_rf_activities[i].name);
        if (g_rf_activities[i].icon[0]) {
            cJSON_AddStringToObject(a, "icon", g_rf_activities[i].icon);
        }
        cJSON_AddItemToArray(acts, a);
    }

    cJSON *devs = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < g_rf_device_count; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "id", g_rf_devices[i].id);
        cJSON_AddStringToObject(d, "name", g_rf_devices[i].name);
        cJSON_AddItemToArray(devs, d);
    }

    cJSON *btns = cJSON_AddArrayToObject(root, "buttons");
    for (int i = 0; i < g_rf_button_count; i++) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "ctx", g_rf_buttons[i].context_id);
        cJSON_AddStringToObject(b, "label", g_rf_buttons[i].label);
        cJSON_AddNumberToObject(b, "ctype", g_rf_buttons[i].context_type);
        cJSON_AddNumberToObject(b, "atype", g_rf_buttons[i].action_type);
        cJSON_AddNumberToObject(b, "bidx", g_rf_buttons[i].btn_index);
        if (g_rf_buttons[i].icon_key[0]) {
            cJSON_AddStringToObject(b, "icon", g_rf_buttons[i].icon_key);
        }
        cJSON_AddItemToArray(btns, b);
    }

    cJSON *steps = cJSON_AddArrayToObject(root, "steps");
    for (int i = 0; i < g_rf_step_count; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "aid", g_rf_steps[i].act_id);
        cJSON_AddNumberToObject(s, "idx", g_rf_steps[i].step_idx);
        cJSON_AddNumberToObject(s, "tot", g_rf_steps[i].total_steps);
        cJSON_AddNumberToObject(s, "del", g_rf_steps[i].delay_ms);
        cJSON_AddStringToObject(s, "name", g_rf_steps[i].name);
        cJSON_AddItemToArray(steps, s);
    }

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        FILE *f = fopen(RF_CONFIG_FILE, "w");
        if (f) { fputs(json, f); fclose(f); }
        free(json);
    }
    cJSON_Delete(root);
}

static void load_rf_config(void) {
    FILE *f = fopen(RF_CONFIG_FILE, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 50000) { fclose(f); return; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *j_ver = cJSON_GetObjectItem(root, "version");
    if (j_ver && cJSON_IsNumber(j_ver)) g_config_version = (uint8_t)j_ver->valueint;

    cJSON *j_slot = cJSON_GetObjectItem(root, "slot");
    if (j_slot && cJSON_IsNumber(j_slot) && j_slot->valueint >= 1 && j_slot->valueint <= 15) {
        g_remote_slot = (uint8_t)j_slot->valueint;
    }

    cJSON *acts = cJSON_GetObjectItem(root, "activities");
    g_rf_activity_count = 0;
    if (acts && cJSON_IsArray(acts)) {
        cJSON *a = NULL;
        cJSON_ArrayForEach(a, acts) {
            if (g_rf_activity_count >= MAX_RF_ACTIVITIES) break;
            cJSON *jid = cJSON_GetObjectItem(a, "id");
            cJSON *jname = cJSON_GetObjectItem(a, "name");
            cJSON *jico = cJSON_GetObjectItem(a, "icon");
            if (jid && jname && cJSON_IsString(jid) && cJSON_IsString(jname)) {
                strncpy(g_rf_activities[g_rf_activity_count].id, jid->valuestring, sizeof(g_rf_activities[0].id) - 1);
                g_rf_activities[g_rf_activity_count].id[sizeof(g_rf_activities[0].id) - 1] = '\0';
                strncpy(g_rf_activities[g_rf_activity_count].name, jname->valuestring, sizeof(g_rf_activities[0].name) - 1);
                g_rf_activities[g_rf_activity_count].name[sizeof(g_rf_activities[0].name) - 1] = '\0';
                if (jico && cJSON_IsString(jico) && jico->valuestring[0]) {
                    strncpy(g_rf_activities[g_rf_activity_count].icon, jico->valuestring, sizeof(g_rf_activities[0].icon) - 1);
                    g_rf_activities[g_rf_activity_count].icon[sizeof(g_rf_activities[0].icon) - 1] = '\0';
                } else {
                    map_icon_type_to_str(0, g_rf_activities[g_rf_activity_count].name, g_rf_activities[g_rf_activity_count].icon, sizeof(g_rf_activities[0].icon));
                }
                g_rf_activity_count++;
            }
        }
    }

    cJSON *devs = cJSON_GetObjectItem(root, "devices");
    g_rf_device_count = 0;
    if (devs && cJSON_IsArray(devs)) {
        cJSON *d = NULL;
        cJSON_ArrayForEach(d, devs) {
            if (g_rf_device_count >= MAX_RF_DEVICES) break;
            cJSON *jid = cJSON_GetObjectItem(d, "id");
            cJSON *jname = cJSON_GetObjectItem(d, "name");
            if (jid && jname && cJSON_IsString(jid) && cJSON_IsString(jname)) {
                strncpy(g_rf_devices[g_rf_device_count].id, jid->valuestring, 12);
                g_rf_devices[g_rf_device_count].id[12] = '\0';
                strncpy(g_rf_devices[g_rf_device_count].name, jname->valuestring, 14);
                g_rf_devices[g_rf_device_count].name[14] = '\0';
                g_rf_device_count++;
            }
        }
    }

    cJSON *btns = cJSON_GetObjectItem(root, "buttons");
    g_rf_button_count = 0;
    if (btns && cJSON_IsArray(btns)) {
        cJSON *b = NULL;
        cJSON_ArrayForEach(b, btns) {
            if (g_rf_button_count >= MAX_RF_BUTTONS) break;
            cJSON *jctx = cJSON_GetObjectItem(b, "ctx");
            cJSON *jlbl = cJSON_GetObjectItem(b, "label");
            cJSON *jctype = cJSON_GetObjectItem(b, "ctype");
            cJSON *jatype = cJSON_GetObjectItem(b, "atype");
            cJSON *jico = cJSON_GetObjectItem(b, "icon");
            if (!jico) jico = cJSON_GetObjectItem(b, "icon_key");
            if (jctx && jlbl && cJSON_IsString(jctx) && cJSON_IsString(jlbl)) {
                strncpy(g_rf_buttons[g_rf_button_count].context_id, jctx->valuestring, 12);
                g_rf_buttons[g_rf_button_count].context_id[12] = '\0';
                strncpy(g_rf_buttons[g_rf_button_count].label, jlbl->valuestring, 15);
                g_rf_buttons[g_rf_button_count].label[15] = '\0';
                g_rf_buttons[g_rf_button_count].context_type = jctype ? (uint8_t)jctype->valueint : 0;
                g_rf_buttons[g_rf_button_count].action_type = jatype ? (uint8_t)jatype->valueint : 1;
                cJSON *jbidx = cJSON_GetObjectItem(b, "bidx");
                if (jbidx && cJSON_IsNumber(jbidx)) {
                    g_rf_buttons[g_rf_button_count].btn_index = (uint8_t)jbidx->valueint;
                } else {
                    uint8_t auto_idx = 0;
                    for (int k = 0; k < g_rf_button_count; k++) {
                        if (g_rf_buttons[k].context_type == g_rf_buttons[g_rf_button_count].context_type &&
                            strcmp(g_rf_buttons[k].context_id, g_rf_buttons[g_rf_button_count].context_id) == 0) {
                            auto_idx++;
                        }
                    }
                    g_rf_buttons[g_rf_button_count].btn_index = auto_idx;
                }
                if (jico && cJSON_IsString(jico) && jico->valuestring[0]) {
                    strncpy(g_rf_buttons[g_rf_button_count].icon_key, jico->valuestring, sizeof(g_rf_buttons[0].icon_key) - 1);
                    g_rf_buttons[g_rf_button_count].icon_key[sizeof(g_rf_buttons[0].icon_key) - 1] = '\0';
                } else {
                    g_rf_buttons[g_rf_button_count].icon_key[0] = '\0';
                }
                g_rf_button_count++;
            }
        }
    }

    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    g_rf_step_count = 0;
    if (steps && cJSON_IsArray(steps)) {
        cJSON *s = NULL;
        cJSON_ArrayForEach(s, steps) {
            if (g_rf_step_count >= MAX_RF_STEPS) break;
            cJSON *jaid = cJSON_GetObjectItem(s, "aid");
            cJSON *jidx = cJSON_GetObjectItem(s, "idx");
            cJSON *jtot = cJSON_GetObjectItem(s, "tot");
            cJSON *jdel = cJSON_GetObjectItem(s, "del");
            cJSON *jname = cJSON_GetObjectItem(s, "name");
            if (jaid && jname && cJSON_IsString(jaid) && cJSON_IsString(jname)) {
                strncpy(g_rf_steps[g_rf_step_count].act_id, jaid->valuestring, sizeof(g_rf_steps[0].act_id) - 1);
                g_rf_steps[g_rf_step_count].act_id[sizeof(g_rf_steps[0].act_id) - 1] = '\0';
                g_rf_steps[g_rf_step_count].step_idx = jidx ? (uint8_t)jidx->valueint : 0;
                g_rf_steps[g_rf_step_count].total_steps = jtot ? (uint8_t)jtot->valueint : 1;
                g_rf_steps[g_rf_step_count].delay_ms = jdel ? (uint16_t)jdel->valueint : 1000;
                strncpy(g_rf_steps[g_rf_step_count].name, jname->valuestring, sizeof(g_rf_steps[0].name) - 1);
                g_rf_steps[g_rf_step_count].name[sizeof(g_rf_steps[0].name) - 1] = '\0';
                g_rf_step_count++;
            }
        }
    }

    cJSON_Delete(root);
    g_has_hub_config = (g_rf_activity_count > 0);
    if (g_has_hub_config) {
        g_activity_count = 0;
        for (int i = 0; i < g_rf_activity_count && g_activity_count < MAX_ACTIVITIES; i++) {
            strncpy(g_activities[g_activity_count].id, g_rf_activities[i].id, sizeof(g_activities[0].id) - 1);
            strncpy(g_activities[g_activity_count].name, g_rf_activities[i].name, sizeof(g_activities[0].name) - 1);
            resolve_activity_icon(g_rf_activities[i].icon, g_rf_activities[i].name,
                                  g_activities[g_activity_count].icon_path, sizeof(g_activities[0].icon_path));
            g_activity_count++;
        }
        if (g_activity_count < MAX_ACTIVITIES) {
            strncpy(g_activities[g_activity_count].id, "-1", sizeof(g_activities[0].id) - 1);
            strncpy(g_activities[g_activity_count].name, "Power Off", sizeof(g_activities[0].name) - 1);
            strncpy(g_activities[g_activity_count].icon_path, "/opt/luadraws/appscommon/asset/commands/116.png", sizeof(g_activities[0].icon_path) - 1);
            g_activity_count++;
        }
    }
}

/* Settings Sub-views (Hierarchical Categories) */
typedef enum {
    SETTINGS_VIEW_CATEGORIES = 0,
    SETTINGS_VIEW_ACTIVITIES,
    SETTINGS_VIEW_DISPLAY,
    SETTINGS_VIEW_TOUCH,
    SETTINGS_VIEW_POWER,
    SETTINGS_VIEW_SYSTEM,
    SETTINGS_VIEW_FACTORY_RESET,
    SETTINGS_VIEW_REBOOT
} settings_subview_t;

static settings_subview_t g_settings_subview = SETTINGS_VIEW_CATEGORIES;
static int g_selected_category = 0;
static int g_selected_sub_item = 0;
static int g_factory_reset_stage = 0; /* 0 = prompt/cancel, 1 = are you sure */
static int g_selected_pair_opt = 0;
#define PAIR_OPT_COUNT 2
static volatile bool g_rf_link_connected = false;
static bool g_touch_enabled = true;
static bool g_cap_buttons_enabled = true;
static int g_touch_sensitivity = 2;   /* 1 = Low, 2 = Normal, 3 = High */
static uint64_t g_last_switch_completed_ms = 0;
static char g_last_switch_target_id[16] = "";

typedef struct {
    char button_name[16];
    uint16_t rf_key_code;
    uint8_t atype; /* 0x02 = start, 0x03 = stop */
    char target_act_id[12];
} activity_trigger_btn_t;

#define MAX_ACT_TRIGGERS 16
static activity_trigger_btn_t g_act_triggers[MAX_ACT_TRIGGERS];
static int g_act_trigger_count = 0;

static void init_act_triggers(void) {
    g_act_trigger_count = 1;
    strncpy(g_act_triggers[0].button_name, "PwrOff", sizeof(g_act_triggers[0].button_name) - 1);
    g_act_triggers[0].rf_key_code = 0x01EC;
    g_act_triggers[0].atype = 0x03;
    strncpy(g_act_triggers[0].target_act_id, "-1", sizeof(g_act_triggers[0].target_act_id) - 1);
}

static bool g_gyro_enabled = true;
static int g_gyro_rate_hz = 25;       /* 10, 25, 50 Hz */
static int g_btn_backlight_mode = 1;  /* 0 = OFF, 1 = ON, 2 = NIGHT_SCHEDULE */
static int g_night_start_hour = 22;   /* 10 PM */
static int g_night_end_hour = 7;      /* 7 AM */
static int g_timeout_seconds = 120;
static int g_brightness_val = 3; /* 1..5 */
static bool g_confirm_activity = false;
static int g_confirm_pending_idx = -1;
static uint64_t g_confirm_pending_deadline_ms = 0;
static int g_confirm_pending_rf_code = 0;
static uint64_t g_confirm_pending_rf_deadline_ms = 0;
static bool g_settings_dirty = false;

typedef struct {
    uint8_t percent;
    uint8_t charging;
    uint16_t voltage_mv;
    char powersource[16];
} elite_battery_info_t;

static elite_battery_info_t g_battery_info = { 100, 0, 4100, "battery" };
static bool g_screen_on = true;
static bool g_screen_nav_mode = true;  /* Default true: bottom buttons navigate screen UI on boot */
static int g_nav_toggle_key = 139;     /* Default: 139 (Menu), also 358 (Info), 395 (Guide), 389 (DVR), 174 (Exit) */
static int g_nav_toggle_mode = 1;      /* 0 = Short Press, 1 = Long Press (Hold >= 500ms) */
static bool g_usbeth_enabled = false;  /* Dynamic USB Ethernet mode */
static bool g_usb_connected = false;   /* Dynamic physical USB cable detection */

static bool g_screen_dirty = true;

/* On-screen notice banner */
static char g_notice_msg[64] = {0};
static uint64_t g_notice_until_ms = 0;

static void show_notice(const char *msg, int duration_ms) {
    if (!msg) return;
    strncpy(g_notice_msg, msg, sizeof(g_notice_msg) - 1);
    g_notice_until_ms = get_monotonic_ms() + duration_ms;
    g_screen_dirty = true;
}

/* Button press timing for long-press toggle */
static uint64_t g_toggle_key_down_ms = 0;
static bool g_toggle_key_triggered = false;


/* Backbuffer & Double-buffering */
static uint16_t g_backbuffer[FB_WIDTH * FB_HEIGHT];
static uint16_t g_bg_wallpaper[FB_WIDTH * FB_HEIGHT];
static bool g_has_wallpaper = false;

/* State */
static volatile bool g_running = true;
static int g_fb_fd = -1;
static uint16_t *g_fb_mem = NULL;
static size_t g_fb_size = FB_SIZE;
static int g_btn_fd = -1;
static int g_touch_fd = -1;
static int g_gyro_fd = -1;
static int g_rf_fd = -1;
static time_t g_last_activity_time = 0;
static pthread_mutex_t g_render_mutex = PTHREAD_MUTEX_INITIALIZER;

static void render_screen(void);
static void handle_navigation_key(int code, int value);
static int rf_send_frame(rf_frame_t *frame);

/* Touch tracking */
static int g_touch_cur_x = 0;
static int g_touch_cur_y = 0;
static int g_touch_down_x = 0;
static int g_touch_down_y = 0;
static bool g_touch_is_down = false;
static bool g_touch_need_down_latch = false;
static uint64_t g_touch_down_ms = 0;

/* Full 8x8 ASCII Font (Characters 32 to 126) */
static const uint8_t g_font_8x8[95][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 32   */
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, /* 33 ! */
    {0x66, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 34 " */
    {0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00}, /* 35 # */
    {0x18, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x18, 0x00}, /* 36 $ */
    {0x00, 0x63, 0x66, 0x0C, 0x18, 0x33, 0x63, 0x00}, /* 37 % */
    {0x30, 0x48, 0x30, 0x54, 0x66, 0x44, 0x3A, 0x00}, /* 38 & */
    {0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 39 ' */
    {0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00}, /* 40 ( */
    {0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00}, /* 41 ) */
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, /* 42 * */
    {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00}, /* 43 + */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}, /* 44 , */
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}, /* 45 - */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}, /* 46 . */
    {0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00}, /* 47 / */
    {0x3C, 0x66, 0x6E, 0x7E, 0x76, 0x66, 0x3C, 0x00}, /* 48 0 */
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, /* 49 1 */
    {0x3C, 0x66, 0x06, 0x1C, 0x30, 0x66, 0x7E, 0x00}, /* 50 2 */
    {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00}, /* 51 3 */
    {0x0E, 0x1E, 0x36, 0x66, 0x7F, 0x06, 0x0F, 0x00}, /* 52 4 */
    {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00}, /* 53 5 */
    {0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00}, /* 54 6 */
    {0x7E, 0x66, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x00}, /* 55 7 */
    {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}, /* 56 8 */
    {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00}, /* 57 9 */
    {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00}, /* 58 : */
    {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x30, 0x00}, /* 59 ; */
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, /* 60 < */
    {0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00}, /* 61 = */
    {0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00}, /* 62 > */
    {0x3C, 0x66, 0x0C, 0x18, 0x18, 0x00, 0x18, 0x00}, /* 63 ? */
    {0x3C, 0x66, 0x6E, 0x6E, 0x60, 0x62, 0x3C, 0x00}, /* 64 @ */
    {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00}, /* 65 A */
    {0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00}, /* 66 B */
    {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00}, /* 67 C */
    {0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00}, /* 68 D */
    {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00}, /* 69 E */
    {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00}, /* 70 F */
    {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00}, /* 71 G */
    {0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00}, /* 72 H */
    {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}, /* 73 I */
    {0x0E, 0x06, 0x06, 0x06, 0x66, 0x66, 0x3C, 0x00}, /* 74 J */
    {0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00}, /* 75 K */
    {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00}, /* 76 L */
    {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00}, /* 77 M */
    {0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00}, /* 78 N */
    {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, /* 79 O */
    {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00}, /* 80 P */
    {0x3C, 0x66, 0x66, 0x66, 0x6E, 0x3C, 0x0E, 0x00}, /* 81 Q */
    {0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00}, /* 82 R */
    {0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00}, /* 83 S */
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* 84 T */
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, /* 85 U */
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}, /* 86 V */
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, /* 87 W */
    {0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00}, /* 88 X */
    {0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00}, /* 89 Y */
    {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00}, /* 90 Z */
    {0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00}, /* 91 [ */
    {0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00}, /* 92 \ */
    {0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00}, /* 93 ] */
    {0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00}, /* 94 ^ */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, /* 95 _ */
    {0x18, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 96 ` */
    {0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x3E, 0x00}, /* 97 a */
    {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00}, /* 98 b */
    {0x00, 0x00, 0x3C, 0x66, 0x60, 0x66, 0x3C, 0x00}, /* 99 c */
    {0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x00}, /* 100 d */
    {0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00}, /* 101 e */
    {0x0C, 0x18, 0x3E, 0x18, 0x18, 0x18, 0x18, 0x00}, /* 102 f */
    {0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x3C}, /* 103 g */
    {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00}, /* 104 h */
    {0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00}, /* 105 i */
    {0x06, 0x00, 0x0E, 0x06, 0x06, 0x66, 0x3C, 0x00}, /* 106 j */
    {0x60, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x00}, /* 107 k */
    {0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}, /* 108 l */
    {0x00, 0x00, 0x66, 0x7F, 0x7F, 0x6B, 0x63, 0x00}, /* 109 m */
    {0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00}, /* 110 n */
    {0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00}, /* 111 o */
    {0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60}, /* 112 p */
    {0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x06}, /* 113 q */
    {0x00, 0x00, 0x7C, 0x66, 0x60, 0x60, 0x60, 0x00}, /* 114 r */
    {0x00, 0x00, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x00}, /* 115 s */
    {0x18, 0x18, 0x7E, 0x18, 0x18, 0x18, 0x0E, 0x00}, /* 116 t */
    {0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00}, /* 117 u */
    {0x00, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}, /* 118 v */
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x3E, 0x36, 0x00}, /* 119 w */
    {0x00, 0x00, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x00}, /* 120 x */
    {0x00, 0x00, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x3C}, /* 121 y */
    {0x00, 0x00, 0x7E, 0x0C, 0x18, 0x30, 0x7E, 0x00}, /* 122 z */
    {0x0E, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0E, 0x00}, /* 123 { */
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, /* 124 | */
    {0x70, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x70, 0x00}, /* 125 } */
    {0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}  /* 126 ~ */
};

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

/* Backlight brightness control (0 to 31) */
static void set_backlight_level(int level) {
    int fd = open(BACKLIGHT_SYSFS, O_WRONLY);
    if (fd >= 0) {
        char buf[16];
        int val = level * 6; /* 1..5 maps to 6..30 */
        if (val > 31) val = 31;
        snprintf(buf, sizeof(buf), "%d\n", val);
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

/* Synaptics Clearpad Hardware Init & Reset via I2C */
static void clearpad_hw_init(void) {
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0) return;
    if (ioctl(fd, I2C_SLAVE_FORCE, 0x20) < 0) {
        close(fd);
        return;
    }

    union i2c_smbus_data data;
    struct i2c_smbus_ioctl_data args;

    /* 1. Software Reset: write 0x01 to F01 CmdBase 0x68 to clear any latched/stuck state */
    data.byte = 0x01;
    args.read_write = I2C_SMBUS_WRITE;
    args.command = 0x68;
    args.size = I2C_SMBUS_BYTE_DATA;
    args.data = &data;
    ioctl(fd, I2C_SMBUS, &args);

    usleep(150000); /* 150ms reset stabilization */

    /* 2. Normal operating mode: write 0x00 to F01 Control 0x35 */
    data.byte = 0x00;
    args.command = 0x35;
    ioctl(fd, I2C_SMBUS, &args);

    /* 3. Enable interrupts: write 0x07 to F01 Interrupt Enable 0x36 */
    data.byte = 0x07;
    args.command = 0x36;
    ioctl(fd, I2C_SMBUS, &args);

    /* 4. Clear pending interrupt: read F01 Interrupt Status 0x14 to release ATTN line HIGH */
    args.read_write = I2C_SMBUS_READ;
    args.command = 0x14;
    ioctl(fd, I2C_SMBUS, &args);

    close(fd);
    printf("[+] Synaptics Clearpad hardware reset & initialized via I2C\n");
}

/* Synaptics Clearpad Standby Wake & Interrupt Re-arm */
static void clearpad_wake(bool full_reset) {
    if (full_reset) {
        clearpad_hw_init();
    } else {
        int fd = open("/dev/i2c-1", O_RDWR);
        if (fd >= 0) {
            if (ioctl(fd, I2C_SLAVE_FORCE, 0x20) >= 0) {
                union i2c_smbus_data data;
                struct i2c_smbus_ioctl_data args;

                /* 1. Ensure normal operating mode: write 0x00 to F01 Control 0x35 */
                data.byte = 0x00;
                args.read_write = I2C_SMBUS_WRITE;
                args.command = 0x35;
                args.size = I2C_SMBUS_BYTE_DATA;
                args.data = &data;
                ioctl(fd, I2C_SMBUS, &args);

                /* 2. Enable interrupts: write 0x07 to F01 Interrupt Enable 0x36 */
                data.byte = 0x07;
                args.command = 0x36;
                ioctl(fd, I2C_SMBUS, &args);

                /* 3. Read 0x14 to clear latched interrupt and de-assert ATTN line HIGH */
                args.read_write = I2C_SMBUS_READ;
                args.command = 0x14;
                ioctl(fd, I2C_SMBUS, &args);
            }
            close(fd);
        }
    }
}

/* Synaptics Clearpad Hardware Sleep (turns off capacitive matrix and ADC scanning) */
static void clearpad_sleep(void) {
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd >= 0) {
        if (ioctl(fd, I2C_SLAVE_FORCE, 0x20) >= 0) {
            union i2c_smbus_data data;
            struct i2c_smbus_ioctl_data args;

            /* Write 0x01 (Sleep Mode) to F01 Control register 0x35 */
            data.byte = 0x01;
            args.read_write = I2C_SMBUS_WRITE;
            args.command = 0x35;
            args.size = I2C_SMBUS_BYTE_DATA;
            args.data = &data;
            ioctl(fd, I2C_SMBUS, &args);
        }
        close(fd);
    }
}

/* Power Debug Logging Infrastructure (/tmp/power_debug.log & /data/power_debug.log) */
#define POWER_LOG_TMP   "/tmp/power_debug.log"

static pthread_mutex_t g_power_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_power_debug = false;
static bool is_usb_hardware_connected(void);
static void read_elite_battery(void);

static void log_power_debug(const char *fmt, ...) {
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

    /* Write to stdout/stderr */
    printf("[PWR %s] %s", time_buf, msg);
    fflush(stdout);

    /* Write exclusively to /tmp/power_debug.log (RAM tmpfs - zero flash wear) */
    FILE *ftmp = fopen(POWER_LOG_TMP, "a");
    if (ftmp) {
        fprintf(ftmp, "[%s mono=%llums] %s", time_buf, (unsigned long long)get_monotonic_ms(), msg);
        fflush(ftmp);
        fclose(ftmp);
    }
    pthread_mutex_unlock(&g_power_log_mutex);
}

static void get_cpu_freq_status(char *out, size_t max_len) {
    char cur_freq[32] = "N/A";
    char gov[32] = "N/A";
    FILE *fc = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (fc) {
        if (fscanf(fc, "%31s", cur_freq) != 1) strncpy(cur_freq, "err", sizeof(cur_freq) - 1);
        fclose(fc);
    }
    FILE *fg = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
    if (fg) {
        if (fscanf(fg, "%31s", gov) != 1) strncpy(gov, "err", sizeof(gov) - 1);
        fclose(fg);
    }
    snprintf(out, max_len, "gov=%.16s, cur=%.16s", gov, cur_freq);
}

static inline bool is_elite_docked(void) {
    return (strcmp(g_battery_info.powersource, "cradle") == 0 ||
            strcmp(g_battery_info.powersource, "usb") == 0 ||
            g_battery_info.charging);
}

/* DaVinci DM365 CPU Frequency Scaling (456MHz active / 96MHz standby) */
static void set_cpu_freq_standby(bool standby) {
    if (standby && is_elite_docked()) {
        log_power_debug("CPU scale -> STANDBY bypassed (docked/charging: holding 456MHz)\n");
        return;
    }
    char before[96];
    get_cpu_freq_status(before, sizeof(before));
    if (standby) {
        bool gov_ok = false;
        FILE *fg = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "w");
        if (fg) {
            if (fputs("userspace\n", fg) >= 0) gov_ok = true;
            fclose(fg);
        }
        bool speed_ok = false;
        if (gov_ok) {
            FILE *fs = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed", "w");
            if (fs) {
                if (fputs("96000\n", fs) >= 0) speed_ok = true;
                fclose(fs);
            }
        }
        if (!gov_ok || !speed_ok) {
            /* Fallback to powersave governor */
            FILE *fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "w");
            if (fp) {
                fputs("powersave\n", fp);
                fclose(fp);
            }
        }
        char after[96];
        get_cpu_freq_status(after, sizeof(after));
        log_power_debug("CPU scale -> STANDBY: before [%s] -> after [%s] (gov_ok=%d, speed_ok=%d)\n",
                        before, after, gov_ok, speed_ok);
    } else {
        bool gov_ok = false;
        FILE *fg = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "w");
        if (fg) {
            if (fputs("performance\n", fg) >= 0) gov_ok = true;
            fclose(fg);
        }
        FILE *fs = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed", "w");
        if (fs) {
            fputs("456000\n", fs);
            fclose(fs);
        }
        if (!gov_ok) {
            FILE *fgu = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "w");
            if (fgu) {
                fputs("userspace\n", fgu);
                fclose(fgu);
            }
        }
        char after[96];
        get_cpu_freq_status(after, sizeof(after));
        log_power_debug("CPU scale -> ACTIVE: before [%s] -> after [%s] (gov_ok=%d)\n",
                        before, after, gov_ok);
    }
}

/* Screen Blank / Unblank */
static bool is_night_time(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) return false;
    int h = tm->tm_hour;
    if (g_night_start_hour > g_night_end_hour) {
        return (h >= g_night_start_hour || h < g_night_end_hour);
    } else {
        return (h >= g_night_start_hour && h < g_night_end_hour);
    }
}

static uint64_t s_last_blank_time_ms = 0;
static uint64_t s_last_wake_ms = 0;
static uint64_t s_last_reconnect_ms = 0;

static void fb_set_blank(bool blank) {
    int blank_res = -1;
    if (g_fb_fd >= 0) {
        blank_res = ioctl(g_fb_fd, FBIOBLANK, blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK);
    }
    int fd = open(FB_BLANK_SYSFS, O_WRONLY);
    if (fd >= 0) {
        /* da8xx_lcdc driver supports FB_BLANK_POWERDOWN (4) and FB_BLANK_UNBLANK (0) */
        const char *val = blank ? "4\n" : "0\n";
        write(fd, val, strlen(val));
        close(fd);
    }
    g_screen_on = !blank;
    uint64_t now_ms = get_monotonic_ms();

    if (g_screen_on) {
        s_last_wake_ms = now_ms;
        uint64_t sleep_sec = s_last_blank_time_ms > 0 ? (now_ms - s_last_blank_time_ms) / 1000 : 0;
        set_cpu_freq_standby(false);
        set_backlight_level(g_brightness_val);
        clearpad_wake(sleep_sec >= 10);
        read_elite_battery();
        rf_wake_and_reconnect();
        g_screen_dirty = true;
        log_power_debug("=== WAKING FROM STANDBY (Screen Active) ===\n");
        log_power_debug("  Slept for: %llu seconds | Backlight: %d | Battery: %dmV, %d%% (%s, chg=%d)\n",
                        (unsigned long long)sleep_sec, g_brightness_val,
                        g_battery_info.voltage_mv, g_battery_info.percent,
                        g_battery_info.powersource, g_battery_info.charging);
    } else {
        s_last_blank_time_ms = now_ms;
        set_backlight_level(0);
        set_cpu_freq_standby(true);
        clearpad_sleep();
        if (g_gyro_fd >= 0) {
            struct input_event ev[16];
            while (read(g_gyro_fd, ev, sizeof(ev)) > 0) {}
        }
        read_elite_battery();
        bool usb_plug = (strcmp(g_battery_info.powersource, "usb") == 0);
        log_power_debug("=== ENTERING STANDBY (Screen Blank) ===\n");
        log_power_debug("  fb_blank=%d, bl=0 | USB Eth: cfg=%d, cable=%d | Battery: %dmV, %d%% (%s, chg=%d)\n",
                        blank_res, g_usbeth_enabled, usb_plug,
                        g_battery_info.voltage_mv, g_battery_info.percent,
                        g_battery_info.powersource, g_battery_info.charging);
    }
    int lfd = open(BTN_LED_SYSFS, O_WRONLY);
    if (lfd >= 0) {
        bool turn_on = !blank;
        if (turn_on) {
            if (g_btn_backlight_mode == 0) turn_on = false;
            else if (g_btn_backlight_mode == 1) turn_on = true;
            else if (g_btn_backlight_mode == 2) turn_on = is_night_time();
        }
        int w_res = (int)write(lfd, turn_on ? "100\n" : "0\n", turn_on ? 4 : 2);
        close(lfd);
        if (!blank) {
            log_power_debug("  Button LED: mode=%d -> %s (res=%d)\n", g_btn_backlight_mode, turn_on ? "ON" : "OFF", w_res);
        }
    }
}

static void save_settings_conf(void);

/* Touchscreen Enable / Disable */
static void set_touch_enabled(bool enable) {
    g_touch_enabled = enable;
    if (!g_touch_enabled) {
        g_screen_nav_mode = true;
        show_notice("Screen nav moved to bottom buttons", 4000);
    }
    save_settings_conf();
}

/* Gyro / Accelerometer Enable / Disable */
static void set_gyro_enabled(bool enable) {
    g_gyro_enabled = enable;
}

static char g_timezone_str[48] = "CET-1CEST,M3.5.0,M10.5.0/3";

typedef struct {
    const char *prefix;
    const char *full_tz;
} tz_mapping_t;

static const tz_mapping_t g_tz_mappings[] = {
    { "CET-1CEST",      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "GMT0BST",        "GMT0BST,M3.5.0/1,M10.5.0" },
    { "WET0WEST",       "WET0WEST,M3.5.0/1,M10.5.0" },
    { "EET-2EEST",      "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "EST5EDT",        "EST5EDT,M3.2.0,M11.1.0" },
    { "CST6CDT",        "CST6CDT,M3.2.0,M11.1.0" },
    { "MST7MDT",        "MST7MDT,M3.2.0,M11.1.0" },
    { "PST8PDT",        "PST8PDT,M3.2.0,M11.1.0" },
    { "AKST9AKDT",      "AKST9AKDT,M3.2.0,M11.1.0" },
    { "NST3:30NDT",     "NST3:30NDT,M3.2.0,M11.1.0" },
    { "AST4ADT",        "AST4ADT,M3.2.0,M11.1.0" },
    { "AEST-10AEDT",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "ACST-9:30ACDT",  "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "NZST-12NZDT",    "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};

static void expand_timezone(const char *in, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (!in || !in[0]) {
        snprintf(out, out_sz, "CET-1CEST,M3.5.0,M10.5.0/3");
        return;
    }
    const char *c1 = strchr(in, ',');
    if (c1 && strchr(c1 + 1, ',')) {
        strncpy(out, in, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    for (size_t i = 0; i < sizeof(g_tz_mappings) / sizeof(g_tz_mappings[0]); i++) {
        if (strncmp(in, g_tz_mappings[i].prefix, strlen(g_tz_mappings[i].prefix)) == 0) {
            strncpy(out, g_tz_mappings[i].full_tz, out_sz - 1);
            out[out_sz - 1] = '\0';
            return;
        }
    }
    if (!strchr(in, ',')) {
        strncpy(out, in, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    snprintf(out, out_sz, "CET-1CEST,M3.5.0,M10.5.0/3");
}

static void set_msp430_rtc(uint32_t epoch_sec) {
    FILE *f = fopen("/sys/bus/i2c/devices/1-0034/time", "w");
    if (!f) return;
    fprintf(f, "%u\n", epoch_sec);
    fclose(f);
}

static void sync_time_from_rtc(void) {
    FILE *ftz = fopen("/etc/TZ", "r");
    if (ftz) {
        char tz[48] = {0};
        if (fgets(tz, sizeof(tz), ftz)) {
            char *nl = strchr(tz, '\n'); if (nl) *nl = '\0';
            char *cr = strchr(tz, '\r'); if (cr) *cr = '\0';
            if (tz[0] && strstr(tz, "EST5EDT") == NULL) {
                expand_timezone(tz, g_timezone_str, sizeof(g_timezone_str));
            }
        }
        fclose(ftz);
    }
    expand_timezone(g_timezone_str, g_timezone_str, sizeof(g_timezone_str));
    setenv("TZ", g_timezone_str, 1);
    tzset();
    ftz = fopen("/etc/TZ", "w");
    if (ftz) {
        fprintf(ftz, "%s\n", g_timezone_str);
        fclose(ftz);
    }

    FILE *f = fopen("/sys/bus/i2c/devices/1-0034/time", "r");
    if (!f) return;
    long long rtc_sec = 0;
    if (fscanf(f, "%lld", &rtc_sec) == 1 && rtc_sec > 1000000000LL) {
        struct timeval tv = { .tv_sec = (time_t)rtc_sec, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) == 0) {
            printf("[+] System time synced from MSP430 RTC: %lld (TZ: %s)\n", rtc_sec, g_timezone_str);
        }
    }
    fclose(f);
}

static const char *get_nav_key_name(int key) {
    switch (key) {
        case 139: return "MENU";
        case 358: return "INFO";
        case 395: return "GUIDE";
        case 389: return "DVR";
        case 174: return "EXIT";
        default:  return "MENU";
    }
}

static void cycle_nav_key(void) {
    if (!g_touch_enabled) {
        show_notice("Screen nav locked ON", 3000);
        return;
    }
    if (g_nav_toggle_key == 139) g_nav_toggle_key = 358;
    else if (g_nav_toggle_key == 358) g_nav_toggle_key = 395;
    else if (g_nav_toggle_key == 395) g_nav_toggle_key = 389;
    else if (g_nav_toggle_key == 389) g_nav_toggle_key = 174;
    else g_nav_toggle_key = 139;
    save_settings_conf();
}

static void cycle_nav_mode(void) {
    if (!g_touch_enabled) {
        show_notice("Screen nav locked ON", 3000);
        return;
    }
    g_nav_toggle_mode = !g_nav_toggle_mode;
    save_settings_conf();
}

static void cycle_touch_sensitivity(void) {
    g_touch_sensitivity++;
    if (g_touch_sensitivity > 3) g_touch_sensitivity = 1;
    save_settings_conf();
}

static void rf_send_sync_query(void);

static void cycle_remote_mode(void) {
    g_remote_mode = !g_remote_mode;
    save_settings_conf();
    rf_send_sync_query();
}

static void toggle_usbeth(void);
static void send_remote_config_backup_rf(void);

static void load_settings_conf(void) {
    FILE *f = fopen("/data/codex_elite.conf", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        char sval[64] = {0};
        if (sscanf(line, "%63[^=]=%63[^\n\r]", key, sval) == 2) {
            int val = atoi(sval);
            if (strcmp(key, "touch") == 0) g_touch_enabled = (val != 0);
            else if (strcmp(key, "cap_buttons") == 0 || strcmp(key, "capacitive_buttons") == 0) g_cap_buttons_enabled = (val != 0);
            else if (strcmp(key, "touch_sens") == 0) g_touch_sensitivity = (val >= 1 && val <= 3) ? val : 2;
            else if (strcmp(key, "remote_mode") == 0) g_remote_mode = (val == 1) ? 1 : 0;
            else if (strcmp(key, "gyro") == 0) g_gyro_enabled = (val != 0);
            else if (strcmp(key, "gyro_rate") == 0) g_gyro_rate_hz = (val > 0) ? val : 25;
            else if (strcmp(key, "btn_backlight") == 0) g_btn_backlight_mode = val;
            else if (strcmp(key, "night_start") == 0) g_night_start_hour = val;
            else if (strcmp(key, "night_end") == 0) g_night_end_hour = val;
            else if (strcmp(key, "timeout") == 0) g_timeout_seconds = val;
            else if (strcmp(key, "brightness") == 0) g_brightness_val = (val >= 1 && val <= 5) ? val : 3;
            else if (strcmp(key, "screen") == 0) g_screen_on = (val != 0);
            else if (strcmp(key, "nav_key") == 0) g_nav_toggle_key = val;
            else if (strcmp(key, "nav_mode") == 0) g_nav_toggle_mode = val;
            else if (strcmp(key, "usbeth") == 0) g_usbeth_enabled = (val != 0);
            else if (strcmp(key, "power_debug") == 0) g_power_debug = (val != 0);
            else if (strcmp(key, "confirm_act") == 0 || strcmp(key, "confirm") == 0) g_confirm_activity = (val != 0);
            else if (strcmp(key, "no_screen") == 0) g_no_screen_mode = (val != 0);
            else if (strcmp(key, "log_level") == 0) g_log_level = val;
            else if (strcmp(key, "slot") == 0) g_remote_slot = (val >= 1 && val <= 15) ? (uint8_t)val : 0x01;
            else if (strcmp(key, "tz") == 0) {
                expand_timezone(sval, g_timezone_str, sizeof(g_timezone_str));
                setenv("TZ", g_timezone_str, 1);
                tzset();
            }
        }
    }
    fclose(f);
    if (access("/data/debug_logging.conf", F_OK) == 0) {
        g_log_level = LOG_LEVEL_DEBUG;
    }
    if (!g_touch_enabled) {
        g_screen_nav_mode = true;
    }
    if (g_nav_toggle_key != 139 && g_nav_toggle_key != 358 &&
        g_nav_toggle_key != 395 && g_nav_toggle_key != 389 &&
        g_nav_toggle_key != 174) {
        g_nav_toggle_key = 139;
    }
}

static void save_settings_conf(void) {
    FILE *f = fopen("/data/codex_elite.conf", "w");
    if (!f) return;
    fprintf(f, "touch=%d\n", g_touch_enabled ? 1 : 0);
    fprintf(f, "cap_buttons=%d\n", g_cap_buttons_enabled ? 1 : 0);
    fprintf(f, "touch_sens=%d\n", g_touch_sensitivity);
    fprintf(f, "remote_mode=%d\n", g_remote_mode);
    fprintf(f, "gyro=%d\n", g_gyro_enabled ? 1 : 0);
    fprintf(f, "gyro_rate=%d\n", g_gyro_rate_hz);
    fprintf(f, "btn_backlight=%d\n", g_btn_backlight_mode);
    fprintf(f, "night_start=%d\n", g_night_start_hour);
    fprintf(f, "night_end=%d\n", g_night_end_hour);
    fprintf(f, "timeout=%d\n", g_timeout_seconds);
    fprintf(f, "brightness=%d\n", g_brightness_val);
    fprintf(f, "screen=1\n");
    fprintf(f, "no_screen=%d\n", g_no_screen_mode ? 1 : 0);
    fprintf(f, "log_level=%d\n", g_log_level);
    fprintf(f, "nav_key=%d\n", g_nav_toggle_key);
    fprintf(f, "nav_mode=%d\n", g_nav_toggle_mode);
    fprintf(f, "confirm_act=%d\n", g_confirm_activity ? 1 : 0);
    fprintf(f, "usbeth=%d\n", g_usbeth_enabled ? 1 : 0);
    fprintf(f, "power_debug=%d\n", g_power_debug ? 1 : 0);
    fprintf(f, "slot=%d\n", g_remote_slot);
    fprintf(f, "tz=%s\n", g_timezone_str);
    fclose(f);
    g_settings_dirty = true;
    if (g_rf_link_connected) {
        send_remote_config_backup_rf();
    }
}

static bool is_usb_hardware_connected(void) {
    FILE *fp = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/powersource", "r");
    if (fp) {
        char psrc[16] = {0};
        if (fscanf(fp, "%15s", psrc) == 1) {
            fclose(fp);
            strncpy(g_battery_info.powersource, psrc, sizeof(g_battery_info.powersource) - 1);
            return (strcmp(psrc, "usb") == 0);
        }
        fclose(fp);
    }
    return false;
}

#define BATT_SAMPLE_COUNT 16
static uint16_t s_batt_samples[BATT_SAMPLE_COUNT];
static int s_batt_sample_idx = 0;
static int s_batt_sample_num = 0;
static uint64_t s_last_batt_sample_ms = 0;
static uint8_t s_filtered_pct = 0xFF;

static void read_elite_battery(void) {
    uint64_t now_ms = get_monotonic_ms();

    /* Rate limit hardware ADC reads to once per second */
    if (s_last_batt_sample_ms > 0 && (now_ms - s_last_batt_sample_ms < 1000) && s_batt_sample_num > 0) {
        return;
    }
    s_last_batt_sample_ms = now_ms;

    /* Voltage */
    FILE *fv = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/vbat", "r");
    float v = 2.05f;
    uint16_t raw_mv = g_battery_info.voltage_mv;
    if (fv) {
        if (fscanf(fv, "%f", &v) == 1) {
            raw_mv = (uint16_t)(v * 2000.0f);
        }
        fclose(fv);
    }

    /* Charger */
    int is_charging = 0;
    FILE *fc = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/charger", "r");
    int chg = 0;
    if (fc) {
        if (fscanf(fc, "%d", &chg) == 1) {
            is_charging = (chg > 0) ? 1 : 0;
        }
        fclose(fc);
    }

    /* Power source */
    FILE *fp = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/powersource", "r");
    char psrc[16] = {0};
    if (fp) {
        if (fscanf(fp, "%15s", psrc) == 1) {
            strncpy(g_battery_info.powersource, psrc, sizeof(g_battery_info.powersource) - 1);
            if (strcmp(psrc, "cradle") == 0 || strcmp(psrc, "usb") == 0) {
                is_charging = 1;
            } else if (strcmp(psrc, "battery") == 0) {
                is_charging = 0;
            }
        }
        fclose(fp);
    }
    g_battery_info.charging = is_charging;

    /* Reset moving average filter immediately if charging state transitions */
    static uint8_t s_last_charging_flag = 0xFF;
    bool chg_transition = (s_last_charging_flag != 0xFF && s_last_charging_flag != is_charging);
    s_last_charging_flag = is_charging;

    /* Moving average window (16 samples = 16 seconds history) */
    if (s_batt_sample_num == 0 || chg_transition) {
        for (int i = 0; i < BATT_SAMPLE_COUNT; i++) {
            s_batt_samples[i] = raw_mv;
        }
        s_batt_sample_num = BATT_SAMPLE_COUNT;
        s_batt_sample_idx = 0;
    } else {
        s_batt_samples[s_batt_sample_idx] = raw_mv;
        s_batt_sample_idx = (s_batt_sample_idx + 1) % BATT_SAMPLE_COUNT;
    }

    uint32_t sum = 0;
    for (int i = 0; i < BATT_SAMPLE_COUNT; i++) {
        sum += s_batt_samples[i];
    }
    uint16_t avg_mv = (uint16_t)(sum / BATT_SAMPLE_COUNT);
    g_battery_info.voltage_mv = avg_mv;

    /* Calculate Li-ion percentage from smoothed moving average */
    int mv = g_battery_info.voltage_mv;
    int pct = 0;
    if (mv > 5000) {
        /* Scaled to ~8000-8200mV max (6800mV empty, 8200mV full) */
        if (mv >= 8200) pct = 100;
        else if (mv <= 6800) pct = 0;
        else pct = (int)((mv - 6800) * 100 / (8200 - 6800));
    } else {
        /* Standard 1S 3.7V Li-ion (3400mV empty, 4150mV full) */
        if (mv >= 4150) pct = 100;
        else if (mv <= 3400) pct = 0;
        else pct = (int)((mv - 3400) * 100 / (4150 - 3400));
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    /* Damping & Hysteresis to eliminate percentage fluctuation */
    if (s_filtered_pct == 0xFF || chg_transition) {
        s_filtered_pct = (uint8_t)pct;
    } else {
        if (g_battery_info.charging) {
            /* While charging: allow percentage to rise, resist momentary downward dip */
            if (pct > s_filtered_pct) {
                s_filtered_pct = (uint8_t)pct;
            } else if (s_filtered_pct - pct > 4) {
                s_filtered_pct--;
            }
        } else {
            /* While on battery: allow percentage to drop monotonically, ignore momentary load recovery */
            if (pct < s_filtered_pct) {
                s_filtered_pct = (uint8_t)pct;
            } else if (pct - s_filtered_pct > 4) {
                /* Only increase if persistent significant upward change */
                s_filtered_pct = (uint8_t)pct;
            }
        }
    }
    g_battery_info.percent = s_filtered_pct;
}

static void send_battery_status_rf(void) {
    if (g_rf_fd < 0) return;
    read_elite_battery();
    rf_frame_t f;
    uint8_t payload[25] = {0};
    payload[0] = g_battery_info.percent;
    payload[1] = g_battery_info.charging;
    payload[2] = (uint8_t)(g_battery_info.voltage_mv & 0xFF);
    payload[3] = (uint8_t)((g_battery_info.voltage_mv >> 8) & 0xFF);
    rf_build_frame(&f, g_remote_slot, RF_MSG_BATTERY_STATUS, ++g_tx_seq, 0, payload, 4);
    rf_send_frame(&f);
    printf("[*] Sent Battery Status over RF: %d%%, chg=%d, %umV\n",
           g_battery_info.percent, g_battery_info.charging, g_battery_info.voltage_mv);
}

static void send_remote_config_backup_rf(void) {
    if (g_rf_fd < 0) return;
    rf_frame_t f;
    uint8_t payload[25] = {0};
    payload[0] = (uint8_t)g_brightness_val;
    payload[1] = (uint8_t)(g_timeout_seconds & 0xFF);
    payload[2] = (uint8_t)(g_touch_enabled ? 1 : 0);
    payload[3] = (uint8_t)(g_nav_toggle_key & 0xFF);
    payload[4] = (uint8_t)((g_nav_toggle_key >> 8) & 0xFF);
    payload[5] = (uint8_t)(g_nav_toggle_mode & 0xFF);
    payload[6] = (uint8_t)(g_usbeth_enabled ? 1 : 0);
    payload[7] = (uint8_t)(g_confirm_activity ? 1 : 0);
    payload[8] = (uint8_t)g_config_version;
    payload[9] = (uint8_t)g_touch_sensitivity;
    payload[10] = (uint8_t)g_remote_mode;
    payload[11] = (uint8_t)(g_gyro_enabled ? 1 : 0);
    payload[12] = (uint8_t)g_gyro_rate_hz;
    payload[13] = (uint8_t)g_btn_backlight_mode;
    payload[14] = (uint8_t)g_night_start_hour;
    payload[15] = (uint8_t)g_night_end_hour;
    payload[16] = (uint8_t)((g_timeout_seconds >> 8) & 0xFF);
    payload[17] = (uint8_t)(g_cap_buttons_enabled ? 1 : 0);
    rf_build_frame(&f, g_remote_slot, RF_MSG_REMOTE_CONFIG_BACKUP, ++g_tx_seq, 0, payload, 18);
    rf_send_frame(&f);
    printf("[*] Sent remote config backup over RF to Hub (slot %d): bright=%d touch=%d cap=%d gyro=%d rate=%d timeout=%d\n",
           g_remote_slot, g_brightness_val, g_touch_enabled, g_cap_buttons_enabled, g_gyro_enabled, g_gyro_rate_hz, g_timeout_seconds);
}

static void send_version_report_rf(void) {
    if (g_rf_fd < 0) return;
    rf_frame_t f;
    uint8_t payload[25] = {0};
    unsigned int maj = 1, min = 3, pat = 0;
    sscanf(CODEX_ELITE_VERSION, "%u.%u.%u", &maj, &min, &pat);
    payload[0] = (uint8_t)maj;
    payload[1] = (uint8_t)min;
    payload[2] = (uint8_t)pat;
    payload[3] = (uint8_t)g_config_version;
    rf_build_frame(&f, g_remote_slot, RF_MSG_VERSION_REPORT, ++g_tx_seq, 0, payload, 4);
    rf_send_frame(&f);
    printf("[*] Sent 24h version report over RF: v%u.%u.%u (cfg=%u)\n", maj, min, pat, g_config_version);
}

static void stop_usbeth_and_dropbear(void) {
    log_power_debug("[*] Stopping USB Ethernet and Dropbear synchronously...\n");
    system("killall -9 dropbear dropbearmulti 2>/dev/null; /usr/bin/usbeth stop 2>/dev/null; ifconfig usb0 down 2>/dev/null; chmod +x /usr/bin/usbhid 2>/dev/null; true");
    log_power_debug("[+] USB Ethernet stopped and Dropbear killed.\n");
}

static void start_usbeth_and_dropbear(void) {
    log_power_debug("[*] Starting USB Ethernet and Dropbear...\n");
    system("killall -9 usbhid 2>/dev/null; chmod -x /usr/bin/usbhid 2>/dev/null; /usr/bin/usbeth start 2>/dev/null; /data/dropbear -R -p 0.0.0.0:22 2>/dev/null || /data/dropbear -R -B 2>/dev/null || true &");
    log_power_debug("[+] USB Ethernet and Dropbear started.\n");
}

static void toggle_usbeth(void) {
    g_usbeth_enabled = !g_usbeth_enabled;
    if (g_usbeth_enabled) {
        save_settings_conf();
        g_usb_connected = is_usb_hardware_connected();
        if (g_usb_connected) {
            show_notice("Starting USB Ethernet...", 1500);
            render_screen();
            start_usbeth_and_dropbear();
            show_notice("USB Ethernet: ACTIVE", 2000);
        } else {
            stop_usbeth_and_dropbear();
            show_notice("USB Eth ON (unplugged)", 2500);
        }
    } else {
        show_notice("Stopping USB Ethernet...", 1500);
        render_screen();
        stop_usbeth_and_dropbear();
        save_settings_conf();
        show_notice("USB Ethernet: OFF", 2000);
    }
}

/* Draw a filled rectangle in RGB565 into backbuffer */
static void fb_fill_rect(int x, int y, int w, int h, uint16_t color) {
    for (int j = y; j < y + h && j < FB_HEIGHT; j++) {
        if (j < 0) continue;
        for (int i = x; i < x + w && i < FB_WIDTH; i++) {
            if (i < 0) continue;
            g_backbuffer[j * FB_WIDTH + i] = color;
        }
    }
}

/* Draw an outline rectangle in RGB565 into backbuffer */
static void fb_draw_rect(int x, int y, int w, int h, uint16_t color) {
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

/* Draw crisp vector cog icon centered at (cx, cy) */
static void fb_draw_cog(int cx, int cy, uint16_t color) {
    /* Central body 9x9 */
    fb_fill_rect(cx - 4, cy - 4, 9, 9, color);
    /* 4 orthogonal teeth */
    fb_fill_rect(cx - 2, cy - 7, 5, 3, color);
    fb_fill_rect(cx - 2, cy + 5, 5, 3, color);
    fb_fill_rect(cx - 7, cy - 2, 3, 5, color);
    fb_fill_rect(cx + 5, cy - 2, 3, 5, color);
    /* 4 diagonal corner teeth */
    fb_fill_rect(cx - 5, cy - 5, 3, 3, color);
    fb_fill_rect(cx + 3, cy - 5, 3, 3, color);
    fb_fill_rect(cx - 5, cy + 3, 3, 3, color);
    fb_fill_rect(cx + 3, cy + 3, 3, 3, color);
    /* Center hole */
    fb_fill_rect(cx - 1, cy - 1, 3, 3, RGB_BG);
}

/* Draw single character from font table into backbuffer */
static void fb_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale) {
    if ((uint8_t)c < 32 || (uint8_t)c > 126) return;
    const uint8_t *glyph = g_font_8x8[(uint8_t)c - 32];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint16_t color = (bits & (1 << (7 - col))) ? fg : bg;
            if (color != bg || bg != 0) {
                fb_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

/* Draw string using bitmap font into backbuffer */
static void fb_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale) {
    int cur_x = x;
    while (*text) {
        fb_draw_char(cur_x, y, *text, fg, bg, scale);
        cur_x += 8 * scale;
        text++;
    }
}

/* Measure width of anti-aliased text */
static int fb_text_width_aa(const char *text, const font_aa_t *font) {
    if (!text || !font) return 0;
    int w = 0;
    while (*text) {
        uint8_t c = (uint8_t)*text++;
        if (c < 32 || c > 126) { w += 8; continue; }
        w += font->glyphs[c - 32].adv;
    }
    return w;
}

/* Draw anti-aliased proportional string into backbuffer with alpha blending */
static void fb_draw_text_aa(int x, int y, const char *text, uint16_t fg_color, const font_aa_t *font) {
    if (!text || !font) return;
    int pen_x = x;
    int pen_y = y;
    while (*text) {
        uint8_t c = (uint8_t)*text++;
        if (c < 32 || c > 126) {
            pen_x += 8;
            continue;
        }
        const glyph_aa_t *g = &font->glyphs[c - 32];
        if (g->w > 0 && g->h > 0) {
            const uint8_t *src = &font->alpha_data[g->offset];
            int dst_x = pen_x + g->ox;
            int dst_y = pen_y + g->oy;
            for (int r = 0; r < g->h; r++) {
                int py = dst_y + r;
                if (py < 0 || py >= FB_HEIGHT) continue;
                for (int col = 0; col < g->w; col++) {
                    int px = dst_x + col;
                    if (px < 0 || px >= FB_WIDTH) continue;
                    uint8_t alpha = src[r * g->w + col];
                    if (alpha == 0) continue;
                    if (alpha == 255) {
                        g_backbuffer[py * FB_WIDTH + px] = fg_color;
                    } else {
                        uint16_t bg = g_backbuffer[py * FB_WIDTH + px];
                        uint8_t bg_r = ((bg >> 11) & 0x1F) << 3;
                        uint8_t bg_g = ((bg >> 5) & 0x3F) << 2;
                        uint8_t bg_b = (bg & 0x1F) << 3;

                        uint8_t fg_r = ((fg_color >> 11) & 0x1F) << 3;
                        uint8_t fg_g = ((fg_color >> 5) & 0x3F) << 2;
                        uint8_t fg_b = (fg_color & 0x1F) << 3;

                        uint8_t out_r = (fg_r * alpha + bg_r * (255 - alpha)) >> 8;
                        uint8_t out_g = (fg_g * alpha + bg_g * (255 - alpha)) >> 8;
                        uint8_t out_b = (fg_b * alpha + bg_b * (255 - alpha)) >> 8;

                        g_backbuffer[py * FB_WIDTH + px] = ((out_r >> 3) << 11) | ((out_g >> 2) << 5) | (out_b >> 3);
                    }
                }
            }
        }
        pen_x += g->adv;
    }
}

/* Cached PNG asset loader and blitter */
typedef struct {
    char path[128];
    int w, h;
    unsigned char *rgba;
} cached_png_t;

#define MAX_CACHED_PNG 96
static cached_png_t g_png_cache[MAX_CACHED_PNG];
static int g_png_cache_count = 0;

static bool blit_png_file(int dst_x, int dst_y, int target_w, int target_h, const char *filepath) {
    if (!filepath || !filepath[0]) return false;

    unsigned char *data = NULL;
    int w = 0, h = 0;
    bool is_cached = false;

    for (int i = 0; i < g_png_cache_count; i++) {
        if (strcmp(g_png_cache[i].path, filepath) == 0) {
            data = g_png_cache[i].rgba;
            w = g_png_cache[i].w;
            h = g_png_cache[i].h;
            is_cached = true;
            break;
        }
    }

    if (!data) {
        int comp = 0;
        data = stbi_load(filepath, &w, &h, &comp, 4);
        if (!data) return false;
        if (g_png_cache_count < MAX_CACHED_PNG) {
            strncpy(g_png_cache[g_png_cache_count].path, filepath, sizeof(g_png_cache[0].path) - 1);
            g_png_cache[g_png_cache_count].w = w;
            g_png_cache[g_png_cache_count].h = h;
            g_png_cache[g_png_cache_count].rgba = data;
            g_png_cache_count++;
            is_cached = true;
        }
    }

    for (int ty = 0; ty < target_h; ty++) {
        int py = dst_y + ty;
        if (py < 0 || py >= FB_HEIGHT) continue;
        int sy = (ty * h) / target_h;
        for (int tx = 0; tx < target_w; tx++) {
            int px = dst_x + tx;
            if (px < 0 || px >= FB_WIDTH) continue;
            int sx = (tx * w) / target_w;

            unsigned char *pixel = &data[(sy * w + sx) * 4];
            uint8_t a = pixel[3];
            if (a == 0) continue;

            uint8_t r = pixel[0];
            uint8_t g = pixel[1];
            uint8_t b = pixel[2];

            /* Stock Logitech icons are black line-art with alpha on transparent canvas.
             * On dark card backgrounds, convert dark glyph pixels to crisp white so they shine clearly! */
            if ((r + g + b) < 100 && a > 30) {
                r = 245;
                g = 245;
                b = 250;
            }

            if (a == 255) {
                g_backbuffer[py * FB_WIDTH + px] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            } else {
                uint16_t bg = g_backbuffer[py * FB_WIDTH + px];
                uint8_t bg_r = ((bg >> 11) & 0x1F) << 3;
                uint8_t bg_g = ((bg >> 5) & 0x3F) << 2;
                uint8_t bg_b = (bg & 0x1F) << 3;

                uint8_t out_r = (r * a + bg_r * (255 - a)) >> 8;
                uint8_t out_g = (g * a + bg_g * (255 - a)) >> 8;
                uint8_t out_b = (b * a + bg_b * (255 - a)) >> 8;

                g_backbuffer[py * FB_WIDTH + px] = ((out_r >> 3) << 11) | ((out_g >> 2) << 5) | (out_b >> 3);
            }
        }
    }
    if (!is_cached) {
        stbi_image_free(data);
    }
    return true;
}

static void load_wallpaper_asset(void) {
    const char *paths[] = {
        "/data/background.png",
        "/opt/luadraws/appscommon/asset/background/b8bb2074-131e-11e2-ae9f-b3f5f4c18767.png",
        "/opt/luadraws/appscommon/asset/background/8d8f352a-131e-11e2-8581-b78bf89f9455.png",
        "/opt/luadraws/appscommon/asset/background/9927055c-131e-11e2-b78a-63d07f7b7808.png",
        "/opt/luadraws/appscommon/asset/background/9f67b146-131e-11e2-9510-7f6da0f7f23b.png",
        "/opt/luadraws/appscommon/asset/background/ab969216-131e-11e2-abe8-9710f8cc7eb0.png",
        "/opt/luadraws/appscommon/asset/background/b2bda41c-131e-11e2-9a42-b76c2c03ce1b.png",
        "/opt/luadraws/appscommon/asset/background/c27fb7a6-131d-11e2-8085-bb50198c538b.png",
        "/opt/luadraws/appscommon/asset/background/c6848614-131e-11e2-8672-dbd27517bd91.png",
        "/opt/luadraws/appscommon/asset/background/cd4e7be4-131e-11e2-888a-df753ee9c6df.png"
    };
    for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        int w = 0, h = 0, comp = 0;
        unsigned char *data = stbi_load(paths[i], &w, &h, &comp, 3);
        if (data && w == FB_WIDTH && h == FB_HEIGHT) {
            for (int j = 0; j < FB_WIDTH * FB_HEIGHT; j++) {
                uint8_t r = data[j * 3 + 0];
                uint8_t g = data[j * 3 + 1];
                uint8_t b = data[j * 3 + 2];
                g_bg_wallpaper[j] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
            stbi_image_free(data);
            g_has_wallpaper = true;
            printf("[+] Loaded stock wallpaper from %s\n", paths[i]);
            return;
        }
        if (data) stbi_image_free(data);
    }

    /* Fallback: sleek dark carbon-mesh gradient matching stock Logitech look */
    for (int y = 0; y < FB_HEIGHT; y++) {
        for (int x = 0; x < FB_WIDTH; x++) {
            uint8_t grad = (uint8_t)(14 + (y * 14) / FB_HEIGHT);
            if (((x / 2 + y / 2) & 1) == 0) grad += 3;
            g_bg_wallpaper[y * FB_WIDTH + x] = ((grad >> 3) << 11) | (((grad + 2) >> 2) << 5) | ((grad + 5) >> 3);
        }
    }
    g_has_wallpaper = true;
    printf("[*] Initialized high-res carbon mesh wallpaper\n");
}

/* Resolve Activity icon from firmware assets (GUID or semantic alias) */
static void resolve_activity_icon(const char *image_key, const char *clean_name, char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return;
    out_path[0] = '\0';

    char base[64] = {0};
    if (image_key && image_key[0]) {
        const char *slash = strrchr(image_key, '/');
        const char *fn = slash ? slash + 1 : image_key;
        strncpy(base, fn, sizeof(base) - 1);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        size_t blen = strlen(base);
        if (blen > 6 && strcmp(base + blen - 6, "_small") == 0) {
            base[blen - 6] = '\0';
        }
    }

    if (base[0]) {
        char test_path[128];
        snprintf(test_path, sizeof(test_path), "/opt/luadraws/appscommon/asset/juniperactivity/%s_small.png", base);
        if (access(test_path, R_OK) == 0) {
            strncpy(out_path, test_path, max_len - 1);
            return;
        }
        snprintf(test_path, sizeof(test_path), "/opt/luadraws/appscommon/asset/juniperactivity/%s.png", base);
        if (access(test_path, R_OK) == 0) {
            strncpy(out_path, test_path, max_len - 1);
            return;
        }
        snprintf(test_path, sizeof(test_path), "/opt/luadraws/appscommon/asset/juniperDevice/%s.png", base);
        if (access(test_path, R_OK) == 0) {
            strncpy(out_path, test_path, max_len - 1);
            return;
        }
        snprintf(test_path, sizeof(test_path), "/opt/luadraws/appscommon/asset/hopsactivity/%s_small.png", base);
        if (access(test_path, R_OK) == 0) {
            strncpy(out_path, test_path, max_len - 1);
            return;
        }
    }

    char low[64] = {0};
    const char *src = base[0] ? base : (clean_name ? clean_name : "");
    for (int k = 0; src[k] && k < 63; k++) low[k] = tolower((unsigned char)src[k]);

    if (strstr(low, "watchtv") || strstr(low, "tv") || strstr(low, "television")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/3BF99E7B-9904-4AFA-8D3D-7337CD8E9BFD_small.png");
    } else if (strstr(low, "movie") || strstr(low, "cinema") || strstr(low, "stream") || strstr(low, "netflix") || strstr(low, "plex")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/BAB09C4A-E61D-4284-B1F3-84135F562217_small.png");
    } else if (strstr(low, "switch") || strstr(low, "nintendo") || strstr(low, "game") || strstr(low, "console")) {
        if (access("/data/assets/switch.png", R_OK) == 0) {
            snprintf(out_path, max_len, "/data/assets/switch.png");
        } else if (access("/opt/luadraws/appscommon/asset/hopsactivity/9D24EB35-468B-4148-A6A2-BB764AA9609C_small.png", R_OK) == 0) {
            snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/hopsactivity/9D24EB35-468B-4148-A6A2-BB764AA9609C_small.png");
        } else {
            snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/9D24EB35-468B-4148-A6A2-BB764AA9609C_small.png");
        }
    } else if (strstr(low, "ps") || strstr(low, "playstation")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/DEC07396-B858-47E8-AE50-4AB346AF1D98_small.png");
    } else if (strstr(low, "xbox")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/xbox.png");
    } else if (strstr(low, "shield")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/shield.png");
    } else if (strstr(low, "music") || strstr(low, "audio") || strstr(low, "radio") || strstr(low, "spotify")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/72D0AAB4-2ED8-4644-84D2-0031BDAE356E_small.png");
    } else if (strstr(low, "apple")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/0F40F257-68C8-4F24-9561-3CB3441DA395_small.png");
    } else if (strstr(low, "roku")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/6EBD03B8-C815-48BE-ADBB-D36E06CAAC5E_small.png");
    } else if (strstr(low, "firetv") || strstr(low, "amazon") || strstr(low, "fire")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/5F5E0C9D-1D5A-4F6F-9DD4-243AFCCD8285_small.png");
    } else if (strstr(low, "sonos")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/sonos.png");
    } else if (strstr(low, "heos") || strstr(low, "denon")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/heos.png");
    } else if (strstr(low, "pc") || strstr(low, "computer") || strstr(low, "laptop")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/468668AB-2587-4662-9E89-CF66BD70F314_small.png");
    } else if (strstr(low, "power") || strstr(low, "off")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/116.png");
    } else {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperactivity/53469038-7E1C-4241-B9BD-BAE95CC0509C_small.png");
    }
}

/* Resolve On-Screen Button icon from firmware stock assets */
static bool resolve_button_icon(const char *icon_key, const char *label, char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return false;
    out_path[0] = '\0';

    char low[48] = {0};
    const char *src = (icon_key && icon_key[0]) ? icon_key : (label ? label : "");
    for (int k = 0; src[k] && k < 47; k++) low[k] = tolower((unsigned char)src[k]);

    /* If purely numeric (FunctionId), check commands/<num>.png */
    bool is_num = (low[0] >= '0' && low[0] <= '9');
    for (int i = 0; low[i]; i++) {
        if (low[i] < '0' || low[i] > '9') { is_num = false; break; }
    }
    if (is_num && low[0]) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/%s.png", low);
        return true;
    }

    if (strstr(low, "power") || strstr(low, "pwr") || strstr(low, "standby")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/116.png");
    } else if (strstr(low, "playpause")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/playpause.png");
    } else if (strstr(low, "play")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/113.png");
    } else if (strstr(low, "pause")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/106.png");
    } else if (strstr(low, "stop")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/footer/close.png");
    } else if (strstr(low, "ff") || strstr(low, "fastforward") || strstr(low, "fwd")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/76.png");
    } else if (strstr(low, "rew") || strstr(low, "rewind")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/131.png");
    } else if (strstr(low, "next") || strstr(low, "skipfwd") || strstr(low, "skip+")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/144.png");
    } else if (strstr(low, "prev") || strstr(low, "skiprev") || strstr(low, "skip-")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/143.png");
    } else if (strstr(low, "mute")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/91.png");
    } else if (strstr(low, "vol+") || strstr(low, "volup") || strstr(low, "volume up")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/171.png");
    } else if (strstr(low, "vol-") || strstr(low, "voldn") || strstr(low, "volume down")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/166.png");
    } else if (strstr(low, "ch+") || strstr(low, "channel up")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/52.png");
    } else if (strstr(low, "ch-") || strstr(low, "channel down")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/51.png");
    } else if (strstr(low, "home")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/195.png");
    } else if (strstr(low, "menu")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/90.png");
    } else if (strstr(low, "back") || strstr(low, "return")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/39.png");
    } else if (strstr(low, "exit")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/75.png");
    } else if (strstr(low, "info")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/85.png");
    } else if (strstr(low, "search")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/135.png");
    } else if (strstr(low, "denon") || strstr(low, "avr") || strstr(low, "receiver") || strstr(low, "amp")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/5.png");
    } else if (strstr(low, "shield")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/shield.png");
    } else if (strstr(low, "ps") || strstr(low, "playstation")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/ps.png");
    } else if (strstr(low, "xbox")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/xbox.png");
    } else if (strstr(low, "tv")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/1.png");
    } else if (strstr(low, "sonos")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/sonos.png");
    } else if (strstr(low, "firetv") || strstr(low, "fire")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperDevice/firetv.png");
    } else if (strstr(low, "light") || strstr(low, "lamp") || strstr(low, "bulb")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperHomeAutomation/bulb_on.png");
    } else if (strstr(low, "plug")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperHomeAutomation/plug_on.png");
    } else if (strstr(low, "blind") || strstr(low, "shade")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/juniperHomeAutomation/blinds_on.png");
    } else if (strstr(low, "numpad") || strstr(low, "123")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/numpad.png");
    } else if (strstr(low, "seq") || strstr(low, "macro")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/sequence.png");
    } else if (strstr(low, "red")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/127.png");
    } else if (strstr(low, "green")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/83.png");
    } else if (strstr(low, "yellow")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/172.png");
    } else if (strstr(low, "blue")) {
        snprintf(out_path, max_len, "/opt/luadraws/appscommon/asset/commands/40.png");
    } else {
        return false;
    }
    return true;
}

/* Parse activities from local JSON cache */
static void load_activities(void) {
    g_activity_count = 0;
    const char *act_path = ACTIVITY_JSON_FILE;
    if (access(act_path, F_OK) != 0) act_path = "/data/codex/config/activities.json";
    if (access(act_path, F_OK) != 0) act_path = STOCK_ACTIVITY_JSON_FILE;
    FILE *f = fopen(act_path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return; }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *acts = cJSON_GetObjectItem(root, "activities");
    if (!acts) acts = cJSON_GetObjectItem(root, "Activities");
    if (acts && cJSON_IsArray(acts)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, acts) {
            if (g_activity_count >= MAX_ACTIVITIES - 1) break;
            cJSON *j_name = cJSON_GetObjectItem(item, "name");
            if (!j_name) j_name = cJSON_GetObjectItem(item, "Name");
            cJSON *j_id = cJSON_GetObjectItem(item, "id");
            if (!j_id) j_id = cJSON_GetObjectItem(item, "Id-");
            if (!j_id) j_id = cJSON_GetObjectItem(item, "Id");


            if (j_name && cJSON_IsString(j_name) && j_id) {
                const char *n = j_name->valuestring;
                char id_str[32] = {0};
                if (cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%d", j_id->valueint);
                else if (cJSON_IsString(j_id)) strncpy(id_str, j_id->valuestring, sizeof(id_str) - 1);

                /* Replace non-breaking space \xA0 or \xC2\xA0 with standard space */
                char clean_name[48] = {0};
                int ci = 0;
                for (int i = 0; n[i] && ci < 47; i++) {
                    if ((unsigned char)n[i] == 0xA0) {
                        clean_name[ci++] = ' ';
                    } else if ((unsigned char)n[i] == 0xC2 && (unsigned char)n[i+1] == 0xA0) {
                        clean_name[ci++] = ' ';
                        i++;
                    } else {
                        clean_name[ci++] = n[i];
                    }
                }

                cJSON *j_img = cJSON_GetObjectItem(item, "ImageKey");
                if (!j_img) j_img = cJSON_GetObjectItem(item, "icon");
                const char *img_key = (j_img && cJSON_IsString(j_img)) ? j_img->valuestring : NULL;

                char icon_file[128] = {0};
                resolve_activity_icon(img_key, clean_name, icon_file, sizeof(icon_file));

                strncpy(g_activities[g_activity_count].name, clean_name, sizeof(g_activities[0].name) - 1);
                strncpy(g_activities[g_activity_count].id, id_str, sizeof(g_activities[0].id) - 1);
                strncpy(g_activities[g_activity_count].icon_path, icon_file, sizeof(g_activities[0].icon_path) - 1);
                g_activity_count++;
            }
        }
    }
    cJSON_Delete(root);

    /* Always ensure Power Off option exists at end if activities were loaded */
    if (g_activity_count > 0) {
        bool has_power_off = false;
        for (int i = 0; i < g_activity_count; i++) {
            if (strcmp(g_activities[i].id, "-1") == 0) { has_power_off = true; break; }
        }
        if (!has_power_off && g_activity_count < MAX_ACTIVITIES) {
            strncpy(g_activities[g_activity_count].id, "-1", sizeof(g_activities[0].id) - 1);
            strncpy(g_activities[g_activity_count].name, "Power Off", sizeof(g_activities[0].name) - 1);
            strncpy(g_activities[g_activity_count].icon_path, "/opt/luadraws/appscommon/asset/commands/116.png", sizeof(g_activities[0].icon_path) - 1);
            g_activity_count++;
        }
    }
}

/* Map Linux event0 keycode to Logitech Harmony RF key code */
static uint16_t linux_to_harmony_rf_key(int code) {
    switch (code) {
        /* Activities & Power */
        case 116: return 0x01EC; /* KEY_POWER -> PowerOffActivity */

        /* Navigation */
        case 103: return 0x0052; /* KEY_UP -> DirectionUp */
        case 108: return 0x0051; /* KEY_DOWN -> DirectionDown */
        case 105: return 0x0050; /* KEY_LEFT -> DirectionLeft */
        case 106: return 0x004F; /* KEY_RIGHT -> DirectionRight */
        case 352: /* KEY_OK */
        case 28:  return 0x0058; /* KEY_ENTER -> Select */
        case 174: return 0x0094; /* KEY_EXIT -> Exit */
        case 158: return 0x0225; /* KEY_BACK -> Back */
        case 139: /* KEY_MENU */
        case 127: return 0x0065; /* KEY_COMPOSE -> Menu */
        case 358: return 0x01FF; /* KEY_INFO -> Info */
        case 395: return 0x008D; /* KEY_LIST -> Guide */
        case 389: return 0x009A; /* KEY_ORIGINAL -> Dvr */

        /* Volume & Channel */
        case 115: return 0x00E9; /* KEY_VOLUMEUP -> VolumeUp */
        case 114: return 0x00EA; /* KEY_VOLUMEDOWN -> VolumeDown */
        case 113: return 0x00E2; /* KEY_MUTE -> VolumeMute */
        case 402: return 0x009C; /* KEY_CHANNELUP -> ChannelUp */
        case 403: return 0x009D; /* KEY_CHANNELDOWN -> ChannelDown */
        case 412: return 0x0224; /* KEY_PREVIOUS -> PrevChannel */

        /* Transport / Playback */
        case 207: return 0x00B0; /* KEY_PLAY -> Play */
        case 119: return 0x00B1; /* KEY_PAUSE -> Pause */
        case 167: return 0x00B2; /* KEY_RECORD -> Record */
        case 159: return 0x00B3; /* KEY_FORWARD -> FastForward */
        case 168: return 0x00B4; /* KEY_REWIND -> Rewind */
        case 128: return 0x00B7; /* KEY_STOP -> Stop */

        /* Color Buttons */
        case 398: return 0x01F7; /* KEY_RED -> Red */
        case 399: return 0x01F6; /* KEY_GREEN -> Green */
        case 400: return 0x01F5; /* KEY_YELLOW -> Yellow */
        case 401: return 0x01F4; /* KEY_BLUE -> Blue */

        /* Home Automation */
        case 148: return 0x0FF2; /* KEY_PROG1 -> Light On */
        case 149: return 0x0FF3; /* KEY_PROG2 -> Light Off */
        case 202: return 0x0FF4; /* KEY_PROG3 -> Plug On */
        case 203: return 0x0FF5; /* KEY_PROG4 -> Plug Off */
        case 78:  return 0x0FF0; /* KEY_KPPLUS -> RockerUp */
        case 74:  return 0x0FF1; /* KEY_KPMINUS -> RockerDown */

        default:  return 0;
    }
}

static void rf_send_button_event(uint16_t key_code, int value);

static uint16_t g_held_rf_code = 0;
static pthread_mutex_t g_repeat_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_repeat_cond = PTHREAD_COND_INITIALIZER;

static void *key_repeat_thread(void *arg) {
    (void)arg;
    while (g_running) {
        pthread_mutex_lock(&g_repeat_mutex);
        while (g_held_rf_code == 0 && g_running) {
            pthread_cond_wait(&g_repeat_cond, &g_repeat_mutex);
        }
        if (!g_running) {
            pthread_mutex_unlock(&g_repeat_mutex);
            break;
        }
        uint16_t key = g_held_rf_code;
        pthread_mutex_unlock(&g_repeat_mutex);

        /* 300ms initial hold delay */
        usleep(300000);

        while (g_running) {
            pthread_mutex_lock(&g_repeat_mutex);
            if (g_held_rf_code != key) {
                pthread_mutex_unlock(&g_repeat_mutex);
                break;
            }
            pthread_mutex_unlock(&g_repeat_mutex);

            /* Transmit repeated press event */
            rf_send_button_event(key, 1);
            usleep(100000); /* 100ms repeat rate */
        }
    }
    return NULL;
}

static pthread_mutex_t g_rf_write_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_pairing_state = 0; /* 0=idle, 1=pairing, 2=paired, 3=failed */
static volatile uint64_t g_last_hub_frame_ms = 0;
static uint8_t g_remote_own_addr[4] = {0};
static uint8_t g_paired_hub_addr[4] = {0};
static bool g_unpair_confirm_active = false;

/* Send a Codex RF frame over /dev/rfspi using stock 32-byte Report 0x20 (Remote device mode framing matching hal_remote) */
static int rf_send_frame(rf_frame_t *frame) {
    if (g_rf_fd < 0 || !frame) return -1;
    uint8_t out[32];
    memset(out, 0, sizeof(out));
    uint8_t slot = (g_remote_slot >= 1 && g_remote_slot <= 15) ? g_remote_slot : 0x01;
    out[0] = 0x20; /* Remote CC2544 uplink Report 0x20 (Device mode) */
    out[1] = 0x20 | (slot & 0x0F); /* 0x20 | pipe (Hub connection) */
    out[2] = 0x12; /* Sub-ID 0x12 */
    out[3] = (slot & 0x0F); /* Host sequence/pipe */
    out[4] = frame->hdr_flag ? frame->hdr_flag : (0x50 | (frame->seq & 0x0F));
    out[5] = frame->msg_type;
    out[6] = frame->flags;
    memcpy(&out[7], frame->payload, 25);

    if (g_log_level >= LOG_LEVEL_DEBUG) {
        printf("[*] RF TX (32 bytes Report 0x20): ");
        for (int i = 0; i < 32; i++) printf("%02X ", out[i]);
        printf("\n");
        fflush(stdout);
    }

    pthread_mutex_lock(&g_rf_write_mutex);
    int ret = write(g_rf_fd, out, 32);
    pthread_mutex_unlock(&g_rf_write_mutex);
    return ret;
}

/* Send activity sync query to Hub */
static void rf_send_sync_query(void) {
    if (g_rf_fd < 0) return;
    rf_frame_t f;
    uint8_t payload[25] = {0};
    payload[0] = 0x01; /* activity only */
    rf_build_frame(&f, g_remote_slot, RF_MSG_SYNC_REQUEST, ++g_tx_seq, 0, payload, 1);
    rf_send_frame(&f);
    printf("[*] RF TX SYNC_REQUEST seq=%d slot=%d (byte1=0x%02X)\n", f.seq, f.dev_slot & 0x0F, f.dev_slot);
    fflush(stdout);
}

/* Request full config from Hub */
static void rf_send_full_sync_request(void) {
    if (g_rf_fd < 0 || g_is_syncing_config || g_is_ota_updating) return;
    uint64_t now = get_monotonic_ms();
    static uint64_t last_sync_req_ms = 0;
    if (now - last_sync_req_ms < 5000) return;
    last_sync_req_ms = now;

    rf_frame_t f;
    uint8_t payload[25] = {0};
    payload[0] = 0x02; /* full config */
    rf_build_frame(&f, g_remote_slot, RF_MSG_SYNC_REQUEST, ++g_tx_seq, 0, payload, 1);
    rf_send_frame(&f);
    printf("[*] RF TX FULL_SYNC_REQUEST seq=%d slot=%d (byte1=0x%02X)\n", f.seq, f.dev_slot & 0x0F, f.dev_slot);
    fflush(stdout);
}

/* Wake CC2544 radio and re-sync with Hub */
static void rf_wake_and_reconnect(void) {
    if (g_rf_fd < 0) return;
    s_last_reconnect_ms = get_monotonic_ms();
    /* Dongle Active Sync pulse (matches stock hal_remote & AGENTS.md Rule 3) */
    uint8_t pkt_dongle[10] = {0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    pthread_mutex_lock(&g_rf_write_mutex);
    if (write(g_rf_fd, pkt_dongle, sizeof(pkt_dongle)) < 0) {}
    pthread_mutex_unlock(&g_rf_write_mutex);
    usleep(5000);
    /* Send activity sync query to re-establish active session */
    rf_send_sync_query();
}

/* Worker thread: initiates pairing mode and retries command every second matching stock pairing.lua */
static void *rf_pairing_worker(void *arg) {
    (void)arg;
    int attempts = 0;
    while (g_running && g_pairing_state == 1 && attempts < 30) {
        uint8_t pkt_pair[7] = {0x10, 0xFF, 0x80, 0xB2, 0x01, 0x01, 0x00};
        pthread_mutex_lock(&g_rf_write_mutex);
        if (write(g_rf_fd, pkt_pair, sizeof(pkt_pair)) < 0) {
            perror("write pkt_pair");
        }
        pthread_mutex_unlock(&g_rf_write_mutex);

        sleep(1);
        attempts++;
        if (g_pairing_state != 1) break;

        /* Query paired Hub info on pipe 0x20 */
        uint8_t q_hub[7] = {0x10, 0xFF, 0x83, 0xB5, 0x20, 0x00, 0x00};
        pthread_mutex_lock(&g_rf_write_mutex);
        if (write(g_rf_fd, q_hub, sizeof(q_hub)) < 0) {}
        pthread_mutex_unlock(&g_rf_write_mutex);
    }
    if (g_pairing_state == 1) {
        printf("[-] Pairing timed out after 30s\n");
        g_pairing_state = 3;
        render_screen();
    }
    return NULL;
}

/* Start RF pairing to Hub */
static void rf_start_pairing(void) {
    if (g_rf_fd < 0) return;
    g_pairing_state = 1; /* In progress */
    g_rf_link_connected = false;

    printf("[*] Initiating RF Pairing to Hub...\n");
    pthread_t th;
    if (pthread_create(&th, NULL, rf_pairing_worker, NULL) == 0) {
        pthread_detach(th);
    }
    fflush(stdout);
}

/* Stop pairing mode on Remote */
static void rf_stop_pairing(void) {
    if (g_rf_fd < 0) return;
    printf("[*] Stopping pairing mode on Remote...\n");
    uint8_t pkt_close[7] = {0x10, 0xFF, 0x80, 0xB2, 0x02, 0x00, 0x00};
    if (write(g_rf_fd, pkt_close, sizeof(pkt_close)) < 0) {}
    g_pairing_state = 0;
    render_screen();
}

/* Unpair from Hub: completely wipe bonding from CC2544 flash and clear local config */
static void rf_unpair(void) {
    if (g_rf_fd < 0) return;

    /* Notify Hub over RF before removing bonding */
    rf_frame_t unpair_f;
    memset(&unpair_f, 0, sizeof(unpair_f));
    unpair_f.report_id = 0x20;
    unpair_f.sub_id = 0x12;
    unpair_f.msg_type = RF_MSG_UNPAIR_NOTIFY;
    unpair_f.hdr_flag = 0x50 | (g_tx_seq & 0x0F);
    unpair_f.seq = g_tx_seq++;
    rf_send_frame(&unpair_f);
    usleep(30000);

    g_pairing_state = 0;
    g_rf_link_connected = false;

    printf("[*] Unpairing & removing bonding: entering test mode and clearing CC2544 stored device pipes...\n");
    pthread_mutex_lock(&g_rf_write_mutex);

    /* 1. Enter Test Mode: 10 FF 80 D0 01 00 00 (matches stock hal_remote 0x1a1b4) */
    uint8_t enter_test[7] = {0x10, 0xFF, 0x80, 0xD0, 0x01, 0x00, 0x00};
    if (write(g_rf_fd, enter_test, sizeof(enter_test)) < 0) {}
    usleep(100000);

    /* 2. Clear pipe 0x20: 11 FF 82 B5 20 00 ... */
    uint8_t clr_pipe20[20] = {0x11, 0xFF, 0x82, 0xB5, 0x20, 0};
    if (write(g_rf_fd, clr_pipe20, sizeof(clr_pipe20)) < 0) {}
    usleep(100000);

    /* 3. Clear pipe 0x30: 11 FF 82 B5 30 00 ... */
    uint8_t clr_pipe30[20] = {0x11, 0xFF, 0x82, 0xB5, 0x30, 0};
    if (write(g_rf_fd, clr_pipe30, sizeof(clr_pipe30)) < 0) {}
    usleep(100000);

    /* 4. Disable Test Mode: 10 FF 80 D0 00 00 00 */
    uint8_t exit_test[7] = {0x10, 0xFF, 0x80, 0xD0, 0x00, 0x00, 0x00};
    if (write(g_rf_fd, exit_test, sizeof(exit_test)) < 0) {}
    usleep(100000);

    pthread_mutex_unlock(&g_rf_write_mutex);

    /* 5. Wipe custom config files (NEVER delete stock files in /data/resources/) */
    unlink(RF_CONFIG_FILE);
    unlink(ACTIVITY_JSON_FILE);


    /* 7. Reset all in-memory config and RF state */
    g_has_hub_config = false;
    g_activity_count = 0;
    g_rf_activity_count = 0;
    g_rf_device_count = 0;
    g_rf_button_count = 0;
    memset(g_activities, 0, sizeof(g_activities));
    memset(g_rf_activities, 0, sizeof(g_rf_activities));
    memset(g_rf_devices, 0, sizeof(g_rf_devices));
    memset(g_rf_buttons, 0, sizeof(g_rf_buttons));
    memset(g_paired_hub_addr, 0, sizeof(g_paired_hub_addr));
    g_active_activity_id[0] = '\0';
    g_active_activity_name[0] = '\0';
    g_config_version = 0;
    g_unpair_confirm_active = false;

    printf("[+] Remote unpairing & bonding removal complete. Ready to pair to new hub.\n");
    render_screen();
}

/* Transmit button event over RF to Hub */
static void rf_send_button_event(uint16_t key_code, int value) {
    if (key_code == 0 || g_rf_fd < 0) return;
    if (value < 0 || value > 2) return;

    const char *short_name = rf_short_button_name(key_code);
    uint8_t state = (value == 1) ? 0x01 : ((value == 2) ? 0x02 : 0x00);

    rf_frame_t f;
    uint8_t payload[26] = {0};
    payload[0] = (uint8_t)(key_code >> 8);
    payload[1] = (uint8_t)(key_code & 0xFF);
    payload[2] = state;
    strncpy((char *)&payload[3], short_name, 12);
    rf_build_frame(&f, g_remote_slot, RF_MSG_BUTTON_PRESS, ++g_tx_seq, 0, payload, 15);

    rf_send_frame(&f);

    printf("[*] RF TX BUTTON_PRESS: 0x%04X %s %s\n", key_code,
           state == 0x01 ? "press" : (state == 0x02 ? "hold" : "release"), short_name);
}

/* Transmit Activity Switch command over RF */
static void rf_send_activity_start(const char *act_id) {
    if (g_rf_fd < 0) return;

    rf_frame_t f;
    uint8_t payload[26] = {0};
    strncpy((char *)payload, act_id, 12);
    rf_build_frame(&f, g_remote_slot, RF_MSG_ACTIVITY_REQUEST, ++g_tx_seq, 0, payload, 12);

    rf_send_frame(&f);

    printf("[*] RF TX ACTIVITY_REQUEST: %s\n", act_id);
}

/* Transmit Device command over RF */
static void rf_send_device_command(const char *dev_id, const char *cmd) {
    if (g_rf_fd < 0) return;

    rf_frame_t f;
    uint8_t payload[26] = {0};
    strncpy((char *)payload, dev_id, 10);
    strncpy((char *)&payload[10], cmd, 16);
    rf_build_frame(&f, g_remote_slot, RF_MSG_DEVICE_CMD_REQUEST, ++g_tx_seq, 0, payload, 26);

    rf_send_frame(&f);

    printf("[*] RF TX DEVICE_CMD: dev=%s cmd=%s\n", dev_id, cmd);
}

static int build_combined_activity_sequence(const char *from_id, const char *to_id,
                                            rf_act_step_t *out_steps, int max_steps,
                                            uint32_t *out_total_ms) {
    int count = 0;
    uint32_t total = 0;

    /* Phase 1: Departing activity stop sequence */
    if (from_id && from_id[0] && (!to_id || strcmp(from_id, to_id) != 0)) {
        int found_stop = 0;
        for (int i = 0; i < g_rf_step_count && count < max_steps; i++) {
            if (strcmp(g_rf_steps[i].act_id, from_id) == 0 && (g_rf_steps[i].step_idx & 0x80)) {
                out_steps[count] = g_rf_steps[i];
                if (strncmp(out_steps[count].name, "Power Off O", 11) == 0 ||
                    strcmp(out_steps[count].name, "Power Off Old") == 0) {
                    strncpy(out_steps[count].name, "Stop old act", sizeof(out_steps[count].name) - 1);
                } else if (strcasecmp(out_steps[count].name, "Switched") == 0) {
                    strncpy(out_steps[count].name, "Activity switched", sizeof(out_steps[count].name) - 1);
                }
                total += out_steps[count].delay_ms;
                count++;
                found_stop = 1;
            }
        }
        if (!found_stop && count < max_steps && strcmp(from_id, "-1") != 0) {
            strncpy(out_steps[count].act_id, from_id, sizeof(out_steps[count].act_id) - 1);
            out_steps[count].step_idx = 0x80;
            out_steps[count].delay_ms = 1500;
            strncpy(out_steps[count].name, "Stop old act", sizeof(out_steps[count].name) - 1);
            total += 1500;
            count++;
        }
    }

    /* Phase 2: Entering activity start sequence (including Switched marker & tail steps) */
    if (to_id && to_id[0]) {
        for (int i = 0; i < g_rf_step_count && count < max_steps; i++) {
            if (strcmp(g_rf_steps[i].act_id, to_id) == 0 && !(g_rf_steps[i].step_idx & 0x80)) {
                out_steps[count] = g_rf_steps[i];
                if (strcasecmp(out_steps[count].name, "Switched") == 0) {
                    strncpy(out_steps[count].name, "Activity switched", sizeof(out_steps[count].name) - 1);
                }
                total += out_steps[count].delay_ms;
                count++;
            }
        }
    }

    if (total < 2000) total = 3500;
    if (out_total_ms) *out_total_ms = total;
    return count;
}

static void trigger_activity_switch(const char *id, const char *name) {
    if (!g_rf_link_connected) {
        printf("[!] Cannot switch activity '%s': Hub RF connection lost\n", name ? name : "");
        rf_send_sync_query();
        g_screen_dirty = true;
        render_screen();
        return;
    }
    g_is_switching_activity = true;
    strncpy(g_switching_target_id, id ? id : "-1", sizeof(g_switching_target_id) - 1);
    strncpy(g_switching_target_name, name ? name : "Activity", sizeof(g_switching_target_name) - 1);
    g_switching_start_ms = get_monotonic_ms();

    rf_act_step_t combined_steps[32];
    uint32_t total_delay = 0;
    int step_cnt = build_combined_activity_sequence(g_active_activity_id, g_switching_target_id,
                                                   combined_steps, 32, &total_delay);
    g_switching_total_ms = total_delay;
    printf("[*] Starting activity switch to '%s' (%s): %d combined steps (stop+start), %ums total delay\n",
           g_switching_target_name, g_switching_target_id, step_cnt, g_switching_total_ms);

    rf_send_activity_start(id ? id : "-1");
    render_screen();
}

static void trigger_activity_switch_external(const char *id, const char *name) {
    g_is_switching_activity = true;
    strncpy(g_switching_target_id, id ? id : "-1", sizeof(g_switching_target_id) - 1);
    strncpy(g_switching_target_name, name ? name : "Activity", sizeof(g_switching_target_name) - 1);
    g_switching_start_ms = get_monotonic_ms();

    rf_act_step_t combined_steps[32];
    uint32_t total_delay = 0;
    int step_cnt = build_combined_activity_sequence(g_active_activity_id, g_switching_target_id,
                                                   combined_steps, 32, &total_delay);
    g_switching_total_ms = total_delay;
    printf("[*] External activity switch started for '%s' (%s): %d combined steps (stop+start), %ums total delay\n",
           g_switching_target_name, g_switching_target_id, step_cnt, g_switching_total_ms);

    g_screen_dirty = true;
}

static void request_activity_switch_with_confirm(int act_idx) {
    if (act_idx < 0 || act_idx >= g_activity_count) return;
    activity_item_t *sel = &g_activities[act_idx];

    /* Item 4: Prevent double execution of already running activity */
    if (strcmp(g_active_activity_id, sel->id) == 0) {
        show_notice("Activity already active", 2000);
        render_screen();
        return;
    }

    if (!g_confirm_activity) {
        trigger_activity_switch(sel->id, sel->name);
        return;
    }

    uint64_t now = get_monotonic_ms();
    if (g_confirm_pending_idx == act_idx && now < g_confirm_pending_deadline_ms) {
        g_confirm_pending_idx = -1;
        g_confirm_pending_deadline_ms = 0;
        trigger_activity_switch(sel->id, sel->name);
    } else {
        g_confirm_pending_idx = act_idx;
        g_confirm_pending_deadline_ms = now + 4000;
        show_notice("Press OK or tap again to confirm", 3000);
        render_screen();
    }
}

static void rf_send_screen_button_press(const char *act_id, uint8_t btn_idx, const char *label) {
    if (g_rf_fd < 0) return;
    rf_frame_t f;
    uint8_t payload[25] = {0};
    strncpy((char *)&payload[0], act_id, 10);
    payload[10] = btn_idx;
    strncpy((char *)&payload[11], label, 12);
    rf_build_frame(&f, g_remote_slot, RF_MSG_DEVICE_CMD_REQUEST, ++g_tx_seq, 0x01, payload, sizeof(payload));
    rf_send_frame(&f);
    printf("[*] RF TX SCREEN_BTN: act=%s idx=%d label=%s\n", act_id, btn_idx, label);
}

static void execute_active_dev_cmd(int filtered_idx) {
    char label[32] = {0};
    uint8_t btn_idx = 0, atype = 1;
    if (get_active_dev_cmd_item(filtered_idx, label, sizeof(label), &btn_idx, &atype, NULL, 0) == 0) {
        if (atype == 2) {
            trigger_activity_switch(g_switching_target_id, label);
        } else if (atype == 3) {
            trigger_activity_switch("-1", "PowerOff");
        } else {
            const char *ctx = (g_remote_mode == 1) ? ((g_current_view == VIEW_ACTIVITIES) ? "page1" : "page2") : g_active_activity_id;
            rf_send_screen_button_press(ctx, btn_idx, label);
        }
    }
}

/* Handle incoming Codex RF protocol frame on Remote */
static void handle_codex_frame_remote(const rf_frame_t *f) {
    if (f->dev_slot & 0x0F) g_remote_slot = f->dev_slot & 0x0F;

    switch (f->msg_type) {

        case RF_MSG_ACTIVITY_SYNC: {
            char act_id[13] = {0};
            char act_name[24] = {0};
            if (f->payload[1] == '\0' || f->payload[2] == '\0' || f->payload[0] == '-') {
                /* Short ID format (1-2 byte ID, up to 22 byte name) */
                strncpy(act_id, (const char *)&f->payload[0], sizeof(act_id) - 1);
                strncpy(act_name, (const char *)&f->payload[3], sizeof(act_name) - 1);
            } else if (f->payload[10] >= 0x20) {
                memcpy(act_id, f->payload, 10);
                memcpy(act_name, &f->payload[10], 15);
            } else {
                memcpy(act_id, f->payload, 12);
                memcpy(act_name, &f->payload[12], 14);
            }

            if (g_remote_mode == 1) {
                /* In direct mode, ignore activity sync popups */
                strncpy(g_active_activity_id, act_id, sizeof(g_active_activity_id) - 1);
                strncpy(g_active_activity_name, act_name, sizeof(g_active_activity_name) - 1);
                break;
            }

            uint64_t now_ms = get_monotonic_ms();

            /* If remote is currently displaying the switching progress bar, and Hub confirms switch */
            if (g_is_switching_activity && strcmp(act_id, g_switching_target_id) == 0) {
                uint64_t elapsed = now_ms - g_switching_start_ms;
                /* If switch sequence timing is still in progress (e.g. Activity switched marker reached,
                 * but warm-up delays or tail commands are still executing), update active ID but DO NOT unlock yet */
                strncpy(g_active_activity_id, act_id, sizeof(g_active_activity_id) - 1);
                strncpy(g_active_activity_name, act_name, sizeof(g_active_activity_name) - 1);
                if (elapsed + 500 < g_switching_total_ms) {
                    printf("[*] Hub confirmed activity switched to '%s' (%s) at %llums/%ums; continuing tail steps\n",
                           act_name, act_id, (unsigned long long)elapsed, g_switching_total_ms);
                    g_screen_dirty = true;
                    break;
                }
                printf("[*] Hub confirmed switch complete to '%s' (%s), unlocking screen\n", act_name, act_id);
                g_is_switching_activity = false;
                g_last_switch_completed_ms = now_ms;
                strncpy(g_last_switch_target_id, g_switching_target_id, sizeof(g_last_switch_target_id) - 1);
                if (strcmp(g_switching_target_id, "-1") != 0) {
                    g_current_view = VIEW_DEVICES;
                } else {
                    g_current_view = VIEW_ACTIVITIES;
                }
                g_selected_device_cmd = 0;
                reset_idle_timer();
                g_screen_dirty = true;
                break;
            }

            if (g_initial_sync_received && strcmp(g_active_activity_id, act_id) == 0) {
                break;
            }

            uint8_t is_query_reply = f->payload[24];
            uint64_t time_since_wake = (s_last_wake_ms > 0) ? (now_ms - s_last_wake_ms) : 999999;
            uint64_t time_since_reconnect = (s_last_reconnect_ms > 0) ? (now_ms - s_last_reconnect_ms) : 999999;
            bool is_wake_or_reconnect = (time_since_wake < 6000 || time_since_reconnect < 6000);
            bool is_state_catchup = (!g_initial_sync_received || is_query_reply == 0x01 || is_wake_or_reconnect);

            if (is_state_catchup) {
                g_initial_sync_received = true;
                printf("[*] Hub Activity Sync (catch-up: wake=%llums rec=%llums qreply=%d): id=%s name=%s (already running, no switch overlay)\n",
                       (unsigned long long)time_since_wake, (unsigned long long)time_since_reconnect, is_query_reply, act_id, act_name);
                strncpy(g_active_activity_id, act_id, sizeof(g_active_activity_id) - 1);
                strncpy(g_active_activity_name, act_name, sizeof(g_active_activity_name) - 1);
                strncpy(g_last_switch_target_id, act_id, sizeof(g_last_switch_target_id) - 1);
                g_last_switch_completed_ms = now_ms;
                if (strcmp(act_id, "-1") != 0) {
                    g_current_view = VIEW_DEVICES;
                } else {
                    g_current_view = VIEW_ACTIVITIES;
                }
                g_selected_device_cmd = 0;
                reset_idle_timer();
                g_screen_dirty = true;
                break;
            }

            printf("[*] Hub Activity Sync (live change): id=%s name=%s\n", act_id, act_name);
            bool is_recent_same_switch = (strcmp(g_last_switch_target_id, act_id) == 0 && (now_ms - g_last_switch_completed_ms < 20000));
            if (!g_is_switching_activity && !is_recent_same_switch) {
                trigger_activity_switch_external(act_id, act_name);
            }
            strncpy(g_active_activity_id, act_id, sizeof(g_active_activity_id) - 1);
            strncpy(g_active_activity_name, act_name, sizeof(g_active_activity_name) - 1);
            g_selected_device_cmd = 0;
            reset_idle_timer();
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_CONFIG_END: {
            uint8_t phase = f->payload[0];
            uint8_t version = f->payload[1];
            printf("[*] CONFIG_END: phase=%s version=%d\n", phase == 0 ? "clear" : "complete", version);
            if (phase == 0x00) {
                g_is_syncing_config = true;
                g_sync_start_ms = get_monotonic_ms();
                g_sync_last_packet_ms = g_sync_start_ms;
                g_sync_chunk_count = 0;
                g_rf_activity_count = 0;
                g_rf_device_count = 0;
                g_rf_button_count = 0;
                g_rf_step_count = 0;
                init_act_triggers();
                g_screen_dirty = true;
            } else {
                g_is_syncing_config = false;
                g_config_version = version;
                g_has_hub_config = true;
                save_rf_config();
                g_activity_count = 0;
                for (int i = 0; i < g_rf_activity_count && g_activity_count < MAX_ACTIVITIES; i++) {
                    strncpy(g_activities[g_activity_count].id, g_rf_activities[i].id, sizeof(g_activities[0].id) - 1);
                    strncpy(g_activities[g_activity_count].name, g_rf_activities[i].name, sizeof(g_activities[0].name) - 1);
                    resolve_activity_icon(g_rf_activities[i].icon, g_rf_activities[i].name,
                                          g_activities[g_activity_count].icon_path, sizeof(g_activities[0].icon_path));
                    g_activity_count++;
                }
                if (g_activity_count < MAX_ACTIVITIES) {
                    strncpy(g_activities[g_activity_count].id, "-1", sizeof(g_activities[0].id) - 1);
                    strncpy(g_activities[g_activity_count].name, "Power Off", sizeof(g_activities[0].name) - 1);
                    strncpy(g_activities[g_activity_count].icon_path, "/opt/luadraws/appscommon/asset/commands/116.png", sizeof(g_activities[0].icon_path) - 1);
                    g_activity_count++;
                }
                g_screen_dirty = true;
            }
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            break;
        }

        case RF_MSG_ACTIVITY_LIST_CHUNK: {
            if (!g_is_syncing_config) {
                g_is_syncing_config = true;
                g_sync_start_ms = get_monotonic_ms();
            }
            g_sync_last_packet_ms = get_monotonic_ms();
            g_sync_chunk_count++;
            uint8_t idx = f->payload[0];
            if (idx < MAX_RF_ACTIVITIES) {
                uint8_t itype = 0;
                if (f->payload[2] < 0x20) {
                    itype = f->payload[2];
                    if (f->payload[4] == '\0' || f->payload[5] == '\0' || f->payload[3] == '-') {
                        /* Short ID format (1-2 byte ID + null terminator): up to 19 bytes for activity name! */
                        strncpy(g_rf_activities[idx].id, (const char *)&f->payload[3], sizeof(g_rf_activities[0].id) - 1);
                        g_rf_activities[idx].id[sizeof(g_rf_activities[0].id) - 1] = '\0';
                        strncpy(g_rf_activities[idx].name, (const char *)&f->payload[6], sizeof(g_rf_activities[0].name) - 1);
                        g_rf_activities[idx].name[sizeof(g_rf_activities[0].name) - 1] = '\0';
                    } else if (f->payload[11] >= 0x20) {
                        memcpy(g_rf_activities[idx].id, &f->payload[3], 8);
                        g_rf_activities[idx].id[8] = '\0';
                        memcpy(g_rf_activities[idx].name, &f->payload[11], 14);
                        g_rf_activities[idx].name[14] = '\0';
                    } else {
                        memcpy(g_rf_activities[idx].id, &f->payload[3], 10);
                        g_rf_activities[idx].id[10] = '\0';
                        memcpy(g_rf_activities[idx].name, &f->payload[13], 12);
                        g_rf_activities[idx].name[12] = '\0';
                    }
                } else {
                    memcpy(g_rf_activities[idx].id, &f->payload[2], 10);
                    g_rf_activities[idx].id[10] = '\0';
                    memcpy(g_rf_activities[idx].name, &f->payload[12], 14);
                    g_rf_activities[idx].name[14] = '\0';
                }
                map_icon_type_to_str(itype, g_rf_activities[idx].name, g_rf_activities[idx].icon, sizeof(g_rf_activities[idx].icon));
                if (idx >= g_rf_activity_count) g_rf_activity_count = idx + 1;
                printf("[*] Activity chunk %d: %s (%s, icon=%s)\n", idx, g_rf_activities[idx].name, g_rf_activities[idx].id, g_rf_activities[idx].icon);
            }
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_DEVICE_LIST_CHUNK: {
            if (!g_is_syncing_config) {
                g_is_syncing_config = true;
                g_sync_start_ms = get_monotonic_ms();
            }
            g_sync_last_packet_ms = get_monotonic_ms();
            g_sync_chunk_count++;
            uint8_t idx = f->payload[0];
            if (idx < MAX_RF_DEVICES) {
                memcpy(g_rf_devices[idx].id, &f->payload[2], 10);
                g_rf_devices[idx].id[10] = '\0';
                memcpy(g_rf_devices[idx].name, &f->payload[12], 14);
                g_rf_devices[idx].name[14] = '\0';
                if (idx >= g_rf_device_count) g_rf_device_count = idx + 1;
            }
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_BUTTON_LIST_CHUNK: {
            if (!g_is_syncing_config) {
                g_is_syncing_config = true;
                g_sync_start_ms = get_monotonic_ms();
            }
            g_sync_last_packet_ms = get_monotonic_ms();
            g_sync_chunk_count++;
            uint8_t idx = f->payload[0];
            if (idx < MAX_RF_BUTTONS) {
                g_rf_buttons[idx].context_type = f->payload[2];
                memcpy(g_rf_buttons[idx].context_id, &f->payload[3], 9);
                g_rf_buttons[idx].context_id[9] = '\0';
                memcpy(g_rf_buttons[idx].label, &f->payload[12], 12);
                g_rf_buttons[idx].label[12] = '\0';
                g_rf_buttons[idx].btn_index = f->payload[1];
                g_rf_buttons[idx].action_type = f->payload[24];
                if (idx >= g_rf_button_count) g_rf_button_count = idx + 1;

                /* Check if this button is mapped to an activity start or stop */
                if (f->payload[2] == 0x00 && (f->payload[24] == 0x02 || f->payload[24] == 0x03)) {
                    if (g_act_trigger_count < MAX_ACT_TRIGGERS) {
                        strncpy(g_act_triggers[g_act_trigger_count].button_name, g_rf_buttons[idx].label, sizeof(g_act_triggers[0].button_name) - 1);
                        g_act_triggers[g_act_trigger_count].atype = f->payload[24];
                        strncpy(g_act_triggers[g_act_trigger_count].target_act_id, g_rf_buttons[idx].context_id, sizeof(g_act_triggers[0].target_act_id) - 1);
                        g_act_triggers[g_act_trigger_count].rf_key_code = 0;
                        if (strcasecmp(g_act_triggers[g_act_trigger_count].button_name, "PwrOff") == 0 ||
                            strcasecmp(g_act_triggers[g_act_trigger_count].button_name, "PowerOff") == 0) {
                            g_act_triggers[g_act_trigger_count].rf_key_code = 0x01EC;
                        }
                        g_act_trigger_count++;
                    }
                }
            }
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_STEP_LIST_CHUNK: {
            if (!g_is_syncing_config) {
                g_is_syncing_config = true;
                g_sync_start_ms = get_monotonic_ms();
            }
            g_sync_last_packet_ms = get_monotonic_ms();
            g_sync_chunk_count++;
            uint8_t chunk_idx = f->payload[0];
            if (chunk_idx == 0) {
                g_rf_step_count = 0;
            }
            if (g_rf_step_count < MAX_RF_STEPS) {
                g_rf_steps[g_rf_step_count].step_idx = f->payload[1];
                g_rf_steps[g_rf_step_count].total_steps = f->payload[2];
                strncpy(g_rf_steps[g_rf_step_count].act_id, (char *)&f->payload[3], 9);
                g_rf_steps[g_rf_step_count].act_id[9] = '\0';
                g_rf_steps[g_rf_step_count].delay_ms = (uint16_t)f->payload[12] | ((uint16_t)f->payload[13] << 8);
                strncpy(g_rf_steps[g_rf_step_count].name, (char *)&f->payload[14], 11);
                g_rf_steps[g_rf_step_count].name[11] = '\0';
                g_rf_step_count++;
                printf("[*] Step chunk %d: act=%s step=%d/%d del=%d name='%s'\n",
                       chunk_idx, g_rf_steps[g_rf_step_count - 1].act_id,
                       g_rf_steps[g_rf_step_count - 1].step_idx,
                       g_rf_steps[g_rf_step_count - 1].total_steps,
                       g_rf_steps[g_rf_step_count - 1].delay_ms,
                       g_rf_steps[g_rf_step_count - 1].name);
            }
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_SETTINGS_PUSH: {
            g_brightness_val = f->payload[0];
            if (g_brightness_val < 1) g_brightness_val = 1;
            if (g_brightness_val > 5) g_brightness_val = 5;
            g_touch_enabled = (f->payload[1] != 0);
            g_gyro_enabled = (f->payload[2] != 0);
            g_timeout_seconds = ((int)f->payload[3] << 8) | f->payload[4];
            int nkey = (int)f->payload[5] | ((int)f->payload[6] << 8);
            if (nkey > 0) g_nav_toggle_key = nkey;
            g_nav_toggle_mode = f->payload[7];
            g_confirm_activity = (f->payload[8] != 0);
            if (f->payload[9] >= 1 && f->payload[9] <= 3) g_touch_sensitivity = f->payload[9];
            if (f->payload[10] <= 1) g_remote_mode = f->payload[10];

            /* Extended settings fields */
            if (f->payload[11] > 0) g_gyro_rate_hz = f->payload[11];
            if (f->payload[12] <= 2) g_btn_backlight_mode = f->payload[12];
            if (f->payload[13] <= 23) g_night_start_hour = f->payload[13];
            if (f->payload[14] <= 23) g_night_end_hour = f->payload[14];
            g_usbeth_enabled = (f->payload[15] != 0);
            bool prev_no_screen = g_no_screen_mode;
            g_no_screen_mode = (f->payload[17] != 0);
            if (f->payload[18] == 1) g_cap_buttons_enabled = true;
            else if (f->payload[18] == 2) g_cap_buttons_enabled = false;

            if (g_no_screen_mode) {
                set_backlight_level(0);
                g_screen_on = false;
            } else {
                g_screen_on = true;
                set_backlight_level(g_brightness_val);
            }
            save_settings_conf();
            printf("[*] Settings push: bright=%d touch=%d cap=%d gyro=%d rate=%d timeout=%d nav_key=%d nav_mode=%d confirm=%d sens=%d mode=%d bl=%d night=%02d-%02d usbeth=%d no_screen=%d\n",
                   g_brightness_val, g_touch_enabled, g_cap_buttons_enabled, g_gyro_enabled, g_gyro_rate_hz, g_timeout_seconds,
                   g_nav_toggle_key, g_nav_toggle_mode, g_confirm_activity, g_touch_sensitivity, g_remote_mode,
                   g_btn_backlight_mode, g_night_start_hour, g_night_end_hour, g_usbeth_enabled, g_no_screen_mode);
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);

            if (prev_no_screen && !g_no_screen_mode && !g_fb_mem) {
                /* Remote booted in headless mode; restart via watchdog to cleanly init framebuffer and touch */
                printf("[*] Screen re-enabled from WebUI. Restarting codex_elite to initialize display...\n");
                usleep(150000);
                _exit(0);
            }

            if (!g_no_screen_mode) {
                render_screen();
            }
            break;
        }

        case RF_MSG_OTA_START: {
            read_elite_battery();
            uint32_t tot_b = (uint32_t)f->payload[0] | ((uint32_t)f->payload[1] << 8) |
                             ((uint32_t)f->payload[2] << 16) | ((uint32_t)f->payload[3] << 24);
            uint16_t tot_c = (uint16_t)f->payload[4] | ((uint16_t)f->payload[5] << 8);

            printf("[*] RF OTA START: %u bytes, %u chunks, batt=%d%% chg=%d\n",
                   tot_b, tot_c, g_battery_info.percent, g_battery_info.charging);

            if (g_battery_info.percent < 40 && !g_battery_info.charging) {
                printf("[-] RF OTA REJECTED: Low battery (%d%%) and not charging!\n", g_battery_info.percent);
                rf_frame_t ack;
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x02);
                rf_send_frame(&ack);
                break;
            }

            if (g_ota_file) {
                fclose(g_ota_file);
                g_ota_file = NULL;
            }
            g_ota_file = fopen("/tmp/codex_elite.ota.gz", "wb");
            if (!g_ota_file) {
                printf("[-] RF OTA REJECTED: Could not open /tmp/codex_elite.ota.gz\n");
                rf_frame_t ack;
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x03);
                rf_send_frame(&ack);
                break;
            }

            g_is_ota_updating = true;
            g_last_activity_time = time(NULL);
            if (!g_screen_on) {
                fb_set_blank(false);
            }
            g_ota_start_ms = get_monotonic_ms();
            g_ota_last_packet_ms = g_ota_start_ms;
            g_ota_total_bytes = tot_b;
            g_ota_received_bytes = 0;
            g_ota_total_chunks = tot_c;
            g_ota_received_chunks = 0;
            g_screen_dirty = true;

            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            break;
        }

        case RF_MSG_OTA_CHUNK: {
            if (!g_is_ota_updating || !g_ota_file) break;
            g_ota_last_packet_ms = get_monotonic_ms();
            g_last_activity_time = time(NULL);

            uint16_t c_idx = (uint16_t)f->payload[0] | ((uint16_t)f->payload[1] << 8);
            uint8_t d_len = f->payload[2];
            if (d_len > 22) d_len = 22;

            if (d_len > 0) {
                fwrite(&f->payload[3], 1, d_len, g_ota_file);
                g_ota_received_bytes += d_len;
                g_ota_received_chunks++;
            }

            if ((f->flags & RF_FLAG_ACK_REQ) || (c_idx % 4 == 0)) {
                rf_frame_t ack;
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
                rf_send_frame(&ack);
            }
            static uint64_t last_ota_render_ms = 0;
            uint64_t now_ms = get_monotonic_ms();
            if (now_ms - last_ota_render_ms >= 500) {
                last_ota_render_ms = now_ms;
                g_screen_dirty = true;
            }
            break;
        }

        case RF_MSG_OTA_END: {
            if (!g_is_ota_updating || !g_ota_file) break;
            fclose(g_ota_file);
            g_ota_file = NULL;

            printf("[+] RF OTA END received! Received %u bytes (%u chunks). Verifying...\n",
                   g_ota_received_bytes, g_ota_received_chunks);

            // 1. Attempt extracting as tar package (bundle containing binary + scripts)
            system("rm -rf /tmp/elite_ota_pkg && mkdir -p /tmp/elite_ota_pkg");
            int tar_rc = system("tar -xzf /tmp/codex_elite.ota.gz -C /tmp/elite_ota_pkg 2>/dev/null || "
                                "/data/busybox tar -xzf /tmp/codex_elite.ota.gz -C /tmp/elite_ota_pkg 2>/dev/null || "
                                "/bin/busybox tar -xzf /tmp/codex_elite.ota.gz -C /tmp/elite_ota_pkg 2>/dev/null");

            bool is_pkg = false;
            char bin_path[128] = {0};

            if (tar_rc == 0) {
                if (access("/tmp/elite_ota_pkg/codex_elite", F_OK) == 0) {
                    strcpy(bin_path, "/tmp/elite_ota_pkg/codex_elite");
                    is_pkg = true;
                } else if (access("/tmp/elite_ota_pkg/bin/codex_elite", F_OK) == 0) {
                    strcpy(bin_path, "/tmp/elite_ota_pkg/bin/codex_elite");
                    is_pkg = true;
                }
            }

            // Fallback: legacy raw gzipped binary
            if (!is_pkg) {
                system("gzip -d -c /tmp/codex_elite.ota.gz > /data/codex_elite.new 2>/dev/null || cat /tmp/codex_elite.ota.gz > /data/codex_elite.new");
                strcpy(bin_path, "/data/codex_elite.new");
            }
            system("chmod +x /tmp/elite_ota_pkg/codex_elite /data/codex_elite.new 2>/dev/null || true");

            bool valid_elf = false;
            FILE *felf = fopen(bin_path, "rb");
            if (felf) {
                uint8_t hdr[4] = {0};
                if (fread(hdr, 1, 4, felf) == 4) {
                    if (hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
                        valid_elf = true;
                    }
                }
                fclose(felf);
            }

            rf_frame_t ack;
            if (valid_elf) {
                printf("[+] ELF header verified! Replacing binary and installing scripts...\n");
                system("cp -f /data/codex_elite /data/codex_elite.bak 2>/dev/null");

                if (is_pkg) {
                    // Install scripts from package
                    if (access("/tmp/elite_ota_pkg/rcS.local", F_OK) == 0) {
                        system("cp -f /tmp/elite_ota_pkg/rcS.local /etc/init.d/rcS.local && chmod 755 /etc/init.d/rcS.local");
                        system("cp -f /tmp/elite_ota_pkg/rcS.local /data/rcS.local 2>/dev/null && chmod 755 /data/rcS.local 2>/dev/null || true");
                    }
                    if (access("/tmp/elite_ota_pkg/codex_watchdog.sh", F_OK) == 0) {
                        system("cp -f /tmp/elite_ota_pkg/codex_watchdog.sh /data/codex_watchdog.sh && chmod 755 /data/codex_watchdog.sh");
                    }
                    if (access("/tmp/elite_ota_pkg/factory_reset.sh", F_OK) == 0) {
                        system("cp -f /tmp/elite_ota_pkg/factory_reset.sh /data/factory_reset.sh && chmod 755 /data/factory_reset.sh");
                    }
                    system("for s in /tmp/elite_ota_pkg/*.sh; do [ -f \"$s\" ] && cp -f \"$s\" /data/ && chmod 755 \"/data/$(basename \"$s\")\"; done 2>/dev/null || true");
                    char cp_cmd[256];
                    snprintf(cp_cmd, sizeof(cp_cmd), "mv -f %s /data/codex_elite && chmod 755 /data/codex_elite", bin_path);
                    system(cp_cmd);
                } else {
                    system("mv -f /data/codex_elite.new /data/codex_elite && chmod 755 /data/codex_elite");
                }

                system("rm -rf /tmp/elite_ota_pkg /tmp/codex_elite.ota.gz /data/codex_elite.new 2>/dev/null || true");

                rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
                rf_send_frame(&ack);

                g_is_ota_updating = false;
                render_screen();
                system("/data/codex_sync 2>/dev/null; sync 2>/dev/null; reboot -f 2>/dev/null || (sleep 1; echo b > /proc/sysrq-trigger 2>/dev/null) &");
                usleep(250000);
                reboot(RB_AUTOBOOT);
                exit(0);
            } else {
                printf("[-] OTA ELF verification failed! Corrupted transfer.\n");
                system("rm -rf /tmp/elite_ota_pkg /data/codex_elite.new /tmp/codex_elite.ota.gz 2>/dev/null || true");
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x05);
                rf_send_frame(&ack);
                g_is_ota_updating = false;
                g_screen_dirty = true;
            }
            break;
        }

        case RF_MSG_TIME_SYNC: {
            uint32_t epoch = (uint32_t)f->payload[0] |
                            ((uint32_t)f->payload[1] << 8) |
                            ((uint32_t)f->payload[2] << 16) |
                            ((uint32_t)f->payload[3] << 24);
            uint8_t tz_len = f->payload[4];
            char tz_str[32] = {0};
            if (tz_len > 0 && tz_len < sizeof(tz_str)) {
                memcpy(tz_str, &f->payload[5], tz_len);
                tz_str[tz_len] = '\0';
            }

            printf("[*] Remote RF TIME_SYNC received: epoch=%u tz=%s\n", epoch, tz_str);
            if (epoch > 1500000000U) {
                struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                set_msp430_rtc(epoch);
            }
            if (tz_str[0]) {
                char full_tz[48] = {0};
                expand_timezone(tz_str, full_tz, sizeof(full_tz));
                if (full_tz[0] && strcmp(g_timezone_str, full_tz) != 0) {
                    FILE *ftz = fopen("/etc/TZ", "w");
                    if (ftz) {
                        fprintf(ftz, "%s\n", full_tz);
                        fclose(ftz);
                    }
                    strncpy(g_timezone_str, full_tz, sizeof(g_timezone_str) - 1);
                    g_timezone_str[sizeof(g_timezone_str) - 1] = '\0';
                    setenv("TZ", g_timezone_str, 1);
                    tzset();
                    save_settings_conf();
                    printf("[+] Timezone updated to: %s\n", g_timezone_str);
                }
            }

            /* Hub config version embedded at byte 20 */
            uint8_t hub_ver = f->payload[20];
            if (hub_ver != 0 && hub_ver != g_config_version && !g_is_syncing_config && !g_is_ota_updating) {
                static uint64_t last_ver_check_ms = 0;
                uint64_t now_ms = get_monotonic_ms();
                if (now_ms - last_ver_check_ms > 8000) {
                    last_ver_check_ms = now_ms;
                    printf("[*] Remote detected Hub config version mismatch (hub=%d, local=%d). Requesting full sync...\n",
                           hub_ver, g_config_version);
                    rf_send_full_sync_request();
                }
            }

            /* Hub RF Address embedded in payload bytes 21..24 */
            if (tz_len <= 16 && (f->payload[21] || f->payload[22] || f->payload[23] || f->payload[24])) {
                memcpy(g_paired_hub_addr, &f->payload[21], 4);
                printf("[+] Remote learned Hub RF Address from TIME_SYNC: %02X%02X%02X%02X\n",
                       g_paired_hub_addr[0], g_paired_hub_addr[1], g_paired_hub_addr[2], g_paired_hub_addr[3]);
            }
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_UNPAIR_NOTIFY: {
            printf("[*] Remote: received UNPAIR_NOTIFY from Hub! Unpairing...\n");
            g_pairing_state = 0;
            g_rf_link_connected = false;
            pthread_mutex_lock(&g_rf_write_mutex);
            uint8_t enter_test[7] = {0x10, 0xFF, 0x80, 0xD0, 0x01, 0x00, 0x00};
            if (write(g_rf_fd, enter_test, sizeof(enter_test)) < 0) {}
            usleep(50000);
            uint8_t clr_pipe20[20] = {0x11, 0xFF, 0x82, 0xB5, 0x20, 0};
            if (write(g_rf_fd, clr_pipe20, sizeof(clr_pipe20)) < 0) {}
            usleep(50000);
            uint8_t clr_pipe30[20] = {0x11, 0xFF, 0x82, 0xB5, 0x30, 0};
            if (write(g_rf_fd, clr_pipe30, sizeof(clr_pipe30)) < 0) {}
            usleep(50000);
            uint8_t exit_test[7] = {0x10, 0xFF, 0x80, 0xD0, 0x00, 0x00, 0x00};
            if (write(g_rf_fd, exit_test, sizeof(exit_test)) < 0) {}
            pthread_mutex_unlock(&g_rf_write_mutex);
            show_notice("Hub unpaired this remote", 4000);
            g_current_view = VIEW_PAIRING;
            g_screen_dirty = true;
            break;
        }

        case RF_MSG_CONFIG_PULL_REQ: {
            printf("[*] Remote: received CONFIG_PULL_REQ from Hub, sending config backup...\n");
            send_remote_config_backup_rf();
            break;
        }

        case RF_MSG_ACK:
            break;

        case RF_MSG_PING: {
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            break;
        }

        default:
            if (g_log_level >= LOG_LEVEL_DEBUG) {
                printf("[*] Remote: unknown Codex frame type 0x%02X\n", f->msg_type);
            }
            break;
    }
}

/* Background thread: blocking read on /dev/rfspi for Activity Sync and commands from Hub */
static void *rf_rx_worker(void *arg) {
    (void)arg;
    uint8_t pkt[64];

    while (g_running) {
        if (g_rf_fd < 0) { usleep(100000); continue; }
        ssize_t n = read(g_rf_fd, pkt, sizeof(pkt));
        if (n <= 0) {
            if (n < 0 && errno != EINTR) usleep(50000);
            continue;
        }



        if (n == 7 && pkt[0] == 0x10 && pkt[2] == 0x41) {
            bool cc2544_lost = (pkt[4] & 0x40) != 0;
            static uint64_t s_last_link_ok_ms = 0;
            if (!cc2544_lost) {
                s_last_link_ok_ms = get_monotonic_ms();
                if (!g_rf_link_connected) {
                    g_rf_link_connected = true;
                    g_screen_dirty = true;
                }
                if (g_pairing_state == 1) {
                    g_pairing_state = 2;
                    if (g_current_view == VIEW_PAIRING) {
                        g_current_view = VIEW_ACTIVITIES;
                        render_screen();
                    }
                } else if (g_pairing_state == 0) {
                    g_pairing_state = 2;
                }
                if (g_activity_count <= 1 && !g_has_hub_config) {
                    rf_send_full_sync_request();
                    render_screen();
                }
            } else if (g_screen_on) {
                if (get_monotonic_ms() - s_last_link_ok_ms > 6000) {
                    if (g_rf_link_connected) {
                        g_rf_link_connected = false;
                        g_screen_dirty = true;
                    }
                }
            }
            static bool s_prev_cc2544_lost = true;
            if (cc2544_lost != s_prev_cc2544_lost || g_log_level >= LOG_LEVEL_DEBUG) {
                s_prev_cc2544_lost = cc2544_lost;
                printf("[*] Remote RF CC2544 Link Status: %s (byte4=0x%02X byte5=0x%02X byte6=0x%02X, connected=%d)\n",
                       cc2544_lost ? "LOST" : "ESTABLISHED", pkt[4], pkt[5], pkt[6], g_rf_link_connected);
                fflush(stdout);
            }
        }

        /* Pipe Device Info replies (11 FF 83 B5 <pipe> <data...>) */
        if (n >= 16 && pkt[0] == 0x11 && pkt[2] == 0x83 && pkt[3] == 0xB5) {
            uint8_t pipe = pkt[4];
            if (pipe == 0x03) {
                memcpy(g_remote_own_addr, &pkt[5], 4);
                printf("[+] Remote Own RF Address (pipe 0x03): %02X%02X%02X%02X\n",
                       g_remote_own_addr[0], g_remote_own_addr[1], g_remote_own_addr[2], g_remote_own_addr[3]);
            } else if (pipe == 0x20 || pipe == 0x30) {
                /* In CC2544 paired device record, RF address is at bytes 12..15 */
                if (pkt[12] != 0 || pkt[13] != 0 || pkt[14] != 0 || pkt[15] != 0) {
                    memcpy(g_paired_hub_addr, &pkt[12], 4);
                    printf("[+] Remote Paired Hub Address (pipe 0x%02X): %02X%02X%02X%02X\n",
                           pipe, g_paired_hub_addr[0], g_paired_hub_addr[1], g_paired_hub_addr[2], g_paired_hub_addr[3]);
                }
            }
            if (g_current_view == VIEW_PAIRING) render_screen();
            continue;
        }

        if (n >= 5 && pkt[0] == 0x10 && pkt[2] == 0x4A) {
            uint8_t pair_status = pkt[4];
            printf("[*] Remote RF CC2544 Pairing Event: status=0x%02X slot=%d\n", pair_status, pkt[1]);
            if (pkt[1] >= 1 && pkt[1] <= 15) {
                g_remote_slot = pkt[1];
            }
            if (pair_status == 0x01 || pair_status == 0x00) {
                printf("[+] Remote RF Pairing SUCCESS! Paired to Hub on Slot %d\n", g_remote_slot);
                g_rf_link_connected = true;
                g_pairing_state = 2;
                if (g_current_view == VIEW_PAIRING) g_current_view = VIEW_ACTIVITIES;
                rf_send_full_sync_request();
            } else {
                printf("[-] Remote RF Pairing failed / closed (status=0x%02X)\n", pair_status);
                g_pairing_state = 3;
            }
            render_screen();
            continue;
        }

        if (rf_is_valid_frame(pkt, (size_t)n)) {
            g_last_hub_frame_ms = get_monotonic_ms();
            if (!g_rf_link_connected) {
                g_rf_link_connected = true;
                g_screen_dirty = true;
            }
            if (g_pairing_state == 1) {
                g_pairing_state = 2;
                if (g_current_view == VIEW_PAIRING) {
                    g_current_view = VIEW_ACTIVITIES;
                    g_screen_dirty = true;
                }
            } else if (g_pairing_state == 0) {
                g_pairing_state = 2;
            }
            rf_frame_t f;
            memset(&f, 0, sizeof(f));
            if (pkt[0] == 0x20 && n >= 32) {
                if ((pkt[1] & 0x0F) >= 1 && (pkt[1] & 0x0F) <= 15) {
                    g_remote_slot = pkt[1] & 0x0F;
                }
                if ((pkt[4] & 0xE0) == 0x40 || (pkt[4] & 0xE0) == 0x60) {
                    f.report_id = 0x20;
                    f.dev_slot = pkt[1] & 0x0F;
                    f.sub_id = 0x12;
                    f.seq = (pkt[4] & 0x0F);
                    f.hdr_flag = pkt[4];
                    f.msg_type = pkt[5];
                    f.flags = pkt[6];
                    memcpy(f.payload, &pkt[7], sizeof(f.payload));
                } else {
                    memcpy(&f, pkt, sizeof(f));
                }
            } else if (pkt[0] == 0x12 && n >= 30) {
                if (pkt[1] >= 1 && pkt[1] <= 15) {
                    g_remote_slot = pkt[1];
                }
                if ((pkt[2] & 0xE0) == 0x40 || (pkt[2] & 0xE0) == 0x60) {
                    f.report_id = 0x20;
                    f.dev_slot = g_remote_slot;
                    f.sub_id = 0x12;
                    f.seq = (pkt[2] & 0x0F);
                    f.hdr_flag = pkt[2];
                    f.msg_type = pkt[3];
                    f.flags = pkt[4];
                    memcpy(f.payload, &pkt[5], sizeof(f.payload));
                } else {
                    f.report_id = 0x20;
                    f.dev_slot = g_remote_slot;
                    f.sub_id = 0x12;
                    f.seq = pkt[1];
                    f.hdr_flag = 0x40;
                    f.msg_type = pkt[2];
                    f.flags = pkt[3];
                    memcpy(f.payload, &pkt[4], sizeof(f.payload));
                }
            }
            printf("[*] RF RX -> Codex frame type=0x%02X seq=%d\n", f.msg_type, f.seq);
            fflush(stdout);
            handle_codex_frame_remote(&f);
        } else {
            /* Not a Codex frame — check if it's a 0x42 container with embedded data */
            if (n >= 32 && pkt[0] == 0x20 && pkt[2] == 0x42) {
                printf("[*] RF RX eQuad container (0x42): parsing payload at offset 3\n");
                /* The 0x42 container holds eQuad payload starting at byte 3 */
                rf_frame_t f;
                memset(&f, 0, sizeof(f));
                f.report_id = 0x20;
                f.dev_slot = pkt[1];
                f.sub_id = 0x12;
                if ((pkt[4] & 0xE0) == 0x40 || (pkt[4] & 0xE0) == 0x60) {
                    f.seq = pkt[3];
                    f.hdr_flag = pkt[4];
                    f.msg_type = pkt[5];
                    f.flags = pkt[6];
                    memcpy(f.payload, &pkt[7], sizeof(f.payload));
                } else {
                    f.seq = pkt[3];
                    f.msg_type = pkt[4];
                    f.flags = pkt[5];
                    memcpy(f.payload, &pkt[6], sizeof(f.payload));
                }
                /* Validate msg_type range */
                if ((f.msg_type >= RF_MSG_ACTIVITY_SYNC && f.msg_type <= RF_MSG_STEP_LIST_CHUNK) ||
                    (f.msg_type >= RF_MSG_BUTTON_PRESS && f.msg_type <= RF_MSG_SYNC_REQUEST) ||
                    (f.msg_type >= RF_MSG_OTA_START && f.msg_type <= RF_MSG_OTA_END) ||
                    f.msg_type == RF_MSG_ACK || f.msg_type == RF_MSG_PING) {
                    g_last_hub_frame_ms = get_monotonic_ms();
                    if (!g_rf_link_connected) {
                        g_rf_link_connected = true;
                        g_screen_dirty = true;
                    }
                    printf("[*] RF RX 0x42 -> Codex frame type=0x%02X seq=%d\n", f.msg_type, f.seq);
                    fflush(stdout);
                    handle_codex_frame_remote(&f);
                }
            }
            /* Log as legacy for any non-Codex packet */
            if (pkt[0] != 0x20 || pkt[2] != 0x42) {
                printf("[*] Remote RF RX legacy (%zd bytes): ", n);
                for (ssize_t i = 0; i < n; i++) printf("%02X ", pkt[i]);
                printf("\n");
                fflush(stdout);
            }
        }
    }
    return NULL;
}

/* Render Header Bar */
static void render_header(void) {
    /* Header background: blit authentic toolbarBG.png or solid fallback */
    if (!blit_png_file(0, 0, FB_WIDTH, 34, "/opt/luadraws/appscommon/asset/footer/toolbarBG.png")) {
        fb_fill_rect(0, 0, FB_WIDTH, 34, 0x0841);
    }

    /* View Title in bold anti-aliased font */
    const char *title = (g_current_view == VIEW_ACTIVITIES) ? (g_remote_mode == 1 ? "PAGE 1" : "ACTIVITIES") :
                        ((g_current_view == VIEW_DEVICES) ? (g_remote_mode == 1 ? "PAGE 2" : "DEVICES") :
                        ((g_current_view == VIEW_PAIRING) ? "RF PAIRING" : "SETTINGS"));
    fb_draw_text_aa(10, 9, title, RGB_WHITE, &g_font_title);

    /* Right-aligned RF Connection Badge at fixed offset FB_WIDTH - 142 to prevent title clipping */
    int badge_x = FB_WIDTH - 142;
    int badge_w = 54;
    int badge_h = 16;
    int badge_y = 9;

    if (!g_rf_link_connected) {
        /* Prominent Red [NO HUB] Badge */
        fb_fill_rect(badge_x, badge_y, badge_w, badge_h, RGB_RED);
        fb_draw_rect(badge_x, badge_y, badge_w, badge_h, RGB_WHITE);
        fb_draw_text(badge_x + 3, badge_y + 4, "NO HUB", RGB_WHITE, RGB_RED, 1);
    } else {
        /* Subtle Teal [RF] / Dot */
        fb_fill_rect(FB_WIDTH - 98, badge_y + 4, 8, 8, RGB_TEAL);
        fb_draw_rect(FB_WIDTH - 98, badge_y + 4, 8, 8, RGB_WHITE);
    }

    /* Clock HH:MM */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[16];
    if (tm_info) {
        strftime(time_str, sizeof(time_str), "%H:%M", tm_info);
    } else {
        strncpy(time_str, "12:00", sizeof(time_str) - 1);
        time_str[sizeof(time_str) - 1] = '\0';
    }
    fb_draw_text_aa(FB_WIDTH - 84, 9, time_str, RGB_WHITE, &g_font_main);

    /* Battery level gauge / authentic stock icon */
    read_elite_battery();
    if (strcmp(g_battery_info.powersource, "usb") == 0) {
        blit_png_file(FB_WIDTH - 48, 9, 18, 14, "/opt/luadraws/appscommon/asset/header/usb.png");
    }

    char batt_icon[96] = {0};
    if (g_battery_info.charging) {
        snprintf(batt_icon, sizeof(batt_icon), "/opt/luadraws/appscommon/asset/header/hopscharging.png");
    } else {
        int lvl = (g_battery_info.percent / 10) * 10;
        if (lvl < 10) lvl = 10;
        if (lvl > 100) lvl = 100;
        snprintf(batt_icon, sizeof(batt_icon), "/opt/luadraws/appscommon/asset/header/hopslevel%d.png", lvl);
    }
    bool has_batt = blit_png_file(FB_WIDTH - 26, 9, 20, 16, batt_icon);
    if (!has_batt) {
        if (g_battery_info.charging) {
            blit_png_file(FB_WIDTH - 26, 9, 20, 16, "/opt/luadraws/appscommon/asset/header/charging.png");
        } else {
            blit_png_file(FB_WIDTH - 26, 9, 20, 16, "/opt/luadraws/appscommon/asset/header/level100.png");
        }
    }

    /* 1px crisp divider line */
    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);
}

/* Render Footer / Tab Bar (Interactive Bottom Dock) */
static void render_footer(void) {
    int dock_y = FB_HEIGHT - 38;
    int dock_h = 38;

    /* Background bar */
    if (!blit_png_file(0, dock_y, FB_WIDTH, dock_h, "/opt/luadraws/appscommon/asset/footer/toolbarBG.png")) {
        fb_fill_rect(0, dock_y, FB_WIDTH, dock_h, RGB_TAB_BG);
    }
    fb_fill_rect(0, dock_y - 1, FB_WIDTH, 1, RGB_DIVIDER);

    /* 3 Tabs: [ACTIVITIES / PG 1], [DEVICES / PG 2], [SETTINGS] */
    int tab_w = 58;

    for (int t = 0; t < 3; t++) {
        int tx = 4 + t * (tab_w + 2);
        int ty = dock_y + 4;
        int th = dock_h - 8;
        bool is_active_view = (g_current_view == (ui_view_t)t) || (t == 2 && g_current_view == VIEW_PAIRING);

        uint16_t bg = is_active_view ? RGB_TAB_ACTIVE : RGB_TAB_BG;
        uint16_t border = is_active_view ? RGB_TEAL : 0x1924;
        uint16_t fg = is_active_view ? RGB_WHITE : RGB_TEXT_MUTED;

        fb_fill_rect(tx, ty, tab_w, th, bg);
        fb_draw_rect(tx, ty, tab_w, th, border);

        /* Indicator line for active view */
        if (is_active_view) {
            fb_fill_rect(tx + 2, ty + th - 2, tab_w - 4, 2, RGB_TEAL);
        }

        if (t == 1) {
            if (g_remote_mode == 1) {
                int tw = fb_text_width_aa("PG 2", &g_font_main);
                fb_draw_text_aa(tx + (tab_w - tw) / 2, ty + 6, "PG 2", fg, &g_font_main);
            } else {
                /* Authentic Devices icon */
                if (!blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/devices.png")) {
                    int tw = fb_text_width_aa("DEV", &g_font_main);
                    fb_draw_text_aa(tx + (tab_w - tw) / 2, ty + 6, "DEV", fg, &g_font_main);
                }
            }
        } else if (t == 2) {
            /* Authentic Settings icon */
            if (!blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/settings.png")) {
                fb_draw_cog(tx + tab_w / 2, ty + th / 2, fg);
            }
        } else {
            if (g_remote_mode == 1) {
                int tw = fb_text_width_aa("PG 1", &g_font_main);
                fb_draw_text_aa(tx + (tab_w - tw) / 2, ty + 6, "PG 1", fg, &g_font_main);
            } else {
                /* Authentic Activities icon */
                if (!blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/wiihome.png") &&
                    !blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/menu.png") &&
                    !blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/activities.png")) {
                    int tw = fb_text_width_aa("ACT", &g_font_main);
                    fb_draw_text_aa(tx + (tab_w - tw) / 2, ty + 6, "ACT", fg, &g_font_main);
                }
            }
        }
    }

    /* Mode Pill on far right: [NAV] / [DEV] */
    int pill_x = FB_WIDTH - 52;
    int pill_y = dock_y + 4;
    int pill_w = 48;
    int pill_h = dock_h - 8;

    uint16_t pbg = g_screen_nav_mode ? RGB_TEAL : 0x1824;
    uint16_t pfg = g_screen_nav_mode ? RGB_BLACK : RGB_TEXT_MUTED;
    uint16_t pborder = g_screen_nav_mode ? RGB_WHITE : 0x2945;

    fb_fill_rect(pill_x, pill_y, pill_w, pill_h, pbg);
    fb_draw_rect(pill_x, pill_y, pill_w, pill_h, pborder);

    const char *mode_str = g_screen_nav_mode ? "NAV" : "DEV";
    int mw = fb_text_width_aa(mode_str, &g_font_main);
    fb_draw_text_aa(pill_x + (pill_w - mw) / 2, pill_y + 6, mode_str, pfg, &g_font_main);
}

/* Render Activity List View (Dynamic Scrolling, 4 cards per page, 56px height) */
static void render_activities_view(void) {
    int start_y = 38;
    int card_h = 56;
    int spacing = 4;
    int page_size = 4;
    int total = g_activity_count;

    if (total <= 0) {
        fb_fill_rect(12, start_y + 10, FB_WIDTH - 24, 120, RGB_CARD);
        fb_draw_rect(12, start_y + 10, FB_WIDTH - 24, 120, RGB_DIVIDER);
        fb_draw_text_aa(24, start_y + 30, "No Activities", RGB_WHITE, &g_font_title);
        fb_draw_text_aa(24, start_y + 55, "Syncing with Hub...", RGB_CYAN, &g_font_main);
        return;
    }

    int page_start = (g_selected_activity / page_size) * page_size;
    int cur_page = (g_selected_activity / page_size) + 1;
    int total_pages = (total + page_size - 1) / page_size;

    for (int i = 0; i < page_size && (page_start + i) < total; i++) {
        int idx = page_start + i;
        int y = start_y + i * (card_h + spacing);
        bool is_sel = (idx == g_selected_activity);
        bool is_active = (strcmp(g_activities[idx].id, g_active_activity_id) == 0);

        bool is_confirm_pending = (g_confirm_pending_idx == idx);
        uint16_t bg = is_confirm_pending ? 0x2100 : (is_sel ? RGB_CARD_SEL : RGB_CARD);
        uint16_t border = is_confirm_pending ? RGB_YELLOW : (is_sel ? RGB_TEAL : 0x2945);

        /* Card background & outline */
        fb_fill_rect(6, y, FB_WIDTH - 12, card_h, bg);
        fb_draw_rect(6, y, FB_WIDTH - 12, card_h, border);

        /* Active / Selected left bar */
        if (is_confirm_pending) {
            fb_fill_rect(6, y, 4, card_h, RGB_YELLOW);
        } else if (is_active) {
            fb_fill_rect(6, y, 4, card_h, RGB_GREEN);
        } else if (is_sel) {
            fb_fill_rect(6, y, 4, card_h, RGB_TEAL);
        }

        /* 40x40 Activity icon with high-contrast subtle backing plate */
        int icon_x = 14;
        int icon_y = y + 8;
        /* Draw soft contrast backing rounded plate */
        fb_fill_rect(icon_x - 2, icon_y - 2, 44, 44, is_sel ? 0x2125 : 0x18C3);
        fb_draw_rect(icon_x - 2, icon_y - 2, 44, 44, is_sel ? RGB_TEAL : 0x31A6);

        bool has_icon = false;
        if (g_activities[idx].icon_path[0]) {
            has_icon = blit_png_file(icon_x, icon_y, 40, 40, g_activities[idx].icon_path);
        }

        if (!has_icon) {
            /* Vector icon plate fallback */
            fb_draw_rect(icon_x, icon_y, 40, 40, is_sel ? RGB_TEAL : 0x32A9);
            if (strcmp(g_activities[idx].id, "-1") == 0) {
                /* Power symbol */
                fb_fill_rect(icon_x + 19, icon_y + 8, 2, 10, RGB_RED);
                fb_draw_rect(icon_x + 12, icon_y + 12, 16, 16, RGB_RED);
            } else {
                /* TV screen vector */
                fb_draw_rect(icon_x + 10, icon_y + 12, 20, 14, is_sel ? RGB_WHITE : RGB_TEXT_MUTED);
                fb_fill_rect(icon_x + 18, icon_y + 26, 4, 3, is_sel ? RGB_WHITE : RGB_TEXT_MUTED);
            }
        }

        /* Activity Name in anti-aliased font */
        uint16_t text_color = (is_sel || is_active || is_confirm_pending) ? RGB_WHITE : 0xD6DA;
        fb_draw_text_aa(64, y + 13, g_activities[idx].name, text_color, &g_font_main);

        if (is_confirm_pending) {
            fb_draw_text_aa(64, y + 33, "CONFIRM? PRESS OK", RGB_YELLOW, &g_font_main);
        } else if (is_active) {
            fb_draw_text_aa(64, y + 33, "ACTIVE", RGB_GREEN, &g_font_main);
        }

        /* Selection chevron */
        if (is_sel) {
            if (!blit_png_file(FB_WIDTH - 24, y + 20, 10, 15, "/opt/luadraws/appscommon/asset/home/chevron.png")) {
                fb_draw_text_aa(FB_WIDTH - 24, y + 19, ">", RGB_TEAL, &g_font_title);
            }
        }
    }

    /* Page scroll indicator (e.g., 1/2) if multiple pages */
    if (total_pages > 1) {
        char pstr[16];
        snprintf(pstr, sizeof(pstr), "%d/%d", cur_page, total_pages);
        fb_draw_text_aa(FB_WIDTH - 32, start_y + page_size * (card_h + spacing) - 2, pstr, RGB_TEXT_MUTED, &g_font_main);
    }
}

/* Render Devices View (Dynamic Scrolling, 6 buttons per page) */
static void render_devices_view(void) {
    int start_y = 38;
    int card_h = 34;
    int spacing = 5;
    int page_size = 6;
    int total = get_active_dev_cmd_count();

    if (total <= 0) {
        fb_fill_rect(12, start_y + 10, FB_WIDTH - 24, 120, RGB_CARD);
        fb_draw_rect(12, start_y + 10, FB_WIDTH - 24, 120, RGB_DIVIDER);
        fb_draw_text_aa(24, start_y + 30, "No Device Buttons", RGB_WHITE, &g_font_title);
        fb_draw_text_aa(24, start_y + 55, "Configure in WebUI", RGB_CYAN, &g_font_main);
        fb_draw_text_aa(24, start_y + 75, "for this activity", RGB_TEXT_MUTED, &g_font_main);
        return;
    }

    int page_start = (g_selected_device_cmd / page_size) * page_size;
    int cur_page = (g_selected_device_cmd / page_size) + 1;
    int total_pages = (total + page_size - 1) / page_size;

    for (int i = 0; i < page_size && (page_start + i) < total; i++) {
        int idx = page_start + i;
        int y = start_y + i * (card_h + spacing);
        bool is_sel = (idx == g_selected_device_cmd);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : 0x2945;

        char label[32] = {0};
        char icon_key[24] = {0};
        uint8_t bidx = 0, atype = 1;
        get_active_dev_cmd_item(idx, label, sizeof(label), &bidx, &atype, icon_key, sizeof(icon_key));

        fb_fill_rect(8, y, FB_WIDTH - 16, card_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, card_h, border);

        char btn_icon_path[128] = {0};
        bool has_btn_icon = resolve_button_icon(icon_key, label, btn_icon_path, sizeof(btn_icon_path));
        if (has_btn_icon) {
            has_btn_icon = blit_png_file(14, y + (card_h - 22) / 2, 22, 22, btn_icon_path);
        }

        int text_x = has_btn_icon ? 42 : 16;
        fb_draw_text_aa(text_x, y + 9, label, is_sel ? RGB_WHITE : RGB_CYAN, &g_font_main);

        if (is_sel) {
            fb_draw_text_aa(FB_WIDTH - 24, y + 8, ">", RGB_CYAN, &g_font_title);
        }
    }

    if (total_pages > 1) {
        char pstr[16];
        snprintf(pstr, sizeof(pstr), "%d/%d", cur_page, total_pages);
        fb_draw_text_aa(FB_WIDTH - 32, start_y + page_size * (card_h + spacing) - 2, pstr, RGB_TEXT_MUTED, &g_font_main);
    }
}

/* Render Blocking Activity Switch View with dynamic step tracking */
static void render_switching_view(void) {
    /* Header */
    fb_fill_rect(0, 0, FB_WIDTH, 34, RGB_HEADER);
    fb_draw_text_aa(12, 9, "SWITCHING ACTIVITY", RGB_CYAN, &g_font_title);
    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);

    /* Center Card */
    int card_y = 48;
    int card_h = 224;
    fb_fill_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_CARD);
    fb_draw_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_CARD_SEL);

    /* Target Activity Name */
    fb_draw_text_aa(22, card_y + 14, "SWITCHING TO", RGB_TEXT_MUTED, &g_font_main);
    fb_draw_text_aa(22, card_y + 36, g_switching_target_name, RGB_WHITE, &g_font_title);

    /* Draw Target Activity Icon on Right side of Center Card */
    char switch_icon[128] = {0};
    resolve_activity_icon(NULL, g_switching_target_name, switch_icon, sizeof(switch_icon));
    if (switch_icon[0]) {
        blit_png_file(FB_WIDTH - 20 - 52, card_y + 14, 44, 44, switch_icon);
    }

    rf_act_step_t target_steps[32];
    uint32_t calc_total_ms = 0;
    int count = build_combined_activity_sequence(g_active_activity_id, g_switching_target_id,
                                                target_steps, 32, &calc_total_ms);

    uint64_t elapsed = get_monotonic_ms() - g_switching_start_ms;
    uint32_t total_ms = (g_switching_total_ms > 0) ? g_switching_total_ms : calc_total_ms;

    int current_step_idx = 1;
    char current_step_name[32] = "Configuring devices";
    if (strcmp(g_switching_target_id, "-1") == 0) {
        strncpy(current_step_name, "Powering off all devices", sizeof(current_step_name) - 1);
    }

    if (count > 0) {
        uint64_t accum = 0;
        for (int s = 0; s < count; s++) {
            accum += target_steps[s].delay_ms;
            if (elapsed < accum || s == count - 1) {
                current_step_idx = s + 1;
                strncpy(current_step_name, target_steps[s].name, sizeof(current_step_name) - 1);
                current_step_name[sizeof(current_step_name) - 1] = '\0';
                break;
            }
        }
    }

    /* Step counter header */
    char step_hdr[32];
    if (count > 0) {
        snprintf(step_hdr, sizeof(step_hdr), "Step %d of %d", current_step_idx, count);
    } else {
        snprintf(step_hdr, sizeof(step_hdr), "Executing Actions");
    }
    fb_draw_text_aa(22, card_y + 70, step_hdr, RGB_CYAN, &g_font_main);

    /* Step description */
    fb_draw_text_aa(22, card_y + 92, current_step_name, RGB_GREEN, &g_font_title);

    /* Time counter: elapsed / total */
    char time_str[32];
    double cur_sec = (double)elapsed / 1000.0;
    double tot_sec = (double)total_ms / 1000.0;
    if (cur_sec > tot_sec) cur_sec = tot_sec;
    snprintf(time_str, sizeof(time_str), "Timing: %.1fs / %.1fs", cur_sec, tot_sec);
    fb_draw_text_aa(22, card_y + 126, time_str, RGB_TEXT_MUTED, &g_font_main);

    /* Progress bar */
    int bar_x = 22;
    int max_w = FB_WIDTH - 44;
    int bar_w = (int)((elapsed * max_w) / total_ms);
    if (bar_w > max_w) bar_w = max_w;
    fb_fill_rect(bar_x, card_y + 152, max_w, 10, RGB_BG);
    fb_draw_rect(bar_x, card_y + 152, max_w, 10, 0x2945);
    if (bar_w > 0) {
        fb_fill_rect(bar_x, card_y + 152, bar_w, 10, RGB_TEAL);
    }

    fb_draw_text_aa(22, card_y + 178, "Please wait...", RGB_TEXT_MUTED, &g_font_main);

    /* Lock banner */
    fb_fill_rect(0, FB_HEIGHT - 30, FB_WIDTH, 30, RGB_HEADER);
    fb_draw_text_aa(18, FB_HEIGHT - 22, "Screen locked during switch", RGB_TEXT_MUTED, &g_font_main);
}

/* Render Blocking Config Sync View */
static void render_sync_overlay(void) {
    /* Header */
    fb_fill_rect(0, 0, FB_WIDTH, 34, RGB_HEADER);
    fb_draw_text_aa(14, 9, "SYNCHRONIZING CONFIG", RGB_CYAN, &g_font_title);
    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);

    /* Center Card */
    int card_y = 52;
    int card_h = 216;
    fb_fill_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_CARD);
    fb_draw_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_CARD_SEL);

    /* Status Text */
    fb_draw_text_aa(22, card_y + 20, "UPDATING FROM HUB", RGB_WHITE, &g_font_title);
    fb_draw_text_aa(22, card_y + 46, "Syncing activities & mappings...", RGB_TEXT_MUTED, &g_font_main);

    char items_str[48];
    snprintf(items_str, sizeof(items_str), "Acts: %d | Devs: %d | Btns: %d",
             g_rf_activity_count, g_rf_device_count, g_rf_button_count);
    fb_draw_text_aa(22, card_y + 80, items_str, RGB_TEAL, &g_font_main);

    char chunks_str[32];
    snprintf(chunks_str, sizeof(chunks_str), "Chunks received: %d", g_sync_chunk_count);
    fb_draw_text_aa(22, card_y + 106, chunks_str, RGB_TEXT_MUTED, &g_font_main);

    /* Animated progress pulse */
    uint64_t elapsed = get_monotonic_ms() - g_sync_start_ms;
    int bar_x = 22;
    int max_w = FB_WIDTH - 44;
    int pulse_pos = (int)((elapsed / 15) % (max_w > 30 ? max_w - 30 : 1));
    fb_fill_rect(bar_x, card_y + 140, max_w, 8, RGB_BG);
    fb_draw_rect(bar_x, card_y + 140, max_w, 8, 0x2945);
    fb_fill_rect(bar_x + pulse_pos, card_y + 140, 30, 8, RGB_CYAN);

    fb_draw_text_aa(22, card_y + 168, "Please wait...", RGB_TEXT_MUTED, &g_font_main);

    /* Lock banner */
    fb_fill_rect(0, FB_HEIGHT - 30, FB_WIDTH, 30, RGB_HEADER);
    fb_draw_text_aa(18, FB_HEIGHT - 22, "Screen locked during sync", RGB_TEXT_MUTED, &g_font_main);
}

/* Render Blocking RF OTA Firmware Update View */
static void render_ota_overlay(void) {
    /* Header */
    fb_fill_rect(0, 0, FB_WIDTH, 34, RGB_HEADER);
    fb_draw_text_aa(14, 9, "FIRMWARE OTA UPDATE", RGB_YELLOW, &g_font_title);
    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);

    /* Center Card */
    int card_y = 52;
    int card_h = 216;
    fb_fill_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_CARD);
    fb_draw_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_YELLOW);

    /* Title */
    fb_draw_text_aa(22, card_y + 18, "UPDATING FIRMWARE", RGB_WHITE, &g_font_title);
    fb_draw_text_aa(22, card_y + 40, "Receiving codex_elite over RF", RGB_TEXT_MUTED, &g_font_main);

    /* Progress Stats */
    char prog_str[48];
    int pct = (g_ota_total_bytes > 0) ? (int)((g_ota_received_bytes * 100ULL) / g_ota_total_bytes) : 0;
    if (pct > 100) pct = 100;
    snprintf(prog_str, sizeof(prog_str), "Progress: %d%% (%u / %u KB)",
             pct, g_ota_received_bytes / 1024, g_ota_total_bytes / 1024);
    fb_draw_text_aa(22, card_y + 72, prog_str, RGB_YELLOW, &g_font_main);

    char chunk_str[48];
    snprintf(chunk_str, sizeof(chunk_str), "Chunks: %u / %u",
             g_ota_received_chunks, g_ota_total_chunks);
    fb_draw_text_aa(22, card_y + 96, chunk_str, RGB_TEXT_MUTED, &g_font_main);

    /* Progress Bar */
    int bar_x = 22;
    int max_w = FB_WIDTH - 44;
    int bar_w = (int)((pct * max_w) / 100);
    fb_fill_rect(bar_x, card_y + 124, max_w, 10, RGB_BG);
    fb_draw_rect(bar_x, card_y + 124, max_w, 10, 0x2945);
    if (bar_w > 0) {
        fb_fill_rect(bar_x, card_y + 124, bar_w, 10, RGB_YELLOW);
    }

    fb_draw_text_aa(22, card_y + 148, "Do not power off remote or dock!", RGB_RED, &g_font_main);
    fb_draw_text_aa(22, card_y + 172, "Keep remote close to Hub.", RGB_TEXT_MUTED, &g_font_main);

    /* Lock banner */
    fb_fill_rect(0, FB_HEIGHT - 30, FB_WIDTH, 30, RGB_HEADER);
    fb_draw_text_aa(18, FB_HEIGHT - 22, "Firmware update in progress", RGB_YELLOW, &g_font_main);
}

static void execute_remote_reboot(void) {
    show_notice("Rebooting Remote...", 4000);
    render_screen();
    log_power_debug("[*] System reboot initiated by user from Settings menu. Syncing and rebooting (-f)...\n");
    sync();
    system("/data/codex_sync 2>/dev/null; sync 2>/dev/null; reboot -f 2>/dev/null || (sleep 1; echo b > /proc/sysrq-trigger 2>/dev/null) &");
    usleep(250000);
    reboot(RB_AUTOBOOT);
    exit(0);
}

static void execute_elite_factory_reset(void) {
    show_notice("Restoring stock & rebooting...", 6000);
    render_screen();
    system("chmod +x /usr/bin/watchdog /usr/bin/usbhid /usr/bin/hal /opt/luaworks/luaworks /opt/luadraws/luadraws 2>/dev/null || true; "
           "rm -f /etc/tdeenable /etc/nowatchdog /data/reboot_counter /tmp/codex_elite.log 2>/dev/null || true; "
           "if [ -f /data/factory_reset.sh ]; then /data/factory_reset.sh --confirm; "
           "elif [ -f /data/stock_backup/rcS.local.orig ]; then cp -f /data/stock_backup/rcS.local.orig /etc/init.d/rcS.local; "
           "chmod 755 /etc/init.d/rcS.local; rm -f /data/codex_elite /data/codex_sync /data/dropbear*; sync; reboot -f; "
           "else rm -f /etc/init.d/rcS.local /data/codex_elite /data/codex_sync /data/dropbear*; sync; reboot -f; fi &");
    sleep(1);
    exit(0);
}

static int get_settings_sub_count(void) {
    switch (g_settings_subview) {
        case SETTINGS_VIEW_CATEGORIES: return 6;
        case SETTINGS_VIEW_ACTIVITIES: return 3;
        case SETTINGS_VIEW_DISPLAY:    return 5;
        case SETTINGS_VIEW_TOUCH:      return 6;
        case SETTINGS_VIEW_POWER:      return 4;
        case SETTINGS_VIEW_SYSTEM:     return 7;
        case SETTINGS_VIEW_FACTORY_RESET: return 2;
        case SETTINGS_VIEW_REBOOT:        return 2;
        default: return 0;
    }
}

static void execute_settings_action(int idx) {
    if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
        if (idx >= 0 && idx < 5) {
            g_selected_category = idx;
            g_settings_subview = (settings_subview_t)(SETTINGS_VIEW_ACTIVITIES + idx);
            g_selected_sub_item = 0;
        } else if (idx == 5) {
            send_remote_config_backup_rf();
            show_notice("Settings pushed to Hub!", 2500);
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_ACTIVITIES) {
        switch (idx) {
            case 0:
                g_confirm_activity = !g_confirm_activity;
                save_settings_conf();
                break;
            case 1:
                cycle_remote_mode();
                save_settings_conf();
                break;
            case 2:
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_DISPLAY) {
        switch (idx) {
            case 0:
                g_brightness_val = (g_brightness_val % 5) + 1;
                set_backlight_level(g_brightness_val);
                save_settings_conf();
                break;
            case 1:
                g_timeout_seconds = (g_timeout_seconds == 10) ? 15 :
                                    ((g_timeout_seconds == 15) ? 30 :
                                     ((g_timeout_seconds == 30) ? 60 :
                                      ((g_timeout_seconds == 60) ? 120 : 10)));
                save_settings_conf();
                break;
            case 2:
                g_btn_backlight_mode = (g_btn_backlight_mode + 1) % 3;
                fb_set_blank(!g_screen_on);
                save_settings_conf();
                break;
            case 3:
                if (g_night_start_hour == 22) { g_night_start_hour = 23; g_night_end_hour = 6; }
                else if (g_night_start_hour == 23) { g_night_start_hour = 21; g_night_end_hour = 8; }
                else { g_night_start_hour = 22; g_night_end_hour = 7; }
                save_settings_conf();
                break;
            case 4:
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_TOUCH) {
        switch (idx) {
            case 0:
                set_touch_enabled(!g_touch_enabled);
                save_settings_conf();
                break;
            case 1:
                g_cap_buttons_enabled = !g_cap_buttons_enabled;
                save_settings_conf();
                break;
            case 2:
                cycle_touch_sensitivity();
                save_settings_conf();
                break;
            case 3:
                cycle_nav_key();
                save_settings_conf();
                break;
            case 4:
                cycle_nav_mode();
                save_settings_conf();
                break;
            case 5:
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_POWER) {
        switch (idx) {
            case 0:
                set_gyro_enabled(!g_gyro_enabled);
                save_settings_conf();
                break;
            case 1:
                g_gyro_rate_hz = (g_gyro_rate_hz == 10) ? 25 : ((g_gyro_rate_hz == 25) ? 50 : 10);
                save_settings_conf();
                break;
            case 2: {
                read_elite_battery();
                char bat_msg[64];
                snprintf(bat_msg, sizeof(bat_msg), "Battery: %d%% (%dmV) %s",
                         g_battery_info.percent, g_battery_info.voltage_mv,
                         g_battery_info.charging ? "Chg" : "Dis");
                show_notice(bat_msg, 2000);
                break;
            }
            case 3:
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_SYSTEM) {
        switch (idx) {
            case 0: {
                char vmsg[64];
                snprintf(vmsg, sizeof(vmsg), "Codex Elite v%s (cfg=%d)", CODEX_ELITE_VERSION, g_config_version);
                show_notice(vmsg, 2500);
                break;
            }
            case 1:
                toggle_usbeth();
                save_settings_conf();
                break;
            case 2:
                if (g_power_debug) {
                    log_power_debug("[*] Power debugging disabled from System menu\n");
                    g_power_debug = false;
                    save_settings_conf();
                    show_notice("Power Debug: DISABLED", 2000);
                } else {
                    g_power_debug = true;
                    save_settings_conf();
                    log_power_debug("[*] Power debugging enabled from System menu\n");
                    show_notice("Power Debug: ENABLED", 2000);
                }
                break;
            case 3:
                g_current_view = VIEW_PAIRING;
                g_selected_pair_opt = 0;
                g_unpair_confirm_active = false;
                if (g_rf_fd >= 0) {
                    uint8_t q_own[7] = {0x10, 0xFF, 0x83, 0xB5, 0x03, 0x00, 0x00};
                    if (write(g_rf_fd, q_own, sizeof(q_own)) < 0) {}
                    usleep(10000);
                    uint8_t q_hub[7] = {0x10, 0xFF, 0x83, 0xB5, 0x20, 0x00, 0x00};
                    if (write(g_rf_fd, q_hub, sizeof(q_hub)) < 0) {}
                }
                break;
            case 4:
                g_settings_subview = SETTINGS_VIEW_REBOOT;
                g_selected_sub_item = 0;
                break;
            case 5:
                g_settings_subview = SETTINGS_VIEW_FACTORY_RESET;
                g_factory_reset_stage = 0;
                g_selected_sub_item = 0;
                break;
            case 6:
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                break;
            default: break;
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_REBOOT) {
        if (idx == 0) {
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_selected_sub_item = 4;
        } else if (idx == 1) {
            execute_remote_reboot();
        }
        return;
    }

    if (g_settings_subview == SETTINGS_VIEW_FACTORY_RESET) {
        if (idx == 0) {
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_factory_reset_stage = 0;
            g_selected_sub_item = 5;
        } else if (idx == 1) {
            if (g_factory_reset_stage == 0) {
                g_factory_reset_stage = 1;
                g_selected_sub_item = 0;
            } else {
                execute_elite_factory_reset();
            }
        }
        return;
    }
}

static void render_settings_categories(void) {
    int start_y = 38;
    int card_h = 35;
    int spacing = 4;
    int total = 6;

    const char *cat_names[6] = {
        "ACTIVITIES",
        "DISPLAY",
        "TOUCH",
        "POWER",
        "SYSTEM",
        "PUSH TO HUB"
    };
    const char *cat_descs[6] = {
        "Confirm switch, remote mode",
        "Brightness, timeout, night",
        "Sensitivity, nav buttons",
        "Tilt wake, battery health",
        "USB ethernet, pairing, reset",
        "Sync settings over RF"
    };
    const char *cat_icons[6] = {
        "/opt/luadraws/appscommon/asset/footer/devices.png",
        "/opt/luadraws/appscommon/asset/hue/brightness.png",
        "/opt/luadraws/appscommon/asset/gesture/gesturehand.png",
        "/opt/luadraws/appscommon/asset/footer/flip.png",
        "/opt/luadraws/appscommon/asset/pairing/connectHub.png",
        "/opt/luadraws/appscommon/asset/pairing/connectHub.png"
    };

    for (int i = 0; i < total; i++) {
        int y = start_y + i * (card_h + spacing);
        bool is_sel = (i == g_selected_category);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : 0x2945;

        fb_fill_rect(8, y, FB_WIDTH - 16, card_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, card_h, border);

        bool has_icon = blit_png_file(14, y + 7, 20, 20, cat_icons[i]);
        int text_x = has_icon ? 40 : 14;
        fb_draw_text_aa(text_x, y + 3, cat_names[i], is_sel ? RGB_WHITE : RGB_CYAN, &g_font_title);
        fb_draw_text_aa(text_x, y + 19, cat_descs[i], RGB_TEXT_MUTED, &g_font_main);

        if (i == 5) {
            fb_draw_text_aa(FB_WIDTH - 52, y + 10, "[PUSH]", is_sel ? RGB_TEAL : RGB_TEXT_MUTED, &g_font_main);
        } else {
            fb_draw_text_aa(FB_WIDTH - 20, y + 9, ">", is_sel ? RGB_TEAL : RGB_TEXT_MUTED, &g_font_title);
        }
    }
}

static void render_settings_items(const char *title, int count, const char **labels, const char values[][32], const uint16_t *val_colors, const char **icons, int sel_idx) {
    /* Header: Clean title without overlapping button */
    fb_fill_rect(0, 0, FB_WIDTH, 36, RGB_BLACK);
    fb_draw_text_aa(14, 10, title, RGB_CYAN, &g_font_title);

    int start_y = 40;
    int card_h = (count >= 7) ? 30 : ((count >= 6) ? 34 : 36);
    int spacing = (count >= 7) ? 3 : ((count >= 6) ? 4 : 5);

    for (int i = 0; i < count; i++) {
        int y = start_y + i * (card_h + spacing);
        bool is_sel = (i == sel_idx);
        bool is_back = (i == count - 1);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : 0x2945;

        fb_fill_rect(8, y, FB_WIDTH - 16, card_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, card_h, border);

        bool has_icon = false;
        if (icons && icons[i]) {
            has_icon = blit_png_file(14, y + (card_h <= 30 ? 5 : (card_h == 34 ? 7 : 8)), 20, 20, icons[i]);
        }
        int text_x = has_icon ? 38 : 14;
        uint16_t text_col = is_back ? (is_sel ? RGB_WHITE : RGB_CYAN) : (is_sel ? RGB_WHITE : RGB_TEXT_MUTED);
        fb_draw_text_aa(text_x, y + (card_h <= 30 ? 6 : (card_h == 34 ? 8 : 9)), labels[i], text_col, &g_font_main);

        if (values && values[i][0]) {
            int vw = fb_text_width_aa(values[i], &g_font_main);
            uint16_t col = val_colors ? val_colors[i] : RGB_TEAL;
            fb_draw_text_aa(FB_WIDTH - 18 - vw, y + (card_h <= 30 ? 6 : (card_h == 34 ? 8 : 9)), values[i], col, &g_font_main);
        }
    }
}

static void render_settings_factory_reset(void) {
    /* Header */
    fb_fill_rect(0, 0, FB_WIDTH, 36, RGB_BLACK);
    char title[32];
    snprintf(title, sizeof(title), "RESET (%d/2)", g_factory_reset_stage + 1);
    fb_draw_text_aa(14, 10, title, RGB_RED, &g_font_title);

    /* Warning card */
    int card_y = 38;
    int card_h = 130;
    fb_fill_rect(8, card_y, FB_WIDTH - 16, card_h, RGB_CARD);
    fb_draw_rect(8, card_y, FB_WIDTH - 16, card_h, g_factory_reset_stage == 0 ? RGB_YELLOW : RGB_RED);

    if (g_factory_reset_stage == 0) {
        fb_draw_text_aa(16, card_y + 10, "REVERT TO STOCK FIRMWARE", RGB_YELLOW, &g_font_title);
        fb_draw_text_aa(16, card_y + 36, "Removes Codex custom software", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(16, card_y + 56, "and restores official Logitech UI.", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(16, card_y + 82, "Configs preserved on flash:", RGB_TEAL, &g_font_main);
        fb_draw_text_aa(16, card_y + 100, "/data/codex_elite_config.json", RGB_GREEN, &g_font_main);
    } else {
        fb_draw_text_aa(16, card_y + 10, "ARE YOU ABSOLUTELY SURE?", RGB_RED, &g_font_title);
        fb_draw_text_aa(16, card_y + 36, "Remote will restore stock startup,", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(16, card_y + 56, "sync flash, and reboot to Logitech.", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(16, card_y + 82, "Re-running install restores setups.", RGB_TEAL, &g_font_main);
        fb_draw_text_aa(16, card_y + 100, "Device reboots immediately.", RGB_YELLOW, &g_font_main);
    }

    /* Option Buttons */
    int btn_y = card_y + card_h + 10;
    int btn_h = 38;
    int spacing = 8;

    const char *btn_labels[2][2] = {
        { "CANCEL - KEEP CODEX", "PROCEED TO RESET >" },
        { "NO, GO BACK", "YES, RESET REMOTE NOW" }
    };

    for (int i = 0; i < 2; i++) {
        int y = btn_y + i * (btn_h + spacing);
        bool is_sel = (i == g_selected_sub_item);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : 0x2945;
        uint16_t text_col = RGB_WHITE;

        if (i == 1) {
            border = is_sel ? RGB_RED : 0x8000;
            text_col = is_sel ? RGB_RED : RGB_YELLOW;
        }

        fb_fill_rect(8, y, FB_WIDTH - 16, btn_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, btn_h, border);
        int tw = fb_text_width_aa(btn_labels[g_factory_reset_stage][i], &g_font_title);
        fb_draw_text_aa((FB_WIDTH - tw) / 2, y + 10, btn_labels[g_factory_reset_stage][i], text_col, &g_font_title);
    }
}

static void render_settings_reboot(void) {
    render_header();

    /* Warning card */
    int card_y = 38;
    int card_h = 130;
    fb_fill_rect(8, card_y, FB_WIDTH - 16, card_h, RGB_CARD);
    fb_draw_rect(8, card_y, FB_WIDTH - 16, card_h, RGB_YELLOW);

    fb_draw_text_aa(16, card_y + 10, "REBOOT REMOTE", RGB_YELLOW, &g_font_title);
    fb_draw_text_aa(16, card_y + 36, "Restart remote cleanly.", RGB_WHITE, &g_font_main);
    fb_draw_text_aa(16, card_y + 56, "Saved configs and paired Hub", RGB_WHITE, &g_font_main);
    fb_draw_text_aa(16, card_y + 76, "connection will be restored.", RGB_WHITE, &g_font_main);
    fb_draw_text_aa(16, card_y + 100, "Device reboots in a few seconds.", RGB_TEAL, &g_font_main);

    /* Option Buttons */
    int btn_y = card_y + card_h + 10;
    int btn_h = 38;
    int spacing = 8;

    const char *btn_labels[2] = {
        "CANCEL - DO NOT REBOOT",
        "REBOOT REMOTE NOW"
    };

    for (int i = 0; i < 2; i++) {
        int y = btn_y + i * (btn_h + spacing);
        bool is_sel = (i == g_selected_sub_item);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? (i == 1 ? RGB_RED : RGB_TEAL) : (i == 1 ? 0x8000 : 0x2945);
        uint16_t text_col = (i == 1) ? (is_sel ? RGB_RED : RGB_YELLOW) : RGB_WHITE;

        fb_fill_rect(8, y, FB_WIDTH - 16, btn_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, btn_h, border);
        int tw = fb_text_width_aa(btn_labels[i], &g_font_title);
        fb_draw_text_aa((FB_WIDTH - tw) / 2, y + 10, btn_labels[i], text_col, &g_font_title);
    }
}

/* Render Settings View (Hierarchical Submenus) */
static void render_settings_view(void) {
    switch (g_settings_subview) {
        case SETTINGS_VIEW_CATEGORIES:
            render_settings_categories();
            break;
        case SETTINGS_VIEW_ACTIVITIES: {
            const char *labels[3] = { "Confirm Switch", "Remote Mode", "< Back" };
            char values[3][32];
            snprintf(values[0], sizeof(values[0]), "[ %s ]", g_confirm_activity ? "PROMPT" : "INSTANT");
            snprintf(values[1], sizeof(values[1]), "[ %s ]", (g_remote_mode == 1) ? "DIRECT" : "ACTIVITY");
            values[2][0] = '\0';
            uint16_t cols[3] = {
                g_confirm_activity ? RGB_GREEN : RGB_TEXT_MUTED,
                (g_remote_mode == 1) ? RGB_YELLOW : RGB_CYAN,
                RGB_CYAN
            };
            const char *icons[3] = {
                "/opt/luadraws/appscommon/asset/settings/checkmark.png",
                "/opt/luadraws/appscommon/asset/footer/devices.png",
                "/opt/luadraws/appscommon/asset/footer/close.png"
            };
            render_settings_items("ACTIVITIES", 3, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_DISPLAY: {
            const char *labels[5] = { "Display Brightness", "Display Timeout", "Button Backlight", "Night Schedule", "< Back" };
            char values[5][32];
            snprintf(values[0], sizeof(values[0]), "[ LVL %d ]", g_brightness_val);
            snprintf(values[1], sizeof(values[1]), "[ %ds ]", g_timeout_seconds);
            snprintf(values[2], sizeof(values[2]), "[ %s ]", g_btn_backlight_mode == 0 ? "OFF" : (g_btn_backlight_mode == 1 ? "ON" : "NIGHT"));
            snprintf(values[3], sizeof(values[3]), "[ %02d-%02d ]", g_night_start_hour, g_night_end_hour);
            values[4][0] = '\0';
            uint16_t cols[5] = {
                RGB_TEAL,
                RGB_TEAL,
                g_btn_backlight_mode == 0 ? RGB_RED : (g_btn_backlight_mode == 1 ? RGB_GREEN : RGB_YELLOW),
                RGB_TEAL,
                RGB_CYAN
            };
            const char *icons[5] = {
                "/opt/luadraws/appscommon/asset/hue/brightness.png",
                "/opt/luadraws/appscommon/asset/commands/145.png",
                "/opt/luadraws/appscommon/asset/juniperHomeAutomation/lamp_on.png",
                "/opt/luadraws/appscommon/asset/clock_small.png",
                "/opt/luadraws/appscommon/asset/footer/close.png"
            };
            render_settings_items("DISPLAY", 5, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_TOUCH: {
            const char *labels[6] = { "Touchscreen", "Capacitive Buttons", "Touch Sensitivity", "Nav Toggle Button", "Nav Toggle Mode", "< Back" };
            char values[6][32];
            snprintf(values[0], sizeof(values[0]), "[ %s ]", g_touch_enabled ? "ON" : "OFF");
            snprintf(values[1], sizeof(values[1]), "[ %s ]", g_cap_buttons_enabled ? "ON" : "OFF");
            const char *sens_names[] = { "", "LOW", "NORMAL", "HIGH" };
            int s_idx = (g_touch_sensitivity >= 1 && g_touch_sensitivity <= 3) ? g_touch_sensitivity : 2;
            snprintf(values[2], sizeof(values[2]), "[ %s ]", sens_names[s_idx]);
            if (!g_touch_enabled) {
                snprintf(values[3], sizeof(values[3]), "[ LOCKED ]");
                snprintf(values[4], sizeof(values[4]), "[ LOCKED ]");
            } else {
                snprintf(values[3], sizeof(values[3]), "[ %s ]", get_nav_key_name(g_nav_toggle_key));
                snprintf(values[4], sizeof(values[4]), "[ %s ]", g_nav_toggle_mode == 0 ? "SHORT" : "LONG");
            }
            values[5][0] = '\0';
            uint16_t cols[6] = {
                g_touch_enabled ? RGB_GREEN : RGB_RED,
                g_cap_buttons_enabled ? RGB_GREEN : RGB_RED,
                RGB_TEAL,
                !g_touch_enabled ? RGB_TEXT_MUTED : RGB_TEAL,
                !g_touch_enabled ? RGB_TEXT_MUTED : RGB_TEAL,
                RGB_CYAN
            };
            const char *icons[6] = {
                "/opt/luadraws/appscommon/asset/gesture/hint.png",
                "/opt/luadraws/appscommon/asset/footer/devices.png",
                "/opt/luadraws/appscommon/asset/gesture/gesturehand.png",
                "/opt/luadraws/appscommon/asset/edit/arrow.png",
                "/opt/luadraws/appscommon/asset/edit/arrow.png",
                "/opt/luadraws/appscommon/asset/footer/close.png"
            };
            render_settings_items("TOUCH", 6, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_POWER: {
            const char *labels[4] = { "Motion / Gyro Wake", "Gyro Sampling Rate", "Battery Status", "< Back" };
            char values[4][32];
            snprintf(values[0], sizeof(values[0]), "[ %s ]", g_gyro_enabled ? "ON" : "OFF");
            snprintf(values[1], sizeof(values[1]), "[ %dHz ]", g_gyro_rate_hz);
            read_elite_battery();
            snprintf(values[2], sizeof(values[2]), "[ %d%% %dmV ]", g_battery_info.percent, g_battery_info.voltage_mv);
            values[3][0] = '\0';
            uint16_t cols[4] = {
                g_gyro_enabled ? RGB_GREEN : RGB_RED,
                RGB_TEAL,
                (g_battery_info.percent < 20) ? RGB_RED : RGB_GREEN,
                RGB_CYAN
            };
            const char *icons[4] = {
                "/opt/luadraws/appscommon/asset/footer/flip.png",
                "/opt/luadraws/appscommon/asset/footer/flip.png",
                "/opt/luadraws/appscommon/asset/battery/level100.png",
                "/opt/luadraws/appscommon/asset/footer/close.png"
            };
            render_settings_items("POWER", 4, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_SYSTEM: {
            const char *labels[7] = { "Software Version", "USB Ethernet Mode", "Power Debug Log", "RF Pairing Menu", "Reboot Remote", "Factory Reset", "< Back" };
            char values[7][32];
            uint16_t cols[7];

            snprintf(values[0], sizeof(values[0]), "v" CODEX_ELITE_VERSION);
            cols[0] = RGB_GREEN;

            if (!g_usbeth_enabled) {
                snprintf(values[1], sizeof(values[1]), "[ OFF ]");
                cols[1] = RGB_TEXT_MUTED;
            } else {
                if (g_usb_connected) {
                    snprintf(values[1], sizeof(values[1]), "[ ACTIVE ]");
                    cols[1] = RGB_GREEN;
                } else {
                    snprintf(values[1], sizeof(values[1]), "[ UNPLUGGED ]");
                    cols[1] = RGB_YELLOW;
                }
            }
            snprintf(values[2], sizeof(values[2]), "[ %s ]", g_power_debug ? "ON" : "OFF");
            cols[2] = g_power_debug ? RGB_GREEN : RGB_TEXT_MUTED;
            snprintf(values[3], sizeof(values[3]), "[ %s ]", g_rf_link_connected ? "LINKED" : "OPEN");
            cols[3] = g_rf_link_connected ? RGB_GREEN : RGB_YELLOW;
            snprintf(values[4], sizeof(values[4]), "[ REBOOT ]");
            cols[4] = RGB_YELLOW;
            snprintf(values[5], sizeof(values[5]), "[ REVERT ]");
            cols[5] = RGB_RED;
            values[6][0] = '\0';
            cols[6] = RGB_CYAN;
            const char *icons[7] = {
                "/opt/luadraws/appscommon/asset/footer/settings.png",
                "/opt/luadraws/appscommon/asset/header/usb.png",
                "/opt/luadraws/appscommon/asset/header/charging.png",
                "/opt/luadraws/appscommon/asset/pairing/connectHub.png",
                "/opt/luadraws/appscommon/asset/home/power_icon.png",
                "/opt/luadraws/appscommon/asset/footer/close.png",
                "/opt/luadraws/appscommon/asset/footer/close.png"
            };
            render_settings_items("SYSTEM", 7, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_REBOOT:
            render_settings_reboot();
            break;
        case SETTINGS_VIEW_FACTORY_RESET:
            render_settings_factory_reset();
            break;
        default:
            render_settings_categories();
            break;
    }
}

/* Context-aware action executor for Pairing Menu */
static void execute_pairing_menu_opt(int idx) {
    bool is_paired = g_rf_link_connected || (g_pairing_state == 2) ||
                     (g_paired_hub_addr[0] | g_paired_hub_addr[1] | g_paired_hub_addr[2] | g_paired_hub_addr[3]) != 0;

    if (g_unpair_confirm_active) {
        if (idx == 0) {
            rf_unpair();
        } else {
            g_unpair_confirm_active = false;
        }
        g_selected_pair_opt = 0;
    } else if (g_pairing_state == 1) {
        if (idx == 0) {
            rf_stop_pairing();
        } else {
            g_current_view = VIEW_SETTINGS;
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_selected_sub_item = 2;
        }
        g_selected_pair_opt = 0;
    } else if (is_paired) {
        if (idx == 0) {
            g_unpair_confirm_active = true;
            g_selected_pair_opt = 0;
        } else {
            g_current_view = VIEW_SETTINGS;
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_selected_sub_item = 2;
        }
    } else {
        if (idx == 0) {
            rf_start_pairing();
            g_selected_pair_opt = 0;
        } else {
            g_current_view = VIEW_SETTINGS;
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_selected_sub_item = 2;
        }
    }
    render_screen();
}

/* Dedicated Pairing Menu View */
static void render_pairing_view(void) {
    int start_y = 42;
    int card_h = 104;

    /* Info Card: RF Status & Hardware IDs */
    fb_fill_rect(8, start_y, FB_WIDTH - 16, card_h, RGB_CARD);
    fb_draw_rect(8, start_y, FB_WIDTH - 16, card_h, RGB_CARD_BORDER);

    fb_draw_text_aa(16, start_y + 8, "RF HARDWARE & LINK", RGB_CYAN, &g_font_title);

    bool is_paired = g_rf_link_connected || (g_pairing_state == 2) ||
                     (g_paired_hub_addr[0] | g_paired_hub_addr[1] | g_paired_hub_addr[2] | g_paired_hub_addr[3]) != 0;

    char status_str[48];
    if (g_unpair_confirm_active) {
        snprintf(status_str, sizeof(status_str), "UNPAIR CONFIRMATION");
        fb_draw_text_aa(16, start_y + 27, status_str, RGB_YELLOW, &g_font_main);
    } else if (g_pairing_state == 1) {
        snprintf(status_str, sizeof(status_str), "Status: PAIRING IN PROGRESS...");
        fb_draw_text_aa(16, start_y + 27, status_str, RGB_YELLOW, &g_font_main);
    } else if (is_paired) {
        snprintf(status_str, sizeof(status_str), "Status: PAIRED TO HUB (OK)");
        fb_draw_text_aa(16, start_y + 27, status_str, RGB_GREEN, &g_font_main);
    } else if (g_pairing_state == 3) {
        snprintf(status_str, sizeof(status_str), "Status: PAIRING FAILED / TIMEOUT");
        fb_draw_text_aa(16, start_y + 27, status_str, RGB_RED, &g_font_main);
    } else {
        snprintf(status_str, sizeof(status_str), "Status: NOT PAIRED");
        fb_draw_text_aa(16, start_y + 27, status_str, RGB_RED, &g_font_main);
    }

    /* Remote RF Address */
    char remote_addr_str[48];
    if (g_remote_own_addr[0] || g_remote_own_addr[1] || g_remote_own_addr[2] || g_remote_own_addr[3]) {
        snprintf(remote_addr_str, sizeof(remote_addr_str), "Remote RF: %02X%02X%02X%02X (Slot %d)",
                 g_remote_own_addr[0], g_remote_own_addr[1], g_remote_own_addr[2], g_remote_own_addr[3], g_remote_slot);
    } else {
        snprintf(remote_addr_str, sizeof(remote_addr_str), "Remote RF: [ READING... ] (Slot %d)", g_remote_slot);
    }
    fb_draw_text_aa(16, start_y + 46, remote_addr_str, RGB_WHITE, &g_font_main);

    /* Paired Hub RF Address */
    char hub_addr_str[48];
    if (g_paired_hub_addr[0] || g_paired_hub_addr[1] || g_paired_hub_addr[2] || g_paired_hub_addr[3]) {
        snprintf(hub_addr_str, sizeof(hub_addr_str), "Paired Hub: %02X%02X%02X%02X",
                 g_paired_hub_addr[0], g_paired_hub_addr[1], g_paired_hub_addr[2], g_paired_hub_addr[3]);
        fb_draw_text_aa(16, start_y + 65, hub_addr_str, RGB_TEAL, &g_font_main);
    } else if (g_pairing_state == 1) {
        fb_draw_text_aa(16, start_y + 65, "Paired Hub: [ SEARCHING... ]", RGB_YELLOW, &g_font_main);
    } else {
        fb_draw_text_aa(16, start_y + 65, "Paired Hub: [ NONE / UNPAIRED ]", RGB_TEXT_MUTED, &g_font_main);
    }

    if (g_unpair_confirm_active) {
        fb_draw_text_aa(16, start_y + 84, "Wipe bonding & local config?", RGB_YELLOW, &g_font_main);
    } else {
        fb_draw_text_aa(16, start_y + 84, "Protocol: eQuad / CC2544", RGB_TEXT_MUTED, &g_font_main);
    }

    /* Context-Aware Action Options (2 items) */
    const char *opts[PAIR_OPT_COUNT];
    if (g_unpair_confirm_active) {
        opts[0] = "Confirm Unpair & Wipe";
        opts[1] = "Cancel";
    } else if (g_pairing_state == 1) {
        opts[0] = "Stop Pairing";
        opts[1] = "Back to Settings";
    } else if (is_paired) {
        opts[0] = "Unpair from Hub";
        opts[1] = "Back to Settings";
    } else {
        opts[0] = "Start Pairing to Hub";
        opts[1] = "Back to Settings";
    }

    int opt_y = start_y + 114;
    int opt_h = 40;
    int spacing = 10;

    for (int i = 0; i < PAIR_OPT_COUNT; i++) {
        int y = opt_y + i * (opt_h + spacing);
        bool is_sel = (i == g_selected_pair_opt);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? (g_unpair_confirm_active && i == 0 ? RGB_RED : RGB_TEAL) : 0x2945;
        uint16_t fg = is_sel ? (g_unpair_confirm_active && i == 0 ? RGB_RED : RGB_WHITE) : RGB_TEXT_MUTED;

        fb_fill_rect(8, y, FB_WIDTH - 16, opt_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, opt_h, border);

        fb_draw_text_aa(18, y + 12, opts[i], fg, &g_font_main);

        if (is_sel) {
            fb_draw_text_aa(FB_WIDTH - 28, y + 11, ">", (g_unpair_confirm_active && i == 0 ? RGB_RED : RGB_TEAL), &g_font_title);
        }
    }
}

/* Main Display Render Routine (100% Flicker-Free Double-Buffered) */
static void render_screen(void) {
    if (!g_fb_mem || !g_screen_on) return;
    pthread_mutex_lock(&g_render_mutex);

    /* 1. Paint background into offscreen backbuffer */
    if (g_has_wallpaper) {
        memcpy(g_backbuffer, g_bg_wallpaper, sizeof(g_backbuffer));
    } else {
        memset(g_backbuffer, 0x10, sizeof(g_backbuffer));
    }

    if (g_is_ota_updating) {
        render_ota_overlay();
    } else if (g_is_syncing_config) {
        render_sync_overlay();
    } else if (g_is_switching_activity) {
        render_switching_view();
    } else {
        render_header();

        switch (g_current_view) {
            case VIEW_ACTIVITIES:
                if (g_remote_mode == 1) render_devices_view();
                else render_activities_view();
                break;
            case VIEW_DEVICES:    render_devices_view();    break;
            case VIEW_SETTINGS:   render_settings_view();   break;
            case VIEW_PAIRING:    render_pairing_view();    break;
            default: break;
        }

        render_footer();
    }

    /* Notice popup banner */
    if (g_notice_until_ms > get_monotonic_ms() && g_notice_msg[0]) {
        int nw = fb_text_width_aa(g_notice_msg, &g_font_main);
        int box_w = nw + 16;
        if (box_w > FB_WIDTH - 8) box_w = FB_WIDTH - 8;
        int box_h = 28;
        int box_x = (FB_WIDTH - box_w) / 2;
        int box_y = FB_HEIGHT - 68; /* above bottom tabs */
        fb_fill_rect(box_x, box_y, box_w, box_h, RGB_CARD);
        fb_draw_rect(box_x, box_y, box_w, box_h, RGB_TEAL);
        fb_draw_text_aa(box_x + (box_w - nw) / 2, box_y + 7, g_notice_msg, RGB_WHITE, &g_font_main);
    }

    /* 2. Atomically copy backbuffer to physical framebuffer */
    memcpy((void *)g_fb_mem, g_backbuffer, FB_SIZE);
    if (g_fb_size >= FB_SIZE * 2) {
        memcpy((uint8_t *)g_fb_mem + FB_SIZE, g_backbuffer, FB_SIZE);
    }
    if (g_fb_size >= FB_SIZE * 3) {
        memcpy((uint8_t *)g_fb_mem + (FB_SIZE * 2), g_backbuffer, FB_SIZE);
    }

    g_screen_dirty = false;
    pthread_mutex_unlock(&g_render_mutex);
}

/* User interaction resets the idle sleep timer */
static void reset_idle_timer(void) {
    g_last_activity_time = time(NULL);
    if (!g_screen_on) {
        fb_set_blank(false);
        rf_wake_and_reconnect();
        render_screen();
    }
}

/* Accelerometer Hardware Motion Interrupt Initialization (INT1 / event1) */
static void init_gyro_interrupt(void) {
    int fdr = open(GYRO_DATA_RATE_SYS, O_WRONLY);
    if (fdr >= 0) {
        char rstr[16];
        snprintf(rstr, sizeof(rstr), "%d\n", g_gyro_rate_hz > 0 ? g_gyro_rate_hz : 25);
        write(fdr, rstr, strlen(rstr));
        close(fdr);
    }
    int fdt = open(GYRO_INT1_THRESHOLD_SYS, O_WRONLY);
    if (fdt >= 0) {
        write(fdt, "3\n", 2);
        close(fdt);
    }
    int fdd = open(GYRO_INT1_DURATION_SYS, O_WRONLY);
    if (fdd >= 0) {
        write(fdd, "1\n", 2);
        close(fdd);
    }
    printf("[+] Accelerometer INT1 hardware interrupt configured (rate=%dHz, thresh=3, dur=1)\n",
           g_gyro_rate_hz > 0 ? g_gyro_rate_hz : 25);
}

static volatile sig_atomic_t g_wake_requested = 0;
static void sig_wake_handler(int sig) {
    (void)sig;
    g_wake_requested = 1;
}

static void sig_deep_sleep_handler(int sig) {
    (void)sig;
    fb_set_blank(true);
    uint64_t now_ms = get_monotonic_ms();
    s_last_blank_time_ms = (now_ms > 2000000) ? (now_ms - 2000000) : 1;
    log_power_debug("[*] Forcing IMMEDIATE Deep Standby via SIGUSR2\n");
}

/* Kernel Suspend-to-RAM (mem) Coordination */
static void enter_kernel_suspend(void) {
    /* Only suspend to RAM on pure battery; never on USB */
    read_elite_battery();
    if (strcmp(g_battery_info.powersource, "battery") != 0) {
        return;
    }

    /* Arm MSP430 wake timer (3600 seconds) */
    int mfd = open("/sys/bus/i2c/devices/1-0034/wakeup", O_WRONLY);
    if (mfd >= 0) {
        write(mfd, "3600\n", 5);
        close(mfd);
    }

    log_power_debug("[*] Entering kernel Suspend-to-RAM (echo mem > /sys/power/state)...\n");
    int fd = open("/sys/power/state", O_WRONLY);
    if (fd >= 0) {
        ssize_t w = write(fd, "mem\n", 4);
        close(fd);
        if (w < 0) {
            log_power_debug("[-] Kernel suspend returned error: %s\n", strerror(errno));
            usleep(5000000); /* Back off if suspend rejected */
            return;
        }
    }
    log_power_debug("[*] Resumed from kernel Suspend-to-RAM!\n");

    /* Check wake reason from MSP430 */
    FILE *fev = fopen("/sys/bus/i2c/devices/1-0034/wakeup_event", "r");
    if (fev) {
        char wev[32] = {0};
        if (fscanf(fev, "%31s", wev) == 1) {
            log_power_debug("[*] Kernel resume wakeup_event: %s\n", wev);
            if (strcmp(wev, "alarm") != 0) {
                /* Woke from key, tilt, or cradle transition -> request display wake */
                g_wake_requested = 1;
            }
        }
        fclose(fev);
    } else {
        g_wake_requested = 1;
    }

    /* Re-synchronize CC2544 FHSS with Hub */
    rf_wake_and_reconnect();
}

/* Power / Timeout Watcher Thread */
static void *power_manager(void *arg) {
    (void)arg;
    uint64_t last_standby_heartbeat_ms = 0;

    while (g_running) {
        if (!g_screen_on) {
            bool is_docked = is_elite_docked();
            if (is_docked) {
                usleep(500000);  /* 500ms check when charging on dock (external power) */
            } else {
                read_elite_battery();
                if (strcmp(g_battery_info.powersource, "battery") == 0) {
                    /* Enter kernel Suspend-to-RAM on battery */
                    enter_kernel_suspend();
                    usleep(100000);
                } else {
                    usleep(10000000); /* 10s sleep when on USB (preserve USB dev connection) */
                }
            }

            /* Check for dock removal or attachment transition */
            FILE *fpsrc = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/powersource", "r");
            if (fpsrc) {
                char psrc[16] = {0};
                if (fscanf(fpsrc, "%15s", psrc) == 1) {
                    bool was_charging = (strcmp(g_battery_info.powersource, "cradle") == 0 ||
                                         strcmp(g_battery_info.powersource, "usb") == 0 ||
                                         g_battery_info.charging);
                    bool now_charging = (strcmp(psrc, "cradle") == 0 || strcmp(psrc, "usb") == 0);

                    if (was_charging && !now_charging) {
                        /* Removed from dock / disconnected charging */
                        log_power_debug("[*] Dock disconnected (%s -> %s), waking display\n",
                                        g_battery_info.powersource, psrc);
                        printf("[*] Dock disconnected (%s -> %s), waking display\n",
                               g_battery_info.powersource, psrc);
                        s_last_batt_sample_ms = 0;
                        read_elite_battery();
                        g_wake_requested = 1;
                    } else if (!was_charging && now_charging) {
                        /* Placed on dock / connected charging */
                        log_power_debug("[*] Dock connected (%s -> %s), waking display\n",
                                        g_battery_info.powersource, psrc);
                        printf("[*] Dock connected (%s -> %s), waking display\n",
                               g_battery_info.powersource, psrc);
                        s_last_batt_sample_ms = 0;
                        read_elite_battery();
                        g_wake_requested = 1;
                    }
                }
                fclose(fpsrc);
            }

            uint64_t now_ms = get_monotonic_ms();
            if (now_ms - last_standby_heartbeat_ms >= 300000) { /* 300s / 5m heartbeat (was 60s) */
                last_standby_heartbeat_ms = now_ms;
                read_elite_battery();
                char cstat[96];
                get_cpu_freq_status(cstat, sizeof(cstat));
                uint64_t elapsed_s = s_last_blank_time_ms > 0 ? (now_ms - s_last_blank_time_ms) / 1000 : 0;
                log_power_debug("[HEARTBEAT in STANDBY] Slept %llus | Batt: %dmV, %d%%, %s (chg=%d) | CPU: %s\n",
                                (unsigned long long)elapsed_s,
                                g_battery_info.voltage_mv, g_battery_info.percent,
                                g_battery_info.powersource, g_battery_info.charging, cstat);
            }
        } else {
            usleep(200000);  /* 200ms check when screen awake */
            last_standby_heartbeat_ms = 0;
        }
        if (g_wake_requested) {
            g_wake_requested = 0;
            reset_idle_timer();
        }
        if (g_timeout_seconds > 0 && g_screen_on) {
            time_t now = time(NULL);
            if (now - g_last_activity_time > g_timeout_seconds) {
                if (g_is_ota_updating) {
                    reset_idle_timer();
                } else {
                    printf("[*] Idle timeout reached (%ds), blanking display\n", g_timeout_seconds);
                    fb_set_blank(true);
                }
            }
        }
    }
    return NULL;
}

/* Handle Touchscreen tap at coordinates (x, y) */
static void handle_screen_touch_tap(int x, int y) {
    reset_idle_timer();
    if (!g_screen_on) {
        fb_set_blank(false);
        render_screen();
        return;
    }
    if (!g_touch_enabled) return;
    if (g_is_switching_activity || g_is_syncing_config || g_is_ota_updating) return;

    printf("[*] Touch tap: screen x=%d, y=%d (down at %d, %d)\n", x, y, g_touch_down_x, g_touch_down_y);

    int h_thresh = (g_touch_sensitivity == 1) ? 48 : ((g_touch_sensitivity == 3) ? 28 : 38);
    int v_thresh = (g_touch_sensitivity == 1) ? 36 : ((g_touch_sensitivity == 3) ? 22 : 28);
    int tap_slop = (g_touch_sensitivity == 1) ? 16 : ((g_touch_sensitivity == 3) ? 25 : 20);

    /* Horizontal Swipe Gesture across views (only if displacement exceeds tap_slop and has 1.5x horizontal dominance) */
    int dx = x - g_touch_down_x;
    int dy = y - g_touch_down_y;
    bool is_tap = (abs(dx) <= tap_slop && abs(dy) <= tap_slop);

    if (!is_tap && abs(dx) >= h_thresh && abs(dx) >= (abs(dy) * 3 / 2) && y < 280) {
        if (dx > 0) {
            /* Swipe Right -> Previous View (wraparound across main tabs) */
            if (g_current_view == VIEW_ACTIVITIES) g_current_view = VIEW_SETTINGS;
            else if (g_current_view == VIEW_DEVICES) g_current_view = VIEW_ACTIVITIES;
            else if (g_current_view == VIEW_SETTINGS) g_current_view = VIEW_DEVICES;
            else if (g_current_view == VIEW_PAIRING) g_current_view = VIEW_SETTINGS;
            g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            g_selected_sub_item = 0;
            g_factory_reset_stage = 0;
            printf("[*] Swipe right (dx=%d) -> view %d\n", dx, g_current_view);
            render_screen();
            return;
        } else {
            /* Swipe Left -> Next View (wraparound across main tabs) */
            if (g_current_view == VIEW_ACTIVITIES) g_current_view = VIEW_DEVICES;
            else if (g_current_view == VIEW_DEVICES) g_current_view = VIEW_SETTINGS;
            else if (g_current_view == VIEW_SETTINGS) g_current_view = VIEW_ACTIVITIES;
            else if (g_current_view == VIEW_PAIRING) g_current_view = VIEW_SETTINGS;
            g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            g_selected_sub_item = 0;
            g_factory_reset_stage = 0;
            printf("[*] Swipe left (dx=%d) -> view %d\n", dx, g_current_view);
            render_screen();
            return;
        }
    }

    /* Vertical Swipe Gesture for scrolling cards/pages (only if displacement exceeds tap_slop and has 1.5x vertical dominance) */
    if (!is_tap && abs(dy) >= v_thresh && abs(dy) >= (abs(dx) * 3 / 2)) {
        if (dy < 0) {
            /* Swipe Up -> Next full page */
            printf("[*] Swipe Up (dy=%d) -> next page\n", dy);
            if (g_current_view == VIEW_ACTIVITIES) {
                if (g_remote_mode == 1) {
                    int tot = get_active_dev_cmd_count();
                    int cur_page = g_selected_device_cmd / 6;
                    if ((cur_page + 1) * 6 < tot) g_selected_device_cmd = (cur_page + 1) * 6;
                    else if (tot > 0) g_selected_device_cmd = tot - 1;
                } else {
                    int cur_page = g_selected_activity / 4;
                    if ((cur_page + 1) * 4 < g_activity_count) g_selected_activity = (cur_page + 1) * 4;
                    else if (g_activity_count > 0) g_selected_activity = g_activity_count - 1;
                }
            } else if (g_current_view == VIEW_DEVICES) {
                int tot = get_active_dev_cmd_count();
                int cur_page = g_selected_device_cmd / 6;
                if ((cur_page + 1) * 6 < tot) g_selected_device_cmd = (cur_page + 1) * 6;
                else if (tot > 0) g_selected_device_cmd = tot - 1;
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
                    g_selected_category = (g_selected_category + 1) % 6;
                } else {
                    int count = get_settings_sub_count();
                    if (count > 0) g_selected_sub_item = (g_selected_sub_item + 1) % count;
                }
            } else if (g_current_view == VIEW_PAIRING) {
                g_selected_pair_opt = (g_selected_pair_opt + 1) % PAIR_OPT_COUNT;
            }
        } else {
            /* Swipe Down -> Prev full page */
            printf("[*] Swipe Down (dy=%d) -> prev page\n", dy);
            if (g_current_view == VIEW_ACTIVITIES) {
                if (g_remote_mode == 1) {
                    int cur_page = g_selected_device_cmd / 6;
                    if (cur_page > 0) g_selected_device_cmd = (cur_page - 1) * 6;
                    else g_selected_device_cmd = 0;
                } else {
                    int cur_page = g_selected_activity / 4;
                    if (cur_page > 0) g_selected_activity = (cur_page - 1) * 4;
                    else g_selected_activity = 0;
                }
            } else if (g_current_view == VIEW_DEVICES) {
                int cur_page = g_selected_device_cmd / 6;
                if (cur_page > 0) g_selected_device_cmd = (cur_page - 1) * 6;
                else g_selected_device_cmd = 0;
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
                    g_selected_category = (g_selected_category + 5) % 6;
                } else {
                    int count = get_settings_sub_count();
                    if (count > 0) g_selected_sub_item = (g_selected_sub_item + count - 1) % count;
                }
            } else if (g_current_view == VIEW_PAIRING) {
                g_selected_pair_opt = (g_selected_pair_opt + PAIR_OPT_COUNT - 1) % PAIR_OPT_COUNT;
            }
        }
        render_screen();
        return;
    }

    /* Bottom Dock Bar: y >= 280 (dock_y = 320 - 38 = 282) */
    if (y >= 280 && y < 320) {
        if (g_confirm_pending_idx >= 0) {
            g_confirm_pending_idx = -1;
        }
        g_confirm_pending_rf_code = 0;
        ui_view_t prev_view = g_current_view;
        if (x < 62) {
            /* Tab: Activities */
            g_current_view = VIEW_ACTIVITIES;
            g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            g_selected_sub_item = 0;
            g_factory_reset_stage = 0;
        } else if (x < 124) {
            /* Tab: Devices */
            g_current_view = VIEW_DEVICES;
            g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            g_selected_sub_item = 0;
            g_factory_reset_stage = 0;
        } else if (x < 186) {
            /* Tab: Settings */
            g_current_view = VIEW_SETTINGS;
            g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            g_selected_sub_item = 0;
            g_factory_reset_stage = 0;
        } else {
            /* Mode Pill: Toggle NAV / DEV */
            if (g_touch_enabled) {
                g_screen_nav_mode = !g_screen_nav_mode;
                printf("[*] Mode toggled by screen dock: %s\n", g_screen_nav_mode ? "NAV" : "DEV");
            }
        }
        if ((prev_view == VIEW_SETTINGS || prev_view == VIEW_PAIRING) && g_current_view != prev_view && g_settings_dirty) {
            g_settings_dirty = false;
        }
        render_screen();
        return;
    }

    /* Capacitive touch buttons below LCD screen (screen height is 320): y >= 320 */
    if (y >= 320) {
        g_confirm_pending_rf_code = 0;
        if (x < 120) {
            printf("[*] Capacitive tap: Activities (x=%d, y=%d)\n", x, y);
            g_current_view = VIEW_ACTIVITIES;
        } else {
            printf("[*] Capacitive tap: Devices (x=%d, y=%d)\n", x, y);
            g_current_view = VIEW_DEVICES;
        }
        render_screen();
        return;
    }

    /* Filter out ambiguous drags or flick release that didn't meet swipe thresholds */
    if (!is_tap) {
        return;
    }

    /* Content Area: 38 <= y < 280 */
    int start_y = 38;
    if (g_current_view == VIEW_ACTIVITIES) {
        if (g_remote_mode == 1) {
            int card_h = 34;
            int spacing = 5;
            int page_size = 6;
            int page_start = (g_selected_device_cmd / page_size) * page_size;
            int total = get_active_dev_cmd_count();
            for (int i = 0; i < page_size && (page_start + i) < total; i++) {
                int cy = start_y + i * (card_h + spacing);
                if (y >= cy && y < cy + card_h + spacing) {
                    int idx = page_start + i;
                    g_selected_device_cmd = idx;
                    execute_active_dev_cmd(idx);
                    render_screen();
                    return;
                }
            }
            return;
        }
        int card_h = 56;
        int spacing = 4;
        int page_size = 4;
        int page_start = (g_selected_activity / page_size) * page_size;
        for (int i = 0; i < page_size && (page_start + i) < g_activity_count; i++) {
            int cy = start_y + i * (card_h + spacing);
            if (y >= cy && y < cy + card_h + spacing) {
                int idx = page_start + i;
                g_selected_activity = idx;
                printf("[*] Touch selected Activity: %s (id=%s)\n", g_activities[idx].name, g_activities[idx].id);
                request_activity_switch_with_confirm(idx);
                return;
            }
        }
    } else if (g_current_view == VIEW_DEVICES) {
        int card_h = 34;
        int spacing = 5;
        int page_size = 6;
        int page_start = (g_selected_device_cmd / page_size) * page_size;
        int total = get_active_dev_cmd_count();
        for (int i = 0; i < page_size && (page_start + i) < total; i++) {
            int cy = start_y + i * (card_h + spacing);
            if (y >= cy && y < cy + card_h + spacing) {
                int idx = page_start + i;
                g_selected_device_cmd = idx;
                execute_active_dev_cmd(idx);
                render_screen();
                return;
            }
        }
    } else if (g_current_view == VIEW_SETTINGS) {
        if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
            int start_y = 38;
            int card_h = 35;
            int spacing = 4;
            for (int i = 0; i < 6; i++) {
                int cy = start_y + i * (card_h + spacing);
                if (y >= cy && y < cy + card_h) {
                    g_selected_category = i;
                    execute_settings_action(i);
                    render_screen();
                    return;
                }
            }
        } else if (g_settings_subview == SETTINGS_VIEW_FACTORY_RESET || g_settings_subview == SETTINGS_VIEW_REBOOT) {
            int card_y = 38;
            int card_h = 130;
            int btn_y = card_y + card_h + 10;
            int btn_h = 38;
            int spacing = 8;
            for (int i = 0; i < 2; i++) {
                int cy = btn_y + i * (btn_h + spacing);
                if (y >= cy && y < cy + btn_h) {
                    g_selected_sub_item = i;
                    execute_settings_action(i);
                    render_screen();
                    return;
                }
            }
        } else {
            int start_y = 40;
            int count = get_settings_sub_count();
            int card_h = (count >= 7) ? 30 : ((count >= 6) ? 34 : 36);
            int spacing = (count >= 7) ? 3 : ((count >= 6) ? 4 : 5);
            for (int i = 0; i < count; i++) {
                int cy = start_y + i * (card_h + spacing);
                if (y >= cy && y < cy + card_h) {
                    g_selected_sub_item = i;
                    execute_settings_action(i);
                    render_screen();
                    return;
                }
            }
        }
    } else if (g_current_view == VIEW_PAIRING) {
        int opt_y = 42 + 114;
        int opt_h = 40;
        int spacing = 10;
        for (int i = 0; i < PAIR_OPT_COUNT; i++) {
            int cy = opt_y + i * (opt_h + spacing);
            if (y >= cy && y < cy + opt_h) {
                g_selected_pair_opt = i;
                execute_pairing_menu_opt(i);
                return;
            }
        }
    }
}

/* Navigation & Button Handling */
static void handle_navigation_key(int code, int value) {
    if (value != 1 && value != 2) return; /* Only on press/repeat */
    reset_idle_timer();

    /* If a confirmation prompt is active, physical back/nav buttons cancel the prompt without switching screens */
    if (g_confirm_pending_idx >= 0 || g_confirm_pending_rf_code != 0 || g_unpair_confirm_active) {
        if (code == 158 || code == 202 || code == 149 || code == 203 || (g_cap_buttons_enabled && (code == 364 || code == 102))) {
            printf("[*] Confirm cancelled by back/nav key (code=%d)\n", code);
            g_confirm_pending_idx = -1;
            g_confirm_pending_deadline_ms = 0;
            g_confirm_pending_rf_code = 0;
            g_confirm_pending_rf_deadline_ms = 0;
            g_unpair_confirm_active = false;
            show_notice(NULL, 0);
            render_screen();
            return;
        }
    }

    /* Capacitive touch buttons directly below screen: 364=Activities, 102=Devices */
    if (code == 364) { /* Left capacitive button directly below screen: always Activities */
        if (!g_cap_buttons_enabled) return;
        if (g_is_switching_activity || g_is_syncing_config || g_is_ota_updating) return;
        if (g_current_view == VIEW_ACTIVITIES) return;
        printf("[*] Capacitive key pressed: Activities (code=364)\n");
        g_current_view = VIEW_ACTIVITIES;
        render_screen();
        return;
    }
    if (code == 102) { /* Right capacitive button directly below screen: always Devices */
        if (!g_cap_buttons_enabled) return;
        if (g_is_switching_activity || g_is_syncing_config || g_is_ota_updating) return;
        if (g_current_view == VIEW_DEVICES) return;
        printf("[*] Capacitive key pressed: Devices (code=102)\n");
        g_current_view = VIEW_DEVICES;
        render_screen();
        return;
    }

    /* If switching activity, sync in progress, or OTA updating, block navigation */
    if (g_is_switching_activity || g_is_syncing_config || g_is_ota_updating) return;

    /* In Screen Navigation Mode (or when on Settings): Bottom buttons navigate on-screen cards */
    if (!g_screen_nav_mode && g_touch_enabled && g_current_view != VIEW_SETTINGS && g_current_view != VIEW_PAIRING) {
        return;
    }

    switch (code) {
        case 78:  /* KEY_KPPLUS (RockerUp / Dimmer +) */
            if (g_current_view == VIEW_ACTIVITIES) {
                if (g_remote_mode == 1) {
                    int count = get_active_dev_cmd_count();
                    if (count > 0) {
                        if (g_selected_device_cmd > 0) g_selected_device_cmd--;
                        else g_selected_device_cmd = count - 1;
                    }
                } else {
                    if (g_activity_count > 0) {
                        if (g_selected_activity > 0) g_selected_activity--;
                        else g_selected_activity = g_activity_count - 1;
                    }
                }
            } else if (g_current_view == VIEW_DEVICES) {
                int count = get_active_dev_cmd_count();
                if (count > 0) {
                    if (g_selected_device_cmd > 0) g_selected_device_cmd--;
                    else g_selected_device_cmd = count - 1;
                }
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
                    g_selected_category = (g_selected_category + 5) % 6;
                } else {
                    int count = get_settings_sub_count();
                    if (count > 0) g_selected_sub_item = (g_selected_sub_item + count - 1) % count;
                }
            } else if (g_current_view == VIEW_PAIRING) {
                if (g_selected_pair_opt > 0) g_selected_pair_opt--;
                else g_selected_pair_opt = PAIR_OPT_COUNT - 1;
            }
            render_screen();
            break;

        case 74:  /* KEY_KPMINUS (RockerDown / Dimmer -) */
            if (g_current_view == VIEW_ACTIVITIES) {
                if (g_remote_mode == 1) {
                    int count = get_active_dev_cmd_count();
                    if (count > 0) {
                        if (g_selected_device_cmd < count - 1) g_selected_device_cmd++;
                        else g_selected_device_cmd = 0;
                    }
                } else {
                    if (g_activity_count > 0) {
                        if (g_selected_activity < g_activity_count - 1) g_selected_activity++;
                        else g_selected_activity = 0;
                    }
                }
            } else if (g_current_view == VIEW_DEVICES) {
                int count = get_active_dev_cmd_count();
                if (count > 0) {
                    if (g_selected_device_cmd < count - 1) g_selected_device_cmd++;
                    else g_selected_device_cmd = 0;
                }
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
                    g_selected_category = (g_selected_category + 1) % 6;
                } else {
                    int count = get_settings_sub_count();
                    if (count > 0) g_selected_sub_item = (g_selected_sub_item + 1) % count;
                }
            } else if (g_current_view == VIEW_PAIRING) {
                if (g_selected_pair_opt < PAIR_OPT_COUNT - 1) g_selected_pair_opt++;
                else g_selected_pair_opt = 0;
            }
            render_screen();
            break;

        case 149: /* KEY_PROG2 (Light Off -> Left / Prev Tab) */
            if ((g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING) && g_settings_dirty) {
                g_settings_dirty = false;
            }
            if (g_current_view == VIEW_PAIRING) {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_SYSTEM;
                g_selected_sub_item = 3;
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_REBOOT || g_settings_subview == SETTINGS_VIEW_FACTORY_RESET) {
                    bool was_reboot = (g_settings_subview == SETTINGS_VIEW_REBOOT);
                    g_settings_subview = SETTINGS_VIEW_SYSTEM;
                    g_factory_reset_stage = 0;
                    g_selected_sub_item = was_reboot ? 4 : 5;
                } else if (g_settings_subview != SETTINGS_VIEW_CATEGORIES) {
                    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                    g_factory_reset_stage = 0;
                } else {
                    g_current_view = VIEW_DEVICES;
                }
            } else if (g_current_view == VIEW_DEVICES) {
                g_current_view = VIEW_ACTIVITIES;
            } else if (g_current_view == VIEW_ACTIVITIES) {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            }
            render_screen();
            break;

        case 203: /* KEY_PROG4 (Ha4 / Socket Off -> Right / Next Tab) */
            if ((g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING) && g_settings_dirty) {
                g_settings_dirty = false;
            }
            if (g_current_view == VIEW_PAIRING) {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_SYSTEM;
                g_selected_sub_item = 3;
            } else if (g_current_view == VIEW_ACTIVITIES) {
                g_current_view = VIEW_DEVICES;
            } else if (g_current_view == VIEW_DEVICES) {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview != SETTINGS_VIEW_CATEGORIES) {
                    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                    g_factory_reset_stage = 0;
                } else {
                    g_current_view = VIEW_ACTIVITIES;
                }
            }
            render_screen();
            break;

        case 148: /* KEY_PROG1 (Ha1 / Light On -> Select / OK) */
            if (g_confirm_pending_rf_code != 0 && get_monotonic_ms() < g_confirm_pending_rf_deadline_ms) {
                int pending_rf = g_confirm_pending_rf_code;
                g_confirm_pending_rf_code = 0;
                g_confirm_pending_rf_deadline_ms = 0;
                show_notice(NULL, 0);
                activity_trigger_btn_t *matched_trig = NULL;
                for (int t = 0; t < g_act_trigger_count; t++) {
                    if ((g_act_triggers[t].rf_key_code != 0 && g_act_triggers[t].rf_key_code == pending_rf) ||
                        strcasecmp(g_act_triggers[t].button_name, rf_short_button_name(pending_rf)) == 0) {
                        matched_trig = &g_act_triggers[t];
                        break;
                    }
                }
                if (matched_trig) {
                    const char *target_id = matched_trig->target_act_id;
                    if (matched_trig->atype == 0x03) target_id = "-1";
                    const char *target_name = "PowerOff";
                    if (strcmp(target_id, "-1") != 0) {
                        for (int a = 0; a < g_activity_count; a++) {
                            if (strcmp(g_activities[a].id, target_id) == 0) {
                                target_name = g_activities[a].name;
                                break;
                            }
                        }
                    }
                    trigger_activity_switch(target_id, target_name);
                    render_screen();
                    break;
                }
            }
            if (g_confirm_pending_idx >= 0 && get_monotonic_ms() < g_confirm_pending_deadline_ms) {
                int idx = g_confirm_pending_idx;
                g_confirm_pending_idx = -1;
                g_confirm_pending_deadline_ms = 0;
                show_notice(NULL, 0);
                trigger_activity_switch(g_activities[idx].id, g_activities[idx].name);
                render_screen();
                break;
            }
            if (g_current_view == VIEW_ACTIVITIES) {
                if (g_remote_mode == 1) {
                    execute_active_dev_cmd(g_selected_device_cmd);
                    render_screen();
                } else {
                    if (g_selected_activity >= 0 && g_selected_activity < g_activity_count) {
                        request_activity_switch_with_confirm(g_selected_activity);
                    }
                }
            } else if (g_current_view == VIEW_DEVICES) {
                execute_active_dev_cmd(g_selected_device_cmd);
                render_screen();
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_CATEGORIES) {
                    execute_settings_action(g_selected_category);
                } else {
                    execute_settings_action(g_selected_sub_item);
                }
                render_screen();
            } else if (g_current_view == VIEW_PAIRING) {
                execute_pairing_menu_opt(g_selected_pair_opt);
            }
            break;

        case 158: /* KEY_BACK */
        case 202: /* KEY_PROG3 (Plug On -> Back / Cancel) */
            if (g_current_view == VIEW_PAIRING) {
                if (g_unpair_confirm_active) {
                    g_unpair_confirm_active = false;
                } else {
                    g_current_view = VIEW_SETTINGS;
                    g_settings_subview = SETTINGS_VIEW_SYSTEM;
                    g_selected_sub_item = 3;
                }
            } else if (g_current_view == VIEW_SETTINGS) {
                if (g_settings_subview == SETTINGS_VIEW_REBOOT || g_settings_subview == SETTINGS_VIEW_FACTORY_RESET) {
                    bool was_reboot = (g_settings_subview == SETTINGS_VIEW_REBOOT);
                    g_settings_subview = SETTINGS_VIEW_SYSTEM;
                    g_factory_reset_stage = 0;
                    g_selected_sub_item = was_reboot ? 4 : 5;
                } else if (g_settings_subview != SETTINGS_VIEW_CATEGORIES) {
                    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
                    g_factory_reset_stage = 0;
                } else {
                    g_current_view = VIEW_DEVICES;
                }
            } else if (g_current_view == VIEW_DEVICES) {
                g_current_view = VIEW_ACTIVITIES;
            } else if (g_current_view == VIEW_ACTIVITIES) {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            }
            render_screen();
            break;
        default:
            break;
    }
}

/* Find and open touchscreen device by probing device names */
static int find_touchscreen_fd(void) {
    const char *candidates[] = {
        "/dev/input/event2",
        "/dev/input/event3",
        "/dev/input/touchscreen0",
        "/dev/input/event1",
        "/dev/input/event0"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int fd = open(candidates[i], O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            char name[128] = {0};
            if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
                if (strstr(name, "Clearpad") || strstr(name, "clearpad") ||
                    strstr(name, "touch") || strstr(name, "Touch")) {
                    printf("[+] Discovered touchscreen at %s: %s (fd=%d)\n", candidates[i], name, fd);
                    return fd;
                }
            }
            close(fd);
        }
    }
    return open(TOUCH_EVENT_DEV, O_RDONLY | O_NONBLOCK);
}

static void crash_sig_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)ucontext;
    /* 1. Hardware fail-safe: immediately turn off backlight and button LEDs */
    int bfd = open("/sys/class/backlight/ap3156_backlight/brightness", O_WRONLY);
    if (bfd >= 0) {
        write(bfd, "0\n", 2);
        close(bfd);
    }
    int lfd = open("/sys/class/leds/tps6507x:white/brightness", O_WRONLY);
    if (lfd >= 0) {
        write(lfd, "0\n", 2);
        close(lfd);
    }

    /* 2. Log crash diagnostic info to /data/codex_crash.log */
    FILE *cf = fopen("/data/codex_crash.log", "a");
    if (cf) {
        time_t now = time(NULL);
        char tbuf[32] = {0};
        struct tm *tm = localtime(&now);
        if (tm) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
        else snprintf(tbuf, sizeof(tbuf), "epoch=%ld", (long)now);

        const char *signame = "UNKNOWN";
        if (sig == SIGSEGV) signame = "SIGSEGV (Segmentation Fault)";
        else if (sig == SIGBUS) signame = "SIGBUS (Bus Error)";
        else if (sig == SIGABRT) signame = "SIGABRT (Abort)";
        else if (sig == SIGFPE) signame = "SIGFPE (Floating Point Exception)";
        else if (sig == SIGILL) signame = "SIGILL (Illegal Instruction)";

        void *fault_addr = info ? info->si_addr : NULL;
        int code = info ? info->si_code : 0;

        fprintf(cf, "=== CRASH %s ===\n", tbuf);
        fprintf(cf, "Signal: %d (%s), Code: %d, Fault Addr: %p\n", sig, signame, code, fault_addr);
        fprintf(cf, "State: act='%s', view=%d, screen_on=%d, no_screen=%d, rf_conn=%d, slot=%d\n",
                g_active_activity_id, g_current_view, g_screen_on ? 1 : 0, g_no_screen_mode ? 1 : 0,
                g_rf_link_connected ? 1 : 0, g_remote_slot);
        fprintf(cf, "Uptime mono: %llums\n", (unsigned long long)get_monotonic_ms());

        FILE *sf = fopen("/proc/self/status", "r");
        if (sf) {
            char sline[128];
            while (fgets(sline, sizeof(sline), sf)) {
                if (strncmp(sline, "VmPeak:", 7) == 0 ||
                    strncmp(sline, "VmSize:", 7) == 0 ||
                    strncmp(sline, "VmRSS:", 6) == 0 ||
                    strncmp(sline, "Threads:", 8) == 0) {
                    fputs(sline, cf);
                }
            }
            fclose(sf);
        }
        fprintf(cf, "========================================\n\n");
        fflush(cf);
        fclose(cf);
    }

    sync();
    _exit(128 + sig);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Enforce single instance to prevent duplicate access to devices */
    int lock_fd = open("/var/run/codex_elite.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                fprintf(stderr, "[-] FATAL: Another codex_elite instance is already running! Exiting.\n");
                return 1;
            }
            fprintf(stderr, "[-] Warning: flock failed: %s (ignoring non-contention error)\n", strerror(errno));
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGUSR1, sig_wake_handler);
    signal(SIGUSR2, sig_deep_sleep_handler);

    /* Setup crash signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_sig_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    printf("[+] Starting Harmony Elite Custom Interface (codex_elite)...\n");

    /* Load persistent config from /data/codex_elite.conf if present */
    load_settings_conf();

    /* Initialize USB hardware connection state and guarantee clean network state */
    g_usb_connected = is_usb_hardware_connected();
    log_power_debug("[*] codex_elite INIT: usbeth_enabled=%d, usb_cable_connected=%d\n",
                    g_usbeth_enabled, g_usb_connected);
    if (!g_usb_connected || !g_usbeth_enabled) {
        stop_usbeth_and_dropbear();
    } else {
        start_usbeth_and_dropbear();
    }

    /* Sync system time from MSP430 hardware RTC */
    sync_time_from_rtc();

    bool do_pairing = false;
    /* Parse flags (CLI overrides) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-touch") == 0) {
            g_touch_enabled = false;
            g_screen_nav_mode = true;
        }
        if (strcmp(argv[i], "--no-gyro") == 0)  g_gyro_enabled = false;
        if (strcmp(argv[i], "--no-cap") == 0)   g_cap_buttons_enabled = false;
        if (strcmp(argv[i], "--blank") == 0)    g_screen_on = false;
        if (strcmp(argv[i], "--pair") == 0)     do_pairing = true;
        if (strcmp(argv[i], "--no-screen") == 0) g_no_screen_mode = true;
    }

    if (g_no_screen_mode) {
        printf("[+] NO-SCREEN MODE ACTIVE: Headless physical button remote, screen & touch disabled\n");
        g_screen_on = false;
        g_touch_enabled = false;
        g_gyro_enabled = false;
        int bfd = open("/sys/class/backlight/ap3156_backlight/brightness", O_WRONLY);
        if (bfd >= 0) { write(bfd, "0\n", 2); close(bfd); }
    } else if (!g_touch_enabled) {
        g_screen_nav_mode = true;
        show_notice("Screen nav moved to bottom buttons", 4000);
    }

    init_act_triggers();
    load_rf_config();
    if (!g_has_hub_config) {
        load_activities();
    }
    printf("[+] Loaded %d activities (has_hub_config=%d)\n", g_activity_count, g_has_hub_config);

    if (!g_no_screen_mode) {
        /* 1. Open Framebuffer */
        g_fb_fd = open(FB_DEV, O_RDWR);
        if (g_fb_fd >= 0) {
            struct fb_fix_screeninfo finfo;
            struct fb_var_screeninfo vinfo;
            if (ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0 &&
                ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
                g_fb_size = finfo.smem_len;
                g_fb_mem = (uint16_t *)mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
                if (g_fb_mem == MAP_FAILED) g_fb_mem = NULL;

                /* Switch active display to buffer 0 */
                vinfo.xoffset = 0;
                vinfo.yoffset = 0;
                ioctl(g_fb_fd, FBIOPAN_DISPLAY, &vinfo);
            } else {
                g_fb_mem = (uint16_t *)mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
                if (g_fb_mem == MAP_FAILED) g_fb_mem = NULL;
            }
        }

        fb_set_blank(!g_screen_on);
        reset_idle_timer();
        load_wallpaper_asset();

        /* 2. Initialize and Open Touch and Gyro nodes */
        clearpad_hw_init();
        g_touch_fd = find_touchscreen_fd();
        if (g_touch_fd < 0) {
            perror("[-] Failed to open touchscreen node");
        } else {
            printf("[+] Opened touchscreen fd=%d\n", g_touch_fd);
        }
        if (g_touch_fd >= 0 && !g_touch_enabled) {
            set_touch_enabled(false);
        }

        g_gyro_fd = open(GYRO_EVENT_DEV, O_RDONLY | O_NONBLOCK);
        if (g_gyro_fd >= 0) {
            init_gyro_interrupt();
            if (!g_gyro_enabled) {
                set_gyro_enabled(false);
            }
        }
    }

    /* 3. Open RF SPI */
    g_rf_fd = open(RFSPI_DEV, O_RDWR);
    if (g_rf_fd >= 0) {
        printf("[+] Opened %s for RF TX/RX\n", RFSPI_DEV);
        /* 1. Init message engine (matches stock hal_remote) */
        uint8_t pkt_init[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};
        if (write(g_rf_fd, pkt_init, sizeof(pkt_init)) < 0) {}
        usleep(30000);

        /* 2. Enable extended frames (matches stock hal_remote libhal_rf_hid_write) */
        uint8_t pkt_ext[7] = {0x10, 0xFF, 0x80, 0xFD, 0x01, 0x00, 0x00};
        if (write(g_rf_fd, pkt_ext, sizeof(pkt_ext)) < 0) {}
        usleep(30000);

        /* 3. Query remote own address (pipe 0x03) and paired Hub address (pipe 0x20) */
        uint8_t pkt_pipe[7] = {0x10, 0xFF, 0x83, 0xB5, 0x03, 0x00, 0x00};
        if (write(g_rf_fd, pkt_pipe, sizeof(pkt_pipe)) < 0) {}
        usleep(20000);
        uint8_t pkt_hub[7] = {0x10, 0xFF, 0x83, 0xB5, 0x20, 0x00, 0x00};
        if (write(g_rf_fd, pkt_hub, sizeof(pkt_hub)) < 0) {}
        usleep(20000);

        /* 4. Set CC2544 active power mode (matches stock hal_remote hot_set_powermode 0x20) */
        uint8_t pkt_power[7] = {0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};
        if (write(g_rf_fd, pkt_power, sizeof(pkt_power)) < 0) {}
        usleep(30000);

        /* Start RF RX worker to process asynchronous link status and sync responses */
        pthread_t th_rf_rx;
        pthread_create(&th_rf_rx, NULL, rf_rx_worker, NULL);
        usleep(30000);

        if (do_pairing) {
            rf_start_pairing();
        } else {
            /* Dongle Active Sync (triggers CC2544 hopping sync to Hub beacon) */
            uint8_t pkt_dongle0[10] = {0x12, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            uint8_t pkt_dongle1[10] = {0x12, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            printf("[*] Writing Dongle Active Sync (10 bytes)...\n");
            if (write(g_rf_fd, pkt_dongle0, sizeof(pkt_dongle0)) < 0) { perror("write dongle sync 0"); }
            if (write(g_rf_fd, pkt_dongle1, sizeof(pkt_dongle1)) < 0) { perror("write dongle sync 1"); }
            fflush(stdout);

            /* Wait up to 500ms for FHSS synchronization */
            for (int w = 0; w < 10 && !g_rf_link_connected; w++) {
                usleep(50000);
            }

            /* Load cached config FIRST */
            load_rf_config();
            if (!g_has_hub_config) {
                load_activities();
                rf_send_full_sync_request();
            } else {
                rf_send_sync_query();
            }
            usleep(50000);
        }
    } else {
        load_rf_config();
        if (!g_has_hub_config) {
            load_activities();
        }
    }

    /* 4. Open Physical Buttons */
    g_btn_fd = open(BTN_EVENT_DEV, O_RDONLY | O_NONBLOCK);
    if (g_btn_fd < 0) {
        perror("open " BTN_EVENT_DEV);
        return 1;
    }
    printf("[+] Listening on %s for physical buttons...\n", BTN_EVENT_DEV);

    /* Background Threads */
    pthread_t th_power, th_repeat;
    pthread_create(&th_power, NULL, power_manager, NULL);
    pthread_create(&th_repeat, NULL, key_repeat_thread, NULL);

    /* Initial Render */
    render_screen();

    /* Event Loop with select() multiplexing physical buttons and touchscreen */
    uint64_t last_sync_query_ms = 0;
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int max_fd = -1;

        if (g_btn_fd >= 0) {
            FD_SET(g_btn_fd, &rfds);
            if (g_btn_fd > max_fd) max_fd = g_btn_fd;
        }
        if (g_touch_fd >= 0 && g_screen_on) {
            FD_SET(g_touch_fd, &rfds);
            if (g_touch_fd > max_fd) max_fd = g_touch_fd;
        }
        uint64_t standby_s = (s_last_blank_time_ms > 0) ? (get_monotonic_ms() - s_last_blank_time_ms) / 1000 : 0;
        bool is_docked = is_elite_docked();
        bool allow_gyro_wake = true;
        if (g_gyro_fd >= 0 && !g_screen_on && g_gyro_enabled && allow_gyro_wake) {
            FD_SET(g_gyro_fd, &rfds);
            if (g_gyro_fd > max_fd) max_fd = g_gyro_fd;
        }

        struct timeval tv;
        if (g_screen_on) {
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
        } else {
            tv.tv_sec = (!is_docked && standby_s >= 1800) ? 30 : 10;
            tv.tv_usec = 0;
        }
        int sel = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Periodic Keepalive: send SYNC_REQUEST every 800ms while screen is awake to hold FHSS lock */
        /* Periodic Keepalive: send SYNC_REQUEST every 800ms while screen is awake to hold FHSS lock */
        uint64_t now_ms = get_monotonic_ms();
        bool has_paired_hub = (g_paired_hub_addr[0] != 0 || g_paired_hub_addr[1] != 0 ||
                               g_paired_hub_addr[2] != 0 || g_paired_hub_addr[3] != 0);
        if (g_screen_on && g_rf_fd >= 0 && g_pairing_state != 1 && has_paired_hub && (now_ms - last_sync_query_ms >= 800)) {
            rf_send_sync_query();
            last_sync_query_ms = now_ms;
        }

        /* Periodic Reconnect Pulse: if disconnected, paired, and screen awake, re-sync FHSS every 2000ms */
        static uint64_t last_reconnect_pulse_ms = 0;
        if (!g_rf_link_connected && g_screen_on && g_rf_fd >= 0 && g_pairing_state != 1 && has_paired_hub && (now_ms - last_reconnect_pulse_ms >= 2000)) {
            rf_wake_and_reconnect();
            last_reconnect_pulse_ms = now_ms;
        }

        /* Check Long Press mode toggle key duration (triggers immediately at 500ms while holding) */
        if (g_touch_enabled && g_nav_toggle_mode == 1 && g_toggle_key_down_ms > 0 && !g_toggle_key_triggered) {
            if (now_ms - g_toggle_key_down_ms >= 500) {
                g_screen_nav_mode = !g_screen_nav_mode;
                g_toggle_key_triggered = true;
                printf("[*] Mode toggled by LONG press hold (key %d): %s\n", g_nav_toggle_key, g_screen_nav_mode ? "NAV" : "DEV");
                g_screen_dirty = true;
            }
        }

        /* Activity switch timeout & animated progress update */
        static uint64_t last_progress_render_ms = 0;
        if (g_is_switching_activity) {
            if (now_ms - g_switching_start_ms > (uint64_t)g_switching_total_ms + 400) {
                printf("[*] Activity switch complete (elapsed %llums), unlocking screen\n", (unsigned long long)(now_ms - g_switching_start_ms));
                g_is_switching_activity = false;
                g_last_switch_completed_ms = now_ms;
                strncpy(g_last_switch_target_id, g_switching_target_id, sizeof(g_last_switch_target_id) - 1);
                /* Requirement 8: Auto switch to devices screen on Elite when done changing activity */
                if (strcmp(g_switching_target_id, "-1") != 0) {
                    g_current_view = VIEW_DEVICES;
                } else {
                    g_current_view = VIEW_ACTIVITIES;
                }
                g_screen_dirty = true;
            } else if (now_ms - last_progress_render_ms >= 100) {
                g_screen_dirty = true;
                last_progress_render_ms = now_ms;
            }
        }

        /* Dynamic USB Connection Monitor: instantly start/drop Dropbear and Ethernet */
        static uint64_t last_usb_check_ms = 0;
        uint32_t usb_check_interval = g_usb_connected ? 1000 : 3000;
        if ((g_usbeth_enabled || (g_screen_on && g_current_view == VIEW_SETTINGS)) &&
            (now_ms - last_usb_check_ms >= usb_check_interval)) {
            last_usb_check_ms = now_ms;
            bool usb_now = is_usb_hardware_connected();
            if (usb_now != g_usb_connected) {
                g_usb_connected = usb_now;
                if (g_usb_connected) {
                    if (g_usbeth_enabled) {
                        log_power_debug("[+] USB cable plugged: starting USB Ethernet and Dropbear...\n");
                        start_usbeth_and_dropbear();
                    } else {
                        log_power_debug("[+] USB cable plugged (Ethernet mode is disabled in settings).\n");
                    }
                } else {
                    log_power_debug("[-] USB cable disconnected: stopping Dropbear and USB Ethernet...\n");
                    stop_usbeth_and_dropbear();
                }
                g_screen_dirty = true;
            }
        }

        /* Battery Reporting: send once every 1 hour (3600000ms), or immediately if charging state changes */
        static uint64_t last_battery_report_ms = 0;
        static uint8_t last_charging_state = 0xFF;
        if (g_screen_on && g_rf_fd >= 0 && g_rf_link_connected && !g_is_ota_updating) {
            read_elite_battery();
            if (now_ms - last_battery_report_ms >= 3600000 || g_battery_info.charging != last_charging_state) {
                last_charging_state = g_battery_info.charging;
                last_battery_report_ms = now_ms;
                send_battery_status_rf();
            }
        }

        /* Version Reporting: send once every 24 hours (86400000ms), or immediately on first link */
        static uint64_t last_version_report_ms = 0;
        if (g_screen_on && g_rf_fd >= 0 && g_rf_link_connected && !g_is_ota_updating) {
            if (last_version_report_ms == 0 || (now_ms - last_version_report_ms >= 86400000ULL)) {
                last_version_report_ms = now_ms;
                send_version_report_rf();
            }
        }

        /* Expire confirm activity pending state */
        if (g_confirm_pending_idx >= 0 && now_ms >= g_confirm_pending_deadline_ms) {
            g_confirm_pending_idx = -1;
            g_screen_dirty = true;
        }
        if (g_confirm_pending_rf_code != 0 && now_ms >= g_confirm_pending_rf_deadline_ms) {
            g_confirm_pending_rf_code = 0;
            g_confirm_pending_rf_deadline_ms = 0;
            g_screen_dirty = true;
        }

        /* Minute Clock Tick Check */
        static int last_rendered_minute = -1;
        time_t cur_time = time(NULL);
        struct tm *tm_cur = localtime(&cur_time);
        int cur_min = tm_cur ? tm_cur->tm_min : 0;
        if (cur_min != last_rendered_minute && g_screen_on) {
            last_rendered_minute = cur_min;
            g_screen_dirty = true;
        }

        /* Config sync timeout watchdog (45s) */
        if (g_is_syncing_config && (now_ms - g_sync_last_packet_ms >= 45000)) {
            printf("[!] Config sync timed out (45s without packets). Unlocking UI.\n");
            g_is_syncing_config = false;
            g_screen_dirty = true;
        }

        /* OTA update timeout watchdog (20s) */
        if (g_is_ota_updating && (now_ms - g_ota_last_packet_ms >= 20000)) {
            printf("[!] OTA update timed out (20s without packets). Unlocking UI.\n");
            g_is_ota_updating = false;
            if (g_ota_file) {
                fclose(g_ota_file);
                g_ota_file = NULL;
            }
            g_screen_dirty = true;
        }

        /* Check notice expiry to clear screen dirty */
        static bool s_notice_active = false;
        if (g_notice_until_ms > 0) {
            if (now_ms < g_notice_until_ms) {
                s_notice_active = true;
            } else if (s_notice_active) {
                s_notice_active = false;
                g_notice_until_ms = 0;
                g_screen_dirty = true;
            }
        }

        /* Render Screen only when dirty to prevent flicker and waste */
        if (g_screen_dirty && g_screen_on) {
            render_screen();
        }

        /* Check for external pairing trigger */
        if (access("/tmp/trigger_rf_pair", F_OK) == 0) {
            unlink("/tmp/trigger_rf_pair");
            printf("[*] External trigger: starting RF pairing...\n");
            rf_start_pairing();
            g_screen_dirty = true;
        }
        if (access("/tmp/trigger_rf_unpair", F_OK) == 0) {
            unlink("/tmp/trigger_rf_unpair");
            printf("[*] External trigger: unpairing Remote & wiping bonding...\n");
            rf_unpair();
            g_screen_dirty = true;
        }

        /* 1. Physical Buttons (event0) */
        if (g_btn_fd >= 0 && FD_ISSET(g_btn_fd, &rfds)) {
            struct input_event ev;
            while (read(g_btn_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY) {
                    reset_idle_timer();

                    /* If activity switch, config sync, or OTA is in progress, lock physical inputs */
                    if (g_is_switching_activity || g_is_syncing_config || g_is_ota_updating) continue;

                    /* No-screen mode: ignore capacitive screen keys and forward physical buttons directly */
                    if (g_no_screen_mode) {
                        if (ev.code == 102 || ev.code == 364) continue;
                        uint16_t rfk = linux_to_harmony_rf_key(ev.code);
                        if (rfk != 0) {
                            rf_send_button_event(rfk, ev.value);
                        }
                        continue;
                    }

                    /* Capacitive buttons: 364=Activities, 102=Devices (never forward over RF) */
                    if (ev.code == 102 || ev.code == 364) {
                        if (!g_cap_buttons_enabled) continue;
                        printf("[*] Capacitive key pressed (event0): code=%d val=%d\n", ev.code, ev.value);
                        reset_idle_timer();
                        handle_navigation_key(ev.code, ev.value);
                        continue;
                    }

                    /* Mode Toggle: Configured physical button switches between NAV and DEV (only if touch enabled) */
                    if (g_touch_enabled && ev.code == g_nav_toggle_key) {
                        if (g_nav_toggle_mode == 0) {
                            /* Short Press Mode: Immediate toggle on key down */
                            if (ev.value == 1) {
                                g_screen_nav_mode = !g_screen_nav_mode;
                                printf("[*] Mode toggled by short press (key %d): %s\n", g_nav_toggle_key, g_screen_nav_mode ? "NAV" : "DEV");
                                g_screen_dirty = true;
                            }
                            continue;
                        } else {
                            /* Long Press Mode (Hold >= 500ms): If held, toggle mode; if released quickly, forward RF button press! */
                            if (ev.value == 1) {
                                g_toggle_key_down_ms = get_monotonic_ms();
                                g_toggle_key_triggered = false;
                            } else if (ev.value == 2) {
                                if (!g_toggle_key_triggered && g_toggle_key_down_ms > 0 &&
                                    (get_monotonic_ms() - g_toggle_key_down_ms >= 500)) {
                                    g_screen_nav_mode = !g_screen_nav_mode;
                                    g_toggle_key_triggered = true;
                                    printf("[*] Mode toggled by LONG press (key %d): %s\n", g_nav_toggle_key, g_screen_nav_mode ? "NAV" : "DEV");
                                    g_screen_dirty = true;
                                }
                            } else if (ev.value == 0) {
                                uint64_t dur = get_monotonic_ms() - g_toggle_key_down_ms;
                                if (!g_toggle_key_triggered) {
                                    if (dur >= 500) {
                                        g_screen_nav_mode = !g_screen_nav_mode;
                                        printf("[*] Mode toggled by LONG press release (key %d): %s\n", g_nav_toggle_key, g_screen_nav_mode ? "NAV" : "DEV");
                                        g_screen_dirty = true;
                                    } else {
                                        /* Short press released (<500ms): Forward normal RF button press to Hub! */
                                        uint16_t rfk = linux_to_harmony_rf_key(g_nav_toggle_key);
                                        if (rfk != 0) {
                                            rf_send_button_event(rfk, 1);
                                            usleep(40000);
                                            rf_send_button_event(rfk, 0);
                                        }
                                    }
                                }
                                g_toggle_key_down_ms = 0;
                                g_toggle_key_triggered = false;
                            }
                            continue;
                        }
                    }

                    /* Bottom Navigation Buttons: 78=RockerUp, 74=RockerDown, 149=LightOff, 203=PlugOff, 148=LightOn, 202=PlugOn */
                    bool is_bottom_nav_key = (ev.code == 78 || ev.code == 74 || ev.code == 149 ||
                                              ev.code == 203 || ev.code == 148 || ev.code == 202);
                    bool is_confirm_pending = (g_confirm_pending_idx >= 0 || g_confirm_pending_rf_code != 0 || g_unpair_confirm_active);

                    if ((is_bottom_nav_key && (!g_touch_enabled || g_screen_nav_mode || g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING)) ||
                        (is_confirm_pending && (is_bottom_nav_key || ev.code == 158))) {
                        /* Screen Navigation or Confirm cancellation: bottom buttons and Back control screen */
                        handle_navigation_key(ev.code, ev.value);
                        continue;
                    }

                    /* All other buttons (and bottom buttons when in DEV mode, and D-PAD always) send RF to Hub */
                    uint16_t rf_code = linux_to_harmony_rf_key(ev.code);
                    if (rf_code != 0) {
                        /* Check if button is mapped to an activity trigger (or PowerOff) */
                        activity_trigger_btn_t *matched_trig = NULL;
                        for (int t = 0; t < g_act_trigger_count; t++) {
                            if ((g_act_triggers[t].rf_key_code != 0 && g_act_triggers[t].rf_key_code == rf_code) ||
                                strcasecmp(g_act_triggers[t].button_name, rf_short_button_name(rf_code)) == 0) {
                                matched_trig = &g_act_triggers[t];
                                break;
                            }
                        }

                        if (matched_trig && g_remote_mode == 0) {
                            if (ev.value == 1) {
                                const char *target_id = matched_trig->target_act_id;
                                if (matched_trig->atype == 0x03) target_id = "-1";

                                /* Item 4: Check if already active */
                                if (strcmp(g_active_activity_id, target_id) == 0) {
                                    if (strcmp(target_id, "-1") == 0) {
                                        show_notice("Already Powered Off", 2000);
                                    } else {
                                        show_notice("Activity already active", 2000);
                                    }
                                    g_screen_dirty = true;
                                    continue;
                                }

                                /* Item 3: Activity Confirmation check */
                                uint64_t now = get_monotonic_ms();
                                if (g_confirm_activity) {
                                    if (g_confirm_pending_rf_code == rf_code && now < g_confirm_pending_rf_deadline_ms) {
                                        g_confirm_pending_rf_code = 0;
                                        g_confirm_pending_rf_deadline_ms = 0;
                                    } else {
                                        g_confirm_pending_rf_code = rf_code;
                                        g_confirm_pending_rf_deadline_ms = now + 4000;
                                        show_notice("Press again or OK to confirm", 3000);
                                        g_screen_dirty = true;
                                        continue;
                                    }
                                }

                                /* Trigger switch immediately */
                                const char *target_name = "PowerOff";
                                if (strcmp(target_id, "-1") != 0) {
                                    for (int a = 0; a < g_activity_count; a++) {
                                        if (strcmp(g_activities[a].id, target_id) == 0) {
                                            target_name = g_activities[a].name;
                                            break;
                                        }
                                    }
                                }
                                trigger_activity_switch(target_id, target_name);
                            }
                            continue;
                        }

                        if (ev.value == 1) {
                            rf_send_button_event(rf_code, 1);
                            pthread_mutex_lock(&g_repeat_mutex);
                            g_held_rf_code = rf_code;
                            pthread_cond_signal(&g_repeat_cond);
                            pthread_mutex_unlock(&g_repeat_mutex);
                        } else if (ev.value == 0) {
                            pthread_mutex_lock(&g_repeat_mutex);
                            if (g_held_rf_code == rf_code) g_held_rf_code = 0;
                            pthread_mutex_unlock(&g_repeat_mutex);
                            rf_send_button_event(rf_code, 0);
                        }
                    }
                }
            }
        }

        /* 2. Touchscreen & Capacitive Buttons (event2) */
        if (g_touch_fd >= 0 && FD_ISSET(g_touch_fd, &rfds)) {
            struct input_event ev;
            while (read(g_touch_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY) {
                    /* Capacitive buttons below screen: 364=Favorites/Activities, 102=Home/Devices */
                    if (ev.code == 102 || ev.code == 364) {
                        if (!g_cap_buttons_enabled) continue;
                        printf("[*] Capacitive key pressed (event2): code=%d val=%d\n", ev.code, ev.value);
                        reset_idle_timer();
                        handle_navigation_key(ev.code, ev.value);
                        continue;
                    }
                    if (!g_touch_enabled) continue;
                    if (ev.code == 330) { /* BTN_TOUCH */
                        printf("[*] Touch BTN_TOUCH: val=%d at (%d, %d)\n", ev.value, g_touch_cur_x, g_touch_cur_y);
                        if (ev.value == 0) {
                            if (g_touch_is_down) {
                                g_touch_is_down = false;
                                g_touch_need_down_latch = false;
                                handle_screen_touch_tap(g_touch_cur_x, g_touch_cur_y);
                            }
                        } else {
                            reset_idle_timer();
                            g_touch_is_down = true;
                            g_touch_need_down_latch = true;
                        }
                    }
                } else if (ev.type == EV_ABS) {
                    if (!g_touch_enabled) continue;
                    if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                        g_touch_cur_x = ev.value;
                    } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                        g_touch_cur_y = ev.value;
                    } else if (ev.code == ABS_PRESSURE) {
                        if (ev.value > 0) {
                            reset_idle_timer();
                            if (!g_touch_is_down) {
                                g_touch_is_down = true;
                                g_touch_need_down_latch = true;
                            }
                        } else {
                            if (g_touch_is_down) {
                                g_touch_is_down = false;
                                g_touch_need_down_latch = false;
                                handle_screen_touch_tap(g_touch_cur_x, g_touch_cur_y);
                            }
                        }
                    }
                } else if (ev.type == EV_SYN) {
                    if (g_touch_need_down_latch && g_touch_is_down) {
                        g_touch_down_x = g_touch_cur_x;
                        g_touch_down_y = g_touch_cur_y;
                        g_touch_down_ms = get_monotonic_ms();
                        g_touch_need_down_latch = false;
                        printf("[*] Touch DOWN latched on SYN at (%d, %d)\n", g_touch_down_x, g_touch_down_y);
                    }
                }
            }
        }

        /* 3. Accelerometer Hardware Motion Wake (event1 / INT1) */
        if (g_gyro_fd >= 0 && FD_ISSET(g_gyro_fd, &rfds)) {
            struct input_event ev[16];
            ssize_t n = read(g_gyro_fd, ev, sizeof(ev));
            bool is_docked = is_elite_docked();
            bool allow_gyro_wake = true;
            if (n > 0 && !g_screen_on && g_gyro_enabled && allow_gyro_wake) {
                log_power_debug("[*] Accelerometer INT1 motion interrupt, waking display\n");
                reset_idle_timer();
                fb_set_blank(false);
                render_screen();
            }
        }
    }

    printf("[+] Cleaning up hardware resources...\n");
    if (g_touch_fd >= 0) { ioctl(g_touch_fd, EVIOCGRAB, 0); close(g_touch_fd); }
    if (g_gyro_fd >= 0)  { ioctl(g_gyro_fd, EVIOCGRAB, 0);  close(g_gyro_fd); }
    if (g_rf_fd >= 0)    close(g_rf_fd);
    if (g_btn_fd >= 0)   close(g_btn_fd);
    if (g_fb_mem)        munmap(g_fb_mem, g_fb_size);
    if (g_fb_fd >= 0)    close(g_fb_fd);

    return 0;
}