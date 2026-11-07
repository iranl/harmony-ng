#ifndef HARMONY_REMOTE_CONFIG_H
#define HARMONY_REMOTE_CONFIG_H

#include "remote_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void load_settings_conf(void);
void save_settings_conf(void);
void load_rf_config(void);
/* Builds the home screen's activity list and icon paths from the received RF config */
void build_activity_list(void);
/* Activity starts on this remote, which order the home screen when "most used first" is chosen */
void load_activity_usage(void);
void record_activity_start(const char *id);
void flush_activity_usage(void);
void save_rf_config(void);
void load_activities(void);
void init_act_triggers(void);
void resolve_activity_icon(uint16_t icon_id, const char *image_key, const char *clean_name, char *out_path, size_t max_len);
bool resolve_device_icon(uint16_t icon_id, const char *icon_key, const char *dev_name, const char *dev_id, char *out_path, size_t max_len);
bool resolve_button_icon(const char *icon_key, const char *label, char *out_path, size_t max_len);
/* Switches to a theme and redraws: icon paths and the wallpaper depend on it */
void apply_theme(const char *name);
void map_icon_type_to_str(uint8_t type, const char *name, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_CONFIG_H */
