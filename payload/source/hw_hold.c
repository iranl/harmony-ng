/*
 * A key held for a set time: press, a hold every HW_HOLD_BEAT_MS, release (hw_action.h).
 * Used by the MQTT send_command sequencer (hub_mqtt.c), the activity and device sequence steps
 * (orchestrator.c) and Test Live (http_api_activities.c), so a hold has the same timing from all
 * of them. The caller owns the hw_hold_t and drives it from its own loop: no thread, no lock.
 */
#define _GNU_SOURCE
#include "hw_action.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t hw_hold_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

uint32_t hw_hold_clamp_ms(long ms) {
    if (ms <= 0) return 0;
    return ms > HW_HOLD_MAX_MS ? HW_HOLD_MAX_MS : (uint32_t)ms;
}

int hw_hold_press(hw_hold_t *h, const char *target, int match_mode, const char *command,
                  uint32_t hold_ms, uint64_t now_ms) {
    if (!h || !target || !target[0] || !command || !command[0]) return -1;
    hw_hold_release(h);
    hold_ms = hw_hold_clamp_ms((long)hold_ms);
    int rc = hw_device_command_send_action(target, match_mode, command, HW_BTN_ACTION_PRESS);
    if (rc != 0) return rc;
    h->active = 1;
    h->match_mode = match_mode;
    snprintf(h->target, sizeof(h->target), "%s", target);
    snprintf(h->command, sizeof(h->command), "%s", command);
    h->release_ms = now_ms + hold_ms;
    h->next_beat_ms = now_ms + HW_HOLD_BEAT_MS;
    return 0;
}

void hw_hold_release(hw_hold_t *h) {
    if (!h || !h->active) return;
    h->active = 0;
    hw_device_command_send_action(h->target, h->match_mode, h->command, HW_BTN_ACTION_RELEASE);
}

int hw_hold_tick(hw_hold_t *h, uint64_t now_ms) {
    if (!h || !h->active) return 0;
    if (now_ms >= h->release_ms) {
        hw_hold_release(h);
        return 0;
    }
    if (now_ms >= h->next_beat_ms) {
        hw_device_command_send_action(h->target, h->match_mode, h->command, HW_BTN_ACTION_REPEAT);
        h->next_beat_ms = now_ms + HW_HOLD_BEAT_MS;
    }
    return 1;
}

uint64_t hw_hold_next_ms(const hw_hold_t *h) {
    if (!h || !h->active) return 0;
    return h->next_beat_ms < h->release_ms ? h->next_beat_ms : h->release_ms;
}

int hw_hold_run(const char *target, int match_mode, const char *command, uint32_t hold_ms) {
    hw_hold_t h;
    memset(&h, 0, sizeof(h));
    int rc = hw_hold_press(&h, target, match_mode, command, hold_ms, hw_hold_now_ms());
    if (rc != 0) return rc;
    for (;;) {
        uint64_t now = hw_hold_now_ms();
        if (!hw_hold_tick(&h, now)) break;
        uint64_t next = hw_hold_next_ms(&h);
        now = hw_hold_now_ms();
        if (next > now) usleep((useconds_t)(next - now) * 1000);
    }
    return 0;
}
