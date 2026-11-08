#include "remote_ui.h"
#include "remote_ui_scroll.h"
#include "remote_fb.h"
#include "remote_config.h"
#include "remote_rf.h"

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
    record_activity_start(id);
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
        /* Already running: open its screen, as after a switch */
        g_confirm_pending_idx = -1;
        g_current_view = VIEW_DEVICES;
        g_selected_device_cmd = 0;
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
        show_notice(tr(STR_PRESS_OK_TO_CONFIRM), 3000);
        render_screen();
    }
}

int activity_list_count(void) {
    return g_activity_count;
}

int home_layout(void) {
    if (g_physical_nav_enabled) return HOME_LAYOUT_LIST;
    if (g_home_layout != HOME_LAYOUT_THEME) return g_home_layout;
    return g_theme.layout == THEME_LAYOUT_GRID ? HOME_LAYOUT_GRID
         : g_theme.layout == THEME_LAYOUT_CAROUSEL ? HOME_LAYOUT_CAROUSEL : HOME_LAYOUT_LIST;
}

const char *home_layout_name(void) {
    int l = home_layout();
    return l == HOME_LAYOUT_GRID ? "grid" : l == HOME_LAYOUT_CAROUSEL ? "carousel" : "list";
}

bool home_layout_is_grid(void) {
    return home_layout() == HOME_LAYOUT_GRID;
}

bool home_layout_is_carousel(void) {
    return home_layout() == HOME_LAYOUT_CAROUSEL;
}

int activity_page_size(void) {
    return home_layout_is_carousel() ? 1 : home_layout_is_grid() ? ACT_GRID_COLS * ACT_GRID_ROWS : ACT_PAGE_SIZE;
}

/* The carousel starts on the running activity */
void home_select_running(void) {
    if (!home_layout_is_carousel()) return;
    for (int i = 0; i < g_activity_count; i++) {
        if (strcmp(g_activities[i].id, g_active_activity_id) == 0) g_selected_activity = i;
    }
}

/* Where the activity at position slot of the current page goes */
void activity_slot_rect(int slot, int *x, int *y, int *w, int *h) {
    if (home_layout_is_carousel()) {
        *x = 0;
        *y = UI_CONTENT_Y;
        *w = FB_WIDTH;
        *h = HOME_PAGE_PITCH - ACT_CARD_GAP;
    } else if (home_layout_is_grid()) {
        *w = (FB_WIDTH - 12 - ACT_CARD_GAP) / ACT_GRID_COLS;
        *h = ACT_TILE_H;
        *x = 6 + (slot % ACT_GRID_COLS) * (*w + ACT_CARD_GAP);
        *y = UI_CONTENT_Y + (slot / ACT_GRID_COLS) * (ACT_TILE_H + ACT_CARD_GAP);
    } else {
        *x = 6;
        *w = FB_WIDTH - 12;
        *h = ACT_CARD_H;
        *y = UI_CONTENT_Y + slot * (ACT_CARD_H + ACT_CARD_GAP);
    }
}

int home_activity_at(int x, int y) {
    int page_size = activity_page_size();
    int page_start = (g_selected_activity / page_size) * page_size;
    for (int i = 0; i < page_size && (page_start + i) < activity_list_count(); i++) {
        int cx, cy, cw, ch;
        activity_slot_rect(i, &cx, &cy, &cw, &ch);
        if (y >= cy && y < cy + ch + ACT_CARD_GAP && (!home_layout_is_grid() || (x >= cx && x < cx + cw + ACT_CARD_GAP))) {
            return page_start + i;
        }
    }
    return -1;
}

int home_card_device_at(int y) {
    if (!home_layout_is_carousel() || g_selected_activity >= activity_list_count() ||
        strcmp(g_activities[g_selected_activity].id, g_active_activity_id) != 0) return -1;
    const rf_activity_t *act = rf_activity_by_id(g_active_activity_id);
    return act ? activity_card_device_at(act, UI_CONTENT_Y + HOME_PAGE_PITCH, y) : -1;
}

/* The icon in its well; a placeholder when the image is missing. In the grid the tile's own
   border shows the selection, so the well keeps its idle border. */
static void draw_activity_icon(int idx, int icon_x, int icon_y, bool is_sel, bool grid) {
    fb_fill_rect(icon_x - 2, icon_y - 2, 44, 44, is_sel ? g_theme.pal.icon_well_sel : g_theme.pal.icon_well);
    fb_draw_rect(icon_x - 2, icon_y - 2, 44, 44, is_sel && !grid ? RGB_TEAL : g_theme.pal.icon_well_border);

    bool has_icon = false;
    if (g_activities[idx].icon_path[0]) {
        has_icon = blit_png_file(icon_x, icon_y, 40, 40, g_activities[idx].icon_path);
    }

    if (!has_icon) {
        fb_draw_rect(icon_x, icon_y, 40, 40, is_sel ? RGB_TEAL : g_theme.pal.icon_outline);
        fb_draw_rect(icon_x + 10, icon_y + 12, 20, 14, is_sel ? RGB_WHITE : RGB_TEXT_MUTED);
        fb_fill_rect(icon_x + 18, icon_y + 26, 4, 3, is_sel ? RGB_WHITE : RGB_TEXT_MUTED);
    }
}

static void draw_centered_text(int x, int w, int y, const char *text, uint16_t color) {
    char fit[sizeof(g_activities[0].name)];
    fb_fit_text_aa(fit, sizeof(fit), text, &g_font_main, w - 8);
    fb_draw_text_aa(x + (w - fb_text_width_aa(fit, &g_font_main)) / 2, y, fit, color, &g_font_main);
}

/* One page of activities, page_start first, drawn y_off pixels below its place */
static void render_activity_page(int page_start, int y_off) {
    int page_size = activity_page_size();
    int total = activity_list_count();
    bool grid = home_layout_is_grid();

    for (int i = 0; i < page_size && (page_start + i) < total; i++) {
        int idx = page_start + i;
        int x, y, w, card_h;
        activity_slot_rect(i, &x, &y, &w, &card_h);
        y += y_off;
        bool is_sel = (idx == g_selected_activity);
        bool is_active = (strcmp(g_activities[idx].id, g_active_activity_id) == 0);

        bool is_confirm_pending = (g_confirm_pending_idx == idx);
        uint16_t bg = is_confirm_pending ? g_theme.pal.warning_bg : (is_sel ? RGB_CARD_SEL : RGB_CARD);
        uint16_t border = is_confirm_pending ? RGB_YELLOW : (is_sel ? RGB_TEAL : g_theme.pal.outline);
        uint16_t mark = is_confirm_pending ? RGB_YELLOW : (is_active ? RGB_GREEN : (is_sel ? RGB_TEAL : 0));
        uint16_t text_color = (is_sel || is_active || is_confirm_pending) ? RGB_WHITE : RGB_TEXT_MUTED;

        fb_fill_rect(x, y, w, card_h, bg);
        fb_draw_rect(x, y, w, card_h, border);

        if (grid) {
            /* Tile: the state as a bar along the top, the icon, the name, then the state */
            if (mark) fb_fill_rect(x, y, w, 4, mark);
            draw_activity_icon(idx, x + (w - 40) / 2, y + 10, is_sel, true);
            draw_centered_text(x, w, y + 54, g_activities[idx].name, text_color);
            if (is_confirm_pending) draw_centered_text(x, w, y + 68, tr(STR_CONFIRM_PRESS_OK), RGB_YELLOW);
            else if (is_active) draw_centered_text(x, w, y + 68, tr(STR_ACTIVE), RGB_GREEN);
            continue;
        }

        if (mark) fb_fill_rect(x, y, 4, card_h, mark);
        draw_activity_icon(idx, 14, y + 5, is_sel, false);

        char name[sizeof(g_activities[0].name)];
        fb_draw_text_aa(64, y + 10, fb_fit_text_aa(name, sizeof(name), g_activities[idx].name, &g_font_main, FB_WIDTH - 30 - 64), text_color, &g_font_main);

        if (is_confirm_pending) {
            fb_draw_text_aa(64, y + 28, tr(STR_CONFIRM_PRESS_OK), RGB_YELLOW, &g_font_main);
        } else if (is_active) {
            fb_draw_text_aa(64, y + 28, tr(STR_ACTIVE), RGB_GREEN, &g_font_main);
        }

        if (is_sel) {
            if (!blit_png_file(FB_WIDTH - 24, y + 18, 10, 15, "/opt/luadraws/appscommon/asset/home/chevron.png")) {
                fb_draw_text_aa(FB_WIDTH - 24, y + 17, ">", RGB_TEAL, &g_font_title);
            }
        }
    }

}

/* The activity at idx as a card filling the content area, x_off pixels right of its place */
static void render_carousel_page(int idx, int x_off) {
    rf_activity_t fallback = {0};
    const rf_activity_t *act = rf_activity_by_id(g_activities[idx].id);
    if (!act) {
        /* Activities from a local file have no Hub record: the card shows the name and the
           icon the name matches */
        snprintf(fallback.id, sizeof(fallback.id), "%s", g_activities[idx].id);
        snprintf(fallback.name, sizeof(fallback.name), "%s", g_activities[idx].name);
        act = &fallback;
    }
    bool active = strcmp(g_activities[idx].id, g_active_activity_id) == 0;
    bool confirm = g_confirm_pending_idx == idx;
    draw_activity_card(act, x_off, UI_CONTENT_Y + HOME_PAGE_PITCH,
                       confirm ? tr(STR_CONFIRM_PRESS_OK) : active ? tr(STR_ACTIVE) : NULL,
                       confirm ? RGB_YELLOW : RGB_GREEN);
}

/* A dot per activity below the content, the current one filled */
static void render_page_dots(int page, int pages) {
    if (pages <= 1) return;
    const int pitch = 10, size = 6;
    int x = (FB_WIDTH - (pages - 1) * pitch - size) / 2;
    int y = UI_CONTENT_Y + HOME_PAGE_PITCH + 3;
    for (int i = 0; i < pages; i++, x += pitch) {
        if (i == page) fb_fill_rect(x, y, size, size, RGB_TEAL);
        else fb_draw_rect(x, y, size, size, RGB_TEXT_MUTED);
    }
}

void render_activities_screen(void) {
    int page_size = activity_page_size();
    int total = activity_list_count();

    if (total <= 0) {
        fb_draw_text_aa(8, UI_CONTENT_Y + 20, tr(STR_NO_ACTIVITIES), RGB_WHITE, &g_font_title);
        fb_draw_text_aa(8, UI_CONTENT_Y + 45, tr(STR_SYNCING_WITH_HUB), RGB_CYAN, &g_font_main);
        return;
    }

    int page = g_selected_activity / page_size;
    int total_pages = (total + page_size - 1) / page_size;

    if (home_layout_is_carousel()) {
        /* One activity per page; a drag slides the page sideways and its neighbour in */
        int off = home_scroll_offset();
        fb_clip_rows(UI_CONTENT_Y, UI_CONTENT_Y + HOME_PAGE_PITCH);
        render_carousel_page(page, off);
        if (off > 0 && page > 0) render_carousel_page(page - 1, off - FB_WIDTH);
        if (off < 0 && page + 1 < total) render_carousel_page(page + 1, off + FB_WIDTH);
        fb_clip_reset();
        render_page_dots(page, total);
        return;
    }

    /* While dragging, the page moves with the finger and its neighbour follows it in;
       both are clipped to the rows between the status bar and the page indicator */
    int off = home_scroll_offset();
    fb_clip_rows(UI_CONTENT_Y, UI_CONTENT_Y + HOME_PAGE_PITCH);
    render_activity_page(page * page_size, off);
    if (off > 0 && page > 0) render_activity_page((page - 1) * page_size, off - HOME_PAGE_PITCH);
    if (off < 0 && page + 1 < total_pages) render_activity_page((page + 1) * page_size, off + HOME_PAGE_PITCH);
    fb_clip_reset();

    render_page_indicator(page + 1, total_pages);
}

void render_switching_view(void) {
    fb_fill_rect(0, 0, FB_WIDTH, 34, RGB_HEADER);
    fb_draw_text_aa(12, 9, tr(STR_SWITCHING_ACTIVITY), RGB_CYAN, &g_font_title);
    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);

    int card_y = 48;

    fb_draw_text_aa(8, card_y + 14, tr(STR_SWITCHING_TO), RGB_TEXT_MUTED, &g_font_main);
    char name[sizeof(g_switching_target_name)];
    fb_draw_text_aa(8, card_y + 36, fb_fit_text_aa(name, sizeof(name), g_switching_target_name, &g_font_title, FB_WIDTH - 8 - 52), RGB_WHITE, &g_font_title);

    char switch_icon[128] = {0};
    uint16_t act_icon_id = 0;
    const char *act_icon_key = NULL;
    for (int i = 0; i < g_rf_activity_count; i++) {
        if (strcmp(g_rf_activities[i].id, g_switching_target_id) == 0) {
            act_icon_id = g_rf_activities[i].icon_id;
            act_icon_key = g_rf_activities[i].icon;
            break;
        }
    }
    resolve_activity_icon(act_icon_id, act_icon_key, g_switching_target_name, switch_icon, sizeof(switch_icon));
    if (switch_icon[0]) {
        blit_png_file(FB_WIDTH - 8 - 44, card_y + 14, 44, 44, switch_icon);
    }

    rf_act_step_t target_steps[32];
    uint32_t calc_total_ms = 0;
    int count = build_combined_activity_sequence(g_active_activity_id, g_switching_target_id,
                                                target_steps, 32, &calc_total_ms);

    uint32_t total_ms = (calc_total_ms > 0) ? calc_total_ms : ((g_switching_total_ms > 0) ? g_switching_total_ms : 2000);
    if (total_ms == 0) total_ms = 2000;

    uint64_t now = get_monotonic_ms();
    uint64_t elapsed = (now > g_switching_start_ms) ? (now - g_switching_start_ms) : 0;

    int current_step_idx = 1;
    char current_step_name[32] = {0};
    strncpy(current_step_name, tr(STR_CONFIGURING_DEVICES), sizeof(current_step_name) - 1);
    if (strcmp(g_switching_target_id, "-1") == 0) {
        strncpy(current_step_name, tr(STR_POWERING_OFF_ALL), sizeof(current_step_name) - 1);
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
        snprintf(step_hdr, sizeof(step_hdr), "%s", tr(STR_EXECUTING_ACTIONS));
    }
    fb_draw_text_aa(8, card_y + 70, step_hdr, RGB_CYAN, &g_font_main);

    fb_draw_text_aa(8, card_y + 92, current_step_name, RGB_GREEN, &g_font_title);

    char time_str[32];
    double cur_sec = (double)elapsed / 1000.0;
    double tot_sec = (double)total_ms / 1000.0;
    if (cur_sec > tot_sec) cur_sec = tot_sec;
    snprintf(time_str, sizeof(time_str), "Timing: %.1fs / %.1fs", cur_sec, tot_sec);
    fb_draw_text_aa(8, card_y + 126, time_str, RGB_TEXT_MUTED, &g_font_main);

    int bar_x = 8;
    int max_w = FB_WIDTH - 16;
    int bar_w = (int)((elapsed * max_w) / total_ms);
    if (bar_w > max_w) bar_w = max_w;
    fb_fill_rect(bar_x, card_y + 152, max_w, 10, RGB_BG);
    fb_draw_rect(bar_x, card_y + 152, max_w, 10, g_theme.pal.outline);
    if (bar_w > 0) {
        fb_fill_rect(bar_x, card_y + 152, bar_w, 10, RGB_TEAL);
    }

    fb_draw_text_aa(8, card_y + 178, tr(STR_PLEASE_WAIT), RGB_TEXT_MUTED, &g_font_main);

    fb_fill_rect(0, FB_HEIGHT - 30, FB_WIDTH, 30, RGB_HEADER);
    fb_draw_text_aa(18, FB_HEIGHT - 22, tr(STR_SCREEN_LOCKED_SWITCH), RGB_TEXT_MUTED, &g_font_main);
}
