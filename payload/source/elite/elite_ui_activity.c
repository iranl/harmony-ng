/* The activity screen's hero card (ADR-0005): shown when the running activity has no screen
 * buttons, it confirms what is running and which devices it uses. */
#include "elite_ui.h"
#include "elite_fb.h"
#include "elite_config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define HERO_ICON 70  /* the largest stock activity image */
#define HERO_DEVICE_ROWS 4
#define HERO_DEVICE_ICON 28  /* the stock device images are 45 px */
#define HERO_ROW_H 30

/* The stock firmware keeps a 70 px image next to each *_small.png list icon */
static void large_icon_path(const char *small, char *out, size_t len) {
    snprintf(out, len, "%s", small);
    size_t n = strlen(out);
    const char *suffix = "_small.png";
    size_t sn = strlen(suffix);
    if (n <= sn || strcmp(out + n - sn, suffix) != 0) return;
    char large[160], fs[256];
    snprintf(large, sizeof(large), "%.*s.png", (int)(n - sn), out);
    if (access(elite_fs_path(large, fs, sizeof(fs)), R_OK) == 0) snprintf(out, len, "%s", large);
}

static void draw_centered(int y, const char *text, uint16_t color, const font_aa_t *font) {
    char fit[64];
    fb_fit_text_aa(fit, sizeof(fit), text, font, FB_WIDTH - 16);
    fb_draw_text_aa((FB_WIDTH - fb_text_width_aa(fit, font)) / 2, y, fit, color, font);
}

/* Returns false when no activity is running, so the caller can show its empty state */
bool render_activity_hero(void) {
    const rf_activity_t *act = NULL;
    for (int i = 0; i < g_rf_activity_count; i++) {
        if (strcmp(g_rf_activities[i].id, g_active_activity_id) == 0) act = &g_rf_activities[i];
    }
    if (!act || strcmp(g_active_activity_id, "-1") == 0) return false;

    int x = 0, w = FB_WIDTH;

    /* The device rows sit at the bottom of the screen, the last device nearest the edge */
    int rows = act->dev_count > HERO_DEVICE_ROWS ? HERO_DEVICE_ROWS + 1 : act->dev_count;
    int ry = FB_HEIGHT - 8 - rows * HERO_ROW_H;

    /* The icon, name and ACTIVE are centred in the space above the device rows */
    int space_bottom = rows > 0 ? ry - 6 : FB_HEIGHT;
    int block_h = HERO_ICON + 26 + g_font_main.height;
    int y = UI_CONTENT_Y + (space_bottom - UI_CONTENT_Y - block_h) / 2;

    char icon[128] = {0}, large[160];
    resolve_activity_icon(act->icon_id, act->icon, act->name, icon, sizeof(icon));
    large_icon_path(icon, large, sizeof(large));
    blit_png_file((FB_WIDTH - HERO_ICON) / 2, y, HERO_ICON, HERO_ICON, large);

    draw_centered(y + HERO_ICON + 4, act->name, RGB_WHITE, &g_font_title);
    draw_centered(y + HERO_ICON + 26, tr(STR_ACTIVE), RGB_GREEN, &g_font_main);

    if (act->dev_count > 0) {
        fb_fill_rect(x + 12, ry - 6, w - 24, 1, RGB_DIVIDER);
        for (int i = 0; i < act->dev_count && i < HERO_DEVICE_ROWS; i++, ry += HERO_ROW_H) {
            if (act->devs[i] >= g_rf_device_count) continue;
            const rf_device_t *d = &g_rf_devices[act->devs[i]];
            char dpath[128] = {0};
            bool has_icon = resolve_device_icon(d->icon_id, d->icon, d->name, d->id, dpath, sizeof(dpath)) &&
                            blit_png_file(x + 16, ry + 1, HERO_DEVICE_ICON, HERO_DEVICE_ICON, dpath);
            int tx = x + (has_icon ? 16 + HERO_DEVICE_ICON + 10 : 16);
            char fit[sizeof(d->name)];
            fb_draw_text_aa(tx, ry + (HERO_ROW_H - g_font_main.height) / 2, fb_fit_text_aa(fit, sizeof(fit), d->name, &g_font_main, x + w - 16 - tx), RGB_TEXT_MUTED, &g_font_main);
        }
        if (act->dev_count > HERO_DEVICE_ROWS) {
            char more[24];
            snprintf(more, sizeof(more), "+%d", act->dev_count - HERO_DEVICE_ROWS);
            fb_draw_text_aa(x + 16 + HERO_DEVICE_ICON + 10, ry + (HERO_ROW_H - g_font_main.height) / 2, more, RGB_TEXT_MUTED, &g_font_main);
        }
    }
    return true;
}
