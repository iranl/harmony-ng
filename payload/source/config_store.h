#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define MAX_IR_DEVICES 32
#define MAX_IR_COMMANDS 512
#define MAX_ACTIVITIES 24
#define MAX_ACTIVITY_STEPS 64
#define MAX_BT_SCRIPT_LEN 2048

#define DEVICE_LIST_FILE "/data/resources/DeviceList.json"
#define ACTIVITY_LIST_FILE "/data/resources/ActivityList.json"
#define BT_TEXT_FIFO "/tmp/bthid_input"

#define HARMONY_DIR "/data/harmony"
#define HARMONY_CONFIG_DIR "/data/harmony/config"
#define HARMONY_DEVICES_FILE "/data/harmony/config/devices.json"
#define HARMONY_ACTIVITIES_FILE "/data/harmony/config/activities.json"
#define HARMONY_REMOTES_FILE "/data/harmony/config/remotes.json"
#define HARMONY_SETTINGS_FILE "/data/harmony/config/settings.json"
#define HARMONY_STOCK_IMPORT_FLAG "/data/harmony/config/.imported"
#define HARMONY_MQTT_FILE "/data/harmony/mqtt_config.json"
#define HARMONY_MQTT_FILE_ALT "/data/harmony/mqtt.json"

#define STOCK_IMPORT_FLAG HARMONY_STOCK_IMPORT_FLAG

struct activity_step {
    char type[32];
    char device_id[32];
    char command[80];
    int delay_ms;
    int order;
};

struct ir_command {
    char id[32];
    char name[128];
    char keycode[256];
    char raw[2048];
    int protocol_id;
    int learned;
    int has_raw;
};

struct ir_device {
    char id[32];
    char name[128];
    char manufacturer[128];
    char model[128];
    char type[80];
    int control_port;
    int transport;
    int command_count;
    int command_cap;
    struct ir_command *commands;
    int power_on_delay;
    int is_power_always_on;
    int power_on_count;
    struct activity_step *power_on_steps;
    int power_off_count;
    struct activity_step *power_off_steps;
    int mqtt_enabled;
    char mqtt_topic[256];
    int mqtt_pulse_ms;
    int inter_device_delay;
    int inter_key_delay;
    int hold_inter_key_delay;
    int default_inter_device_delay;
    int bt_auto_reconnect;          /* 0 = disabled, 1 = enabled (default: 1) */
    int bt_reconnect_duration_sec;  /* duration of reconnect in seconds (default: 30) */
    int bt_reconnect_delay_sec;     /* delay before reconnect in seconds (default: 2 for trigger, 0 for button) */
    char bt_trigger_device_id[32];      /* device_id whose command triggers bt reconnect */
    char bt_trigger_command_name[64];   /* command name that triggers bt reconnect */
};

struct ir_inventory {
    int device_count;
    long max_device_id;
    long max_command_id;
    struct ir_device devices[MAX_IR_DEVICES];
};

struct activity_item {
    char id[32];
    char name[128];
    char type[64];
    int order;
    char device_ids[16][32];
    int device_count;
    struct activity_step start_steps[MAX_ACTIVITY_STEPS];
    int start_count;
    struct activity_step stop_steps[MAX_ACTIVITY_STEPS];
    int stop_count;
};

struct activity_inventory {
    struct activity_item items[MAX_ACTIVITIES];
    int count;
    long max_id;
    char current_id[32];
    char current_name[128];
};

struct bt_saved_command {
    char name[128];
    char script[MAX_BT_SCRIPT_LEN];
    int delay_ms;
};

struct bt_saved_device {
    char id[40];
    char name[128];
    char type[40];
    char bdaddr[32];
    int command_count;
    struct bt_saved_command commands[32];
};

struct bt_inventory {
    int device_count;
    struct bt_saved_device devices[12];
};

struct hub_settings {
    int dry_run;                    /* 1 = simulate IR/BT/MQTT without sending (default: 0) */
    char hub_name[64];              /* Custom hub name (default: "Harmony Hub") */
    char ntp_server[128];           /* Custom NTP server hostname/IP (default: empty = use pools) */
    char timezone[64];              /* Timezone string e.g. "CET-1CEST,M3.5.0,M10.5.0/3" (default: empty) */
    char led_mode[32];              /* "always_on", "always_off", "flashing_slow", "flashing_fast" (default: "always_on") */
};

#ifdef __cplusplus
extern "C" {
#endif

char *read_file_text(const char *path, size_t max_bytes);
int write_file_atomic(const char *path, const char *content);
const char *config_store_path(const char *rel_path);

int load_ir_inventory(struct ir_inventory *inv);
void free_ir_inventory(struct ir_inventory *inv);

int load_activity_inventory(struct activity_inventory *inv);

int load_bt_inventory(struct bt_inventory *inv);
int save_bthid_target(const char *type, const char *bdaddr);

int run_bt_saved_script(const char *type, const char *bdaddr, const char *script,
                        int gap_ms, char *out, size_t outlen);

int config_store_init(void);
cJSON *config_store_get_devices_json(void);
cJSON *config_store_get_activities_json(void);
int config_store_save_devices(cJSON *devices_json);
int config_store_save_activities(cJSON *activities_json);
int config_store_get_settings(struct hub_settings *settings);
int config_store_save_settings(const struct hub_settings *settings);
void config_store_invalidate_cache(void);
void notify_rf_push_config(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_STORE_H */
