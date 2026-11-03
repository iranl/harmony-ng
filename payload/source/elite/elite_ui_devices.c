#include "elite_ui.h"
#include "elite_fb.h"
#include "elite_config.h"
#include "elite_rf.h"
#include "elite_ui_scroll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void execute_active_dev_cmd(int filtered_idx) {
    char label[32] = {0};
    uint8_t btn_idx = 0, atype = 1;
    if (get_active_dev_cmd_item(filtered_idx, label, sizeof(label), &btn_idx, &atype, NULL, 0) == 0) {
        if (atype == 2) {
            trigger_activity_switch(g_switching_target_id, label);
        } else if (atype == 3) {
            trigger_activity_switch("-1", "PowerOff");
        } else {
            const char *ctx = g_active_activity_id;
            rf_send_screen_button_press(ctx, btn_idx, label);
        }
    }
}

void render_devices_screen(void) {
    int start_y = UI_CONTENT_Y;
    int card_h = LIST_ROW_H;
    int total = get_active_dev_cmd_count();

    if (total <= 0) {
        if (render_activity_hero()) return;
        fb_draw_text_aa(8, start_y + 20, tr(STR_NO_COMMANDS), RGB_WHITE, &g_font_title);
        fb_draw_text_aa(8, start_y + 45, tr(STR_CONFIGURE_IN_WEBUI), RGB_CYAN, &g_font_main);
        fb_draw_text_aa(8, start_y + 65, tr(STR_FOR_THIS_ACTIVITY), RGB_TEXT_MUTED, &g_font_main);
        return;
    }

    list_geom_t g;
    list_geometry(&g);
    int scroll = list_scroll_offset();
    int first = scroll > 0 ? scroll / LIST_ROW_PITCH : 0;
    fb_clip_rows(g.y, g.y + g.rows * LIST_ROW_PITCH);
    for (int idx = first; idx < total; idx++) {
        int y = start_y + idx * LIST_ROW_PITCH - scroll;
        if (y >= g.y + g.rows * LIST_ROW_PITCH) break;
        bool is_sel = (idx == g_selected_device_cmd);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : g_theme.pal.outline;

        char label[32] = {0};
        char icon_key[24] = {0};
        uint8_t bidx = 0, atype = 1;
        get_active_dev_cmd_item(idx, label, sizeof(label), &bidx, &atype, icon_key, sizeof(icon_key));

        fb_fill_rect(8, y, FB_WIDTH - 16, card_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, card_h, border);

        char btn_icon_path[128] = {0};
        bool has_btn_icon = resolve_button_icon(icon_key, label, btn_icon_path, sizeof(btn_icon_path));
        if (has_btn_icon) {
            has_btn_icon = blit_png_file(14, y + (card_h - 22) / 2, 22, 22, btn_icon_path);
        }

        int text_x = has_btn_icon ? 42 : 16;
        fb_draw_text_aa(text_x, y + 9, label, is_sel ? RGB_WHITE : RGB_CYAN, &g_font_main);

        if (is_sel) {
            fb_draw_text_aa(FB_WIDTH - 24, y + 8, ">", RGB_CYAN, &g_font_title);
        }
    }
    fb_clip_reset();
    render_list_scrollbar();
}

/* Inside a device the rows are its commands, below a heading with the device's name */
int browser_item_count(void) {
    if (g_browse_dev < 0) return g_rf_device_count;
    int n = 0;
    for (int i = 0; i < g_rf_dev_cmd_count; i++) {
        if (g_rf_dev_cmds[i].dev_idx == g_browse_dev) n++;
    }
    return n;
}

/* The command on row n of the browsed device, in the order the Hub sent them */
const rf_dev_cmd_t *browser_cmd(int n) {
    for (int i = 0; i < g_rf_dev_cmd_count; i++) {
        if (g_rf_dev_cmds[i].dev_idx == g_browse_dev && n-- == 0) return &g_rf_dev_cmds[i];
    }
    return NULL;
}

int browser_page_size(void) {
    return g_browse_dev >= 0 ? LIST_PAGE_SIZE - 1 : LIST_PAGE_SIZE;
}

int browser_list_y(void) {
    return g_browse_dev >= 0 ? SETTINGS_LIST_Y : UI_CONTENT_Y;
}

/* The screen showing the activity card a device was opened from, or -1 */
static int s_card_view = -1;

void open_device_browser(void) {
    g_current_view = VIEW_DEVICE_BROWSER;
    g_browse_dev = -1;
    g_browse_sel = 0;
    s_card_view = -1;
}

void open_device_from_card(int dev) {
    if (dev < 0 || dev >= g_rf_device_count) return;
    s_card_view = (int)g_current_view;
    g_current_view = VIEW_DEVICE_BROWSER;
    g_browse_dev = dev;
    g_browse_sel = 0;
}

/* From a device back to the card it was opened from, or to the device list; from the list to
   the running activity, or home */
void device_browser_back(void) {
    if (g_browse_dev >= 0 && s_card_view >= 0) {
        g_current_view = (ui_view_t)s_card_view;
        g_browse_dev = -1;
        s_card_view = -1;
    } else if (g_browse_dev >= 0) {
        g_browse_sel = g_browse_dev;
        g_browse_dev = -1;
    } else if (g_active_activity_id[0] && strcmp(g_active_activity_id, "-1") != 0) {
        g_current_view = VIEW_DEVICES;
    } else {
        g_current_view = VIEW_ACTIVITIES;
    }
}

void device_browser_move(int delta) {
    int n = browser_item_count();
    if (n > 0) g_browse_sel = ((g_browse_sel + delta) % n + n) % n;
}

/* Open a device from the list, or send the chosen command to the Hub. A command stored without
   its value takes one from the slider, so a tap only says how to use it. */
void device_browser_activate(int n) {
    if (n < 0 || n >= browser_item_count()) return;
    g_browse_sel = n;
    if (g_browse_dev < 0) {
        g_browse_dev = n;
        g_browse_sel = 0;
        return;
    }
    const rf_dev_cmd_t *c = browser_cmd(n);
    if (c && c->ranged) {
        show_notice(tr(STR_SLIDE_TO_SET), 2000);
        return;
    }
    if (!c || g_rf_fd < 0) return;
    uint8_t payload[25] = {0};
    memcpy(payload, g_rf_devices[g_browse_dev].id, strnlen(g_rf_devices[g_browse_dev].id, 4));
    payload[4] = c->cmd_idx;
    memcpy(&payload[5], c->name, strnlen(c->name, 20));
    rf_frame_t f;
    rf_build_frame(&f, g_remote_slot, RF_MSG_DEVICE_CMD_INDEX, ++g_tx_seq, 0, payload, sizeof(payload));
    rf_send_frame(&f);
    printf("[*] RF TX DEVICE_CMD_INDEX: dev=%s idx=%d cmd=%s\n", g_rf_devices[g_browse_dev].id, c->cmd_idx, c->name);
}

/* Slide: a sideways drag on a command stored without its value sets the value, filling the
   row from its left edge to the finger. The value is sent as the finger moves, at most every
   SLIDER_SEND_MS, and once more where the finger lifts. An up or down drag still scrolls the
   list. */

#define SLIDER_LEFT    8
#define SLIDER_WIDTH   (FB_WIDTH - 16)
#define SLIDER_SEND_MS 200

static struct {
    bool tracking; /* went down on a slider row and has not moved past the tap slop yet */
    bool sliding;
    int row;
    int down_x, down_y;
    int value;
    int sent;        /* the value last sent during this slide, or -1 */
    uint64_t sent_ms;
} s_slide;

static rf_dev_cmd_t *ranged_cmd_at(int row) {
    if (g_current_view != VIEW_DEVICE_BROWSER || g_browse_dev < 0 || row < 0) return NULL;
    rf_dev_cmd_t *c = (rf_dev_cmd_t *)browser_cmd(row);
    return c && c->ranged ? c : NULL;
}

/* The value at screen x, from vmin at the card's left edge to vmax at its right edge */
static int slider_value_at(const rf_dev_cmd_t *c, int x) {
    int pos = x < SLIDER_LEFT ? 0 : (x > SLIDER_LEFT + SLIDER_WIDTH ? SLIDER_WIDTH : x - SLIDER_LEFT);
    return c->vmin + (pos * (c->vmax - c->vmin) + SLIDER_WIDTH / 2) / SLIDER_WIDTH;
}

static void send_cmd_value(const rf_dev_cmd_t *c, int value) {
    if (g_rf_fd < 0) return;
    uint8_t payload[25] = {0};
    memcpy(payload, g_rf_devices[g_browse_dev].id, strnlen(g_rf_devices[g_browse_dev].id, 4));
    payload[4] = c->cmd_idx;
    payload[5] = (uint8_t)value;
    memcpy(&payload[6], c->name, strnlen(c->name, 19));
    rf_frame_t f;
    rf_build_frame(&f, g_remote_slot, RF_MSG_DEVICE_CMD_VALUE, ++g_tx_seq, 0, payload, sizeof(payload));
    rf_send_frame(&f);
    printf("[*] RF TX DEVICE_CMD_VALUE: dev=%s idx=%d cmd=%s value=%d\n", g_rf_devices[g_browse_dev].id, c->cmd_idx, c->name, value);
}

/* Sends the slider's value unless it was sent already, or another was sent under
   SLIDER_SEND_MS ago and force is false */
static void slider_send(uint64_t ms, bool force) {
    rf_dev_cmd_t *c = ranged_cmd_at(s_slide.row);
    if (!c || s_slide.value == s_slide.sent) return;
    if (!force && s_slide.sent >= 0 && ms - s_slide.sent_ms < SLIDER_SEND_MS) return;
    c->value = (int16_t)s_slide.value;
    send_cmd_value(c, s_slide.value);
    s_slide.sent = s_slide.value;
    s_slide.sent_ms = ms;
}

void value_slider_touch_down(int x, int y, uint64_t ms) {
    (void)ms;
    s_slide.tracking = s_slide.sliding = false;
    if (!g_touch_enabled || g_menu_open || ui_help_active() || g_link_panel_open || g_key_hint_pending) return;
    int row = list_row_at(y);
    if (!ranged_cmd_at(row)) return;
    s_slide.tracking = true;
    s_slide.row = row;
    s_slide.down_x = x;
    s_slide.down_y = y;
}

/* Called before the list sees the move, so a sideways drag never scrolls it */
bool value_slider_touch_move(int x, int y, uint64_t ms) {
    const rf_dev_cmd_t *c = ranged_cmd_at(s_slide.row);
    if (s_slide.tracking) {
        int dx = abs(x - s_slide.down_x), dy = abs(y - s_slide.down_y);
        if (dx <= touch_tap_slop() && dy <= touch_tap_slop()) return false;
        s_slide.tracking = false;
        if (dx < dy || !c) return false;
        list_scroll_cancel();
        g_browse_sel = s_slide.row;
        s_slide.sliding = true;
        s_slide.value = slider_value_at(c, x);
        s_slide.sent = -1;
        slider_send(ms, true);
        return true;
    }
    if (!s_slide.sliding) return false;
    if (!c) {
        s_slide.sliding = false;
        return true;
    }
    int v = slider_value_at(c, x);
    if (v == s_slide.value) return false;
    s_slide.value = v;
    slider_send(ms, false);
    return true;
}

void value_slider_tick(uint64_t ms) {
    if (s_slide.sliding) slider_send(ms, false);
}

bool value_slider_touch_up(int x, int y, uint64_t ms) {
    (void)y;
    s_slide.tracking = false;
    if (!s_slide.sliding) return false;
    const rf_dev_cmd_t *c = ranged_cmd_at(s_slide.row);
    if (c) {
        s_slide.value = slider_value_at(c, x);
        slider_send(ms, true);
    }
    s_slide.sliding = false;
    return true;
}

bool value_slider_active(void) {
    return s_slide.sliding;
}

/* a mixed with b, weight 0 (all b) to 256 (all a), in RGB565 */
static uint16_t mix565(uint16_t a, uint16_t b, int weight) {
    int r = (((a >> 11) & 0x1F) * weight + ((b >> 11) & 0x1F) * (256 - weight)) >> 8;
    int g = (((a >> 5) & 0x3F) * weight + ((b >> 5) & 0x3F) * (256 - weight)) >> 8;
    int bl = ((a & 0x1F) * weight + (b & 0x1F) * (256 - weight)) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* A slider row: the card filled with the accent from its left edge to the value, brighter
   while the finger is on it, and the value at the right, or the range before one was sent */
static void render_value_fill(const rf_dev_cmd_t *c, int y, int card_h, bool sliding, int value) {
    if (value >= 0) {
        int fill = SLIDER_WIDTH * (value - c->vmin) / (c->vmax - c->vmin);
        fb_fill_rect(SLIDER_LEFT + 1, y + 1, fill - 2, card_h - 2, mix565(RGB_TEAL, RGB_CARD, sliding ? 200 : 120));
        if (sliding && fill > 0) fb_fill_rect(SLIDER_LEFT + fill - 3, y + 1, 3, card_h - 2, RGB_WHITE);
    }
    if (sliding) fb_draw_rect(SLIDER_LEFT, y, SLIDER_WIDTH, card_h, RGB_WHITE);
    char text[12];
    if (value >= 0) snprintf(text, sizeof(text), "%d", value);
    else snprintf(text, sizeof(text), "%d-%d", c->vmin, c->vmax);
    const font_aa_t *font = value >= 0 ? &g_font_title : &g_font_main;
    int tw = fb_text_width_aa(text, font);
    fb_draw_text_aa(FB_WIDTH - 18 - tw, y + (value >= 0 ? 8 : 9), text, value >= 0 ? RGB_WHITE : RGB_TEXT_MUTED, font);
}

/* Render the device list, or the browsed device's heading and commands */
void render_device_browser_view(void) {
    if (g_browse_dev >= g_rf_device_count) g_browse_dev = -1;
    int start_y = browser_list_y();
    int card_h = LIST_ROW_H;
    int total = browser_item_count();
    if (g_browse_sel >= total) g_browse_sel = 0;

    if (g_browse_dev >= 0) {
        char fit[RF_NAME_SIZE];
        fb_draw_text_aa(14, SETTINGS_HEADING_Y, fb_fit_text_aa(fit, sizeof(fit), g_rf_devices[g_browse_dev].name, &g_font_title, FB_WIDTH - 28), RGB_CYAN, &g_font_title);
        if (total <= 0) {
            fb_draw_text_aa(14, start_y + 10, tr(STR_CONFIGURE_IN_WEBUI), RGB_TEXT_MUTED, &g_font_main);
            return;
        }
    }

    if (total <= 0) {
        fb_draw_text_aa(8, start_y + 20, tr(STR_NO_DEVICES), RGB_WHITE, &g_font_title);
        fb_draw_text_aa(8, start_y + 45, tr(STR_CONFIGURE_IN_WEBUI), RGB_CYAN, &g_font_main);
        return;
    }

    list_geom_t g;
    list_geometry(&g);
    int scroll = list_scroll_offset();
    int first = scroll > 0 ? scroll / LIST_ROW_PITCH : 0;
    fb_clip_rows(g.y, g.y + g.rows * LIST_ROW_PITCH);
    for (int idx = first; idx < total; idx++) {
        int y = start_y + idx * LIST_ROW_PITCH - scroll;
        if (y >= g.y + g.rows * LIST_ROW_PITCH) break;
        bool is_sel = (idx == g_browse_sel);
        const rf_dev_cmd_t *c = (g_browse_dev >= 0) ? browser_cmd(idx) : NULL;
        const char *label = (g_browse_dev < 0) ? g_rf_devices[idx].name : (c ? c->name : "");

        fb_fill_rect(8, y, FB_WIDTH - 16, card_h, is_sel ? RGB_CARD_SEL : RGB_CARD);
        fb_draw_rect(8, y, FB_WIDTH - 16, card_h, is_sel ? RGB_TEAL : g_theme.pal.outline);

        /* Device images are 45 px line art, drawn at 28 px; command glyphs stay at 22 px */
        int icon_size = g_browse_dev < 0 ? 28 : 22;
        char dev_icon_path[128] = {0};
        bool has_icon = false;
        if (g_browse_dev < 0) {
            if (resolve_device_icon(g_rf_devices[idx].icon_id, g_rf_devices[idx].icon, g_rf_devices[idx].name, g_rf_devices[idx].id, dev_icon_path, sizeof(dev_icon_path))) {
                has_icon = blit_png_file(14, y + (card_h - icon_size) / 2, icon_size, icon_size, dev_icon_path);
            }
        } else {
            if (resolve_button_icon(NULL, label, dev_icon_path, sizeof(dev_icon_path))) {
                has_icon = blit_png_file(14, y + (card_h - icon_size) / 2, icon_size, icon_size, dev_icon_path);
            }
        }

        int text_x = has_icon ? 14 + icon_size + 6 : 16;
        int text_w = FB_WIDTH - 30 - text_x;
        if (c && c->ranged) {
            bool sliding = s_slide.sliding && s_slide.row == idx;
            render_value_fill(c, y, card_h, sliding, sliding ? s_slide.value : c->value);
            text_w -= 50; /* room for the value */
        }
        char fit[RF_NAME_SIZE];
        fb_fit_text_aa(fit, sizeof(fit), label, &g_font_main, text_w);
        fb_draw_text_aa(text_x, y + 9, fit, is_sel || (c && c->ranged) ? RGB_WHITE : RGB_CYAN, &g_font_main);
        if (g_browse_dev < 0) {
            fb_draw_text_aa(FB_WIDTH - 24, y + 8, ">", is_sel ? RGB_CYAN : RGB_TEXT_MUTED, &g_font_title);
        }
    }
    fb_clip_reset();
    render_list_scrollbar();
}

/* A thumb at the right edge of the list, sized to the share of rows on screen */
void render_list_scrollbar(void) {
    list_geom_t g;
    if (!list_geometry(&g) || g.count <= g.rows) return;
    int track = g.rows * LIST_ROW_PITCH - LIST_ROW_GAP;
    int max = (g.count - g.rows) * LIST_ROW_PITCH;
    int thumb = track * g.rows / g.count;
    if (thumb < 16) thumb = 16;
    int scroll = list_scroll_offset();
    if (scroll < 0) scroll = 0;
    if (scroll > max) scroll = max;
    fb_fill_rect(FB_WIDTH - 5, g.y + (track - thumb) * scroll / max, 3, thumb, RGB_TEXT_MUTED);
}
