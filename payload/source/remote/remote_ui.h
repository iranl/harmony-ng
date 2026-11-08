#ifndef HARMONY_REMOTE_UI_H
#define HARMONY_REMOTE_UI_H

#include "remote_state.h"
#include "remote_link.h"

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
void render_charging_page(void);
void render_link_panel(void);
bool ui_link_panel_dismiss(void);
void render_page_indicator(int page, int pages);
/* The scrollbar of the list on screen (remote_ui_scroll.h), when it has more rows than fit */
void render_list_scrollbar(void);
/* The home layout: the Settings choice, or the theme's when it is "theme". Drawing
   and touch handling place activities with these. */
bool home_layout_is_grid(void);
bool home_layout_is_carousel(void);
/* The home layout in use, after the user's choice and the theme's: HOME_LAYOUT_LIST, _GRID or _CAROUSEL */
int home_layout(void);
const char *home_layout_name(void);
/* Shows the running activity's page in the carousel */
void home_select_running(void);
int activity_page_size(void);
void activity_slot_rect(int slot, int *x, int *y, int *w, int *h);
/* The activity on the current home page under (x, y), or -1 */
int home_activity_at(int x, int y);
/* On the running activity's carousel card, the device row at y (an index into g_rf_devices), or -1 */
int home_card_device_at(int y);
void render_sync_overlay(void);
void render_switching_view(void);

void render_activities_screen(void);
void render_devices_screen(void);
bool render_activity_hero(void);
const rf_activity_t *rf_activity_by_id(const char *id);
/* An activity's large icon, name, state line and devices, x pixels from the left, with its
   device rows ending at bottom */
void draw_activity_card(const rf_activity_t *act, int x, int bottom, const char *state, uint16_t state_color);
/* The device (an index into g_rf_devices) on the card's row at screen y, or -1 */
int activity_card_device_at(const rf_activity_t *act, int bottom, int y);
void render_device_browser_view(void);
void render_settings_screen(void);
void render_pairing_screen(void);
void render_not_paired_screen(void);

int activity_list_count(void);
int browser_item_count(void);
const rf_dev_cmd_t *browser_cmd(int n);
/* Device browser rows per page, and where they start: inside a device, below its heading */
int browser_page_size(void);
int browser_list_y(void);
void open_device_browser(void);
/* Opens a device's commands from an activity card; Back returns to the card */
void open_device_from_card(int dev);
void device_browser_back(void);
/* One screen back, for the Back key and a left-to-right swipe; false when there is nowhere to go */
bool ui_go_back(void);
void device_browser_move(int delta);
void device_browser_activate(int n);

void trigger_activity_switch(const char *target_id, const char *target_name);
void trigger_activity_switch_external(const char *id, const char *name);
void request_activity_switch_with_confirm(int act_idx);
void execute_active_dev_cmd(int idx);
void execute_settings_action(int idx);
int pair_opt_count(void);
void execute_pairing_menu_opt(int idx);
int get_settings_sub_count(void);

/* Status bar menu (remote_ui_menu.c): opened from the gear or a long press of the Activities key */
void ui_menu_open(void);
void ui_menu_close(void);
void render_menu(void);
bool ui_menu_handle_key(int code);
void ui_menu_handle_tap(int x, int y);
bool ui_key_hint_dismiss(void);
bool ui_charging_page_active(void);
bool ui_charging_page_dismiss(void);

/* Help (remote_ui_help.c): repairs the running activity, started from the menu */
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

#endif /* HARMONY_REMOTE_UI_H */
