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
#include "bt_fake_btstack.h"

/* ---- the code ---- */
#include "../payload/source/hub_btstack/bt_state.c"
#include "../payload/source/hub_btstack/bt_hid.c"
#if __has_include("../payload/source/hub_btstack/bt_hid_queue.c")
#include "../payload/source/hub_btstack/bt_hid_queue.c"
#endif
#include "../payload/source/hub_btstack/bt_cmd.c"
#include "../payload/source/hub_btstack/bt_pairing.c"

/* The sleep calls of libc, counted. The code above is built into this file, so these definitions
 * are the ones it calls; each still sleeps (straight system call). The text check counts calls
 * instead of timing them, so a slow machine cannot fail it. */
#include <sys/syscall.h>
#include <unistd.h>
static int s_sleep_calls;
int nanosleep(const struct timespec *req, struct timespec *rem) {
    s_sleep_calls++;
    return (int)syscall(SYS_nanosleep, req, rem);
}
int clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem) {
    s_sleep_calls++;
    return (int)syscall(SYS_clock_nanosleep, clk, flags, req, rem) == 0 ? 0 : errno;
}
int usleep(useconds_t us) {
    s_sleep_calls++;
    struct timespec ts = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };
    return (int)syscall(SYS_nanosleep, &ts, NULL);
}
unsigned int sleep(unsigned int s) {
    s_sleep_calls++;
    struct timespec ts = { (time_t)s, 0 };
    return syscall(SYS_nanosleep, &ts, NULL) == 0 ? 0 : s;
}

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

    /* "adv_listen" starts with neither "scan" nor "adv ": it must reach its own handler */
    g_user_scan_active = false;
    int before = s_listen_cmds;
    execute_btstack_cmd("adv_listen switch2-wake 30 53050100037E05\n");
    CHECK(s_listen_cmds == before + 1 && !g_user_scan_active, "adv_listen goes to the listen handler, not the remote scan");

    /* the user's remote scan wins over a plugin's listen: the listen ends first */
    g_adv_listen_active = true;
    int ends = s_listen_ends;
    execute_btstack_cmd("scan\n");
    CHECK(s_listen_ends == ends + 1 && !g_adv_listen_active && g_user_scan_active, "a remote scan ends a listen");
    execute_btstack_cmd("stop_scan\n");
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
    s_sleep_calls = 0;
    send_host_text("abc");
    int sleeps = s_sleep_calls;
    CHECK(s_reports[0] == 1 && s_text_timer.armed && sleeps == 0, "one character typed at once, the rest from a timer (no sleep: %d sleep calls)", sleeps);
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
