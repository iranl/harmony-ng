#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "elite/elite_state.h"
#include "elite/elite_ui.h"
#include "elite/elite_ui_scroll.h"
#include "elite/elite_fb.h"
#include "elite/elite_input.h"
#include "elite/elite_power.h"
#include "elite/elite_rf.h"
#include "elite/elite_rf_sync.h"

static void tap(int x, int y) {
    g_touch_down_x = x;
    g_touch_down_y = y;
    handle_screen_touch_tap(x, y);
}

int main(void) {
    printf("[*] Running Headless Elite UI Engine Mock Test...\n");

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

    elite_lang_t orig_lang = g_language;
    elite_lang_cycle();
    assert(g_language != orig_lang);
    printf("[+] Language cycled from %s to %s\n", elite_lang_code(orig_lang), elite_lang_code(g_language));

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

    /* 10. RF link quality (ADR-0012): the lower of link reports and Hub replies sets the bars */
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

    /* 11. Help (ADR-0004): listed in the menu only while an activity runs; follows the Hub's
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

    /* 12. Drag scrolling of the home pages (ADR-0009) */
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
    #undef SETTLE
    printf("[+] Home drag scrolling: drag, flick, snap back, edge resistance, taps\n");

    free(g_fb_mem);
    printf("\n[+] ALL HEADLESS ELITE UI CHECKS PASSED!\n");
    return 0;
}
