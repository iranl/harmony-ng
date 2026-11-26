/*
 * test_remote_haptic.c - the Vibration setting (Settings, TOUCH).
 *
 *  1. Off (the default): a capacitive key writes nothing to the haptic driver.
 *  2. On: the capacitive keys (on the key device and on the touch strip below the display) write
 *     the driver's timed command for the set length (03 <ms, little endian>); a hard key, a tap
 *     on a menu row and a swipe write nothing.
 *  3. The setting's row steps through OFF, 20, 40 and 80 ms with one pulse of each new length;
 *     the length is saved as haptic=<ms> and read back, and a value it does not offer reads as off.
 *
 * Run on the mock hardware: /dev/drv2605 is $HARMONY_MOCK_DIR/haptic.log, appended to.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "remote/remote_state.h"
#include "remote/remote_config.h"
#include "remote/remote_input.h"
#include "remote/remote_ui.h"

static int s_fail;
static char s_log[512];

#define CHECK(cond, msg) do { if (!(cond)) { printf("[-] %s\n", msg); s_fail = 1; } else printf("[+] %s\n", msg); } while (0)

/* The bytes written to the driver so far */
static long log_bytes(uint8_t *buf, size_t max) {
    FILE *f = fopen(s_log, "rb");
    if (!f) return 0;
    long n = (long)fread(buf, 1, max, f);
    fclose(f);
    return n;
}

/* The last pulse written is 03 <ms> */
static bool last_pulse_is(const uint8_t *buf, long n, int ms) {
    return n >= 3 && buf[n - 3] == 0x03 && buf[n - 2] == (ms & 0xFF) && buf[n - 1] == ((ms >> 8) & 0xFF);
}

static void touch(int x0, int y0, int x, int y) {
    g_touch_down_x = x0;
    g_touch_down_y = y0;
    handle_screen_touch_tap(x, y);
}

int main(void) {
    const char *md = getenv("HARMONY_MOCK_DIR");
    if (!md) { printf("[-] HARMONY_MOCK_DIR not set\n"); return 1; }
    snprintf(s_log, sizeof(s_log), "%s/haptic.log", md);
    remove(s_log);

    g_fb_size = FB_SIZE;
    g_fb_mem = (uint16_t *)calloc(1, FB_SIZE);
    if (!g_fb_mem) return 1;
    g_screen_on = true;
    g_touch_enabled = true;
    g_cap_buttons_enabled = true;
    g_rf_fd = -1;
    g_current_view = VIEW_ACTIVITIES;

    uint8_t buf[64];
    long n;

    /* 1 */
    CHECK(g_haptic_ms == 0, "Vibration is off by default");
    handle_navigation_key(102, 1);
    CHECK(log_bytes(buf, sizeof(buf)) == 0, "off: a capacitive key writes nothing to the haptic driver");

    /* 2 */
    g_haptic_ms = 40;
    handle_navigation_key(102, 1);
    n = log_bytes(buf, sizeof(buf));
    CHECK(n == 3 && last_pulse_is(buf, n, 40), "on: the Devices key writes the timed command for 40 ms (03 28 00)");
    handle_navigation_key(102, 0);
    handle_navigation_key(364, 1);
    handle_navigation_key(364, 0);
    n = log_bytes(buf, sizeof(buf));
    CHECK(n == 6 && last_pulse_is(buf, n, 40), "on: the Activities key too, on its press only");
    touch(60, FB_HEIGHT + 10, 60, FB_HEIGHT + 10);
    n = log_bytes(buf, sizeof(buf));
    CHECK(n == 9 && last_pulse_is(buf, n, 40), "on: a capacitive key read on the touch strip below the display");
    handle_navigation_key(115, 1);   /* KEY_VOLUMEUP */
    handle_navigation_key(115, 0);
    CHECK(log_bytes(buf, sizeof(buf)) == 9, "on: a hard key writes nothing");
    g_current_view = VIEW_SETTINGS;
    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
    touch(120, 120, 120, 250);   /* a swipe down */
    CHECK(log_bytes(buf, sizeof(buf)) == 9, "on: a swipe writes nothing");
    g_current_view = VIEW_SETTINGS;
    g_settings_subview = SETTINGS_VIEW_CATEGORIES;
    touch(120, UI_CONTENT_Y + 20, 120, UI_CONTENT_Y + 20);   /* a settings category */
    CHECK(log_bytes(buf, sizeof(buf)) == 9, "on: a tap on a menu row writes nothing");

    /* 3 */
    g_haptic_ms = 0;
    g_current_view = VIEW_SETTINGS;
    g_settings_subview = SETTINGS_VIEW_TOUCH;
    CHECK(get_settings_sub_count() == 5, "the TOUCH page has 5 rows (Vibration last)");
    remove(s_log);
    const int steps[4] = { 20, 40, 80, 0 };
    bool steps_ok = true;
    long expect = 0;
    for (int i = 0; i < 4; i++) {
        execute_settings_action(4);
        if (steps[i]) expect += 3;
        n = log_bytes(buf, sizeof(buf));
        if (g_haptic_ms != steps[i] || n != expect || (steps[i] && !last_pulse_is(buf, n, steps[i]))) {
            printf("[-] step %d: %d ms set, %ld bytes written\n", i, g_haptic_ms, n);
            steps_ok = false;
        }
    }
    CHECK(steps_ok, "the Vibration row steps OFF -> 20 -> 40 -> 80 -> OFF, with one pulse of each new length");
    execute_settings_action(4);
    execute_settings_action(4);   /* 40 ms */
    g_haptic_ms = 0;
    load_settings_conf();
    CHECK(g_haptic_ms == 40, "haptic=40 is saved and read back");
    FILE *f = fopen(SETTINGS_CONFIG_FILE, "a");
    if (f) { fputs("haptic=1\n", f); fclose(f); }
    load_settings_conf();
    CHECK(g_haptic_ms == 0, "a length the setting does not offer (haptic=1) reads as off");

    if (s_fail) {
        printf("[-] test_remote_haptic: FAILED\n");
        return 1;
    }
    printf("[+] test_remote_haptic: all checks passed\n");
    return 0;
}
