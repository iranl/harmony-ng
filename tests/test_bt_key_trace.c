/* hub_btstack's key trace (bt_hid.c) on the host, with BTstack and the rest of hub_btstack
 * replaced by stubs as in test_bt_hid_hex.c. With debug logging on, hub_daemon sends the times
 * of a key from a remote before its command; the first report of that command writes one line
 * to the trace log:
 *
 *   21:04:07.512 KBD_DOWN 4F      rf>daemon=1 daemon=1 pipe=0 bt=1 total=3 ms
 *
 * Checks: nothing is written without a TS line or with a broken one, one line per key with the
 * four parts adding up to the total, the release after it writes no second line, and a report
 * the stack refuses is marked as waiting. */
#define HARMONY_BT_STATE_H
#define HARMONY_BT_PAIRING_H
#define KEY_TRACE_LOG "/tmp/test_bt_key_trace.log"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#define UNUSED(x) (void)(x)
typedef uint8_t bd_addr_t[6];
typedef uint16_t hci_con_handle_t;
typedef struct btstack_timer_source {
    void (*process)(struct btstack_timer_source *ts);
    uint32_t timeout;
} btstack_timer_source_t;

#define MAX_HOST_CONNS 4
typedef struct {
    uint16_t hid_cid;
    hci_con_handle_t acl_handle;
    bd_addr_t addr;
    bool connected;
    bool manual_disconnect;
    char name[64];
} host_conn_t;

host_conn_t g_hosts[MAX_HOST_CONNS];
int g_active_host_idx = 0;
bool g_host_reconnect_active = false;
unsigned long g_keys_sent = 0;
char g_pending_wake_btn[64];
time_t g_pending_wake_time;

static uint8_t s_send_status; /* what the stack answers to a report: 0 = taken */

void btstack_run_loop_add_timer(btstack_timer_source_t *ts) { UNUSED(ts); }
int btstack_run_loop_remove_timer(btstack_timer_source_t *ts) { UNUSED(ts); return 0; }
void btstack_run_loop_set_timer(btstack_timer_source_t *ts, uint32_t ms) { ts->timeout = ms; }
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *ts, void (*h)(btstack_timer_source_t *)) { ts->process = h; }
uint8_t hid_device_send_interrupt_message(uint16_t cid, const uint8_t *msg, uint16_t len) {
    UNUSED(cid); UNUSED(msg); UNUSED(len);
    return s_send_status;
}
uint32_t btstack_run_loop_get_time_ms(void) { return 0; }
void hid_device_request_can_send_now_event(uint16_t cid) { UNUSED(cid); }
void log_msg(const char *msg) { UNUSED(msg); }

int is_null_bd_addr(const bd_addr_t addr) { UNUSED(addr); return 1; }
int get_pinned_host_target(bd_addr_t addr) { UNUSED(addr); return 0; }
uint16_t get_target_host_hid_cid(void) { return 0x41; }
void start_host_reconnect(const bd_addr_t target, int max_attempts) { UNUSED(target); UNUSED(max_attempts); }
void try_connect_host(void) {}
void update_bthid_status(void) {}
void hub_bt_log_debug(const char *msg) { UNUSED(msg); }

#include "../payload/source/hub_btstack/bt_hid.c"
#include "../payload/source/hub_btstack/bt_hid_queue.c"

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("[-] FAIL: " __VA_ARGS__); printf("\n"); s_fail = 1; } } while (0)

static const uint8_t PRESS[] = { 0xA1, 0x01, 0x00, 0x00, 0x4F, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t RELEASE[] = { 0xA1, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

/* The trace log: number of lines, the last one in `last` */
static int read_log(char *last, size_t len) {
    int n = 0;
    last[0] = '\0';
    FILE *f = fopen(KEY_TRACE_LOG, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        n++;
        snprintf(last, len, "%s", line);
    }
    fclose(f);
    return n;
}

static unsigned long long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

/* A key that hub_rf received 30 ms ago, hub_daemon took 20 ms ago and wrote 10 ms ago */
static void begin_key(const char *cmd) {
    unsigned long long now = mono_ms();
    char times[96];
    snprintf(times, sizeof(times), "%llu %llu %llu", now - 30, now - 20, now - 10);
    bt_hid_trace_begin(times);
    bt_hid_trace_command(cmd);
}

int main(void) {
    char last[256];
    unlink(KEY_TRACE_LOG);
    unlink(KEY_TRACE_LOG ".1");

    /* 1. No TS line (debug logging off, or not a key from a remote): nothing is written */
    bt_hid_trace_command("KBD_DOWN 4F");
    bt_hid_send_report(0x41, PRESS, sizeof(PRESS));
    CHECK(read_log(last, sizeof(last)) == 0, "a report without a TS line wrote to the trace log");

    /* 2. A broken TS line: nothing is written */
    bt_hid_trace_begin("12 garbage");
    bt_hid_trace_command("KBD_DOWN 4F");
    bt_hid_send_report(0x41, PRESS, sizeof(PRESS));
    CHECK(read_log(last, sizeof(last)) == 0, "a broken TS line wrote to the trace log");

    /* 3. A traced key: one line, the four parts add up to the total */
    begin_key("KBD_DOWN 4F");
    bt_hid_send_report(0x41, PRESS, sizeof(PRESS));
    int n = read_log(last, sizeof(last));
    int hh = -1, mm = -1, ss = -1, ms = -1;
    char c1[16] = "", c2[16] = "", unit[8] = "";
    unsigned long long rf = 0, dm = 0, pipe = 0, bt = 0, total = 0;
    int fields = sscanf(last, "%2d:%2d:%2d.%3d %15s %15s rf>daemon=%llu daemon=%llu pipe=%llu bt=%llu total=%llu %7s",
                        &hh, &mm, &ss, &ms, c1, c2, &rf, &dm, &pipe, &bt, &total, unit);
    CHECK(n == 1, "a traced key wrote %d line(s), expected 1", n);
    CHECK(fields == 12 && strcmp(c1, "KBD_DOWN") == 0 && strcmp(c2, "4F") == 0 && strcmp(unit, "ms") == 0,
          "the line does not have the documented form: %s", last);
    CHECK(hh >= 0 && hh < 24 && mm >= 0 && mm < 60 && ss >= 0 && ss < 61 && ms >= 0 && ms < 1000,
          "the line does not start with the time of day: %s", last);
    CHECK(rf == 10 && dm == 10 && pipe >= 10 && pipe < 500 && bt < 500,
          "times: rf>daemon=%llu daemon=%llu pipe=%llu bt=%llu (10, 10, 10 or a little more, about 0)", rf, dm, pipe, bt);
    CHECK(total == rf + dm + pipe + bt && total >= 30, "total=%llu is not the sum of the parts", total);
    CHECK(strstr(last, "waiting") == NULL, "a report the stack took is marked as waiting: %s", last);

    /* 4. The release that follows, sent without a TS line: no second line */
    bt_hid_trace_command("RELEASE");
    bt_hid_send_report(0x41, RELEASE, sizeof(RELEASE));
    CHECK(read_log(last, sizeof(last)) == 1, "a command without a TS line added a line");

    /* 5. Of several commands after one TS line, the first is the traced one */
    begin_key("KBD_DOWN 52");
    bt_hid_trace_command("RELEASE");
    bt_hid_send_report(0x41, PRESS, sizeof(PRESS));
    n = read_log(last, sizeof(last));
    CHECK(n == 2 && strstr(last, " KBD_DOWN 52 ") != NULL, "line %d is not for the first command after TS: %s", n, last);

    /* 6. A report the stack refuses (ACL buffers full) waits in the report queue: marked */
    s_send_status = 0x57;
    begin_key("RELEASE");
    bt_hid_send_report(0x41, RELEASE, sizeof(RELEASE));
    n = read_log(last, sizeof(last));
    CHECK(n == 3 && strstr(last, "(waiting in the report queue)") != NULL, "a refused report is not marked as waiting: %s", last);

    unlink(KEY_TRACE_LOG);
    if (s_fail) return 1;
    printf("[+] test_bt_key_trace: one line per traced key, parts add up, nothing without a TS line\n");
    return 0;
}
