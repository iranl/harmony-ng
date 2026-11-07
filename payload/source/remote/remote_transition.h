#ifndef HARMONY_REMOTE_TRANSITION_H
#define HARMONY_REMOTE_TRANSITION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Screen transitions: a snapshot of the screen before a change and one of the
 * screen after it are composited for 150-200 ms. Going deeper slides the new screen in from
 * the right, going back slides it in from the left, other changes crossfade. The status bar
 * stays still. */

typedef enum { TRANSITION_NONE = 0, TRANSITION_PUSH, TRANSITION_POP, TRANSITION_FADE } transition_t;

/* Where a screen sits: its section (home, devices, Settings) and its depth in it */
typedef struct {
    int section;
    int depth;
    int id;        /* tells apart screens at the same depth */
    bool overlay;  /* switching, sync, OTA, Help, the key hint or the link panel: no transition */
} screen_place_t;

screen_place_t screen_place(void);
transition_t transition_between(screen_place_t from, screen_place_t to);
/* The next screen change is a step back, whatever the places say */
void transition_hint_back(void);
/* The screen was off: the next frame has nothing to transition from */
void transition_reset(void);

/* Composites the frame at progress 0..256 into out */
void transition_compose(transition_t t, const uint16_t *from, const uint16_t *to, uint16_t *out, int progress);

/* render_screen() calls transition_prepare() before drawing a frame, while g_backbuffer still
 * holds the frame on screen. When it returns a transition, transition_play() shows it once
 * the new frame is drawn, ending on the new frame. */
transition_t transition_prepare(screen_place_t place);
void transition_play(transition_t t);

#ifdef __cplusplus
}
#endif

#endif
