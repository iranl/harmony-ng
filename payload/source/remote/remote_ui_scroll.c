/* Drag scrolling: the home screen's pages, up and down in the list and grid and
 * sideways in the carousel, and the free-scrolling lists. No hardware access: the host
 * renderer links it to draw frames mid-drag. */
#include "remote_ui_scroll.h"
#include "remote_ui.h"
#include <math.h>
#include <stdlib.h>

#define FLING_PX_PER_MS   0.4f /* a release faster than this moves to the neighbour page */
#define VELOCITY_WINDOW   60   /* ms of movement the release velocity is measured over */
#define EDGE_RESISTANCE   3    /* past the first or last page the page moves a third as far */
#define EDGE_MAX_PX       60
#define SETTLE_MIN_MS     120
#define SETTLE_MAX_MS     250
#define SLIDE_MS          200  /* a key moving to the neighbour page (at most 200 ms) */

_Static_assert(ACT_PAGE_SIZE * (ACT_CARD_H + ACT_CARD_GAP) == HOME_PAGE_PITCH, "list page height");
_Static_assert(ACT_GRID_ROWS * (ACT_TILE_H + ACT_CARD_GAP) == HOME_PAGE_PITCH, "grid page height");

typedef struct { int pos; uint64_t ms; } sample_t; /* finger position along the scroll axis */
typedef struct { sample_t samples[8]; int count; } tracker_t;

static struct {
    bool tracking;     /* a touch went down on the home screen's pages */
    bool dragging;     /* it moved past the tap slop, mostly vertically */
    int down_x, down_y;
    int offset;
    tracker_t finger;
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

static bool sideways(void) {
    return home_layout_is_carousel();
}

static int pitch(void) {
    return sideways() ? FB_WIDTH : HOME_PAGE_PITCH;
}

static bool can_scroll(void) {
    return g_current_view == VIEW_ACTIVITIES && g_touch_enabled && !g_menu_open && !ui_help_active() &&
           !g_link_panel_open && !g_key_hint_pending && !ui_charging_page_active() && g_confirm_pending_idx < 0 &&
           !g_is_switching_activity && !g_is_syncing_config && !g_is_ota_updating && page_count() > 1;
}

/* The finger's travel as a page offset, resisting past the first and last page */
static int drag_offset(int d) {
    bool no_prev = current_page() == 0, no_next = current_page() >= page_count() - 1;
    if ((d > 0 && no_prev) || (d < 0 && no_next)) {
        int edge = d / EDGE_RESISTANCE;
        return edge > EDGE_MAX_PX ? EDGE_MAX_PX : (edge < -EDGE_MAX_PX ? -EDGE_MAX_PX : edge);
    }
    return d > pitch() ? pitch() : (d < -pitch() ? -pitch() : d);
}

static void add_sample(tracker_t *t, int pos, uint64_t ms) {
    int n = sizeof(t->samples) / sizeof(t->samples[0]);
    if (t->count == n) {
        for (int i = 1; i < n; i++) t->samples[i - 1] = t->samples[i];
        t->count--;
    }
    t->samples[t->count++] = (sample_t){ pos, ms };
}

/* Pixels per ms over the last VELOCITY_WINDOW ms; positive is down or right */
static float release_velocity(const tracker_t *t) {
    if (t->count < 2) return 0;
    const sample_t *last = &t->samples[t->count - 1];
    const sample_t *first = &t->samples[0];
    for (int i = t->count - 2; i >= 0; i--) {
        first = &t->samples[i];
        if (last->ms - first->ms >= VELOCITY_WINDOW) break;
    }
    if (last->ms <= first->ms) return 0;
    return (float)(last->pos - first->pos) / (float)(last->ms - first->ms);
}

void home_scroll_touch_down(int x, int y, uint64_t ms) {
    s.tracking = false;
    s.dragging = false;
    if (s.settling || !can_scroll() || y < UI_CONTENT_Y) return;
    s.tracking = true;
    s.down_x = x;
    s.down_y = y;
    s.finger.count = 0;
    add_sample(&s.finger, sideways() ? x : y, ms);
}

bool home_scroll_touch_move(int x, int y, uint64_t ms) {
    if (!s.tracking) return false;
    if (!can_scroll()) {
        home_scroll_cancel();
        return true;
    }
    add_sample(&s.finger, sideways() ? x : y, ms);
    int along = sideways() ? x - s.down_x : y - s.down_y;
    int across = sideways() ? y - s.down_y : x - s.down_x;
    if (!s.dragging) {
        if (abs(along) <= touch_tap_slop() || abs(along) < abs(across)) return false;
        s.dragging = true;
    }
    int offset = drag_offset(along);
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
    if (!s.tracking) return false;
    s.tracking = false;
    if (!s.dragging) return false;
    s.dragging = false;
    add_sample(&s.finger, sideways() ? x : y, ms);

    float v = release_velocity(&s.finger);
    int target = 0;
    bool has_prev = current_page() > 0, has_next = current_page() < page_count() - 1;
    if (has_prev && (s.offset > pitch() / 3 || (v > FLING_PX_PER_MS && s.offset > 0))) {
        target = pitch();
    } else if (has_next && (s.offset < -pitch() / 3 || (v < -FLING_PX_PER_MS && s.offset < 0))) {
        target = -pitch();
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

void home_scroll_slide_from(int old_page, uint64_t ms) {
    int delta = current_page() - old_page;
    if ((delta != 1 && delta != -1) || g_current_view != VIEW_ACTIVITIES) return;
    s.tracking = s.dragging = false;
    /* Start with the old page in place, which is the new page one pitch away */
    s.offset = delta > 0 ? pitch() : -pitch();
    settle_to(0, 0, ms);
    s.settle_ms = SLIDE_MS;
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

/* Free-scrolling lists: the device browser's rows and the activity screen's commands */

#define LIST_FLING_TAU    325  /* ms: a fling travels its release speed times this */
#define LIST_SETTLE_MS    150
#define LIST_FLING_MAX_MS 1000
#define LIST_BOUNCE_MS    250  /* back from past the first or last row */
#define LIST_REVEAL_MS    120  /* a key moved the selection off the visible rows */

static struct {
    int key;           /* the list the scroll belongs to: changes when the screen shows another */
    int scroll;        /* pixels of list above the first visible row's place */
    bool tracking;
    bool dragging;
    bool caught;       /* the touch stopped a fling: its release is not a tap */
    int down_x, down_y, down_scroll;
    tracker_t finger;
    bool settling;
    int settle_from, settle_to;
    uint64_t settle_start_ms;
    uint32_t settle_ms;
} l = { .key = -1 };

bool list_geometry(list_geom_t *g) {
    if (g_current_view == VIEW_DEVICES) {
        *g = (list_geom_t){ 1, UI_CONTENT_Y, LIST_PAGE_SIZE, get_active_dev_cmd_count(), g_selected_device_cmd };
    } else if (g_current_view == VIEW_DEVICE_BROWSER) {
        *g = (list_geom_t){ 2 + g_browse_dev, browser_list_y(), browser_page_size(), browser_item_count(), g_browse_sel };
    } else {
        return false;
    }
    return true;
}

static int list_max(const list_geom_t *g) {
    return g->count > g->rows ? (g->count - g->rows) * LIST_ROW_PITCH : 0;
}

static int clamp_scroll(const list_geom_t *g, int px) {
    int max = list_max(g);
    return px < 0 ? 0 : (px > max ? max : px);
}

/* A list shown for the first time opens on the page holding its selection */
static bool list_sync(list_geom_t *g) {
    if (!list_geometry(g)) return false;
    if (g->key != l.key) {
        l.key = g->key;
        l.tracking = l.dragging = l.caught = l.settling = false;
        l.scroll = clamp_scroll(g, g->sel / g->rows * g->rows * LIST_ROW_PITCH);
    } else if (!l.dragging && !l.settling) {
        l.scroll = clamp_scroll(g, l.scroll);
    }
    return true;
}

static bool list_can_scroll(list_geom_t *g) {
    return list_sync(g) && g_touch_enabled && !g_menu_open && !ui_help_active() && !g_link_panel_open &&
           !g_key_hint_pending && !ui_charging_page_active() && !g_is_switching_activity && !g_is_syncing_config && !g_is_ota_updating &&
           g->count > g->rows;
}

static void list_settle_to(int target, uint32_t ms_dur, uint64_t ms) {
    l.settling = true;
    l.settle_from = l.scroll;
    l.settle_to = target;
    l.settle_start_ms = ms;
    l.settle_ms = ms_dur;
}

void list_scroll_touch_down(int x, int y, uint64_t ms) {
    list_geom_t g;
    l.tracking = l.dragging = l.caught = false;
    if (!list_can_scroll(&g) || y < g.y || y >= g.y + g.rows * LIST_ROW_PITCH) return;
    l.tracking = true;
    l.caught = l.settling;
    l.settling = false;
    l.down_x = x;
    l.down_y = y;
    l.down_scroll = l.scroll;
    l.finger.count = 0;
    add_sample(&l.finger, y, ms);
}

bool list_scroll_touch_move(int x, int y, uint64_t ms) {
    list_geom_t g;
    if (!l.tracking) return false;
    if (!list_can_scroll(&g)) {
        list_scroll_cancel();
        return true;
    }
    add_sample(&l.finger, y, ms);
    int dy = y - l.down_y;
    if (!l.dragging) {
        if (abs(dy) <= touch_tap_slop() || abs(dy) < abs(x - l.down_x)) return false;
        l.dragging = true;
    }
    /* Past the first or last row the list moves a third as far as the finger */
    int px = l.down_scroll - dy, max = list_max(&g);
    if (px < 0) px = px / EDGE_RESISTANCE < -EDGE_MAX_PX ? -EDGE_MAX_PX : px / EDGE_RESISTANCE;
    else if (px > max) px = max + ((px - max) / EDGE_RESISTANCE > EDGE_MAX_PX ? EDGE_MAX_PX : (px - max) / EDGE_RESISTANCE);
    if (px == l.scroll) return false;
    l.scroll = px;
    return true;
}

/* A fling carries on and slows down, coming to rest with a row at the top */
bool list_scroll_touch_up(int x, int y, uint64_t ms) {
    list_geom_t g;
    (void)x;
    if (!l.tracking) return false;
    l.tracking = false;
    if (!list_sync(&g)) return false;
    if (!l.dragging) {
        if (!l.caught) return false;
        l.caught = false;
        int row = (l.scroll + LIST_ROW_PITCH / 2) / LIST_ROW_PITCH * LIST_ROW_PITCH;
        list_settle_to(clamp_scroll(&g, row), LIST_SETTLE_MS, ms);
        return true;
    }
    l.dragging = l.caught = false;
    add_sample(&l.finger, y, ms);

    int max = list_max(&g);
    if (l.scroll < 0 || l.scroll > max) {
        list_settle_to(clamp_scroll(&g, l.scroll), LIST_BOUNCE_MS, ms);
        return true;
    }
    float v = release_velocity(&l.finger);
    float rest = (float)l.scroll - v * LIST_FLING_TAU;
    int target = clamp_scroll(&g, (int)(rest / LIST_ROW_PITCH + (rest < 0 ? -0.5f : 0.5f)) * LIST_ROW_PITCH);
    /* Ease-out cubic starts at 3 * distance / duration: the list leaves at the finger's speed */
    float speed = fabsf(v);
    float dur = speed > 0.05f ? 3.0f * (float)abs(target - l.scroll) / speed : LIST_SETTLE_MS;
    list_settle_to(target, dur < LIST_SETTLE_MS ? LIST_SETTLE_MS : (dur > LIST_FLING_MAX_MS ? LIST_FLING_MAX_MS : (uint32_t)dur), ms);
    return true;
}

bool list_scroll_tick(uint64_t ms) {
    list_geom_t g;
    if (!l.settling) return false;
    if (!list_geometry(&g) || g.key != l.key) {
        list_scroll_cancel();
        return true;
    }
    uint64_t t = ms - l.settle_start_ms;
    if (t >= l.settle_ms) {
        l.scroll = l.settle_to;
        l.settling = false;
        return true;
    }
    float p = 1.0f - (float)t / (float)l.settle_ms;
    int px = l.settle_from + (int)((float)(l.settle_to - l.settle_from) * (1.0f - p * p * p));
    if (px == l.scroll) return false;
    l.scroll = px;
    return true;
}

void list_scroll_reveal(uint64_t ms) {
    list_geom_t g;
    if (!list_sync(&g) || g.count <= 0) return;
    int top = g.sel * LIST_ROW_PITCH, target = l.scroll;
    if (top < l.scroll) target = top;
    else if (top + LIST_ROW_PITCH > l.scroll + g.rows * LIST_ROW_PITCH) target = top - (g.rows - 1) * LIST_ROW_PITCH;
    target = clamp_scroll(&g, target);
    l.tracking = l.dragging = l.caught = false;
    if (target == l.scroll) return;
    /* A step to the next row slides; a wrap from the last row to the first jumps */
    if (abs(target - l.scroll) <= g.rows * LIST_ROW_PITCH) {
        list_settle_to(target, LIST_REVEAL_MS, ms);
    } else {
        l.settling = false;
        l.scroll = target;
    }
}

int list_scroll_offset(void) {
    list_geom_t g;
    list_sync(&g);
    return l.scroll;
}

void list_scroll_set_drag(int px) {
    list_geom_t g;
    l.key = -1;
    if (!list_sync(&g)) return;
    l.scroll -= px;
    l.dragging = px != 0; /* held where the finger left it, past the ends too */
}

void list_scroll_to(int px) {
    list_geom_t g;
    l.key = -1;
    if (list_sync(&g)) l.scroll = clamp_scroll(&g, px);
}

int list_row_at(int y) {
    list_geom_t g;
    if (!list_sync(&g) || y < g.y || y >= g.y + g.rows * LIST_ROW_PITCH) return -1;
    int idx = (y - g.y + l.scroll) / LIST_ROW_PITCH;
    return idx >= 0 && idx < g.count ? idx : -1;
}

bool list_scroll_active(void) {
    return l.dragging || l.settling;
}

bool list_scroll_settling(void) {
    return l.settling;
}

void list_scroll_cancel(void) {
    list_geom_t g;
    l.tracking = l.dragging = l.caught = l.settling = false;
    if (list_geometry(&g) && g.key == l.key) l.scroll = clamp_scroll(&g, l.scroll);
}

bool ui_scroll_active(void) {
    return home_scroll_active() || list_scroll_active();
}

bool ui_scroll_settling(void) {
    return home_scroll_settling() || list_scroll_settling();
}
