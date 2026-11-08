/* Help: repairs the running activity. The Hub re-sends the activity's commands
 * except power toggles, then the remote asks about each device with only a power toggle; on
 * "No" the Hub sends the toggle. Help covers the content area until it finishes. */
#include "remote_ui.h"
#include "remote_fb.h"
#include "remote_rf.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define HELP_MAX_QUESTIONS  16
#define HELP_REQUEST_TRIES  3
#define HELP_RETRY_MS       2000    /* resend the request until the Hub says it started */
#define HELP_RESEND_MS      130000  /* the Hub waits up to 120 s for the commands to go out */
#define HELP_NEXT_MS        10000   /* wait for the next question */
#define HELP_DONE_MS        3000    /* summary shown before returning to the activity screen */

#define HELP_BTN_Y (FB_HEIGHT - 76)
#define HELP_BTN_H 48
#define HELP_BTN_W ((FB_WIDTH - 24) / 2)

typedef enum { HELP_OFF, HELP_REQUESTING, HELP_RESENDING, HELP_QUESTION, HELP_DONE } help_state_t;

static struct {
    help_state_t state;
    char act_id[32];
    uint64_t since_ms;  /* entered the current state, or sent the last request */
    int tries;
    int count, received;
    struct { char id[9]; char name[RF_NAME_SIZE]; } q[HELP_MAX_QUESTIONS];
    int cur, sel;       /* question shown; 0 = Yes, 1 = No */
    int switched_on;
    remote_str_id_t result;
    bool failed;
} s_help;

/* ui_help_on_status runs on the RF thread, everything else on the UI thread */
static pthread_mutex_t s_help_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_state(help_state_t st, uint64_t now) {
    s_help.state = st;
    s_help.since_ms = now;
    g_screen_dirty = true;
}

static void finish(remote_str_id_t result, bool failed, uint64_t now) {
    s_help.result = result;
    s_help.failed = failed;
    set_state(HELP_DONE, now);
}

bool ui_help_active(void) {
    pthread_mutex_lock(&s_help_lock);
    bool active = s_help.state != HELP_OFF;
    pthread_mutex_unlock(&s_help_lock);
    return active;
}

void ui_help_start(void) {
    if (strcmp(g_active_activity_id, "-1") == 0 || !g_active_activity_id[0]) return;
    pthread_mutex_lock(&s_help_lock);
    memset(&s_help, 0, sizeof(s_help));
    snprintf(s_help.act_id, sizeof(s_help.act_id), "%s", g_active_activity_id);
    s_help.tries = 1;
    set_state(HELP_REQUESTING, get_monotonic_ms());
    rf_send_help_request(s_help.act_id);
    pthread_mutex_unlock(&s_help_lock);
}

static void close_locked(void) {
    if (s_help.state == HELP_OFF) return;
    s_help.state = HELP_OFF;
    g_current_view = VIEW_DEVICES;
    g_screen_dirty = true;
}

/* Closes Help and shows the activity screen */
void ui_help_close(void) {
    pthread_mutex_lock(&s_help_lock);
    close_locked();
    pthread_mutex_unlock(&s_help_lock);
}

static void show_question(int idx, uint64_t now) {
    s_help.cur = idx;
    s_help.sel = 0;
    set_state(HELP_QUESTION, now);
}

/* RF_MSG_HELP_STATUS from the Hub, on the RF thread */
void ui_help_on_status(const rf_frame_t *f) {
    uint64_t now = get_monotonic_ms();
    const uint8_t *p = f->payload;
    pthread_mutex_lock(&s_help_lock);
    if (s_help.state == HELP_OFF || s_help.state == HELP_DONE) {
        pthread_mutex_unlock(&s_help_lock);
        return;
    }
    switch (p[0]) {
        case RF_HELP_STARTED:
            if (s_help.state == HELP_REQUESTING) set_state(HELP_RESENDING, now);
            break;
        case RF_HELP_BUSY:         finish(STR_HELP_BUSY, true, now); break;
        case RF_HELP_FAILED:       finish(STR_HELP_FAILED, true, now); break;
        case RF_HELP_NO_QUESTIONS: finish(STR_HELP_ALL_ON, false, now); break;
        case RF_HELP_QUESTION: {
            int idx = p[1];
            if (idx >= HELP_MAX_QUESTIONS || p[2] == 0) break;
            s_help.count = p[2] < HELP_MAX_QUESTIONS ? p[2] : HELP_MAX_QUESTIONS;
            memcpy(s_help.q[idx].id, &p[3], 8);
            s_help.q[idx].id[8] = '\0';
            char name[15] = {0};
            memcpy(name, &p[11], 14);
            snprintf(s_help.q[idx].name, sizeof(s_help.q[idx].name), "%s", name);
            /* The pushed device list has the full name */
            for (int i = 0; i < g_rf_device_count; i++) {
                if (strcmp(g_rf_devices[i].id, s_help.q[idx].id) == 0) {
                    snprintf(s_help.q[idx].name, sizeof(s_help.q[idx].name), "%s", g_rf_devices[i].name);
                }
            }
            if (idx + 1 > s_help.received) s_help.received = idx + 1;
            if (s_help.state != HELP_QUESTION && idx == 0) show_question(0, now);
            g_screen_dirty = true;
            break;
        }
    }
    pthread_mutex_unlock(&s_help_lock);
}

/* Main loop: request retries and timeouts */
void ui_help_tick(uint64_t now) {
    static uint64_t last_frame_ms;
    pthread_mutex_lock(&s_help_lock);
    uint64_t age = now - s_help.since_ms;
    if ((s_help.state == HELP_REQUESTING || s_help.state == HELP_RESENDING) && now - last_frame_ms >= 250) {
        last_frame_ms = now;
        g_screen_dirty = true; /* animate the bar */
    }
    switch (s_help.state) {
        case HELP_REQUESTING:
            if (age < HELP_RETRY_MS) break;
            if (s_help.tries >= HELP_REQUEST_TRIES) {
                finish(STR_LINK_HUB_SILENT, true, now);
            } else {
                s_help.tries++;
                s_help.since_ms = now;
                rf_send_help_request(s_help.act_id);
            }
            break;
        case HELP_RESENDING:
            if (age >= HELP_RESEND_MS) finish(STR_LINK_HUB_SILENT, true, now);
            break;
        case HELP_QUESTION:
            if (s_help.cur >= s_help.received && age >= HELP_NEXT_MS) finish(STR_LINK_HUB_SILENT, true, now);
            break;
        case HELP_DONE:
            if (age >= HELP_DONE_MS) close_locked();
            break;
        default:
            break;
    }
    pthread_mutex_unlock(&s_help_lock);
}

static void answer(bool on) {
    if (s_help.state != HELP_QUESTION || s_help.cur >= s_help.received) return;
    uint64_t now = get_monotonic_ms();
    rf_send_help_answer(s_help.q[s_help.cur].id, on);
    if (!on) s_help.switched_on++;
    if (s_help.cur + 1 >= s_help.count) finish(s_help.switched_on ? STR_HELP_SWITCHED_ON : STR_HELP_ALL_ON, false, now);
    else show_question(s_help.cur + 1, now);
}

static void draw_line(int y, const char *text, uint16_t color, const font_aa_t *font) {
    char fit[64];
    fb_draw_text_aa(12, y, fb_fit_text_aa(fit, sizeof(fit), text, font, FB_WIDTH - 24), color, font);
}

static void draw_button(int x, const char *label, bool sel) {
    fb_fill_rect(x, HELP_BTN_Y, HELP_BTN_W, HELP_BTN_H, sel ? RGB_CARD_SEL : RGB_CARD);
    if (sel) fb_draw_rect(x, HELP_BTN_Y, HELP_BTN_W, HELP_BTN_H, RGB_TEAL);
    int tw = fb_text_width_aa(label, &g_font_title);
    fb_draw_text_aa(x + (HELP_BTN_W - tw) / 2, HELP_BTN_Y + 14, label, sel ? RGB_WHITE : RGB_TEXT_MUTED, &g_font_title);
}

void render_help(void) {
    pthread_mutex_lock(&s_help_lock);
    int y = UI_CONTENT_Y;
    fb_fill_rect(0, y - 3, FB_WIDTH, FB_HEIGHT - y + 3, RGB_BG);
    fb_draw_text_aa(12, y + 8, tr(STR_HELP), RGB_WHITE, &g_font_title);
    const char *act = s_help.act_id;
    for (int i = 0; i < g_rf_activity_count; i++) {
        if (strcmp(g_rf_activities[i].id, s_help.act_id) == 0) act = g_rf_activities[i].name;
    }
    draw_line(y + 34, act, RGB_TEXT_MUTED, &g_font_main);
    fb_fill_rect(12, y + 60, FB_WIDTH - 24, 1, RGB_DIVIDER);

    char buf[64];
    switch (s_help.state) {
        case HELP_REQUESTING:
        case HELP_RESENDING: {
            draw_line(y + 84, tr(STR_HELP_RESENDING), RGB_WHITE, &g_font_main);
            int bar_w = FB_WIDTH - 24, pulse = 40;
            int pos = (int)((get_monotonic_ms() / 15) % (uint64_t)(bar_w - pulse));
            fb_draw_rect(12, y + 114, bar_w, 8, RGB_CARD_BORDER);
            fb_fill_rect(12 + pos, y + 114, pulse, 8, RGB_CYAN);
            draw_line(FB_HEIGHT - 26, tr(STR_HELP_BACK_CLOSES), RGB_TEXT_MUTED, &g_font_main);
            break;
        }
        case HELP_QUESTION:
            snprintf(buf, sizeof(buf), tr(STR_HELP_DEVICE_N_OF), s_help.cur + 1, s_help.count);
            draw_line(y + 74, buf, RGB_TEXT_MUTED, &g_font_main);
            if (s_help.cur < s_help.received) {
                draw_line(y + 104, s_help.q[s_help.cur].name, RGB_WHITE, &g_font_title);
                draw_line(y + 134, tr(STR_HELP_IS_IT_ON), RGB_CYAN, &g_font_title);
                draw_button(8, tr(STR_YES), s_help.sel == 0);
                draw_button(FB_WIDTH - 8 - HELP_BTN_W, tr(STR_NO), s_help.sel == 1);
            }
            break;
        case HELP_DONE:
            snprintf(buf, sizeof(buf), tr(s_help.result), s_help.switched_on);
            draw_line(y + 84, buf, s_help.failed ? RGB_RED : RGB_GREEN, &g_font_title);
            break;
        default:
            break;
    }
    pthread_mutex_unlock(&s_help_lock);
}

/* Keys while Help shows; returns false for keys Help leaves alone, such as volume */
bool ui_help_handle_key(int code) {
    bool handled = true;
    pthread_mutex_lock(&s_help_lock);
    switch (code) {
        case 105: case 106: case 103: case 108: case 78: case 74:  /* D-pad, rocker */
            if (s_help.state != HELP_QUESTION) handled = false;
            else s_help.sel = !s_help.sel;
            break;
        case 28: case 148:  /* OK, KEY_PROG1 */
            if (s_help.state == HELP_QUESTION) answer(s_help.sel == 0);
            else if (s_help.state == HELP_DONE) close_locked();
            break;
        case 158: case 202: case 174:  /* Back, KEY_PROG3, Exit */
            close_locked();
            break;
        default:
            handled = false;
    }
    pthread_mutex_unlock(&s_help_lock);
    if (!handled) return false;
    g_screen_dirty = true;
    render_screen();
    return true;
}

void ui_help_handle_tap(int x, int y) {
    pthread_mutex_lock(&s_help_lock);
    if (s_help.state == HELP_DONE) {
        close_locked();
    } else if (s_help.state == HELP_QUESTION && y >= HELP_BTN_Y && y < HELP_BTN_Y + HELP_BTN_H) {
        if (x >= 8 && x < 8 + HELP_BTN_W) answer(true);
        else if (x >= FB_WIDTH - 8 - HELP_BTN_W && x < FB_WIDTH - 8) answer(false);
    }
    pthread_mutex_unlock(&s_help_lock);
    render_screen();
}
