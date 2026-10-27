#include "elite_ui.h"
#include "elite_fb.h"
#include "elite_rf.h"

#include <stdio.h>
#include <string.h>

void execute_pairing_menu_opt(int idx) {
    bool is_paired = g_rf_link_connected || (g_pairing_state == 2) ||
                     (g_paired_hub_addr[0] | g_paired_hub_addr[1] | g_paired_hub_addr[2] | g_paired_hub_addr[3]) != 0;

    if (g_unpair_confirm_active) {
        if (idx == 0) {
            rf_unpair();
        } else {
            g_unpair_confirm_active = false;
        }
        g_selected_pair_opt = 0;
    } else if (g_pairing_state == 1) {
        if (idx == 0) {
            rf_stop_pairing();
        } else {
            g_current_view = VIEW_SETTINGS;
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_selected_sub_item = 2;
        }
        g_selected_pair_opt = 0;
    } else if (is_paired) {
        if (idx == 0) {
            g_unpair_confirm_active = true;
            g_selected_pair_opt = 0;
        } else {
            g_current_view = VIEW_SETTINGS;
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_selected_sub_item = 2;
        }
    } else {
        if (idx == 0) {
            rf_start_pairing();
            g_selected_pair_opt = 0;
        } else {
            g_current_view = VIEW_SETTINGS;
            g_settings_subview = SETTINGS_VIEW_SYSTEM;
            g_selected_sub_item = 2;
        }
    }
    render_screen();
}

void render_pairing_screen(void) {
    int start_y = 42;
    int card_h = 104;

    fb_fill_rect(8, start_y, FB_WIDTH - 16, card_h, RGB_CARD);
    fb_draw_rect(8, start_y, FB_WIDTH - 16, card_h, RGB_CARD_BORDER);

    fb_draw_text_aa(16, start_y + 8, "RF HARDWARE & LINK", RGB_CYAN, &g_font_title);

    bool is_paired = g_rf_link_connected || (g_pairing_state == 2) ||
                     (g_paired_hub_addr[0] | g_paired_hub_addr[1] | g_paired_hub_addr[2] | g_paired_hub_addr[3]) != 0;

    char status_str[48];
    if (g_unpair_confirm_active) {
        snprintf(status_str, sizeof(status_str), "UNPAIR CONFIRMATION");
        fb_draw_text_aa(16, start_y + 27, status_str, RGB_YELLOW, &g_font_main);
    } else if (g_pairing_state == 1) {
        snprintf(status_str, sizeof(status_str), "Status: PAIRING IN PROGRESS...");
        fb_draw_text_aa(16, start_y + 27, status_str, RGB_YELLOW, &g_font_main);
    } else if (is_paired) {
        snprintf(status_str, sizeof(status_str), "Status: PAIRED TO HUB (OK)");
        fb_draw_text_aa(16, start_y + 27, status_str, RGB_GREEN, &g_font_main);
    } else if (g_pairing_state == 3) {
        snprintf(status_str, sizeof(status_str), "Status: PAIRING FAILED / TIMEOUT");
        fb_draw_text_aa(16, start_y + 27, status_str, RGB_RED, &g_font_main);
    } else {
        snprintf(status_str, sizeof(status_str), "Status: NOT PAIRED");
        fb_draw_text_aa(16, start_y + 27, status_str, RGB_RED, &g_font_main);
    }

    char remote_addr_str[48];
    if (g_remote_own_addr[0] || g_remote_own_addr[1] || g_remote_own_addr[2] || g_remote_own_addr[3]) {
        snprintf(remote_addr_str, sizeof(remote_addr_str), "Remote RF: %02X%02X%02X%02X (Slot %d)",
                 g_remote_own_addr[0], g_remote_own_addr[1], g_remote_own_addr[2], g_remote_own_addr[3], g_remote_slot);
    } else {
        snprintf(remote_addr_str, sizeof(remote_addr_str), "Remote RF: [ READING... ] (Slot %d)", g_remote_slot);
    }
    fb_draw_text_aa(16, start_y + 46, remote_addr_str, RGB_WHITE, &g_font_main);

    char hub_addr_str[48];
    if (g_paired_hub_addr[0] || g_paired_hub_addr[1] || g_paired_hub_addr[2] || g_paired_hub_addr[3]) {
        snprintf(hub_addr_str, sizeof(hub_addr_str), "Paired Hub: %02X%02X%02X%02X",
                 g_paired_hub_addr[0], g_paired_hub_addr[1], g_paired_hub_addr[2], g_paired_hub_addr[3]);
        fb_draw_text_aa(16, start_y + 65, hub_addr_str, RGB_TEAL, &g_font_main);
    } else if (g_pairing_state == 1) {
        fb_draw_text_aa(16, start_y + 65, "Paired Hub: [ SEARCHING... ]", RGB_YELLOW, &g_font_main);
    } else {
        fb_draw_text_aa(16, start_y + 65, "Paired Hub: [ NONE / UNPAIRED ]", RGB_TEXT_MUTED, &g_font_main);
    }

    if (g_unpair_confirm_active) {
        fb_draw_text_aa(16, start_y + 84, "Wipe bonding & local config?", RGB_YELLOW, &g_font_main);
    } else {
        fb_draw_text_aa(16, start_y + 84, "Protocol: eQuad / CC2544", RGB_TEXT_MUTED, &g_font_main);
    }

    const char *opts[PAIR_OPT_COUNT];
    if (g_unpair_confirm_active) {
        opts[0] = "Confirm Unpair & Wipe";
        opts[1] = "Cancel";
    } else if (g_pairing_state == 1) {
        opts[0] = "Stop Pairing";
        opts[1] = "Back to Settings";
    } else if (is_paired) {
        opts[0] = "Unpair from Hub";
        opts[1] = "Back to Settings";
    } else {
        opts[0] = "Start Pairing to Hub";
        opts[1] = "Back to Settings";
    }

    int opt_y = start_y + 114;
    int opt_h = 40;
    int spacing = 10;

    for (int i = 0; i < PAIR_OPT_COUNT; i++) {
        int y = opt_y + i * (opt_h + spacing);
        bool is_sel = (i == g_selected_pair_opt);
        uint16_t bg = is_sel ? RGB_CARD_SEL : RGB_CARD;
        uint16_t border = is_sel ? (g_unpair_confirm_active && i == 0 ? RGB_RED : RGB_TEAL) : 0x2945;
        uint16_t fg = is_sel ? (g_unpair_confirm_active && i == 0 ? RGB_RED : RGB_WHITE) : RGB_TEXT_MUTED;

        fb_fill_rect(8, y, FB_WIDTH - 16, opt_h, bg);
        fb_draw_rect(8, y, FB_WIDTH - 16, opt_h, border);

        fb_draw_text_aa(18, y + 12, opts[i], fg, &g_font_main);

        if (is_sel) {
            fb_draw_text_aa(FB_WIDTH - 28, y + 11, ">", (g_unpair_confirm_active && i == 0 ? RGB_RED : RGB_TEAL), &g_font_title);
        }
    }
}

void render_not_paired_screen(void) {
    int card_x = 10;
    int card_y = 42;
    int card_w = FB_WIDTH - 20;
    int card_h = 232;

    fb_fill_rect(card_x, card_y, card_w, card_h, RGB_CARD);
    fb_draw_rect(card_x, card_y, card_w, card_h, RGB_CARD_BORDER);

    if (!blit_png_file((FB_WIDTH - 48) / 2, card_y + 16, 48, 48, "/opt/luadraws/appscommon/asset/pairing/connectHub.png")) {
        fb_fill_rect((FB_WIDTH - 40) / 2, card_y + 20, 40, 40, RGB_CARD_SEL);
        fb_draw_rect((FB_WIDTH - 40) / 2, card_y + 20, 40, 40, RGB_RED);
    }

    int tw = fb_text_width_aa("REMOTE NOT PAIRED", &g_font_title);
    fb_draw_text_aa((FB_WIDTH - tw) / 2, card_y + 74, "REMOTE NOT PAIRED", RGB_WHITE, &g_font_title);

    int m1w = fb_text_width_aa("Connect with Harmony Hub", &g_font_main);
    fb_draw_text_aa((FB_WIDTH - m1w) / 2, card_y + 102, "Connect with Harmony Hub", RGB_TEXT_MUTED, &g_font_main);

    int m2w = fb_text_width_aa("to control activities & devices.", &g_font_main);
    fb_draw_text_aa((FB_WIDTH - m2w) / 2, card_y + 120, "to control activities & devices.", RGB_TEXT_MUTED, &g_font_main);

    int m3w = fb_text_width_aa("Tap button below to pair:", &g_font_main);
    fb_draw_text_aa((FB_WIDTH - m3w) / 2, card_y + 144, "Tap button below to pair:", RGB_CYAN, &g_font_main);

    int btn_x = 24;
    int btn_y = card_y + 166;
    int btn_w = FB_WIDTH - 48;
    int btn_h = 44;

    fb_fill_rect(btn_x, btn_y, btn_w, btn_h, RGB_CARD_SEL);
    fb_draw_rect(btn_x, btn_y, btn_w, btn_h, RGB_TEAL);
    fb_fill_rect(btn_x + 4, btn_y + btn_h - 3, btn_w - 8, 2, RGB_TEAL);

    int bw = fb_text_width_aa("PAIR TO HUB", &g_font_title);
    fb_draw_text_aa((FB_WIDTH - bw) / 2, btn_y + 13, "PAIR TO HUB", RGB_WHITE, &g_font_title);
}
