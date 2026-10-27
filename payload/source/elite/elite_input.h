#ifndef HARMONY_ELITE_INPUT_H
#define HARMONY_ELITE_INPUT_H

#include "elite_state.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint16_t g_held_rf_code;
extern pthread_mutex_t g_repeat_mutex;
extern pthread_cond_t g_repeat_cond;

int find_touchscreen_fd(void);
void handle_screen_touch_tap(int x, int y);
uint16_t linux_to_harmony_rf_key(int code);
void handle_navigation_key(int code, int value);
void *key_repeat_thread(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_ELITE_INPUT_H */
