/* Sets up the firmware's state for a render and draws tool-only screens. */
#include "render_state.h"
#include "remote_fb.h"
#include "remote_config.h"
#include "remote_link.h"
#include "remote_ui.h"
#include "remote_ui_scroll.h"

#define KEYWORD_COUNT 17

extern time_t g_host_wall_time;

static const char *activity_keyword(int i) {
    if (strcmp(g_activities[i].id, "-1") == 0) return "power";
    for (int r = 0; r < g_rf_activity_count; r++) {
        if (strcmp(g_rf_activities[r].id, g_activities[i].id) == 0) {
            return rf_activity_icon_keyword(rf_activity_icon_type(g_rf_activities[r].icon, g_rf_activities[r].name, NULL));
        }
    }
    return rf_activity_icon_keyword(rf_activity_icon_type(NULL, g_activities[i].name, NULL));
}

/* Icon file for a keyword: <dir>/<keyword>.png, or the stock asset the firmware picks today */
static void keyword_icon_path(const char *icons, const char *keyword, char *out, size_t len) {
    if (icons && icons[0] && strcmp(icons, "classic") != 0) {
        snprintf(out, len, "%s/%s.png", icons, keyword);
    } else if (strcmp(keyword, "power") == 0) {
        snprintf(out, len, "/opt/luadraws/appscommon/asset/commands/116.png");
    } else {
        resolve_activity_icon(0, keyword, keyword, out, len);
    }
}

static void set_view(const render_options_t *o) {
    const char *v = o->view ? o->view : "activities";
    g_is_syncing_config = false;
    g_menu_open = false;
    g_key_hint_pending = false;
    g_link_panel_open = false;
    g_charging_page_until_ms = 0;
    ui_help_close();
    if (strcmp(v, "charging") == 0) {
        g_current_view = VIEW_ACTIVITIES;
        g_charging_page_until_ms = (uint32_t)(get_monotonic_ms() + 60000) | 1;
    } else if (strcmp(v, "menu") == 0 || strcmp(v, "hint") == 0 || strcmp(v, "link") == 0) {
        g_current_view = VIEW_ACTIVITIES;
        g_selected_activity = o->select;
        g_menu_open = (strcmp(v, "menu") == 0);
        g_key_hint_pending = (strcmp(v, "hint") == 0);
        g_link_panel_open = (strcmp(v, "link") == 0);
    } else if (strcmp(v, "commands") == 0) {
        g_current_view = VIEW_DEVICES;
        g_selected_device_cmd = o->select;
    } else if (strcmp(v, "devices") == 0) {
        g_current_view = VIEW_DEVICE_BROWSER;
        g_browse_dev = -1;
        g_browse_sel = o->select;
    } else if (strcmp(v, "device") == 0) {
        g_current_view = VIEW_DEVICE_BROWSER;
        g_browse_dev = o->select;
        g_browse_sel = 0;
    } else if (strcmp(v, "settings") == 0) {
        g_current_view = VIEW_SETTINGS;
    } else if (strcmp(v, "pairing") == 0) {
        g_current_view = VIEW_PAIRING;
    } else if (strcmp(v, "not-paired") == 0) {
        g_current_view = VIEW_NOT_PAIRED;
    } else if (strncmp(v, "help-", 5) == 0) {
        /* Help over the activity screen, driven by the frames the Hub sends */
        g_current_view = VIEW_DEVICES;
        ui_help_close();
        ui_help_start();
        rf_frame_t f = {0};
        f.payload[0] = RF_HELP_STARTED;
        ui_help_on_status(&f);
        if (strcmp(v, "help-wait") != 0) {
            f.payload[0] = RF_HELP_QUESTION;
            f.payload[2] = strcmp(v, "help-done") == 0 ? 1 : 2;
            int d = 0;
            for (int i = 0; i < g_rf_activity_count; i++) {
                if (strcmp(g_rf_activities[i].id, g_active_activity_id) == 0 && g_rf_activities[i].dev_count) d = g_rf_activities[i].devs[0];
            }
            if (d < g_rf_device_count) snprintf((char *)&f.payload[3], 9, "%s", g_rf_devices[d].id);
            ui_help_on_status(&f);
        }
        if (strcmp(v, "help-done") == 0) {
            ui_help_handle_key(106); /* No */
            ui_help_handle_key(28);
        }
    } else if (strcmp(v, "sync") == 0) {
        g_is_syncing_config = true;
        g_sync_chunk_count = o->sync_done;
        g_sync_total = o->sync_total;
    } else {
        g_current_view = VIEW_ACTIVITIES;
        g_selected_activity = o->select;
    }
}

void render_load(const render_options_t *o) {
    g_fs_root = o->root ? o->root : "";
    /* The status bar shows localtime(); mktime() makes it the requested time in any time zone
       (the browser build follows the browser's zone, whatever TZ says) */
    struct tm clock = { .tm_year = 126, .tm_mday = 1, .tm_hour = o->minutes / 60, .tm_min = o->minutes % 60, .tm_isdst = -1 };
    g_host_wall_time = mktime(&clock);
    theme_load(o->theme);
    g_home_layout = o->layout && strcmp(o->layout, "grid") == 0 ? HOME_LAYOUT_GRID
                  : o->layout && strcmp(o->layout, "list") == 0 ? HOME_LAYOUT_LIST
                  : o->layout && strcmp(o->layout, "carousel") == 0 ? HOME_LAYOUT_CAROUSEL : HOME_LAYOUT_THEME;
    load_rf_config();
    load_wallpaper_asset();
    g_rf_link_connected = !o->no_hub;
    /* Samples behind the link indicator, as the remote records them over the last 30 s */
    link_reset();
    uint64_t now = get_monotonic_ms();
    g_last_hub_frame_ms = o->hub_silent ? now - 14000 : now - 400;
    if ((o->link_loss > 0 || o->hub_silent) && now > 30000) {
        for (int i = 0; i < 30; i++) { /* a query every 800 ms, as the remote sends them */
            uint64_t t = now - 25000 + i * 800;
            bool lost = i % 10 < o->link_loss / 10;
            link_note_status(o->hub_silent || !lost, t);
            link_note_query((uint8_t)i, t);
            if (!o->hub_silent && !lost) link_note_reply((uint8_t)i, t + 100);
        }
    }
    g_pairing_state = 2;
    g_initial_sync_received = true;
    g_battery_info.percent = (uint8_t)o->battery;
    g_battery_info.charging = o->charging ? 1 : 0;
    snprintf(g_battery_info.powersource, sizeof(g_battery_info.powersource), "%s", o->usb ? "usb" : "battery");
    g_active_activity_id[0] = '\0';
    g_active_activity_name[0] = '\0';
    if (o->active) {
        snprintf(g_active_activity_id, sizeof(g_active_activity_id), "%s", o->active);
        for (int i = 0; i < g_activity_count; i++) {
            if (strcmp(g_activities[i].id, o->active) == 0) {
                snprintf(g_active_activity_name, sizeof(g_active_activity_name), "%s", g_activities[i].name);
            }
        }
    }
    if (o->icons && o->icons[0] && strcmp(o->icons, "classic") != 0) {
        for (int i = 0; i < g_activity_count; i++) {
            keyword_icon_path(o->icons, activity_keyword(i), g_activities[i].icon_path, sizeof(g_activities[i].icon_path));
        }
    }
    set_view(o);
    home_scroll_set_offset(o->scroll);
    if (o->has_list_top) list_scroll_to(o->list_top);
    else list_scroll_set_drag(o->scroll);
}

void render_keyword_sheet(const char *title, const char *icons) {
    if (g_has_wallpaper) memcpy(g_backbuffer, g_bg_wallpaper, sizeof(g_backbuffer));
    else memset(g_backbuffer, 0x10, sizeof(g_backbuffer));
    fb_fill_rect(0, 0, FB_WIDTH, 26, RGB_HEADER);
    fb_draw_text_aa(8, 5, title, RGB_WHITE, &g_font_main);
    fb_fill_rect(0, 26, FB_WIDTH, 1, RGB_DIVIDER);

    for (int k = 0; k < KEYWORD_COUNT; k++) {
        const char *kw = rf_activity_icon_keyword((uint8_t)(k + 1));
        int x = (k % 4) * 60, y = 30 + (k / 4) * 58;
        fb_fill_rect(x + 8, y, 44, 44, 0x18C3);
        fb_draw_rect(x + 8, y, 44, 44, 0x31A6);
        char path[160];
        keyword_icon_path(icons, kw, path, sizeof(path));
        if (!blit_png_file(x + 10, y + 2, 40, 40, path)) {
            fb_draw_text(x + 26, y + 18, "?", RGB_RED, 0, 1);
        }
        int tw = fb_text_width_aa(kw, &g_font_main);
        fb_draw_text_aa(x + (60 - tw) / 2, y + 43, kw, RGB_TEXT_MUTED, &g_font_main);
    }
}

void render_copy_out(uint8_t *canvas, int cw, int ox, int oy, int channels) {
    for (int y = 0; y < FB_HEIGHT; y++) {
        for (int x = 0; x < FB_WIDTH; x++) {
            uint16_t p = g_backbuffer[y * FB_WIDTH + x];
            uint8_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            uint8_t *d = &canvas[((size_t)(oy + y) * cw + ox + x) * channels];
            d[0] = (uint8_t)((r << 3) | (r >> 2));
            d[1] = (uint8_t)((g << 2) | (g >> 4));
            d[2] = (uint8_t)((b << 3) | (b >> 2));
            if (channels == 4) d[3] = 255;
        }
    }
}
