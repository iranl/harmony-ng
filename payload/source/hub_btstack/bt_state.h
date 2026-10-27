#ifndef HARMONY_BT_STATE_H
#define HARMONY_BT_STATE_H

#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "btstack_config.h"
#include "btstack.h"
#include "btstack_event.h"
#include "btstack_run_loop_posix.h"
#include "btstack_tlv.h"
#include "btstack_tlv_posix.h"
#include "ble/le_device_db_tlv.h"
#include "ble/sm.h"
#include "ble/gatt-service/hids_host.h"
#include "ad_parser.h"
#include "hci_dump.h"
#include "hci_dump_posix_stdout.h"
#include "hci_transport.h"
#include "hci_transport_linux.h"
#include "cJSON.h"

#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "classic/device_id_server.h"
#include "classic/hid_device.h"
#include "classic/btstack_link_key_db.h"
#include "classic/btstack_link_key_db_tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PID_FILE            "/tmp/hub_btstack.pid"
#define STATUS_FILE         "/tmp/hub_btstack_status.json"
#define LOG_FILE            "/tmp/hub-btstack.log"
#define BT_EVENT_FILE       "/tmp/bt_remote_events.log"
#define MAP_FILE            "/data/harmony/bt_remote_map.json"
#define CURRENT_ACT_FILE    "/data/harmony/current_activity"
#define TARGET_FILE         "/data/harmony/bt_remote_target"
#define TLV_DB_PATH         "/data/harmony/btstack_keys.tlv"
#define DEBUG_LOG_CONFIG    "/data/harmony/debug_logging.conf"
#define BT_DEBUG_FLAG       "/data/harmony/bt_remote_debug"

#define CMD_FILE            "/tmp/hub_btstack_cmd"
#define CMD_SOCK            "/tmp/hub_btstack.sock"
#define SCAN_RESULT_FILE    "/tmp/hub_btstack_scan.json"
#define BTHID_FIFO          "/tmp/bthid_input"
#define BTHID_STATUS_FILE   "/tmp/bthid_status"
#define LOG_MAX_BYTES       65536

#define TLV_TAG_HOGD ((((uint32_t)'H') << 24) | (((uint32_t)'O') << 16) | (((uint32_t)'G') << 8) | 'D')
#define TLV_TAG_HIDR ((((uint32_t)'H') << 24) | (((uint32_t)'I') << 16) | (((uint32_t)'D') << 8) | 'R')

typedef struct {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
} le_device_addr_t;

typedef enum {
    APP_IDLE = 0,
    APP_W4_WORKING,
    APP_SCANNING,
    APP_CONNECTING,
    APP_ENCRYPTING,
    APP_W4_HIDS,
    APP_READY
} app_state_t;

#define MAX_HOST_CONNS 4
typedef struct {
    uint16_t hid_cid;
    hci_con_handle_t acl_handle;
    bd_addr_t addr;
    bool connected;
    char name[64];
} host_conn_t;

#define MAX_HOST_RECONNECT_ATTEMPTS 15

/* Shared Globals */
extern app_state_t app_state;
extern le_device_addr_t remote_device;
extern hci_con_handle_t connection_handle;
extern uint16_t hids_cid;
extern hid_protocol_mode_t protocol_mode;
extern uint8_t hid_descriptor_storage[1000];

extern btstack_timer_source_t reconnect_timer;
extern btstack_tlv_posix_t tlv_context;
extern const btstack_tlv_t * tlv_impl;

extern btstack_packet_callback_registration_t hci_event_callback_registration;
extern btstack_packet_callback_registration_t sm_event_callback_registration;
extern gatt_client_notification_t gatt_notification_listener;

extern host_conn_t g_hosts[MAX_HOST_CONNS];
extern int g_active_host_idx;
extern uint16_t g_host_hid_cid;
extern bd_addr_t g_host_addr;
extern bool g_host_connected;
extern bool g_host_pairing;
extern char g_host_name[64];
extern uint8_t hid_service_buffer[1024];
extern uint8_t device_id_sdp_service_buffer[512];
extern int g_bthid_fifo_fd;
extern unsigned long g_keys_sent;

extern btstack_timer_source_t g_host_reconnect_timer;
extern bool g_host_reconnect_active;
extern int g_host_reconnect_attempts;
extern bd_addr_t g_host_reconnect_target;
extern bool g_host_connecting;
extern time_t g_host_connecting_start;
extern char g_pending_wake_btn[32];
extern time_t g_pending_wake_time;

extern const uint8_t hid_descriptor_composite[];
extern const size_t hid_descriptor_composite_len;

extern char g_last_btn[64];
extern time_t g_last_seen;
extern volatile sig_atomic_t g_running;

/* Common Functions */
bool is_debug_log_enabled(void);
void log_msg(const char *msg);
void hub_bt_log_debug(const char *msg);
int parse_bd_addr(const char *str, bd_addr_t addr);
int is_null_bd_addr(const bd_addr_t addr);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_STATE_H */
