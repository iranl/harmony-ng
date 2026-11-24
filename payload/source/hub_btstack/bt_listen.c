#include "bt_listen.h"
#include "bt_listen_report.h"
#include "bt_pairing.h"
#include "bt_le_pair_window.h"
#include "plugin_mgr.h"

#define ADV_LISTEN_MAX_S 60
#define ADV_LISTEN_REMOTE_HOLD_MS 15000

bool g_adv_listen_active = false;

static char g_listen_plugin[64];
static uint8_t g_listen_filter[ADV_LISTEN_FILTER_MAX];
static int g_listen_filter_len = 0;
static int g_listen_sock = -1;
static btstack_timer_source_t g_listen_timer;
/* Ended for the remote: no listen while set (a flag and a one-shot timer, not a time compare:
 * the run loop clock is 32-bit milliseconds and wraps after 49.7 days) */
static bool g_listen_hold;
static btstack_timer_source_t g_listen_hold_timer;
/* The remote ended a listen and then was not ready once during the whole hold (bond lost, or
 * another device sends from its address): its reports end no listen until it is ready, and for
 * one listen at most (its end, or ADV_LISTEN_MAX_S of it with renewals), so a remote that never
 * connects still ends every second listen */
static bool g_listen_ignore_remote;
static btstack_timer_source_t g_listen_ignore_timer;
/* The remote was ready at some point since the hold began */
static bool g_listen_hold_ready;

static void listen_ignore_clear(void) {
    g_listen_ignore_remote = false;
    btstack_run_loop_remove_timer(&g_listen_ignore_timer);
}

static void listen_ignore_end(btstack_timer_source_t *ts) {
    UNUSED(ts);
    g_listen_ignore_remote = false;
}

/* Starts the one-listen limit of the flag when a listen runs (a renewal does not restart it) */
static void listen_ignore_arm(void) {
    if (!g_listen_ignore_remote || !g_adv_listen_active) return;
    btstack_run_loop_remove_timer(&g_listen_ignore_timer);
    btstack_run_loop_set_timer_handler(&g_listen_ignore_timer, &listen_ignore_end);
    btstack_run_loop_set_timer(&g_listen_ignore_timer, ADV_LISTEN_MAX_S * 1000);
    btstack_run_loop_add_timer(&g_listen_ignore_timer);
}

static void listen_hold_end(btstack_timer_source_t *ts) {
    UNUSED(ts);
    g_listen_hold = false;
    if (g_listen_hold_ready || app_state == APP_READY) return;
    g_listen_ignore_remote = true;
    listen_ignore_arm();
}

void bt_listen_remote_ready(void) {
    g_listen_hold_ready = true;
    listen_ignore_clear();
}

static void listen_stop(const char *why) {
    if (!g_adv_listen_active) return;
    g_adv_listen_active = false;
    btstack_run_loop_remove_timer(&g_listen_timer);
    listen_ignore_clear();
    if (g_listen_sock >= 0) {
        close(g_listen_sock);
        g_listen_sock = -1;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "BLE listen for plugin '%s' ended (%s)", g_listen_plugin, why);
    log_msg(msg);
    /* A user scan keeps the scan; otherwise it goes back to the remote reconnect (none while the
     * remote's link is up: encrypting counts, not only READY) */
    if (g_user_scan_active) return;
    gap_stop_scan();
    if (connection_handle == HCI_CON_HANDLE_INVALID) hog_start_connect();
}

void bt_listen_end(const char *why) {
    listen_stop(why);
}

void bt_listen_end_for_remote(void) {
    if (!g_adv_listen_active || g_listen_ignore_remote) return;
    g_listen_hold = true;
    g_listen_hold_ready = false;
    btstack_run_loop_remove_timer(&g_listen_hold_timer);
    btstack_run_loop_set_timer_handler(&g_listen_hold_timer, &listen_hold_end);
    btstack_run_loop_set_timer(&g_listen_hold_timer, ADV_LISTEN_REMOTE_HOLD_MS);
    btstack_run_loop_add_timer(&g_listen_hold_timer);
    listen_stop("the remote wants to connect");
}

bool bt_listen_remote_may_end(void) {
    return !g_listen_ignore_remote;
}

static void listen_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    listen_stop("time up");
}

void bt_listen_handle_cmd(const char *args) {
    char id[64] = "", hex[2 * ADV_LISTEN_FILTER_MAX + 2] = "";
    int secs = -1;
    if (!args || sscanf(args, "%63s %d %33s", id, &secs, hex) < 2 || !plugin_mgr_is_safe_id(id)) {
        log_msg("adv_listen: usage adv_listen <plugin_id> <seconds> [hex_filter], ignored");
        return;
    }
    if (secs <= 0) {
        if (g_adv_listen_active && strcmp(id, g_listen_plugin) == 0) listen_stop("stopped by the plugin");
        return;
    }
    uint8_t flt[ADV_LISTEN_FILTER_MAX];
    int flt_len = adv_listen_parse_filter(hex, flt);
    if (flt_len < 0) {
        log_msg("adv_listen: the filter is not hex of at most 16 bytes, ignored");
        return;
    }
    if (g_user_scan_active || le_pair_window_allows(remote_device.addr, le_pair_now_s())) {
        log_msg("adv_listen: a remote scan or pairing runs, refused");
        return;
    }
    if (g_listen_hold && connection_handle == HCI_CON_HANDLE_INVALID) {
        log_msg("adv_listen: the remote is connecting, refused");
        return;
    }
    if (g_adv_listen_active && strcmp(id, g_listen_plugin) != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "adv_listen: plugin '%s' listens already, '%s' refused", g_listen_plugin, id);
        log_msg(msg);
        return;
    }
    if (secs > ADV_LISTEN_MAX_S) secs = ADV_LISTEN_MAX_S;

    if (!g_adv_listen_active) {
        g_listen_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (g_listen_sock < 0) {
            log_msg("adv_listen: no socket, refused");
            return;
        }
        fcntl(g_listen_sock, F_SETFD, FD_CLOEXEC);
    }
    snprintf(g_listen_plugin, sizeof(g_listen_plugin), "%s", id);
    memcpy(g_listen_filter, flt, (size_t)flt_len);
    g_listen_filter_len = flt_len;

    /* The reconnect may not take the scan meanwhile (hog_connect waits, see bt_pairing.c) */
    btstack_run_loop_remove_timer(&reconnect_timer);
    if (app_state == APP_CONNECTING) {
        gap_connect_cancel();
        app_state = APP_SCANNING;
    }
    if (!g_adv_listen_active) {
        g_adv_listen_active = true;
        listen_ignore_arm();
    }
    gap_set_scan_parameters(0, 96, 48); /* passive, 50 %: no scan requests; the plugin drops repeats */
    /* while a host is paged the scan stays paused: the page end starts it (bt_main.c) */
    if (!g_host_connecting) gap_start_scan();

    btstack_run_loop_remove_timer(&g_listen_timer);
    btstack_run_loop_set_timer_handler(&g_listen_timer, &listen_timer_handler);
    btstack_run_loop_set_timer(&g_listen_timer, (uint32_t)secs * 1000);
    btstack_run_loop_add_timer(&g_listen_timer);

    char msg[128];
    snprintf(msg, sizeof(msg), "BLE listen for plugin '%s': %d s, filter %d bytes", id, secs, flt_len);
    log_msg(msg);
}

void bt_listen_on_report(const uint8_t *packet) {
    if (!g_adv_listen_active || g_listen_sock < 0) return;
    const uint8_t *ad = gap_event_advertising_report_get_data(packet);
    int ad_len = gap_event_advertising_report_get_data_length(packet);
    if (!adv_listen_match(ad, ad_len, g_listen_filter, g_listen_filter_len)) return;

    bd_addr_t addr;
    gap_event_advertising_report_get_address(packet, addr);
    char msg[384];
    int n = adv_listen_format(msg, sizeof(msg), g_listen_plugin, addr,
                              gap_event_advertising_report_get_address_type(packet),
                              gap_event_advertising_report_get_advertising_event_type(packet),
                              (int8_t)gap_event_advertising_report_get_rssi(packet), ad, ad_len);
    if (n < 0) return;

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s/%s.sock", HARMONY_PLUGINS_RUN_DIR, g_listen_plugin);
    /* Never wait for a plugin: a full queue drops the report */
    sendto(g_listen_sock, msg, (size_t)n, MSG_DONTWAIT, (struct sockaddr *)&sun, sizeof(sun));
}
