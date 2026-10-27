#include "elite_ui.h"
#include "elite_fb.h"
#include "elite_config.h"
#include "elite_rf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void trigger_activity_switch(const char *id, const char *name) {
    if (!g_rf_link_connected) {
        printf("[!] Cannot switch activity '%s': Hub RF connection lost\n", name ? name : "");
        rf_send_sync_query();
        g_screen_dirty = true;
        render_screen();
        return;
    }
    g_is_switching_activity = true;
    strncpy(g_switching_target_id, id ? id : "-1", sizeof(g_switching_target_id) - 1);
    strncpy(g_switching_target_name, name ? name : "Activity", sizeof(g_switching_target_name) - 1);
    g_switching_start_ms = get_monotonic_ms();

    rf_act_step_t combined_steps[32];
    uint32_t total_delay = 0;
    int step_cnt = build_combined_activity_sequence(g_active_activity_id, g_switching_target_id,
                                                   combined_steps, 32, &total_delay);
    g_switching_total_ms = total_delay;
    printf("[*] Starting activity switch to '%s' (%s): %d combined steps (stop+start), %ums total delay\n",
           g_switching_target_name, g_switching_target_id, step_cnt, g_switching_total_ms);

    rf_send_activity_start(id ? id : "-1");
    render_screen();
}

void trigger_activity_switch_external(const char *id, const char *name) {
    g_is_switching_activity = true;
    strncpy(g_switching_target_id, id ? id : "-1", sizeof(g_switching_target_id) - 1);
    strncpy(g_switching_target_name, name ? name : "Activity", sizeof(g_switching_target_name) - 1);
    g_switching_start_ms = get_monotonic_ms();

    rf_act_step_t combined_steps[32];
    uint32_t total_delay = 0;
    int step_cnt = build_combined_activity_sequence(g_active_activity_id, g_switching_target_id,
                                                   combined_steps, 32, &total_delay);
    g_switching_total_ms = total_delay;
    printf("[*] External activity switch started for '%s' (%s): %d combined steps (stop+start), %ums total delay\n",
           g_switching_target_name, g_switching_target_id, step_cnt, g_switching_total_ms);

    g_screen_dirty = true;
}

void request_activity_switch_with_confirm(int act_idx) {
    if (act_idx < 0 || act_idx >= g_activity_count) return;
    activity_item_t *sel = &g_activities[act_idx];

    if (strcmp(g_active_activity_id, sel->id) == 0) {
        show_notice("Activity already active", 2000);
        render_screen();
        return;
    }

    if (!g_confirm_activity) {
        trigger_activity_switch(sel->id, sel->name);
        return;
    }

    uint64_t now = get_monotonic_ms();
    if (g_confirm_pending_idx == act_idx && now < g_confirm_pending_deadline_ms) {
        g_confirm_pending_idx = -1;
        g_confirm_pending_deadline_ms = 0;
        trigger_activity_switch(sel->id, sel->name);
    } else {
        g_confirm_pending_idx = act_idx;
        g_confirm_pending_deadline_ms = now + 4000;
        show_notice("Press OK or tap again to confirm", 3000);
        render_screen();
    }
}

void render_activities_screen(void) {
    int start_y = 38;
    int card_h = 56;
    int spacing = 4;
    int page_size = 4;
    int total = g_activity_count;

    if (total <= 0) {
        fb_fill_rect(12, start_y + 10, FB_WIDTH - 24, 120, RGB_CARD);
        fb_draw_rect(12, start_y + 10, FB_WIDTH - 24, 120, RGB_DIVIDER);
        fb_draw_text_aa(24, start_y + 30, "No Activities", RGB_WHITE, &g_font_title);
        fb_draw_text_aa(24, start_y + 55, "Syncing with Hub...", RGB_CYAN, &g_font_main);
        return;
    }

    int page_start = (g_selected_activity / page_size) * page_size;
    int cur_page = (g_selected_activity / page_size) + 1;
    int total_pages = (total + page_size - 1) / page_size;

    for (int i = 0; i < page_size && (page_start + i) < total; i++) {
        int idx = page_start + i;
        int y = start_y + i * (card_h + spacing);
        bool is_sel = (idx == g_selected_activity);
        bool is_active = (strcmp(g_activities[idx].id, g_active_activity_id) == 0);

        bool is_confirm_pending = (g_confirm_pending_idx == idx);
        uint16_t bg = is_confirm_pending ? 0x2100 : (is_sel ? RGB_CARD_SEL : RGB_CARD);
        uint16_t border = is_confirm_pending ? RGB_YELLOW : (is_sel ? RGB_TEAL : 0x2945);

        fb_fill_rect(6, y, FB_WIDTH - 12, card_h, bg);
        fb_draw_rect(6, y, FB_WIDTH - 12, card_h, border);

        if (is_confirm_pending) {
            fb_fill_rect(6, y, 4, card_h, RGB_YELLOW);
        } else if (is_active) {
            fb_fill_rect(6, y, 4, card_h, RGB_GREEN);
        } else if (is_sel) {
            fb_fill_rect(6, y, 4, card_h, RGB_TEAL);
        }

        int icon_x = 14;
        int icon_y = y + 8;
        fb_fill_rect(icon_x - 2, icon_y - 2, 44, 44, is_sel ? 0x2125 : 0x18C3);
        fb_draw_rect(icon_x - 2, icon_y - 2, 44, 44, is_sel ? RGB_TEAL : 0x31A6);

        bool has_icon = false;
        if (g_activities[idx].icon_path[0]) {
            has_icon = blit_png_file(icon_x, icon_y, 40, 40, g_activities[idx].icon_path);
        }

        if (!has_icon) {
            fb_draw_rect(icon_x, icon_y, 40, 40, is_sel ? RGB_TEAL : 0x32A9);
            if (strcmp(g_activities[idx].id, "-1") == 0) {
                fb_fill_rect(icon_x + 19, icon_y + 8, 2, 10, RGB_RED);
                fb_draw_rect(icon_x + 12, icon_y + 12, 16, 16, RGB_RED);
            } else {
                fb_draw_rect(icon_x + 10, icon_y + 12, 20, 14, is_sel ? RGB_WHITE : RGB_TEXT_MUTED);
                fb_fill_rect(icon_x + 18, icon_y + 26, 4, 3, is_sel ? RGB_WHITE : RGB_TEXT_MUTED);
            }
        }

        uint16_t text_color = (is_sel || is_active || is_confirm_pending) ? RGB_WHITE : 0xD6DA;
        fb_draw_text_aa(64, y + 13, g_activities[idx].name, text_color, &g_font_main);

        if (is_confirm_pending) {
            fb_draw_text_aa(64, y + 33, "CONFIRM? PRESS OK", RGB_YELLOW, &g_font_main);
        } else if (is_active) {
            fb_draw_text_aa(64, y + 33, "ACTIVE", RGB_GREEN, &g_font_main);
        }

        if (is_sel) {
            if (!blit_png_file(FB_WIDTH - 24, y + 20, 10, 15, "/opt/luadraws/appscommon/asset/home/chevron.png")) {
                fb_draw_text_aa(FB_WIDTH - 24, y + 19, ">", RGB_TEAL, &g_font_title);
            }
        }
    }

    if (total_pages > 1) {
        char pstr[16];
        snprintf(pstr, sizeof(pstr), "%d/%d", cur_page, total_pages);
        fb_draw_text_aa(FB_WIDTH - 32, start_y + page_size * (card_h + spacing) - 2, pstr, RGB_TEXT_MUTED, &g_font_main);
    }
}

void render_switching_view(void) {
    fb_fill_rect(0, 0, FB_WIDTH, 34, RGB_HEADER);
    fb_draw_text_aa(12, 9, "SWITCHING ACTIVITY", RGB_CYAN, &g_font_title);
    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);

    int card_y = 48;
    int card_h = 224;
    fb_fill_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_CARD);
    fb_draw_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_CARD_SEL);

    fb_draw_text_aa(22, card_y + 14, "SWITCHING TO", RGB_TEXT_MUTED, &g_font_main);
    fb_draw_text_aa(22, card_y + 36, g_switching_target_name, RGB_WHITE, &g_font_title);

    char switch_icon[128] = {0};
    resolve_activity_icon(NULL, g_switching_target_name, switch_icon, sizeof(switch_icon));
    if (switch_icon[0]) {
        blit_png_file(FB_WIDTH - 20 - 52, card_y + 14, 44, 44, switch_icon);
    }

    rf_act_step_t target_steps[32];
    uint32_t calc_total_ms = 0;
    int count = build_combined_activity_sequence(g_active_activity_id, g_switching_target_id,
                                                target_steps, 32, &calc_total_ms);

    uint64_t elapsed = get_monotonic_ms() - g_switching_start_ms;
    uint32_t total_ms = (g_switching_total_ms > 0) ? g_switching_total_ms : calc_total_ms;

    int current_step_idx = 1;
    char current_step_name[32] = "Configuring devices";
    if (strcmp(g_switching_target_id, "-1") == 0) {
        strncpy(current_step_name, "Powering off all devices", sizeof(current_step_name) - 1);
    }

    if (count > 0) {
        uint64_t accum = 0;
        for (int s = 0; s < count; s++) {
            accum += target_steps[s].delay_ms;
            if (elapsed < accum || s == count - 1) {
                current_step_idx = s + 1;
                strncpy(current_step_name, target_steps[s].name, sizeof(current_step_name) - 1);
                current_step_name[sizeof(current_step_name) - 1] = '\0';
                break;
            }
        }
    }

    char step_hdr[32];
    if (count > 0) {
        snprintf(step_hdr, sizeof(step_hdr), "Step %d of %d", current_step_idx, count);
    } else {
        snprintf(step_hdr, sizeof(step_hdr), "Executing Actions");
    }
    fb_draw_text_aa(22, card_y + 70, step_hdr, RGB_CYAN, &g_font_main);

    fb_draw_text_aa(22, card_y + 92, current_step_name, RGB_GREEN, &g_font_title);

    char time_str[32];
    double cur_sec = (double)elapsed / 1000.0;
    double tot_sec = (double)total_ms / 1000.0;
    if (cur_sec > tot_sec) cur_sec = tot_sec;
    snprintf(time_str, sizeof(time_str), "Timing: %.1fs / %.1fs", cur_sec, tot_sec);
    fb_draw_text_aa(22, card_y + 126, time_str, RGB_TEXT_MUTED, &g_font_main);

    int bar_x = 22;
    int max_w = FB_WIDTH - 44;
    int bar_w = (int)((elapsed * max_w) / total_ms);
    if (bar_w > max_w) bar_w = max_w;
    fb_fill_rect(bar_x, card_y + 152, max_w, 10, RGB_BG);
    fb_draw_rect(bar_x, card_y + 152, max_w, 10, 0x2945);
    if (bar_w > 0) {
        fb_fill_rect(bar_x, card_y + 152, bar_w, 10, RGB_TEAL);
    }

    fb_draw_text_aa(22, card_y + 178, "Please wait...", RGB_TEXT_MUTED, &g_font_main);

    fb_fill_rect(0, FB_HEIGHT - 30, FB_WIDTH, 30, RGB_HEADER);
    fb_draw_text_aa(18, FB_HEIGHT - 22, "Screen locked during switch", RGB_TEXT_MUTED, &g_font_main);
}
