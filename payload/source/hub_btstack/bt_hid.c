#include "bt_hid.h"
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include "bt_hid_queue.h"
#include "bt_pairing.h"

btstack_timer_source_t g_host_key_release_timer;

/* Report queue (bt_hid_queue.c) on BTstack: the send, the "can send now" request, the log, and a
 * retry timer that runs while a report waits */
#define HIDQ_RETRY_MS 20
static btstack_timer_source_t s_hidq_timer;

static uint8_t hidq_btstack_send(uint16_t cid, const uint8_t *report, uint16_t len) {
    return hid_device_send_interrupt_message(cid, report, len);
}

static void hidq_timer_handler(btstack_timer_source_t *ts);

static void hidq_arm_timer(void) {
    btstack_run_loop_remove_timer(&s_hidq_timer);
    if (!hidq_pending_any()) return;
    btstack_run_loop_set_timer_handler(&s_hidq_timer, hidq_timer_handler);
    btstack_run_loop_set_timer(&s_hidq_timer, HIDQ_RETRY_MS);
    btstack_run_loop_add_timer(&s_hidq_timer);
}

static void hidq_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    hidq_service_all(btstack_run_loop_get_time_ms());
    hidq_arm_timer();
}

static bool s_hidq_ready;

void bt_hid_queue_init(void) {
    if (s_hidq_ready) return;
    hidq_init(hidq_btstack_send, hid_device_request_can_send_now_event, log_msg);
    s_hidq_ready = true;
}

/* ---- Key trace (debug logging) ----
 * hub_daemon sends "TS <rx> <daemon> <write>" before the command of a key from a remote: when
 * hub_rf received the key, when hub_daemon took it from the key channel and when it wrote it to
 * the pipe, all CLOCK_MONOTONIC in ms. Without that line nothing here runs. */
#ifndef KEY_TRACE_LOG
#define KEY_TRACE_LOG "/tmp/key_trace.log"
#endif
#define KEY_TRACE_MAX_BYTES 262144
static unsigned long long s_trace_rx_ms, s_trace_daemon_ms, s_trace_write_ms, s_trace_read_ms;
static char s_trace_cmd[32];

static unsigned long long trace_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static unsigned long long trace_span(unsigned long long from, unsigned long long to) {
    return to > from ? to - from : 0;
}

void bt_hid_trace_begin(const char *times) {
    s_trace_rx_ms = s_trace_daemon_ms = s_trace_write_ms = 0;
    s_trace_cmd[0] = '\0';
    if (!times || sscanf(times, "%llu %llu %llu", &s_trace_rx_ms, &s_trace_daemon_ms, &s_trace_write_ms) != 3) {
        s_trace_rx_ms = 0;
    }
}

void bt_hid_trace_command(const char *cmd) {
    if (!s_trace_rx_ms || s_trace_cmd[0]) return;
    snprintf(s_trace_cmd, sizeof(s_trace_cmd), "%s", cmd ? cmd : "");
    s_trace_read_ms = trace_now_ms();
}

/* One line per key from a remote: where its time went between hub_rf and the Bluetooth stack.
 *   rf>daemon  hub_rf's queue and the key channel, until hub_daemon took the key
 *   daemon     hub_daemon, until it wrote the command to the pipe
 *   pipe       until hub_btstack read the command
 *   bt         hub_btstack, until the report was handed to the stack (or queued behind another) */
static void trace_report_sent(uint16_t cid) {
    if (!s_trace_rx_ms || !s_trace_cmd[0]) return;
    unsigned long long now = trace_now_ms();
    struct stat st;
    if (stat(KEY_TRACE_LOG, &st) == 0 && st.st_size > KEY_TRACE_MAX_BYTES) rename(KEY_TRACE_LOG, KEY_TRACE_LOG ".1");
    FILE *f = fopen(KEY_TRACE_LOG, "a");
    if (f) {
        struct timespec wall;
        clock_gettime(CLOCK_REALTIME, &wall);
        struct tm tmv;
        localtime_r(&wall.tv_sec, &tmv);
        fprintf(f, "%02d:%02d:%02d.%03ld %-16s rf>daemon=%llu daemon=%llu pipe=%llu bt=%llu total=%llu ms%s\n",
                tmv.tm_hour, tmv.tm_min, tmv.tm_sec, wall.tv_nsec / 1000000L, s_trace_cmd,
                trace_span(s_trace_rx_ms, s_trace_daemon_ms), trace_span(s_trace_daemon_ms, s_trace_write_ms),
                trace_span(s_trace_write_ms, s_trace_read_ms), trace_span(s_trace_read_ms, now),
                trace_span(s_trace_rx_ms, now), hidq_pending(cid) ? " (waiting in the report queue)" : "");
        fclose(f);
    }
    s_trace_rx_ms = 0;
    s_trace_cmd[0] = '\0';
}

void bt_hid_send_report(uint16_t cid, const uint8_t *report, uint16_t len) {
    bt_hid_queue_init(); /* a report sent before main() set it up must not be lost */
    hidq_send(cid, report, len, btstack_run_loop_get_time_ms());
    hidq_arm_timer();
    trace_report_sent(cid);
}

void bt_hid_can_send_now(uint16_t cid) {
    hidq_service(cid, btstack_run_loop_get_time_ms());
    hidq_arm_timer();
}

void bt_hid_link_closed(uint16_t cid) {
    hidq_drop(cid);
    hidq_arm_timer();
}
uint8_t g_host_pending_release_type = 0; // 1 = keyboard, 2 = consumer, 3 = custom
uint8_t g_host_custom_release_buf[16];
int     g_host_custom_release_len = 0;
int     g_host_send_hold_mode = 0;
uint8_t g_host_mouse_buttons = 0;

static btstack_timer_source_t g_host_crelease_timer;
static btstack_timer_source_t g_host_mrelease_timer;
/* The HID channel the pending press went to. A release goes there and nowhere else: the old
 * code sent one release report to every connected host in a loop, and with a TV paired next to
 * the Shield the Shield's report (second in the loop) never went out, so its key stayed pressed
 * and repeated until the link dropped. One report per release, on the channel of the press. */
uint16_t g_host_pending_cid = 0;

static bool cid_connected(uint16_t cid) {
    if (cid == 0) return false;
    for (int i = 0; i < MAX_HOST_CONNS; i++) {
        if (g_hosts[i].connected && g_hosts[i].hid_cid == cid) return true;
    }
    return false;
}

/* Where a release belongs: the channel of the pending press while it is up, else the target */
static uint16_t release_cid(void) {
    if (cid_connected(g_host_pending_cid)) return g_host_pending_cid;
    return get_target_host_hid_cid();
}

/* The consumer and mouse release timers carry the channel they were armed for and leave
 * g_host_pending_cid alone: a press made while one of them is armed sets that to its own channel,
 * and clearing it here would send that press's release to the current target instead. */
static uint16_t s_crelease_cid = 0;
static uint16_t s_mrelease_cid = 0;

static uint16_t timer_release_cid(uint16_t armed_cid) {
    if (cid_connected(armed_cid)) return armed_cid;
    return get_target_host_hid_cid();
}

static void host_crelease_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    uint16_t cid = timer_release_cid(s_crelease_cid);
    s_crelease_cid = 0;
    if (cid != 0) {
        uint8_t crelease[4] = {0xa1, 0x02, 0, 0};
        bt_hid_send_report(cid, crelease, sizeof(crelease));
    }
}

static void host_mrelease_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    uint16_t cid = timer_release_cid(s_mrelease_cid);
    s_mrelease_cid = 0;
    if (cid != 0) {
        uint8_t mrelease[6] = {0xa1, 0x03, 0, 0, 0, 0};
        bt_hid_send_report(cid, mrelease, sizeof(mrelease));
    }
    g_host_mouse_buttons = 0;
}

static void host_key_release_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    send_host_release_all();
}

/* (Re)arms the key release timer, always with its handler: a raw report from the FIFO can be
 * the first key since start, before any other path set the handler, and BTstack calls the
 * handler of a due timer without checking it */
void arm_host_key_release_timer(uint32_t timeout_ms) {
    btstack_run_loop_remove_timer(&g_host_key_release_timer);
    btstack_run_loop_set_timer_handler(&g_host_key_release_timer, host_key_release_timer_handler);
    btstack_run_loop_set_timer(&g_host_key_release_timer, timeout_ms);
    btstack_run_loop_add_timer(&g_host_key_release_timer);
}

void send_host_release_all(void) {
    uint8_t prev_type = g_host_pending_release_type;
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        g_host_pending_release_type = 0;
    }
    uint16_t cid = release_cid();
    g_host_pending_cid = 0;
    if (cid == 0) {
        /* No reachable host: a host whose link dropped released the keys itself */
        g_host_mouse_buttons = 0;
        return;
    }
    /* One report now; a second kind of release goes out on a timer, never back to back */
    if (prev_type == 2) {
        uint8_t crelease[4] = {0xa1, 0x02, 0, 0};
        bt_hid_send_report(cid, crelease, sizeof(crelease));
    } else if (prev_type == 3 && g_host_custom_release_len > 0) {
        bt_hid_send_report(cid, g_host_custom_release_buf, g_host_custom_release_len);
    } else if (prev_type == 4) {
        uint8_t mrelease[6] = {0xa1, 0x03, 0, 0, 0, 0};
        bt_hid_send_report(cid, mrelease, sizeof(mrelease));
        g_host_mouse_buttons = 0;
    } else {
        /* keyboard (type 1), or unknown: keyboard release now, consumer release 20 ms later */
        uint8_t release[10] = {0xa1, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
        bt_hid_send_report(cid, release, sizeof(release));
        if (prev_type != 1) {
            s_crelease_cid = cid;
            btstack_run_loop_remove_timer(&g_host_crelease_timer);
            btstack_run_loop_set_timer_handler(&g_host_crelease_timer, host_crelease_timer_handler);
            btstack_run_loop_set_timer(&g_host_crelease_timer, 20);
            btstack_run_loop_add_timer(&g_host_crelease_timer);
        }
    }
    if (g_host_mouse_buttons != 0) {
        s_mrelease_cid = cid;
        btstack_run_loop_remove_timer(&g_host_mrelease_timer);
        btstack_run_loop_set_timer_handler(&g_host_mrelease_timer, host_mrelease_timer_handler);
        btstack_run_loop_set_timer(&g_host_mrelease_timer, 40);
        btstack_run_loop_add_timer(&g_host_mrelease_timer);
    }
}

/* The target host's link is down: keep the key (when there is one) for when it opens, and start a
 * reconnect. A target that could not be resolved gets neither: its keys are dropped. */
static void host_link_down(const char *wake_key) {
    if (g_active_host_idx == BT_HOST_IDX_NONE) {
        hub_bt_log_debug("Host key dropped: the target host is unknown");
        return;
    }
    if (wake_key) wake_key_keep(wake_key);
    if (!g_host_reconnect_active) {
        bd_addr_t tgt;
        if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && !is_null_bd_addr(g_hosts[g_active_host_idx].addr)) {
            start_host_reconnect(g_hosts[g_active_host_idx].addr, 20);
        } else if (get_pinned_host_target(tgt)) {
            start_host_reconnect(tgt, 20);
        } else {
            try_connect_host();
        }
    }
}

/* A mouse button held with no later mouse report (MOUSE_DOWN, then the client went away) is
 * released after this long, like a held key; every mouse report starts the time again */
#define HOST_MOUSE_HOLD_MS 10000

void send_host_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) {
        if (g_active_host_idx == BT_HOST_IDX_NONE) return;
        if (!g_host_reconnect_active) {
            bd_addr_t tgt;
            if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && !is_null_bd_addr(g_hosts[g_active_host_idx].addr)) {
                start_host_reconnect(g_hosts[g_active_host_idx].addr, 20);
            } else if (get_pinned_host_target(tgt)) {
                start_host_reconnect(tgt, 20);
            } else {
                try_connect_host();
            }
        }
        return;
    }
    uint8_t report[6] = {0xa1, 0x03, buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};
    bt_hid_send_report(cid, report, sizeof(report));
    if (buttons) g_host_pending_cid = cid;
    g_keys_sent++;
    update_bthid_status();
    btstack_run_loop_remove_timer(&g_host_mrelease_timer);
    if (buttons) {
        s_mrelease_cid = cid;
        btstack_run_loop_set_timer_handler(&g_host_mrelease_timer, host_mrelease_timer_handler);
        btstack_run_loop_set_timer(&g_host_mrelease_timer, HOST_MOUSE_HOLD_MS);
        btstack_run_loop_add_timer(&g_host_mrelease_timer);
    }
}

void send_host_mouse_click(uint8_t button) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) return;
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        host_key_release_timer_handler(NULL);
    }
    g_host_mouse_buttons |= button;
    send_host_mouse_report(g_host_mouse_buttons, 0, 0, 0);
    g_host_pending_cid = cid;
    g_host_pending_release_type = 4;
    arm_host_key_release_timer(40);
}

void send_host_keyboard_report(uint8_t modifier, uint8_t keycode) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) {
        char wake[24];
        snprintf(wake, sizeof(wake), "KEY %02x %02x", modifier, keycode);
        host_link_down(wake);
        return;
    }
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        host_key_release_timer_handler(NULL);
    }
    uint8_t press[10] = {0xa1, 0x01, modifier, 0, keycode, 0, 0, 0, 0, 0};
    bt_hid_send_report(cid, press, sizeof(press));
    g_host_pending_cid = cid;
    g_host_pending_release_type = 1;
    arm_host_key_release_timer(g_host_send_hold_mode ? 10000 : 40);
    g_keys_sent++;
    update_bthid_status();
}

void send_host_consumer_report(uint16_t usage_id) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) {
        char wake[24];
        snprintf(wake, sizeof(wake), "CONSUMER %04x", usage_id);
        host_link_down(wake);
        return;
    }
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        host_key_release_timer_handler(NULL);
    }
    uint8_t press[4] = {0xa1, 0x02, (uint8_t)(usage_id & 0xff), (uint8_t)((usage_id >> 8) & 0xff)};
    bt_hid_send_report(cid, press, sizeof(press));
    g_host_pending_cid = cid;
    g_host_pending_release_type = 2;
    arm_host_key_release_timer(g_host_send_hold_mode ? 10000 : 40);
    g_keys_sent++;
    update_bthid_status();
}

int parse_hex_bytes(const char *hex, uint8_t *out, int max_len) {
    int len = 0;
    while (*hex && len < max_len) {
        while (*hex == ' ') hex++;
        if (!*hex || *hex == '|' || *hex == '\r' || *hex == '\n') break;
        /* Two digits per byte: "%02x" also takes a single digit, and stepping 2 from there went
           past the end of the string */
        unsigned int b = 0;
        if (isxdigit((unsigned char)hex[0]) && isxdigit((unsigned char)hex[1]) && sscanf(hex, "%02x", &b) == 1) {
            out[len++] = (uint8_t)b;
            hex += 2;
        } else {
            break;
        }
    }
    return len;
}

bool send_host_named_key(const char *name) {
    if (!name || !name[0]) return false;
    char msg[128];
    snprintf(msg, sizeof(msg), "Host key send: %s", name);
    hub_bt_log_debug(msg);

    if (strcasecmp(name, "release_all") == 0 || strcasecmp(name, "release") == 0) {
        send_host_release_all();
        return true;
    }

    uint8_t mod = 0;
    const char *p = name;
    while (*p) {
        if (strncasecmp(p, "ctrl+", 5) == 0) { mod |= 0x01; p += 5; }
        else if (strncasecmp(p, "ctrl", 4) == 0 && strlen(p) > 4) { mod |= 0x01; p += 4; }
        else if (strncasecmp(p, "shift+", 6) == 0) { mod |= 0x02; p += 6; }
        else if (strncasecmp(p, "shift", 5) == 0 && strlen(p) > 5) { mod |= 0x02; p += 5; }
        else if (strncasecmp(p, "alt+", 4) == 0) { mod |= 0x04; p += 4; }
        else if (strncasecmp(p, "alt", 3) == 0 && strlen(p) > 3) { mod |= 0x04; p += 3; }
        else if (strncasecmp(p, "win+", 4) == 0 || strncasecmp(p, "gui+", 4) == 0 || strncasecmp(p, "cmd+", 4) == 0) { mod |= 0x08; p += 4; }
        else if ((strncasecmp(p, "win", 3) == 0 || strncasecmp(p, "gui", 3) == 0 || strncasecmp(p, "cmd", 3) == 0) && strlen(p) > 3) { mod |= 0x08; p += 3; }
        else break;
    }
    name = p;

    // D-Pad / Navigation
    if (strcasecmp(name, "up") == 0 || strcasecmp(name, "DirectionUp") == 0 || strcasecmp(name, "ArrowUp") == 0) { send_host_keyboard_report(mod, 0x52); return true; }
    if (strcasecmp(name, "down") == 0 || strcasecmp(name, "DirectionDown") == 0 || strcasecmp(name, "ArrowDown") == 0) { send_host_keyboard_report(mod, 0x51); return true; }
    if (strcasecmp(name, "left") == 0 || strcasecmp(name, "DirectionLeft") == 0 || strcasecmp(name, "ArrowLeft") == 0) { send_host_keyboard_report(mod, 0x50); return true; }
    if (strcasecmp(name, "right") == 0 || strcasecmp(name, "DirectionRight") == 0 || strcasecmp(name, "ArrowRight") == 0) { send_host_keyboard_report(mod, 0x4f); return true; }
    if (strcasecmp(name, "select") == 0 || strcasecmp(name, "enter") == 0 || strcasecmp(name, "OK") == 0 || strcasecmp(name, "Enter") == 0 || strcasecmp(name, "Select") == 0) { send_host_keyboard_report(mod, 0x28); return true; }
    if (strcasecmp(name, "back") == 0 || strcasecmp(name, "Return") == 0 || strcasecmp(name, "Exit") == 0 || strcasecmp(name, "Back") == 0) { send_host_consumer_report(0x0224); return true; } // Android Back
    if (strcasecmp(name, "home") == 0 || strcasecmp(name, "Home") == 0) { send_host_consumer_report(0x0223); return true; } // Android Home
    if (strcasecmp(name, "menu") == 0 || strcasecmp(name, "Menu") == 0 || strcasecmp(name, "settings") == 0 || strcasecmp(name, "Settings") == 0 || strcasecmp(name, "guide") == 0 || strcasecmp(name, "Guide") == 0) { send_host_consumer_report(0x008D); return true; }
    if (strcasecmp(name, "info") == 0 || strcasecmp(name, "Info") == 0) { send_host_consumer_report(0x01BD); return true; }

    // Media Controls
    if (strcasecmp(name, "play") == 0 || strcasecmp(name, "Play") == 0) { send_host_consumer_report(0x00B0); return true; }
    if (strcasecmp(name, "pause") == 0 || strcasecmp(name, "Pause") == 0) { send_host_consumer_report(0x00B1); return true; }
    if (strcasecmp(name, "play_pause") == 0 || strcasecmp(name, "PlayPause") == 0) { send_host_consumer_report(0x00CD); return true; }
    if (strcasecmp(name, "stop") == 0 || strcasecmp(name, "Stop") == 0) { send_host_consumer_report(0x00B7); return true; }
    if (strcasecmp(name, "next") == 0 || strcasecmp(name, "SkipForward") == 0 || strcasecmp(name, "NextTrack") == 0) { send_host_consumer_report(0x00B5); return true; }
    if (strcasecmp(name, "prev") == 0 || strcasecmp(name, "SkipBack") == 0 || strcasecmp(name, "PreviousTrack") == 0) { send_host_consumer_report(0x00B6); return true; }
    if (strcasecmp(name, "rewind") == 0 || strcasecmp(name, "Rewind") == 0) { send_host_consumer_report(0x00B4); return true; }
    if (strcasecmp(name, "fastforward") == 0 || strcasecmp(name, "FastForward") == 0) { send_host_consumer_report(0x00B3); return true; }

    // Audio & Power & Channels
    if (strcasecmp(name, "vol_up") == 0 || strcasecmp(name, "VolumeUp") == 0) { send_host_consumer_report(0x00E9); return true; }
    if (strcasecmp(name, "vol_down") == 0 || strcasecmp(name, "VolumeDown") == 0) { send_host_consumer_report(0x00EA); return true; }
    if (strcasecmp(name, "mute") == 0 || strcasecmp(name, "Mute") == 0) { send_host_consumer_report(0x00E2); return true; }
    if (strcasecmp(name, "power") == 0 || strcasecmp(name, "PowerToggle") == 0) { send_host_consumer_report(0x0030); return true; }
    if (strcasecmp(name, "ch_up") == 0 || strcasecmp(name, "ChannelUp") == 0) { send_host_consumer_report(0x009C); return true; }
    if (strcasecmp(name, "ch_down") == 0 || strcasecmp(name, "ChannelDown") == 0) { send_host_consumer_report(0x009D); return true; }

    // Color Keys (Android TV standard / Red, Green, Yellow, Blue)
    if (strcasecmp(name, "red") == 0) { send_host_consumer_report(0x0069); return true; }
    if (strcasecmp(name, "green") == 0) { send_host_consumer_report(0x006A); return true; }
    if (strcasecmp(name, "yellow") == 0) { send_host_consumer_report(0x006C); return true; }
    if (strcasecmp(name, "blue") == 0) { send_host_consumer_report(0x006B); return true; }

    // Digits
    if (strcmp(name, "0") == 0 || strcasecmp(name, "Number0") == 0) { send_host_keyboard_report(mod, 0x27); return true; }
    if (strcmp(name, "1") == 0 || strcasecmp(name, "Number1") == 0) { send_host_keyboard_report(mod, 0x1E); return true; }
    if (strcmp(name, "2") == 0 || strcasecmp(name, "Number2") == 0) { send_host_keyboard_report(mod, 0x1F); return true; }
    if (strcmp(name, "3") == 0 || strcasecmp(name, "Number3") == 0) { send_host_keyboard_report(mod, 0x20); return true; }
    if (strcmp(name, "4") == 0 || strcasecmp(name, "Number4") == 0) { send_host_keyboard_report(mod, 0x21); return true; }
    if (strcmp(name, "5") == 0 || strcasecmp(name, "Number5") == 0) { send_host_keyboard_report(mod, 0x22); return true; }
    if (strcmp(name, "6") == 0 || strcasecmp(name, "Number6") == 0) { send_host_keyboard_report(mod, 0x23); return true; }
    if (strcmp(name, "7") == 0 || strcasecmp(name, "Number7") == 0) { send_host_keyboard_report(mod, 0x24); return true; }
    if (strcmp(name, "8") == 0 || strcasecmp(name, "Number8") == 0) { send_host_keyboard_report(mod, 0x25); return true; }
    if (strcmp(name, "9") == 0 || strcasecmp(name, "Number9") == 0) { send_host_keyboard_report(mod, 0x26); return true; }

    // Keyboard keys
    if (strcasecmp(name, "escape") == 0 || strcasecmp(name, "esc") == 0) { send_host_keyboard_report(mod, 0x29); return true; }
    if (strcasecmp(name, "backspace") == 0) { send_host_keyboard_report(mod, 0x2a); return true; }
    if (strcasecmp(name, "tab") == 0) { send_host_keyboard_report(mod, 0x2b); return true; }
    if (strcasecmp(name, "space") == 0) { send_host_keyboard_report(mod, 0x2c); return true; }
    if (strcasecmp(name, "insert") == 0) { send_host_keyboard_report(mod, 0x49); return true; }
    if (strcasecmp(name, "delete") == 0) { send_host_keyboard_report(mod, 0x4c); return true; }
    if (strcasecmp(name, "pageup") == 0) { send_host_keyboard_report(mod, 0x4b); return true; }
    if (strcasecmp(name, "pagedown") == 0) { send_host_keyboard_report(mod, 0x4e); return true; }
    if (strcasecmp(name, "end") == 0) { send_host_keyboard_report(mod, 0x4d); return true; }
    if (strcasecmp(name, "capslock") == 0) { send_host_keyboard_report(mod, 0x39); return true; }

    // Symbols & Punctuation
    if (strcasecmp(name, "minus") == 0) { send_host_keyboard_report(mod, 0x2d); return true; }
    if (strcasecmp(name, "equal") == 0) { send_host_keyboard_report(mod, 0x2e); return true; }
    if (strcasecmp(name, "leftbracket") == 0) { send_host_keyboard_report(mod, 0x2f); return true; }
    if (strcasecmp(name, "rightbracket") == 0) { send_host_keyboard_report(mod, 0x30); return true; }
    if (strcasecmp(name, "backslash") == 0) { send_host_keyboard_report(mod, 0x31); return true; }
    if (strcasecmp(name, "semicolon") == 0) { send_host_keyboard_report(mod, 0x33); return true; }
    if (strcasecmp(name, "apostrophe") == 0) { send_host_keyboard_report(mod, 0x34); return true; }
    if (strcasecmp(name, "grave") == 0) { send_host_keyboard_report(mod, 0x35); return true; }
    if (strcasecmp(name, "comma") == 0) { send_host_keyboard_report(mod, 0x36); return true; }
    if (strcasecmp(name, "period") == 0) { send_host_keyboard_report(mod, 0x37); return true; }
    if (strcasecmp(name, "slash") == 0) { send_host_keyboard_report(mod, 0x38); return true; }

    // Function keys F1-F12
    if ((name[0] == 'f' || name[0] == 'F') && strlen(name) <= 3) {
        int fnum = atoi(name + 1);
        if (fnum >= 1 && fnum <= 12) {
            send_host_keyboard_report(mod, (uint8_t)(0x3a + (fnum - 1)));
            return true;
        }
    }

    // Single character
    if (strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'z') { send_host_keyboard_report(mod, 0x04 + (name[0] - 'a')); return true; }
    if (strlen(name) == 1 && name[0] >= 'A' && name[0] <= 'Z') { send_host_keyboard_report(mod | 0x02, 0x04 + (name[0] - 'A')); return true; }

    /* A consumer usage given as a number: only a complete "0x..." value. "%x" alone took any name
       that starts with a hex digit ("Eject", "Connect") as a usage and reported it as handled. */
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X') && isxdigit((unsigned char)name[2])) {
        char *end = NULL;
        unsigned long usage = strtoul(name + 2, &end, 16);
        if (end && *end == '\0' && usage <= 0xFFFF) {
            send_host_consumer_report((uint16_t)usage);
            return true;
        }
    }
    return false;
}

bool send_host_named_key_ex(const char *name, int hold_mode) {
    g_host_send_hold_mode = hold_mode;
    bool ret = send_host_named_key(name);
    g_host_send_hold_mode = 0;
    return ret;
}

/* Text is typed from a run loop timer, one character every HOST_TEXT_CHAR_MS, each on the host
 * that was the target when the text arrived. It used to be typed in one loop with a 25 ms sleep
 * per character: the single hub_btstack thread then served no HCI event, no timer and no other
 * command until the whole text was typed (about 25 s for 1000 characters). */
#define HOST_TEXT_CHAR_MS 25
#define HOST_TEXT_MAX 1024
static btstack_timer_source_t s_text_timer;
static char s_text_chars[HOST_TEXT_MAX];
static int8_t s_text_host[HOST_TEXT_MAX];
static size_t s_text_len = 0, s_text_pos = 0;

static bool text_char_key(unsigned char c, uint8_t *mod, uint8_t *keycode) {
    *mod = 0;
    *keycode = 0;
    if (c >= 'a' && c <= 'z') { *keycode = 0x04 + (c - 'a'); }
    else if (c >= 'A' && c <= 'Z') { *mod = 0x02; *keycode = 0x04 + (c - 'A'); }
    else if (c >= '1' && c <= '9') { *keycode = 0x1E + (c - '1'); }
    else if (c == '0') { *keycode = 0x27; }
    else if (c == ' ') { *keycode = 0x2c; }
    else if (c == '\n' || c == '\r') { *keycode = 0x28; }
    else if (c == '\t') { *keycode = 0x2b; }
    else if (c == '-') { *keycode = 0x2d; }
    else if (c == '_') { *mod = 0x02; *keycode = 0x2d; }
    else if (c == '=') { *keycode = 0x2e; }
    else if (c == '+') { *mod = 0x02; *keycode = 0x2e; }
    else if (c == '.') { *keycode = 0x37; }
    else if (c == '/') { *keycode = 0x38; }
    else if (c == ':') { *mod = 0x02; *keycode = 0x33; }
    else if (c == ';') { *keycode = 0x33; }
    else if (c == ',') { *keycode = 0x36; }
    else if (c == '<') { *mod = 0x02; *keycode = 0x36; }
    else if (c == '>') { *mod = 0x02; *keycode = 0x37; }
    else if (c == '?') { *mod = 0x02; *keycode = 0x38; }
    else if (c == '!') { *mod = 0x02; *keycode = 0x1e; }
    else if (c == '@') { *mod = 0x02; *keycode = 0x1f; }
    else if (c == '#') { *mod = 0x02; *keycode = 0x20; }
    else if (c == '$') { *mod = 0x02; *keycode = 0x21; }
    else if (c == '%') { *mod = 0x02; *keycode = 0x22; }
    return *keycode != 0;
}

/* Types the next character that has a key; true when one was sent */
static bool text_type_next(void) {
    while (s_text_pos < s_text_len) {
        uint8_t mod, keycode;
        unsigned char c = (unsigned char)s_text_chars[s_text_pos];
        int host = s_text_host[s_text_pos];
        s_text_pos++;
        if (!text_char_key(c, &mod, &keycode)) continue;
        /* the host of this text, not whatever host a later TARGET line chose */
        int saved = g_active_host_idx;
        g_active_host_idx = host;
        bool up = get_target_host_hid_cid() != 0;
        if (up) send_host_keyboard_report(mod, keycode);
        g_active_host_idx = saved;
        if (up) return true;
    }
    return false;
}

static void text_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (text_type_next() && s_text_pos < s_text_len) {
        btstack_run_loop_set_timer_handler(&s_text_timer, text_timer_handler);
        btstack_run_loop_set_timer(&s_text_timer, HOST_TEXT_CHAR_MS);
        btstack_run_loop_add_timer(&s_text_timer);
        return;
    }
    s_text_len = s_text_pos = 0;
}

void send_host_text(const char *text) {
    if (!text || get_target_host_hid_cid() == 0) return;
    bool busy = s_text_pos < s_text_len;
    if (!busy) s_text_len = s_text_pos = 0;
    else if (s_text_pos > 0) {
        /* move what is still to be typed to the front: the characters already typed used to
         * keep their room, so a second text was cut although the queue was nearly empty */
        memmove(s_text_chars, s_text_chars + s_text_pos, s_text_len - s_text_pos);
        memmove(s_text_host, s_text_host + s_text_pos, s_text_len - s_text_pos);
        s_text_len -= s_text_pos;
        s_text_pos = 0;
    }
    size_t n = strlen(text), added = 0;
    while (added < n && s_text_len < HOST_TEXT_MAX) {
        s_text_chars[s_text_len] = text[added++];
        s_text_host[s_text_len++] = (int8_t)g_active_host_idx;
    }
    if (added < n) hub_bt_log_debug("Host text: too much text waiting to be typed, the rest is dropped");
    if (busy) return; /* the timer types it after the text before */
    text_timer_handler(&s_text_timer);
}
