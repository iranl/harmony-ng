/* WebAssembly build of the Harmony Elite screen renderer, for previews in a browser.
 *
 * The page writes the remote's files into the module's filesystem under a root directory
 * (config at <root>/data/elite_rf_config.json, stock assets under
 * <root>/opt/luadraws/appscommon/asset/), then calls er_render() and copies the returned
 * 240x320 RGBA pixels into a canvas. See tools/elite_render/web/elite_preview.js. */
#include <emscripten/emscripten.h>
#include "render_state.h"
#include "elite_fb.h"
#include "elite_ui.h"
#include "elite_config.h"
#include "elite_stock_icons.h"

static uint8_t g_rgba[FB_WIDTH * FB_HEIGHT * 4];

EMSCRIPTEN_KEEPALIVE
uint8_t *er_render(const char *root, const char *view, int select, const char *active, const char *icons,
                   int battery, int usb, int charging, int no_hub, int minutes) {
    render_options_t o = {
        .root = root, .view = view, .select = select, .active = active, .icons = icons,
        .battery = battery, .usb = usb != 0, .charging = charging != 0, .no_hub = no_hub != 0, .minutes = minutes,
    };
    render_load(&o);
    render_frame();
    render_copy_out(g_rgba, FB_WIDTH, 0, 0, 4);
    return g_rgba;
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
