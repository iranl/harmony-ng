#ifndef HARMONY_ELITE_UI_H
#define HARMONY_ELITE_UI_H

#include "elite_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void show_notice(const char *msg, int duration_ms);
void render_screen(void);
void render_header(void);
void render_footer(void);
void render_sync_overlay(void);
void render_switching_view(void);

void render_activities_screen(void);
void render_devices_screen(void);
void render_settings_screen(void);
void render_pairing_screen(void);
void render_not_paired_screen(void);

void trigger_activity_switch(const char *target_id, const char *target_name);
void trigger_activity_switch_external(const char *id, const char *name);
void request_activity_switch_with_confirm(int act_idx);
void execute_active_dev_cmd(int idx);
void execute_settings_action(int idx);
void execute_pairing_menu_opt(int idx);
int get_settings_sub_count(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_ELITE_UI_H */
