/* Themes: the palette, fonts, icons, wallpaper and home layout the screens draw with.
   A theme is a directory /data/themes/<name>/; Classic is built in
   and supplies everything a theme leaves out. */
#ifndef HARMONY_REMOTE_THEME_H
#define HARMONY_REMOTE_THEME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "font_aa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define THEME_DIR "/data/themes"

/* Colour roles, RGB565 */
typedef struct {
    uint16_t bg;               /* screen background when there is no wallpaper */
    uint16_t card;             /* list card */
    uint16_t card_sel;         /* selected card */
    uint16_t card_border;      /* card border, panel border */
    uint16_t divider;
    uint16_t outline;          /* idle card outline, progress bar frame */
    uint16_t text;
    uint16_t text_muted;
    uint16_t on_accent;        /* glyphs on an accent badge */
    uint16_t accent;           /* selection, highlights */
    uint16_t success;
    uint16_t danger;
    uint16_t danger_dim;       /* idle outline of a destructive item */
    uint16_t warning;
    uint16_t warning_bg;       /* card waiting for a confirmation */
    uint16_t info;
    uint16_t header;           /* overlay header and footer bars */
    uint16_t status_bg;        /* status bar, when the stock toolbar image is missing */
    uint16_t icon_well;        /* square behind a list icon */
    uint16_t icon_well_sel;
    uint16_t icon_well_border;
    uint16_t icon_outline;     /* placeholder drawn when an icon is missing */
} theme_palette_t;

typedef enum { THEME_LAYOUT_LIST = 0, THEME_LAYOUT_GRID, THEME_LAYOUT_CAROUSEL } theme_layout_t;

typedef struct {
    char name[32];             /* "classic", or the directory name under THEME_DIR */
    char label[24];            /* theme.json "name", shown in Settings */
    char dir[64];              /* THEME_DIR "/<name>"; empty for Classic */
    theme_palette_t pal;
    const font_aa_t *font_main;
    const font_aa_t *font_title;
    theme_layout_t layout;
    char wallpaper[96];        /* remote path of the wallpaper; empty: Classic's; "none": pal.bg */
    uint8_t line_art[3];       /* RGB the dark pixels of stock line art are drawn in: near-white on
                                  dark themes, dark on light ones (theme.json "line_art") */
} theme_t;

extern theme_t g_theme;

/* Loads THEME_DIR/<name>/theme.json over Classic; Classic for "classic", an empty name, or a
   theme that can't be read. Reload the config and wallpaper afterwards, since icon paths and
   the wallpaper depend on the theme. */
void theme_load(const char *name);

/* The name of the theme after the current one: Classic, then the themes in THEME_DIR in
   name order */
void theme_next_name(char *out, size_t len);

/* The theme's icon for an activity keyword: <dir>/icons/<keyword>_small.png, with a large
   version beside it as <keyword>.png. False when the theme has none (Classic draws it). */
bool theme_keyword_icon(const char *keyword, char *out, size_t len);

/* The theme's replacement for a stock asset (<dir>/stock/<path under the stock asset
   directory>), or path itself when the theme doesn't replace it */
const char *theme_stock_path(const char *path, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_THEME_H */
