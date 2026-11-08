/* The status bar menu, the RF link panel and the one-time key hint.
 * The menu opens from the gear at the left of the status bar, or from a long press of the
 * Activities key, and holds actions that have no physical key. */
#include "remote_ui.h"
#include "remote_fb.h"
#include "remote_config.h"

#include <stdio.h>
#include <string.h>

typedef enum { MENU_HELP, MENU_SETTINGS } menu_item_t;

#define MENU_X     4
#define MENU_Y     36
#define MENU_W     150
#define MENU_ROW_H 34
#define MENU_MAX   4

static int menu_items(menu_item_t *out) {
    int n = 0;
    if (g_active_activity_id[0] && strcmp(g_active_activity_id, "-1") != 0) out[n++] = MENU_HELP;
    out[n++] = MENU_SETTINGS;
    return n;
}

void ui_menu_open(void) {
    g_menu_open = true;
    g_menu_sel = 0;
    g_screen_dirty = true;
}

void ui_menu_close(void) {
    g_menu_open = false;
    g_screen_dirty = true;
}

static void menu_activate(int idx) {
    menu_item_t items[MENU_MAX];
    int n = menu_items(items);
    ui_menu_close();
    if (idx < 0 || idx >= n) return;
    switch (items[idx]) {
        case MENU_HELP:
            ui_help_start();
            break;
        case MENU_SETTINGS:
            g_current_view = VIEW_SETTINGS;
            g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            g_selected_sub_item = 0;
            g_factory_reset_stage = 0;
            break;
    }
}

void render_menu(void) {
    menu_item_t items[MENU_MAX];
    int n = menu_items(items);
    int h = n * MENU_ROW_H + 4;
    fb_fill_rect(MENU_X, MENU_Y, MENU_W, h, RGB_CARD);
    fb_draw_rect(MENU_X, MENU_Y, MENU_W, h, RGB_TEAL);
    for (int i = 0; i < n; i++) {
        int y = MENU_Y + 2 + i * MENU_ROW_H;
        bool sel = (i == g_menu_sel);
        if (sel) fb_fill_rect(MENU_X + 2, y, MENU_W - 4, MENU_ROW_H, RGB_CARD_SEL);
        const char *icon = NULL, *label = "";
        switch (items[i]) {
            case MENU_HELP:
                icon = "/opt/luadraws/appscommon/asset/footer/help.png";
                label = tr(STR_HELP);
                break;
            case MENU_SETTINGS:
                icon = "/opt/luadraws/appscommon/asset/footer/settings.png";
                label = tr(STR_SETTINGS);
                break;
        }
        if (!blit_png_file(MENU_X + 8, y + 6, 22, 22, icon)) {
            if (items[i] == MENU_SETTINGS) fb_draw_cog(MENU_X + 19, y + 17, RGB_WHITE);
            else fb_draw_text_aa(MENU_X + 15, y + 8, "?", RGB_WHITE, &g_font_title);
        }
        fb_draw_text_aa(MENU_X + 38, y + 9, label, sel ? RGB_WHITE : RGB_TEXT_MUTED, &g_font_main);
    }
}

/* Keys that drive the open menu; returns false for keys the menu leaves alone */
bool ui_menu_handle_key(int code) {
    menu_item_t items[MENU_MAX];
    int n = menu_items(items);
    switch (code) {
        case 103: case 78:  /* KEY_UP, rocker up */
            g_menu_sel = (g_menu_sel + n - 1) % n;
            break;
        case 108: case 74:  /* KEY_DOWN, rocker down */
            g_menu_sel = (g_menu_sel + 1) % n;
            break;
        case 28: case 148:  /* OK, KEY_PROG1 */
            menu_activate(g_menu_sel);
            break;
        case 158: case 202: case 174:  /* Back, KEY_PROG3, Exit */
            ui_menu_close();
            break;
        default:
            return false;
    }
    render_screen();
    return true;
}

/* A tap on a row chooses it; a tap anywhere else closes the menu */
void ui_menu_handle_tap(int x, int y) {
    menu_item_t items[MENU_MAX];
    int n = menu_items(items);
    int row = (y - MENU_Y - 2) / MENU_ROW_H;
    if (x >= MENU_X && x < MENU_X + MENU_W && y >= MENU_Y + 2 && row < n) {
        menu_activate(row);
    } else {
        ui_menu_close();
    }
    render_screen();
}

/* Dismisses the key hint if it is showing; returns true when the input was used for that */
bool ui_key_hint_dismiss(void) {
    if (!g_key_hint_pending || !is_remote_paired() || g_menu_open) return false;
    g_key_hint_pending = false;
    save_settings_conf();
    render_screen();
    return true;
}

/* RF link details below the status bar, opened by tapping the link indicator */
void render_link_panel(void) {
    link_quality_t q;
    uint64_t now = get_monotonic_ms();
    link_quality(now, &q);

    const char *msg = q.state == LINK_NO_HUB ? tr(STR_LINK_LOST) : q.state == LINK_HUB_SILENT ? tr(STR_LINK_HUB_SILENT) : NULL;
    int y = UI_CONTENT_Y - 4, h = msg ? 124 : 104;
    fb_fill_rect(0, y, FB_WIDTH, h, RGB_CARD);
    fb_fill_rect(0, y + h - 1, FB_WIDTH, 1, RGB_DIVIDER);
    fb_draw_text_aa(8, y + 8, tr(STR_LINK_TITLE), RGB_WHITE, &g_font_title);
    int ry = y + 34;
    if (msg) {
        fb_draw_text_aa(8, ry, msg, RGB_RED, &g_font_main);
        ry += 20;
    }

    char pct_up[8], pct_rep[8], age[16];
    snprintf(pct_up, sizeof(pct_up), q.status_pct < 0 ? "-" : "%d%%", q.status_pct);
    snprintf(pct_rep, sizeof(pct_rep), q.reply_pct < 0 ? "-" : "%d%%", q.reply_pct);
    uint64_t frame_age = ms_since(now, g_last_hub_frame_ms);
    if (g_last_hub_frame_ms == 0) snprintf(age, sizeof(age), "-");
    else if (frame_age < 10000) snprintf(age, sizeof(age), "%.1f s", frame_age / 1000.0);
    else if (frame_age < 120000) snprintf(age, sizeof(age), "%d s", (int)(frame_age / 1000));
    else snprintf(age, sizeof(age), "%d min", (int)(frame_age / 60000));

    const char *labels[3] = { tr(STR_LINK_UP), tr(STR_LINK_REPLIES), tr(STR_LINK_LAST_FRAME) };
    const char *values[3] = { pct_up, pct_rep, age };
    for (int i = 0; i < 3; i++, ry += 20) {
        fb_draw_text_aa(8, ry, labels[i], RGB_TEXT_MUTED, &g_font_main);
        fb_draw_text_aa(FB_WIDTH - 8 - fb_text_width_aa(values[i], &g_font_main), ry, values[i], RGB_WHITE, &g_font_main);
    }
}

/* Closes the link panel if it is open; returns true when it was */
bool ui_link_panel_dismiss(void) {
    if (!g_link_panel_open) return false;
    g_link_panel_open = false;
    g_screen_dirty = true;
    return true;
}
