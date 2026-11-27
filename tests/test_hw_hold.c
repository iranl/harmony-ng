/*
 * The timed hold of hw_hold.c: press, a hold every HW_HOLD_BEAT_MS, release when the time is up
 * (the MQTT send_command sequencer, sequence steps and Test Live all use it). The send is a stub
 * that records each action; the clock is the test's (hw_hold_tick() takes the time).
 */
#include "../payload/source/hw_action.h"

#include <stdio.h>
#include <string.h>

static struct { char target[128]; char command[64]; int match; int action; } s_sent[64];
static int s_count = 0;
static int s_press_rc = 0;

int hw_device_command_send_action(const char *target, int match_mode, const char *command, int action) {
    if (s_count < 64) {
        snprintf(s_sent[s_count].target, sizeof(s_sent[s_count].target), "%s", target);
        snprintf(s_sent[s_count].command, sizeof(s_sent[s_count].command), "%s", command);
        s_sent[s_count].match = match_mode;
        s_sent[s_count].action = action;
        s_count++;
    }
    return action == HW_BTN_ACTION_PRESS ? s_press_rc : 0;
}

static int g_fails = 0;

static void check(int cond, const char *what) {
    printf("%s %s\n", cond ? "[+]" : "[-]", what);
    if (!cond) g_fails++;
}

static int count_of(int action) {
    int n = 0;
    for (int i = 0; i < s_count; i++) n += s_sent[i].action == action;
    return n;
}

/* drives h from t0 every 10 ms until it is released; returns the time of the release */
static uint64_t run_until_release(hw_hold_t *h, uint64_t t0, uint64_t limit) {
    for (uint64_t t = t0; t <= t0 + limit; t += 10) {
        if (!hw_hold_tick(h, t)) return t;
    }
    return 0;
}

int main(void) {
    hw_hold_t h;
    memset(&h, 0, sizeof(h));

    /* 1. a 1 s hold: press, a hold at 300, 600 and 900 ms, release at 1000 ms */
    s_count = 0;
    check(hw_hold_press(&h, "7", 1, "PowerToggle", 1000, 0) == 0, "a press of a held key goes out");
    check(h.active && s_count == 1 && s_sent[0].action == HW_BTN_ACTION_PRESS, "the press comes first");
    check(hw_hold_next_ms(&h) == HW_HOLD_BEAT_MS, "the first hold is due one beat after the press");
    uint64_t released = run_until_release(&h, 10, 5000);
    check(released == 1000, "the key is released when its hold is up");
    check(count_of(HW_BTN_ACTION_REPEAT) == 3, "a hold every 300 ms while the key is down (3 in 1 s)");
    check(s_count == 5 && s_sent[4].action == HW_BTN_ACTION_RELEASE, "the release comes last, once");
    check(strcmp(s_sent[4].target, "7") == 0 && strcmp(s_sent[4].command, "PowerToggle") == 0 &&
          s_sent[4].match == 1, "the release is for the key that was pressed");
    check(!h.active && hw_hold_next_ms(&h) == 0, "nothing is held after the release");
    check(hw_hold_tick(&h, 2000) == 0 && s_count == 5, "a tick after the release sends nothing");

    /* 2. a hold shorter than a beat: press, release, no hold */
    s_count = 0;
    hw_hold_press(&h, "7", 1, "Ok", 200, 0);
    check(hw_hold_next_ms(&h) == 200, "a hold shorter than a beat is next due at its release");
    check(run_until_release(&h, 10, 5000) == 200 && count_of(HW_BTN_ACTION_REPEAT) == 0,
          "a 200 ms hold sends no hold between its press and its release");

    /* 3. the longest hold is HW_HOLD_MAX_MS */
    s_count = 0;
    hw_hold_press(&h, "7", 1, "Ok", 60000, 0);
    check(h.release_ms == HW_HOLD_MAX_MS, "a hold is capped at HW_HOLD_MAX_MS");
    check(hw_hold_clamp_ms(-5) == 0 && hw_hold_clamp_ms(2500) == 2500 && hw_hold_clamp_ms(99999) == HW_HOLD_MAX_MS,
          "hw_hold_clamp_ms keeps 0 to HW_HOLD_MAX_MS");

    /* 4. a new press releases the key still held first; a cancel releases it at once */
    s_count = 0;
    hw_hold_press(&h, "8", 1, "Menu", 2000, 100);
    check(s_count == 2 && s_sent[0].action == HW_BTN_ACTION_RELEASE && strcmp(s_sent[0].command, "Ok") == 0 &&
          s_sent[1].action == HW_BTN_ACTION_PRESS && strcmp(s_sent[1].command, "Menu") == 0,
          "a new press releases the key held before, then presses");
    hw_hold_release(&h);
    check(s_count == 3 && s_sent[2].action == HW_BTN_ACTION_RELEASE && !h.active, "a cancel releases the key at once");
    hw_hold_release(&h);
    check(s_count == 3, "a second cancel sends nothing");

    /* 5. a press that fails holds nothing */
    s_count = 0;
    s_press_rc = -5;
    check(hw_hold_press(&h, "9", 0, "Play", 1000, 0) == -5, "a failed press gives its error");
    check(!h.active && hw_hold_tick(&h, 500) == 0 && s_count == 1, "a failed press holds nothing and sends no release");
    s_press_rc = 0;

    /* 6. hw_hold_run() holds in place, on the real clock */
    s_count = 0;
    uint64_t t0 = hw_hold_now_ms();
    check(hw_hold_run("7", 1, "PowerToggle", 700) == 0, "hw_hold_run presses, holds and releases");
    uint64_t took = hw_hold_now_ms() - t0;
    check(took >= 700 && took < 1200, "hw_hold_run returns after the hold");
    check(s_count == 4 && s_sent[0].action == HW_BTN_ACTION_PRESS && count_of(HW_BTN_ACTION_REPEAT) == 2 &&
          s_sent[3].action == HW_BTN_ACTION_RELEASE, "hw_hold_run sends press, 2 holds in 700 ms, release");

    if (g_fails) {
        printf("%d hold check(s) failed\n", g_fails);
        return 1;
    }
    printf("all timed hold checks passed\n");
    return 0;
}
