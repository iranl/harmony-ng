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
            const char *ctx = (g_remote_mode == 1) ? ((g_current_view == VIEW_ACTIVITIES) ? "page1" : "page2") : g_active_activity_id;
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
