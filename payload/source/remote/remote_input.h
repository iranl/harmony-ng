#ifndef HARMONY_REMOTE_INPUT_H
#define HARMONY_REMOTE_INPUT_H

#include "remote_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* While a key that goes to the Hub is down, a hold frame follows the press after this long and
 * then at this rate (the stock remote's timing) */
#define KEY_HOLD_INTERVAL_MS 300

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
/* A hard key, or the capacitive Activities / Devices key, is down now (asked from the kernel, so
 * a release that was never read cannot leave it set). A key down for more than
 * KEY_HELD_AWAKE_MAX_MS no longer counts: a key pressed in a sofa cushion must not drain the
 * battery. */
bool remote_key_held(void);
/* A key release was read: the next hold counts from its own press, not from an earlier hold
 * that no idle check saw end */
void remote_key_held_reset(void);
/* The kernel says this hard key is down now */
bool remote_hard_key_down(int code);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_INPUT_H */
