/*
 * test_remote_key_release.c - The release of a key held over RF must clear the held key and
 * send the release frame even while an overlay (activity switch, config sync, update) is up,
 * so the key repeat thread stops sending hold frames. RF frames go to a pipe and are decoded.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include "remote/remote_state.h"
#include "remote/remote_input.h"
#include "remote/remote_rf.h"
#include "rf_proto.h"

#define KEY_VOLUMEUP_CODE   115
#define KEY_VOLUMEDOWN_CODE 114
#define RF_VOLUMEUP         0x00E9

static int s_rx_fd = -1;
static int s_failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) printf("  [ok] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); s_failures++; } \
} while (0)

/* Read the button frames written so far; count them by state */
static void drain(int *press, int *hold, int *release) {
    uint8_t out[32];
    while (read(s_rx_fd, out, sizeof(out)) == (ssize_t)sizeof(out)) {
        if (out[5] != RF_MSG_BUTTON_PRESS) continue;
        uint16_t key = (uint16_t)((out[7] << 8) | out[8]);
        if (key != RF_VOLUMEUP) continue;
        if (out[9] == 0x01 && press) (*press)++;
        else if (out[9] == 0x02 && hold) (*hold)++;
        else if (out[9] == 0x00 && release) (*release)++;
    }
}

static void press_volume_up(void) {
    /* What the main loop does for a press of a key that maps to an RF code */
    rf_send_button_event(RF_VOLUMEUP, 1);
    pthread_mutex_lock(&g_repeat_mutex);
    g_held_rf_code = RF_VOLUMEUP;
    pthread_cond_signal(&g_repeat_cond);
    pthread_mutex_unlock(&g_repeat_mutex);
}

static uint16_t held_code(void) {
    pthread_mutex_lock(&g_repeat_mutex);
    uint16_t c = g_held_rf_code;
    pthread_mutex_unlock(&g_repeat_mutex);
    return c;
}

int main(void) {
    printf("[*] test_remote_key_release\n");

    int p[2];
    if (pipe(p) != 0) { perror("pipe"); return 1; }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    s_rx_fd = p[0];
    g_rf_fd = p[1];
    g_remote_slot = 1;
    g_current_view = VIEW_ACTIVITIES;

    pthread_t th;
    pthread_create(&th, NULL, key_repeat_thread, NULL);

    /* Hold Volume Up long enough for hold frames, then an activity switch overlay starts */
    int press = 0, hold = 0, release = 0;
    press_volume_up();
    usleep(800000);
    g_is_switching_activity = true;

    /* Another key's release does not touch the held key */
    CHECK(!remote_release_held_rf_key(KEY_VOLUMEDOWN_CODE), "release of another key is not taken");
    CHECK(held_code() == RF_VOLUMEUP, "Volume Up still held");

    /* The user lets go while the overlay is up */
    CHECK(remote_release_held_rf_key(KEY_VOLUMEUP_CODE), "release of the held key is taken during the overlay");
    CHECK(held_code() == 0, "held key cleared");
    drain(&press, &hold, &release);
    CHECK(press == 1, "one press frame");
    CHECK(hold > 0, "hold frames while the key was held");
    CHECK(release == 1, "release frame sent during the overlay");

    /* No more hold frames after the release, during and after the overlay */
    int hold_after = 0, release_after = 0;
    usleep(500000);
    g_is_switching_activity = false;
    usleep(300000);
    drain(NULL, &hold_after, &release_after);
    CHECK(hold_after == 0, "no hold frames after the release");

    /* A second release of the same key is not taken again */
    CHECK(!remote_release_held_rf_key(KEY_VOLUMEUP_CODE), "second release not taken");
    drain(NULL, NULL, &release_after);
    CHECK(release_after == 0, "no second release frame");

    g_running = false;
    pthread_mutex_lock(&g_repeat_mutex);
    pthread_cond_signal(&g_repeat_cond);
    pthread_mutex_unlock(&g_repeat_mutex);
    pthread_join(th, NULL);

    if (s_failures) {
        printf("[-] test_remote_key_release: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("[+] test_remote_key_release passed\n");
    return 0;
}
