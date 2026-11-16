/* hub_btstack's host_forget <addr> (sent by POST /api/bt-forget-host), against the fake BTstack
 * (bt_fake_btstack.h). There was no way to remove a paired classic host.
 *
 * Checks:
 *   - the host's link key is dropped, its HID channels and its link are closed
 *   - it leaves the host table; keys for it go nowhere, not to another connected host
 *   - its reconnect loop stops (a loop for another host goes on), it leaves the reconnect queue,
 *     a key kept for it and a device link waiting for it are dropped
 *   - the "last paired host" record and the default host file go when they name it
 *   - a host the Hub is still connecting to stays gone: closing that connection reports a failed
 *     open at once (BTstack), which used to start a new 25-attempt reconnect loop to it
 *   - a forget right after a restart is not undone by the restore of the hosts connected before
 *   - a wrong address does nothing; "forget" alone still forgets the BLE remote */
#define TEST_DIR "/tmp/hng_bt_forget_test"
#define DAEMON_PORT 9244 /* nothing listens: device links wait */
#include "bt_fake_btstack.h"

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

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static bool in_table(const uint8_t *a) {
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (memcmp(g_hosts[i].addr, a, 6) == 0) return true;
    }
    return false;
}

/* BTstack closing a channel that still waits for its link: "open failed" inside the call */
static int s_failed_opens;
static void pending_open_fails(uint16_t cid) {
    if (cid != 0x50) return;
    s_failed_opens++;
    host_open_failed(ADDR_A, cid, 0x16);
}

static int pending_links(void) {
#ifdef LINK_PENDING_MAX
    return s_links_n;
#else
    return 0;
#endif
}

/* hosts A (connected, cid 0x41, link 0x0010) and B (connected, cid 0x42, link 0x0011); target A */
static void setup_hosts(void) {
    memset(g_hosts, 0, sizeof(g_hosts));
    for (int i = 0; i < MAX_HOST_CONNS; i++) g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
    memcpy(g_hosts[0].addr, ADDR_A, 6);
    g_hosts[0].connected = true;
    g_hosts[0].hid_cid = 0x41;
    g_hosts[0].acl_handle = 0x0010;
    memcpy(g_hosts[1].addr, ADDR_B, 6);
    g_hosts[1].connected = true;
    g_hosts[1].hid_cid = 0x42;
    g_hosts[1].acl_handle = 0x0011;
    g_active_host_idx = 0;
    g_host_connected = true;
    g_host_reconnect_active = false;
    g_host_connecting = false;
    g_pending_wake_btn[0] = '\0';
    memset(s_reports, 0, sizeof(s_reports));
    rq_clear();
    tlv_impl = &s_fake_tlv;
    memcpy(g_host_addr, ADDR_A, 6);
    memcpy(s_tag_val, ADDR_A, 6);
    s_tag_len = 6;
    reset_calls();
}

int main(void) {
    if (system("rm -rf " TEST_DIR " && mkdir -p " TEST_DIR) != 0) return 1;

    printf("[-] forget a connected host\n");
    setup_hosts();
    /* a loop for A, B queued behind it, a key kept for A, a device link to A waiting for hub_daemon */
    memcpy(g_host_reconnect_target, ADDR_A, 6);
    g_host_reconnect_active = true;
    btstack_run_loop_add_timer(&g_host_reconnect_timer);
    rq_push(ADDR_A);
    snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "CONSUMER 0223@00:11:22:33:44:01");
    g_pending_wake_time = time(NULL);
    link_bt_device_to_addr("3", "00:11:22:33:44:01");
    CHECK(pending_links() == 1, "setup: the device link waits for hub_daemon");
    write_file(TARGET_FILE, "00:11:22:33:44:09\n");
    reset_calls();

    execute_btstack_cmd("host_forget 00:11:22:33:44:01");
    CHECK(count_calls("gap_drop_link_key 01") == 1, "its link key is dropped");
    CHECK(count_calls("hid_device_disconnect 0041") == 1 && count_calls("gap_disconnect 0010") == 1,
          "its HID channels and its link are closed");
    CHECK(count_calls("hid_device_disconnect 0042") == 0 && count_calls("gap_disconnect 0011") == 0, "the other host's are not");
    CHECK(!in_table(ADDR_A) && in_table(ADDR_B), "it leaves the host table, the other host stays");
    CHECK(!g_host_reconnect_active && rq_count() == 0, "its reconnect loop stops and it leaves the queue");
    CHECK(!g_pending_wake_btn[0], "the key kept for it is dropped");
    CHECK(pending_links() == 0, "the device link waiting for it is dropped");
    CHECK(is_null_bd_addr(g_host_addr) && count_calls("tlv_delete") == 1, "the last paired host record naming it goes");
    CHECK(access(TARGET_FILE, F_OK) == 0, "the BLE remote is left alone");

    printf("[-] keys after it\n");
    CHECK(g_active_host_idx == BT_HOST_IDX_NONE, "its keys have no target");
    send_host_keyboard_report(0, 0x52);
    CHECK(s_reports[0] == 0 && s_reports[1] == 0, "a key meant for it reaches no host (not the other connected one)");

    printf("[-] what stays\n");
    setup_hosts();
    memcpy(g_host_reconnect_target, ADDR_B, 6);
    g_host_reconnect_active = true;
    memcpy(g_host_addr, ADDR_B, 6);
    execute_btstack_cmd("host_forget 00:11:22:33:44:01");
    CHECK(g_host_reconnect_active && memcmp(g_host_reconnect_target, ADDR_B, 6) == 0, "a reconnect loop for another host goes on");
    CHECK(memcmp(g_host_addr, ADDR_B, 6) == 0 && count_calls("tlv_delete") == 0, "the last paired host record naming another host stays");
    g_host_reconnect_active = false;

    setup_hosts();
    execute_btstack_cmd("host_forget 001122334401");
    execute_btstack_cmd("host_forget");
    CHECK(count_calls("gap_drop_link_key") == 0 && in_table(ADDR_A) && access(TARGET_FILE, F_OK) == 0,
          "a missing or wrong address does nothing");
    execute_btstack_cmd("forget");
    CHECK(access(TARGET_FILE, F_OK) != 0 && in_table(ADDR_A), "\"forget\" alone still forgets the BLE remote, not a host");

    printf("[-] default host file\n");
    setup_hosts();
    write_file(HOST_TARGET_FILE, "bdaddr=00:11:22:33:44:01\n");
    execute_btstack_cmd("host_forget 00:11:22:33:44:01");
    CHECK(access(HOST_TARGET_FILE, F_OK) != 0, "the default host file naming it is removed (a connection may have written it again)");
    setup_hosts();
    write_file(HOST_TARGET_FILE, "bdaddr=00:11:22:33:44:02\n");
    execute_btstack_cmd("host_forget 00:11:22:33:44:01");
    CHECK(access(HOST_TARGET_FILE, F_OK) == 0, "one naming another host stays");
    unlink(HOST_TARGET_FILE);

    printf("[-] a host the Hub is still connecting to\n");
    setup_hosts();
    memset(&g_hosts[0], 0, sizeof(g_hosts[0]));
    g_hosts[0].acl_handle = HCI_CON_HANDLE_INVALID;
    g_active_host_idx = -1;
    register_host_target(ADDR_A, "Old TV");
    start_host_reconnect(ADDR_A, MAX_HOST_RECONNECT_ATTEMPTS); /* a key for it: paging, cid 0x50 */
    CHECK(g_host_reconnect_active && count_calls("hid_device_connect 01") == 1, "setup: the Hub is paging host A");
    s_fake_on_hid_disconnect = pending_open_fails;
    reset_calls();
    execute_btstack_cmd("host_forget 00:11:22:33:44:01");
    s_fake_on_hid_disconnect = NULL;
    CHECK(count_calls("hid_device_disconnect 0050") == 1 && s_failed_opens == 1, "the pending connection is closed (and reports a failed open at once)");
    int attempts = 0;
    for (int i = 0; i < 40 && g_host_reconnect_active; i++) {
        g_host_connecting = false;
        g_host_reconnect_timer.armed = false;
        g_host_reconnect_timer.process(&g_host_reconnect_timer);
        attempts++;
    }
    CHECK(!g_host_reconnect_active && count_calls("hid_device_connect") == 0 && !in_table(ADDR_A),
          "no reconnect loop, no connection attempt, not back in the table (%d loop ticks, %d attempts)", attempts, count_calls("hid_device_connect"));
    host_open_failed(ADDR_A, 0x50, 0x16); /* the same report arriving later */
    CHECK(!g_host_reconnect_active && !in_table(ADDR_A), "a failed open reported later does not bring it back either");
    setup_hosts();
    g_hosts[1].connected = false;
    g_hosts[1].hid_cid = 0;
    host_open_failed(ADDR_B, 0x52, 0x04);
    CHECK(g_host_reconnect_active && memcmp(g_host_reconnect_target, ADDR_B, 6) == 0, "a known host whose connection failed is still reconnected");
    stop_host_reconnect();

    printf("[-] right after a restart\n");
    setup_hosts();
    memcpy(s_hosts_before[0], ADDR_A, 6);
    memcpy(s_hosts_before[1], ADDR_B, 6);
    s_hosts_before_n = 2;
    execute_btstack_cmd("host_forget 00:11:22:33:44:01");
    rq_clear();
    bt_hosts_restore_after_start();
    CHECK(!in_table(ADDR_A) && in_table(ADDR_B), "the restore of the hosts connected before the restart does not bring it back");
    stop_host_reconnect();
    rq_clear();

    if (system("rm -rf " TEST_DIR) != 0) return 1;
    if (s_fail) return 1;
    printf("[+] test_bt_forget_host_cmd: hub_btstack forgets a classic host\n");
    return 0;
}
