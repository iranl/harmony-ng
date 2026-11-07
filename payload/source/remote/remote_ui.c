#include "remote_ui.h"
#include "remote_fb.h"
#include "remote_power.h"
#include "remote_rf.h"
#include "remote_rf_ota.h"
#include "remote_present.h"
#include "remote_ui_scroll.h"
#include "remote_transition.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

void show_notice(const char *msg, int duration_ms) {
    if (!msg) {
        g_notice_msg[0] = '\0';
        g_notice_until_ms = 0;
        g_screen_dirty = true;
        return;
    }
    strncpy(g_notice_msg, msg, sizeof(g_notice_msg) - 1);
    g_notice_until_ms = get_monotonic_ms() + duration_ms;
    g_screen_dirty = true;
}

/* Right side of the status bar, laid out from the right edge: time, battery, USB, RF link,
   NAV indicator, update badge. The gear sits alone at the left. */
#define HDR_Y 9

#define LINK_BARS_W 15  /* four 3 px bars, 1 px apart */
#define BATT_W      21  /* 19 px body and a 2 px terminal */

/* Bars, USB and battery stand on the clock's baseline and are no taller than its digits */
#define HDR_GLYPH_H 9
#define HDR_BASE    (HDR_Y + 11)  /* last row of the digits */

static const char *link_badge(const link_quality_t *q) {
    return q->state == LINK_NO_HUB ? tr(STR_NO_HUB) : q->state == LINK_HUB_SILENT ? tr(STR_HUB_SILENT) : NULL;
}

void header_layout(header_layout_t *l) {
    int x = FB_WIDTH - 4 - fb_text_width_aa("00:00", &g_font_main); /* time */
    x -= 4 + BATT_W + 4;                                             /* battery */
    if (strcmp(g_battery_info.powersource, "usb") == 0) x -= 2 + 18; /* USB */
    link_quality(get_monotonic_ms(), &l->link);
    const char *badge = link_badge(&l->link);
    l->link_w = badge ? 6 + 8 * (int)strlen(badge) : LINK_BARS_W;
    l->link_x = x - l->link_w;
    x -= 4 + l->link_w;
    l->nav_x = -1;
    l->nav_w = 22;
    if (!g_touch_enabled) {
        x -= 6 + l->nav_w;
        l->nav_x = x;
    }
    bool ota_avail = g_update_ota_available && harmony_version_compare(g_hub_ota_version, HARMONY_REMOTE_VERSION) > 0;
    l->update_x = -1;
    l->update_w = 18;
    if (g_update_config_available || ota_avail) {
        x -= 6 + l->update_w;
        l->update_x = x;
    }
}

void render_header(void) {
    if (!blit_png_file(0, 0, FB_WIDTH, 34, "/opt/luadraws/appscommon/asset/footer/toolbarBG.png")) {
        fb_fill_rect(0, 0, FB_WIDTH, 34, g_theme.pal.status_bg);
    }

    uint16_t gear_col = g_menu_open ? RGB_TEAL : RGB_WHITE;
    if (g_menu_open) fb_fill_rect(2, 3, HDR_GEAR_W - 4, 28, RGB_CARD_SEL);
    if (!blit_png_file(6, 6, 22, 22, "/opt/luadraws/appscommon/asset/footer/settings.png")) {
        fb_draw_cog(17, 17, gear_col);
    }

    header_layout_t l;
    header_layout(&l);

    if (l.update_x >= 0) {
        bool ota_avail = g_update_ota_available && harmony_version_compare(g_hub_ota_version, HARMONY_REMOTE_VERSION) > 0;
        int nx = l.update_x, ny = HDR_Y;
        uint16_t n_fg = RGB_BLACK;
        fb_fill_rect(nx, ny, l.update_w, 16, ota_avail ? RGB_YELLOW : RGB_TEAL);
        fb_draw_rect(nx, ny, l.update_w, 16, RGB_WHITE);
        fb_fill_rect(nx + 8, ny + 3, 2, 5, n_fg);
        fb_fill_rect(nx + 5, ny + 8, 8, 1, n_fg);
        fb_fill_rect(nx + 6, ny + 9, 6, 1, n_fg);
        fb_fill_rect(nx + 7, ny + 10, 4, 1, n_fg);
        fb_fill_rect(nx + 8, ny + 11, 2, 1, n_fg);
        fb_fill_rect(nx + 4, ny + 13, 10, 1, n_fg);
    }

    if (l.nav_x >= 0) {
        int cx = l.nav_x + l.nav_w / 2;
        int cy = HDR_Y + 8;
        fb_fill_rect(l.nav_x, HDR_Y, l.nav_w, 16, RGB_TEAL);
        fb_draw_rect(l.nav_x, HDR_Y, l.nav_w, 16, RGB_WHITE);
        fb_fill_rect(cx - 1, cy - 5, 3, 11, RGB_BLACK);
        fb_fill_rect(cx - 5, cy - 1, 11, 3, RGB_BLACK);
        fb_fill_rect(cx, cy, 1, 1, RGB_TEAL);
    }

    int x = l.link_x;
    const char *badge = link_badge(&l.link);
    if (badge) {
        fb_fill_rect(x, HDR_Y, l.link_w, 16, RGB_RED);
        fb_draw_rect(x, HDR_Y, l.link_w, 16, RGB_WHITE);
        fb_draw_text(x + 3, HDR_Y + 4, badge, RGB_WHITE, RGB_RED, 1);
    } else {
        /* Signal bars, like Wi-Fi: one bar is amber */
        uint16_t on = l.link.bars == 1 ? RGB_YELLOW : RGB_TEAL;
        for (int b = 0; b < 4; b++) {
            int h = 3 + 2 * b;
            fb_fill_rect(x + 4 * b, HDR_BASE + 1 - h, 3, h, b < l.link.bars ? on : RGB_CARD_BORDER);
        }
    }
    x += l.link_w;

    read_remote_battery();
    if (strcmp(g_battery_info.powersource, "usb") == 0) {
        blit_png_file(x + 2, HDR_Y, 18, 14, "/opt/luadraws/appscommon/asset/header/usb.png");
        x += 2 + 18;
    }

    /* Battery outline with a fill for the level: teal while charging, red at 20 % or less */
    int bx = x + 4, by = HDR_BASE + 1 - HDR_GLYPH_H, bw = BATT_W - 2;
    int pct = g_battery_info.percent > 100 ? 100 : g_battery_info.percent;
    uint16_t fill = g_battery_info.charging ? RGB_TEAL : pct <= 20 ? RGB_RED : RGB_WHITE;
    fb_draw_rect(bx, by, bw, HDR_GLYPH_H, RGB_WHITE);
    fb_fill_rect(bx + bw, by + 3, 2, HDR_GLYPH_H - 6, RGB_WHITE);
    int fw = (bw - 4) * pct / 100;
    if (fw < 1) fw = 1;
    fb_fill_rect(bx + 2, by + 2, fw, HDR_GLYPH_H - 4, fill);

    time_t now = ui_wall_time();
    struct tm *tm_info = localtime(&now);
    char time_str[16] = "12:00";
    if (tm_info) strftime(time_str, sizeof(time_str), "%H:%M", tm_info);
    fb_draw_text_aa(FB_WIDTH - 4 - fb_text_width_aa(time_str, &g_font_main), HDR_Y, time_str, RGB_WHITE, &g_font_main);

    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);
}

/* "page/pages" in the 8 px font at the bottom right, below the last list row */
void render_page_indicator(int page, int pages) {
    if (pages <= 1) return;
    char pstr[16];
    snprintf(pstr, sizeof(pstr), "%d/%d", page, pages);
    fb_draw_text(FB_WIDTH - 6 - (int)strlen(pstr) * 8, FB_HEIGHT - 9, pstr, RGB_TEXT_MUTED, 0, 1);
}

/* One-time page over the content after the dock was removed: navigation moved to the physical keys */
void render_key_hint(void) {
    int x = 8, y = UI_CONTENT_Y + 12;
    fb_fill_rect(0, UI_CONTENT_Y - 4, FB_WIDTH, FB_HEIGHT - UI_CONTENT_Y + 4, RGB_CARD);
    fb_draw_text_aa(x, y + 12, tr(STR_KEY_HINT_TITLE), RGB_WHITE, &g_font_title);
    fb_draw_text_aa(x, y + 44, tr(STR_KEY_HINT_ACTIVITIES), RGB_TEXT_MUTED, &g_font_main);
    fb_draw_text_aa(x, y + 66, tr(STR_KEY_HINT_DEVICES), RGB_TEXT_MUTED, &g_font_main);
    fb_draw_text_aa(x, y + 88, tr(STR_KEY_HINT_OFF), RGB_TEXT_MUTED, &g_font_main);
    fb_draw_text_aa(x, y + 110, tr(STR_KEY_HINT_MENU), RGB_TEXT_MUTED, &g_font_main);
    fb_draw_text_aa(x, y + 138, tr(STR_KEY_HINT_DISMISS), RGB_CYAN, &g_font_main);
}

/* Most edges one row of a polygon passed to fill_polygon() may cross; further crossings are dropped */
#define POLY_MAX_INTERSECTIONS 8

/* Fills the polygon pts (x, y pairs, n points) with the even-odd rule, sampling each row at its centre */
static void fill_polygon(int ox, int oy, const int *pts, int n, uint16_t color) {
    int y0 = pts[1], y1 = pts[1];
    for (int i = 1; i < n; i++) {
        if (pts[2 * i + 1] < y0) y0 = pts[2 * i + 1];
        if (pts[2 * i + 1] > y1) y1 = pts[2 * i + 1];
    }
    for (int y = y0; y < y1; y++) {
        int xs[POLY_MAX_INTERSECTIONS], nx = 0;
        for (int i = 0, j = n - 1; i < n && nx < POLY_MAX_INTERSECTIONS; j = i++) {
            int xa = pts[2 * j], ya = pts[2 * j + 1], xb = pts[2 * i], yb = pts[2 * i + 1];
            if ((ya <= y) == (yb <= y)) continue;
            xs[nx++] = xa + (2 * (y - ya) + 1) * (xb - xa) / (2 * (yb - ya));
        }
        for (int a = 1; a < nx; a++)
            for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) { int t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
        for (int k = 0; k + 1 < nx; k += 2) fb_fill_rect(ox + xs[k], oy + y, xs[k + 1] - xs[k], 1, color);
    }
}

/* Full-screen page for a few seconds after the remote is docked or plugged in: a large battery
   with a bolt, its level and "Charging" */
void render_charging_page(void) {
    fb_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, RGB_BG);

    const int bw = 120, bh = 60, tw = 8, th = 24;
    int bx = (FB_WIDTH - bw - tw) / 2, by = FB_HEIGHT / 2 - 70;
    for (int i = 0; i < 3; i++) fb_draw_rect(bx + i, by + i, bw - 2 * i, bh - 2 * i, RGB_WHITE);
    fb_fill_rect(bx + bw, by + (bh - th) / 2, tw, th, RGB_WHITE);
    int pct = g_battery_info.percent > 100 ? 100 : g_battery_info.percent;
    int fw = (bw - 12) * pct / 100;
    if (fw > 0) fb_fill_rect(bx + 6, by + 6, fw, bh - 12, RGB_TEAL);

    static const int bolt[] = { 14, 0, 30, 0, 22, 18, 32, 18, 10, 48, 16, 26, 6, 26 };
    fill_polygon(bx + bw / 2 - 19, by + (bh - 48) / 2, bolt, 7, RGB_WHITE);

    const char *title = tr(STR_CHARGING);
    fb_draw_text_aa((FB_WIDTH - fb_text_width_aa(title, &g_font_title)) / 2, by + bh + 28, title, RGB_WHITE, &g_font_title);
    char pstr[8];
    snprintf(pstr, sizeof(pstr), "%d%%", pct);
    fb_draw_text_aa((FB_WIDTH - fb_text_width_aa(pstr, &g_font_main)) / 2, by + bh + 58, pstr, RGB_TEXT_MUTED, &g_font_main);
}

bool ui_charging_page_active(void) {
    uint32_t until = g_charging_page_until_ms;
    return until != 0 && (int32_t)(until - (uint32_t)get_monotonic_ms()) > 0;
}

/* Closes the charging page if it is showing; returns true when the input was used for that */
bool ui_charging_page_dismiss(void) {
    if (!ui_charging_page_active()) return false;
    g_charging_page_until_ms = 0;
    render_screen();
    return true;
}

void render_sync_overlay(void) {
    fb_fill_rect(0, 0, FB_WIDTH, 34, RGB_HEADER);
    fb_draw_text_aa(14, 9, tr(STR_SYNC_CONFIG_TITLE), RGB_CYAN, &g_font_title);
    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);

    int card_y = 52;

    fb_draw_text_aa(8, card_y + 20, tr(STR_UPDATING_FROM_HUB), RGB_WHITE, &g_font_title);
    fb_draw_text_aa(8, card_y + 46, tr(STR_SYNCING_ACTS_MAPS), RGB_TEXT_MUTED, &g_font_main);

    char items_str[48];
    snprintf(items_str, sizeof(items_str), "Acts: %d | Devs: %d | Btns: %d",
             g_rf_activity_count, g_rf_device_count, g_rf_button_count);
    fb_draw_text_aa(8, card_y + 80, items_str, RGB_TEAL, &g_font_main);

    char chunks_str[40];
    if (g_sync_total > 0) {
        int done = g_sync_chunk_count < g_sync_total ? g_sync_chunk_count : g_sync_total;
        snprintf(chunks_str, sizeof(chunks_str), "Received %d of %d", done, g_sync_total);
    } else {
        snprintf(chunks_str, sizeof(chunks_str), "Chunks received: %d", g_sync_chunk_count);
    }
    fb_draw_text_aa(8, card_y + 106, chunks_str, RGB_TEXT_MUTED, &g_font_main);

    int bar_x = 8;
    int max_w = FB_WIDTH - 16;
    fb_fill_rect(bar_x, card_y + 140, max_w, 8, RGB_BG);
    fb_draw_rect(bar_x, card_y + 140, max_w, 8, g_theme.pal.outline);
    if (g_sync_total > 0) {
        int done = g_sync_chunk_count < g_sync_total ? g_sync_chunk_count : g_sync_total;
        fb_fill_rect(bar_x, card_y + 140, max_w * done / g_sync_total, 8, RGB_CYAN);
    } else {
        uint64_t elapsed = get_monotonic_ms() - g_sync_start_ms;
        int pulse_pos = (int)((elapsed / 15) % (max_w > 30 ? max_w - 30 : 1));
        fb_fill_rect(bar_x + pulse_pos, card_y + 140, 30, 8, RGB_CYAN);
    }

    fb_draw_text_aa(8, card_y + 168, tr(STR_PLEASE_WAIT), RGB_TEXT_MUTED, &g_font_main);

    fb_fill_rect(0, FB_HEIGHT - 30, FB_WIDTH, 30, RGB_HEADER);
    fb_draw_text_aa(18, FB_HEIGHT - 22, tr(STR_SCREEN_LOCKED_SYNC), RGB_TEXT_MUTED, &g_font_main);
}

/* Draws the current state into g_backbuffer. Shared by the remote and the host renderer. */
void render_frame(void) {
    fb_clip_reset();
    if (g_has_wallpaper) {
        memcpy(g_backbuffer, g_bg_wallpaper, sizeof(g_backbuffer));
    } else {
        fb_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, RGB_BG);
    }

    if (g_is_ota_updating) {
        render_ota_overlay();
    } else if (g_is_syncing_config) {
        render_sync_overlay();
    } else if (g_is_switching_activity) {
        render_switching_view();
    } else if (ui_charging_page_active()) {
        render_charging_page(); /* full screen: nothing under it is drawn */
    } else {
        render_header();

        switch (g_current_view) {
            case VIEW_ACTIVITIES:
                render_activities_screen();
                break;
            case VIEW_DEVICES:    render_devices_screen();    break;
            case VIEW_SETTINGS:   render_settings_screen();   break;
            case VIEW_PAIRING:    render_pairing_screen();    break;
            case VIEW_NOT_PAIRED: render_not_paired_screen(); break;
            case VIEW_DEVICE_BROWSER: render_device_browser_view(); break;
            default: break;
        }

        if (ui_help_active()) render_help();
        if (g_menu_open) render_menu();
        else if (g_key_hint_pending && is_remote_paired()) render_key_hint();
        else if (g_link_panel_open) render_link_panel();
    }

    if (g_notice_until_ms > get_monotonic_ms() && g_notice_msg[0]) {
        int nw = fb_text_width_aa(g_notice_msg, &g_font_main);
        int box_w = nw + 16;
        if (box_w > FB_WIDTH - 8) box_w = FB_WIDTH - 8;
        int box_h = 28;
        int box_x = (FB_WIDTH - box_w) / 2;
        int box_y = FB_HEIGHT - 68;
        fb_fill_rect(box_x, box_y, box_w, box_h, RGB_CARD);
        fb_draw_rect(box_x, box_y, box_w, box_h, RGB_TEAL);
        fb_draw_text_aa(box_x + (box_w - nw) / 2, box_y + 7, g_notice_msg, RGB_WHITE, &g_font_main);
    }
}

/* What the frame shows, for the frame timing log */
static const char *screen_name(void) {
    static const char *views[VIEW_COUNT] = { "activities", "activity", "settings", "pairing", "not-paired", "devices" };
    static char name[32];
    if (g_is_ota_updating) return "ota";
    if (g_is_syncing_config) return "sync";
    if (g_is_switching_activity) return "switching";
    if (ui_charging_page_active()) return "charging";
    if (g_menu_open) return "menu";
    if (ui_help_active()) return "help";
    if (g_current_view == VIEW_SETTINGS) {
        snprintf(name, sizeof(name), "settings/%d", (int)g_settings_subview);
        return name;
    }
    return (g_current_view >= 0 && g_current_view < VIEW_COUNT) ? views[g_current_view] : "?";
}

void render_screen(void) {
    if (!g_fb_mem || !g_screen_on) return;
    pthread_mutex_lock(&g_render_mutex);

    int decodes, scales;
    fb_png_work_take(&decodes, &scales);
    transition_t transition = transition_prepare(screen_place());
    uint64_t t0 = get_monotonic_us();
    render_frame();
    uint64_t t1 = get_monotonic_us();
    bool flipped = false;
    if (transition != TRANSITION_NONE) transition_play(transition);
    else flipped = fb_present(g_backbuffer, ui_scroll_active());
    uint64_t t2 = get_monotonic_us();
    if (frame_timing_enabled()) {
        frame_timing_t t = { screen_name(), g_theme.name, home_layout_name(),
                             (uint32_t)(t1 - t0), (uint32_t)(t2 - t1), flipped, 0, 0 };
        fb_png_work_take(&t.decodes, &t.scales);
        frame_timing_record(&t);
    }

    g_screen_dirty = false;
    pthread_mutex_unlock(&g_render_mutex);
}
