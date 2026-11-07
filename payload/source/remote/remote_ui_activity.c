/* The activity screen's hero card: shown when the running activity has no screen
 * buttons, it confirms what is running and which devices it uses. The carousel home layout
 * draws the same card for each activity. */
#include "remote_ui.h"
#include "remote_fb.h"
#include "remote_config.h"

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
    if (access(remote_fs_path(large, fs, sizeof(fs)), R_OK) == 0) snprintf(out, len, "%s", large);
}

static void draw_centered(int x, int y, const char *text, uint16_t color, const font_aa_t *font) {
    char fit[64];
    fb_fit_text_aa(fit, sizeof(fit), text, font, FB_WIDTH - 16);
    fb_draw_text_aa(x + (FB_WIDTH - fb_text_width_aa(fit, font)) / 2, y, fit, color, font);
}

const rf_activity_t *rf_activity_by_id(const char *id) {
    for (int i = 0; i < g_rf_activity_count; i++) {
        if (strcmp(g_rf_activities[i].id, id) == 0) return &g_rf_activities[i];
    }
    return NULL;
}

/* The device rows sit at the bottom, the last device nearest the edge */
static int device_rows_y(const rf_activity_t *act, int bottom) {
    int rows = act->dev_count > HERO_DEVICE_ROWS ? HERO_DEVICE_ROWS + 1 : act->dev_count;
    return bottom - 8 - rows * HERO_ROW_H;
}

int activity_card_device_at(const rf_activity_t *act, int bottom, int y) {
    int ry = device_rows_y(act, bottom);
    if (y < ry) return -1;
    int i = (y - ry) / HERO_ROW_H;
    if (i >= act->dev_count || i >= HERO_DEVICE_ROWS || act->devs[i] >= g_rf_device_count) return -1;
    return act->devs[i];
}

void draw_activity_card(const rf_activity_t *act, int x, int bottom, const char *state, uint16_t state_color) {
    int rows = act->dev_count > HERO_DEVICE_ROWS ? HERO_DEVICE_ROWS + 1 : act->dev_count;
    int ry = device_rows_y(act, bottom);

    /* The icon, name and state are centred in the space above the device rows */
    int space_bottom = rows > 0 ? ry - 6 : bottom;
    int block_h = HERO_ICON + 26 + g_font_main.height;
    int y = UI_CONTENT_Y + (space_bottom - UI_CONTENT_Y - block_h) / 2;

    char icon[128] = {0}, large[160];
    resolve_activity_icon(act->icon_id, act->icon, act->name, icon, sizeof(icon));
    large_icon_path(icon, large, sizeof(large));
    blit_png_file(x + (FB_WIDTH - HERO_ICON) / 2, y, HERO_ICON, HERO_ICON, large);

    draw_centered(x, y + HERO_ICON + 4, act->name, RGB_WHITE, &g_font_title);
    if (state) draw_centered(x, y + HERO_ICON + 26, state, state_color, &g_font_main);

    if (act->dev_count > 0) {
        fb_fill_rect(x + 12, ry - 6, FB_WIDTH - 24, 1, RGB_DIVIDER);
        for (int i = 0; i < act->dev_count && i < HERO_DEVICE_ROWS; i++, ry += HERO_ROW_H) {
            if (act->devs[i] >= g_rf_device_count) continue;
            const rf_device_t *d = &g_rf_devices[act->devs[i]];
            char dpath[128] = {0};
            bool has_icon = resolve_device_icon(d->icon_id, d->icon, d->name, d->id, dpath, sizeof(dpath)) &&
                            blit_png_file(x + 16, ry + 1, HERO_DEVICE_ICON, HERO_DEVICE_ICON, dpath);
            int tx = x + (has_icon ? 16 + HERO_DEVICE_ICON + 10 : 16);
            char fit[sizeof(d->name)];
            fb_draw_text_aa(tx, ry + (HERO_ROW_H - g_font_main.height) / 2, fb_fit_text_aa(fit, sizeof(fit), d->name, &g_font_main, x + FB_WIDTH - 16 - tx), RGB_TEXT_MUTED, &g_font_main);
        }
        if (act->dev_count > HERO_DEVICE_ROWS) {
            char more[24];
            snprintf(more, sizeof(more), "+%d", act->dev_count - HERO_DEVICE_ROWS);
            fb_draw_text_aa(x + 16 + HERO_DEVICE_ICON + 10, ry + (HERO_ROW_H - g_font_main.height) / 2, more, RGB_TEXT_MUTED, &g_font_main);
        }
    }
}

/* Returns false when no activity is running, so the caller can show its empty state */
bool render_activity_hero(void) {
    const rf_activity_t *act = rf_activity_by_id(g_active_activity_id);
    if (!act || strcmp(g_active_activity_id, "-1") == 0) return false;
    draw_activity_card(act, 0, FB_HEIGHT, tr(STR_ACTIVE), RGB_GREEN);
    return true;
}
