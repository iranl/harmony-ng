/*
 * test_remote_help_clock.c - Help's state time stamp is written by the RF thread with its own
 * clock read, and the main loop compares it with a now_ms it read earlier in its round. A stamp
 * newer than now_ms must count as age 0: the unsigned difference wrapped to a huge age, Help ended
 * at once with "Hub is not answering" and closed 3 s later while the Hub was re-sending the
 * activity's commands.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "remote/remote_state.h"
#include "remote/remote_ui.h"
#include "rf_proto.h"

int main(void) {
    g_rf_fd = -1; /* the help request frame goes nowhere */
    snprintf(g_active_activity_id, sizeof(g_active_activity_id), "%s", "12");

    ui_help_start();
    if (!ui_help_active()) {
        printf("[-] test_remote_help_clock: Help did not start\n");
        return 1;
    }

    /* The main loop reads now_ms ... */
    uint64_t now_ms = get_monotonic_ms();
    usleep(5000);
    /* ... the RF thread handles the Hub's "started" and stamps the state with a later clock ... */
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.msg_type = RF_MSG_HELP_STATUS;
    f.payload[0] = RF_HELP_STARTED;
    ui_help_on_status(&f);
    /* ... and the main loop ticks Help with its older now_ms */
    ui_help_tick(now_ms);

    /* 3 s later Help is still re-sending (the Hub may take up to 120 s); a Help that was ended by
       the wrapped age has closed by then */
    ui_help_tick(now_ms + 3000);
    if (!ui_help_active()) {
        printf("[-] test_remote_help_clock: Help closed 3 s after the Hub said it started\n");
        return 1;
    }
    ui_help_close();
    printf("[+] test_remote_help_clock: a state stamp newer than now_ms counts as age 0\n");
    return 0;
}
