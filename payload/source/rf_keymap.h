#ifndef HARMONY_RF_KEYMAP_H
#define HARMONY_RF_KEYMAP_H

#include "rf_state.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

extern pthread_mutex_t g_map_lock;

const char *map_button_code(uint8_t k0, uint8_t k1);
void dispatch_elite_button(uint8_t slot, const char *button_name, const char *action_state, const char *dev_id);
void enqueue_elite_button(uint8_t slot, const char *name, const char *action, const char *dev_id);
void enqueue_elite_device_cmd(const char *dev_id, const char *cmd);
void enqueue_elite_activity(const char *act_id);
void *dispatch_worker(void *arg);
void handle_companion_packet(const uint8_t *pkt, size_t len);
void check_companion_timeouts(void);

cJSON *get_elite_map(void);
cJSON *get_remote_map(uint8_t slot);
bool get_remote_screen_button(uint8_t slot, const char *act_or_dev, int btn_idx, char *out_dev, size_t dev_sz, char *out_cmd, size_t cmd_sz);
int is_slot_direct_mode(uint8_t slot);
cJSON *get_elite_direct_map(int slot);

void dispatch_activity_to_daemon(const char *act_id);
void dispatch_device_command_to_webui(const char *payload);
void get_current_activity(char *out, size_t outlen);
void get_activity_name_by_id(const char *act_id, char *out_name, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RF_KEYMAP_H */
