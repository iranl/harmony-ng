/*
 * test_remote_wake_catchup.c - An activity change the Hub reports right after the remote woke up
 * is a catch-up: the switch happened while the remote slept, so no "starting activity" overlay.
 * The wake is stamped with get_boot_ms() (monotonic time plus the time suspended); the ACTIVITY_SYNC
 * handler compared it with get_monotonic_ms(), so after the first suspend the difference wrapped
 * and the wake window never matched again.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote/remote_state.h"
#include "remote/remote_rf_sync.h"
#include "rf_proto.h"

/* The Hub reports activity "2" as a live change (no query-reply flag) */
static void hub_reports_activity_2(void) {
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.report_id = 0x20;
    f.dev_slot = 1;
    f.sub_id = 0x12;
    f.hdr_flag = 0x40;
    f.msg_type = RF_MSG_ACTIVITY_SYNC;
    f.payload[0] = '2';
    memcpy(&f.payload[3], "Watch a Movie", 13);
    handle_hub_frame_remote(&f);
}

static int run_case(const char *label, uint64_t suspended_ms) {
    g_suspended_ms = suspended_ms;
    g_initial_sync_received = true;
    g_is_switching_activity = false;
    snprintf(g_active_activity_id, sizeof(g_active_activity_id), "%s", "1");
    g_last_switch_target_id[0] = '\0';
    s_last_wake_ms = get_boot_ms(); /* what fb_set_blank(false) stores when the screen wakes */

    hub_reports_activity_2();

    if (g_is_switching_activity) {
        printf("  [FAIL] %s: the change reported just after the wake started the switch overlay\n", label);
        g_is_switching_activity = false;
        return 1;
    }
    if (strcmp(g_active_activity_id, "2") != 0) {
        printf("  [FAIL] %s: activity is \"%s\", expected \"2\"\n", label, g_active_activity_id);
        return 1;
    }
    return 0;
}

int main(void) {
    int fail = 0;
    fail |= run_case("no suspend yet", 0);
    fail |= run_case("after one hour suspended", 3600000);
    if (fail) {
        printf("[-] test_remote_wake_catchup failed\n");
        return 1;
    }
    printf("[+] test_remote_wake_catchup: a change reported right after a wake is a catch-up\n");
    return 0;
}
