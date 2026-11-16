/*
 * test_remote_key_counter.c - Every key frame the remote sends (RF_MSG_BUTTON_PRESS) carries a
 * key event counter in payload[19]: 1..255, then 1 again, one step per frame (press, hold and
 * release alike), in the order the frames are written, also when the main loop and the key
 * repeat thread send at the same time. The Hub counts the gaps to measure lost key frames.
 * RF frames go to a pipe and are decoded.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include "remote/remote_state.h"
#include "remote/remote_rf.h"
#include "rf_proto.h"

#define RF_VOLUMEUP 0x00E9
#define RF_RIGHT    0x004F
#define COUNTER_AT  (7 + 19) /* payload[19] in the 32-byte report: the payload starts at byte 7 */

/* The number after c, as the protocol defines it (docs/HARMONY_RF_PROTOCOL.md) */
static uint8_t next_counter(uint8_t c) { return (uint8_t)(c >= 255 ? 1 : c + 1); }

static int s_rx_fd = -1;
static FILE *s_out; /* the test's own output; remote_rf.c logs every frame to stdout */
static int s_failures = 0;

#define CHECK(cond, ...) do { \
    if (cond) { fprintf(s_out, "  [ok] " __VA_ARGS__); } \
    else { fprintf(s_out, "  [FAIL] line %d: ", __LINE__); fprintf(s_out, __VA_ARGS__); s_failures++; } \
    fprintf(s_out, "\n"); \
} while (0)

/* Read the key frames written so far, in order, into frames[][32]; returns how many */
static int drain(uint8_t frames[][32], int max) {
    uint8_t out[32];
    int n = 0;
    while (read(s_rx_fd, out, sizeof(out)) == (ssize_t)sizeof(out)) {
        if (out[5] != RF_MSG_BUTTON_PRESS) continue;
        if (n < max) memcpy(frames[n], out, 32);
        n++;
    }
    return n;
}

static void *hold_sender(void *arg) {
    int count = *(int *)arg;
    for (int i = 0; i < count; i++) rf_send_button_event(RF_VOLUMEUP, 2);
    return NULL;
}

static uint8_t s_frames[1600][32];

int main(void) {
    s_out = fdopen(dup(1), "w");
    int null_fd = open("/dev/null", O_WRONLY);
    if (!s_out || null_fd < 0) return 1;
    fflush(stdout);
    dup2(null_fd, 1);
    close(null_fd);
    fprintf(s_out, "[*] test_remote_key_counter\n");

    int p[2];
    if (pipe(p) != 0) { perror("pipe"); return 1; }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    s_rx_fd = p[0];
    g_rf_fd = p[1];
    g_remote_slot = 1;
    g_current_view = VIEW_ACTIVITIES;

    /* A press, holds and a release: consecutive numbers from 1 */
    rf_send_button_event(RF_VOLUMEUP, 1);
    rf_send_button_event(RF_VOLUMEUP, 2);
    rf_send_button_event(RF_VOLUMEUP, 2);
    rf_send_button_event(RF_VOLUMEUP, 0);
    int n = drain(s_frames, 1600);
    CHECK(n == 4, "four key frames written (%d)", n);
    CHECK(n == 4 && s_frames[0][COUNTER_AT] == 1 && s_frames[1][COUNTER_AT] == 2 && s_frames[2][COUNTER_AT] == 3 &&
          s_frames[3][COUNTER_AT] == 4, "press, hold, hold, release carry counters 1,2,3,4 in payload[19] (%u,%u,%u,%u)",
          s_frames[0][COUNTER_AT], s_frames[1][COUNTER_AT], s_frames[2][COUNTER_AT], s_frames[3][COUNTER_AT]);
    CHECK(n == 4 && s_frames[0][9] == 0x01 && s_frames[1][9] == 0x02 && s_frames[3][9] == 0x00 &&
          strcmp((const char *)&s_frames[0][10], "VolUp") == 0, "key code, state and short name are where they were");
    int ctx_zero = 1;
    for (int i = 20; i <= 24; i++) if (s_frames[0][7 + i] != 0) ctx_zero = 0;
    CHECK(n == 4 && ctx_zero, "device context bytes 20..24 stay zero outside the device browser");

    /* An event that is not sent takes no number */
    rf_send_button_event(0, 1);
    rf_send_button_event(RF_VOLUMEUP, 7);
    rf_send_button_event(RF_RIGHT, 1);
    n = drain(s_frames, 1600);
    CHECK(n == 1 && s_frames[0][COUNTER_AT] == 5, "events that are not sent take no number: the next frame is #5 (%d frame(s), #%u)",
          n, n ? s_frames[0][COUNTER_AT] : 0);

    /* The device context of the device browser sits right after the counter */
    g_rf_device_count = 1;
    snprintf(g_rf_devices[0].id, sizeof(g_rf_devices[0].id), "42");
    g_browse_dev = 0;
    g_current_view = VIEW_DEVICE_BROWSER;
    rf_send_button_event(RF_RIGHT, 0);
    g_current_view = VIEW_ACTIVITIES;
    n = drain(s_frames, 1600);
    CHECK(n == 1 && s_frames[0][COUNTER_AT] == 6 && strcmp((const char *)&s_frames[0][7 + 20], "42") == 0,
          "with a device context: counter in payload[19], context from payload[20]");

    /* 255 wraps to 1, never 0 */
    for (int i = 0; i < 300; i++) rf_send_button_event(RF_VOLUMEUP, i % 3 == 0 ? 1 : (i % 3 == 1 ? 2 : 0));
    n = drain(s_frames, 1600);
    int consecutive = (n == 300);
    int saw_wrap = 0, saw_zero = 0;
    uint8_t prev = 6;
    for (int i = 0; i < n && i < 1600; i++) {
        uint8_t c = s_frames[i][COUNTER_AT];
        if (c == 0) saw_zero = 1;
        if (c != next_counter(prev)) consecutive = 0;
        if (prev == 255 && c == 1) saw_wrap = 1;
        prev = c;
    }
    CHECK(consecutive, "300 more key frames carry consecutive counters (%d frames, last #%u)", n, prev);
    CHECK(saw_wrap && !saw_zero, "the counter after 255 is 1, and 0 is never sent");

    /* The main loop (press, release) and the repeat thread (hold) send at the same time: the
     * numbers follow the order in which the frames are written */
    int per_thread = 600;
    pthread_t th;
    pthread_create(&th, NULL, hold_sender, &per_thread);
    for (int i = 0; i < per_thread; i++) rf_send_button_event(RF_RIGHT, i % 2);
    pthread_join(th, NULL);
    n = drain(s_frames, 1600);
    consecutive = (n == 2 * per_thread);
    int from_main = 0, from_repeat = 0;
    for (int i = 0; i < n && i < 1600; i++) {
        uint8_t c = s_frames[i][COUNTER_AT];
        if (c != next_counter(prev)) consecutive = 0;
        if (s_frames[i][9] == 0x02) from_repeat++; else from_main++;
        prev = c;
    }
    CHECK(from_main == per_thread && from_repeat == per_thread, "both senders' frames were written (%d + %d)", from_main, from_repeat);
    CHECK(consecutive, "two senders at once: counters consecutive in the order written (%d frames)", n);

    if (s_failures) {
        fprintf(s_out, "[-] test_remote_key_counter: %d check(s) failed\n", s_failures);
        return 1;
    }
    fprintf(s_out, "[+] test_remote_key_counter passed\n");
    return 0;
}
