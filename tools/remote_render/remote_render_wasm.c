/* WebAssembly build of the Harmony remote screen renderer, for previews in a browser.
 *
 * The page writes the remote's files into the module's filesystem under a root directory
 * (config at <root>/data/remote_rf_config.json, stock assets under
 * <root>/opt/luadraws/appscommon/asset/), then calls er_render() and copies the returned
 * 240x320 RGBA pixels into a canvas. See tools/remote_render/web/remote_preview.js. */
#include <emscripten/emscripten.h>
#include "render_state.h"
#include "remote_fb.h"
#include "remote_ui.h"
#include "remote_config.h"
#include "remote_stock_icons.h"
#include "remote_ui_scroll.h"

#include <stdio.h>
#include <string.h>

static uint8_t g_rgba[FB_WIDTH * FB_HEIGHT * 4];

EMSCRIPTEN_KEEPALIVE
uint8_t *er_render(const char *root, const char *view, int select, const char *active, const char *icons,
                   int battery, int usb, int charging, int no_hub, int minutes, const char *theme,
                   const char *layout, int list_top) {
    render_options_t o = {
        .root = root, .view = view, .select = select, .active = active, .icons = icons, .theme = theme, .layout = layout,
        .battery = battery, .usb = usb != 0, .charging = charging != 0, .no_hub = no_hub != 0, .minutes = minutes,
        .list_top = list_top, .has_list_top = list_top >= 0,
    };
    render_load(&o);
    render_frame();
    render_copy_out(g_rgba, FB_WIDTH, 0, 0, 4);
    return g_rgba;
}

/* Pixels the list on the last screen er_render() drew is scrolled down (0 without a list) */
EMSCRIPTEN_KEEPALIVE
int er_list_top(void) {
    list_geom_t g;
    return list_geometry(&g) ? list_scroll_offset() : 0;
}

/* Activities on a home page in the layout er_render() last drew */
EMSCRIPTEN_KEEPALIVE
int er_home_page_size(void) {
    return activity_page_size();
}

/* What a tap at (x, y) on the last screen er_render() drew opens, found with the remote's own
   touch handling: "activity ID" (the remote's activity ID), "device N" (the Nth device in the
   config), or "" for anything else */
EMSCRIPTEN_KEEPALIVE
const char *er_hit(int x, int y) {
    static char hit[48];
    hit[0] = '\0';
    if (g_menu_open || g_key_hint_pending || g_link_panel_open || ui_help_active() || g_is_syncing_config) return hit;
    int dev = -1;
    if (g_current_view == VIEW_ACTIVITIES) {
        dev = home_card_device_at(y);
        int idx = dev < 0 ? home_activity_at(x, y) : -1;
        if (idx >= 0) snprintf(hit, sizeof(hit), "activity %s", g_activities[idx].id);
    } else if (g_current_view == VIEW_DEVICES && get_active_dev_cmd_count() <= 0) {
        const rf_activity_t *act = rf_activity_by_id(g_active_activity_id);
        if (act) dev = activity_card_device_at(act, FB_HEIGHT, y);
    } else if (g_current_view == VIEW_DEVICE_BROWSER && g_browse_dev < 0) {
        dev = list_row_at(y);
    }
    if (dev >= 0) snprintf(hit, sizeof(hit), "device %d", dev);
    return hit;
}

/* What the remote stores for an activity or device after the Hub's config push, so a page
   can write the config the way the push does: the icon keyword (the Hub sends
   rf_activity_icon_type(), the remote maps it with map_icon_type_to_str() and the name it
   received) and the stock icon index (lookup_stock_icon_id() of the icon, then the name) */
EMSCRIPTEN_KEEPALIVE
const char *er_icon_keyword(const char *icon, const char *name, const char *type, const char *stored_name) {
    static char keyword[64];
    map_icon_type_to_str(rf_activity_icon_type(icon, name, type), stored_name, keyword, sizeof(keyword));
    return keyword;
}

EMSCRIPTEN_KEEPALIVE
int er_stock_icon_id(const char *icon, const char *name) {
    uint16_t id = (icon && icon[0]) ? lookup_stock_icon_id(icon) : 0;
    if (id == 0 && name && name[0]) id = lookup_stock_icon_id(name);
    return id;
}

/* The image the remote draws for an activity in a theme: a path in the module's filesystem
   under root, resolved from <root>/data/remote_rf_config.json the way the remote resolves its
   config */
EMSCRIPTEN_KEEPALIVE
const char *er_activity_icon(const char *root, const char *id, const char *theme) {
    g_fs_root = root ? root : "";
    theme_load(theme);
    load_rf_config();
    for (int i = 0; i < g_activity_count; i++) {
        if (strcmp(g_activities[i].id, id) == 0) return g_activities[i].icon_path;
    }
    return "";
}

/* The image the remote draws for a device in a theme (id as the push cuts it, 4 characters),
   resolved like er_activity_icon */
EMSCRIPTEN_KEEPALIVE
const char *er_device_icon(const char *root, const char *id, const char *theme) {
    static char path[256];
    g_fs_root = root ? root : "";
    theme_load(theme);
    load_rf_config();
    for (int i = 0; i < g_rf_device_count; i++) {
        rf_device_t *d = &g_rf_devices[i];
        if (strcmp(d->id, id) == 0 && resolve_device_icon(d->icon_id, d->icon, d->name, d->id, path, sizeof(path))) return path;
    }
    return "";
}

/* Call after replacing files in the filesystem, so cached images are decoded again */
EMSCRIPTEN_KEEPALIVE
void er_assets_changed(void) {
    fb_invalidate_png_cache("/");
}

int main(void) {
    setenv("TZ", "UTC0", 1);
    tzset();
    return 0;
}
