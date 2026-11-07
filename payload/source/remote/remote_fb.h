#ifndef HARMONY_REMOTE_FB_H
#define HARMONY_REMOTE_FB_H

#include "remote_state.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint16_t g_backbuffer[FB_WIDTH * FB_HEIGHT];
extern uint16_t g_bg_wallpaper[FB_WIDTH * FB_HEIGHT];
extern bool g_has_wallpaper;

/* Limits drawing to rows [y0, y1) until fb_clip_reset(); render_frame() starts unclipped */
void fb_clip_rows(int y0, int y1);
void fb_clip_reset(void);
void fb_fill_rect(int x, int y, int w, int h, uint16_t color);
void fb_draw_rect(int x, int y, int w, int h, uint16_t color);
void fb_draw_cog(int cx, int cy, uint16_t color);
void fb_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale);
void fb_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale);
int fb_text_width_aa(const char *text, const font_aa_t *font);
const char *fb_fit_text_aa(char *out, size_t size, const char *text, const font_aa_t *font, int max_w);
void fb_draw_text_aa(int x, int y, const char *text, uint16_t fg_color, const font_aa_t *font);
bool blit_png_file(int dst_x, int dst_y, int target_w, int target_h, const char *filepath);
void load_wallpaper_asset(void);
void fb_invalidate_png_cache(const char *substr);
/* PNG decodes and scalings since the last call, for the frame timing log */
void fb_png_work_take(int *decodes, int *scales);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_FB_H */
