/* hub_btstack bt_cmd.c on the host, with BTstack and the rest of hub_btstack replaced by stubs
 * (their header guards are defined first, so the real headers are skipped). Checks the key pipe
 * (/tmp/bthid_input, here a FIFO in a temporary directory):
 *   - the pipe is a data source of the run loop, so a key is read when hub_daemon writes it. It
 *     was read only from the 100 ms command timer.
 *   - a line cut by the end of a read is kept and runs once, whole, with the next read. Its two
 *     halves used to run as two commands ("KBD_DO" and "WN 4F" were typed to the host as text).
 *   - two commands in one write both run, in order; target and command in one write (what
 *     hub_daemon sends) select the host and send the key.
 *   - a "TS" line (key trace, debug logging) gives its times to the trace and is never taken
 *     for a command; the command after it is the traced one. */
#define HARMONY_BT_STATE_H
#define HARMONY_BT_PAIRING_H
#define HARMONY_BT_HID_H
#define HARMONY_BT_REMOTE_H
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define UNUSED(x) (void)(x)
typedef uint8_t bd_addr_t[6];
typedef uint16_t hci_con_handle_t;
typedef struct btstack_timer_source {
    void (*process)(struct btstack_timer_source *ts);
    uint32_t timeout;
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
    bool added;
} btstack_data_source_t;
typedef enum { APP_IDLE = 0, APP_READY } app_state_t;

static char s_fifo_path[160];
#define BTHID_FIFO s_fifo_path
#define CMD_FILE "/nonexistent/hub_btstack_cmd"
#define CMD_SOCK "/nonexistent/hub_btstack.sock"
#define MAX_HOST_RECONNECT_ATTEMPTS 25
#define MAX_HOST_CONNS 4
typedef struct {
    uint16_t hid_cid;
    hci_con_handle_t acl_handle;
    bd_addr_t addr;
    bool connected;
    bool manual_disconnect;
    char name[64];
} host_conn_t;

/* ---- state the real files own ---- */
app_state_t app_state = APP_READY;
host_conn_t g_hosts[MAX_HOST_CONNS];
int g_active_host_idx = -1;
uint16_t g_host_hid_cid = 0;
bd_addr_t g_host_addr;
bool g_host_connected = false;
bool g_host_pairing = false;
bool g_pairing_new_key_valid = false;
char g_host_name[64];
int g_bthid_fifo_fd = -1;
unsigned long g_keys_sent = 0;
bool g_host_reconnect_active = false;
bd_addr_t g_host_reconnect_target;
bool g_host_connecting = false;
time_t g_host_connecting_start = 0;
char g_pending_wake_btn[64];
time_t g_pending_wake_time;
volatile sig_atomic_t g_running = 1;
bool g_user_scan_active = false;
time_t g_user_scan_end = 0;
btstack_timer_source_t g_host_key_release_timer;
uint8_t g_host_pending_release_type = 0;
uint16_t g_host_pending_cid = 0;
uint8_t g_host_custom_release_buf[16];
int g_host_custom_release_len = 0;
int g_host_send_hold_mode = 0;
uint8_t g_host_mouse_buttons = 0;

/* ---- what the commands did ---- */
#define MAX_CALLS 512
static char s_calls[MAX_CALLS][96];
static int s_ncalls;
static int s_texts; /* lines that fell through to "type this text" */

static void record(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void record(const char *fmt, ...) {
    if (s_ncalls >= MAX_CALLS) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_calls[s_ncalls++], sizeof(s_calls[0]), fmt, ap);
    va_end(ap);
}

/* ---- run loop: remembers the data sources ---- */
static btstack_data_source_t *s_sources[4];
static int s_nsources;
void btstack_run_loop_set_data_source_fd(btstack_data_source_t *ds, int fd) { ds->fd = fd; }
void btstack_run_loop_set_data_source_handler(btstack_data_source_t *ds, void (*process)(btstack_data_source_t *, btstack_data_source_callback_type_t)) { ds->process = process; }
void btstack_run_loop_add_data_source(btstack_data_source_t *ds) {
    ds->added = true;
    if (s_nsources < 4) s_sources[s_nsources++] = ds;
}
int btstack_run_loop_remove_data_source(btstack_data_source_t *ds) { ds->added = false; return 1; }
void btstack_run_loop_enable_data_source_callbacks(btstack_data_source_t *ds, uint16_t types) { ds->flags |= types; }
void btstack_run_loop_add_timer(btstack_timer_source_t *ts) { UNUSED(ts); }
int btstack_run_loop_remove_timer(btstack_timer_source_t *ts) { UNUSED(ts); return 0; }
void btstack_run_loop_set_timer(btstack_timer_source_t *ts, uint32_t ms) { ts->timeout = ms; }
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *ts, void (*h)(btstack_timer_source_t *)) { ts->process = h; }
void btstack_run_loop_trigger_exit(void) {}

/* ---- BTstack and the rest of hub_btstack ---- */
void gap_discoverable_control(uint8_t on) { UNUSED(on); }
void gap_connectable_control(uint8_t on) { UNUSED(on); }
void gap_set_local_name(const char *name) { UNUSED(name); }
void gap_set_class_of_device(uint32_t cod) { UNUSED(cod); }
void gap_stop_scan(void) {}
void hid_device_disconnect(uint16_t cid) { UNUSED(cid); }
void log_msg(const char *msg) { UNUSED(msg); }
void update_status(const char *state) { UNUSED(state); }
void update_bthid_status(void) {}
void read_current_activity(char *out, size_t outlen) { snprintf(out, outlen, "-1"); }
void bt_remote_invalidate_map(void) { record("reload_map"); }
bool get_reconnect_target(bd_addr_t out) { UNUSED(out); return false; }
bool is_host_connected(const bd_addr_t addr) { UNUSED(addr); return true; }
void set_host_manual_disconnect(const bd_addr_t addr, bool on) { UNUSED(addr); UNUSED(on); }
void start_host_reconnect(const bd_addr_t target, int max_attempts) { UNUSED(target); UNUSED(max_attempts); }
void stop_host_reconnect(void) {}
void host_reconnect_next(void) {}
void try_connect_host_addr(const bd_addr_t addr) { UNUSED(addr); }
void try_connect_host(void) {}
int get_pinned_host_target(bd_addr_t addr) { UNUSED(addr); return 0; }
uint16_t get_target_host_hid_cid(void) { return 0x41; }
void hog_start_connect(void) {}
void forget_ble_remote(void) {}
void forget_classic_host(const bd_addr_t addr) { UNUSED(addr); }
void start_user_scan(void) {}
void stop_user_scan(void) {}
void start_user_pair(const char *addr_str) { UNUSED(addr_str); }
void write_scan_results(bool scanning) { UNUSED(scanning); }
void rq_clear(void) {}
bool rq_remove(const uint8_t addr[6]) { UNUSED(addr); return false; }
int is_null_bd_addr(const bd_addr_t addr) {
    static const bd_addr_t zero = {0};
    return memcmp(addr, zero, 6) == 0;
}
int parse_bd_addr(const char *str, bd_addr_t addr) {
    unsigned int b[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return 0;
    for (int i = 0; i < 6; i++) addr[i] = (uint8_t)b[i];
    return 1;
}
host_conn_t *register_host_target(const bd_addr_t addr, const char *name) {
    UNUSED(name);
    memcpy(g_hosts[0].addr, addr, 6);
    g_hosts[0].connected = true;
    g_hosts[0].hid_cid = 0x41;
    record("target %02x:%02x", addr[4], addr[5]);
    return &g_hosts[0];
}

/* ---- bt_hid.c: what a line ends up as ---- */
void send_host_release_all(void) { record("release"); }
void send_host_keyboard_report(uint8_t modifier, uint8_t keycode) {
    UNUSED(modifier);
    record("kbd %02x%s", keycode, g_host_send_hold_mode ? " down" : "");
}
void send_host_consumer_report(uint16_t usage_id) { record("consumer %04x%s", usage_id, g_host_send_hold_mode ? " down" : ""); }
void send_host_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) { UNUSED(buttons); UNUSED(dx); UNUSED(dy); UNUSED(wheel); record("mouse"); }
void send_host_mouse_click(uint8_t button) { UNUSED(button); record("click"); }
bool send_host_named_key(const char *name) { UNUSED(name); return false; } /* not a key name */
bool send_host_named_key_ex(const char *name, int hold_mode) { record("key %s%s", name, hold_mode ? " down" : ""); return true; }
void send_host_text(const char *text) { s_texts++; record("text \"%s\"", text); }
int parse_hex_bytes(const char *hex, uint8_t *out, int max_len) { UNUSED(hex); UNUSED(out); UNUSED(max_len); return 0; }
void bt_hid_send_report(uint16_t cid, const uint8_t *report, uint16_t len) { UNUSED(cid); UNUSED(report); UNUSED(len); record("report"); }
void arm_host_key_release_timer(uint32_t timeout_ms) { UNUSED(timeout_ms); }
/* key trace: the times of a "TS" line, and the command lines offered to it */
static char s_trace_times[64];
static char s_trace_cmds[8][48];
static int s_trace_begins, s_trace_ncmds;
void bt_hid_trace_begin(const char *times) {
    s_trace_begins++;
    snprintf(s_trace_times, sizeof(s_trace_times), "%s", times);
}
void bt_hid_trace_command(const char *cmd) {
    if (s_trace_ncmds < 8) snprintf(s_trace_cmds[s_trace_ncmds], sizeof(s_trace_cmds[0]), "%s", cmd);
    s_trace_ncmds++;
}

#include "../payload/source/hub_btstack/bt_cmd.c"

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("[-] FAIL: " __VA_ARGS__); printf("\n"); s_fail = 1; } } while (0)

static btstack_data_source_t *fifo_source(void) {
    for (int i = 0; i < s_nsources; i++) {
        if (s_sources[i]->added && s_sources[i]->fd == g_bthid_fifo_fd) return s_sources[i];
    }
    return NULL;
}

/* What the run loop does while the pipe is readable: one handler call per readable event. Without
 * a data source (the code before this change) the 100 ms timer made the same call. */
static void pump(void) {
    btstack_data_source_t *ds = fifo_source();
    for (int i = 0; i < 64; i++) {
        struct pollfd pfd = { .fd = g_bthid_fifo_fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) <= 0) break;
        if (ds && (ds->flags & DATA_SOURCE_CALLBACK_READ)) ds->process(ds, DATA_SOURCE_CALLBACK_READ);
        else poll_bthid_fifo();
    }
}

static void reset_calls(void) {
    s_ncalls = 0;
    s_texts = 0;
}

static void put(int wfd, const char *s) {
    ssize_t w = write(wfd, s, strlen(s));
    if (w != (ssize_t)strlen(s)) {
        printf("[-] FAIL: test write to the pipe: %s\n", strerror(errno));
        exit(1);
    }
}

static void dump_calls(void) {
    for (int i = 0; i < s_ncalls && i < 8; i++) printf("      %s\n", s_calls[i]);
}

int main(void) {
    char dir[] = "/tmp/bt_key_pipe_XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(s_fifo_path, sizeof(s_fifo_path), "%s/bthid_input", dir);

    /* 1. The first call (from the command timer at start) creates and opens the pipe, and hands
     * it to the run loop */
    poll_bthid_fifo();
    CHECK(g_bthid_fifo_fd >= 0, "the pipe is not open");
    if (g_bthid_fifo_fd < 0) return 1;
    btstack_data_source_t *ds = fifo_source();
    CHECK(ds != NULL, "the pipe is not a data source of the run loop (read only from the 100 ms timer)");
    CHECK(ds == NULL || (ds->process && (ds->flags & DATA_SOURCE_CALLBACK_READ)),
          "the pipe's data source has no read callback");
    int wfd = open(s_fifo_path, O_WRONLY | O_NONBLOCK);
    CHECK(wfd >= 0, "cannot open the pipe for writing");
    if (wfd < 0) return 1;

    /* 2. Two commands in one write both run, in order */
    reset_calls();
    put(wfd, "KBD_DOWN 4F\nRELEASE\n");
    pump();
    CHECK(s_ncalls == 2 && strcmp(s_calls[0], "kbd 4f down") == 0 && strcmp(s_calls[1], "release") == 0,
          "two commands in one write: %d call(s), first '%s'", s_ncalls, s_ncalls ? s_calls[0] : "");

    /* 3. Target and command in one write, as hub_daemon hands a key over */
    reset_calls();
    put(wfd, "TARGET 00:11:22:33:44:55\nKBD 28\n");
    pump();
    CHECK(s_ncalls == 2 && strcmp(s_calls[0], "target 44:55") == 0 && strcmp(s_calls[1], "kbd 28") == 0,
          "target and command in one write: %d call(s), first '%s'", s_ncalls, s_ncalls ? s_calls[0] : "");

    /* 4. A command that arrives in two pieces: nothing runs for the first piece, and the whole
     * command runs once when the rest is there */
    reset_calls();
    put(wfd, "TARGET 00:11:22:33:44:55\nKBD_DO");
    pump();
    CHECK(s_ncalls == 1 && strcmp(s_calls[0], "target 44:55") == 0,
          "first piece of a cut command: %d call(s), expected the target only", s_ncalls);
    if (s_ncalls != 1) dump_calls();
    put(wfd, "WN 4F\n");
    pump();
    CHECK(s_ncalls == 2 && strcmp(s_calls[1], "kbd 4f down") == 0 && s_texts == 0,
          "cut command: %d call(s), %d typed as text, expected one 'kbd 4f down'", s_ncalls, s_texts);
    if (s_fail) dump_calls();

    /* 5. More waiting than one read takes (1023 bytes): the read ends inside the 147th line.
     * All 200 keys run once, in order, and nothing is typed as text. */
    reset_calls();
    for (int i = 1; i <= 200; i++) {
        char line[16];
        snprintf(line, sizeof(line), "KBD %02X\n", i);
        put(wfd, line);
    }
    pump();
    int in_order = (s_ncalls == 200);
    for (int i = 0; in_order && i < 200; i++) {
        char want[16];
        snprintf(want, sizeof(want), "kbd %02x", i + 1);
        if (strcmp(s_calls[i], want) != 0) in_order = 0;
    }
    CHECK(in_order && s_texts == 0, "200 keys behind each other: %d call(s), %d typed as text, order %s",
          s_ncalls, s_texts, in_order ? "kept" : "broken");

    /* 6. A writer that does not end its line (printf without \n by hand) is still served */
    reset_calls();
    put(wfd, "KBD 05");
    pump();
    CHECK(s_ncalls == 1 && strcmp(s_calls[0], "kbd 05") == 0, "a line without a newline: %d call(s)", s_ncalls);

    /* 7. Key trace: target, times and command in one write, as hub_daemon sends a key from a
     * remote with debug logging on */
    reset_calls();
    s_trace_begins = s_trace_ncmds = 0;
    put(wfd, "TARGET 00:11:22:33:44:55\nTS 5839211 5839213 5839214\nKBD_DOWN 4F\n");
    pump();
    CHECK(s_trace_begins == 1 && strcmp(s_trace_times, "5839211 5839213 5839214") == 0,
          "the TS line did not reach the trace (%d call(s), '%s')", s_trace_begins, s_trace_times);
    CHECK(s_trace_ncmds == 1 && strcmp(s_trace_cmds[0], "KBD_DOWN 4F") == 0,
          "the traced command is not the key (%d offered, first '%s')", s_trace_ncmds, s_trace_cmds[0]);
    CHECK(s_ncalls == 2 && strcmp(s_calls[1], "kbd 4f down") == 0 && s_texts == 0,
          "target, TS and key: %d call(s), %d typed as text", s_ncalls, s_texts);

    close(wfd);
    close(g_bthid_fifo_fd);
    unlink(s_fifo_path);
    rmdir(dir);
    if (s_fail) return 1;
    printf("[+] test_bt_key_pipe: the key pipe is read when written, a cut line runs once and whole, a TS line goes to the trace\n");
    return 0;
}
