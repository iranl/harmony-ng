/*
 * test_rf_companion_timeout.c - A held companion (stock protocol) key whose key-up packet
 * is lost must be released by check_companion_timeouts() after 30 s, and the hold repeats
 * must stop. Runs the real rf_keymap.c and its dispatch worker with a test clock and
 * counts the actions of the /api/button-press bodies it posts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "rf_keymap.h"

volatile bool g_running = true;
static volatile uint64_t s_now_ms = 0;
uint64_t get_monotonic_ms(void) { return s_now_ms; }
void mark_remote_slot_active(uint8_t slot) { (void)slot; }
void rf_note_slot_family(uint8_t slot, remote_model_t family) { (void)slot; (void)family; }

static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_actions[256][16];
static int s_action_count = 0;

int http_post_local(int port, const char *path, const char *body) {
    (void)port;
    if (strcmp(path, "/api/button-press") != 0 || !body) return 200;
    const char *a = strstr(body, "\"action\":\"");
    if (!a) return 200;
    a += strlen("\"action\":\"");
    pthread_mutex_lock(&s_log_lock);
    if (s_action_count < 256) {
        size_t n = strcspn(a, "\"");
        if (n >= sizeof(s_actions[0])) n = sizeof(s_actions[0]) - 1;
        memcpy(s_actions[s_action_count], a, n);
        s_actions[s_action_count][n] = '\0';
        s_action_count++;
    }
    pthread_mutex_unlock(&s_log_lock);
    return 200;
}

static int s_failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  [ok] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); s_failures++; } \
} while (0)

static int action_count(void) {
    pthread_mutex_lock(&s_log_lock);
    int n = s_action_count;
    pthread_mutex_unlock(&s_log_lock);
    return n;
}

/* Move the clock and give the dispatch worker (which polls the timeouts every 20 ms and
 * sends what is queued) time to post until `want` events were seen, or 1 s at most. */
static void advance_to(uint64_t t, int want) {
    s_now_ms = t;
    for (int i = 0; i < 100 && action_count() < want; i++) usleep(10000);
    usleep(60000);
}

int main(void) {
    printf("[*] test_rf_companion_timeout\n");

    const uint64_t t0 = 1000;
    s_now_ms = t0;
    pthread_t th;
    pthread_create(&th, NULL, dispatch_worker, NULL);

    /* Key-down of Volume Up (consumer usage 0x00E9) from companion slot 1 */
    const uint8_t key_down[5] = {0x00, 0x01, 0x03, 0xE9, 0x00};
    handle_companion_packet(key_down, sizeof(key_down));
    advance_to(t0, 1);              /* press */

    /* The key-up never arrives */
    advance_to(t0 + 400, 2);        /* first hold after 350 ms */
    advance_to(t0 + 540, 3);        /* next hold 140 ms later */
    advance_to(t0 + 29000, 4);      /* still inside the 30 s: a hold, no release */
    advance_to(t0 + 30020, 5);      /* past the 30 s safety timeout: release */
    advance_to(t0 + 31000, 5);      /* after the release: nothing more */
    advance_to(t0 + 60000, 5);

    g_running = false;
    pthread_join(th, NULL);

    int press = 0, hold = 0, release = 0, hold_after_release = 0;
    for (int i = 0; i < s_action_count; i++) {
        if (strcmp(s_actions[i], "press") == 0) press++;
        else if (strcmp(s_actions[i], "hold") == 0) { hold++; if (release) hold_after_release++; }
        else if (strcmp(s_actions[i], "release") == 0) release++;
    }
    printf("  events: press=%d hold=%d release=%d\n", press, hold, release);

    CHECK(press == 1, "one press sent");
    CHECK(hold == 3, "hold repeats sent while the key is held");
    CHECK(release == 1, "the key is released once after the 30 s safety timeout");
    CHECK(s_action_count > 0 && strcmp(s_actions[s_action_count - 1], "release") == 0,
          "the release is the last event");
    CHECK(hold_after_release == 0, "no hold repeats after the release");

    if (s_failures) {
        printf("[-] test_rf_companion_timeout: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("[+] test_rf_companion_timeout passed\n");
    return 0;
}
