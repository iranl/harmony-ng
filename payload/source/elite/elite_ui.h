#ifndef HARMONY_ELITE_UI_H
#define HARMONY_ELITE_UI_H

#include "elite_state.h"
#include "elite_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Screen layout below the status bar, shared by drawing and touch handling */
#define UI_CONTENT_Y      38
#define ACT_PAGE_SIZE     5   /* activity cards per page */
#define ACT_CARD_H        50
#define ACT_CARD_GAP      4
#define ACT_GRID_COLS     2   /* the grid home layout: tiles per row */
#define ACT_GRID_ROWS     3
#define ACT_TILE_H        86
#define LIST_PAGE_SIZE    7   /* command and device rows per page */
#define LIST_ROW_H        34
#define LIST_ROW_GAP      5
#define SETTINGS_HEADING_Y 42 /* heading of a Settings page, below the status bar */
#define SETTINGS_LIST_Y   66
#define HDR_GEAR_W        34  /* touch width of the gear at the left of the status bar */
#define ACT_KEY_LONG_MS   800 /* Activities key held this long opens the menu */

/* Status bar elements that can be tapped; x < 0 when not shown */
typedef struct {
    int update_x, update_w;
    int nav_x, nav_w;
    int link_x, link_w;
    link_quality_t link;
} header_layout_t;

void show_notice(const char *msg, int duration_ms);
void render_frame(void);
void render_screen(void);
void render_header(void);
void header_layout(header_layout_t *l);
void render_key_hint(void);
void render_link_panel(void);
bool ui_link_panel_dismiss(void);
void render_page_indicator(int page, int pages);
/* The home layout (ADR-0008): the Settings choice, or the theme's when it is "theme". Drawing
   and touch handling place activities with these. */
bool home_layout_is_grid(void);
int activity_page_size(void);
void activity_slot_rect(int slot, int *x, int *y, int *w, int *h);
void render_sync_overlay(void);
void render_switching_view(void);

void render_activities_screen(void);
void render_devices_screen(void);
bool render_activity_hero(void);
void render_device_browser_view(void);
void render_settings_screen(void);
void render_pairing_screen(void);
void render_not_paired_screen(void);

int activity_list_count(void);
int browser_item_count(void);
const rf_dev_cmd_t *browser_cmd(int n);
void open_device_browser(void);
void device_browser_back(void);
void device_browser_move(int delta);
void device_browser_page(int dir);
void device_browser_activate(int n);

void trigger_activity_switch(const char *target_id, const char *target_name);
void trigger_activity_switch_external(const char *id, const char *name);
void request_activity_switch_with_confirm(int act_idx);
void execute_active_dev_cmd(int idx);
void execute_settings_action(int idx);
void execute_pairing_menu_opt(int idx);
int get_settings_sub_count(void);

/* Status bar menu (elite_ui_menu.c): opened from the gear or a long press of the Activities key */
void ui_menu_open(void);
void ui_menu_close(void);
void render_menu(void);
bool ui_menu_handle_key(int code);
void ui_menu_handle_tap(int x, int y);
bool ui_key_hint_dismiss(void);

/* Help (elite_ui_help.c): repairs the running activity, started from the menu */
void ui_help_start(void);
void ui_help_close(void);
bool ui_help_active(void);
void ui_help_on_status(const rf_frame_t *f);
void ui_help_tick(uint64_t now);
void render_help(void);
bool ui_help_handle_key(int code);
void ui_help_handle_tap(int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_ELITE_UI_H */
