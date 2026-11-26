#include "remote_ui.h"
#include "remote_fb.h"
#include "remote_power.h"
#include "remote_config.h"
#include "remote_rf.h"
#include "harmony_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>

int get_settings_sub_count(void) {
    switch (g_settings_subview) {
        case SETTINGS_VIEW_CATEGORIES: return 6;
        case SETTINGS_VIEW_ACTIVITIES: return 4;
        case SETTINGS_VIEW_DISPLAY:    return 8;
        case SETTINGS_VIEW_TOUCH:      return 5;
        case SETTINGS_VIEW_POWER:      return 5;
        case SETTINGS_VIEW_SYSTEM:     return 3;
        case SETTINGS_VIEW_UPDATE:     return 2;
        case SETTINGS_VIEW_FACTORY_RESET: return 2;
        case SETTINGS_VIEW_REBOOT:        return 2;
        case SETTINGS_VIEW_SUSPEND:       return 2;
        default: return 0;
    }
}

/* Row geometry of the Settings pages, used by the drawing and by settings_item_at() */
#define SETTINGS_CAT_Y        UI_CONTENT_Y
#define SETTINGS_CAT_H        35
#define SETTINGS_CAT_GAP      4
#define SETTINGS_CONFIRM_Y    (38 + 130 + 10) /* below the 130 px text card at 38 */
#define SETTINGS_CONFIRM_H    38
#define SETTINGS_CONFIRM_GAP  8
#define SETTINGS_UPDATE_INFO_Y (SETTINGS_LIST_Y - 4)
#define SETTINGS_UPDATE_INFO_H 76
#define SETTINGS_UPDATE_Y     (SETTINGS_UPDATE_INFO_Y + SETTINGS_UPDATE_INFO_H + 6)
#define SETTINGS_UPDATE_H     34
#define SETTINGS_UPDATE_GAP   5

static void settings_list_rows(int count, int *card_h, int *spacing) {
    /* 8 rows of 28 + 3 px still fit below SETTINGS_LIST_Y on the 320 px screen */
    *card_h = (count >= 8) ? 28 : ((count >= 7) ? 30 : ((count >= 6) ? 34 : 36));
    *spacing = (count >= 6) ? ((count >= 7) ? 3 : 4) : 5;
}

int settings_item_at(int y) {
    int y0, h, gap, n = get_settings_sub_count();
    switch (g_settings_subview) {
        case SETTINGS_VIEW_CATEGORIES:
            y0 = SETTINGS_CAT_Y; h = SETTINGS_CAT_H; gap = SETTINGS_CAT_GAP;
            break;
        case SETTINGS_VIEW_FACTORY_RESET:
        case SETTINGS_VIEW_REBOOT:
        case SETTINGS_VIEW_SUSPEND:
            y0 = SETTINGS_CONFIRM_Y; h = SETTINGS_CONFIRM_H; gap = SETTINGS_CONFIRM_GAP;
            break;
        case SETTINGS_VIEW_UPDATE:
            y0 = SETTINGS_UPDATE_Y; h = SETTINGS_UPDATE_H; gap = SETTINGS_UPDATE_GAP;
            break;
        default:
            y0 = SETTINGS_LIST_Y;
            settings_list_rows(n, &h, &gap);
            break;
    }
    for (int i = 0; i < n; i++) {
        int cy = y0 + i * (h + gap);
        if (y >= cy && y < cy + h) return i;
    }
    return -1;
}

static void render_settings_categories(void) {
    int start_y = SETTINGS_CAT_Y;
    int card_h = SETTINGS_CAT_H;
    int spacing = SETTINGS_CAT_GAP;
    int total = 6;

    const char *cat_names[6] = {
        tr(STR_CAT_CONFIG),
        tr(STR_CAT_DISPLAY),
        tr(STR_CAT_TOUCH),
        tr(STR_CAT_POWER),
        tr(STR_CAT_SYSTEM),
        tr(STR_CAT_UPDATE)
    };
    const char *cat_descs[6] = {
        tr(STR_CAT_CONFIG_DESC),
        tr(STR_CAT_DISPLAY_DESC),
        tr(STR_CAT_TOUCH_DESC),
        tr(STR_CAT_POWER_DESC),
        tr(STR_CAT_SYSTEM_DESC),
        tr(STR_CAT_UPDATE_DESC)
    };
    const char *cat_icons[6] = {
        "/opt/luadraws/appscommon/asset/footer/devices.png",
        "/opt/luadraws/appscommon/asset/hue/brightness.png",
        "/opt/luadraws/appscommon/asset/gesture/gesturehand.png",
        "/opt/luadraws/appscommon/asset/footer/flip.png",
        "/opt/luadraws/appscommon/asset/pairing/connectHub.png",
        "/opt/luadraws/appscommon/asset/footer/footermenu_sleeptimer.png"
    };

    for (int i = 0; i < total; i++) {
        int y = start_y + i * (card_h + spacing);
        bool is_sel = (i == g_selected_category);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : g_theme.pal.outline;

        fb_fill_rect(8, y, FB_WIDTH - 16, card_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, card_h, border);

        bool has_icon = blit_png_file(14, y + 7, 20, 20, cat_icons[i]);
        int text_x = has_icon ? 40 : 14;

        /* The badge first: the title and the description are fitted to the space left of it */
        bool ota_avail = g_update_ota_available && harmony_version_compare(g_hub_ota_version, HARMONY_REMOTE_VERSION) > 0;
        int badge_x;
        if (i == 5 && ota_avail) {
            badge_x = FB_WIDTH - 64;
            fb_draw_text_aa(badge_x, y + 10, "[UPDATE]", RGB_YELLOW, &g_font_main);
        } else if (i == 0 && g_update_config_available) {
            badge_x = FB_WIDTH - 55;
            fb_draw_text_aa(badge_x, y + 10, "[SYNC]", RGB_TEAL, &g_font_main);
        } else {
            badge_x = FB_WIDTH - 20;
            fb_draw_text_aa(badge_x, y + 9, ">", is_sel ? RGB_TEAL : RGB_TEXT_MUTED, &g_font_title);
        }
        char fit[64];
        fb_draw_text_aa(text_x, y + 3, fb_fit_text_aa(fit, sizeof(fit), cat_names[i], &g_font_title, badge_x - 4 - text_x),
                        is_sel ? RGB_WHITE : RGB_CYAN, &g_font_title);
        fb_draw_text_aa(text_x, y + 19, fb_fit_text_aa(fit, sizeof(fit), cat_descs[i], &g_font_main, badge_x - 4 - text_x),
                        RGB_TEXT_MUTED, &g_font_main);
    }
}

static void render_settings_items(const char *title, int count, const char **labels, const char values[][32], const uint16_t *val_colors, const char **icons, int sel_idx) {
    fb_draw_text_aa(14, SETTINGS_HEADING_Y, title, RGB_CYAN, &g_font_title);

    int start_y = SETTINGS_LIST_Y;
    int card_h, spacing;
    settings_list_rows(count, &card_h, &spacing);

    for (int i = 0; i < count; i++) {
        int y = start_y + i * (card_h + spacing);
        bool is_sel = (i == sel_idx);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : g_theme.pal.outline;

        fb_fill_rect(8, y, FB_WIDTH - 16, card_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, card_h, border);

        bool has_icon = false;
        if (icons && icons[i]) {
            has_icon = blit_png_file(14, y + (card_h <= 30 ? 5 : (card_h == 34 ? 7 : 8)), 20, 20, icons[i]);
        }
        int text_x = has_icon ? 38 : 14;
        int text_y = y + (card_h <= 30 ? 6 : (card_h == 34 ? 8 : 9));
        uint16_t text_col = is_sel ? RGB_WHITE : RGB_TEXT_MUTED;

        /* The value first, right-aligned: the label is fitted to the space left of it */
        int label_end = FB_WIDTH - 18;
        if (values && values[i][0]) {
            int vw = fb_text_width_aa(values[i], &g_font_main);
            uint16_t col = val_colors ? val_colors[i] : RGB_TEAL;
            fb_draw_text_aa(FB_WIDTH - 18 - vw, text_y, values[i], col, &g_font_main);
            label_end = FB_WIDTH - 18 - vw - 6;
        }
        char fit[64];
        fb_draw_text_aa(text_x, text_y, fb_fit_text_aa(fit, sizeof(fit), labels[i], &g_font_main, label_end - text_x),
                        text_col, &g_font_main);
    }
}

static void render_settings_factory_reset(void) {
    int card_y = 38;
    int card_h = 130;

    if (g_factory_reset_stage == 0) {
        fb_draw_text_aa(8, card_y + 10, "REVERT TO STOCK FIRMWARE", RGB_YELLOW, &g_font_title);
        fb_draw_text_aa(8, card_y + 36, "Removes custom software", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(8, card_y + 56, "and restores official Logitech UI.", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(8, card_y + 82, "Deletes settings, Hub names,", RGB_TEAL, &g_font_main);
        fb_draw_text_aa(8, card_y + 100, "logs and SSH keys.", RGB_GREEN, &g_font_main);
    } else {
        fb_draw_text_aa(8, card_y + 10, "ARE YOU ABSOLUTELY SURE?", RGB_RED, &g_font_title);
        fb_draw_text_aa(8, card_y + 36, "Remote will restore stock startup,", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(8, card_y + 56, "sync flash, and reboot to Logitech.", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(8, card_y + 82, "The RF pairing is kept.", RGB_TEAL, &g_font_main);
        fb_draw_text_aa(8, card_y + 100, "Device reboots immediately.", RGB_YELLOW, &g_font_main);
    }

    (void)card_h;
    int btn_y = SETTINGS_CONFIRM_Y;
    int btn_h = SETTINGS_CONFIRM_H;
    int spacing = SETTINGS_CONFIRM_GAP;

    const char *btn_labels[2][2] = {
        { tr(STR_CANCEL_KEEP_SYSTEM), tr(STR_PROCEED_TO_RESET) },
        { tr(STR_NO_GO_BACK), tr(STR_YES_RESET_REMOTE) }
    };

    for (int i = 0; i < 2; i++) {
        int y = btn_y + i * (btn_h + spacing);
        bool is_sel = (i == g_selected_sub_item);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : g_theme.pal.outline;
        uint16_t text_col = RGB_WHITE;

        if (i == 1) {
            border = is_sel ? RGB_RED : g_theme.pal.danger_dim;
            text_col = is_sel ? RGB_RED : RGB_YELLOW;
        }

        fb_fill_rect(8, y, FB_WIDTH - 16, btn_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, btn_h, border);
        int tw = fb_text_width_aa(btn_labels[g_factory_reset_stage][i], &g_font_title);
        fb_draw_text_aa((FB_WIDTH - tw) / 2, y + 10, btn_labels[g_factory_reset_stage][i], text_col, &g_font_title);
    }
}

static void render_settings_reboot(void) {
    int card_y = 38;
    int card_h = 130;

    fb_draw_text_aa(8, card_y + 10, tr(STR_REBOOT_REMOTE), RGB_YELLOW, &g_font_title);
    fb_draw_text_aa(8, card_y + 36, "Restart remote cleanly.", RGB_WHITE, &g_font_main);
    fb_draw_text_aa(8, card_y + 56, "Saved configs and paired Hub", RGB_WHITE, &g_font_main);
    fb_draw_text_aa(8, card_y + 76, "connection will be restored.", RGB_WHITE, &g_font_main);
    fb_draw_text_aa(8, card_y + 100, "Device reboots in a few seconds.", RGB_TEAL, &g_font_main);

    (void)card_h;
    int btn_y = SETTINGS_CONFIRM_Y;
    int btn_h = SETTINGS_CONFIRM_H;
    int spacing = SETTINGS_CONFIRM_GAP;

    const char *btn_labels[2] = {
        tr(STR_CANCEL_DO_NOT_REBOOT),
        tr(STR_REBOOT_REMOTE_NOW)
    };

    for (int i = 0; i < 2; i++) {
        int y = btn_y + i * (btn_h + spacing);
        bool is_sel = (i == g_selected_sub_item);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? (i == 1 ? RGB_RED : RGB_TEAL) : (i == 1 ? g_theme.pal.danger_dim : g_theme.pal.outline);
        uint16_t text_col = (i == 1) ? (is_sel ? RGB_RED : RGB_YELLOW) : RGB_WHITE;

        fb_fill_rect(8, y, FB_WIDTH - 16, btn_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, btn_h, border);
        int tw = fb_text_width_aa(btn_labels[i], &g_font_title);
        fb_draw_text_aa((FB_WIDTH - tw) / 2, y + 10, btn_labels[i], text_col, &g_font_title);
    }
}

static void render_settings_suspend(void) {
    int card_y = 38;
    int card_h = 130;

    fb_draw_text_aa(8, card_y + 10, "SUSPEND TO RAM", RGB_TEAL, &g_font_title);
    fb_draw_text_aa(8, card_y + 34, "Suspends Linux kernel to RAM.", RGB_WHITE, &g_font_main);
    fb_draw_text_aa(8, card_y + 54, "Wakes on motion or button.", RGB_WHITE, &g_font_main);

    uint64_t now_ms = get_monotonic_ms();
    uint64_t elapsed_s = (g_suspend_countdown_start_ms > 0 && now_ms >= g_suspend_countdown_start_ms) ?
                         (now_ms - g_suspend_countdown_start_ms) / 1000 : 0;
    int remain_s = (elapsed_s < 10) ? (int)(10 - elapsed_s) : 0;

    read_remote_battery();
    bool on_usb = (strcmp(g_battery_info.powersource, "usb") == 0);

    if (on_usb) {
        fb_draw_text_aa(8, card_y + 78, "Notice: Unplug USB to suspend!", RGB_YELLOW, &g_font_main);
        fb_draw_text_aa(8, card_y + 100, "(USB charging rejects RAM sleep)", RGB_TEXT_MUTED, &g_font_main);
    } else {
        char cdown[48];
        snprintf(cdown, sizeof(cdown), "Suspending in %d seconds...", remain_s);
        fb_draw_text_aa(8, card_y + 78, cdown, RGB_GREEN, &g_font_title);
        fb_draw_text_aa(8, card_y + 104, "Put remote down on table.", RGB_TEAL, &g_font_main);
    }

    (void)card_h;
    int btn_y = SETTINGS_CONFIRM_Y;
    int btn_h = SETTINGS_CONFIRM_H;
    int spacing = SETTINGS_CONFIRM_GAP;

    const char *btn_labels[2] = {
        tr(STR_CANCEL_STAY_AWAKE),
        tr(STR_SUSPEND_TO_RAM_NOW)
    };

    for (int i = 0; i < 2; i++) {
        int y = btn_y + i * (btn_h + spacing);
        bool is_sel = (i == g_selected_sub_item);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? (i == 1 ? RGB_TEAL : RGB_RED) : (i == 1 ? RGB_CARD_BORDER : g_theme.pal.outline);
        uint16_t text_col = (i == 1) ? (is_sel ? RGB_TEAL : RGB_GREEN) : (is_sel ? RGB_WHITE : RGB_TEXT_MUTED);

        fb_fill_rect(8, y, FB_WIDTH - 16, btn_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, btn_h, border);
        int tw = fb_text_width_aa(btn_labels[i], &g_font_title);
        fb_draw_text_aa((FB_WIDTH - tw) / 2, y + 10, btn_labels[i], text_col, &g_font_title);
    }
}

static void render_settings_update(void) {
    fb_draw_text_aa(14, SETTINGS_HEADING_Y, tr(STR_CAT_UPDATE), RGB_CYAN, &g_font_title);

    int start_y = SETTINGS_UPDATE_INFO_Y;
    int card_h = SETTINGS_UPDATE_INFO_H;

    fb_fill_rect(8, start_y, FB_WIDTH - 16, card_h, RGB_CARD);
    fb_draw_rect(8, start_y, FB_WIDTH - 16, card_h, RGB_CARD_BORDER);

    char sw_str[48];
    snprintf(sw_str, sizeof(sw_str), "Remote SW: v%s", HARMONY_REMOTE_VERSION);
    fb_draw_text_aa(16, start_y + 10, sw_str, RGB_WHITE, &g_font_title);

    char hub_ota_str[48];
    bool ota_avail = g_update_ota_available && harmony_version_compare(g_hub_ota_version, HARMONY_REMOTE_VERSION) > 0;
    if (ota_avail) {
        snprintf(hub_ota_str, sizeof(hub_ota_str), "Hub Image: %s [READY]",
                 g_hub_ota_version[0] ? g_hub_ota_version : "Update");
        fb_draw_text_aa(16, start_y + 34, hub_ota_str, RGB_YELLOW, &g_font_main);
    } else {
        snprintf(hub_ota_str, sizeof(hub_ota_str), "Hub Image: [UP TO DATE]");
        fb_draw_text_aa(16, start_y + 34, hub_ota_str, RGB_GREEN, &g_font_main);
    }

    read_remote_battery();
    char batt_str[48];
    snprintf(batt_str, sizeof(batt_str), "Battery: %d%% (%s)",
             g_battery_info.percent,
             g_battery_info.charging ? "Charging" : (g_battery_info.percent >= 40 ? "Ready" : "Req >=40%"));
    fb_draw_text_aa(16, start_y + 54, batt_str,
                    (g_battery_info.charging || g_battery_info.percent >= 40) ? RGB_TEXT_MUTED : RGB_RED,
                    &g_font_main);

    const char *labels[2] = {
        tr(STR_PULL_FIRMWARE_OTA),
        tr(STR_CHECK_UPDATES)
    };
    const char *icons[2] = {
        "/opt/luadraws/appscommon/asset/footer/footermenu_sleeptimer.png",
        "/opt/luadraws/appscommon/asset/footer/settings.png"
    };

    int item_y_start = SETTINGS_UPDATE_Y;
    int item_h = SETTINGS_UPDATE_H;
    int spacing = SETTINGS_UPDATE_GAP;

    for (int i = 0; i < 2; i++) {
        int y = item_y_start + i * (item_h + spacing);
        bool is_sel = (i == g_selected_sub_item);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : g_theme.pal.outline;

        fb_fill_rect(8, y, FB_WIDTH - 16, item_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, item_h, border);

        bool has_icon = blit_png_file(14, y + 7, 20, 20, icons[i]);
        int tx = has_icon ? 40 : 14;

        uint16_t fg = is_sel ? RGB_WHITE : RGB_CYAN;
        int badge_x = (i == 0 && ota_avail) ? FB_WIDTH - 55 : FB_WIDTH - 20;
        if (i == 0 && ota_avail) {
            fb_draw_text_aa(badge_x, y + 9, "[READY]", RGB_YELLOW, &g_font_main);
        } else {
            fb_draw_text_aa(badge_x, y + 9, ">", is_sel ? RGB_TEAL : RGB_TEXT_MUTED, &g_font_main);
        }
        char fit[64];
        fb_draw_text_aa(tx, y + 9, fb_fit_text_aa(fit, sizeof(fit), labels[i], &g_font_main, badge_x - 4 - tx), fg, &g_font_main);
    }
}

void render_settings_screen(void) {
    switch (g_settings_subview) {
        case SETTINGS_VIEW_CATEGORIES:
            render_settings_categories();
            break;
        case SETTINGS_VIEW_ACTIVITIES: {
            const char *labels[4] = { tr(STR_CONFIRM_SWITCH), tr(STR_LANGUAGE), tr(STR_SYNC_CONFIG_HUB), tr(STR_ACTIVITY_ORDER) };
            char values[4][32];
            snprintf(values[0], sizeof(values[0]), "[ %s ]", g_confirm_activity ? tr(STR_CONFIRM_PROMPT) : tr(STR_CONFIRM_INSTANT));
            snprintf(values[1], sizeof(values[1]), "[ %s ]", remote_lang_name(g_language));
            snprintf(values[2], sizeof(values[2]), "v%d %s", g_config_version, g_update_config_available ? "[NEW]" : "[OK]");
            snprintf(values[3], sizeof(values[3]), "[ %s ]", g_activity_order == ACTIVITY_ORDER_MOST_USED ? "MOST USED" : "YOURS");
            uint16_t cols[4] = {
                g_confirm_activity ? RGB_GREEN : RGB_TEXT_MUTED,
                RGB_TEAL,
                g_update_config_available ? RGB_YELLOW : RGB_TEAL,
                RGB_TEAL
            };
            const char *icons[4] = {
                "/opt/luadraws/appscommon/asset/settings/checkmark.png",
                "/opt/luadraws/appscommon/asset/footer/settings.png",
                "/opt/luadraws/appscommon/asset/footer/devices.png",
                "/opt/luadraws/appscommon/asset/footer/menu.png"
            };
            render_settings_items(tr(STR_CAT_CONFIG), 4, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_DISPLAY: {
            const char *labels[8] = { tr(STR_DISPLAY_BRIGHTNESS), tr(STR_DISPLAY_TIMEOUT), tr(STR_BTN_BACKLIGHT), tr(STR_NIGHT_START), tr(STR_NIGHT_END), tr(STR_DISPLAY_THEME), tr(STR_HOME_LAYOUT), tr(STR_CLEAN_WAKE) };
            static const char *const layouts[HOME_LAYOUT_COUNT] = { "THEME", "LIST", "GRID", "CAROUSEL" };
            char values[8][32];
            snprintf(values[0], sizeof(values[0]), "[ LVL %d ]", g_brightness_val);
            snprintf(values[1], sizeof(values[1]), "[ %ds ]", g_timeout_seconds);
            snprintf(values[2], sizeof(values[2]), "[ %s ]", g_btn_backlight_mode == 0 ? "OFF" : (g_btn_backlight_mode == 1 ? "ON" : "NIGHT"));
            snprintf(values[3], sizeof(values[3]), "[ %02d:00 ]", g_night_start_hour);
            snprintf(values[4], sizeof(values[4]), "[ %02d:00 ]", g_night_end_hour);
            snprintf(values[5], sizeof(values[5]), "[ %s ]", g_theme.label);
            snprintf(values[6], sizeof(values[6]), "[ %s ]", g_physical_nav_enabled ? "LIST" : layouts[g_home_layout]);
            snprintf(values[7], sizeof(values[7]), "[ %s ]", g_clean_wake ? "ON" : "OFF");
            uint16_t cols[8] = {
                RGB_TEAL,
                RGB_TEAL,
                g_btn_backlight_mode == 0 ? RGB_RED : (g_btn_backlight_mode == 1 ? RGB_GREEN : RGB_YELLOW),
                RGB_TEAL,
                RGB_TEAL,
                RGB_TEAL,
                RGB_TEAL,
                g_clean_wake ? RGB_GREEN : RGB_TEAL
            };
            const char *icons[8] = {
                "/opt/luadraws/appscommon/asset/hue/brightness.png",
                "/opt/luadraws/appscommon/asset/commands/145.png",
                "/opt/luadraws/appscommon/asset/juniperHomeAutomation/lamp_on.png",
                "/opt/luadraws/appscommon/asset/clock_small.png",
                "/opt/luadraws/appscommon/asset/clock_small.png",
                "/opt/luadraws/appscommon/asset/hue/color.png",
                "/opt/luadraws/appscommon/asset/footer/menu.png",
                "/opt/luadraws/appscommon/asset/battery/level100.png"
            };
            render_settings_items(tr(STR_CAT_DISPLAY), 8, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_TOUCH: {
            const char *labels[5] = { tr(STR_TOUCHSCREEN), tr(STR_CAP_BUTTONS), tr(STR_TOUCH_SENSITIVITY), tr(STR_PHYSICAL_NAV),
                                      tr(STR_HAPTIC_FEEDBACK) };
            char values[5][32];
            snprintf(values[0], sizeof(values[0]), "[ %s ]", g_touch_enabled ? "ON" : "OFF");
            snprintf(values[1], sizeof(values[1]), "[ %s ]", g_cap_buttons_enabled ? "ON" : "OFF");
            const char *sens_names[] = { "", "LOW", "NORMAL", "HIGH" };
            int s_idx = (g_touch_sensitivity >= 1 && g_touch_sensitivity <= 3) ? g_touch_sensitivity : 2;
            snprintf(values[2], sizeof(values[2]), "[ %s ]", sens_names[s_idx]);
            snprintf(values[3], sizeof(values[3]), "[ %s ]", g_physical_nav_enabled ? "ON" : "OFF");
            if (g_haptic_ms > 0) snprintf(values[4], sizeof(values[4]), "[ %d ms ]", g_haptic_ms);
            else snprintf(values[4], sizeof(values[4]), "[ OFF ]");
            uint16_t cols[5] = {
                g_touch_enabled ? RGB_GREEN : RGB_RED,
                g_cap_buttons_enabled ? RGB_GREEN : RGB_RED,
                RGB_TEAL,
                g_physical_nav_enabled ? RGB_GREEN : RGB_RED,
                g_haptic_ms > 0 ? RGB_GREEN : RGB_RED
            };
            const char *icons[5] = {
                "/opt/luadraws/appscommon/asset/gesture/hint.png",
                "/opt/luadraws/appscommon/asset/footer/devices.png",
                "/opt/luadraws/appscommon/asset/gesture/gesturehand.png",
                "/opt/luadraws/appscommon/asset/edit/arrow.png",
                "/opt/luadraws/appscommon/asset/footer/gesture_cue.png"
            };
            render_settings_items(tr(STR_CAT_TOUCH), 5, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_POWER: {
            const char *labels[5] = { tr(STR_TILT_WAKE), tr(STR_DEEP_STANDBY), tr(STR_BATTERY_LEVEL), tr(STR_DEBUG_LOGGING), tr(STR_SUSPEND_RAM) };
            char values[5][32];
            snprintf(values[0], sizeof(values[0]), "[ %s ]", g_gyro_enabled ? "ON" : "OFF");
            if (g_deep_standby_min == 0) {
                snprintf(values[1], sizeof(values[1]), "[ NEVER ]");
            } else if (g_deep_standby_min < 0) {
                snprintf(values[1], sizeof(values[1]), "[ AT ONCE ]");
            } else {
                snprintf(values[1], sizeof(values[1]), "[ %dm ]", g_deep_standby_min);
            }
            read_remote_battery();
            snprintf(values[2], sizeof(values[2]), "[ %d%% %dmV ]", g_battery_info.percent, g_battery_info.voltage_mv);
            snprintf(values[3], sizeof(values[3]), "[ %s ]", g_power_debug ? "ON" : "OFF");
            snprintf(values[4], sizeof(values[4]), "[ SLEEP ]");
            uint16_t cols[5] = {
                g_gyro_enabled ? RGB_GREEN : RGB_RED,
                (g_deep_standby_min == 0) ? RGB_YELLOW : RGB_TEAL,
                (g_battery_info.percent < 20) ? RGB_RED : RGB_GREEN,
                g_power_debug ? RGB_GREEN : RGB_TEXT_MUTED,
                RGB_TEAL
            };
            const char *icons[5] = {
                "/opt/luadraws/appscommon/asset/footer/flip.png",
                "/opt/luadraws/appscommon/asset/footer/footermenu_sleeptimer.png",
                "/opt/luadraws/appscommon/asset/battery/level100.png",
                "/opt/luadraws/appscommon/asset/header/charging.png",
                "/opt/luadraws/appscommon/asset/footer/footermenu_sleeptimer.png"
            };
            render_settings_items(tr(STR_CAT_POWER), 5, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_SYSTEM: {
            const char *labels[3] = { tr(STR_RF_PAIRING_LINK), tr(STR_REBOOT_REMOTE), tr(STR_FACTORY_RESET) };
            char values[3][32];
            uint16_t cols[3];

            snprintf(values[0], sizeof(values[0]), "[ %s ]", g_rf_link_connected ? "LINKED" : "OPEN");
            cols[0] = g_rf_link_connected ? RGB_GREEN : RGB_YELLOW;
            snprintf(values[1], sizeof(values[1]), "[ REBOOT ]");
            cols[1] = RGB_YELLOW;
            snprintf(values[2], sizeof(values[2]), "[ REVERT ]");
            cols[2] = RGB_RED;
            const char *icons[3] = {
                "/opt/luadraws/appscommon/asset/pairing/connectHub.png",
                "/opt/luadraws/appscommon/asset/home/power_icon.png",
                "/opt/luadraws/appscommon/asset/footer/close.png"
            };
            render_settings_items(tr(STR_CAT_SYSTEM), 3, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_UPDATE:
            render_settings_update();
            break;
        case SETTINGS_VIEW_REBOOT:
            render_settings_reboot();
            break;
        case SETTINGS_VIEW_SUSPEND:
            render_settings_suspend();
            break;
        case SETTINGS_VIEW_FACTORY_RESET:
            render_settings_factory_reset();
            break;
        default:
            render_settings_categories();
            break;
    }
}
