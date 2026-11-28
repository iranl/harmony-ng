/* Issue 294: legacy PIN pairing and the PS3 host profile (Sony BD Remote), against the fake BTstack.
 *
 * Checks:
 *   - PIN (bt_pin.c): the first request of a host gets 0000 at once; a host that drops the link
 *     after 0000 without a link key is noted as wanting a PIN; its next request is held for the
 *     code the user enters, until a reply, a cancel or the timeout; a host that took 0000 is not
 *     asked; only 1 to 16 digits are a PIN
 *   - BD Remote reports (bt_ps3.c): key code, gamepad bitmap, press byte and release as in
 *     hid-sony.c; keyboard and consumer keys translated; the stock descriptor's length
 *   - hub_btstack: a PS3 host gets BD Remote reports for named keys, KBD, CONSUMER and PS3 lines
 *     (a keyboard host keeps keyboard reports); the profile file; pair_host_on profile=ps3; the
 *     PIN commands; the status file's profile and pin fields */
#define TEST_DIR "/tmp/hng_bt_ps3_pin_test"
#include "bt_fake_btstack.h"

#include "../payload/source/hub_btstack/bt_state.c"
#include "../payload/source/hub_btstack/bt_hid.c"
#include "../payload/source/hub_btstack/bt_hid_queue.c"
#include "../payload/source/hub_btstack/bt_cmd.c"
#include "../payload/source/hub_btstack/bt_pairing.c"

#include <sys/stat.h>

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("[-] FAIL: " __VA_ARGS__); printf("\n"); s_fail = 1; } else { printf("  ok  " __VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t ADDR_A[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x01};
static const uint8_t ADDR_B[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x02};
static const uint8_t ADDR_C[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x03};

static void fire(btstack_timer_source_t *ts) {
    if (!ts->armed) return;
    ts->armed = false;
    ts->process(ts);
}

static void test_pin(void) {
    printf("[-] PIN: 0000 first, the host's code when it refused that\n");
    uint8_t a[6];
    CHECK(pin_on_request(ADDR_A, 0) == PIN_ACTION_DEFAULT, "a host's first PIN request gets 0000 at once");
    pin_on_link_key(ADDR_A);
    CHECK(!pin_on_disconnect(ADDR_A) && pin_state() == PIN_STATE_NONE, "a host that took 0000 (link key) is no refusal when it drops later");
    CHECK(pin_on_request(ADDR_A, 0) == PIN_ACTION_DEFAULT, "and is answered with 0000 again");
    pin_on_link_key(ADDR_A);

    CHECK(!pin_on_disconnect(ADDR_C), "a link that drops without a PIN request is no refusal");
    CHECK(pin_on_request(ADDR_B, 0) == PIN_ACTION_DEFAULT, "host B's first request gets 0000 too");
    CHECK(pin_on_disconnect(ADDR_B), "B drops the link without a link key: it refused 0000");
    CHECK(pin_state() == PIN_STATE_REQUIRED && memcmp(pin_state_addr(), ADDR_B, 6) == 0, "the state says B wants its own code (pair again)");
    CHECK(!pin_take_reply("1234", a), "a PIN sent before B asks again is not used");

    CHECK(pin_on_request(ADDR_B, 1000) == PIN_ACTION_WAIT, "B's next request is held");
    CHECK(pin_state() == PIN_STATE_WAITING, "and the state says a code is awaited");
    CHECK(!pin_take_reply("84a631", a) && !pin_take_reply("", a) && !pin_take_reply("12345678901234567", a),
          "letters, nothing and 17 digits are no PIN");
    CHECK(pin_take_reply("842631", a) && memcmp(a, ADDR_B, 6) == 0 && pin_state() == PIN_STATE_NONE,
          "six digits answer B's request");

    CHECK(pin_on_request(ADDR_B, 5000) == PIN_ACTION_WAIT, "B asks again later: held at once (no 0000 round)");
    CHECK(!pin_take_timeout(5000 + PIN_WAIT_MS - 1, a), "the wait has not run out before PIN_WAIT_MS");
    CHECK(pin_take_timeout(5000 + PIN_WAIT_MS, a) && memcmp(a, ADDR_B, 6) == 0, "after PIN_WAIT_MS the request is rejected");
    CHECK(pin_state() == PIN_STATE_REQUIRED, "and the user is told to pair again");

    CHECK(pin_on_request(ADDR_B, 6000) == PIN_ACTION_WAIT, "(B held again)");
    CHECK(!pin_on_disconnect(ADDR_B) && pin_state() == PIN_STATE_REQUIRED && !pin_take_reply("1234", a),
          "B drops the link while held: no code is taken any more, pair again");
    pin_on_request(ADDR_B, 0);
    CHECK(pin_take_cancel(a) && memcmp(a, ADDR_B, 6) == 0 && pin_state() == PIN_STATE_NONE, "cancel rejects the held request");
    CHECK(!pin_take_cancel(a), "a second cancel has nothing to reject");
    CHECK(pin_valid("0") && pin_valid("1234567890123456") && !pin_valid("12 4") && !pin_valid(NULL), "PIN: 1 to 16 digits");

    pin_on_request(ADDR_C, 0);
    pin_session_end();
    CHECK(!pin_on_disconnect(ADDR_C), "pairing mode ended: C's 0000 is forgotten, a later drop is no refusal");
    CHECK(pin_on_request(ADDR_C, 0) == PIN_ACTION_DEFAULT, "C asks in a new session: 0000 again");
    CHECK(pin_on_request(ADDR_C, 100) == PIN_ACTION_WAIT && pin_state() == PIN_STATE_WAITING,
          "C asks again on the same link without a link key: it refused 0000, its request is held");
    CHECK(pin_take_cancel(a), "(cancelled)");
    CHECK(pin_on_request(ADDR_C, 200) == PIN_ACTION_DEFAULT, "a cancelled entry: C gets 0000 first again");
    CHECK(pin_on_request(ADDR_B, 0) == PIN_ACTION_DEFAULT, "B's entry was cancelled before: 0000 first again too");
    CHECK(pin_on_disconnect(ADDR_B), "(B refuses 0000 again)");
    pin_session_end();
    CHECK(pin_on_request(ADDR_B, 0) == PIN_ACTION_WAIT, "B, which refused 0000 before, is asked for its code in the next session");
    pin_forget(ADDR_B);
    CHECK(pin_state() == PIN_STATE_NONE && pin_on_request(ADDR_B, 0) == PIN_ACTION_DEFAULT, "forgetting B drops its PIN state: 0000 first again");
    pin_session_end();
}

static void test_reports(void) {
    printf("[-] BD Remote reports (hid-sony.c layout)\n");
    uint8_t r[PS3_REPORT_LEN];
    ps3_report(ps3_key_by_name("PS"), r);
    static const uint8_t ps[PS3_REPORT_LEN] = {0xa1, 0x01, 0x00, 0x00, 0x01, 0x43, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x05};
    CHECK(memcmp(r, ps, sizeof(ps)) == 0, "PS: bitmap bit 16, code 0x43, pressed, battery 5");
    ps3_report(ps3_key_by_name("triangle"), r);
    CHECK(r[2] == 0 && r[3] == 0x10 && r[4] == 0 && r[5] == 0x5c, "Triangle: bitmap bit 12, code 0x5c (names in any case)");
    ps3_report(ps3_key_by_name("TopMenu"), r);
    CHECK(r[2] == 0 && r[3] == 0 && r[4] == 0 && r[5] == 0x1a, "Top Menu: a remote key, no bitmap bit");
    ps3_report(NULL, r);
    static const uint8_t rel[PS3_REPORT_LEN] = {0xa1, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x05};
    CHECK(memcmp(r, rel, sizeof(rel)) == 0, "release: no bit, code 0xff, not pressed");
    CHECK(ps3_remote_descriptor_len == 73, "the descriptor is the stock record's 73 bytes");

    CHECK(ps3_key_by_name("Number5") && ps3_key_by_name("Number5")->code == 0x04, "Number5 is the remote's 5 (code 0x04)");
    CHECK(ps3_key_by_name("Back") && ps3_key_by_name("Back")->code == 0x0e, "Back is Return");
    CHECK(ps3_key_by_name("Select") && ps3_key_by_name("Select")->code == 0x0b, "Select (OK) is Enter, not the gamepad Select");
    CHECK(ps3_key_by_name("VolumeUp") == NULL, "the remote has no volume key");
    CHECK(ps3_key_from_keyboard(0x52) && ps3_key_from_keyboard(0x52)->code == 0x54, "keyboard Up is the remote's Up");
    CHECK(ps3_key_from_keyboard(0x1e)->code == 0x00 && ps3_key_from_keyboard(0x27)->code == 0x09, "keyboard 1 and 0 are the remote's 1 and 0");
    CHECK(ps3_key_from_consumer(0x00cd)->code == 0x39 && ps3_key_from_consumer(0x0224)->code == 0x0e, "Play/Pause is Pause, AC Back is Return");
    CHECK(ps3_key_from_consumer(0x00e9) == NULL, "Volume Up has no BD Remote key");
}

static void setup_hosts(void) {
    memset(g_hosts, 0, sizeof(g_hosts));
    for (int i = 0; i < MAX_HOST_CONNS; i++) g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
    memcpy(g_hosts[0].addr, ADDR_A, 6); /* a PS3 */
    g_hosts[0].connected = true;
    g_hosts[0].hid_cid = 0x41;
    g_hosts[0].profile = BT_PROFILE_PS3;
    memcpy(g_hosts[1].addr, ADDR_B, 6); /* a keyboard host */
    g_hosts[1].connected = true;
    g_hosts[1].hid_cid = 0x42;
    g_active_host_idx = 0;
    g_host_pending_release_type = 0;
    g_host_pending_cid = 0;
    btstack_run_loop_remove_timer(&g_host_key_release_timer);
    memset(s_reports, 0, sizeof(s_reports));
    reset_calls();
}

static void fifo_lines(const char *text) {
    int fd = open(BTHID_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0 || write(fd, text, strlen(text)) < 0) s_fail = 1;
    if (fd >= 0) close(fd);
    poll_bthid_fifo();
}

static void test_hosts(void) {
    printf("[-] a PS3 host gets BD Remote reports\n");
    setup_hosts();
    fifo_lines("TARGET 00:11:22:33:44:01\nKEY Triangle\n");
    CHECK(s_reports[0] == 1 && s_last_report[0][1] == 0x01 && s_last_report[0][5] == 0x5c && s_last_report[0][11] == 0x01,
          "KEY Triangle: the remote's Triangle, pressed");
    fire(&g_host_key_release_timer);
    CHECK(s_reports[0] == 2 && s_last_report[0][5] == 0xff && s_last_report[0][11] == 0x00, "then the remote's release");
    fifo_lines("TARGET 00:11:22:33:44:01\nKBD 52\n");
    CHECK(s_reports[0] == 3 && s_last_report[0][5] == 0x54 && s_last_report[0][2] == 0x10, "KBD 52 (Up) becomes the remote's Up");
    fire(&g_host_key_release_timer);
    fifo_lines("TARGET 00:11:22:33:44:01\nKEY DirectionDown\n");
    CHECK(s_reports[0] == 5 && s_last_report[0][5] == 0x56, "KEY DirectionDown is the remote's Down");
    fire(&g_host_key_release_timer);
    int before = s_reports[0];
    fifo_lines("TARGET 00:11:22:33:44:01\nCONSUMER E9\n");
    CHECK(s_reports[0] == before, "CONSUMER E9 (Volume Up) sends nothing: no keyboard report to a PS3");
    fifo_lines("TARGET 00:11:22:33:44:01\nPS3_DOWN 43\n");
    CHECK(s_reports[0] == before + 1 && s_last_report[0][5] == 0x43 && s_last_report[0][4] == 0x01, "PS3_DOWN 43: the PS key, held");
    fifo_lines("TARGET 00:11:22:33:44:01\nRELEASE\n");
    CHECK(s_reports[0] == before + 2 && s_last_report[0][5] == 0xff, "RELEASE sends the remote's release");

    fifo_lines("TARGET 00:11:22:33:44:02\nKBD 52\n");
    CHECK(s_reports[1] == 1 && s_last_report[1][1] == 0x01 && s_last_report[1][4] == 0x52, "the keyboard host B still gets a keyboard report");
    fire(&g_host_key_release_timer);
    before = s_reports[1];
    fifo_lines("TARGET 00:11:22:33:44:02\nPS3 5C\n");
    CHECK(s_reports[1] == before, "PS3 5C to the keyboard host B sends nothing (no BD Remote report to a keyboard host)");

    printf("[-] the profile file\n");
    unlink(BT_PROFILES_FILE);
    CHECK(host_profile_load(ADDR_A) == BT_PROFILE_KEYBOARD, "no file: every host is a keyboard");
    host_profile_save(ADDR_A, BT_PROFILE_PS3);
    host_profile_save(ADDR_C, BT_PROFILE_PS3);
    CHECK(host_profile_load(ADDR_A) == BT_PROFILE_PS3 && host_profile_load(ADDR_C) == BT_PROFILE_PS3 &&
          host_profile_load(ADDR_B) == BT_PROFILE_KEYBOARD, "A and C saved as PS3, B is not in the file");
    host_profile_save(ADDR_A, BT_PROFILE_KEYBOARD);
    CHECK(host_profile_load(ADDR_A) == BT_PROFILE_KEYBOARD && host_profile_load(ADDR_C) == BT_PROFILE_PS3, "A back to the keyboard keeps C's line");
    memset(&g_hosts[2], 0, sizeof(g_hosts[2]));
    host_conn_t *c = register_host_target(ADDR_C, NULL);
    CHECK(c && c->profile == BT_PROFILE_PS3, "a host entered in the table takes its saved profile");
    forget_classic_host(ADDR_C);
    CHECK(host_profile_load(ADDR_C) == BT_PROFILE_KEYBOARD && access(BT_PROFILES_FILE, F_OK) != 0, "forgetting C removes its line (and the empty file)");

    printf("[-] pairing commands\n");
    setup_hosts();
    execute_btstack_cmd("pair_host_on profile=ps3 Harmony Keyboard");
    CHECK(g_host_pairing && g_pairing_profile == BT_PROFILE_PS3, "pair_host_on profile=ps3 pairs as the BD Remote");
    CHECK(strcmp(g_host_name, "Harmony Keyboard") == 0, "the name after the profile is still the keyboard's name");
    execute_btstack_cmd("pair_host_off");
    CHECK(!g_host_pairing && g_pairing_profile == BT_PROFILE_KEYBOARD, "pair_host_off goes back to the keyboard");
    execute_btstack_cmd("pair_host_on Living Room");
    CHECK(g_pairing_profile == BT_PROFILE_KEYBOARD && strcmp(g_host_name, "Living Room") == 0, "pair_host_on without a profile is the keyboard");
    s_now_ms += PAIRING_TIMEOUT_MS - 1000;
    g_pairing_timer.armed = false;
    pairing_timeout_restart();
    CHECK(g_pairing_timer.armed, "a refusal before the limit restarts the pairing timeout");
    s_now_ms += PAIRING_TIMEOUT_MS - 1000;
    g_pairing_timer.armed = false;
    pairing_timeout_restart();
    CHECK(!g_pairing_timer.armed, "a refusal that would run past PAIRING_MAX_MS does not restart it");
    execute_btstack_cmd("pair_host_off");
    execute_btstack_cmd("pin_reply 842631");
    CHECK(strcmp(s_pin_cmd, "pin_reply 842631") == 0, "pin_reply goes to the PIN handler");
    fifo_lines("pin_cancel\n");
    CHECK(strcmp(s_pin_cmd, "pin_cancel") == 0, "pin_cancel on the pipe too");

    printf("[-] the status file\n");
    setup_hosts();
    pin_on_request(ADDR_B, 0); /* 0000, refused, then held */
    pin_on_disconnect(ADDR_B);
    pin_on_request(ADDR_B, 0);
    update_status("ready");
    char st[4096] = "";
    FILE *f = fopen(STATUS_FILE, "r");
    size_t n = f ? fread(st, 1, sizeof(st) - 1, f) : 0;
    if (f) fclose(f);
    st[n] = '\0';
    cJSON *j = cJSON_Parse(st);
    CHECK(j != NULL, "the status file is JSON");
    cJSON *pin = cJSON_GetObjectItem(j, "pin");
    CHECK(pin && strcmp(cJSON_GetObjectItem(pin, "state")->valuestring, "waiting") == 0 &&
          strcmp(cJSON_GetObjectItem(pin, "addr")->valuestring, "00:11:22:33:44:02") == 0, "pin: waiting, for B");
    cJSON *hosts = cJSON_GetObjectItem(j, "hosts");
    CHECK(cJSON_GetArraySize(hosts) == 2 &&
          strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(hosts, 0), "profile")->valuestring, "ps3") == 0 &&
          strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(hosts, 1), "profile")->valuestring, "keyboard") == 0,
          "hosts carry their profile: A ps3, B keyboard");
    cJSON_Delete(j);
    pin_session_end();
}

int main(void) {
    if (system("rm -rf " TEST_DIR " && mkdir -p " TEST_DIR) != 0) return 1;
    if (mkfifo(BTHID_FIFO, 0600) != 0) {
        perror("mkfifo");
        return 1;
    }
    poll_bthid_fifo();
    test_pin();
    test_reports();
    test_hosts();
    if (g_bthid_fifo_fd >= 0) close(g_bthid_fifo_fd);
    if (system("rm -rf " TEST_DIR) != 0) return 1;
    if (s_fail) return 1;
    printf("[+] test_bt_ps3_pin: PIN pairing, BD Remote reports, PS3 hosts and the profile file\n");
    return 0;
}
