#ifndef HARMONY_BT_HID_H
#define HARMONY_BT_HID_H

#include "bt_state.h"

#ifdef __cplusplus
extern "C" {
#endif

extern btstack_timer_source_t g_host_key_release_timer;
extern uint8_t g_host_pending_release_type;
extern uint8_t g_host_custom_release_buf[16];
extern int g_host_custom_release_len;
extern int g_host_send_hold_mode;
extern uint8_t g_host_mouse_buttons;

void send_host_release_all(void);
void send_host_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
void send_host_mouse_click(uint8_t button);
void send_host_keyboard_report(uint8_t modifier, uint8_t keycode);
void send_host_consumer_report(uint16_t usage_id);
int parse_hex_bytes(const char *hex, uint8_t *out, int max_len);
bool send_host_named_key(const char *name);
bool send_host_named_key_ex(const char *name, int hold_mode);
void send_host_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_HID_H */
