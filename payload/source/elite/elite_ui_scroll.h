#ifndef HARMONY_ELITE_UI_SCROLL_H
#define HARMONY_ELITE_UI_SCROLL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drag scrolling of the home screen's pages (ADR-0009). A drag moves the page with the
 * finger, up and down in the list and grid and sideways in the carousel; on release it
 * settles on a page boundary, moving to the neighbour page when dragged a third of a page or
 * flung. */

#define HOME_PAGE_PITCH 270 /* height of a home page: five list cards, three grid rows or a carousel card */

/* Movement in screen pixels that still counts as a tap, by touch sensitivity */
int touch_tap_slop(void);

void home_scroll_touch_down(int x, int y, uint64_t ms);
/* Returns whether the screen needs a new frame */
bool home_scroll_touch_move(int x, int y, uint64_t ms);
/* Returns whether the touch was a drag, which a tap handler should then ignore */
bool home_scroll_touch_up(int x, int y, uint64_t ms);
/* Advances a settle; returns whether the screen needs a new frame */
bool home_scroll_tick(uint64_t ms);
/* A drag or settle is running: frames are paced to the display, and slow work waits */
bool home_scroll_active(void);
/* A settle is running: the main loop draws its frames without waiting for input */
bool home_scroll_settling(void);
void home_scroll_cancel(void);
/* After a key moved the selection from old_page to a neighbour page, slides the new page in */
void home_scroll_slide_from(int old_page, uint64_t ms);
/* Pixels the current page is drawn below (carousel: right of) its place: positive shows the
   previous page */
int home_scroll_offset(void);
/* Sets the offset directly, for host renders of a frame mid-drag */
void home_scroll_set_offset(int px);

#ifdef __cplusplus
}
#endif

#endif
