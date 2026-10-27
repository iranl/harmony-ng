#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "elite_fb.h"

uint16_t g_backbuffer[FB_WIDTH * FB_HEIGHT];
uint16_t g_bg_wallpaper[FB_WIDTH * FB_HEIGHT];
bool g_has_wallpaper = false;

/* Full 8x8 ASCII Font (Characters 32 to 126) */
static const uint8_t g_font_8x8[95][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 32   */
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, /* 33 ! */
    {0x66, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 34 " */
    {0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00}, /* 35 # */
    {0x18, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x18, 0x00}, /* 36 $ */
    {0x00, 0x63, 0x66, 0x0C, 0x18, 0x33, 0x63, 0x00}, /* 37 % */
    {0x30, 0x48, 0x30, 0x54, 0x66, 0x44, 0x3A, 0x00}, /* 38 & */
    {0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 39 ' */
    {0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00}, /* 40 ( */
    {0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00}, /* 41 ) */
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, /* 42 * */
    {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00}, /* 43 + */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}, /* 44 , */
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}, /* 45 - */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}, /* 46 . */
    {0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00}, /* 47 / */
    {0x3C, 0x66, 0x6E, 0x7E, 0x76, 0x66, 0x3C, 0x00}, /* 48 0 */
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, /* 49 1 */
    {0x3C, 0x66, 0x06, 0x1C, 0x30, 0x66, 0x7E, 0x00}, /* 50 2 */
    {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00}, /* 51 3 */
    {0x0E, 0x1E, 0x36, 0x66, 0x7F, 0x06, 0x0F, 0x00}, /* 52 4 */
    {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00}, /* 53 5 */
    {0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00}, /* 54 6 */
    {0x7E, 0x66, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x00}, /* 55 7 */
    {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}, /* 56 8 */
    {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00}, /* 57 9 */
    {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00}, /* 58 : */
    {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x30, 0x00}, /* 59 ; */
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, /* 60 < */
    {0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00}, /* 61 = */
    {0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00}, /* 62 > */
    {0x3C, 0x66, 0x0C, 0x18, 0x18, 0x00, 0x18, 0x00}, /* 63 ? */
    {0x3C, 0x66, 0x6E, 0x6E, 0x60, 0x62, 0x3C, 0x00}, /* 64 @ */
    {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00}, /* 65 A */
    {0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00}, /* 66 B */
    {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00}, /* 67 C */
    {0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00}, /* 68 D */
    {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00}, /* 69 E */
    {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00}, /* 70 F */
    {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00}, /* 71 G */
    {0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00}, /* 72 H */
    {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}, /* 73 I */
    {0x0E, 0x06, 0x06, 0x06, 0x66, 0x66, 0x3C, 0x00}, /* 74 J */
    {0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00}, /* 75 K */
    {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00}, /* 76 L */
    {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00}, /* 77 M */
    {0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00}, /* 78 N */
    {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, /* 79 O */
    {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00}, /* 80 P */
    {0x3C, 0x66, 0x66, 0x66, 0x6E, 0x3C, 0x0E, 0x00}, /* 81 Q */
    {0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00}, /* 82 R */
    {0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00}, /* 83 S */
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* 84 T */
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, /* 85 U */
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}, /* 86 V */
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, /* 87 W */
    {0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00}, /* 88 X */
    {0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00}, /* 89 Y */
    {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00}, /* 90 Z */
    {0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00}, /* 91 [ */
    {0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00}, /* 92 \ */
    {0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00}, /* 93 ] */
    {0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00}, /* 94 ^ */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, /* 95 _ */
    {0x18, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 96 ` */
    {0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x3E, 0x00}, /* 97 a */
    {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00}, /* 98 b */
    {0x00, 0x00, 0x3C, 0x66, 0x60, 0x66, 0x3C, 0x00}, /* 99 c */
    {0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x00}, /* 100 d */
    {0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00}, /* 101 e */
    {0x0C, 0x18, 0x3E, 0x18, 0x18, 0x18, 0x18, 0x00}, /* 102 f */
    {0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x3C}, /* 103 g */
    {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00}, /* 104 h */
    {0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00}, /* 105 i */
    {0x06, 0x00, 0x0E, 0x06, 0x06, 0x66, 0x3C, 0x00}, /* 106 j */
    {0x60, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x00}, /* 107 k */
    {0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}, /* 108 l */
    {0x00, 0x00, 0x66, 0x7F, 0x7F, 0x6B, 0x63, 0x00}, /* 109 m */
    {0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00}, /* 110 n */
    {0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00}, /* 111 o */
    {0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60}, /* 112 p */
    {0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x06}, /* 113 q */
    {0x00, 0x00, 0x7C, 0x66, 0x60, 0x60, 0x60, 0x00}, /* 114 r */
    {0x00, 0x00, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x00}, /* 115 s */
    {0x18, 0x18, 0x7E, 0x18, 0x18, 0x18, 0x0E, 0x00}, /* 116 t */
    {0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00}, /* 117 u */
    {0x00, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}, /* 118 v */
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x3E, 0x36, 0x00}, /* 119 w */
    {0x00, 0x00, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x00}, /* 120 x */
    {0x00, 0x00, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x3C}, /* 121 y */
    {0x00, 0x00, 0x7E, 0x0C, 0x18, 0x30, 0x7E, 0x00}, /* 122 z */
    {0x0E, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0E, 0x00}, /* 123 { */
};

void fb_fill_rect(int x, int y, int w, int h, uint16_t color) {
    for (int j = y; j < y + h && j < FB_HEIGHT; j++) {
        if (j < 0) continue;
        for (int i = x; i < x + w && i < FB_WIDTH; i++) {
            if (i < 0) continue;
            g_backbuffer[j * FB_WIDTH + i] = color;
        }
    }
}

void fb_draw_rect(int x, int y, int w, int h, uint16_t color) {
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

void fb_draw_cog(int cx, int cy, uint16_t color) {
    fb_fill_rect(cx - 4, cy - 4, 9, 9, color);
    fb_fill_rect(cx - 2, cy - 7, 5, 3, color);
    fb_fill_rect(cx - 2, cy + 5, 5, 3, color);
    fb_fill_rect(cx - 7, cy - 2, 3, 5, color);
    fb_fill_rect(cx + 5, cy - 2, 3, 5, color);
    fb_fill_rect(cx - 5, cy - 5, 3, 3, color);
    fb_fill_rect(cx + 3, cy - 5, 3, 3, color);
    fb_fill_rect(cx - 5, cy + 3, 3, 3, color);
    fb_fill_rect(cx + 3, cy + 3, 3, 3, color);
    fb_fill_rect(cx - 1, cy - 1, 3, 3, RGB_BG);
}

void fb_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale) {
    if ((uint8_t)c < 32 || (uint8_t)c > 126) return;
    const uint8_t *glyph = g_font_8x8[(uint8_t)c - 32];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint16_t color = (bits & (1 << (7 - col))) ? fg : bg;
            if (color != bg || bg != 0) {
                fb_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

void fb_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale) {
    int cur_x = x;
    while (*text) {
        fb_draw_char(cur_x, y, *text, fg, bg, scale);
        cur_x += 8 * scale;
        text++;
    }
}

int fb_text_width_aa(const char *text, const font_aa_t *font) {
    if (!text || !font) return 0;
    int w = 0;
    while (*text) {
        uint8_t c = (uint8_t)*text++;
        if (c < 32 || c > 126) { w += 8; continue; }
        w += font->glyphs[c - 32].adv;
    }
    return w;
}

void fb_draw_text_aa(int x, int y, const char *text, uint16_t fg_color, const font_aa_t *font) {
    if (!text || !font) return;
    int pen_x = x;
    int pen_y = y;
    while (*text) {
        uint8_t c = (uint8_t)*text++;
        if (c < 32 || c > 126) {
            pen_x += 8;
            continue;
        }
        const glyph_aa_t *g = &font->glyphs[c - 32];
        if (g->w > 0 && g->h > 0) {
            const uint8_t *src = &font->alpha_data[g->offset];
            int dst_x = pen_x + g->ox;
            int dst_y = pen_y + g->oy;
            for (int r = 0; r < g->h; r++) {
                int py = dst_y + r;
                if (py < 0 || py >= FB_HEIGHT) continue;
                for (int col = 0; col < g->w; col++) {
                    int px = dst_x + col;
                    if (px < 0 || px >= FB_WIDTH) continue;
                    uint8_t alpha = src[r * g->w + col];
                    if (alpha == 0) continue;
                    if (alpha == 255) {
                        g_backbuffer[py * FB_WIDTH + px] = fg_color;
                    } else {
                        uint16_t bg = g_backbuffer[py * FB_WIDTH + px];
                        uint8_t bg_r = ((bg >> 11) & 0x1F) << 3;
                        uint8_t bg_g = ((bg >> 5) & 0x3F) << 2;
                        uint8_t bg_b = (bg & 0x1F) << 3;

                        uint8_t fg_r = ((fg_color >> 11) & 0x1F) << 3;
                        uint8_t fg_g = ((fg_color >> 5) & 0x3F) << 2;
                        uint8_t fg_b = (fg_color & 0x1F) << 3;

                        uint8_t out_r = (fg_r * alpha + bg_r * (255 - alpha)) >> 8;
                        uint8_t out_g = (fg_g * alpha + bg_g * (255 - alpha)) >> 8;
                        uint8_t out_b = (fg_b * alpha + bg_b * (255 - alpha)) >> 8;

                        g_backbuffer[py * FB_WIDTH + px] = ((out_r >> 3) << 11) | ((out_g >> 2) << 5) | (out_b >> 3);
                    }
                }
            }
        }
        pen_x += g->adv;
    }
}

/* Cached PNG asset loader and blitter */
typedef struct {
    char path[128];
    int w, h;
    unsigned char *rgba;
} cached_png_t;

#define MAX_CACHED_PNG 96
static cached_png_t g_png_cache[MAX_CACHED_PNG];
static int g_png_cache_count = 0;

bool blit_png_file(int dst_x, int dst_y, int target_w, int target_h, const char *filepath) {
    if (!filepath || !filepath[0]) return false;

    unsigned char *data = NULL;
    int w = 0, h = 0;
    bool is_cached = false;

    for (int i = 0; i < g_png_cache_count; i++) {
        if (strcmp(g_png_cache[i].path, filepath) == 0) {
            data = g_png_cache[i].rgba;
            w = g_png_cache[i].w;
            h = g_png_cache[i].h;
            is_cached = true;
            break;
        }
    }

    if (!data) {
        int comp = 0;
        data = stbi_load(filepath, &w, &h, &comp, 4);
        if (!data) return false;
        if (g_png_cache_count < MAX_CACHED_PNG) {
            strncpy(g_png_cache[g_png_cache_count].path, filepath, sizeof(g_png_cache[0].path) - 1);
            g_png_cache[g_png_cache_count].w = w;
            g_png_cache[g_png_cache_count].h = h;
            g_png_cache[g_png_cache_count].rgba = data;
            g_png_cache_count++;
            is_cached = true;
        }
    }

    for (int ty = 0; ty < target_h; ty++) {
        int py = dst_y + ty;
        if (py < 0 || py >= FB_HEIGHT) continue;
        int sy = (ty * h) / target_h;
        for (int tx = 0; tx < target_w; tx++) {
            int px = dst_x + tx;
            if (px < 0 || px >= FB_WIDTH) continue;
            int sx = (tx * w) / target_w;

            unsigned char *pixel = &data[(sy * w + sx) * 4];
            uint8_t a = pixel[3];
            if (a == 0) continue;

            uint8_t r = pixel[0];
            uint8_t g = pixel[1];
            uint8_t b = pixel[2];

            /* Stock Logitech icons are black line-art with alpha on transparent canvas.
             * On dark card backgrounds, convert dark glyph pixels to crisp white so they shine clearly! */
            if ((r + g + b) < 100 && a > 30) {
                r = 245;
                g = 245;
                b = 250;
            }

            if (a == 255) {
                g_backbuffer[py * FB_WIDTH + px] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            } else {
                uint16_t bg = g_backbuffer[py * FB_WIDTH + px];
                uint8_t bg_r = ((bg >> 11) & 0x1F) << 3;
                uint8_t bg_g = ((bg >> 5) & 0x3F) << 2;
                uint8_t bg_b = (bg & 0x1F) << 3;

                uint8_t out_r = (r * a + bg_r * (255 - a)) >> 8;
                uint8_t out_g = (g * a + bg_g * (255 - a)) >> 8;
                uint8_t out_b = (b * a + bg_b * (255 - a)) >> 8;

                g_backbuffer[py * FB_WIDTH + px] = ((out_r >> 3) << 11) | ((out_g >> 2) << 5) | (out_b >> 3);
            }
        }
    }
    if (!is_cached) {
        stbi_image_free(data);
    }
    return true;
}

void load_wallpaper_asset(void) {
    const char *paths[] = {
        "/data/background.png",
        "/opt/luadraws/appscommon/asset/background/b8bb2074-131e-11e2-ae9f-b3f5f4c18767.png",
        "/opt/luadraws/appscommon/asset/background/8d8f352a-131e-11e2-8581-b78bf89f9455.png",
        "/opt/luadraws/appscommon/asset/background/9927055c-131e-11e2-b78a-63d07f7b7808.png",
        "/opt/luadraws/appscommon/asset/background/9f67b146-131e-11e2-9510-7f6da0f7f23b.png",
        "/opt/luadraws/appscommon/asset/background/ab969216-131e-11e2-abe8-9710f8cc7eb0.png",
        "/opt/luadraws/appscommon/asset/background/b2bda41c-131e-11e2-9a42-b76c2c03ce1b.png",
        "/opt/luadraws/appscommon/asset/background/c27fb7a6-131d-11e2-8085-bb50198c538b.png",
        "/opt/luadraws/appscommon/asset/background/c6848614-131e-11e2-8672-dbd27517bd91.png",
        "/opt/luadraws/appscommon/asset/background/cd4e7be4-131e-11e2-888a-df753ee9c6df.png"
    };
    for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        int w = 0, h = 0, comp = 0;
        unsigned char *data = stbi_load(paths[i], &w, &h, &comp, 3);
        if (data && w == FB_WIDTH && h == FB_HEIGHT) {
            for (int j = 0; j < FB_WIDTH * FB_HEIGHT; j++) {
                uint8_t r = data[j * 3 + 0];
                uint8_t g = data[j * 3 + 1];
                uint8_t b = data[j * 3 + 2];
                g_bg_wallpaper[j] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
            stbi_image_free(data);
            g_has_wallpaper = true;
            printf("[+] Loaded stock wallpaper from %s\n", paths[i]);
            return;
        }
        if (data) stbi_image_free(data);
    }

    /* Fallback: sleek dark carbon-mesh gradient matching stock Logitech look */
    for (int y = 0; y < FB_HEIGHT; y++) {
        for (int x = 0; x < FB_WIDTH; x++) {
            uint8_t grad = (uint8_t)(14 + (y * 14) / FB_HEIGHT);
            if (((x / 2 + y / 2) & 1) == 0) grad += 3;
            g_bg_wallpaper[y * FB_WIDTH + x] = ((grad >> 3) << 11) | (((grad + 2) >> 2) << 5) | ((grad + 5) >> 3);
        }
    }
    g_has_wallpaper = true;
    printf("[*] Initialized high-res carbon mesh wallpaper\n");
}

void fb_invalidate_png_cache(const char *substr) {
    if (!substr) return;
    for (int i = 0; i < g_png_cache_count; i++) {
        if (strstr(g_png_cache[i].path, substr)) {
            g_png_cache[i].path[0] = '\0';
        }
    }
}
