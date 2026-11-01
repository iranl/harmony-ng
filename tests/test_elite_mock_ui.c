#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "elite/elite_state.h"
#include "elite/elite_ui.h"
#include "elite/elite_fb.h"
#include "elite/elite_input.h"
#include "elite/elite_power.h"
#include "elite/elite_rf.h"

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

    /* 4. Test Header Mode Toggle Tap (x=120, y=20) */
    printf("[-] Testing top header mode toggle tap...\n");
    bool prev_nav = g_screen_nav_mode;
    g_touch_down_x = 120;
    g_touch_down_y = 20;
    handle_screen_touch_tap(120, 20);
    assert(g_screen_nav_mode != prev_nav);
    printf("[+] Nav mode toggle successfully triggered\n");

    /* 5. Test Activity Selection Tap (x=120, y=80 -> Card 0) */
    printf("[-] Testing activity card selection tap...\n");
    g_touch_down_x = 120;
    g_touch_down_y = 80;
    handle_screen_touch_tap(120, 80);
    assert(g_is_switching_activity == true);
    assert(strcmp(g_switching_target_name, "Watch Apple TV") == 0);
    printf("[+] Activity selection triggered switch to '%s'\n", g_switching_target_name);

    /* Simulate switch completion */
    g_is_switching_activity = false;
    strncpy(g_active_activity_id, "1001", sizeof(g_active_activity_id));
    strncpy(g_active_activity_name, "Watch Apple TV", sizeof(g_active_activity_name));

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

    free(g_fb_mem);
    printf("\n[+] ALL HEADLESS ELITE UI CHECKS PASSED!\n");
    return 0;
}
