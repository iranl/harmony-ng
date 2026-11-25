#ifndef HARMONY_RF_KEYMAP_H
#define HARMONY_RF_KEYMAP_H

#include "rf_state.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

extern pthread_mutex_t g_map_lock;
/* hub_daemon's key channel socket (hub_key_channel.h); tests point it elsewhere */
extern const char *g_rf_key_sock_path;

const char *map_button_code(uint8_t k0, uint8_t k1);
void dispatch_remote_button(uint8_t slot, const char *button_name, const char *action_state, const char *dev_id);
void enqueue_remote_button(uint8_t slot, const char *name, const char *action, const char *dev_id);
void enqueue_remote_device_cmd(const char *dev_id, const char *cmd);
void enqueue_remote_activity(const char *act_id);
void *dispatch_worker(void *arg);
void handle_companion_packet(const uint8_t *pkt, size_t len);
void check_companion_timeouts(void);

cJSON *get_remote_map_unlocked(uint8_t slot);
bool get_remote_screen_button(uint8_t slot, const char *act_or_dev, int btn_idx, char *out_dev, size_t dev_sz, char *out_cmd, size_t cmd_sz);
int is_slot_direct_mode(uint8_t slot);
cJSON *get_remote_direct_map(int slot);

void dispatch_activity_to_daemon(const char *act_id);
void dispatch_device_command_to_webui(const char *payload);
void get_current_activity(char *out, size_t outlen);
/* The target of the running switch (/tmp/activity_target); 0 when none is written */
int get_activity_target(char *out, size_t outlen);
void get_activity_name_by_id(const char *act_id, char *out_name, size_t out_len);

cJSON *rf_get_devices_cache(void);
void rf_devices_cache_lock(void);
void rf_devices_cache_unlock(void);
cJSON *rf_get_activities_cache(void);
void rf_keymap_invalidate_devices_cache(void);
void rf_keymap_invalidate_activities_cache(void);
void rf_keymap_invalidate_map(uint8_t slot);

/* The key layer of a remote slot (1 to MAX_PAIRED_REMOTES), RAM only: while it is on, the keys of the map's
 * "layers" -> <name> object come before the running activity's own, where the name is the
 * activity's id for its own layer, or one set through rf_keymap_layer_set() (a device's menus,
 * say). It belongs to the activity it was turned on in, and is off once another activity, or
 * none, runs. A key bound as {"action": "layer_toggle", "on": {"deviceId", "command"},
 * "off": {"deviceId", "command"}} turns any layer off and sends "off", or, with none on, the
 * activity's own on and sends "on". active: 1 on (act_out = its activity, name_out = its name,
 * "" for the activity's own). set: name NULL or "" for the activity's own, letters, digits,
 * '_' and '-' otherwise, not digits only, and a layer the slot's map has; on only while an
 * activity runs and none is being started or stopped (a layer_toggle key turns none on then
 * either); -1 for a bad slot or name. A key's holds and its release send the command its
 * press sent, whatever the layer is by then, and nothing when the press sent none. Off
 * (PowerOffActivity) stops the activity unless the layer that is on binds it; a touchscreen
 * remote stops the activity itself and never sends Off as a key. */
int rf_keymap_layer_active(uint8_t slot, char *act_out, size_t act_len, char *name_out, size_t name_len);
int rf_keymap_layer_set(uint8_t slot, int on, const char *name);
/* Every slot's layer off: the activity changed, stopped or is being switched */
void rf_keymap_layers_off(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RF_KEYMAP_H */
