#include "elite_ui.h"
#include "elite_fb.h"
#include "elite_power.h"
#include "elite_rf.h"
#include "elite_rf_ota.h"

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

void render_header(void) {
    if (!blit_png_file(0, 0, FB_WIDTH, 34, "/opt/luadraws/appscommon/asset/footer/toolbarBG.png")) {
        fb_fill_rect(0, 0, FB_WIDTH, 34, 0x0841);
    }

    const char *title = (g_current_view == VIEW_ACTIVITIES) ? "ACTIVITIES" :
                        ((g_current_view == VIEW_DEVICES) ? "COMMANDS" :
                        ((g_current_view == VIEW_PAIRING) ? "RF PAIRING" :
                        ((g_current_view == VIEW_NOT_PAIRED) ? "NOT PAIRED" : "SETTINGS")));
    char browser_title[32];
    if (g_current_view == VIEW_DEVICE_BROWSER) {
        bool in_dev = (g_browse_dev >= 0 && g_browse_dev < g_rf_device_count);
        if (in_dev) {
            snprintf(browser_title, sizeof(browser_title), "< %s", g_rf_devices[g_browse_dev].name);
        } else {
            snprintf(browser_title, sizeof(browser_title), "DEVICES");
        }
        /* Keep the title clear of the RF badge at FB_WIDTH - 142 */
        size_t len = strlen(browser_title);
        while (len > 3 && fb_text_width_aa(browser_title, &g_font_title) > FB_WIDTH - 142 - 14) {
            browser_title[--len] = '\0';
        }
        title = browser_title;
    }
    fb_draw_text_aa(10, 9, title, RGB_WHITE, &g_font_title);

    bool ota_avail = g_update_ota_available && harmony_version_compare(g_hub_ota_version, HARMONY_ELITE_VERSION) > 0;
    if (g_update_config_available || ota_avail) {
        int notif_x = 76;
        int notif_y = 9;
        int notif_w = 18;
        int notif_h = 16;
        uint16_t n_bg = ota_avail ? RGB_YELLOW : RGB_TEAL;
        uint16_t n_fg = RGB_BLACK;

        fb_fill_rect(notif_x, notif_y, notif_w, notif_h, n_bg);
        fb_draw_rect(notif_x, notif_y, notif_w, notif_h, RGB_WHITE);

        fb_fill_rect(notif_x + 8, notif_y + 3, 2, 5, n_fg);
        fb_fill_rect(notif_x + 5, notif_y + 8, 8, 1, n_fg);
        fb_fill_rect(notif_x + 6, notif_y + 9, 6, 1, n_fg);
        fb_fill_rect(notif_x + 7, notif_y + 10, 4, 1, n_fg);
        fb_fill_rect(notif_x + 8, notif_y + 11, 2, 1, n_fg);
        fb_fill_rect(notif_x + 4, notif_y + 13, 10, 1, n_fg);
    }

    int nav_x = 112;
    int nav_y = 9;
    int nav_w = 22;
    int nav_h = 16;
    if (g_screen_nav_mode) {
        fb_fill_rect(nav_x, nav_y, nav_w, nav_h, RGB_TEAL);
        fb_draw_rect(nav_x, nav_y, nav_w, nav_h, RGB_WHITE);
        int cx = nav_x + nav_w / 2;
        int cy = nav_y + nav_h / 2;
        fb_fill_rect(cx - 1, cy - 5, 3, 11, RGB_BLACK);
        fb_fill_rect(cx - 5, cy - 1, 11, 3, RGB_BLACK);
        fb_fill_rect(cx, cy, 1, 1, RGB_TEAL);
    } else {
        fb_fill_rect(nav_x, nav_y, nav_w, nav_h, 0x1824);
        fb_draw_rect(nav_x, nav_y, nav_w, nav_h, 0x2945);
        int cx = nav_x + nav_w / 2;
        int cy = nav_y + nav_h / 2;
        fb_draw_rect(cx - 5, cy - 4, 11, 7, RGB_TEXT_MUTED);
        fb_fill_rect(cx - 1, cy + 3, 3, 2, RGB_TEXT_MUTED);
        fb_fill_rect(cx - 3, cy + 5, 7, 1, RGB_TEXT_MUTED);
    }

    int badge_x = FB_WIDTH - 142;
    int badge_w = 54;
    int badge_h = 16;
    int badge_y = 9;

    if (!g_rf_link_connected) {
        fb_fill_rect(badge_x, badge_y, badge_w, badge_h, RGB_RED);
        fb_draw_rect(badge_x, badge_y, badge_w, badge_h, RGB_WHITE);
        fb_draw_text(badge_x + 3, badge_y + 4, "NO HUB", RGB_WHITE, RGB_RED, 1);
    } else {
        fb_fill_rect(FB_WIDTH - 98, badge_y + 4, 8, 8, RGB_TEAL);
        fb_draw_rect(FB_WIDTH - 98, badge_y + 4, 8, 8, RGB_WHITE);
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[16];
    if (tm_info) {
        strftime(time_str, sizeof(time_str), "%H:%M", tm_info);
    } else {
        strncpy(time_str, "12:00", sizeof(time_str) - 1);
        time_str[sizeof(time_str) - 1] = '\0';
    }
    fb_draw_text_aa(FB_WIDTH - 84, 9, time_str, RGB_WHITE, &g_font_main);

    read_elite_battery();
    if (strcmp(g_battery_info.powersource, "usb") == 0) {
        blit_png_file(FB_WIDTH - 48, 9, 18, 14, "/opt/luadraws/appscommon/asset/header/usb.png");
    }

    char batt_icon[96] = {0};
    if (g_battery_info.charging) {
        snprintf(batt_icon, sizeof(batt_icon), "/opt/luadraws/appscommon/asset/header/hopscharging.png");
    } else {
        int lvl = (g_battery_info.percent / 10) * 10;
        if (lvl < 10) lvl = 10;
        if (lvl > 100) lvl = 100;
        snprintf(batt_icon, sizeof(batt_icon), "/opt/luadraws/appscommon/asset/header/hopslevel%d.png", lvl);
    }
    bool has_batt = blit_png_file(FB_WIDTH - 26, 9, 20, 16, batt_icon);
    if (!has_batt) {
        if (g_battery_info.charging) {
            blit_png_file(FB_WIDTH - 26, 9, 20, 16, "/opt/luadraws/appscommon/asset/header/charging.png");
        } else {
            blit_png_file(FB_WIDTH - 26, 9, 20, 16, "/opt/luadraws/appscommon/asset/header/level100.png");
        }
    }

    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);
}

void render_footer(void) {
    int dock_y = FB_HEIGHT - 38;
    int dock_h = 38;

    if (!blit_png_file(0, dock_y, FB_WIDTH, dock_h, "/opt/luadraws/appscommon/asset/footer/toolbarBG.png")) {
        fb_fill_rect(0, dock_y, FB_WIDTH, dock_h, RGB_TAB_BG);
    }
    fb_fill_rect(0, dock_y - 1, FB_WIDTH, 1, RGB_DIVIDER);

    bool has_acts = has_configured_activities();
    int num_tabs = has_acts ? 4 : 2;
    int tab_w = has_acts ? 56 : 114;
    int tab_stride = has_acts ? 59 : 118;
    int start_x = has_acts ? 3 : 4;

    for (int t = 0; t < num_tabs; t++) {
        int tx = start_x + t * tab_stride;
        int ty = dock_y + 4;
        int th = dock_h - 8;
        bool paired = is_remote_paired();
        bool is_disabled = false;
        bool is_active_view = false;

        if (has_acts) {
            is_disabled = !paired && (t == 0 || t == 1 || t == 2);
            if (t == 0 && g_current_view == VIEW_ACTIVITIES) is_active_view = true;
            else if (t == 1 && g_current_view == VIEW_DEVICES) is_active_view = true;
            else if (t == 2 && g_current_view == VIEW_DEVICE_BROWSER) is_active_view = true;
            else if (t == 3 && (g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING)) is_active_view = true;
            if (!paired) {
                is_active_view = (t == 3 && (g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING));
            }
        } else {
            is_disabled = !paired && (t == 0);
            if (t == 0 && (g_current_view == VIEW_DEVICE_BROWSER || g_current_view == VIEW_DEVICES || g_current_view == VIEW_ACTIVITIES)) is_active_view = true;
            else if (t == 1 && (g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING)) is_active_view = true;
            if (!paired) {
                is_active_view = (t == 1 && (g_current_view == VIEW_SETTINGS || g_current_view == VIEW_PAIRING));
            }
        }

        uint16_t bg = is_active_view ? RGB_TAB_ACTIVE : RGB_TAB_BG;
        uint16_t border = is_active_view ? RGB_TEAL : 0x1924;
        uint16_t fg = is_disabled ? 0x4A69 : (is_active_view ? RGB_WHITE : RGB_TEXT_MUTED);

        fb_fill_rect(tx, ty, tab_w, th, bg);
        fb_draw_rect(tx, ty, tab_w, th, border);

        if (is_active_view && !is_disabled) {
            fb_fill_rect(tx + 2, ty + th - 2, tab_w - 4, 2, RGB_TEAL);
        }

        if (has_acts) {
            if (t == 0) {
                if (!blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/wiihome.png") &&
                    !blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/activities.png")) {
                    int tw = fb_text_width_aa("ACT", &g_font_main);
                    fb_draw_text_aa(tx + (tab_w - tw) / 2, ty + 6, "ACT", fg, &g_font_main);
                }
            } else if (t == 1) {
                if (!blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/menu.png")) {
                    int tw = fb_text_width_aa("CMD", &g_font_main);
                    fb_draw_text_aa(tx + (tab_w - tw) / 2, ty + 6, "CMD", fg, &g_font_main);
                }
            } else if (t == 2) {
                if (!blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/devices.png")) {
                    int tw = fb_text_width_aa("DEV", &g_font_main);
                    fb_draw_text_aa(tx + (tab_w - tw) / 2, ty + 6, "DEV", fg, &g_font_main);
                }
            } else if (t == 3) {
                if (!blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/settings.png")) {
                    fb_draw_cog(tx + tab_w / 2, ty + th / 2, fg);
                }
            }
        } else {
            if (t == 0) {
                if (!blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/devices.png")) {
                    int tw = fb_text_width_aa("DEVICES", &g_font_main);
                    fb_draw_text_aa(tx + (tab_w - tw) / 2, ty + 6, "DEVICES", fg, &g_font_main);
                }
            } else if (t == 1) {
                if (!blit_png_file(tx + (tab_w - 22) / 2, ty + (th - 22) / 2, 22, 22, "/opt/luadraws/appscommon/asset/footer/settings.png")) {
                    fb_draw_cog(tx + tab_w / 2, ty + th / 2, fg);
                }
            }
        }
    }
}

void render_sync_overlay(void) {
    fb_fill_rect(0, 0, FB_WIDTH, 34, RGB_HEADER);
    fb_draw_text_aa(14, 9, "SYNCHRONIZING CONFIG", RGB_CYAN, &g_font_title);
    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);

    int card_y = 52;
    int card_h = 216;
    fb_fill_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_CARD);
    fb_draw_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_CARD_SEL);

    fb_draw_text_aa(22, card_y + 20, "UPDATING FROM HUB", RGB_WHITE, &g_font_title);
    fb_draw_text_aa(22, card_y + 46, "Syncing activities & mappings...", RGB_TEXT_MUTED, &g_font_main);

    char items_str[48];
    snprintf(items_str, sizeof(items_str), "Acts: %d | Devs: %d | Btns: %d",
             g_rf_activity_count, g_rf_device_count, g_rf_button_count);
    fb_draw_text_aa(22, card_y + 80, items_str, RGB_TEAL, &g_font_main);

    char chunks_str[40];
    if (g_sync_total > 0) {
        int done = g_sync_chunk_count < g_sync_total ? g_sync_chunk_count : g_sync_total;
        snprintf(chunks_str, sizeof(chunks_str), "Received %d of %d", done, g_sync_total);
    } else {
        snprintf(chunks_str, sizeof(chunks_str), "Chunks received: %d", g_sync_chunk_count);
    }
    fb_draw_text_aa(22, card_y + 106, chunks_str, RGB_TEXT_MUTED, &g_font_main);

    int bar_x = 22;
    int max_w = FB_WIDTH - 44;
    fb_fill_rect(bar_x, card_y + 140, max_w, 8, RGB_BG);
    fb_draw_rect(bar_x, card_y + 140, max_w, 8, 0x2945);
    if (g_sync_total > 0) {
        int done = g_sync_chunk_count < g_sync_total ? g_sync_chunk_count : g_sync_total;
        fb_fill_rect(bar_x, card_y + 140, max_w * done / g_sync_total, 8, RGB_CYAN);
    } else {
        uint64_t elapsed = get_monotonic_ms() - g_sync_start_ms;
        int pulse_pos = (int)((elapsed / 15) % (max_w > 30 ? max_w - 30 : 1));
        fb_fill_rect(bar_x + pulse_pos, card_y + 140, 30, 8, RGB_CYAN);
    }

    fb_draw_text_aa(22, card_y + 168, "Please wait...", RGB_TEXT_MUTED, &g_font_main);

    fb_fill_rect(0, FB_HEIGHT - 30, FB_WIDTH, 30, RGB_HEADER);
    fb_draw_text_aa(18, FB_HEIGHT - 22, "Screen locked during sync", RGB_TEXT_MUTED, &g_font_main);
}

void render_screen(void) {
    if (!g_fb_mem || !g_screen_on) return;
    pthread_mutex_lock(&g_render_mutex);

    if (g_has_wallpaper) {
        memcpy(g_backbuffer, g_bg_wallpaper, sizeof(g_backbuffer));
    } else {
        memset(g_backbuffer, 0x10, sizeof(g_backbuffer));
    }

    if (g_is_ota_updating) {
        render_ota_overlay();
    } else if (g_is_syncing_config) {
        render_sync_overlay();
    } else if (g_is_switching_activity) {
        render_switching_view();
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

        render_footer();
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

    memcpy((void *)g_fb_mem, g_backbuffer, FB_SIZE);
    if (g_fb_size >= FB_SIZE * 2) {
        memcpy((uint8_t *)g_fb_mem + FB_SIZE, g_backbuffer, FB_SIZE);
    }
    if (g_fb_size >= FB_SIZE * 3) {
        memcpy((uint8_t *)g_fb_mem + (FB_SIZE * 2), g_backbuffer, FB_SIZE);
    }

    g_screen_dirty = false;
    pthread_mutex_unlock(&g_render_mutex);
}
