/* The status bar menu (ADR-0002) and the one-time key hint (ADR-0001).
 * The menu opens from the gear at the left of the status bar, or from a long press of the
 * Activities key, and holds actions that have no physical key. */
#include "elite_ui.h"
#include "elite_fb.h"
#include "elite_config.h"

#include <stdio.h>
#include <string.h>

typedef enum { MENU_SETTINGS } menu_item_t;

#define MENU_X     4
#define MENU_Y     36
#define MENU_W     150
#define MENU_ROW_H 34
#define MENU_MAX   4

static int menu_items(menu_item_t *out) {
    int n = 0;
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
            case MENU_SETTINGS:
                icon = "/opt/luadraws/appscommon/asset/footer/settings.png";
                label = tr(STR_SETTINGS);
                break;
        }
        if (!blit_png_file(MENU_X + 8, y + 6, 22, 22, icon)) fb_draw_cog(MENU_X + 19, y + 17, RGB_WHITE);
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
