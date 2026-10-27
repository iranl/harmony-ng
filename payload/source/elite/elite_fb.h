#ifndef HARMONY_ELITE_FB_H
#define HARMONY_ELITE_FB_H

#include "elite_state.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint16_t g_backbuffer[FB_WIDTH * FB_HEIGHT];
extern uint16_t g_bg_wallpaper[FB_WIDTH * FB_HEIGHT];
extern bool g_has_wallpaper;

void fb_fill_rect(int x, int y, int w, int h, uint16_t color);
void fb_draw_rect(int x, int y, int w, int h, uint16_t color);
void fb_draw_cog(int cx, int cy, uint16_t color);
void fb_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale);
void fb_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale);
int fb_text_width_aa(const char *text, const font_aa_t *font);
void fb_draw_text_aa(int x, int y, const char *text, uint16_t fg_color, const font_aa_t *font);
bool blit_png_file(int dst_x, int dst_y, int target_w, int target_h, const char *filepath);
void load_wallpaper_asset(void);
void fb_invalidate_png_cache(const char *substr);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_ELITE_FB_H */
