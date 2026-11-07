/* Host build of the Harmony remote screen renderer.
 *
 * Draws the firmware's screens with the firmware's own drawing code (payload/source/remote)
 * from a directory laid out like the remote's filesystem, and writes PNGs.
 *
 *   remote_render render    --root DIR [state options] -o out.png
 *   remote_render sheet     --root DIR [--icons DIR] [--title T] -o out.png
 *   remote_render compare   --root DIR --set "Label=ICONDIR" ... -o out.png
 *   remote_render rasterize --svg in.svg --size N [--color #RRGGBB] [--pad F] -o out.png
 *   remote_render font      --ttf in.ttf --px N --height H --baseline B -o out.faa
 *
 * ICONDIR is a path on the remote's filesystem (for example /data/themes/phosphor) holding
 * <keyword>.png for the activity icon keywords; "classic" means the stock assets.
 * See tools/remote_render/README.md. */
#include "render_state.h"
#include "remote_fb.h"
#include "remote_ui.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static void usage(void) {
    fprintf(stderr,
        "usage: remote_render render|sheet|compare|rasterize|font [options] -o out.png\n"
        "  --root DIR        directory laid out like the remote's filesystem\n"
        "  --view NAME       activities|menu|hint|link|charging|commands|devices|device|settings|pairing|not-paired|sync|\n                    help-wait|help-question|help-done (render; help views need --active)\n"
        "  --select N        selected list row (render)\n"
        "  --active ID       running activity ID\n"
        "  --theme NAME      theme in /data/themes under the root (default classic)\n"
        "  --layout L        home layout: list, grid or carousel (default: the theme's)\n"
        "  --scroll PX       home pages or a list dragged PX pixels, as mid-drag (negative: towards the\n"
        "                    next page or rows; sideways in the carousel)\n"
        "  --icons DIR       activity icon directory on the remote's filesystem, or 'classic'\n"
        "  --title TEXT      heading of the keyword sheet (sheet)\n"
        "  --time HH:MM      clock shown in the status bar (default 21:30)\n"
        "  --battery PCT     battery level (default 80)\n"
        "  --usb             show USB power\n"
        "  --charging        show the battery charging\n"
        "  --no-hub          show the RF link as lost\n"
        "  --link-loss PCT   share of link reports and Hub replies lost (default 0: four bars)\n"
        "  --hub-silent      show the link up but the Hub not answering\n"
        "  --sync DONE/TOTAL config sync progress (with --view sync)\n"
        "  --set LABEL=DIR   icon set for compare (repeatable)\n"
        "  --scale N         integer upscale of the output (default 1)\n"
        "  --svg FILE --size N [--color #RRGGBB] [--pad F]   rasterize an SVG icon\n"
        "  --ttf FILE --px N --height H --baseline B         convert a font to the remote's .faa format\n");
    exit(2);
}

static void png_write(const char *path, const uint8_t *rgb, int w, int h, int scale) {
    int sw = w * scale, sh = h * scale;
    uint8_t *out = malloc((size_t)sw * sh * 3);
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            memcpy(&out[((size_t)y * sw + x) * 3], &rgb[((size_t)(y / scale) * w + x / scale) * 3], 3);
        }
    }
    if (!stbi_write_png(path, sw, sh, 3, out, sw * 3)) {
        fprintf(stderr, "[-] cannot write %s\n", path);
        exit(1);
    }
    free(out);
}

static void write_backbuffer(const char *path, int scale) {
    uint8_t *rgb = malloc(FB_WIDTH * FB_HEIGHT * 3);
    render_copy_out(rgb, FB_WIDTH, 0, 0, 3);
    png_write(path, rgb, FB_WIDTH, FB_HEIGHT, scale);
    free(rgb);
}

static int cmd_rasterize(const char *svg, int size, const char *color, float pad, const char *out) {
    FILE *f = fopen(svg, "rb");
    if (!f) { fprintf(stderr, "[-] cannot read %s\n", svg); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    buf[fread(buf, 1, (size_t)n, f)] = '\0';
    fclose(f);
    NSVGimage *img = nsvgParse(buf, "px", 96.0f);
    free(buf);
    if (!img || img->width <= 0 || img->height <= 0) { fprintf(stderr, "[-] cannot parse %s\n", svg); return 1; }

    if (color) { /* recolour every solid fill and stroke, keeping each shape's opacity */
        unsigned int rgb = (unsigned int)strtoul(color + (color[0] == '#'), NULL, 16);
        unsigned int abgr = ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
        for (NSVGshape *s = img->shapes; s; s = s->next) {
            if (s->fill.type == NSVG_PAINT_COLOR) s->fill.color = (s->fill.color & 0xFF000000u) | abgr;
            if (s->stroke.type == NSVG_PAINT_COLOR) s->stroke.color = (s->stroke.color & 0xFF000000u) | abgr;
        }
    }

    float inner = size * (1.0f - 2.0f * pad);
    float scale = inner / (img->width > img->height ? img->width : img->height);
    float tx = (size - img->width * scale) / 2.0f, ty = (size - img->height * scale) / 2.0f;
    unsigned char *px = calloc((size_t)size * size, 4);
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, img, tx, ty, scale, px, size, size, size * 4);
    int ok = stbi_write_png(out, size, size, 4, px, size * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    free(px);
    if (!ok) { fprintf(stderr, "[-] cannot write %s\n", out); return 1; }
    return 0;
}

/* Writes a font in the .faa format themes use (read by load_font in remote_theme.c): ASCII
   32..126 at px pixels per em, in lines height pixels tall with the baseline baseline pixels
   below the top, the layout of the built-in fonts (main: 15/12, title: 19/15) */
static int cmd_font(const char *ttf, float px, int height, int baseline, const char *out) {
    FILE *f = fopen(ttf, "rb");
    if (!f) { fprintf(stderr, "[-] cannot read %s\n", ttf); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)n);
    if (fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); fprintf(stderr, "[-] cannot read %s\n", ttf); return 1; }
    fclose(f);
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) { fprintf(stderr, "[-] not a font: %s\n", ttf); return 1; }
    float scale = stbtt_ScaleForMappingEmToPixels(&info, px);

    uint8_t head[10 + 95 * 7];
    uint8_t *alpha = NULL;
    uint32_t len = 0;
    for (int c = 32; c <= 126; c++) {
        int adv, lsb, w = 0, h = 0, xoff = 0, yoff = 0;
        stbtt_GetCodepointHMetrics(&info, c, &adv, &lsb);
        unsigned char *bmp = stbtt_GetCodepointBitmap(&info, 0, scale, c, &w, &h, &xoff, &yoff);
        if (w > 255 || h > 255 || len + (uint32_t)(w * h) > 0xFFFF) { fprintf(stderr, "[-] glyph too large\n"); return 1; }
        uint8_t *g = head + 10 + (c - 32) * 7;
        g[0] = (uint8_t)w; g[1] = (uint8_t)h;
        g[2] = (uint8_t)(int8_t)xoff; g[3] = (uint8_t)(int8_t)(baseline + yoff);
        g[4] = (uint8_t)(adv * scale + 0.5f);
        g[5] = (uint8_t)(len & 0xFF); g[6] = (uint8_t)(len >> 8);
        alpha = realloc(alpha, len + (uint32_t)(w * h) + 1);
        if (w * h) memcpy(alpha + len, bmp, (size_t)(w * h));
        len += (uint32_t)(w * h);
        stbtt_FreeBitmap(bmp, NULL);
    }
    memcpy(head, "FAA1", 4);
    head[4] = (uint8_t)height;
    head[5] = (uint8_t)height; /* ascent: the built-in fonts set it to the height */
    for (int i = 0; i < 4; i++) head[6 + i] = (uint8_t)(len >> (8 * i));
    FILE *o = fopen(out, "wb");
    if (!o || fwrite(head, 1, sizeof(head), o) != sizeof(head) || fwrite(alpha, 1, len, o) != len) {
        fprintf(stderr, "[-] cannot write %s\n", out);
        return 1;
    }
    fclose(o);
    free(alpha);
    free(data);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    const char *cmd = argv[1];
    render_options_t o = { .battery = 80, .minutes = 21 * 60 + 30 };
    const char *out = NULL, *svg = NULL, *color = NULL, *title = NULL, *ttf = NULL;
    float px = 0;
    int height = 0, baseline = 0;
    const char *sets[16];
    int set_count = 0, size = 40, scale = 1;
    float pad = 0.0f;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        int hh, mm;
#define TAKE() do { if (!v) usage(); i++; } while (0)
        if (!strcmp(a, "-o")) { TAKE(); out = v; }
        else if (!strcmp(a, "--root")) { TAKE(); o.root = v; }
        else if (!strcmp(a, "--view")) { TAKE(); o.view = v; }
        else if (!strcmp(a, "--select")) { TAKE(); o.select = atoi(v); }
        else if (!strcmp(a, "--active")) { TAKE(); o.active = v; }
        else if (!strcmp(a, "--theme")) { TAKE(); o.theme = v; }
        else if (!strcmp(a, "--layout")) { TAKE(); o.layout = v; }
        else if (!strcmp(a, "--scroll")) { TAKE(); o.scroll = atoi(v); }
        else if (!strcmp(a, "--icons")) { TAKE(); o.icons = v; }
        else if (!strcmp(a, "--title")) { TAKE(); title = v; }
        else if (!strcmp(a, "--time")) { TAKE(); if (sscanf(v, "%d:%d", &hh, &mm) != 2) usage(); o.minutes = hh * 60 + mm; }
        else if (!strcmp(a, "--battery")) { TAKE(); o.battery = atoi(v); }
        else if (!strcmp(a, "--usb")) { o.usb = true; }
        else if (!strcmp(a, "--charging")) { o.charging = true; }
        else if (!strcmp(a, "--no-hub")) { o.no_hub = true; }
        else if (!strcmp(a, "--link-loss")) { TAKE(); o.link_loss = atoi(v); }
        else if (!strcmp(a, "--hub-silent")) { o.hub_silent = true; }
        else if (!strcmp(a, "--sync")) { TAKE(); if (sscanf(v, "%d/%d", &o.sync_done, &o.sync_total) != 2) usage(); }
        else if (!strcmp(a, "--set")) { TAKE(); if (set_count < 16) sets[set_count++] = v; }
        else if (!strcmp(a, "--scale")) { TAKE(); scale = atoi(v) > 0 ? atoi(v) : 1; }
        else if (!strcmp(a, "--svg")) { TAKE(); svg = v; }
        else if (!strcmp(a, "--size")) { TAKE(); size = atoi(v); }
        else if (!strcmp(a, "--color")) { TAKE(); color = v; }
        else if (!strcmp(a, "--pad")) { TAKE(); pad = (float)atof(v); }
        else if (!strcmp(a, "--ttf")) { TAKE(); ttf = v; }
        else if (!strcmp(a, "--px")) { TAKE(); px = (float)atof(v); }
        else if (!strcmp(a, "--height")) { TAKE(); height = atoi(v); }
        else if (!strcmp(a, "--baseline")) { TAKE(); baseline = atoi(v); }
        else usage();
#undef TAKE
    }
    if (!out) usage();

    if (!strcmp(cmd, "rasterize")) {
        if (!svg || size <= 0) usage();
        return cmd_rasterize(svg, size, color, pad, out);
    }
    if (!strcmp(cmd, "font")) {
        if (!ttf || px <= 0 || height <= 0 || baseline <= 0 || height > 255) usage();
        return cmd_font(ttf, px, height, baseline, out);
    }
    if (!o.root) usage();

    setenv("TZ", "UTC0", 1);
    tzset();

    if (!strcmp(cmd, "render")) {
        render_load(&o);
        render_frame();
        write_backbuffer(out, scale);
    } else if (!strcmp(cmd, "sheet")) {
        render_load(&o);
        render_keyword_sheet(title ? title : (o.icons ? o.icons : "classic"), o.icons);
        write_backbuffer(out, scale);
    } else if (!strcmp(cmd, "compare")) {
        /* One column per icon set: the activities screen above, every keyword below */
        if (set_count == 0) usage();
        const int gap = 8;
        int cw = set_count * FB_WIDTH + (set_count + 1) * gap;
        int ch = 2 * FB_HEIGHT + 3 * gap;
        uint8_t *rgb = malloc((size_t)cw * ch * 3);
        memset(rgb, 0x30, (size_t)cw * ch * 3);
        for (int s = 0; s < set_count; s++) {
            char label[64];
            const char *eq = strchr(sets[s], '=');
            if (!eq) usage();
            snprintf(label, sizeof(label), "%.*s", (int)(eq - sets[s]), sets[s]);
            o.icons = eq + 1;
            render_load(&o);
            int ox = gap + s * (FB_WIDTH + gap);
            render_frame();
            render_copy_out(rgb, cw, ox, gap, 3);
            render_keyword_sheet(label, o.icons);
            render_copy_out(rgb, cw, ox, 2 * gap + FB_HEIGHT, 3);
        }
        png_write(out, rgb, cw, ch, scale);
        free(rgb);
    } else {
        usage();
    }
    return 0;
}
