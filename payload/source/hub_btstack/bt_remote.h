#ifndef HARMONY_BT_REMOTE_H
#define HARMONY_BT_REMOTE_H

#include "bt_state.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *parse_button(const uint8_t *data, uint16_t len);
void trigger_http_post(const char *endpoint, const char *json_body);
char *read_entire_file(const char *path);
cJSON *get_cached_map(void);
void read_current_activity(char *out, size_t outlen);
int is_bt_device_id(const char *target);
bool is_remote_release_packet(const uint8_t *data, uint16_t len);
void dispatch_button_event(const char *btn, int is_press);
void dispatch_button(const char *btn);
void handle_gatt_notification_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
void bt_remote_on_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_REMOTE_H */
