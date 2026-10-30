#include "elite_ui.h"
#include "elite_fb.h"
#include "elite_power.h"
#include "elite_config.h"
#include "elite_rf.h"
#include "harmony_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>

static const char *get_nav_key_name(int key) {
    switch (key) {
        case 139: return "MENU";
        case 358: return "INFO";
        case 395: return "GUIDE";
        case 389: return "DVR";
        case 174: return "EXIT";
        default:  return "MENU";
    }
}

int get_settings_sub_count(void) {
    switch (g_settings_subview) {
        case SETTINGS_VIEW_CATEGORIES: return 6;
        case SETTINGS_VIEW_ACTIVITIES: return 4;
        case SETTINGS_VIEW_DISPLAY:    return 5;
        case SETTINGS_VIEW_TOUCH:      return 6;
        case SETTINGS_VIEW_POWER:      return 6;
        case SETTINGS_VIEW_SYSTEM:     return 5;
        case SETTINGS_VIEW_UPDATE:     return 3;
        case SETTINGS_VIEW_FACTORY_RESET: return 2;
        case SETTINGS_VIEW_REBOOT:        return 2;
        case SETTINGS_VIEW_SUSPEND:       return 2;
        default: return 0;
    }
}

static void render_settings_categories(void) {
    int start_y = 38;
    int card_h = 35;
    int spacing = 4;
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
        uint16_t border = is_sel ? RGB_TEAL : 0x2945;

        fb_fill_rect(8, y, FB_WIDTH - 16, card_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, card_h, border);

        bool has_icon = blit_png_file(14, y + 7, 20, 20, cat_icons[i]);
        int text_x = has_icon ? 40 : 14;
        fb_draw_text_aa(text_x, y + 3, cat_names[i], is_sel ? RGB_WHITE : RGB_CYAN, &g_font_title);
        fb_draw_text_aa(text_x, y + 19, cat_descs[i], RGB_TEXT_MUTED, &g_font_main);

        bool ota_avail = g_update_ota_available && harmony_version_compare(g_hub_ota_version, HARMONY_ELITE_VERSION) > 0;
        if (i == 5 && ota_avail) {
            fb_draw_text_aa(FB_WIDTH - 64, y + 10, "[UPDATE]", RGB_YELLOW, &g_font_main);
        } else if (i == 0 && g_update_config_available) {
            fb_draw_text_aa(FB_WIDTH - 55, y + 10, "[SYNC]", RGB_TEAL, &g_font_main);
        } else {
            fb_draw_text_aa(FB_WIDTH - 20, y + 9, ">", is_sel ? RGB_TEAL : RGB_TEXT_MUTED, &g_font_title);
        }
    }
}

static void render_settings_items(const char *title, int count, const char **labels, const char values[][32], const uint16_t *val_colors, const char **icons, int sel_idx) {
    fb_fill_rect(0, 0, FB_WIDTH, 36, RGB_BLACK);
    fb_draw_text_aa(14, 10, title, RGB_CYAN, &g_font_title);

    int start_y = 40;
    int card_h = (count >= 7) ? 30 : ((count >= 6) ? 34 : 36);
    int spacing = (count >= 7) ? 3 : ((count >= 6) ? 4 : 5);

    for (int i = 0; i < count; i++) {
        int y = start_y + i * (card_h + spacing);
        bool is_sel = (i == sel_idx);
        bool is_back = (i == count - 1);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : 0x2945;

        fb_fill_rect(8, y, FB_WIDTH - 16, card_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, card_h, border);

        bool has_icon = false;
        if (icons && icons[i]) {
            has_icon = blit_png_file(14, y + (card_h <= 30 ? 5 : (card_h == 34 ? 7 : 8)), 20, 20, icons[i]);
        }
        int text_x = has_icon ? 38 : 14;
        uint16_t text_col = is_back ? (is_sel ? RGB_WHITE : RGB_CYAN) : (is_sel ? RGB_WHITE : RGB_TEXT_MUTED);
        fb_draw_text_aa(text_x, y + (card_h <= 30 ? 6 : (card_h == 34 ? 8 : 9)), labels[i], text_col, &g_font_main);

        if (values && values[i][0]) {
            int vw = fb_text_width_aa(values[i], &g_font_main);
            uint16_t col = val_colors ? val_colors[i] : RGB_TEAL;
            fb_draw_text_aa(FB_WIDTH - 18 - vw, y + (card_h <= 30 ? 6 : (card_h == 34 ? 8 : 9)), values[i], col, &g_font_main);
        }
    }
}

static void render_settings_factory_reset(void) {
    fb_fill_rect(0, 0, FB_WIDTH, 36, RGB_BLACK);
    char title[32];
    snprintf(title, sizeof(title), "RESET (%d/2)", g_factory_reset_stage + 1);
    fb_draw_text_aa(14, 10, title, RGB_RED, &g_font_title);

    int card_y = 38;
    int card_h = 130;
    fb_fill_rect(8, card_y, FB_WIDTH - 16, card_h, RGB_CARD);
    fb_draw_rect(8, card_y, FB_WIDTH - 16, card_h, g_factory_reset_stage == 0 ? RGB_YELLOW : RGB_RED);

    if (g_factory_reset_stage == 0) {
        fb_draw_text_aa(16, card_y + 10, "REVERT TO STOCK FIRMWARE", RGB_YELLOW, &g_font_title);
        fb_draw_text_aa(16, card_y + 36, "Removes custom software", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(16, card_y + 56, "and restores official Logitech UI.", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(16, card_y + 82, "Configs preserved on flash:", RGB_TEAL, &g_font_main);
        fb_draw_text_aa(16, card_y + 100, "/data/elite_settings.conf", RGB_GREEN, &g_font_main);
    } else {
        fb_draw_text_aa(16, card_y + 10, "ARE YOU ABSOLUTELY SURE?", RGB_RED, &g_font_title);
        fb_draw_text_aa(16, card_y + 36, "Remote will restore stock startup,", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(16, card_y + 56, "sync flash, and reboot to Logitech.", RGB_WHITE, &g_font_main);
        fb_draw_text_aa(16, card_y + 82, "Re-running install restores setups.", RGB_TEAL, &g_font_main);
        fb_draw_text_aa(16, card_y + 100, "Device reboots immediately.", RGB_YELLOW, &g_font_main);
    }

    int btn_y = card_y + card_h + 10;
    int btn_h = 38;
    int spacing = 8;

    const char *btn_labels[2][2] = {
        { tr(STR_CANCEL_KEEP_SYSTEM), tr(STR_PROCEED_TO_RESET) },
        { tr(STR_NO_GO_BACK), tr(STR_YES_RESET_REMOTE) }
    };

    for (int i = 0; i < 2; i++) {
        int y = btn_y + i * (btn_h + spacing);
        bool is_sel = (i == g_selected_sub_item);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : 0x2945;
        uint16_t text_col = RGB_WHITE;

        if (i == 1) {
            border = is_sel ? RGB_RED : 0x8000;
            text_col = is_sel ? RGB_RED : RGB_YELLOW;
        }

        fb_fill_rect(8, y, FB_WIDTH - 16, btn_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, btn_h, border);
        int tw = fb_text_width_aa(btn_labels[g_factory_reset_stage][i], &g_font_title);
        fb_draw_text_aa((FB_WIDTH - tw) / 2, y + 10, btn_labels[g_factory_reset_stage][i], text_col, &g_font_title);
    }
}

static void render_settings_reboot(void) {
    render_header();

    int card_y = 38;
    int card_h = 130;
    fb_fill_rect(8, card_y, FB_WIDTH - 16, card_h, RGB_CARD);
    fb_draw_rect(8, card_y, FB_WIDTH - 16, card_h, RGB_YELLOW);

    fb_draw_text_aa(16, card_y + 10, tr(STR_REBOOT_REMOTE), RGB_YELLOW, &g_font_title);
    fb_draw_text_aa(16, card_y + 36, "Restart remote cleanly.", RGB_WHITE, &g_font_main);
    fb_draw_text_aa(16, card_y + 56, "Saved configs and paired Hub", RGB_WHITE, &g_font_main);
    fb_draw_text_aa(16, card_y + 76, "connection will be restored.", RGB_WHITE, &g_font_main);
    fb_draw_text_aa(16, card_y + 100, "Device reboots in a few seconds.", RGB_TEAL, &g_font_main);

    int btn_y = card_y + card_h + 10;
    int btn_h = 38;
    int spacing = 8;

    const char *btn_labels[2] = {
        tr(STR_CANCEL_DO_NOT_REBOOT),
        tr(STR_REBOOT_REMOTE_NOW)
    };

    for (int i = 0; i < 2; i++) {
        int y = btn_y + i * (btn_h + spacing);
        bool is_sel = (i == g_selected_sub_item);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? (i == 1 ? RGB_RED : RGB_TEAL) : (i == 1 ? 0x8000 : 0x2945);
        uint16_t text_col = (i == 1) ? (is_sel ? RGB_RED : RGB_YELLOW) : RGB_WHITE;

        fb_fill_rect(8, y, FB_WIDTH - 16, btn_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, btn_h, border);
        int tw = fb_text_width_aa(btn_labels[i], &g_font_title);
        fb_draw_text_aa((FB_WIDTH - tw) / 2, y + 10, btn_labels[i], text_col, &g_font_title);
    }
}

static void render_settings_suspend(void) {
    render_header();

    int card_y = 38;
    int card_h = 130;
    fb_fill_rect(8, card_y, FB_WIDTH - 16, card_h, RGB_CARD);
    fb_draw_rect(8, card_y, FB_WIDTH - 16, card_h, RGB_TEAL);

    fb_draw_text_aa(16, card_y + 10, "SUSPEND TO RAM", RGB_TEAL, &g_font_title);
    fb_draw_text_aa(16, card_y + 34, "Suspends Linux kernel to RAM.", RGB_WHITE, &g_font_main);
    fb_draw_text_aa(16, card_y + 54, "Wakes on motion or button.", RGB_WHITE, &g_font_main);

    uint64_t now_ms = get_monotonic_ms();
    uint64_t elapsed_s = (g_suspend_countdown_start_ms > 0 && now_ms >= g_suspend_countdown_start_ms) ?
                         (now_ms - g_suspend_countdown_start_ms) / 1000 : 0;
    int remain_s = (elapsed_s < 10) ? (int)(10 - elapsed_s) : 0;

    read_elite_battery();
    bool on_usb = (strcmp(g_battery_info.powersource, "usb") == 0);

    if (on_usb) {
        fb_draw_text_aa(16, card_y + 78, "Notice: Unplug USB to suspend!", RGB_YELLOW, &g_font_main);
        fb_draw_text_aa(16, card_y + 100, "(USB charging rejects RAM sleep)", RGB_TEXT_MUTED, &g_font_main);
    } else {
        char cdown[48];
        snprintf(cdown, sizeof(cdown), "Suspending in %d seconds...", remain_s);
        fb_draw_text_aa(16, card_y + 78, cdown, RGB_GREEN, &g_font_title);
        fb_draw_text_aa(16, card_y + 104, "Put remote down on table.", RGB_TEAL, &g_font_main);
    }

    int btn_y = card_y + card_h + 10;
    int btn_h = 38;
    int spacing = 8;

    const char *btn_labels[2] = {
        tr(STR_CANCEL_STAY_AWAKE),
        tr(STR_SUSPEND_TO_RAM_NOW)
    };

    for (int i = 0; i < 2; i++) {
        int y = btn_y + i * (btn_h + spacing);
        bool is_sel = (i == g_selected_sub_item);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? (i == 1 ? RGB_TEAL : RGB_RED) : (i == 1 ? RGB_CARD_BORDER : 0x2945);
        uint16_t text_col = (i == 1) ? (is_sel ? RGB_TEAL : RGB_GREEN) : (is_sel ? RGB_WHITE : RGB_TEXT_MUTED);

        fb_fill_rect(8, y, FB_WIDTH - 16, btn_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, btn_h, border);
        int tw = fb_text_width_aa(btn_labels[i], &g_font_title);
        fb_draw_text_aa((FB_WIDTH - tw) / 2, y + 10, btn_labels[i], text_col, &g_font_title);
    }
}

static void render_settings_update(void) {
    fb_fill_rect(0, 0, FB_WIDTH, 36, RGB_BLACK);
    fb_draw_text_aa(14, 10, tr(STR_CAT_UPDATE), RGB_CYAN, &g_font_title);

    int start_y = 38;
    int card_h = 76;

    fb_fill_rect(8, start_y, FB_WIDTH - 16, card_h, RGB_CARD);
    fb_draw_rect(8, start_y, FB_WIDTH - 16, card_h, RGB_CARD_BORDER);

    char sw_str[48];
    snprintf(sw_str, sizeof(sw_str), "Elite SW: v%s", HARMONY_ELITE_VERSION);
    fb_draw_text_aa(16, start_y + 10, sw_str, RGB_WHITE, &g_font_title);

    char hub_ota_str[48];
    bool ota_avail = g_update_ota_available && harmony_version_compare(g_hub_ota_version, HARMONY_ELITE_VERSION) > 0;
    if (ota_avail) {
        snprintf(hub_ota_str, sizeof(hub_ota_str), "Hub Image: %s [READY]",
                 g_hub_ota_version[0] ? g_hub_ota_version : "Update");
        fb_draw_text_aa(16, start_y + 34, hub_ota_str, RGB_YELLOW, &g_font_main);
    } else {
        snprintf(hub_ota_str, sizeof(hub_ota_str), "Hub Image: [UP TO DATE]");
        fb_draw_text_aa(16, start_y + 34, hub_ota_str, RGB_GREEN, &g_font_main);
    }

    read_elite_battery();
    char batt_str[48];
    snprintf(batt_str, sizeof(batt_str), "Battery: %d%% (%s)",
             g_battery_info.percent,
             g_battery_info.charging ? "Charging" : (g_battery_info.percent >= 40 ? "Ready" : "Req >=40%"));
    fb_draw_text_aa(16, start_y + 54, batt_str,
                    (g_battery_info.charging || g_battery_info.percent >= 40) ? RGB_TEXT_MUTED : RGB_RED,
                    &g_font_main);

    const char *labels[3] = {
        tr(STR_PULL_FIRMWARE_OTA),
        tr(STR_CHECK_UPDATES),
        tr(STR_BACK_TO_SETTINGS)
    };
    const char *icons[3] = {
        "/opt/luadraws/appscommon/asset/footer/footermenu_sleeptimer.png",
        "/opt/luadraws/appscommon/asset/footer/settings.png",
        "/opt/luadraws/appscommon/asset/footer/close.png"
    };

    int item_y_start = start_y + card_h + 6;
    int item_h = 34;
    int spacing = 5;

    for (int i = 0; i < 3; i++) {
        int y = item_y_start + i * (item_h + spacing);
        bool is_sel = (i == g_selected_sub_item);
        bool is_back = (i == 2);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? (is_back ? RGB_TEXT_MUTED : RGB_TEAL) : 0x2945;

        fb_fill_rect(8, y, FB_WIDTH - 16, item_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, item_h, border);

        bool has_icon = blit_png_file(14, y + 7, 20, 20, icons[i]);
        int tx = has_icon ? 40 : 14;

        uint16_t fg = is_sel ? RGB_WHITE : (is_back ? RGB_TEXT_MUTED : RGB_CYAN);
        fb_draw_text_aa(tx, y + 9, labels[i], fg, &g_font_main);

        if (i == 0 && ota_avail) {
            fb_draw_text_aa(FB_WIDTH - 55, y + 9, "[READY]", RGB_YELLOW, &g_font_main);
        } else if (!is_back) {
            fb_draw_text_aa(FB_WIDTH - 20, y + 9, ">", is_sel ? RGB_TEAL : RGB_TEXT_MUTED, &g_font_main);
        }
    }
}

void render_settings_screen(void) {
    switch (g_settings_subview) {
        case SETTINGS_VIEW_CATEGORIES:
            render_settings_categories();
            break;
        case SETTINGS_VIEW_ACTIVITIES: {
            const char *labels[4] = { tr(STR_CONFIRM_SWITCH), tr(STR_LANGUAGE), tr(STR_SYNC_CONFIG_HUB), tr(STR_BACK) };
            char values[4][32];
            snprintf(values[0], sizeof(values[0]), "[ %s ]", g_confirm_activity ? tr(STR_CONFIRM_PROMPT) : tr(STR_CONFIRM_INSTANT));
            snprintf(values[1], sizeof(values[1]), "[ %s ]", elite_lang_name(g_language));
            snprintf(values[2], sizeof(values[2]), "v%d %s", g_config_version, g_update_config_available ? "[NEW]" : "[OK]");
            values[3][0] = '\0';
            uint16_t cols[4] = {
                g_confirm_activity ? RGB_GREEN : RGB_TEXT_MUTED,
                RGB_TEAL,
                g_update_config_available ? RGB_YELLOW : RGB_TEAL,
                RGB_CYAN
            };
            const char *icons[4] = {
                "/opt/luadraws/appscommon/asset/settings/checkmark.png",
                "/opt/luadraws/appscommon/asset/footer/settings.png",
                "/opt/luadraws/appscommon/asset/footer/devices.png",
                "/opt/luadraws/appscommon/asset/footer/close.png"
            };
            render_settings_items(tr(STR_CAT_CONFIG), 4, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_DISPLAY: {
            const char *labels[5] = { tr(STR_DISPLAY_BRIGHTNESS), tr(STR_DISPLAY_TIMEOUT), tr(STR_BTN_BACKLIGHT), tr(STR_NIGHT_SCHEDULE), tr(STR_BACK) };
            char values[5][32];
            snprintf(values[0], sizeof(values[0]), "[ LVL %d ]", g_brightness_val);
            snprintf(values[1], sizeof(values[1]), "[ %ds ]", g_timeout_seconds);
            snprintf(values[2], sizeof(values[2]), "[ %s ]", g_btn_backlight_mode == 0 ? "OFF" : (g_btn_backlight_mode == 1 ? "ON" : "NIGHT"));
            snprintf(values[3], sizeof(values[3]), "[ %02d-%02d ]", g_night_start_hour, g_night_end_hour);
            values[4][0] = '\0';
            uint16_t cols[5] = {
                RGB_TEAL,
                RGB_TEAL,
                g_btn_backlight_mode == 0 ? RGB_RED : (g_btn_backlight_mode == 1 ? RGB_GREEN : RGB_YELLOW),
                RGB_TEAL,
                RGB_CYAN
            };
            const char *icons[5] = {
                "/opt/luadraws/appscommon/asset/hue/brightness.png",
                "/opt/luadraws/appscommon/asset/commands/145.png",
                "/opt/luadraws/appscommon/asset/juniperHomeAutomation/lamp_on.png",
                "/opt/luadraws/appscommon/asset/clock_small.png",
                "/opt/luadraws/appscommon/asset/footer/close.png"
            };
            render_settings_items(tr(STR_CAT_DISPLAY), 5, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_TOUCH: {
            const char *labels[6] = { tr(STR_TOUCHSCREEN), tr(STR_CAP_BUTTONS), tr(STR_TOUCH_SENSITIVITY), tr(STR_NAV_TOGGLE_BTN), tr(STR_NAV_TOGGLE_MODE), tr(STR_BACK) };
            char values[6][32];
            snprintf(values[0], sizeof(values[0]), "[ %s ]", g_touch_enabled ? "ON" : "OFF");
            snprintf(values[1], sizeof(values[1]), "[ %s ]", g_cap_buttons_enabled ? "ON" : "OFF");
            const char *sens_names[] = { "", "LOW", "NORMAL", "HIGH" };
            int s_idx = (g_touch_sensitivity >= 1 && g_touch_sensitivity <= 3) ? g_touch_sensitivity : 2;
            snprintf(values[2], sizeof(values[2]), "[ %s ]", sens_names[s_idx]);
            if (!g_touch_enabled) {
                snprintf(values[3], sizeof(values[3]), "[ LOCKED ]");
                snprintf(values[4], sizeof(values[4]), "[ LOCKED ]");
            } else {
                snprintf(values[3], sizeof(values[3]), "[ %s ]", get_nav_key_name(g_nav_toggle_key));
                snprintf(values[4], sizeof(values[4]), "[ %s ]", g_nav_toggle_mode == 0 ? "SHORT" : "LONG");
            }
            values[5][0] = '\0';
            uint16_t cols[6] = {
                g_touch_enabled ? RGB_GREEN : RGB_RED,
                g_cap_buttons_enabled ? RGB_GREEN : RGB_RED,
                RGB_TEAL,
                !g_touch_enabled ? RGB_TEXT_MUTED : RGB_TEAL,
                !g_touch_enabled ? RGB_TEXT_MUTED : RGB_TEAL,
                RGB_CYAN
            };
            const char *icons[6] = {
                "/opt/luadraws/appscommon/asset/gesture/hint.png",
                "/opt/luadraws/appscommon/asset/footer/devices.png",
                "/opt/luadraws/appscommon/asset/gesture/gesturehand.png",
                "/opt/luadraws/appscommon/asset/edit/arrow.png",
                "/opt/luadraws/appscommon/asset/edit/arrow.png",
                "/opt/luadraws/appscommon/asset/footer/close.png"
            };
            render_settings_items(tr(STR_CAT_TOUCH), 6, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_POWER: {
            const char *labels[6] = { tr(STR_TILT_WAKE), tr(STR_GYRO_RATE), tr(STR_BATTERY_LEVEL), tr(STR_DEBUG_LOGGING), tr(STR_SUSPEND_RAM), tr(STR_BACK) };
            char values[6][32];
            snprintf(values[0], sizeof(values[0]), "[ %s ]", g_gyro_enabled ? "ON" : "OFF");
            snprintf(values[1], sizeof(values[1]), "[ %dHz ]", g_gyro_rate_hz);
            read_elite_battery();
            snprintf(values[2], sizeof(values[2]), "[ %d%% %dmV ]", g_battery_info.percent, g_battery_info.voltage_mv);
            snprintf(values[3], sizeof(values[3]), "[ %s ]", g_power_debug ? "ON" : "OFF");
            snprintf(values[4], sizeof(values[4]), "[ SLEEP ]");
            values[5][0] = '\0';
            uint16_t cols[6] = {
                g_gyro_enabled ? RGB_GREEN : RGB_RED,
                RGB_TEAL,
                (g_battery_info.percent < 20) ? RGB_RED : RGB_GREEN,
                g_power_debug ? RGB_GREEN : RGB_TEXT_MUTED,
                RGB_TEAL,
                RGB_CYAN
            };
            const char *icons[6] = {
                "/opt/luadraws/appscommon/asset/footer/flip.png",
                "/opt/luadraws/appscommon/asset/footer/flip.png",
                "/opt/luadraws/appscommon/asset/battery/level100.png",
                "/opt/luadraws/appscommon/asset/header/charging.png",
                "/opt/luadraws/appscommon/asset/footer/footermenu_sleeptimer.png",
                "/opt/luadraws/appscommon/asset/footer/close.png"
            };
            render_settings_items(tr(STR_CAT_POWER), 6, labels, values, cols, icons, g_selected_sub_item);
            break;
        }
        case SETTINGS_VIEW_SYSTEM: {
            const char *labels[5] = { tr(STR_USB_ETHERNET), tr(STR_RF_PAIRING_LINK), tr(STR_REBOOT_REMOTE), tr(STR_FACTORY_RESET), tr(STR_BACK) };
            char values[5][32];
            uint16_t cols[5];

            if (!g_usbeth_enabled) {
                snprintf(values[0], sizeof(values[0]), "[ OFF ]");
                cols[0] = RGB_TEXT_MUTED;
            } else {
                if (g_usb_connected) {
                    snprintf(values[0], sizeof(values[0]), "[ ACTIVE ]");
                    cols[0] = RGB_GREEN;
                } else {
                    snprintf(values[0], sizeof(values[0]), "[ UNPLUGGED ]");
                    cols[0] = RGB_YELLOW;
                }
            }
            snprintf(values[1], sizeof(values[1]), "[ %s ]", g_rf_link_connected ? "LINKED" : "OPEN");
            cols[1] = g_rf_link_connected ? RGB_GREEN : RGB_YELLOW;
            snprintf(values[2], sizeof(values[2]), "[ REBOOT ]");
            cols[2] = RGB_YELLOW;
            snprintf(values[3], sizeof(values[3]), "[ REVERT ]");
            cols[3] = RGB_RED;
            values[4][0] = '\0';
            cols[4] = RGB_CYAN;
            const char *icons[5] = {
                "/opt/luadraws/appscommon/asset/header/usb.png",
                "/opt/luadraws/appscommon/asset/pairing/connectHub.png",
                "/opt/luadraws/appscommon/asset/home/power_icon.png",
                "/opt/luadraws/appscommon/asset/footer/close.png",
                "/opt/luadraws/appscommon/asset/footer/close.png"
            };
            render_settings_items(tr(STR_CAT_SYSTEM), 5, labels, values, cols, icons, g_selected_sub_item);
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
