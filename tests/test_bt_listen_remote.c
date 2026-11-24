/* The pinned BLE remote and a plugin's adv_listen, and BLE broadcasts from another address
 * (addr=): the real bt_listen.c and bt_adv.c with bt_state.c, bt_cmd.c and bt_pairing.c, against
 * the suite's fake BTstack (bt_fake_btstack.h). A clock of its own fires the timers in order.
 *
 * Checks (#266 and two small points of #253):
 *   - a remote that did not connect after it ended a listen is ignored for the next listen only:
 *     the listen after that ends again when it advertises (it was ignored for good)
 *   - renewing a listen does not keep it ignored: at most one listen time (60 s), also when the
 *     hold ends while a listen runs
 *   - a remote that was ready inside the 15 s and went to sleep again is not ignored
 *   - a remote that becomes ready later is not ignored any more
 *   - a remote whose link comes up but never gets ready (bond lost) is still ignored for one
 *     listen: it must not end every listen
 *   - an addr= broadcast queued behind slower ones waits up to 10 s from its first wait (it was
 *     dropped at once), and is still dropped after 10 s of waiting
 *   - sending a broadcast while the retry timer of a waiting addr= one is armed removes the timer
 *     first (BTstack's add_timer does not take a timer already in its list)
 *
 * bt_main.c is not built here: report_from_pinned() plays the end of its advertising report
 * handler (~239), and remote_ready() the places that set APP_READY (bt_main.c, bt_remote.c). */
#define TEST_DIR "/tmp/hng_bt_listen_remote_test"
#define DAEMON_PORT 9621 /* nothing listens */
#define BT_FAKE_REAL_LISTEN_ADV
#include "bt_fake_btstack.h"

/* ---- what bt_adv.c and bt_listen.c need beyond the fake ---- */
typedef enum { GAP_RANDOM_ADDRESS_TYPE_OFF = 0, GAP_RANDOM_ADDRESS_TYPE_STATIC } gap_random_address_type_t;
void gap_random_address_set_mode(gap_random_address_type_t m) { record("random_address_mode %d", (int)m); }
void hci_le_random_address_set(const uint8_t *a) { record("hci_le_random_address_set %02X", a[5]); }
void gap_advertisements_set_params(uint16_t a, uint16_t b, uint8_t t, uint8_t dt, bd_addr_t d, uint8_t ch, uint8_t fp) {
    UNUSED(a); UNUSED(b); UNUSED(t); UNUSED(dt); UNUSED(d); UNUSED(ch); UNUSED(fp);
}
void gap_advertisements_set_data(uint8_t len, uint8_t *d) { UNUSED(len); UNUSED(d); }
void gap_advertisements_enable(int on) { record("adv_enable %d", on); }
int sscanf_bd_addr(const char *s, bd_addr_t addr) {
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return 0;
    for (int i = 0; i < 6; i++) addr[i] = (uint8_t)v[i];
    return 1;
}
static uint8_t s_rep_data[8];
const uint8_t *gap_event_advertising_report_get_data(const uint8_t *p) { UNUSED(p); return s_rep_data; }
uint8_t gap_event_advertising_report_get_data_length(const uint8_t *p) { UNUSED(p); return 0; }
void gap_event_advertising_report_get_address(const uint8_t *p, bd_addr_t a) { UNUSED(p); memset(a, 0x5A, 6); }
uint8_t gap_event_advertising_report_get_address_type(const uint8_t *p) { UNUSED(p); return 1; }
uint8_t gap_event_advertising_report_get_advertising_event_type(const uint8_t *p) { UNUSED(p); return 3; }
int8_t gap_event_advertising_report_get_rssi(const uint8_t *p) { UNUSED(p); return -60; }

/* Builds on code without it too: then the checks fail, not the build */
void bt_listen_remote_ready(void) __attribute__((weak));

#include "../payload/source/hub_btstack/bt_state.c"
#include "../payload/source/hub_btstack/bt_hid.c"
#include "../payload/source/hub_btstack/bt_hid_queue.c"
#include "../payload/source/hub_btstack/bt_cmd.c"
#include "../payload/source/hub_btstack/bt_pairing.c"
#include "../payload/source/hub_btstack/bt_listen.c"
#include "../payload/source/hub_btstack/bt_adv.c"

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("[-] FAIL: " __VA_ARGS__); printf("\n"); s_fail = 1; } else { printf("  ok  " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- the clock: every timer added is noted with its due time ---- */
#define MAX_TIMERS 32
static btstack_timer_source_t *s_timers[MAX_TIMERS];
static uint32_t s_due[MAX_TIMERS];
static int s_ntimers;
static void note_timer(btstack_timer_source_t *ts) {
    int i = 0;
    while (i < s_ntimers && s_timers[i] != ts) i++;
    if (i == s_ntimers) {
        if (s_ntimers == MAX_TIMERS) { printf("[-] FAIL: too many timers\n"); exit(1); }
        s_timers[s_ntimers++] = ts;
    }
    s_due[i] = s_now_ms + ts->timeout;
}
/* lets ms pass, firing the timers that come due, in order */
static void advance(uint32_t ms) {
    uint32_t end = s_now_ms + ms;
    for (;;) {
        int next = -1;
        for (int i = 0; i < s_ntimers; i++) {
            if (!s_timers[i]->armed || (int32_t)(s_due[i] - end) > 0) continue;
            if (next < 0 || (int32_t)(s_due[i] - s_due[next]) < 0) next = i;
        }
        if (next < 0) break;
        if ((int32_t)(s_due[next] - s_now_ms) > 0) s_now_ms = s_due[next];
        s_timers[next]->armed = false;
        s_timers[next]->process(s_timers[next]);
    }
    s_now_ms = end;
}

static void reset(void) {
    for (int i = 0; i < s_ntimers; i++) s_timers[i]->armed = false;
    if (g_adv_listen_active) execute_btstack_cmd("adv_listen plug 0");
    for (int i = 0; i < s_ntimers; i++) s_timers[i]->armed = false;
    g_user_scan_active = false;
    le_pair_window_close();
    g_listen_hold = false;
    g_listen_ignore_remote = false;
    g_adv_head = g_adv_count = 0;
    g_adv_active = g_adv_borrowed = false;
    g_host_connecting = false;
    memset(&remote_device, 0, sizeof(remote_device));
    connection_handle = HCI_CON_HANDLE_INVALID;
    app_state = APP_IDLE;
    FILE *f = fopen(TARGET_FILE, "w");
    if (f) {
        fputs("AA:BB:CC:DD:EE:01\n", f);
        fclose(f);
    }
    hog_start_scan(); /* scanning for the pinned remote */
    reset_calls();
}

/* the end of bt_main.c's advertising report handler for a report of the pinned remote */
static void report_from_pinned(void) {
    if (app_state != APP_SCANNING || g_user_scan_active) return;
    if (g_adv_listen_active && bt_listen_remote_may_end()) bt_listen_end_for_remote();
    if (!g_adv_listen_active) hog_connect();
}
/* the remote's link is up (bt_main.c, LE connection complete) */
static void link_up(void) {
    btstack_run_loop_remove_timer(&reconnect_timer);
    connection_handle = 0x0040;
    app_state = APP_ENCRYPTING;
}
/* ready: encrypted and HIDS connected (bt_main.c re-encryption, bt_remote.c HIDS connected) */
static void remote_ready(void) {
    app_state = APP_READY;
    void (*volatile ready)(void) = bt_listen_remote_ready;
    if (ready) ready();
}
/* the remote's link goes down (bt_main.c, disconnection complete) */
static void link_down(void) {
    connection_handle = HCI_CON_HANDLE_INVALID;
    app_state = APP_IDLE;
    schedule_reconnect(10);
    advance(10);
}
/* a listen of 60 s, the remote advertises once: did the listen end and the Hub connect? */
static int listen_and_report(void) {
    execute_btstack_cmd("adv_listen plug 60");
    if (!g_adv_listen_active) return -1;
    reset_calls();
    report_from_pinned();
    return !g_adv_listen_active && count_calls("gap_connect") == 1;
}
/* the remote ends a listen, the connect times out (10 s), the 15 s hold ends */
static void failed_connect(void) {
    execute_btstack_cmd("adv_listen plug 60");
    report_from_pinned();
    advance(15000);
}

int main(void) {
    mkdir(TEST_DIR, 0755);
    setvbuf(stdout, NULL, _IONBF, 0);
    s_fake_on_add_timer = note_timer;
    s_now_ms = 1000;

    printf("1. a remote that did not connect once\n");
    reset();
    CHECK(listen_and_report() == 1, "its report ends a listen and the Hub connects");
    advance(15000);
    CHECK(app_state == APP_SCANNING && !g_listen_hold, "the connect gives up, the hold ends");
    CHECK(listen_and_report() == 0 && g_adv_listen_active, "the next listen: its report ends nothing");
    advance(60000);
    CHECK(!g_adv_listen_active, "that listen ends (time up)");
    CHECK(listen_and_report() == 1, "the listen after that: its report ends it again and the Hub connects");
    advance(15000);
    CHECK(listen_and_report() == 0, "it did not connect again: the next listen is not ended");
    advance(60000);
    int ended = 0;
    for (int i = 0; i < 6; i++) {
        int r = listen_and_report();
        if (r == 1) {
            ended++;
            advance(15000);
        } else {
            advance(60000);
        }
    }
    CHECK(ended == 3, "six more listens: every second one ends for it (%d of 6)", ended);

    printf("2. a plugin that renews its listen before it ends\n");
    reset();
    failed_connect();
    CHECK(listen_and_report() == 0, "after a failed connect the next listen is not ended");
    advance(30000);
    execute_btstack_cmd("adv_listen plug 60");
    advance(29000);
    execute_btstack_cmd("adv_listen plug 60");
    reset_calls();
    report_from_pinned();
    CHECK(g_adv_listen_active, "renewed, 59 s in: not ended");
    advance(2000);
    execute_btstack_cmd("adv_listen plug 60");
    reset_calls();
    report_from_pinned();
    CHECK(!g_adv_listen_active && count_calls("gap_connect") == 1, "renewed, 61 s in: its report ends the listen");

    printf("3. the remote is ready inside the 15 s and sleeps again\n");
    reset();
    execute_btstack_cmd("adv_listen plug 60");
    report_from_pinned();
    advance(1000);
    link_up();
    remote_ready();
    advance(9000);
    link_down();
    advance(5000);
    CHECK(!g_listen_hold, "the hold ends with the remote asleep");
    CHECK(listen_and_report() == 1, "the next listen ends when it advertises");

    printf("4. ignored after a failed connect, then ready between two listens\n");
    reset();
    failed_connect();
    reset_calls();
    report_from_pinned();
    CHECK(count_calls("gap_connect") == 1, "a report while no listen runs: the Hub connects");
    link_up();
    remote_ready();
    advance(5000);
    link_down();
    CHECK(listen_and_report() == 1, "it was ready: the next listen ends when it advertises");

    printf("5. a link that comes up but is never ready (bond lost) inside the 15 s\n");
    reset();
    execute_btstack_cmd("adv_listen plug 60");
    report_from_pinned();
    advance(1000);
    link_up();
    advance(1000);
    link_down();
    advance(13000);
    CHECK(listen_and_report() == 0 && g_adv_listen_active, "the next listen is not ended by it");

    printf("5b. the same, with a listen renewed while that link was up: the hold ends during the listen\n");
    reset();
    execute_btstack_cmd("adv_listen plug 60");
    report_from_pinned();
    advance(1000);
    link_up();
    execute_btstack_cmd("adv_listen plug 60");
    CHECK(g_adv_listen_active, "a listen while the link is up is taken");
    advance(1000);
    link_down();
    advance(13000); /* the hold ends, the listen runs */
    reset_calls();
    report_from_pinned();
    CHECK(g_adv_listen_active, "right after the hold: its report ends nothing");
    for (int i = 0; i < 2; i++) {
        advance(29500);
        execute_btstack_cmd("adv_listen plug 60");
    }
    reset_calls();
    report_from_pinned();
    CHECK(g_adv_listen_active, "renewed, 59 s after the hold: not ended");
    advance(2000);
    execute_btstack_cmd("adv_listen plug 60");
    reset_calls();
    report_from_pinned();
    CHECK(!g_adv_listen_active && count_calls("gap_connect") == 1, "renewed, 61 s after the hold: its report ends the listen");

    printf("6. an addr= broadcast behind three slower ones, the remote connects as it reaches the head\n");
    reset();
    for (int i = 0; i < 3; i++) execute_btstack_cmd("adv 0201060303AAFE 5000");
    execute_btstack_cmd("adv 0201060303AAFE 500 addr=98:E2:55:99:1F:97");
    advance(10000);
    app_state = APP_CONNECTING;
    btstack_run_loop_remove_timer(&reconnect_timer);
    advance(5000);
    CHECK(g_adv_count == 1 && count_calls("hci_le_random_address_set") == 0, "15 s after it was queued it waits, not dropped");
    advance(4000);
    app_state = APP_READY;
    advance(500);
    CHECK(count_calls("hci_le_random_address_set") == 1 && g_adv_count == 0, "the link is up 4 s later: it goes out");
    advance(1000);

    printf("7. an addr= broadcast that waits more than 10 s\n");
    reset();
    app_state = APP_CONNECTING;
    btstack_run_loop_remove_timer(&reconnect_timer);
    execute_btstack_cmd("adv 0201060303AAFE 500 addr=98:E2:55:99:1F:97");
    advance(9500);
    CHECK(g_adv_count == 1, "9.5 s: still waiting");
    advance(1000);
    CHECK(g_adv_count == 0 && count_calls("hci_le_random_address_set") == 0, "10.5 s: dropped, not sent");
    app_state = APP_READY;
    advance(1000);
    CHECK(count_calls("adv_enable 1") == 0, "not sent late");

    printf("8. a plain broadcast sent while an addr= one waits\n");
    reset();
    app_state = APP_CONNECTING;
    btstack_run_loop_remove_timer(&reconnect_timer);
    execute_btstack_cmd("adv 0201060303AAFE 500 addr=98:E2:55:99:1F:97");
    int before = s_timer_double_adds;
    execute_btstack_cmd("adv 0201060303AAFE 300");
    CHECK(count_calls("adv_enable 1") == 1, "the plain one goes out first");
    CHECK(s_timer_double_adds == before, "no timer added while it was still armed (%d)", s_timer_double_adds - before);
    app_state = APP_READY;
    advance(1000);
    CHECK(count_calls("hci_le_random_address_set") == 1 && g_adv_count == 0, "then the addr= one goes out");

    if (s_fail) {
        printf("[-] test_bt_listen_remote: FAILED\n");
        return 1;
    }
    printf("[+] test_bt_listen_remote: all checks passed\n");
    return 0;
}
