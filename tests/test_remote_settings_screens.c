/*
 * test_remote_settings_screens.c - the Settings pages as drawn and as tapped.
 *
 *  1. Every Settings page is drawn in every language; each text box (fb_draw_text_aa, wrapped)
 *     must stay on the screen and must not run into another text on the same rows. Labels used
 *     to be drawn at full width under the right-aligned value or badge.
 *  2. Every row of every page is tapped (handle_screen_touch_tap); a tap must select the card it
 *     lands on (fb_fill_rect and execute_settings_action wrapped). The Update page used rows of
 *     another size than it drew.
 *  3. Leaving the Suspend page (Cancel, the Back keys) returns to its own item of the Power page,
 *     not to the Debug logging item above it.
 *
 * Linked with -Wl,--wrap=fb_draw_text_aa,--wrap=fb_fill_rect,--wrap=execute_settings_action.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote/remote_state.h"
#include "remote/remote_ui.h"
#include "remote/remote_fb.h"
#include "remote/remote_input.h"
#include "remote/remote_i18n.h"

static int s_fail;

/* ---- text boxes ---- */
#define MAX_BOXES 128
static struct { int x, y, w, h; char text[64]; } s_box[MAX_BOXES];
static int s_boxes;

void __real_fb_draw_text_aa(int x, int y, const char *text, uint16_t fg, const font_aa_t *font);
void __wrap_fb_draw_text_aa(int x, int y, const char *text, uint16_t fg, const font_aa_t *font) {
    if (text && font && text[0] && s_boxes < MAX_BOXES) {
        s_box[s_boxes].x = x;
        s_box[s_boxes].y = y;
        s_box[s_boxes].w = fb_text_width_aa(text, font);
        s_box[s_boxes].h = font->height;
        snprintf(s_box[s_boxes].text, sizeof(s_box[0].text), "%s", text);
        s_boxes++;
    }
    __real_fb_draw_text_aa(x, y, text, fg, font);
}

/* ---- cards ---- */
#define MAX_CARDS 32
static struct { int y, h; } s_card[MAX_CARDS];
static int s_cards;

void __real_fb_fill_rect(int x, int y, int w, int h, uint16_t color);
void __wrap_fb_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (x == 8 && w == FB_WIDTH - 16 && s_cards < MAX_CARDS) {
        s_card[s_cards].y = y;
        s_card[s_cards].h = h;
        s_cards++;
    }
    __real_fb_fill_rect(x, y, w, h, color);
}

static int s_action = -2;
static bool s_pass_actions;
void __real_execute_settings_action(int idx);
void __wrap_execute_settings_action(int idx) {
    s_action = idx;
    if (s_pass_actions) __real_execute_settings_action(idx);
}

static const char *page_name(int v) {
    static const char *names[] = { "categories", "config", "display", "touch", "power", "system", "update",
                                   "factory reset", "reboot", "suspend" };
    return v >= 0 && v < (int)(sizeof(names) / sizeof(names[0])) ? names[v] : "?";
}

static void draw_page(int page) {
    g_current_view = VIEW_SETTINGS;
    g_settings_subview = (settings_subview_t)page;
    g_selected_sub_item = 0;
    g_selected_category = 0;
    s_boxes = 0;
    s_cards = 0;
    memset(g_fb_mem, 0, FB_SIZE);
    render_settings_screen();
}

static void check_text(int page, remote_lang_t lang) {
    for (int a = 0; a < s_boxes; a++) {
        if (s_box[a].x < 0 || s_box[a].x + s_box[a].w > FB_WIDTH) {
            printf("[-] %s, %s: \"%s\" x %d..%d is off the screen\n", remote_lang_code(lang), page_name(page),
                   s_box[a].text, s_box[a].x, s_box[a].x + s_box[a].w);
            s_fail = 1;
        }
        for (int b = a + 1; b < s_boxes; b++) {
            bool rows = s_box[a].y < s_box[b].y + s_box[b].h - 3 && s_box[b].y < s_box[a].y + s_box[a].h - 3;
            bool cols = s_box[a].x < s_box[b].x + s_box[b].w && s_box[b].x < s_box[a].x + s_box[a].w;
            if (rows && cols) {
                printf("[-] %s, %s: \"%s\" x %d..%d runs into \"%s\" x %d..%d\n", remote_lang_code(lang), page_name(page),
                       s_box[a].text, s_box[a].x, s_box[a].x + s_box[a].w, s_box[b].text, s_box[b].x,
                       s_box[b].x + s_box[b].w);
                s_fail = 1;
            }
        }
    }
}

int main(void) {
    g_fb_size = FB_SIZE;
    g_fb_mem = (uint16_t *)calloc(1, FB_SIZE);
    if (!g_fb_mem) return 1;
    g_screen_on = true;
    g_touch_enabled = true;
    g_rf_fd = -1;
    snprintf(g_battery_info.powersource, sizeof(g_battery_info.powersource), "%s", "battery");

    /* 1. Text fits, with every badge shown */
    g_update_config_available = true;
    g_update_ota_available = true;
    snprintf(g_hub_ota_version, sizeof(g_hub_ota_version), "%s", "99.0.0");
    for (int lang = 0; lang < REMOTE_LANG_COUNT; lang++) {
        g_language = (remote_lang_t)lang;
        for (int page = SETTINGS_VIEW_CATEGORIES; page <= SETTINGS_VIEW_UPDATE; page++) {
            draw_page(page);
            check_text(page, (remote_lang_t)lang);
        }
    }
    if (!s_fail) printf("[+] Settings pages: every text fits beside its value or badge, in every language\n");
    g_language = (remote_lang_t)0;

    /* 2. Taps */
    int tap_fail = 0;
    for (int page = SETTINGS_VIEW_CATEGORIES; page <= SETTINGS_VIEW_SUSPEND; page++) {
        draw_page(page);
        int n = page == SETTINGS_VIEW_CATEGORIES ? 6 : get_settings_sub_count();
        if (s_cards < n) {
            printf("[-] %s: %d cards drawn for %d items\n", page_name(page), s_cards, n);
            tap_fail = 1;
            continue;
        }
        int first = s_cards - n; /* the item cards are the last ones drawn (the Update page has an info card first) */
        for (int y = UI_CONTENT_Y; y < FB_HEIGHT; y++) {
            int want = -1;
            for (int i = 0; i < n; i++) {
                if (y >= s_card[first + i].y && y < s_card[first + i].y + s_card[first + i].h) want = i;
            }
            g_settings_subview = (settings_subview_t)page;
            g_current_view = VIEW_SETTINGS;
            s_action = -1;
            g_touch_down_x = 120;
            g_touch_down_y = y;
            handle_screen_touch_tap(120, y);
            if (s_action != want) {
                printf("[-] %s: a tap at y=%d selects item %d, the card drawn there is item %d\n", page_name(page), y,
                       s_action, want);
                tap_fail = 1;
                break;
            }
        }
    }
    if (!tap_fail) printf("[+] Settings pages: every tap selects the card it lands on\n");
    s_fail |= tap_fail;

    /* 3. Leaving the Suspend page */
    s_pass_actions = true;
    snprintf(g_battery_info.powersource, sizeof(g_battery_info.powersource), "%s", "usb");
    g_current_view = VIEW_SETTINGS;
    g_settings_subview = SETTINGS_VIEW_POWER;
    __real_execute_settings_action(SETTINGS_POWER_SUSPEND_ITEM);
    bool opened = g_settings_subview == SETTINGS_VIEW_SUSPEND;
    __real_execute_settings_action(0); /* Cancel */
    bool cancel_ok = opened && g_settings_subview == SETTINGS_VIEW_POWER && g_selected_sub_item == SETTINGS_POWER_SUSPEND_ITEM;
    g_settings_subview = SETTINGS_VIEW_SUSPEND;
    handle_navigation_key(158, 1); /* KEY_BACK */
    bool back_ok = g_settings_subview == SETTINGS_VIEW_POWER && g_selected_sub_item == SETTINGS_POWER_SUSPEND_ITEM;
    g_settings_subview = SETTINGS_VIEW_SUSPEND;
    handle_navigation_key(149, 1); /* the bottom Back key */
    bool back2_ok = g_settings_subview == SETTINGS_VIEW_POWER && g_selected_sub_item == SETTINGS_POWER_SUSPEND_ITEM;
    if (cancel_ok && back_ok && back2_ok) {
        printf("[+] Suspend page: Cancel and the Back keys return to its own Power item\n");
    } else {
        printf("[-] Suspend page: return to the Power page, item %d expected (cancel %d, back %d, bottom back %d)\n",
               SETTINGS_POWER_SUSPEND_ITEM, cancel_ok, back_ok, back2_ok);
        s_fail = 1;
    }

    if (s_fail) {
        printf("[-] test_remote_settings_screens: FAILED\n");
        return 1;
    }
    printf("[+] test_remote_settings_screens: all checks passed\n");
    return 0;
}
