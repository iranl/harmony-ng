#ifndef HARMONY_ELITE_CONFIG_H
#define HARMONY_ELITE_CONFIG_H

#include "elite_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void load_settings_conf(void);
void save_settings_conf(void);
void load_rf_config(void);
void save_rf_config(void);
void load_activities(void);
void init_act_triggers(void);
void resolve_activity_icon(const char *image_key, const char *clean_name, char *out_path, size_t max_len);
bool resolve_button_icon(const char *icon_key, const char *label, char *out_path, size_t max_len);
void map_icon_type_to_str(uint8_t type, const char *name, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_ELITE_CONFIG_H */
