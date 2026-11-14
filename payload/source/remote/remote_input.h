#ifndef HARMONY_REMOTE_INPUT_H
#define HARMONY_REMOTE_INPUT_H

#include "remote_state.h"

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
/* Release of a key whose press went out over RF: clears the held key and sends the release.
 * Returns false (and does nothing) when the key is not the held one. */
bool remote_release_held_rf_key(int code);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_INPUT_H */
