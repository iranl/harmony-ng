/* WebAssembly build of the Harmony Elite screen renderer, for previews in a browser.
 *
 * The page writes the remote's files into the module's filesystem under a root directory
 * (config at <root>/data/elite_rf_config.json, stock assets under
 * <root>/opt/luadraws/appscommon/asset/), then calls er_render() and copies the returned
 * 240x320 RGBA pixels into a canvas. See tools/elite_render/web/preview.js. */
#include <emscripten/emscripten.h>
#include "render_state.h"
#include "elite_fb.h"
#include "elite_ui.h"

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
