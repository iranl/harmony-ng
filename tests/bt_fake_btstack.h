/* A fake BTstack for building hub_btstack's sources on the host (bt_state.c, bt_hid.c, bt_cmd.c,
 * bt_pairing.c). Include it first, then the sources. The real headers are skipped through their
 * guards. Timers and data sources do not run by themselves: the test fires them.
 * A test may define TEST_DIR (where the files go) and DAEMON_PORT (hub_daemon's port on
 * loopback) before including it. */
#ifndef BT_FAKE_BTSTACK_H
#define BT_FAKE_BTSTACK_H
#define HARMONY_BT_STATE_H
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include "cJSON.h"

/* ---- paths ---- */
#ifndef TEST_DIR
#define TEST_DIR "/tmp/hng_bt_host_test"
#endif
#define PID_FILE TEST_DIR "/hub_btstack.pid"
#define STATUS_FILE TEST_DIR "/hub_btstack_status.json"
#define LOG_FILE TEST_DIR "/hub-btstack.log"
#ifndef DAEMON_PORT
#define DAEMON_PORT 1 /* nothing listens: hub_daemon does not answer */
#endif
#define BT_EVENT_FILE TEST_DIR "/bt_remote_events.log"
#define MAP_FILE TEST_DIR "/bt_remote_map.json"
#define CURRENT_ACT_FILE TEST_DIR "/current_activity"
#define TARGET_FILE TEST_DIR "/bt_remote_target"
#define TLV_DB_PATH TEST_DIR "/btstack_keys.tlv"
#define DEBUG_LOG_CONFIG TEST_DIR "/debug_logging.conf"
#define BT_DEBUG_FLAG TEST_DIR "/bt_remote_debug"
#define CMD_FILE TEST_DIR "/hub_btstack_cmd"
#define CMD_SOCK TEST_DIR "/hub_btstack.sock"
#define SCAN_RESULT_FILE TEST_DIR "/hub_btstack_scan.json"
#define BTHID_FIFO TEST_DIR "/bthid_input"
#define BTHID_STATUS_FILE TEST_DIR "/bthid_status"
#define HOST_TARGET_FILE TEST_DIR "/bthid_host_target"
#define DEVICES_FILE TEST_DIR "/devices.json"
#define LOG_MAX_BYTES 65536
#define TLV_TAG_HOGD 0x484F4744u
#define TLV_TAG_HIDR 0x48494452u

/* ---- the BTstack types and constants hub_btstack uses ---- */
#define UNUSED(x) (void)(x)
typedef uint8_t bd_addr_t[6];
typedef uint8_t link_key_t[16];
typedef int link_key_type_t;
typedef uint16_t hci_con_handle_t;
#define HCI_CON_HANDLE_INVALID 0xffff
#define ERROR_CODE_SUCCESS 0
#define HCI_STATE_WORKING 2
typedef enum { BD_ADDR_TYPE_LE_PUBLIC = 0, BD_ADDR_TYPE_LE_RANDOM = 1 } bd_addr_type_t;
typedef enum { HID_PROTOCOL_MODE_BOOT = 0, HID_PROTOCOL_MODE_REPORT = 1 } hid_protocol_mode_t;
typedef struct btstack_timer_source {
    void (*process)(struct btstack_timer_source *ts);
    uint32_t timeout;
    bool armed;
} btstack_timer_source_t;
typedef enum {
    DATA_SOURCE_CALLBACK_POLL = 1 << 0,
    DATA_SOURCE_CALLBACK_READ = 1 << 1,
    DATA_SOURCE_CALLBACK_WRITE = 1 << 2,
} btstack_data_source_callback_type_t;
typedef struct btstack_data_source {
    int fd;
    void (*process)(struct btstack_data_source *ds, btstack_data_source_callback_type_t callback_type);
    uint16_t flags;
} btstack_data_source_t;
typedef struct {
    int (*get_tag)(void *context, uint32_t tag, uint8_t *buffer, uint32_t buffer_size);
    int (*store_tag)(void *context, uint32_t tag, const uint8_t *data, uint32_t data_size);
    void (*delete_tag)(void *context, uint32_t tag);
} btstack_tlv_t;
typedef struct { int dummy; } btstack_tlv_posix_t;
typedef struct { int dummy; } btstack_packet_callback_registration_t;
typedef struct { int dummy; } gatt_client_notification_t;

/* ---- bt_state.h's own types (the header is skipped) ---- */
typedef struct {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
} le_device_addr_t;
typedef enum { APP_IDLE = 0, APP_W4_WORKING, APP_SCANNING, APP_CONNECTING, APP_ENCRYPTING, APP_W4_HIDS, APP_READY } app_state_t;
#define MAX_HOST_CONNS 4
typedef struct {
    uint16_t hid_cid;
    hci_con_handle_t acl_handle;
    bd_addr_t addr;
    bool connected;
    bool manual_disconnect;
    char name[64];
} host_conn_t;
#define MAX_HOST_RECONNECT_ATTEMPTS 25

extern app_state_t app_state;
extern le_device_addr_t remote_device;
extern hci_con_handle_t connection_handle;
extern btstack_timer_source_t reconnect_timer;
extern btstack_tlv_posix_t tlv_context;
extern const btstack_tlv_t *tlv_impl;
extern host_conn_t g_hosts[MAX_HOST_CONNS];
extern int g_active_host_idx;
extern uint16_t g_host_hid_cid;
extern bd_addr_t g_host_addr;
extern bool g_host_connected;
extern bool g_host_pairing;
extern bool g_pairing_new_key_valid;
extern bd_addr_t g_pairing_new_key_addr;
extern char g_host_name[64];
extern int g_bthid_fifo_fd;
extern unsigned long g_keys_sent;
extern btstack_timer_source_t g_host_reconnect_timer;
extern bool g_host_reconnect_active;
extern int g_host_reconnect_attempts;
extern bd_addr_t g_host_reconnect_target;
extern bool g_host_connecting;
extern time_t g_host_connecting_start;
extern char g_pending_wake_btn[64];
extern time_t g_pending_wake_time;
extern char g_last_btn[64];
extern time_t g_last_seen;
extern volatile sig_atomic_t g_running;
bool is_debug_log_enabled(void);
void log_msg(const char *msg);
void hub_bt_log_debug(const char *msg);
int parse_bd_addr(const char *str, bd_addr_t addr);
int is_null_bd_addr(const bd_addr_t addr);
int bt_tlv_store_if_changed(uint32_t tag, const uint8_t *value, uint32_t len);

/* ---- what the fake stack saw ---- */
#define MAX_CALLS 256
static char s_calls[MAX_CALLS][96];
static int s_ncalls;
static void record(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void record(const char *fmt, ...) {
    if (s_ncalls >= MAX_CALLS) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_calls[s_ncalls++], sizeof(s_calls[0]), fmt, ap);
    va_end(ap);
}
__attribute__((unused)) static int count_calls(const char *prefix) {
    int n = 0;
    for (int i = 0; i < s_ncalls; i++) {
        if (strncmp(s_calls[i], prefix, strlen(prefix)) == 0) n++;
    }
    return n;
}
__attribute__((unused)) static void reset_calls(void) { s_ncalls = 0; }

/* reports per host: the last report seen */
static uint8_t s_last_report[4][16];
static int s_reports[4];

/* ---- BTstack ---- */
static uint32_t s_now_ms;
void btstack_run_loop_add_timer(btstack_timer_source_t *ts) { ts->armed = true; }
int btstack_run_loop_remove_timer(btstack_timer_source_t *ts) { ts->armed = false; return 0; }
void btstack_run_loop_set_timer(btstack_timer_source_t *ts, uint32_t ms) { ts->timeout = ms; }
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *ts, void (*h)(btstack_timer_source_t *)) { ts->process = h; }
uint32_t btstack_run_loop_get_time_ms(void) { return s_now_ms; }
static int s_exit_calls; /* counted: test_bt_hub_btstack.c checks reload_map leaves the run loop */
void btstack_run_loop_trigger_exit(void) { s_exit_calls++; }
void btstack_run_loop_set_data_source_fd(btstack_data_source_t *ds, int fd) { ds->fd = fd; }
void btstack_run_loop_set_data_source_handler(btstack_data_source_t *ds, void (*p)(btstack_data_source_t *, btstack_data_source_callback_type_t)) { ds->process = p; }
void btstack_run_loop_add_data_source(btstack_data_source_t *ds) { UNUSED(ds); }
int btstack_run_loop_remove_data_source(btstack_data_source_t *ds) { UNUSED(ds); return 1; }
void btstack_run_loop_enable_data_source_callbacks(btstack_data_source_t *ds, uint16_t types) { ds->flags |= types; }
void btstack_run_loop_disable_data_source_callbacks(btstack_data_source_t *ds, uint16_t types) { ds->flags &= (uint16_t)~types; }

static int host_of_cid(uint16_t cid) { return cid >= 0x41 && cid <= 0x44 ? cid - 0x41 : -1; }
uint8_t hid_device_send_interrupt_message(uint16_t cid, const uint8_t *msg, uint16_t len) {
    int h = host_of_cid(cid);
    record("send %04x %02x %02x", cid, len > 1 ? msg[1] : 0, len > 4 ? msg[4] : 0);
    if (h >= 0) {
        memset(s_last_report[h], 0, sizeof(s_last_report[h]));
        memcpy(s_last_report[h], msg, len < 16 ? len : 16);
        s_reports[h]++;
    }
    return 0;
}
void hid_device_request_can_send_now_event(uint16_t cid) { UNUSED(cid); }
/* A test may set this to play what BTstack does inside hid_device_disconnect(): for a connection
 * still being set up it reports a failed open at once */
static void (*s_fake_on_hid_disconnect)(uint16_t cid);
void hid_device_disconnect(uint16_t cid) {
    record("hid_device_disconnect %04x", cid);
    if (s_fake_on_hid_disconnect) s_fake_on_hid_disconnect(cid);
}
static uint8_t s_connect_status = 0;
uint8_t hid_device_connect(uint8_t *addr, uint16_t *cid) {
    record("hid_device_connect %02X", addr[5]);
    *cid = 0x50;
    return s_connect_status;
}
int hci_get_state(void) { return HCI_STATE_WORKING; }
const char *bd_addr_to_str(const uint8_t *a) {
    static char b[4][18];
    static int i;
    i = (i + 1) % 4;
    snprintf(b[i], sizeof(b[i]), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
    return b[i];
}
int gap_get_link_key_for_bd_addr(bd_addr_t a, link_key_t k, link_key_type_t *t) { UNUSED(a); UNUSED(k); UNUSED(t); return 0; }
void gap_discoverable_control(uint8_t on) { UNUSED(on); }
void gap_connectable_control(uint8_t on) { UNUSED(on); }
void gap_set_local_name(const char *name) { UNUSED(name); }
void gap_set_class_of_device(uint32_t cod) { UNUSED(cod); }
void gap_stop_scan(void) {}
void gap_start_scan(void) {}
void gap_set_scan_parameters(uint8_t t, uint16_t i, uint16_t w) { UNUSED(t); UNUSED(i); UNUSED(w); }
uint8_t gap_connect(const uint8_t *a, bd_addr_type_t t) { UNUSED(a); UNUSED(t); record("gap_connect"); return 0; }
uint8_t gap_connect_cancel(void) { record("gap_connect_cancel"); return 0; }
uint8_t gap_disconnect(hci_con_handle_t h) { record("gap_disconnect %04x", h); return 0; }
int gap_delete_bonding(bd_addr_type_t t, bd_addr_t a) { UNUSED(t); UNUSED(a); return 0; }
void gap_drop_link_key_for_bd_addr(bd_addr_t a) { record("gap_drop_link_key %02X", a[5]); }

/* ---- bt_remote.c ---- */
static char s_activity[16] = "1";
void read_current_activity(char *out, size_t outlen) { snprintf(out, outlen, "%s", s_activity); }
void bt_remote_invalidate_map(void) {}

/* ---- the key store ---- */
static int s_store_calls;
static uint8_t s_tag_val[32];
static uint32_t s_tag_len;
static int fake_get_tag(void *c, uint32_t tag, uint8_t *buf, uint32_t size) {
    UNUSED(c); UNUSED(tag);
    if (!s_tag_len) return 0;
    memcpy(buf, s_tag_val, s_tag_len < size ? s_tag_len : size);
    return (int)s_tag_len;
}
static int fake_store_tag(void *c, uint32_t tag, const uint8_t *d, uint32_t size) {
    UNUSED(c); UNUSED(tag);
    s_store_calls++;
    memcpy(s_tag_val, d, size);
    s_tag_len = size;
    return 0;
}
static void fake_delete_tag(void *c, uint32_t tag) { UNUSED(c); record("tlv_delete %08x", tag); s_tag_len = 0; }
static const btstack_tlv_t s_fake_tlv = {fake_get_tag, fake_store_tag, fake_delete_tag};

#endif
