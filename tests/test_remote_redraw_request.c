/*
 * test_remote_redraw_request.c - g_screen_dirty is how the RF and power threads ask the main
 * loop for a redraw. A request made while render_screen() draws a frame must survive the frame:
 * the frame on screen was drawn from the state before the change. render_screen() cleared the
 * flag after presenting, so the remote kept showing the old screen (for example a frozen
 * activity progress overlay after the Hub confirmed the switch).
 *
 * Linked with -Wl,--wrap=fb_present: the request is made while the frame is being presented,
 * as the RF thread would make it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote/remote_state.h"
#include "remote/remote_ui.h"
#include "remote/remote_fb.h"

static int s_presents;

bool __wrap_fb_present(const uint16_t *frame, bool wait_vsync) {
    (void)frame;
    (void)wait_vsync;
    s_presents++;
    if (s_presents == 1) g_screen_dirty = true; /* state changed by another thread during the frame */
    return true;
}

int main(void) {
    g_fb_size = FB_SIZE;
    g_fb_mem = (uint16_t *)calloc(1, FB_SIZE);
    if (!g_fb_mem) return 1;
    g_activity_count = 1;
    snprintf(g_activities[0].id, sizeof(g_activities[0].id), "%s", "1001");
    snprintf(g_activities[0].name, sizeof(g_activities[0].name), "%s", "Watch TV");
    g_current_view = VIEW_ACTIVITIES;
    g_screen_on = true;
    g_key_hint_pending = false;

    g_screen_dirty = true;
    render_screen();
    if (s_presents != 1) {
        printf("[-] test_remote_redraw_request: %d frames presented, expected 1\n", s_presents);
        return 1;
    }
    if (!g_screen_dirty) {
        printf("[-] test_remote_redraw_request: a redraw requested during the frame was lost\n");
        return 1;
    }
    /* The next frame draws the new state; with no new request the flag is clear afterwards */
    render_screen();
    if (g_screen_dirty) {
        printf("[-] test_remote_redraw_request: the flag stays set after a frame without a request\n");
        return 1;
    }
    printf("[+] test_remote_redraw_request: a redraw requested during a frame gets its own frame\n");
    return 0;
}
