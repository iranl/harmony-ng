/*
 * test_rf_companion_race.c - A companion (stock protocol) key press is stored by the RF
 * receive thread in several steps (key, name, start time), while the dispatch worker reads
 * the same state in check_companion_timeouts(). A reader that runs between the steps sees a
 * pressed key with the start time of the previous press and sends a "hold" for a key that
 * was only just pressed.
 *
 * The window is only a few instructions wide, so the test widens it: rf_keymap.c is built
 * with -fno-builtin-strncpy and the link wraps strncpy (-Wl,--wrap=strncpy). The copy of the
 * key name into the companion state (the only 31-byte copy in rf_keymap.c) sleeps 20 ms,
 * which falls after the key is stored and before the start time is. A second thread runs
 * check_companion_timeouts() in a tight loop. The test clock moves only while no key is
 * down, so every hold that reaches /api/button-press comes from the race.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "rf_keymap.h"

volatile bool g_running = true;
static volatile uint64_t s_now_ms = 100000;
uint64_t get_monotonic_ms(void) { return s_now_ms; }
void mark_remote_slot_active(uint8_t slot) { (void)slot; }
bool is_companion_slot(uint8_t slot) { (void)slot; return false; }
void rf_note_slot_family(uint8_t slot, remote_model_t family) { (void)slot; (void)family; }

char *__real_strncpy(char *dst, const char *src, size_t n);
char *__wrap_strncpy(char *dst, const char *src, size_t n) {
    char *r = __real_strncpy(dst, src, n);
    if (n == 31) usleep(20000); /* the companion state's current_name copy */
    return r;
}

static volatile int s_press, s_hold, s_release;

int http_post_local(int port, const char *path, const char *body) {
    (void)port;
    if (!path || strcmp(path, "/api/button-press") != 0 || !body) return 200;
    if (strstr(body, "\"action\":\"press\"")) __sync_fetch_and_add(&s_press, 1);
    else if (strstr(body, "\"action\":\"hold\"")) __sync_fetch_and_add(&s_hold, 1);
    else if (strstr(body, "\"action\":\"release\"")) __sync_fetch_and_add(&s_release, 1);
    return 200;
}

static void *checker(void *arg) {
    (void)arg;
    while (g_running) check_companion_timeouts();
    return NULL;
}

int main(void) {
    printf("[*] test_rf_companion_race\n");
    enum { PRESSES = 20 };

    pthread_t worker, check;
    pthread_create(&worker, NULL, dispatch_worker, NULL);
    pthread_create(&check, NULL, checker, NULL);

    const uint8_t key_up[5] = {0x00, 0x01, 0x03, 0x00, 0x00};
    for (int i = 0; i < PRESSES; i++) {
        s_now_ms += 1000; /* the previous press is 1 s old: its start time looks like a hold */
        uint8_t key_down[5] = {0x00, 0x01, 0x03, (uint8_t)(0xE9 + (i & 1)), 0x00};
        handle_companion_packet(key_down, sizeof(key_down));
        /* let the worker post the press (and any hold) before the release purges the queue */
        for (int w = 0; w < 100 && s_press <= i; w++) usleep(1000);
        usleep(30000);
        handle_companion_packet(key_up, sizeof(key_up));
    }
    for (int w = 0; w < 100 && s_release < PRESSES; w++) usleep(10000);

    g_running = false;
    pthread_join(check, NULL);
    pthread_join(worker, NULL);

    printf("  events: press=%d hold=%d release=%d (presses %d)\n", s_press, s_hold, s_release, PRESSES);
    int failures = 0;
    if (s_press != PRESSES) { printf("  [FAIL] one press sent per key-down\n"); failures++; }
    else printf("  [ok] one press sent per key-down\n");
    if (s_hold != 0) { printf("  [FAIL] no hold for a key that was only just pressed\n"); failures++; }
    else printf("  [ok] no hold for a key that was only just pressed\n");
    if (s_release != PRESSES) { printf("  [FAIL] one release sent per key-up\n"); failures++; }
    else printf("  [ok] one release sent per key-up\n");

    if (failures) {
        printf("[-] test_rf_companion_race: %d check(s) failed\n", failures);
        return 1;
    }
    printf("[+] test_rf_companion_race passed\n");
    return 0;
}
