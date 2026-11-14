/* hub_btstack bt_hid.c on the host, with BTstack and the rest of hub_btstack replaced by stubs
 * (their header guards are defined first, so the real headers are skipped). Checks the two hex
 * parsers:
 *   - send_host_named_key(): a key name that is not in the table was sent as a consumer usage as
 *     soon as it started with a hex digit ("Eject" -> 0x000E, "Connect" -> 0x000C) and reported as
 *     handled; only a full "0x..." number is a usage now.
 *   - parse_hex_bytes(): an odd last digit stepped over the end of the string and parsed what
 *     followed it ("a1010|a10100" with the '|' cut gave 6 bytes). */
#define HARMONY_BT_STATE_H
#define HARMONY_BT_PAIRING_H
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

static int s_reports;
static uint8_t s_last_report[32];
static int s_last_len;

void btstack_run_loop_add_timer(btstack_timer_source_t *ts) { UNUSED(ts); }
int btstack_run_loop_remove_timer(btstack_timer_source_t *ts) { UNUSED(ts); return 0; }
void btstack_run_loop_set_timer(btstack_timer_source_t *ts, uint32_t ms) { ts->timeout = ms; }
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *ts, void (*h)(btstack_timer_source_t *)) { ts->process = h; }
void hid_device_send_interrupt_message(uint16_t cid, const uint8_t *msg, uint16_t len) {
    UNUSED(cid);
    s_reports++;
    s_last_len = len < sizeof(s_last_report) ? len : (int)sizeof(s_last_report);
    memcpy(s_last_report, msg, (size_t)s_last_len);
}
int is_null_bd_addr(const bd_addr_t addr) { UNUSED(addr); return 1; }
int get_pinned_host_target(bd_addr_t addr) { UNUSED(addr); return 0; }
uint16_t get_target_host_hid_cid(void) { return 0x41; }
void start_host_reconnect(const bd_addr_t target, int max_attempts) { UNUSED(target); UNUSED(max_attempts); }
void try_connect_host(void) {}
void update_bthid_status(void) {}
void hub_bt_log_debug(const char *msg) { UNUSED(msg); }

#include "../payload/source/hub_btstack/bt_hid.c"

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("[-] FAIL: " __VA_ARGS__); printf("\n"); s_fail = 1; } } while (0)

/* Start each case with no press pending, so a report count is this key's alone */
static void reset_reports(void) {
    g_host_pending_release_type = 0;
    g_host_pending_cid = 0;
    s_reports = 0;
}

static void check_unknown_name(const char *name) {
    reset_reports();
    bool handled = send_host_named_key(name);
    CHECK(!handled && s_reports == 0, "unknown key \"%s\": handled=%d, %d report(s) sent", name, handled, s_reports);
}

int main(void) {
    g_hosts[0].hid_cid = 0x41;
    g_hosts[0].connected = true;

    const char *unknown[] = {"Eject", "Clear", "Dvr", "Display", "Favorite", "AudioSelect", "F13",
                             "Connect", "Disconnect", "cafe", "0x", "0x12zz", "0x10000"};
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) check_unknown_name(unknown[i]);

    /* An explicit usage still works */
    reset_reports();
    bool handled = send_host_named_key("0x00E9");
    CHECK(handled && s_reports == 1 && s_last_len == 4 && s_last_report[1] == 0x02 &&
          s_last_report[2] == 0xE9 && s_last_report[3] == 0x00,
          "\"0x00E9\" must be sent as consumer usage 0x00E9 (handled=%d, reports=%d)", handled, s_reports);
    /* A known name is unchanged */
    reset_reports();
    CHECK(send_host_named_key("VolumeUp") && s_reports == 1 && s_last_report[2] == 0xE9, "VolumeUp must be consumer 0x00E9");

    /* parse_hex_bytes: the string ends after an odd digit; what follows must not be read */
    char buf[] = "a1010\0a10100";
    uint8_t out[16];
    int n = parse_hex_bytes(buf, out, sizeof(out));
    CHECK(n == 2 && out[0] == 0xA1 && out[1] == 0x01, "\"a1010\" gave %d bytes, expected 2 (A1 01)", n);
    char raw[] = "1";
    n = parse_hex_bytes(raw, out, sizeof(out));
    CHECK(n == 0, "\"1\" gave %d bytes, expected 0", n);
    char ok[] = "a1 02 e9 00";
    n = parse_hex_bytes(ok, out, sizeof(out));
    CHECK(n == 4 && out[2] == 0xE9, "\"a1 02 e9 00\" gave %d bytes, expected 4", n);

    if (s_fail) return 1;
    printf("[+] test_bt_hid_hex: key names and hex reports parsed only when complete\n");
    return 0;
}
