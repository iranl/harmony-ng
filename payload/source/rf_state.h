#ifndef HARMONY_RF_STATE_H
#define HARMONY_RF_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "cJSON.h"
#include "rf_proto.h"

#define RFSPI_DEV          "/dev/rfspi"
#define RFFW_DEV           "/dev/rffw"
#define CC2544_FW          "/lib/firmware/cc2544.bin"
#define DAEMON_PORT        8089
#define WEBUI_PORT         8089
#define RF_API_PORT        8092
#define CURRENT_ACT_FILE       "/data/harmony/current_activity"

#define MAX_PAIRED_REMOTES 2

#define LOG_LEVEL_ERR   0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3

extern int g_log_level;
#define RF_LOG(level, fmt, ...) do { if (g_log_level >= (level)) { printf(fmt, ##__VA_ARGS__); fflush(stdout); } } while (0)

typedef enum {
    REMOTE_MODEL_UNKNOWN = 0,
    REMOTE_MODEL_ELITE,
    REMOTE_MODEL_COMPANION,
    REMOTE_MODEL_SMART_CONTROL
} remote_model_t;

typedef struct {
    uint8_t slot;
    uint8_t rf_addr[4];
    char name[16];
    bool active;
    bool link_lost;
    uint64_t last_seen_ms;
    remote_model_t model;
    uint16_t equad_id;
} paired_remote_t;

typedef struct {
    uint8_t percent;
    uint8_t charging;
    uint16_t voltage_mv;
    uint64_t last_update_ms;
} remote_battery_t;

typedef struct {
    char sw_version[32];
    uint8_t config_version;
    uint8_t caps;             /* RF_CAP_* from the remote's VERSION_REPORT */
    uint64_t last_report_ms;
    char remote_id[32];       /* factory unit id from RF_MSG_UNIT_ID; empty until reported */
} remote_version_t;

/* RF traffic with one remote since hub_rf started, for diagnosing the link */
typedef struct {
    uint32_t frames_rx;   /* protocol frames received from the remote */
    uint32_t acked_sent;  /* frames sent that wait for the remote's ACK, resends included */
    uint32_t acked_ok;    /* of those, the ones the remote ACKed */
    uint32_t retries;     /* resends of a frame the remote didn't ACK */
    uint32_t naks;        /* sends the Hub's CC2544 refused */
} remote_link_stats_t;

typedef struct {
    volatile bool in_progress;
    int target_slot;
    uint32_t total_bytes;
    uint32_t sent_bytes;
    uint16_t total_chunks;
    uint16_t sent_chunks;
    int status_code; /* 0=idle, 1=running, 2=complete, -1=error */
    char error_msg[64];
} rf_ota_state_t;

/* Globals */
extern volatile bool g_running;
extern int g_rf_fd;
extern pthread_mutex_t g_rf_mutex;
extern uint8_t g_paired_addr[4];
extern uint8_t g_hub_rf_addr[4];
extern uint8_t g_remote_slot;
extern bool g_has_paired_device;
extern uint8_t g_fw_version;
extern volatile bool g_pairing_mode;
extern uint8_t g_config_version;
extern remote_battery_t g_remote_batteries[MAX_PAIRED_REMOTES];
extern remote_version_t g_remote_versions[MAX_PAIRED_REMOTES];
extern remote_link_stats_t g_remote_link[MAX_PAIRED_REMOTES];
extern paired_remote_t g_paired_remotes[MAX_PAIRED_REMOTES];
extern rf_ota_state_t g_rf_ota;
extern pthread_mutex_t g_ota_lock;
extern int g_debug_logging;
extern uint8_t g_tx_seq;
extern int g_ack_status;
extern uint8_t g_ack_payload[25];

#ifdef __cplusplus
extern "C" {
#endif

uint64_t get_monotonic_ms(void);

/* Builds before the elite -> remote rename: <harmony_dir>/elite_slot<N>_{version,config}.json and
 * a staged elite_ui.ota.gz / elite_ui.version (and <cache_dir>/elite_ui.ota.gz) are renamed to the
 * remote_* names unless those exist. Returns the number of files moved. */
int rf_migrate_legacy_names_in(const char *harmony_dir, const char *cache_dir);
void rf_migrate_legacy_names(void); /* /data/harmony and /cache */
void mark_remote_slot_active(uint8_t slot);
bool is_ota_busy(void);
void http_post_local(int port, const char *endpoint, const char *json_body);

/* /data/harmony/remote_slot<N>_version.json: the remote's software version, config version,
 * capabilities and unit id, read back at start. Written only when something changed. */
int rf_state_version_json(int slot, char *buf, size_t len);
void rf_state_save_version_file(int slot);
void rf_state_load_version_file(int slot);
/* One request to a local HTTP server; returns the status code and copies the body into resp,
   or returns -1 if there was no response */
int http_request_local(int port, const char *method, const char *endpoint, const char *json_body,
                       char *resp, size_t resp_len, int timeout_ms);
bool is_companion_slot(uint8_t slot);
const char *get_remote_model_name(remote_model_t model);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RF_STATE_H */
