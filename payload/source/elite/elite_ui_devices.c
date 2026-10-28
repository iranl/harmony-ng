#include "elite_ui.h"
#include "elite_fb.h"
#include "elite_config.h"
#include "elite_rf.h"

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
    int start_y = 38;
    int card_h = 34;
    int spacing = 5;
    int page_size = 6;
    int total = get_active_dev_cmd_count();

    if (total <= 0) {
        fb_fill_rect(12, start_y + 10, FB_WIDTH - 24, 120, RGB_CARD);
        fb_draw_rect(12, start_y + 10, FB_WIDTH - 24, 120, RGB_DIVIDER);
        fb_draw_text_aa(24, start_y + 30, "No Device Buttons", RGB_WHITE, &g_font_title);
        fb_draw_text_aa(24, start_y + 55, "Configure in WebUI", RGB_CYAN, &g_font_main);
        fb_draw_text_aa(24, start_y + 75, "for this activity", RGB_TEXT_MUTED, &g_font_main);
        return;
    }

    int page_start = (g_selected_device_cmd / page_size) * page_size;
    int cur_page = (g_selected_device_cmd / page_size) + 1;
    int total_pages = (total + page_size - 1) / page_size;

    for (int i = 0; i < page_size && (page_start + i) < total; i++) {
        int idx = page_start + i;
        int y = start_y + i * (card_h + spacing);
        bool is_sel = (idx == g_selected_device_cmd);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? RGB_TEAL : 0x2945;

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

    if (total_pages > 1) {
        char pstr[16];
        snprintf(pstr, sizeof(pstr), "%d/%d", cur_page, total_pages);
        fb_draw_text_aa(FB_WIDTH - 32, start_y + page_size * (card_h + spacing) - 2, pstr, RGB_TEXT_MUTED, &g_font_main);
    }
}

int browser_item_count(void) {
    if (g_browse_dev < 0) return g_rf_device_count;
    int n = 0;
    for (int i = 0; i < g_rf_dev_cmd_count; i++) {
        if (g_rf_dev_cmds[i].dev_idx == g_browse_dev) n++;
    }
    return n;
}

/* The nth command of the browsed device, in the order the Hub sent them */
const rf_dev_cmd_t *browser_cmd(int n) {
    for (int i = 0; i < g_rf_dev_cmd_count; i++) {
        if (g_rf_dev_cmds[i].dev_idx == g_browse_dev && n-- == 0) return &g_rf_dev_cmds[i];
    }
    return NULL;
}

void open_device_browser(void) {
    g_current_view = VIEW_DEVICE_BROWSER;
    g_browse_dev = -1;
    g_browse_sel = 0;
}

void device_browser_back(void) {
    if (g_browse_dev >= 0) {
        g_browse_sel = g_browse_dev;
        g_browse_dev = -1;
    } else {
        g_current_view = VIEW_DEVICES;
    }
}

void device_browser_move(int delta) {
    int n = browser_item_count();
    if (n > 0) g_browse_sel = ((g_browse_sel + delta) % n + n) % n;
}

void device_browser_page(int dir) {
    int n = browser_item_count();
    int page = g_browse_sel / 6;
    if (dir > 0) g_browse_sel = ((page + 1) * 6 < n) ? (page + 1) * 6 : (n > 0 ? n - 1 : 0);
    else g_browse_sel = (page > 0) ? (page - 1) * 6 : 0;
}

/* Open a device from the list, or send the chosen command to the Hub */
void device_browser_activate(int n) {
    if (n < 0 || n >= browser_item_count()) return;
    g_browse_sel = n;
    if (g_browse_dev < 0) {
        g_browse_dev = n;
        g_browse_sel = 0;
        return;
    }
    const rf_dev_cmd_t *c = browser_cmd(n);
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

/* Render the device list or the browsed device's commands (6 per page) */
void render_device_browser_view(void) {
    int start_y = 38;
    int card_h = 34;
    int spacing = 5;
    int page_size = 6;
    if (g_browse_dev >= g_rf_device_count) g_browse_dev = -1;
    int total = browser_item_count();
    if (g_browse_sel >= total) g_browse_sel = 0;

    if (total <= 0) {
        fb_fill_rect(12, start_y + 10, FB_WIDTH - 24, 120, RGB_CARD);
        fb_draw_rect(12, start_y + 10, FB_WIDTH - 24, 120, RGB_DIVIDER);
        fb_draw_text_aa(24, start_y + 30, g_browse_dev < 0 ? "No Devices" : "No Commands", RGB_WHITE, &g_font_title);
        fb_draw_text_aa(24, start_y + 55, "Configure in WebUI", RGB_CYAN, &g_font_main);
        return;
    }

    int page_start = (g_browse_sel / page_size) * page_size;
    int total_pages = (total + page_size - 1) / page_size;
    for (int i = 0; i < page_size && (page_start + i) < total; i++) {
        int idx = page_start + i;
        int y = start_y + i * (card_h + spacing);
        bool is_sel = (idx == g_browse_sel);
        const rf_dev_cmd_t *c = (g_browse_dev >= 0) ? browser_cmd(idx) : NULL;
        const char *label = (g_browse_dev < 0) ? g_rf_devices[idx].name : (c ? c->name : "");

        fb_fill_rect(8, y, FB_WIDTH - 16, card_h, is_sel ? RGB_CARD_SEL : RGB_CARD);
        fb_draw_rect(8, y, FB_WIDTH - 16, card_h, is_sel ? RGB_TEAL : 0x2945);
        fb_draw_text_aa(16, y + 9, label, is_sel ? RGB_WHITE : RGB_CYAN, &g_font_main);
        if (g_browse_dev < 0) {
            fb_draw_text_aa(FB_WIDTH - 24, y + 8, ">", is_sel ? RGB_CYAN : RGB_TEXT_MUTED, &g_font_title);
        }
    }

    if (total_pages > 1) {
        char pstr[16];
        snprintf(pstr, sizeof(pstr), "%d/%d", page_start / page_size + 1, total_pages);
        fb_draw_text_aa(FB_WIDTH - 32, start_y + page_size * (card_h + spacing) - 2, pstr, RGB_TEXT_MUTED, &g_font_main);
    }
}
