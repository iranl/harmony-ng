/* hub_btstack on the host: bt_state.c, bt_hid.c, bt_cmd.c and bt_pairing.c built together against a
 * fake BTstack (types and functions below; the real headers are skipped through their guards).
 * Timers do not run by themselves: the test fires them. Files go to TEST_DIR.
 *
 * Checks, each against what the old code did:
 *   - Bluetooth addresses: only "XX:XX:XX:XX:XX:XX" is an address; connect_host and
 *     disconnect_host with something else do nothing (it used to mean "every host"); a TARGET that
 *     cannot be used drops the keys behind it (they went to the previous host)
 *   - the kept wake key: names its host, a stale one is replaced, it is dropped when the Hub is off
 *     or the reconnect gives up, and only the host it was kept for takes it
 *   - keys and buttons put down with no release behind them (raw reports, a modifier-only report,
 *     a mouse button) are released after the hold time; mouse deltas are clamped, not wrapped
 *   - text is typed from a timer, one character per tick, on the host it was meant for
 *   - a stale command file is not run; a key store replaced on disk (restore) restarts hub_btstack
 *   - host table: no gap_disconnect(0) for a host that never had a link, a host that cannot be
 *     tried now waits for its turn, the pinned BLE remote is found in bt_remote_map.json, the
 *     default-host scan stays inside its buffer, the status file is replaced whole
 *   - BLE: forget cancels a connection being set up; the key store gets a record only on change */
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
#define TEST_DIR "/tmp/hng_bt_host_test"
#define PID_FILE TEST_DIR "/hub_btstack.pid"
#define STATUS_FILE TEST_DIR "/hub_btstack_status.json"
#define LOG_FILE TEST_DIR "/hub-btstack.log"
#define DAEMON_PORT 1
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
static int count_calls(const char *prefix) {
    int n = 0;
    for (int i = 0; i < s_ncalls; i++) {
        if (strncmp(s_calls[i], prefix, strlen(prefix)) == 0) n++;
    }
    return n;
}
static void reset_calls(void) { s_ncalls = 0; }

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
static int s_exit_calls;
void btstack_run_loop_trigger_exit(void) { s_exit_calls++; }
void btstack_run_loop_set_data_source_fd(btstack_data_source_t *ds, int fd) { ds->fd = fd; }
void btstack_run_loop_set_data_source_handler(btstack_data_source_t *ds, void (*p)(btstack_data_source_t *, btstack_data_source_callback_type_t)) { ds->process = p; }
void btstack_run_loop_add_data_source(btstack_data_source_t *ds) { UNUSED(ds); }
int btstack_run_loop_remove_data_source(btstack_data_source_t *ds) { UNUSED(ds); return 1; }
void btstack_run_loop_enable_data_source_callbacks(btstack_data_source_t *ds, uint16_t types) { ds->flags |= types; }

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
void hid_device_disconnect(uint16_t cid) { record("hid_device_disconnect %04x", cid); }
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
static void fake_delete_tag(void *c, uint32_t tag) { UNUSED(c); UNUSED(tag); }
static const btstack_tlv_t s_fake_tlv = {fake_get_tag, fake_store_tag, fake_delete_tag};

/* ---- the code ---- */
#include "../payload/source/hub_btstack/bt_state.c"
#include "../payload/source/hub_btstack/bt_hid.c"
#if __has_include("../payload/source/hub_btstack/bt_hid_queue.c")
#include "../payload/source/hub_btstack/bt_hid_queue.c"
#endif
#include "../payload/source/hub_btstack/bt_cmd.c"
#include "../payload/source/hub_btstack/bt_pairing.c"

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("[-] FAIL: " __VA_ARGS__); printf("\n"); s_fail = 1; } else { printf("  ok  " __VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t ADDR_A[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x01};
static const uint8_t ADDR_B[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x02};

static void fire(btstack_timer_source_t *ts) {
    if (!ts->armed) return;
    ts->armed = false;
    ts->process(ts);
}

/* hosts A (connected, cid 0x41) and B (not connected); target A */
static void setup_hosts(void) {
    memset(g_hosts, 0, sizeof(g_hosts));
    for (int i = 0; i < MAX_HOST_CONNS; i++) g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
    memcpy(g_hosts[0].addr, ADDR_A, 6);
    g_hosts[0].connected = true;
    g_hosts[0].hid_cid = 0x41;
    memcpy(g_hosts[1].addr, ADDR_B, 6);
    g_active_host_idx = 0;
    g_host_reconnect_active = false;
    g_host_connecting = false;
    g_pending_wake_btn[0] = '\0';
    g_host_pending_release_type = 0;
    g_host_pending_cid = 0;
    g_host_mouse_buttons = 0;
    btstack_run_loop_remove_timer(&g_host_key_release_timer);
    btstack_run_loop_remove_timer(&g_host_mrelease_timer);
    memset(s_reports, 0, sizeof(s_reports));
    snprintf(s_activity, sizeof(s_activity), "1");
    rq_clear();
    reset_calls();
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void fifo_lines(const char *text) {
    int fd = open(BTHID_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        printf("[-] FAIL: cannot open the test pipe: %s\n", strerror(errno));
        s_fail = 1;
        return;
    }
    if (write(fd, text, strlen(text)) < 0) s_fail = 1;
    close(fd);
    poll_bthid_fifo();
}

static void test_addresses(void) {
    printf("[-] addresses\n");
    bd_addr_t a;
    CHECK(parse_bd_addr("00:11:22:33:44:5f", a) && a[5] == 0x5f, "a full address parses");
    CHECK(parse_bd_addr("00:11:22:33:44:55 30", a), "an address followed by a duration parses");
    CHECK(!parse_bd_addr("0:1:2:3:4:5", a), "one-digit bytes are not an address");
    CHECK(!parse_bd_addr(" 00:11:22:33:44:55", a), "leading blanks are not an address");
    CHECK(!parse_bd_addr("00:11:22:33:44:55x", a), "text after the sixth byte is not an address");
    CHECK(!parse_bd_addr("001122334455", a), "no colons is not an address");

    setup_hosts();
    g_hosts[1].connected = true;
    g_hosts[1].hid_cid = 0x42;
    execute_btstack_cmd("disconnect_host 001122334401");
    CHECK(count_calls("hid_device_disconnect") == 0, "disconnect_host with a wrong address closes no host (it closed every host)");
    execute_btstack_cmd("disconnect_host 00:11:22:33:44:02");
    CHECK(count_calls("hid_device_disconnect 0042") == 1 && count_calls("hid_device_disconnect") == 1, "disconnect_host with an address closes that host only");
    setup_hosts();
    execute_btstack_cmd("connect_host xx");
    CHECK(count_calls("hid_device_connect") == 0, "connect_host with a wrong address does nothing");
    execute_btstack_cmd("connect_host 30");
    CHECK(count_calls("hid_device_connect") == 0 && !g_host_reconnect_active, "connect_host <duration> is \"no address\": the target A is connected already");

    printf("[-] TARGET that cannot be used\n");
    setup_hosts();
    fifo_lines("TARGET not-an-address\nKBD 52\n");
    CHECK(s_reports[0] == 0, "the key after a TARGET that is no address is not sent to the previous host A");
    fifo_lines("TARGET 0:1:2:3:4:5\nKBD 52\n");
    CHECK(s_reports[0] == 0, "the key after a TARGET that does not parse is not sent to the previous host A");
    CHECK(count_calls("hid_device_connect") == 0 && !g_host_reconnect_active, "and no host is reconnected for it");
    fifo_lines("TARGET 00:11:22:33:44:01\nKBD 52\n");
    CHECK(s_reports[0] == 1, "a valid TARGET afterwards routes keys again");
}

static void test_wake_key(void) {
    printf("[-] the kept wake key\n");
    setup_hosts();
    g_active_host_idx = 1; /* B, link down */
    snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "CONSUMER 0030");
    g_pending_wake_time = time(NULL) - 60;
    send_host_consumer_report(0x0223);
    CHECK(strstr(g_pending_wake_btn, "0223") != NULL, "a stale kept key is replaced by the new one (it blocked it): '%s'", g_pending_wake_btn);
    CHECK(strstr(g_pending_wake_btn, "@00:11:22:33:44:02") != NULL, "the kept key names its host: '%s'", g_pending_wake_btn);
    send_host_consumer_report(0x0030);
    CHECK(strstr(g_pending_wake_btn, "0223") != NULL, "a fresh key for the same host stays (the power-on key)");
    char out[64];
    CHECK(!wake_key_take(ADDR_A, out, sizeof(out)) && g_pending_wake_btn[0], "host A opening does not take B's key");
    CHECK(wake_key_take(ADDR_B, out, sizeof(out)) && strcmp(out, "CONSUMER 0223") == 0 && !g_pending_wake_btn[0], "host B takes it: '%s'", out);

    setup_hosts();
    g_active_host_idx = 1;
    snprintf(s_activity, sizeof(s_activity), "-1");
    send_host_consumer_report(0x0032);
    CHECK(!g_pending_wake_btn[0], "Hub off: the reconnect is refused and no key is kept for a later start");

    setup_hosts();
    g_active_host_idx = 1;
    send_host_consumer_report(0x0223);
    memcpy(g_host_reconnect_target, ADDR_B, 6);
    g_host_reconnect_active = true;
    g_host_reconnect_attempts = MAX_HOST_RECONNECT_ATTEMPTS;
    g_host_connecting = false;
    host_reconnect_timer_handler(&g_host_reconnect_timer);
    CHECK(!g_pending_wake_btn[0], "the key is dropped when the reconnect to its host gives up");

    setup_hosts();
    g_active_host_idx = BT_HOST_IDX_NONE;
    send_host_keyboard_report(0, 0x52);
    CHECK(!g_pending_wake_btn[0] && count_calls("hid_device_connect") == 0 && !g_host_reconnect_active,
          "a key for a target that cannot be used is dropped: not kept, no reconnect");
    execute_btstack_cmd("stop_reconnect");
}

static void test_releases(void) {
    printf("[-] keys and buttons with no release behind them\n");
    setup_hosts();
    fifo_lines("A1010200000000000000\n");
    CHECK(g_host_pending_release_type == 1 && g_host_key_release_timer.armed, "a report holding only a modifier gets a release");
    fire(&g_host_key_release_timer);
    CHECK(s_last_report[0][1] == 0x01 && s_last_report[0][2] == 0 && s_last_report[0][4] == 0, "and the host gets it");

    setup_hosts();
    fifo_lines("RAW a1 01 00 00 52 00 00 00 00 00\n");
    CHECK(g_host_pending_release_type == 1 && g_host_key_release_timer.armed && g_host_key_release_timer.timeout == HOST_RAW_HOLD_MS,
          "a RAW key down is released after the hold time");
    fifo_lines("RAW a1 01 00 00 00 00 00 00 00 00\n");
    CHECK(g_host_pending_release_type == 0 && !g_host_key_release_timer.armed, "a RAW release by the writer ends the hold");

    setup_hosts();
    fifo_lines("a10200e9|\n");
    CHECK(g_host_pending_release_type == 2 && g_host_key_release_timer.armed, "\"press|\" with no release part is released after the hold time");

    setup_hosts();
    send_host_mouse_report(0x01, 0, 0, 0);
    CHECK(g_host_mrelease_timer.armed && g_host_mrelease_timer.timeout == HOST_MOUSE_HOLD_MS, "a mouse button held with no later report gets a release");
    fire(&g_host_mrelease_timer);
    CHECK(s_last_report[0][1] == 0x03 && s_last_report[0][2] == 0 && g_host_mouse_buttons == 0, "and the host gets it");

    printf("[-] mouse deltas\n");
    setup_hosts();
    fifo_lines("MOUSE 0 200 -300 0\n");
    CHECK((int8_t)s_last_report[0][3] == 127 && (int8_t)s_last_report[0][4] == -127, "200 and -300 are clamped to 127 and -127 (200 was sent as -56): %d %d",
          (int8_t)s_last_report[0][3], (int8_t)s_last_report[0][4]);
    fifo_lines("MOUSE_WHEEL 129\n");
    CHECK((int8_t)s_last_report[0][5] == 127, "the wheel too");
}

static void test_text(void) {
    printf("[-] text\n");
    setup_hosts();
    g_hosts[1].connected = true;
    g_hosts[1].hid_cid = 0x42;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    send_host_text("abc");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(s_reports[0] == 1 && s_text_timer.armed && ms < 20, "one character typed at once, the rest from a timer (no sleep: %ld ms)", ms);
    g_active_host_idx = 1; /* another key chooses host B meanwhile */
    while (s_text_timer.armed) fire(&s_text_timer);
    fire(&g_host_key_release_timer);
    CHECK(s_reports[1] == 0, "the rest of the text still goes to host A, not to the new target B");
    CHECK(s_last_report[0][4] == 0, "and the last character is released");

    /* a second text while the first is being typed: the room of the characters already typed is
     * used again (the queue holds HOST_TEXT_MAX = 1024) */
    setup_hosts();
    char text[1001];
    memset(text, 'a', 1000);
    text[1000] = '\0';
    send_host_text(text);
    for (int i = 0; i < 899 && s_text_timer.armed; i++) fire(&s_text_timer);
    CHECK(s_text_len - s_text_pos == 100, "900 of 1000 characters typed, 100 waiting (%zu)", s_text_len - s_text_pos);
    text[900] = '\0';
    send_host_text(text);
    CHECK(s_text_len - s_text_pos == 1000, "a second text of 900 waits whole behind them (%zu waiting)", s_text_len - s_text_pos);
    while (s_text_timer.armed) fire(&s_text_timer);
    fire(&g_host_key_release_timer);
    CHECK(s_text_len == 0 && s_text_pos == 0, "and the queue is empty after both");
}

static void test_files(void) {
    printf("[-] command file and key store\n");
    setup_hosts();
    write_file(CMD_FILE, "disconnect_host 00:11:22:33:44:01\n");
    struct utimbuf ut = {time(NULL) - 120, time(NULL) - 120};
    utime(CMD_FILE, &ut);
    check_command_file();
    CHECK(count_calls("hid_device_disconnect") == 0 && access(CMD_FILE, F_OK) != 0, "a command file written long ago is dropped, not run");
    write_file(CMD_FILE, "disconnect_host 00:11:22:33:44:01\n");
    check_command_file();
    CHECK(count_calls("hid_device_disconnect 0041") == 1, "a fresh one is run");

    write_file(TLV_DB_PATH, "old");
    init_cmd_socket();
    g_running = 1;
    s_exit_calls = 0;
    execute_btstack_cmd("reload_map");
    CHECK(g_running == 1 && s_exit_calls == 0, "reload_map with the same key store keeps running");
    write_file(TEST_DIR "/keys.new", "restored");
    rename(TEST_DIR "/keys.new", TLV_DB_PATH);
    execute_btstack_cmd("reload_map");
    CHECK(g_running == 0, "after a restore replaced the key store, reload_map makes hub_btstack start again");
    CHECK(s_exit_calls == 1, "and leaves the run loop at once, not at the next command poll");
    g_running = 1;
    cleanup_cmd_socket();

    tlv_impl = &s_fake_tlv;
    s_store_calls = 0;
    bt_tlv_store_if_changed(TLV_TAG_HIDR, ADDR_A, 6);
    bt_tlv_store_if_changed(TLV_TAG_HIDR, ADDR_A, 6);
    bt_tlv_store_if_changed(TLV_TAG_HIDR, ADDR_A, 6);
    CHECK(s_store_calls == 1, "the same address stored three times is one record (%d)", s_store_calls);
    bt_tlv_store_if_changed(TLV_TAG_HIDR, ADDR_B, 6);
    CHECK(s_store_calls == 2, "a new address is stored");
    tlv_impl = NULL;
}

static void test_hosts(void) {
    printf("[-] host table and connections\n");
    setup_hosts();
    memset(g_hosts, 0, sizeof(g_hosts));
    host_conn_t *h = register_host_target(ADDR_B, NULL);
    CHECK(h && h->acl_handle == HCI_CON_HANDLE_INVALID, "a new host entry has no connection handle (it had 0, a valid handle)");

    setup_hosts();
    g_host_connecting = true;
    g_host_connecting_start = time(NULL);
    try_connect_host_addr(ADDR_B);
    CHECK(rq_count() == 1, "a host that cannot be tried while another attempt runs waits for its turn");
    execute_btstack_cmd("connect_host 00:11:22:33:44:03");
    CHECK(rq_count() == 2, "connect_host during another attempt queues the host (it was dropped)");
    g_host_connecting = false;

    printf("[-] pinned BLE remote and default host\n");
    unlink(TARGET_FILE);
    write_file(MAP_FILE, "{\n\t\"remote\":\t{\n\t\t\"bdaddr\":\t\"00:11:22:33:44:05\"\n\t}\n}\n");
    bd_addr_t a;
    CHECK(get_pinned_target(a) == 1 && a[5] == 0x05, "the remote's address is found in bt_remote_map.json");
    write_file(MAP_FILE, "{\"remote\":{}}");
    CHECK(get_pinned_target(a) == 0, "a map without an address pins nothing");

    unlink(HOST_TARGET_FILE);
    char big[4200];
    memset(big, ' ', sizeof(big));
    memcpy(big + 4083, "00:11:22:33:4", 13);
    big[sizeof(big) - 1] = '\0';
    write_file(DEVICES_FILE, big);
    CHECK(get_pinned_host_target(a) == 0, "an address cut by the end of the buffer is not read past the buffer");
    write_file(HOST_TARGET_FILE, "");
    init_known_hosts();
    CHECK(access(HOST_TARGET_FILE, F_OK) != 0, "an empty default host file is removed at start (it was never rewritten)");

    printf("[-] status file and BLE\n");
    update_status("ready");
    FILE *f = fopen(STATUS_FILE, "r");
    char st[512] = "";
    size_t n = f ? fread(st, 1, sizeof(st) - 1, f) : 0;
    if (f) fclose(f);
    st[n] = '\0';
    CHECK(strstr(st, "\"state\"") != NULL && access(STATUS_FILE ".new", F_OK) != 0, "the status file is written whole and renamed into place");

    reset_calls();
    app_state = APP_CONNECTING;
    forget_ble_remote();
    CHECK(count_calls("gap_connect_cancel") == 1, "forget cancels a connection to the remote being set up");
    reset_calls();
    app_state = APP_IDLE;
    forget_ble_remote();
    CHECK(count_calls("gap_connect_cancel") == 0, "and does nothing of the kind otherwise");
}

int main(void) {
    if (system("rm -rf " TEST_DIR " && mkdir -p " TEST_DIR) != 0) return 1;
    if (mkfifo(BTHID_FIFO, 0600) != 0) {
        perror("mkfifo");
        return 1;
    }
    poll_bthid_fifo(); /* opens the pipe */
    test_addresses();
    test_wake_key();
    test_releases();
    test_text();
    test_files();
    test_hosts();
    if (g_bthid_fifo_fd >= 0) close(g_bthid_fifo_fd);
    if (system("rm -rf " TEST_DIR) != 0) return 1;
    if (s_fail) return 1;
    printf("[+] test_bt_hub_btstack: hub_btstack command lines, hosts, wake key, releases and files\n");
    return 0;
}
