#include "remote_theme.h"
#include "remote_state.h"
#include <dirent.h>

#define STOCK_ASSET_PREFIX "/opt/luadraws/appscommon/asset/"

/* A macro, so g_theme's static initializer can use it too */
#define CLASSIC_PALETTE_INIT { \
    .bg = 0x0000, .card = 0x10A2, .card_sel = 0x1945, .card_border = 0x2965, .divider = 0x2965, \
    .outline = 0x2945, .text = 0xFFFF, .text_muted = 0xD6DA, .on_accent = 0x0000, .accent = 0x067A, \
    .success = 0x27E8, .danger = 0xFA08, .danger_dim = 0x8000, .warning = 0xFFE0, .warning_bg = 0x2100, \
    .info = 0x2D7F, .header = 0x0861, .status_bg = 0x0841, .icon_well = 0x18C3, .icon_well_sel = 0x2125, \
    .icon_well_border = 0x31A6, .icon_outline = 0x32A9, \
}
static const theme_palette_t CLASSIC_PALETTE = CLASSIC_PALETTE_INIT;

theme_t g_theme = {
    .name = "classic", .label = "Classic", .pal = CLASSIC_PALETTE_INIT,
    .font_main = &g_font_main_classic, .font_title = &g_font_title_classic, .line_art = { 245, 245, 250 },
};

static const struct { const char *name; size_t offset; } PALETTE_ROLES[] = {
#define ROLE(r) { #r, offsetof(theme_palette_t, r) }
    ROLE(bg), ROLE(card), ROLE(card_sel), ROLE(card_border), ROLE(divider), ROLE(outline), ROLE(text),
    ROLE(text_muted), ROLE(on_accent), ROLE(accent), ROLE(success), ROLE(danger), ROLE(danger_dim),
    ROLE(warning), ROLE(warning_bg), ROLE(info), ROLE(header), ROLE(status_bg), ROLE(icon_well),
    ROLE(icon_well_sel), ROLE(icon_well_border), ROLE(icon_outline),
#undef ROLE
};

static bool readable(const char *path) {
    char buf[256];
    return access(remote_fs_path(path, buf, sizeof(buf)), R_OK) == 0;
}

static char *read_file(const char *path, long *size_out) {
    char buf[256];
    FILE *f = fopen(remote_fs_path(path, buf, sizeof(buf)), "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = (size > 0 && size < 512 * 1024) ? malloc(size + 1) : NULL;
    if (data && fread(data, 1, size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) data[size] = '\0';
    if (size_out) *size_out = size;
    return data;
}

/* A font in the .faa format the host renderer writes (tools/remote_render, "font"):
   "FAA1", height, ascent, data length (u32 LE), 95 glyphs of w, h, ox, oy, adv, offset (u16 LE)
   for ASCII 32..126, then the alpha data. One allocation, so free() releases it. */
static font_aa_t *load_font(const char *path) {
    long size = 0;
    uint8_t *file = (uint8_t *)read_file(path, &size);
    const long head = 4 + 2 + 4, glyphs = 95 * 7;
    if (!file || size < head + glyphs || memcmp(file, "FAA1", 4) != 0) {
        free(file);
        return NULL;
    }
    uint32_t data_len = file[6] | file[7] << 8 | (uint32_t)file[8] << 16 | (uint32_t)file[9] << 24;
    if (size < head + glyphs + (long)data_len) {
        free(file);
        return NULL;
    }
    font_aa_t *font = malloc(sizeof(font_aa_t) + 95 * sizeof(glyph_aa_t) + data_len);
    if (!font) {
        free(file);
        return NULL;
    }
    glyph_aa_t *g = (glyph_aa_t *)(font + 1);
    uint8_t *alpha = (uint8_t *)(g + 95);
    for (int i = 0; i < 95; i++) {
        const uint8_t *r = file + head + i * 7;
        g[i] = (glyph_aa_t){ r[0], r[1], (int8_t)r[2], (int8_t)r[3], r[4], (uint16_t)(r[5] | r[6] << 8) };
        if (g[i].offset + g[i].w * g[i].h > data_len) g[i].w = g[i].h = 0;
    }
    memcpy(alpha, file + head + glyphs, data_len);
    font->height = file[4];
    font->ascent = file[5];
    font->glyphs = g;
    font->alpha_data = alpha;
    free(file);
    return font;
}

static void set_font(const font_aa_t **slot, const font_aa_t *classic, const cJSON *fonts, const char *role) {
    if (*slot != classic) free((void *)*slot);
    *slot = classic;
    const cJSON *file = cJSON_GetObjectItem(fonts, role);
    if (!cJSON_IsString(file)) return;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", g_theme.dir, file->valuestring);
    font_aa_t *font = load_font(path);
    if (font) *slot = font;
    else printf("[-] Theme %s: can't load font %s\n", g_theme.name, path);
}

/* "#RRGGBB" as RGB565 */
static bool parse_colour(const char *s, uint16_t *out) {
    unsigned int r, g, b;
    if (!s || strlen(s) != 7 || sscanf(s, "#%2x%2x%2x", &r, &g, &b) != 3) return false;
    *out = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return true;
}

void theme_load(const char *name) {
    const font_aa_t *main_font = g_theme.font_main, *title_font = g_theme.font_title;
    snprintf(g_theme.name, sizeof(g_theme.name), "classic");
    snprintf(g_theme.label, sizeof(g_theme.label), "Classic");
    g_theme.dir[0] = '\0';
    g_theme.pal = CLASSIC_PALETTE;
    g_theme.layout = THEME_LAYOUT_LIST;
    g_theme.wallpaper[0] = '\0';
    g_theme.line_art[0] = 245; g_theme.line_art[1] = 245; g_theme.line_art[2] = 250;
    g_theme.font_main = main_font;
    g_theme.font_title = title_font;

    cJSON *root = NULL;
    if (name && name[0] && strcmp(name, "classic") != 0 && !strchr(name, '/')) {
        char path[160];
        snprintf(path, sizeof(path), THEME_DIR "/%s/theme.json", name);
        char *json = read_file(path, NULL);
        root = json ? cJSON_Parse(json) : NULL;
        free(json);
        if (root) {
            snprintf(g_theme.name, sizeof(g_theme.name), "%s", name);
            snprintf(g_theme.dir, sizeof(g_theme.dir), THEME_DIR "/%s", name);
            const cJSON *label = cJSON_GetObjectItem(root, "name");
            snprintf(g_theme.label, sizeof(g_theme.label), "%s", cJSON_IsString(label) ? label->valuestring : name);
        } else {
            printf("[-] Theme %s: can't read %s, using Classic\n", name, path);
        }
    }

    const cJSON *palette = cJSON_GetObjectItem(root, "palette");
    for (size_t i = 0; i < sizeof(PALETTE_ROLES) / sizeof(PALETTE_ROLES[0]); i++) {
        const cJSON *c = cJSON_GetObjectItem(palette, PALETTE_ROLES[i].name);
        if (cJSON_IsString(c)) parse_colour(c->valuestring, (uint16_t *)((char *)&g_theme.pal + PALETTE_ROLES[i].offset));
    }
    const cJSON *line_art = cJSON_GetObjectItem(root, "line_art");
    unsigned int lr, lg, lb;
    if (cJSON_IsString(line_art) && strlen(line_art->valuestring) == 7 &&
        sscanf(line_art->valuestring, "#%2x%2x%2x", &lr, &lg, &lb) == 3) {
        g_theme.line_art[0] = (uint8_t)lr; g_theme.line_art[1] = (uint8_t)lg; g_theme.line_art[2] = (uint8_t)lb;
    }
    const cJSON *fonts = cJSON_GetObjectItem(root, "fonts");
    set_font(&g_theme.font_main, &g_font_main_classic, fonts, "main");
    set_font(&g_theme.font_title, &g_font_title_classic, fonts, "title");
    const cJSON *layout = cJSON_GetObjectItem(root, "layout");
    if (cJSON_IsString(layout) && strcmp(layout->valuestring, "grid") == 0) g_theme.layout = THEME_LAYOUT_GRID;
    if (cJSON_IsString(layout) && strcmp(layout->valuestring, "carousel") == 0) g_theme.layout = THEME_LAYOUT_CAROUSEL;
    const cJSON *wallpaper = cJSON_GetObjectItem(root, "wallpaper");
    if (cJSON_IsString(wallpaper) && strcmp(wallpaper->valuestring, "none") == 0) {
        snprintf(g_theme.wallpaper, sizeof(g_theme.wallpaper), "none");
    } else if (cJSON_IsString(wallpaper) && wallpaper->valuestring[0]) {
        snprintf(g_theme.wallpaper, sizeof(g_theme.wallpaper), "%s/%s", g_theme.dir, wallpaper->valuestring);
    }
    cJSON_Delete(root);
}

void theme_next_name(char *out, size_t len) {
    /* The next name after the current one in sort order; Classic sorts before every theme */
    char next[32] = "", buf[256];
    bool is_classic = strcmp(g_theme.name, "classic") == 0;
    DIR *d = opendir(remote_fs_path(THEME_DIR, buf, sizeof(buf)));
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(next) || strcmp(e->d_name, "classic") == 0) continue;
        char json[160];
        snprintf(json, sizeof(json), THEME_DIR "/%s/theme.json", e->d_name);
        if (!readable(json)) continue;
        if ((is_classic || strcmp(e->d_name, g_theme.name) > 0) && (!next[0] || strcmp(e->d_name, next) < 0)) {
            snprintf(next, sizeof(next), "%s", e->d_name);
        }
    }
    if (d) closedir(d);
    snprintf(out, len, "%s", next[0] ? next : "classic");
}

bool theme_keyword_icon(const char *keyword, char *out, size_t len) {
    if (!g_theme.dir[0] || !keyword || !keyword[0]) return false;
    char path[160];
    snprintf(path, sizeof(path), "%s/icons/%s_small.png", g_theme.dir, keyword);
    if (!readable(path)) return false;
    snprintf(out, len, "%s", path);
    return true;
}

const char *theme_stock_path(const char *path, char *buf, size_t len) {
    size_t n = strlen(STOCK_ASSET_PREFIX);
    if (!g_theme.dir[0] || !path || strncmp(path, STOCK_ASSET_PREFIX, n) != 0) return path;
    snprintf(buf, len, "%s/stock/%s", g_theme.dir, path + n);
    return readable(buf) ? buf : path;
}
