/* Drag scrolling of the home screen's pages (ADR-0009). No hardware access: the host
 * renderer links it to draw frames mid-drag. */
#include "elite_ui_scroll.h"
#include "elite_ui.h"
#include <math.h>
#include <stdlib.h>

#define FLING_PX_PER_MS   0.4f /* a release faster than this moves to the neighbour page */
#define VELOCITY_WINDOW   60   /* ms of movement the release velocity is measured over */
#define EDGE_RESISTANCE   3    /* past the first or last page the page moves a third as far */
#define EDGE_MAX_PX       60
#define SETTLE_MIN_MS     120
#define SETTLE_MAX_MS     250

_Static_assert(ACT_PAGE_SIZE * (ACT_CARD_H + ACT_CARD_GAP) == HOME_PAGE_PITCH, "list page height");
_Static_assert(ACT_GRID_ROWS * (ACT_TILE_H + ACT_CARD_GAP) == HOME_PAGE_PITCH, "grid page height");

typedef struct { int y; uint64_t ms; } sample_t;

static struct {
    bool tracking;     /* a touch went down on the home screen's pages */
    bool dragging;     /* it moved past the tap slop, mostly vertically */
    int down_x, down_y;
    int offset;
    sample_t samples[8];
    int sample_count;
    bool settling;
    int settle_from, settle_to;
    uint64_t settle_start_ms;
    uint32_t settle_ms;
} s;

int touch_tap_slop(void) {
    return g_touch_sensitivity == 1 ? 16 : (g_touch_sensitivity == 3 ? 25 : 20);
}

static int page_count(void) {
    int size = activity_page_size();
    return (activity_list_count() + size - 1) / size;
}

static int current_page(void) {
    return g_selected_activity / activity_page_size();
}

static bool can_scroll(void) {
    return g_current_view == VIEW_ACTIVITIES && g_touch_enabled && !g_menu_open && !ui_help_active() &&
           !g_link_panel_open && !g_key_hint_pending && g_confirm_pending_idx < 0 &&
           !g_is_switching_activity && !g_is_syncing_config && !g_is_ota_updating && page_count() > 1;
}

/* The finger's travel as a page offset, resisting past the first and last page */
static int drag_offset(int dy) {
    bool no_prev = current_page() == 0, no_next = current_page() >= page_count() - 1;
    if ((dy > 0 && no_prev) || (dy < 0 && no_next)) {
        int edge = dy / EDGE_RESISTANCE;
        return edge > EDGE_MAX_PX ? EDGE_MAX_PX : (edge < -EDGE_MAX_PX ? -EDGE_MAX_PX : edge);
    }
    return dy > HOME_PAGE_PITCH ? HOME_PAGE_PITCH : (dy < -HOME_PAGE_PITCH ? -HOME_PAGE_PITCH : dy);
}

static void add_sample(int y, uint64_t ms) {
    int n = sizeof(s.samples) / sizeof(s.samples[0]);
    if (s.sample_count == n) {
        for (int i = 1; i < n; i++) s.samples[i - 1] = s.samples[i];
        s.sample_count--;
    }
    s.samples[s.sample_count++] = (sample_t){ y, ms };
}

/* Pixels per ms over the last VELOCITY_WINDOW ms; positive is downwards */
static float release_velocity(void) {
    if (s.sample_count < 2) return 0;
    const sample_t *last = &s.samples[s.sample_count - 1];
    const sample_t *first = &s.samples[0];
    for (int i = s.sample_count - 2; i >= 0; i--) {
        first = &s.samples[i];
        if (last->ms - first->ms >= VELOCITY_WINDOW) break;
    }
    if (last->ms <= first->ms) return 0;
    return (float)(last->y - first->y) / (float)(last->ms - first->ms);
}

void home_scroll_touch_down(int x, int y, uint64_t ms) {
    s.tracking = false;
    s.dragging = false;
    if (s.settling || !can_scroll() || y < UI_CONTENT_Y) return;
    s.tracking = true;
    s.down_x = x;
    s.down_y = y;
    s.sample_count = 0;
    add_sample(y, ms);
}

bool home_scroll_touch_move(int x, int y, uint64_t ms) {
    if (!s.tracking) return false;
    if (!can_scroll()) {
        home_scroll_cancel();
        return true;
    }
    add_sample(y, ms);
    int dx = x - s.down_x, dy = y - s.down_y;
    if (!s.dragging) {
        if (abs(dy) <= touch_tap_slop() || abs(dy) < abs(dx)) return false;
        s.dragging = true;
    }
    int offset = drag_offset(dy);
    if (offset == s.offset) return false;
    s.offset = offset;
    return true;
}

/* Ease-out cubic from the current offset to target. Its starting speed is 3 * distance /
   duration, so a duration of 3 * distance / release speed carries on at the finger's speed. */
static void settle_to(int target, float velocity, uint64_t ms) {
    s.settling = true;
    s.settle_from = s.offset;
    s.settle_to = target;
    s.settle_start_ms = ms;
    float speed = fabsf(velocity);
    float dur = speed > 0.05f ? 3.0f * (float)abs(target - s.offset) / speed : SETTLE_MAX_MS;
    s.settle_ms = dur < SETTLE_MIN_MS ? SETTLE_MIN_MS : (dur > SETTLE_MAX_MS ? SETTLE_MAX_MS : (uint32_t)dur);
}

bool home_scroll_touch_up(int x, int y, uint64_t ms) {
    (void)x;
    if (!s.tracking) return false;
    s.tracking = false;
    if (!s.dragging) return false;
    s.dragging = false;
    add_sample(y, ms);

    float v = release_velocity();
    int target = 0;
    bool has_prev = current_page() > 0, has_next = current_page() < page_count() - 1;
    if (has_prev && (s.offset > HOME_PAGE_PITCH / 3 || (v > FLING_PX_PER_MS && s.offset > 0))) {
        target = HOME_PAGE_PITCH;
    } else if (has_next && (s.offset < -HOME_PAGE_PITCH / 3 || (v < -FLING_PX_PER_MS && s.offset < 0))) {
        target = -HOME_PAGE_PITCH;
    }
    settle_to(target, v, ms);
    return true;
}

bool home_scroll_tick(uint64_t ms) {
    if (!s.settling) return false;
    if (g_current_view != VIEW_ACTIVITIES) {
        home_scroll_cancel();
        return true;
    }
    uint64_t t = ms - s.settle_start_ms;
    if (t >= s.settle_ms) {
        int size = activity_page_size();
        int page = current_page() + (s.settle_to < 0 ? 1 : (s.settle_to > 0 ? -1 : 0));
        if (page < 0) page = 0;
        if (page != current_page()) g_selected_activity = page * size;
        s.offset = 0;
        s.settling = false;
        return true;
    }
    float p = 1.0f - (float)t / (float)s.settle_ms;
    float eased = 1.0f - p * p * p;
    int offset = s.settle_from + (int)((float)(s.settle_to - s.settle_from) * eased);
    if (offset == s.offset) return false;
    s.offset = offset;
    return true;
}

bool home_scroll_active(void) {
    return s.dragging || s.settling;
}

bool home_scroll_settling(void) {
    return s.settling;
}

void home_scroll_cancel(void) {
    s.tracking = s.dragging = s.settling = false;
    s.offset = 0;
}

int home_scroll_offset(void) {
    return s.offset;
}

void home_scroll_set_offset(int px) {
    s.offset = px;
}
