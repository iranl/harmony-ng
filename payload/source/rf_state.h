#ifndef HARMONY_RF_STATE_H
#define HARMONY_RF_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "cJSON.h"
#include "rf_proto.h"


static inline int rf_create_thread(pthread_t *th, size_t stack_size, void *(*start_routine)(void *), void *arg) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size > 0 ? stack_size : (64 * 1024));
    int rc = pthread_create(th, &attr, start_routine, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

#define RFSPI_DEV          "/dev/rfspi"
#define RFFW_DEV           "/dev/rffw"
#define CC2544_FW          "/lib/firmware/cc2544.bin"
#define DAEMON_PORT        8089
#define WEBUI_PORT         8089
#ifndef RF_API_PORT
#define RF_API_PORT        8092 /* tests build with another port */
#endif
#define CURRENT_ACT_FILE       "/data/harmony/current_activity"

#include "remote_slots.h"
#define MAX_PAIRED_REMOTES REMOTE_SLOT_MAX

#define LOG_LEVEL_ERR   0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3

extern int g_log_level;
#define RF_LOG(level, fmt, ...) do { if (g_log_level >= (level)) { printf(fmt, ##__VA_ARGS__); fflush(stdout); } } while (0)

int is_activity_transitioning(void);

/* Hardware family of a paired remote. The EquadID the Hub's CC2544 reports per slot names the
 * family, not the product: every touchscreen remote (Ultimate, Ultimate One, Ultimate Home,
 * Elite, 950) is "Juniper" 0x4021, and its product comes from the skin id it reports
 * (RF_MSG_UNIT_ID). Smart Control and Companion ("Sprite") run the stock firmware and are told
 * apart by the EquadID alone. */
typedef enum {
    REMOTE_MODEL_UNKNOWN = 0,
    REMOTE_MODEL_JUNIPER,          /* touchscreen remote: product from the skin id */
    REMOTE_MODEL_COMPANION,
    REMOTE_MODEL_SMART_CONTROL
} remote_model_t;

/* EquadIDs seen in the stock Hub firmware (pimento_rfthread) */
#define RF_EQUAD_SMART_CONTROL  0x401E  /* Sprite */
#define RF_EQUAD_JUNIPER        0x4021  /* all touchscreen remotes */
#define RF_EQUAD_SPRITE_A       0x4026  /* Sprite (Companion) */
#define RF_EQUAD_SPRITE_B       0x4027  /* Sprite (Companion) */
#define RF_EQUAD_4024           0x4024  /* own thread in the stock Hub, product not identified */
#define RF_EQUAD_USB_DONGLE     0x880E

/* Skin ids of the Juniper products (the "skin" record of the remote's mfg partition), named as
 * the stock remote UI does (harmony.lua MODEL_NAME); any other skin is "Harmony Touch" there */
#define RF_SKIN_ULTIMATE        0x64
#define RF_SKIN_ULTIMATE_ONE    0x66
#define RF_SKIN_ULTIMATE_HOME_A 0x69
#define RF_SKIN_ULTIMATE_HOME_B 0x6C
#define RF_SKIN_ELITE           0x6F
#define RF_SKIN_950             0x70

typedef struct {
    uint8_t slot;
    uint8_t rf_addr[4];
    char name[16];
    bool active;
    bool link_lost;
    /* a harmony-ng frame came from the slot since hub_rf started: a pipe table entry alone can be
     * an old pairing (a used Hub, a replaced Companion) that no remote answers on */
    bool heard;
    uint64_t last_seen_ms;
    remote_model_t model;
    uint16_t equad_id;
} paired_remote_t;

typedef struct {
    uint8_t percent;
    uint8_t charging;
    uint16_t voltage_mv;
    uint64_t last_update_ms;   /* CLOCK_MONOTONIC ms of the report (system wide, so it survives a
                                * hub_rf restart within a boot), 0 = no report */
    int64_t reported_at;       /* Unix time of the report, 0 = not known yet (clock not set) */
    uint8_t restored;          /* read back from the RAM cache, no report since this start */
} remote_battery_t;

typedef struct {
    char sw_version[32];
    uint8_t config_version;
    uint8_t caps;             /* RF_CAP_* from the remote's VERSION_REPORT */
    uint64_t last_report_ms;
    char remote_id[32];       /* factory unit id from RF_MSG_UNIT_ID; empty until reported */
    uint8_t skin;             /* skin id from RF_MSG_UNIT_ID; 0 until reported */
} remote_version_t;

/* RF traffic with one remote since hub_rf started, for diagnosing the link */
typedef struct {
    uint32_t frames_rx;   /* protocol frames received from the remote */
    uint32_t acked_sent;  /* frames sent that wait for the remote's ACK, resends included */
    uint32_t acked_ok;    /* of those, the ones the remote ACKed */
    uint32_t retries;     /* resends of a frame the remote didn't ACK */
    uint32_t naks;        /* sends the Hub's CC2544 refused */
    /* Key frames (RF_MSG_BUTTON_PRESS) of a remote that numbers them, see rf_key_note_event() */
    uint32_t key_events;        /* key frames received that carry a counter */
    uint32_t key_events_lost;   /* key frames missing between them: the sum of the three below */
    uint32_t key_lost_press;    /* a hold or release came for a key whose press was not seen */
    uint32_t key_lost_release;  /* a key frame came while the key before it was still down */
    uint32_t key_lost_other;    /* the rest: holds, or nothing to tell them by */
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
/* Set by POST /api/rf/restart (hub_daemon, after a backup import that restored the slot table):
 * the main loop exits once no config push, update or pairing runs, and init.sh starts hub_rf
 * again, which reads the slot table, the slot state and the pipe table afresh */
extern volatile bool g_restart_requested;
bool rf_restart_due(void); /* rf_main.c */
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
/* True while any slot holds a paired remote */
bool rf_any_slot_active(void);
extern rf_ota_state_t g_rf_ota;
extern pthread_mutex_t g_ota_lock;
extern int g_debug_logging;
extern uint8_t g_tx_seq;
/* Per thread: the ACK of the last rf_send_frame_acked() of the calling thread */
extern __thread int g_ack_status;
extern __thread uint8_t g_ack_payload[25];

#ifdef __cplusplus
extern "C" {
#endif

uint64_t get_monotonic_ms(void);

/* Builds before the elite -> remote rename: <harmony_dir>/elite_slot<N>_{version,config}.json and
 * a staged elite_ui.ota.gz / elite_ui.version (and <cache_dir>/elite_ui.ota.gz) are renamed to the
 * remote_* names unless those exist. Returns the number of files moved. */
int rf_migrate_legacy_names_in(const char *harmony_dir, const char *cache_dir);
void rf_migrate_legacy_names(void); /* /data/harmony and /cache */

/* The last battery report per slot, kept in RAM across hub_rf and hub_daemon restarts
 * (<dir>/hub_rf_battery_slot<N>.json, dir /tmp; never on flash): a restart shows the last value
 * with "restored": true until the remote reports again, instead of a 0 % placeholder. A reboot
 * empties /tmp: values are unknown until the remote's first report. */
extern const char *g_rf_battery_cache_dir;
extern const char *g_rf_version_dir;
/* Wall clock used for reported_at (time() in the firmware; the tests replace it) */
extern time_t (*g_rf_time_fn)(time_t *);
void rf_battery_note_report(uint8_t slot, uint8_t percent, uint8_t charging, uint16_t voltage_mv);
void rf_battery_restore(void);
/* A slot was unpaired: its pairing, identity, version, config version, capabilities and battery
 * are those of a remote that is gone. All of them are cleared (the version file too), so a remote
 * paired into the slot later starts from nothing. The slot's settings and key map stay. */
void rf_slot_forget(uint8_t slot);
/* Gives a report made before the clock was set its reported_at; returns 1 when it changed one */
int rf_battery_fix_times(void);
void mark_remote_slot_active(uint8_t slot);

/* Slots and CC2544 pipes. The radio picks the pipe of a new remote by itself; the slot is the
 * number the user picked (POST /api/rf/pair?slot=N) and the one every map, setting, file, API
 * route and log of hub_rf uses. Only rf_spi.c (frames in and out, pipe-table replies) and the
 * unpair command translate. The table is a permutation of 1..MAX_PAIRED_REMOTES, the same
 * numbers until a pairing into a chosen slot changes it, kept in g_rf_slot_map_path as the pipe of
 * slots 1 to MAX_PAIRED_REMOTES ("1 2 3 4 5 6"). Numbers outside 1..MAX_PAIRED_REMOTES pass
 * unchanged. */
extern const char *g_rf_slot_map_path;
uint8_t rf_slot_to_pipe(uint8_t slot);
uint8_t rf_pipe_to_slot(uint8_t pipe);
void rf_slot_map_load(void);
/* The table file changed under a running hub_rf (a backup import): takes it over and gives each
 * slot the per-slot state of the remote whose pipe it now is (version file, battery cache,
 * settings file; a slot left without a remote loses them). For the restart that follows: the new hub_rf reads
 * those by slot number. Returns the number of slots that changed. */
int rf_slot_map_follow_file(void);
/* The next remote that pairs goes to this slot (0: the slot of the pipe the radio picks) */
/* The slot a pairing should land in (0: none). new_window: the pairing window opens now, so
 * the pipes that hold a remote are noted; a second request in the same window only changes
 * the slot. */
void rf_slot_map_set_pair_target(uint8_t slot, bool new_window);
/* A pipe-table reply: whether the pipe holds a remote (EquadID), or an unpair emptied it */
void rf_slot_map_note_table(uint8_t pipe, bool has_remote);
/* The CC2544 reported a completed pairing: the chosen slot is kept 15 s more for the pipe-table
 * reply that decides (a frame never moves a pipe) */
void rf_slot_map_note_pair_success(void);
/* A pipe shows a remote (from_pipe_table: its EquadID reply; else a frame from it). While a
 * pairing into a chosen slot is pending, a pipe-table reply for a pipe that held no remote when
 * the window opened moves that pipe to the chosen slot; a frame only maps. Returns the slot. */
uint8_t rf_slot_map_claim_pipe(uint8_t pipe, bool from_pipe_table);
/* Moves what is known of the remote in slot from to slot to (a pipe moved to a chosen slot) */
void rf_slot_move_state(uint8_t from, uint8_t to);
/* Starts a config push in its own thread (a push already running ends this one at once) */
void rf_request_config_push(void);
/* What rf_slot_move_state() calls to start that push: set by main() (NULL: none, as in tests that
 * link the state code without the push code) */
extern void (*g_rf_push_starter)(void);
/* A push left out a paired touchscreen slot not heard since the start: the first frame of such a
 * slot starts the push again (rf_slot_saw_protocol_frame) */
extern volatile bool g_rf_push_deferred;
bool is_ota_busy(void);
/* A config push is running (config_push_worker): an OTA start waits for it to end */
extern volatile bool g_config_push_running;
/* POST json_body to 127.0.0.1:port; returns the HTTP status, -1 when nothing came back */
int http_post_local(int port, const char *endpoint, const char *json_body);

/* /data/harmony/remote_slot<N>_version.json: the remote's software version, config version,
 * capabilities and unit id, read back at start. Written only when something changed. */
int rf_state_version_json(int slot, char *buf, size_t len);
void rf_state_save_version_file(int slot);
void rf_state_load_version_file(int slot);
/* Records a delivered config push in the version file (keeps remote_id and skin) */
void rf_state_note_config_pushed(int slot, int config_ver);
/* One request to a local HTTP server; returns the status code and copies the body into resp,
   or returns -1 if there was no response */
int http_request_local(int port, const char *method, const char *endpoint, const char *json_body,
                       char *resp, size_t resp_len, int timeout_ms);
bool is_companion_slot(uint8_t slot);
/* The slot named by rf_sync_pending_details.json (0: every remote, or no details); its "diff"
 * text goes to diff when one is given */
int rf_sync_pending_slot(char *diff, size_t diff_len);
/* Family name for logs: "juniper", "companion", "smart_control", "unknown" */
const char *get_remote_model_name(remote_model_t model);

/* Product of a remote, from its family, EquadID (0 = not known) and skin id (0 = not reported).
 * model id: ultimate, ultimate_one, ultimate_home, elite, 950, touch (a Juniper skin the stock
 * table does not list), companion, smart_control or unknown; a Juniper remote without a skin id
 * is unknown. model name: the stock product name ("Harmony Ultimate", ...), NULL
 * when unknown (clients have their own fallback). */
const char *rf_model_id_for(remote_model_t family, uint16_t equad_id, uint8_t skin);
const char *rf_model_name_for(remote_model_t family, uint16_t equad_id, uint8_t skin);
/* The same for a slot (1-based) from g_paired_remotes and g_remote_versions */
const char *rf_slot_model_id(uint8_t slot);
const char *rf_slot_model_name(uint8_t slot);
/* Records a family identified from what a slot sends; never replaces an identified one */
void rf_note_slot_family(uint8_t slot, remote_model_t family);
/* A harmony-ng protocol frame from the slot (only touchscreen remotes speak it): names the
 * family Juniper while neither the EquadID nor earlier traffic named one */
void rf_slot_saw_protocol_frame(uint8_t slot);
/* A key frame from the slot with its key event counter (rf_proto.h RF_KEY_COUNTER_OFF), key code
 * and state (1 press, 2 hold, 0 release); name is for the log. Counts the frame and the numbers
 * missing before it in g_remote_link, and logs one line per gap. Counter 0 (a remote without the
 * counter) is not measured. It only measures: the caller dispatches the key as before. */
void rf_key_note_event(uint8_t slot, uint8_t counter, uint16_t key_code, uint8_t state, const char *name);
/* The remote in the slot may have restarted (version report, unit id, link up): if its next key
 * frame fits a counter that began at 1 again better than a gap, it is taken as that */
void rf_key_note_restart_hint(uint8_t slot);
/* RF_MSG_UNIT_ID payload (rf_proto.h): stores the unit id and the skin id when they changed.
 * Returns 1 when something changed (the caller saves the version file), else 0. */
int rf_slot_apply_unit_id(uint8_t slot, const uint8_t *payload);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RF_STATE_H */
