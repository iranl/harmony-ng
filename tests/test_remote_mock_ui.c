#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "remote/remote_state.h"
#include "remote/remote_ui.h"
#include "remote/remote_ui_scroll.h"
#include "remote/remote_transition.h"
#include "remote/remote_fb.h"
#include "remote/remote_input.h"
#include "remote/remote_power.h"
#include "remote/remote_rf.h"
#include "remote/remote_rf_sync.h"

static void tap(int x, int y) {
    g_touch_down_x = x;
    g_touch_down_y = y;
    handle_screen_touch_tap(x, y);
}

int main(void) {
    printf("[*] Running Headless Remote UI Engine Mock Test...\n");

    /* 1. Allocate virtual framebuffer memory */
    g_fb_size = FB_SIZE;
    g_fb_mem = (uint16_t *)calloc(1, FB_SIZE);
    assert(g_fb_mem != NULL);

    /* Initialize wallpaper & font */
    load_wallpaper_asset();

    /* 2. Setup mock activities */
    g_activity_count = 3;
    strncpy(g_activities[0].id, "1001", sizeof(g_activities[0].id));
    strncpy(g_activities[0].name, "Watch Apple TV", sizeof(g_activities[0].name));
    strncpy(g_activities[0].type_str, "VirtualTelevisionN", sizeof(g_activities[0].type_str));

    strncpy(g_activities[1].id, "1002", sizeof(g_activities[1].id));
    strncpy(g_activities[1].name, "Play PlayStation", sizeof(g_activities[1].name));
    strncpy(g_activities[1].type_str, "VirtualGameConsole", sizeof(g_activities[1].type_str));

    strncpy(g_activities[2].id, "1003", sizeof(g_activities[2].id));
    strncpy(g_activities[2].name, "Listen to Music", sizeof(g_activities[2].name));
    strncpy(g_activities[2].type_str, "VirtualAudioSystem", sizeof(g_activities[2].type_str));

    g_current_view = VIEW_ACTIVITIES;
    g_screen_on = true;
    g_touch_enabled = true;
    g_rf_link_connected = true;
    g_confirm_activity = false;
    g_key_hint_pending = false;

    /* 3. Render screen and verify buffer modified */
    printf("[-] Testing initial activities rendering...\n");
    render_screen();

    /* Verify non-zero pixels written */
    int non_zero = 0;
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        if (g_fb_mem[i] != 0) non_zero++;
    }
    assert(non_zero > 1000);
    printf("[+] Render produced %d active pixels\n", non_zero);

    /* 4. Status bar: the gear opens the menu, and a tap outside it closes the menu without acting.
          The bottom keys drive the screen only without touch, which the NAV indicator shows */
    printf("[-] Testing status bar menu and NAV indicator...\n");
    tap(10, 20);
    assert(g_menu_open);
    tap(200, 200);
    assert(!g_menu_open);
    assert(!g_is_switching_activity);
    header_layout_t hl;
    header_layout(&hl);
    assert(hl.nav_x < 0);
    g_selected_activity = 0;
    handle_navigation_key(74, 1); /* rocker down */
    assert(g_selected_activity == 0);
    g_touch_enabled = false;
    header_layout(&hl);
    assert(hl.nav_x > 34);
    handle_navigation_key(74, 1);
    assert(g_selected_activity == 1);
    g_touch_enabled = true;
    g_selected_activity = 0;
    printf("[+] Gear opened and closed the menu; bottom keys drive the screen only without touch\n");

    /* 5. Test Activity Selection Tap (x=120, y=80 -> Card 0) */
    printf("[-] Testing activity card selection tap...\n");
    tap(120, 80);
    assert(g_is_switching_activity == true);
    assert(strcmp(g_switching_target_name, "Watch Apple TV") == 0);
    printf("[+] Activity selection triggered switch to '%s'\n", g_switching_target_name);

    /* Simulate switch completion */
    g_is_switching_activity = false;
    strncpy(g_active_activity_id, "1001", sizeof(g_active_activity_id));
    strncpy(g_active_activity_name, "Watch Apple TV", sizeof(g_active_activity_name));

    /* Selecting the running activity opens its screen instead of switching again */
    g_current_view = VIEW_ACTIVITIES;
    tap(120, 80);
    assert(!g_is_switching_activity);
    assert(g_current_view == VIEW_DEVICES);
    printf("[+] Selecting the running activity opened its screen\n");

    /* 6. Test Settings Navigation */
    printf("[-] Testing settings view and language cycling...\n");
    g_current_view = VIEW_SETTINGS;
    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
    render_screen();

    remote_lang_t orig_lang = g_language;
    remote_lang_cycle();
    assert(g_language != orig_lang);
    printf("[+] Language cycled from %s to %s\n", remote_lang_code(orig_lang), remote_lang_code(g_language));

    /* 7. Test Brightness and sysfs backlight write */
    set_backlight_level(4);
    FILE *fbl = fopen("/sys/class/backlight/ap3156_backlight/brightness", "r");
    assert(fbl != NULL);
    char bl_buf[16] = {0};
    assert(fgets(bl_buf, sizeof(bl_buf), fbl) != NULL);
    int bl_val = atoi(bl_buf);
    assert(bl_val == 24); /* level 4 * 6 = 24 */
    fclose(fbl);
    printf("[+] Backlight level 4 scaled and written to sysfs: %d\n", bl_val);

    /* 8. Test Transition Timing Sequence (build_combined_activity_sequence) */
    printf("[-] Testing activity switch transition timing calculation...\n");
    g_rf_step_count = 0;
    /* -1 (PowerOff) stopSequence: 2 steps before Switched, then Switched + 20s tail + step */
    strncpy(g_rf_steps[g_rf_step_count].act_id, "-1", 11);
    g_rf_steps[g_rf_step_count].step_idx = 0x80;
    g_rf_steps[g_rf_step_count].delay_ms = 2250;
    strncpy(g_rf_steps[g_rf_step_count++].name, "PowerOn", 15);

    strncpy(g_rf_steps[g_rf_step_count].act_id, "-1", 11);
    g_rf_steps[g_rf_step_count].step_idx = 0x81;
    g_rf_steps[g_rf_step_count].delay_ms = 250;
    strncpy(g_rf_steps[g_rf_step_count++].name, "PowerOn", 15);

    strncpy(g_rf_steps[g_rf_step_count].act_id, "-1", 11);
    g_rf_steps[g_rf_step_count].step_idx = 0x82;
    g_rf_steps[g_rf_step_count].delay_ms = 20250;
    strncpy(g_rf_steps[g_rf_step_count++].name, "Switched", 15);

    strncpy(g_rf_steps[g_rf_step_count].act_id, "-1", 11);
    g_rf_steps[g_rf_step_count].step_idx = 0x83;
    g_rf_steps[g_rf_step_count].delay_ms = 250;
    strncpy(g_rf_steps[g_rf_step_count++].name, "PowerOn", 15);

    /* 1 (ShieldTV): startSequence has no Switched step */
    strncpy(g_rf_steps[g_rf_step_count].act_id, "1", 11);
    g_rf_steps[g_rf_step_count].step_idx = 0;
    g_rf_steps[g_rf_step_count].delay_ms = 300;
    strncpy(g_rf_steps[g_rf_step_count++].name, "Play", 15);

    strncpy(g_rf_steps[g_rf_step_count].act_id, "1", 11);
    g_rf_steps[g_rf_step_count].step_idx = 1;
    g_rf_steps[g_rf_step_count].delay_ms = 5300;
    strncpy(g_rf_steps[g_rf_step_count++].name, "PowerOn", 15);

    strncpy(g_rf_steps[g_rf_step_count].act_id, "1", 11);
    g_rf_steps[g_rf_step_count].step_idx = 2;
    g_rf_steps[g_rf_step_count].delay_ms = 300;
    strncpy(g_rf_steps[g_rf_step_count++].name, "InputHdmi4", 15);

    /* 2 (PS5): startSequence has Switched step followed by 20s tail and deferred step */
    strncpy(g_rf_steps[g_rf_step_count].act_id, "2", 11);
    g_rf_steps[g_rf_step_count].step_idx = 0;
    g_rf_steps[g_rf_step_count].delay_ms = 300;
    strncpy(g_rf_steps[g_rf_step_count++].name, "Rewind", 15);

    strncpy(g_rf_steps[g_rf_step_count].act_id, "2", 11);
    g_rf_steps[g_rf_step_count].step_idx = 1;
    g_rf_steps[g_rf_step_count].delay_ms = 5300;
    strncpy(g_rf_steps[g_rf_step_count++].name, "PowerOn", 15);

    strncpy(g_rf_steps[g_rf_step_count].act_id, "2", 11);
    g_rf_steps[g_rf_step_count].step_idx = 2;
    g_rf_steps[g_rf_step_count].delay_ms = 300;
    strncpy(g_rf_steps[g_rf_step_count++].name, "InputHdmi2", 15);

    strncpy(g_rf_steps[g_rf_step_count].act_id, "2", 11);
    g_rf_steps[g_rf_step_count].step_idx = 3;
    g_rf_steps[g_rf_step_count].delay_ms = 20300;
    strncpy(g_rf_steps[g_rf_step_count++].name, "Switched", 15);

    strncpy(g_rf_steps[g_rf_step_count].act_id, "2", 11);
    g_rf_steps[g_rf_step_count].step_idx = 4;
    g_rf_steps[g_rf_step_count].delay_ms = 300;
    strncpy(g_rf_steps[g_rf_step_count++].name, "InputHdmi2", 15);

    /* Test A: Switch PowerOff (-1) -> ShieldTV (1) [no Switched in 1 -> count until last step] */
    rf_act_step_t test_steps[32];
    uint32_t total_a = 0;
    int count_a = build_combined_activity_sequence("-1", "1", test_steps, 32, &total_a);
    assert(count_a == 6);
    assert(total_a == 8400); /* 2250 + 250 + 300 + 5300 + 300 = 8400 ms */
    assert(strcmp(test_steps[5].name, "Activity switched") == 0);
    assert(test_steps[5].delay_ms == 0);
    printf("[+] Switch -1 -> 1 timing verified: %ums across %d steps\n", total_a, count_a);

    /* Test B: Switch PowerOff (-1) -> PS5 (2) [Switched in 2 -> count until Switched step] */
    uint32_t total_b = 0;
    int count_b = build_combined_activity_sequence("-1", "2", test_steps, 32, &total_b);
    assert(count_b == 6);
    assert(total_b == 8400); /* 2250 + 250 + 300 + 5300 + 300 = 8400 ms (excludes 20.3s tail) */
    assert(strcmp(test_steps[5].name, "Activity switched") == 0);
    assert(test_steps[5].delay_ms == 0);
    printf("[+] Switch -1 -> 2 timing verified: %ums across %d steps\n", total_b, count_b);

    /* 9. A device name longer than its list chunk arrives whole in RF_MSG_NAME_TAIL_CHUNK frames */
    const char *long_name = "Marantz AV Receiver in the Living Room!"; /* 39 bytes: chunk 16 + tails 22 + 1 */
    assert(strlen(long_name) == RF_NAME_SIZE - 1);
    rf_frame_t rf;
    uint8_t p[25] = {0};
    p[0] = 2; p[1] = 3; p[2] = 15; /* index, count, icon type */
    memcpy(&p[3], "6", 1);
    memcpy(&p[7], long_name, 16);
    rf_build_frame(&rf, 1, RF_MSG_DEVICE_LIST_CHUNK, 1, RF_FLAG_ACK_REQ, p, 25);
    handle_hub_frame_remote(&rf);
    assert(strcmp(g_rf_devices[2].name, "Marantz AV Recei") == 0);
    for (size_t off = 16; off < strlen(long_name); off += 22) {
        memset(p, 0, sizeof(p));
        p[0] = 1; p[1] = 2; p[2] = (uint8_t)off;
        strncpy((char *)&p[3], long_name + off, 22);
        rf_build_frame(&rf, 1, RF_MSG_NAME_TAIL_CHUNK, (uint8_t)(2 + off), RF_FLAG_ACK_REQ, p, 25);
        handle_hub_frame_remote(&rf);
        handle_hub_frame_remote(&rf); /* a resent frame must not repeat the text */
    }
    assert(strcmp(g_rf_devices[2].name, long_name) == 0);
    printf("[+] Long device name assembled: %s\n", g_rf_devices[2].name);

    /* 10. RF link quality: the lower of link reports and Hub replies sets the bars */
    const uint64_t T = 1000000;
    link_quality_t lq;
    g_rf_link_connected = true;
    link_reset();
    link_quality(T, &lq);
    assert(lq.state == LINK_OK && lq.bars == 4 && lq.status_pct == -1 && lq.reply_pct == -1);
    for (int i = 0; i < 30; i++) {
        uint64_t t = T - 25000 + i * 800;
        link_note_status(true, t);
        link_note_query((uint8_t)(200 + i), t);
        /* Replies carry the low 4 bits of the query's sequence number */
        if (i % 5 < 3) link_note_reply((uint8_t)((200 + i) & 0x0F), t + 300); /* 60 % answered in time */
        else link_note_reply((uint8_t)((200 + i) & 0x0F), t + 1500);          /* too late to count */
    }
    link_note_query(99, T - 200); /* not yet due, unanswered: not counted */
    link_quality(T, &lq);
    assert(lq.status_pct == 100 && lq.reply_pct == 60);
    assert(lq.state == LINK_OK && lq.bars == 2);

    /* Link up but no reply for 10 s: the Hub is silent. On a link that is mostly lost, the
       missing replies are the radio's fault, so it shows one bar instead */
    link_reset();
    for (int i = 0; i < 30; i++) {
        link_note_status(true, T - 25000 + i * 800);
        link_note_query((uint8_t)i, T - 25000 + i * 800);
    }
    link_quality(T, &lq);
    assert(lq.state == LINK_HUB_SILENT);
    link_reset();
    for (int i = 0; i < 30; i++) {
        link_note_status(i % 2 == 0, T - 25000 + i * 800);
        link_note_query((uint8_t)i, T - 25000 + i * 800);
    }
    link_quality(T, &lq);
    assert(lq.state == LINK_OK && lq.bars == 1);
    g_rf_link_connected = false;
    link_quality(T, &lq);
    assert(lq.state == LINK_NO_HUB);
    printf("[+] Link quality: bars from the weaker measure, late replies ignored, silent Hub detected\n");

    /* 11. Help: listed in the menu only while an activity runs; follows the Hub's
       status frames, ignores repeats and frames after it closed */
    g_is_syncing_config = false; /* the device list chunk above started a sync */
    snprintf(g_active_activity_id, sizeof(g_active_activity_id), "-1");
    g_current_view = VIEW_ACTIVITIES;
    ui_menu_open();
    ui_menu_handle_key(28);
    assert(!ui_help_active() && g_current_view == VIEW_SETTINGS);

    snprintf(g_active_activity_id, sizeof(g_active_activity_id), "1001");
    g_current_view = VIEW_ACTIVITIES;
    ui_menu_open();
    ui_menu_handle_key(28);
    assert(ui_help_active());
    memset(p, 0, sizeof(p));
    p[0] = RF_HELP_STARTED;
    rf_build_frame(&rf, 1, RF_MSG_HELP_STATUS, 1, RF_FLAG_ACK_REQ, p, 25);
    handle_hub_frame_remote(&rf);
    p[0] = RF_HELP_QUESTION; p[1] = 0; p[2] = 2;
    memcpy(&p[3], "6", 1);
    memcpy(&p[11], "Marantz AV Rec", 14);
    rf_build_frame(&rf, 1, RF_MSG_HELP_STATUS, 2, RF_FLAG_ACK_REQ, p, 25);
    handle_hub_frame_remote(&rf);
    handle_hub_frame_remote(&rf); /* resent: still the first question */
    tap(FB_WIDTH - 20, FB_HEIGHT - 50); /* No */
    assert(ui_help_active());           /* waits for the second question */
    p[1] = 1;
    memcpy(&p[3], "9", 1);
    rf_build_frame(&rf, 1, RF_MSG_HELP_STATUS, 3, RF_FLAG_ACK_REQ, p, 25);
    handle_hub_frame_remote(&rf);
    ui_help_handle_key(28);             /* Yes: the last answer shows the summary */
    assert(ui_help_active());
    ui_help_tick(get_monotonic_ms() + 3001);
    assert(!ui_help_active() && g_current_view == VIEW_DEVICES);
    p[0] = RF_HELP_STARTED;
    rf_build_frame(&rf, 1, RF_MSG_HELP_STATUS, 4, RF_FLAG_ACK_REQ, p, 25);
    handle_hub_frame_remote(&rf);
    assert(!ui_help_active());

    ui_help_start();
    p[0] = RF_HELP_BUSY;
    rf_build_frame(&rf, 1, RF_MSG_HELP_STATUS, 5, RF_FLAG_ACK_REQ, p, 25);
    handle_hub_frame_remote(&rf);
    tap(FB_WIDTH / 2, FB_HEIGHT / 2);   /* a tap closes the summary */
    assert(!ui_help_active());
    printf("[+] Help: menu item only while an activity runs, questions answered, late frames ignored\n");

    /* 12. Drag scrolling of the home pages */
    g_activity_count = 13;
    for (int i = 0; i < g_activity_count; i++) {
        snprintf(g_activities[i].id, sizeof(g_activities[i].id), "%d", 30001 + i);
        snprintf(g_activities[i].name, sizeof(g_activities[i].name), "Activity %d", i + 1);
    }
    g_current_view = VIEW_ACTIVITIES;
    g_home_layout = HOME_LAYOUT_LIST;
    g_touch_enabled = true;
    g_menu_open = g_key_hint_pending = g_link_panel_open = false;
    g_confirm_pending_idx = -1;
    g_selected_activity = 0;
    uint64_t ms = 1000000;
    #define SETTLE() do { for (int f = 0; f < 40 && home_scroll_settling(); f++) { ms += 28; home_scroll_tick(ms); } } while (0)

    /* Dragged past a third of a page: the next page settles in */
    home_scroll_touch_down(120, 250, ms);
    for (int y = 240; y >= 120; y -= 20) { ms += 40; home_scroll_touch_move(120, y, ms); }
    assert(home_scroll_offset() == -130 && home_scroll_active());
    ms += 40;
    assert(home_scroll_touch_up(120, 120, ms));
    SETTLE();
    assert(!home_scroll_active() && home_scroll_offset() == 0 && g_selected_activity == 5);

    /* A short fast flick moves a page too */
    home_scroll_touch_down(120, 250, ms);
    ms += 10; home_scroll_touch_move(120, 225, ms);
    ms += 10; home_scroll_touch_move(120, 200, ms);
    ms += 10;
    assert(home_scroll_touch_up(120, 200, ms));
    SETTLE();
    assert(g_selected_activity == 10);

    /* A short slow drag settles back on the same page */
    home_scroll_touch_down(120, 150, ms);
    for (int y = 160; y <= 200; y += 10) { ms += 100; home_scroll_touch_move(120, y, ms); }
    ms += 100;
    assert(home_scroll_touch_up(120, 200, ms));
    SETTLE();
    assert(g_selected_activity == 10 && home_scroll_offset() == 0);

    /* Past the last page the page moves a third as far, then settles back */
    home_scroll_touch_down(120, 250, ms);
    ms += 40; home_scroll_touch_move(120, 100, ms);
    assert(home_scroll_offset() == -50);
    ms += 40;
    home_scroll_touch_up(120, 100, ms);
    SETTLE();
    assert(g_selected_activity == 10);

    /* A tap is not a drag */
    home_scroll_touch_down(120, 150, ms);
    ms += 50; home_scroll_touch_move(120, 155, ms);
    assert(!home_scroll_touch_up(120, 155, ms) && !home_scroll_active());

    /* The status bar doesn't start a drag */
    home_scroll_touch_down(120, 10, ms);
    ms += 40; home_scroll_touch_move(120, 200, ms);
    assert(!home_scroll_active() && !home_scroll_touch_up(120, 200, ms));

    /* The carousel: one activity per page, dragged sideways */
    g_home_layout = HOME_LAYOUT_CAROUSEL;
    g_selected_activity = 0;
    assert(activity_page_size() == 1);
    home_scroll_touch_down(200, 150, ms);
    ms += 40; home_scroll_touch_move(150, 155, ms);
    ms += 40; home_scroll_touch_move(100, 155, ms);
    assert(home_scroll_offset() == -100);
    ms += 40;
    assert(home_scroll_touch_up(100, 155, ms));
    SETTLE();
    assert(g_selected_activity == 1);

    /* A vertical drag doesn't move the carousel */
    home_scroll_touch_down(120, 100, ms);
    ms += 40; home_scroll_touch_move(125, 200, ms);
    assert(!home_scroll_active() && !home_scroll_touch_up(125, 200, ms));

    /* A key moving to the neighbour slides the new page in from the side it lies on */
    g_selected_activity = 2;
    home_scroll_slide_from(1, ms);
    assert(home_scroll_settling() && home_scroll_offset() == FB_WIDTH);
    uint64_t slide_start = ms;
    SETTLE();
    assert(home_scroll_offset() == 0 && g_selected_activity == 2 && ms - slide_start <= 200 + 2 * 28);
    g_home_layout = HOME_LAYOUT_LIST;
    #undef SETTLE
    printf("[+] Home drag scrolling: drag, flick, snap back, edge resistance, taps, carousel\n");

    /* 13. Screen transitions */
    {
        g_is_switching_activity = g_is_syncing_config = g_is_ota_updating = false;
        g_menu_open = g_key_hint_pending = g_link_panel_open = false;
        g_current_view = VIEW_ACTIVITIES;
        screen_place_t home = screen_place();
        g_current_view = VIEW_DEVICES;
        screen_place_t activity = screen_place();
        g_current_view = VIEW_SETTINGS; g_settings_subview = SETTINGS_VIEW_CATEGORIES;
        screen_place_t categories = screen_place();
        g_settings_subview = SETTINGS_VIEW_DISPLAY;
        screen_place_t display = screen_place();
        g_settings_subview = SETTINGS_VIEW_REBOOT;
        screen_place_t reboot = screen_place();
        g_current_view = VIEW_DEVICE_BROWSER; g_browse_dev = -1;
        screen_place_t devices = screen_place();
        g_browse_dev = 0;
        screen_place_t device = screen_place();
        assert(transition_between(home, activity) == TRANSITION_PUSH);
        assert(transition_between(activity, home) == TRANSITION_POP);
        assert(transition_between(categories, display) == TRANSITION_PUSH);
        assert(transition_between(reboot, display) == TRANSITION_POP);
        assert(transition_between(devices, device) == TRANSITION_PUSH);
        assert(transition_between(home, categories) == TRANSITION_FADE);
        assert(transition_between(activity, devices) == TRANSITION_FADE);
        assert(transition_between(home, home) == TRANSITION_NONE);
        g_link_panel_open = true;
        assert(transition_between(home, screen_place()) == TRANSITION_NONE);
        g_link_panel_open = false;

        /* A slide halfway: the old screen's right half, then the new screen's left half;
           the status bar is the new screen's */
        static uint16_t from[FB_WIDTH * FB_HEIGHT], to[FB_WIDTH * FB_HEIGHT], out[FB_WIDTH * FB_HEIGHT];
        for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) { from[i] = (uint16_t)(i % FB_WIDTH); to[i] = (uint16_t)(0x8000 | (i % FB_WIDTH)); }
        transition_compose(TRANSITION_PUSH, from, to, out, 128);
        assert(out[10 * FB_WIDTH + 5] == to[10 * FB_WIDTH + 5]);
        assert(out[200 * FB_WIDTH + 0] == from[200 * FB_WIDTH + 120]);
        assert(out[200 * FB_WIDTH + 120] == to[200 * FB_WIDTH + 0]);
        transition_compose(TRANSITION_POP, from, to, out, 128);
        assert(out[200 * FB_WIDTH + 0] == to[200 * FB_WIDTH + 120]);
        assert(out[200 * FB_WIDTH + 120] == from[200 * FB_WIDTH + 0]);
        for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) { from[i] = 0x0000; to[i] = 0xFFFF; }
        transition_compose(TRANSITION_FADE, from, to, out, 256);
        assert(out[200 * FB_WIDTH] == 0xFFFF);
        transition_compose(TRANSITION_FADE, from, to, out, 0);
        assert(out[200 * FB_WIDTH] == 0x0000);
        transition_compose(TRANSITION_FADE, from, to, out, 128);
        assert(out[200 * FB_WIDTH] == ((15 << 11) | (31 << 5) | 15));
        printf("[+] Transitions: slides deeper and back, crossfades across\n");
    }

    /* 14. Going back without Back rows */
    {
        /* Back goes up one level; Settings pages have no Back rows */
        g_pairing_state = 2; /* paired: the categories go back to home, not the pairing prompt */
        g_current_view = VIEW_SETTINGS; g_settings_subview = SETTINGS_VIEW_DISPLAY;
        assert(get_settings_sub_count() == 6);
        assert(ui_go_back() && g_settings_subview == SETTINGS_VIEW_CATEGORIES && g_selected_category == 1);
        assert(ui_go_back() && g_current_view == VIEW_ACTIVITIES);
        assert(!ui_go_back());
        g_current_view = VIEW_DEVICES;
        assert(ui_go_back() && g_current_view == VIEW_ACTIVITIES);
        g_rf_device_count = 1;
        snprintf(g_rf_devices[0].name, sizeof(g_rf_devices[0].name), "Marantz");
        g_rf_dev_cmd_count = 2;
        g_rf_dev_cmds[0].dev_idx = g_rf_dev_cmds[1].dev_idx = 0;
        open_device_browser();
        device_browser_activate(0);
        assert(g_browse_dev == 0 && g_browse_sel == 0 && browser_item_count() == 2 && browser_cmd(0) == &g_rf_dev_cmds[0]);
        assert(ui_go_back() && g_browse_dev == -1);
        printf("[+] Back: one level up by swipe or key, no Back rows, device name as a heading\n");
    }

    /* 15. Free-scrolling lists and device rows on the running activity's card */
    {
        uint64_t ms = 2000000;
        #define SETTLE() do { for (int f = 0; f < 60 && list_scroll_settling(); f++) { ms += 28; list_scroll_tick(ms); } } while (0)
        g_rf_device_count = 20;
        g_rf_dev_cmd_count = 0;
        for (int i = 0; i < 20; i++) snprintf(g_rf_devices[i].name, sizeof(g_rf_devices[i].name), "Device %d", i + 1);
        open_device_browser();
        list_geom_t g;
        assert(list_geometry(&g) && g.rows == 7 && g.count == 20 && list_scroll_offset() == 0);
        int max = 13 * LIST_ROW_PITCH;

        /* A slow drag moves the list with the finger and comes to rest with a row at the top */
        list_scroll_touch_down(120, 250, ms);
        for (int y = 240; y >= 150; y -= 10) { ms += 100; list_scroll_touch_move(120, y, ms); }
        assert(list_scroll_offset() == 100 && list_scroll_active());
        ms += 100;
        assert(list_scroll_touch_up(120, 150, ms));
        SETTLE();
        int at = list_scroll_offset();
        assert(!list_scroll_active() && at > 0 && at < max && at % LIST_ROW_PITCH == 0);
        assert(list_row_at(UI_CONTENT_Y + 5) == at / LIST_ROW_PITCH && list_row_at(10) == -1);

        /* A tap opens the row under the finger, wherever the list is scrolled */
        tap(120, UI_CONTENT_Y + LIST_ROW_PITCH + 5);
        assert(g_browse_dev == at / LIST_ROW_PITCH + 1);
        assert(ui_go_back() && g_browse_dev == -1 && g_browse_sel == at / LIST_ROW_PITCH + 1);

        /* A fling carries on past several pages and stops at the last row */
        list_scroll_touch_down(120, 250, ms);
        ms += 10; list_scroll_touch_move(120, 225, ms);
        ms += 10; list_scroll_touch_move(120, 200, ms);
        ms += 10;
        assert(list_scroll_touch_up(120, 200, ms));
        SETTLE();
        assert(list_scroll_offset() == max);

        /* Past the last row the list moves a third as far, then returns */
        list_scroll_touch_down(120, 250, ms);
        ms += 40; list_scroll_touch_move(120, 160, ms);
        assert(list_scroll_offset() == max + 30);
        ms += 40;
        list_scroll_touch_up(120, 160, ms);
        SETTLE();
        assert(list_scroll_offset() == max);

        /* A touch during a fling stops it; its release is not a tap */
        list_scroll_touch_down(120, 100, ms);
        ms += 10; list_scroll_touch_move(120, 140, ms);
        ms += 10; list_scroll_touch_move(120, 180, ms);
        ms += 10; list_scroll_touch_up(120, 180, ms);
        ms += 100; list_scroll_tick(ms);
        assert(list_scroll_settling());
        list_scroll_touch_down(120, 150, ms);
        assert(!list_scroll_settling());
        assert(list_scroll_touch_up(120, 150, ms + 50));
        SETTLE();
        assert(list_scroll_offset() % LIST_ROW_PITCH == 0);

        /* Keys scroll the selection into view: a wrap from the last row jumps, a step slides */
        g_browse_sel = 18;
        device_browser_move(1);
        list_scroll_reveal(ms);
        SETTLE();
        assert(g_browse_sel == 19 && list_scroll_offset() == max);
        device_browser_move(1);
        list_scroll_reveal(ms);
        assert(g_browse_sel == 0 && list_scroll_offset() == 0 && !list_scroll_settling());
        for (int i = 0; i < 7; i++) { device_browser_move(1); list_scroll_reveal(ms); }
        assert(g_browse_sel == 7 && list_scroll_settling());
        SETTLE();
        assert(list_scroll_offset() == LIST_ROW_PITCH);

        /* A list that fits doesn't scroll */
        g_rf_device_count = 3;
        open_device_browser();
        list_scroll_touch_down(120, 250, ms);
        ms += 40; list_scroll_touch_move(120, 100, ms);
        assert(!list_scroll_active() && !list_scroll_touch_up(120, 100, ms));
        #undef SETTLE

        /* The running activity's card: a device row opens that device, Back returns to the card */
        g_rf_activity_count = 1;
        snprintf(g_rf_activities[0].id, sizeof(g_rf_activities[0].id), "40001");
        g_rf_activities[0].dev_count = 2;
        g_rf_activities[0].devs[0] = 2;
        g_rf_activities[0].devs[1] = 1;
        snprintf(g_active_activity_id, sizeof(g_active_activity_id), "40001");
        g_current_view = VIEW_DEVICES;
        assert(get_active_dev_cmd_count() == 0);
        int ry = FB_HEIGHT - 8 - 2 * 30;
        assert(activity_card_device_at(&g_rf_activities[0], FB_HEIGHT, ry - 1) == -1);
        tap(120, ry + 30 + 5);
        assert(g_current_view == VIEW_DEVICE_BROWSER && g_browse_dev == 1);
        assert(ui_go_back() && g_current_view == VIEW_DEVICES && g_browse_dev == -1);

        /* The home hit tests the Web UI's screen preview shares with the tap handler */
        g_current_view = VIEW_ACTIVITIES;
        g_home_layout = HOME_LAYOUT_LIST;
        g_selected_activity = 5; /* the second page */
        assert(home_activity_at(120, UI_CONTENT_Y + 2 * (ACT_CARD_H + ACT_CARD_GAP) + 5) == 7);
        assert(home_activity_at(120, 10) == -1);
        g_home_layout = HOME_LAYOUT_GRID;
        g_selected_activity = 0;
        assert(home_activity_at(200, UI_CONTENT_Y + ACT_TILE_H + ACT_CARD_GAP + 5) == 3);
        g_home_layout = HOME_LAYOUT_CAROUSEL;
        snprintf(g_activities[0].id, sizeof(g_activities[0].id), "40001");
        g_selected_activity = 0;
        int cry = UI_CONTENT_Y + HOME_PAGE_PITCH - 8 - 2 * 30;
        assert(home_card_device_at(cry + 5) == 2 && home_card_device_at(cry - 5) == -1);
        assert(home_activity_at(120, cry - 5) == 0);
        g_selected_activity = 1; /* not the running activity: its card has no device taps */
        assert(home_card_device_at(cry + 5) == -1);
        g_home_layout = HOME_LAYOUT_LIST;
        printf("[+] Lists: drag, fling, edge resistance, catch, key reveal; card device rows open the device\n");
    }

    free(g_fb_mem);
    printf("\n[+] ALL HEADLESS REMOTE UI CHECKS PASSED!\n");
    return 0;
}
