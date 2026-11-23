/*
 * test_remote_activity_follow.c - An activity change from the Hub moves the remote to the
 * activity's screen only while it shows the home (activity list) or an activity screen. In
 * Settings, pairing or the device browser the page stays (selection, Suspend countdown included),
 * no progress bar covers it, and the remote's activity is updated so the new one is there when
 * the user goes back. A switch started on the remote itself still ends on its target screen.
 *
 * Frames go through handle_hub_frame_remote() as the RF thread would pass them; the progress bar
 * end is remote_show_switch_target(), which the main loop calls when it runs out.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "remote/remote_state.h"
#include "remote/remote_ui.h"
#include "remote/remote_fb.h"
#include "remote/remote_rf.h"
#include "remote/remote_rf_sync.h"
#include "remote/remote_link.h"

static int failures;
static uint8_t s_seq = 1;

#define CHECK(cond, what) do { \
        bool ok_ = (cond); \
        printf("[%s] %s\n", ok_ ? "+" : "-", what); \
        if (!ok_) failures++; \
    } while (0)

/* ACTIVITY_SYNC from the Hub, short id format: id, NUL, name at payload[3], query flag at [24] */
static void hub_activity_frame(const char *id, const char *name, uint8_t state, uint8_t seq) {
    uint8_t p[25] = {0};
    memcpy(p, id, strlen(id) < 2 ? strlen(id) : 2);
    strncpy((char *)&p[3], name, 20);
    p[24] = state;
    rf_frame_t rf;
    rf_build_frame(&rf, 1, RF_MSG_ACTIVITY_SYNC, seq, 0, p, 25);
    handle_hub_frame_remote(&rf);
}

static void hub_activity_state(const char *id, const char *name, uint8_t state) {
    hub_activity_frame(id, name, state, s_seq++);
}

static void hub_activity(const char *id, const char *name, bool query_reply) {
    hub_activity_state(id, name, query_reply ? RF_ACT_SYNC_QUERY : RF_ACT_SYNC_COMPLETE);
}

static void reset_state(const char *running) {
    g_is_switching_activity = false;
    g_switch_hub_started = false;
    g_initial_sync_received = true;
    snprintf(g_active_activity_id, sizeof(g_active_activity_id), "%s", running);
    g_last_switch_target_id[0] = '\0';
    g_last_switch_completed_ms = 0;
    g_menu_open = false;
}

int main(void) {
    printf("[*] test_remote_activity_follow\n");
    g_fb_size = FB_SIZE;
    g_fb_mem = (uint16_t *)calloc(1, FB_SIZE);
    if (!g_fb_mem) return 1;

    g_activity_count = 3;
    snprintf(g_activities[0].id, sizeof(g_activities[0].id), "1");
    snprintf(g_activities[0].name, sizeof(g_activities[0].name), "Watch TV");
    snprintf(g_activities[1].id, sizeof(g_activities[1].id), "2");
    snprintf(g_activities[1].name, sizeof(g_activities[1].name), "Play Games");
    snprintf(g_activities[2].id, sizeof(g_activities[2].id), "3");
    snprintf(g_activities[2].name, sizeof(g_activities[2].name), "Music");
    g_screen_on = true;
    g_rf_link_connected = true;
    g_remote_slot = 1;

    /* 1. Query reply with another activity while Settings > Power is open */
    reset_state("1");
    g_current_view = VIEW_SETTINGS;
    g_settings_subview = SETTINGS_VIEW_POWER;
    g_selected_sub_item = 3;
    hub_activity("2", "Play Games", true);
    CHECK(g_current_view == VIEW_SETTINGS && g_settings_subview == SETTINGS_VIEW_POWER && g_selected_sub_item == 3,
          "catch-up in Settings: the page and its selection stay");
    CHECK(strcmp(g_active_activity_id, "2") == 0, "catch-up in Settings: the new activity is recorded");

    /* 2. The Suspend countdown page keeps counting */
    reset_state("2");
    g_current_view = VIEW_SETTINGS;
    g_settings_subview = SETTINGS_VIEW_SUSPEND;
    g_suspend_countdown_start_ms = 12345;
    hub_activity("3", "Music", true);
    CHECK(g_current_view == VIEW_SETTINGS && g_settings_subview == SETTINGS_VIEW_SUSPEND &&
          g_suspend_countdown_start_ms == 12345, "catch-up on the Suspend page: the countdown page stays");
    g_suspend_countdown_start_ms = 0;

    /* 3. Live change while a device's commands are open in the device browser */
    reset_state("3");
    g_current_view = VIEW_DEVICE_BROWSER;
    g_browse_dev = 0;
    hub_activity("1", "Watch TV", false);
    CHECK(g_current_view == VIEW_DEVICE_BROWSER && g_browse_dev == 0, "live change in the device browser: the device page stays");
    CHECK(!g_is_switching_activity, "live change in the device browser: no progress bar over it");
    CHECK(strcmp(g_active_activity_id, "1") == 0, "live change in the device browser: the new activity is recorded");
    hub_activity("1", "Watch TV", true);
    CHECK(g_current_view == VIEW_DEVICE_BROWSER && !g_is_switching_activity, "the next query reply for it changes nothing");

    /* 4. Live change during pairing */
    reset_state("1");
    g_current_view = VIEW_PAIRING;
    hub_activity("-1", "PowerOff", false);
    CHECK(g_current_view == VIEW_PAIRING && !g_is_switching_activity, "live change during pairing: the pairing page stays");

    /* 4b. A "Not paired" page shown although the radio is paired: the first sync leaves it */
    reset_state("-1");
    g_initial_sync_received = false;
    g_current_view = VIEW_NOT_PAIRED;
    hub_activity("2", "Play Games", true);
    CHECK(g_current_view == VIEW_DEVICES, "first sync on the out-of-date \"Not paired\" page: the activity's screen opens");

    /* 5. Home and activity screens follow, as before */
    reset_state("-1");
    g_current_view = VIEW_ACTIVITIES;
    hub_activity("2", "Play Games", true);
    CHECK(g_current_view == VIEW_DEVICES, "catch-up on home: the activity's screen opens");
    reset_state("2");
    g_current_view = VIEW_DEVICES;
    hub_activity("-1", "PowerOff", true);
    CHECK(g_current_view == VIEW_ACTIVITIES, "catch-up to PowerOff on an activity screen: home opens");

    /* 6. Live change on home: progress bar, then the activity's screen when the Hub confirms */
    reset_state("-1");
    g_current_view = VIEW_ACTIVITIES;
    hub_activity("3", "Music", false);
    CHECK(g_is_switching_activity, "live change on home: progress bar shown");
    hub_activity("3", "Music", true);
    CHECK(!g_is_switching_activity && g_current_view == VIEW_DEVICES, "Hub confirms it: the activity's screen opens");

    /* 7. A Hub switch whose progress bar ends on a page that does not follow (keys and touch
          cannot get there while the bar shows; covers a tap handled just before it started) */
    reset_state("-1");
    g_current_view = VIEW_DEVICES;
    hub_activity("1", "Watch TV", false);
    CHECK(g_is_switching_activity, "live change on an activity screen: progress bar shown");
    g_current_view = VIEW_SETTINGS;
    g_settings_subview = SETTINGS_VIEW_DISPLAY;
    g_is_switching_activity = false; /* the main loop ran out of time on the bar */
    remote_show_switch_target();
    CHECK(g_current_view == VIEW_SETTINGS && g_settings_subview == SETTINGS_VIEW_DISPLAY,
          "end of a Hub switch on a page that does not follow: the page stays");

    /* 8. A switch started on the remote (a hard key mapped to PowerOff, pressed in Settings) */
    reset_state("2");
    g_current_view = VIEW_SETTINGS;
    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
    trigger_activity_switch("-1", "PowerOff");
    CHECK(g_is_switching_activity, "own switch from Settings: progress bar shown");
    hub_activity("-1", "PowerOff", true);
    CHECK(!g_is_switching_activity && g_current_view == VIEW_ACTIVITIES, "own switch: it ends on its target screen");

    /* 9. The Hub unpairs the remote while a Hub switch's progress bar runs: "Not paired" stays */
    reset_state("-1");
    g_current_view = VIEW_ACTIVITIES;
    hub_activity("2", "Play Games", false);
    CHECK(g_is_switching_activity, "Hub switch before the unpair: progress bar shown");
    int saved_fd = g_rf_fd;
    g_rf_fd = open("/dev/null", O_WRONLY);
    rf_perform_unpair_cleanup(false);
    close(g_rf_fd);
    g_rf_fd = saved_fd;
    if (g_is_switching_activity) remote_show_switch_target(); /* the main loop at the bar's end */
    CHECK(!g_is_switching_activity && g_current_view == VIEW_NOT_PAIRED,
          "unpair during a Hub switch: the bar ends, \"Not paired\" stays");

    /* Last: once the Hub sent a start notice, the remote takes it for a Hub that announces its
       switches (a live change without one opens no progress bar) */

    /* 10. A switch the Hub started, on home: the bar opens on the start notice and closes on the end */
    reset_state("-1");
    g_current_view = VIEW_ACTIVITIES;
    hub_activity_state("3", "Music", RF_ACT_SYNC_STARTING);
    CHECK(g_is_switching_activity && g_switch_hub_started && strcmp(g_switching_target_id, "3") == 0,
          "Hub start notice on home: progress bar shown");
    CHECK(strcmp(g_active_activity_id, "-1") == 0, "Hub start notice: the running activity is not changed yet");
    hub_activity_state("3", "Music", RF_ACT_SYNC_COMPLETE);
    CHECK(!g_is_switching_activity && g_current_view == VIEW_DEVICES && strcmp(g_active_activity_id, "3") == 0,
          "Hub end notice: the bar closes on the activity's screen");

    /* 11. A switch started on the remote that the Hub ends on the old activity (it failed) */
    reset_state("2");
    g_current_view = VIEW_DEVICES;
    trigger_activity_switch("1", "Watch TV");
    hub_activity_state("1", "Watch TV", RF_ACT_SYNC_STARTING);
    CHECK(g_is_switching_activity && g_switch_hub_started, "own switch: the Hub's start notice keeps the bar");
    hub_activity_state("2", "Play Games", RF_ACT_SYNC_COMPLETE);
    CHECK(!g_is_switching_activity && strcmp(g_active_activity_id, "2") == 0 && g_current_view == VIEW_DEVICES,
          "own switch ended by the Hub on another activity: the bar closes on it");

    /* 12. A Hub switch while Settings is open: no bar, the page stays, the activity is recorded */
    reset_state("2");
    g_current_view = VIEW_SETTINGS;
    g_settings_subview = SETTINGS_VIEW_DISPLAY;
    hub_activity_state("1", "Watch TV", RF_ACT_SYNC_STARTING);
    CHECK(!g_is_switching_activity, "Hub start notice in Settings: no progress bar");
    hub_activity_state("1", "Watch TV", RF_ACT_SYNC_COMPLETE);
    CHECK(!g_is_switching_activity && g_current_view == VIEW_SETTINGS && strcmp(g_active_activity_id, "1") == 0,
          "Hub end notice in Settings: the page stays, the activity is recorded");

    /* 13. An end notice without a start (remote asleep at the start) opens no bar afterwards
       (case 10 taught the remote that this Hub announces its switches) */
    reset_state("1");
    g_current_view = VIEW_ACTIVITIES;
    hub_activity_state("3", "Music", RF_ACT_SYNC_COMPLETE);
    CHECK(!g_is_switching_activity && g_current_view == VIEW_DEVICES && strcmp(g_active_activity_id, "3") == 0,
          "end notice without a start: no bar for a switch that is over, the activity's screen opens");

    /* 14. A start notice during a config sync does not record the target as running */
    reset_state("1");
    g_is_syncing_config = true;
    hub_activity_state("2", "Play Games", RF_ACT_SYNC_STARTING);
    CHECK(strcmp(g_active_activity_id, "1") == 0 && !g_is_switching_activity,
          "start notice during a config sync: the running activity stays");
    g_is_syncing_config = false;

    /* 15. The end notice is lost: a sync query answered mid-switch keeps the bar, a plain answer
       closes it (the Hub is idle again), also when the switch failed and the old activity runs */
    reset_state("1");
    g_current_view = VIEW_ACTIVITIES;
    hub_activity_state("3", "Music", RF_ACT_SYNC_STARTING);
    hub_activity_state("3", "Music", RF_ACT_SYNC_QUERY_SWITCHING);
    CHECK(g_is_switching_activity && g_switch_hub_started, "query answered mid-switch: the bar stays");
    hub_activity_state("1", "Watch TV", RF_ACT_SYNC_QUERY);
    CHECK(!g_is_switching_activity && strcmp(g_active_activity_id, "1") == 0,
          "lost end notice: the next plain query answer closes the bar on the running activity");

    /* 16. A remote that woke mid-switch sees it from its sync query answer */
    reset_state("1");
    g_current_view = VIEW_ACTIVITIES;
    hub_activity_state("2", "Play Games", RF_ACT_SYNC_QUERY_SWITCHING);
    CHECK(g_is_switching_activity && g_switch_hub_started && strcmp(g_switching_target_id, "2") == 0,
          "query answered mid-switch on home: progress bar shown");
    hub_activity_state("2", "Play Games", RF_ACT_SYNC_COMPLETE);
    CHECK(!g_is_switching_activity && strcmp(g_active_activity_id, "2") == 0, "end notice closes it");

    /* 17. The Hub read its state for a query answer just before it sent a notice: the answer
       arrives after the notice and is older than it */
    reset_state("1");
    g_current_view = VIEW_ACTIVITIES;
    link_note_query(9, get_monotonic_ms() - 100);
    hub_activity_state("3", "Music", RF_ACT_SYNC_STARTING);
    hub_activity_frame("1", "Watch TV", RF_ACT_SYNC_QUERY, 9);
    CHECK(g_is_switching_activity, "a plain answer to a query sent before the start notice keeps the bar");
    link_note_query(10, get_monotonic_ms() - 50);
    hub_activity_state("3", "Music", RF_ACT_SYNC_COMPLETE);
    hub_activity_frame("3", "Music", RF_ACT_SYNC_QUERY_SWITCHING, 10);
    CHECK(!g_is_switching_activity, "a switching answer to a query sent before the end notice opens no bar");
    link_note_query(11, get_monotonic_ms());
    hub_activity_state("2", "Play Games", RF_ACT_SYNC_STARTING);
    link_note_query(12, get_monotonic_ms() + 5);
    hub_activity_frame("2", "Play Games", RF_ACT_SYNC_QUERY_SWITCHING, 12);
    hub_activity_frame("2", "Play Games", RF_ACT_SYNC_COMPLETE, s_seq++);
    CHECK(!g_is_switching_activity && strcmp(g_active_activity_id, "2") == 0,
          "answers to later queries still count, and the end notice closes the bar");

    /* 18. A query answered mid-switch with the screen off (alarm wake): no switch state, it would
       block the suspend with no query to end it */
    reset_state("1");
    g_current_view = VIEW_ACTIVITIES;
    g_screen_on = false;
    hub_activity_state("3", "Music", RF_ACT_SYNC_QUERY_SWITCHING);
    CHECK(!g_is_switching_activity, "query answered mid-switch with the screen off: no switch screen");
    g_screen_on = true;
    hub_activity_state("3", "Music", RF_ACT_SYNC_QUERY_SWITCHING);
    CHECK(g_is_switching_activity, "the next answer with the screen on shows it");
    hub_activity_state("3", "Music", RF_ACT_SYNC_COMPLETE);

    if (failures) {
        printf("[-] test_remote_activity_follow: %d check(s) failed\n", failures);
        return 1;
    }
    printf("[+] test_remote_activity_follow passed\n");
    return 0;
}
