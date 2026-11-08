/* Screen transitions. Shared with the host renderer, which never runs them: they
 * need page flipping to wait for display refreshes. */
#include "remote_transition.h"
#include "remote_fb.h"
#include "remote_present.h"
#include "remote_ui.h"

#include <string.h>

#define TRANSITION_TOP   34   /* rows above stay still: the status bar */
#define SLIDE_MS         200
#define FADE_MS          150

enum { SECTION_HOME = 0, SECTION_DEVICES, SECTION_SETTINGS, SECTION_OTHER };

static uint16_t s_from[FB_WIDTH * FB_HEIGHT];
static uint16_t s_to[FB_WIDTH * FB_HEIGHT];
static screen_place_t s_last;
static bool s_have_last = false;
static bool s_hint_back = false;

screen_place_t screen_place(void) {
    screen_place_t p = { SECTION_OTHER, 0, (int)g_current_view, false };
    p.overlay = g_is_switching_activity || g_is_syncing_config || g_is_ota_updating || ui_help_active() ||
                g_key_hint_pending || g_link_panel_open || ui_charging_page_active();
    switch (g_current_view) {
        case VIEW_ACTIVITIES: p.section = SECTION_HOME; p.depth = 0; break;
        case VIEW_DEVICES:    p.section = SECTION_HOME; p.depth = 1; break;
        case VIEW_DEVICE_BROWSER:
            p.section = SECTION_DEVICES;
            p.depth = g_browse_dev >= 0 ? 1 : 0;
            p.id = g_browse_dev;
            break;
        case VIEW_SETTINGS:
            p.section = SECTION_SETTINGS;
            p.id = (int)g_settings_subview;
            p.depth = g_settings_subview == SETTINGS_VIEW_CATEGORIES ? 0
                    : (g_settings_subview == SETTINGS_VIEW_REBOOT || g_settings_subview == SETTINGS_VIEW_FACTORY_RESET ||
                       g_settings_subview == SETTINGS_VIEW_SUSPEND) ? 2 : 1;
            break;
        case VIEW_PAIRING:    p.section = SECTION_SETTINGS; p.depth = 2; p.id = 100; break;
        default: break;
    }
    return p;
}

transition_t transition_between(screen_place_t from, screen_place_t to) {
    if (from.overlay || to.overlay) return TRANSITION_NONE;
    if (from.section == to.section && from.depth == to.depth && from.id == to.id) return TRANSITION_NONE;
    if (from.section != to.section || from.depth == to.depth) return TRANSITION_FADE;
    return to.depth > from.depth ? TRANSITION_PUSH : TRANSITION_POP;
}

void transition_hint_back(void) {
    s_hint_back = true;
}

void transition_reset(void) {
    s_have_last = false;
    s_hint_back = false;
}

/* a/32 of t over (32-a)/32 of f, for both RGB565 pixels at once */
static inline uint16_t blend565(uint16_t f, uint16_t t, uint32_t a) {
    uint32_t fe = (f | ((uint32_t)f << 16)) & 0x07E0F81F;
    uint32_t te = (t | ((uint32_t)t << 16)) & 0x07E0F81F;
    uint32_t r = ((fe * (32 - a) + te * a) >> 5) & 0x07E0F81F;
    return (uint16_t)(r | (r >> 16));
}

void transition_compose(transition_t t, const uint16_t *from, const uint16_t *to, uint16_t *out, int progress) {
    if (progress < 0) progress = 0;
    if (progress > 256) progress = 256;
    memcpy(out, to, sizeof(uint16_t) * FB_WIDTH * TRANSITION_TOP);
    int shift = FB_WIDTH * progress / 256;
    uint32_t a = (uint32_t)progress / 8;
    for (int y = TRANSITION_TOP; y < FB_HEIGHT; y++) {
        const uint16_t *f = from + y * FB_WIDTH, *n = to + y * FB_WIDTH;
        uint16_t *o = out + y * FB_WIDTH;
        if (t == TRANSITION_PUSH) {
            /* The old screen moves left and the new one follows it in from the right */
            memcpy(o, f + shift, sizeof(uint16_t) * (FB_WIDTH - shift));
            memcpy(o + FB_WIDTH - shift, n, sizeof(uint16_t) * shift);
        } else if (t == TRANSITION_POP) {
            memcpy(o, n + FB_WIDTH - shift, sizeof(uint16_t) * shift);
            memcpy(o + shift, f, sizeof(uint16_t) * (FB_WIDTH - shift));
        } else if (t == TRANSITION_FADE) {
            for (int x = 0; x < FB_WIDTH; x++) o[x] = blend565(f[x], n[x], a);
        } else {
            memcpy(o, n, sizeof(uint16_t) * FB_WIDTH);
        }
    }
}

/* Ease-out cubic of t/dur, scaled to 0..256 */
static int eased(uint64_t t, uint32_t dur) {
    float p = 1.0f - (float)t / (float)dur;
    return (int)(256.0f * (1.0f - p * p * p));
}

transition_t transition_prepare(screen_place_t place) {
    transition_t t = TRANSITION_NONE;
    bool same = s_have_last && s_last.section == place.section && s_last.depth == place.depth && s_last.id == place.id;
    if (s_have_last && !same && fb_can_animate()) {
        t = transition_between(s_last, place);
        if (t != TRANSITION_NONE && s_hint_back) t = TRANSITION_POP;
    }
    if (!same) s_hint_back = false;
    s_last = place;
    s_have_last = true;
    /* g_backbuffer still holds the frame on screen */
    if (t != TRANSITION_NONE) memcpy(s_from, g_backbuffer, sizeof(s_from));
    return t;
}

void transition_play(transition_t t) {
    memcpy(s_to, g_backbuffer, sizeof(s_to));
    uint32_t dur = t == TRANSITION_FADE ? FADE_MS : SLIDE_MS;
    uint64_t start = get_monotonic_ms();
    for (;;) {
        uint64_t elapsed = get_monotonic_ms() - start;
        if (elapsed >= dur) break;
        uint64_t t0 = get_monotonic_us();
        transition_compose(t, s_from, s_to, g_backbuffer, eased(elapsed, dur));
        uint64_t t1 = get_monotonic_us();
        bool flipped = fb_present(g_backbuffer, true);
        if (frame_timing_enabled()) {
            frame_timing_t ft = { "transition", g_theme.name, home_layout_name(),
                                  (uint32_t)(t1 - t0), (uint32_t)(get_monotonic_us() - t1), flipped, 0, 0 };
            frame_timing_record(&ft);
        }
    }
    memcpy(g_backbuffer, s_to, sizeof(s_to));
    fb_present(g_backbuffer, true);
}
