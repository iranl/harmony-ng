/* hub_btstack bt_hid.c on the host, with BTstack and the rest of hub_btstack replaced by stubs
 * (their header guards are defined first, so the real headers are skipped). Timers do not run by
 * themselves: the test fires them in the order the run loop would.
 *
 * A release with nothing pending sends the keyboard release and arms the consumer release timer
 * (20 ms); with mouse buttons down it also arms the mouse release timer (40 ms). Both handlers
 * cleared the shared "channel of the pending press", also when a newer press had set it in the
 * meantime: that press's release then went to whatever host was the target by then, and the host
 * that got the press kept the key down. */
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
    bool armed;
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

#define HOST_A 0x41
#define HOST_B 0x42
static uint16_t s_target = HOST_A;

/* What each host saw last: keyboard key byte (report a1 01 mod 00 key ...) */
static int s_kbd_down[2];

void btstack_run_loop_add_timer(btstack_timer_source_t *ts) { ts->armed = true; }
int btstack_run_loop_remove_timer(btstack_timer_source_t *ts) { ts->armed = false; return 0; }
void btstack_run_loop_set_timer(btstack_timer_source_t *ts, uint32_t ms) { ts->timeout = ms; }
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *ts, void (*h)(btstack_timer_source_t *)) { ts->process = h; }
void hid_device_send_interrupt_message(uint16_t cid, const uint8_t *msg, uint16_t len) {
    int h = cid == HOST_A ? 0 : cid == HOST_B ? 1 : -1;
    if (h < 0 || len < 5) return;
    if (msg[1] == 0x01) s_kbd_down[h] = msg[4];
}
int is_null_bd_addr(const bd_addr_t addr) { UNUSED(addr); return 1; }
int get_pinned_host_target(bd_addr_t addr) { UNUSED(addr); return 0; }
uint16_t get_target_host_hid_cid(void) { return s_target; }
void start_host_reconnect(const bd_addr_t target, int max_attempts) { UNUSED(target); UNUSED(max_attempts); }
void try_connect_host(void) {}
void update_bthid_status(void) {}
void hub_bt_log_debug(const char *msg) { UNUSED(msg); }

#include "../payload/source/hub_btstack/bt_hid.c"

static void fire(btstack_timer_source_t *ts) {
    if (!ts->armed) return;
    ts->armed = false;
    ts->process(ts);
}

static int run_case(const char *label, bool mouse_button_down) {
    memset(s_kbd_down, 0, sizeof(s_kbd_down));
    g_host_pending_release_type = 0;
    g_host_pending_cid = 0;
    g_host_mouse_buttons = mouse_button_down ? 0x01 : 0;
    s_target = HOST_A;

    send_host_release_all();            /* RELEASE with nothing pending: consumer (and mouse) release armed */
    send_host_named_key("up");          /* press on A within 20 ms */
    fire(&g_host_crelease_timer);       /* t = 20 ms */
    s_target = HOST_B;                  /* TARGET B */
    fire(&g_host_mrelease_timer);       /* t = 40 ms */
    fire(&g_host_key_release_timer);    /* t = 40 ms: release of the press on A */

    if (s_kbd_down[0] != 0) {
        printf("[-] FAIL (%s): host A still has key 0x%02x down, its release went to the other host\n", label, s_kbd_down[0]);
        return 1;
    }
    return 0;
}

int main(void) {
    g_hosts[0].hid_cid = HOST_A;
    g_hosts[0].connected = true;
    g_hosts[1].hid_cid = HOST_B;
    g_hosts[1].connected = true;

    int fail = 0;
    fail |= run_case("consumer release timer", false);
    fail |= run_case("mouse release timer", true);
    if (fail) return 1;
    printf("[+] test_bt_release_timers: a key's release goes to the host that got the press\n");
    return 0;
}
