#ifndef HARMONY_REMOTE_UI_SCROLL_H
#define HARMONY_REMOTE_UI_SCROLL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drag scrolling. Home: the home screen's pages. A drag moves the page with the
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

/* Lists: the device browser and the activity screen's commands scroll freely. A drag moves
 * the list with the finger; a fling carries on, slows down and stops with a row at the top. */

#define LIST_ROW_PITCH (LIST_ROW_H + LIST_ROW_GAP)

/* The list on screen: which list, its top, the rows that fit, its length and the selection */
typedef struct { int key, y, rows, count, sel; } list_geom_t;

/* Returns false when the screen shows no scrolling list */
bool list_geometry(list_geom_t *g);
void list_scroll_touch_down(int x, int y, uint64_t ms);
bool list_scroll_touch_move(int x, int y, uint64_t ms);
/* Returns whether the touch was a drag, or stopped a fling: a tap handler should ignore it */
bool list_scroll_touch_up(int x, int y, uint64_t ms);
bool list_scroll_tick(uint64_t ms);
/* After a key moved the selection, scrolls it into view */
void list_scroll_reveal(uint64_t ms);
/* Pixels of list scrolled above the top row's place; negative or past the end while dragged */
int list_scroll_offset(void);
/* Opens the list on screen afresh, then moves it as a drag of px pixels (positive: down), for
   host renders of a frame mid-drag */
void list_scroll_set_drag(int px);
/* Opens the list on screen afresh, scrolled px pixels down, within its ends */
void list_scroll_to(int px);
/* The row at screen y, or -1 */
int list_row_at(int y);
bool list_scroll_active(void);
bool list_scroll_settling(void);
void list_scroll_cancel(void);

/* Either scroller is moving: frames are paced to the display, and slow work waits */
bool ui_scroll_active(void);
/* Either is settling: the main loop draws its frames without waiting for input */
bool ui_scroll_settling(void);

#ifdef __cplusplus
}
#endif

#endif
