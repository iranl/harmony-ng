/*
 * test_rf_key_queue.c - hub_rf's key queue and its dispatch worker (the real rf_keymap.c).
 * There is no key channel socket here, so every key ends in http_post_local(), which this
 * test replaces and where it notes the button, the action and the time.
 *
 *   - A quick tap: the press and the release are both queued before the worker takes the
 *     press. The release used to remove every queued event of its button, the press included,
 *     so the key did nothing. It removes only the queued hold repeats now.
 *   - The worker sends a queued key at once. It slept a flat 20 ms whenever the queue was
 *     empty, so a key waited up to 20 ms.
 *   - The worker sleeps while idle: next to no CPU time in 300 ms without keys (a wait that
 *     returns at once would show here as a busy loop).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rf_keymap.h"

volatile bool g_running = true;

static uint64_t clock_us(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

uint64_t get_monotonic_ms(void) { return clock_us(CLOCK_MONOTONIC) / 1000ULL; }
void mark_remote_slot_active(uint8_t slot) { (void)slot; }
bool is_companion_slot(uint8_t slot) { (void)slot; return false; }
void rf_note_slot_family(uint8_t slot, remote_model_t family) { (void)slot; (void)family; }

#define MAX_EVENTS 256
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_events[MAX_EVENTS][48]; /* "<button> <action>" */
static uint64_t s_event_us[MAX_EVENTS];
static int s_event_count = 0;

static void json_field(const char *body, const char *key, char *out, size_t len) {
    out[0] = '\0';
    const char *p = strstr(body, key);
    if (!p) return;
    p += strlen(key);
    size_t n = strcspn(p, "\"");
    if (n >= len) n = len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

int http_post_local(int port, const char *path, const char *body) {
    (void)port;
    if (strcmp(path, "/api/button-press") != 0 || !body) return 200;
    uint64_t now = clock_us(CLOCK_MONOTONIC);
    char button[24], action[16];
    json_field(body, "\"button\":\"", button, sizeof(button));
    json_field(body, "\"action\":\"", action, sizeof(action));
    pthread_mutex_lock(&s_log_lock);
    if (s_event_count < MAX_EVENTS) {
        snprintf(s_events[s_event_count], sizeof(s_events[0]), "%s %s", button, action);
        s_event_us[s_event_count] = now;
        s_event_count++;
    }
    pthread_mutex_unlock(&s_log_lock);
    return 200;
}

static int s_failures = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  [ok] "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); s_failures++; } \
} while (0)

static int event_count(void) {
    pthread_mutex_lock(&s_log_lock);
    int n = s_event_count;
    pthread_mutex_unlock(&s_log_lock);
    return n;
}

/* Waits until `want` events were posted, 1 s at most; returns the count */
static int wait_events(int want) {
    for (int i = 0; i < 1000 && event_count() < want; i++) usleep(1000);
    return event_count();
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(void) {
    printf("[*] test_rf_key_queue\n");
    /* no hub_daemon here: every key takes the HTTP path, which is the function above */
    g_rf_key_sock_path = "/nonexistent/hub_daemon_keys.sock";

    /* 1. Queued before the worker runs, as when the worker is still busy with the key before:
     * a quick tap of Up, then OK held and let go while Mute's hold repeat is also waiting */
    enqueue_remote_button(1, "DirectionUp", "press", NULL);
    enqueue_remote_button(1, "DirectionUp", "release", NULL);
    enqueue_remote_button(1, "Select", "press", NULL);
    enqueue_remote_button(1, "Select", "hold", NULL);
    enqueue_remote_button(1, "Select", "hold", NULL);
    enqueue_remote_button(1, "Mute", "hold", NULL);
    enqueue_remote_button(1, "Select", "release", NULL);

    pthread_t th;
    pthread_create(&th, NULL, dispatch_worker, NULL);
    static const char *want[] = { "DirectionUp press", "DirectionUp release", "Select press", "Mute hold", "Select release" };
    int n = wait_events(5);
    usleep(60000); /* anything more than the 5 would show by now */
    n = event_count();
    int same = (n == 5);
    for (int i = 0; same && i < 5; i++) same = (strcmp(s_events[i], want[i]) == 0);
    CHECK(n >= 2 && strcmp(s_events[0], "DirectionUp press") == 0 && strcmp(s_events[1], "DirectionUp release") == 0,
          "a tap queued whole is sent as press, then release (first event: %s)", n ? s_events[0] : "none");
    CHECK(same, "the release removes its button's queued holds only (%d events, 5 expected)", n);
    if (!same) for (int i = 0; i < n && i < 8; i++) printf("         %d: %s\n", i, s_events[i]);

    /* 2. An idle worker sends a key at once. 31 keys at shifting points of its 20 ms sleep. */
    uint64_t waits[31];
    for (int i = 0; i < 31; i++) {
        usleep(23000 + (useconds_t)(i * 700)); /* the worker is idle again, at another phase */
        int before = event_count();
        uint64_t t0 = clock_us(CLOCK_MONOTONIC);
        enqueue_remote_button(1, "VolumeUp", "press", NULL);
        if (wait_events(before + 1) <= before) {
            printf("  [FAIL] key %d was never sent\n", i);
            return 1;
        }
        pthread_mutex_lock(&s_log_lock);
        waits[i] = s_event_us[before] > t0 ? s_event_us[before] - t0 : 0;
        pthread_mutex_unlock(&s_log_lock);
    }
    qsort(waits, 31, sizeof(waits[0]), cmp_u64);
    CHECK(waits[15] < 5000, "an idle worker sends a queued key at once (median %.1f ms, longest %.1f ms of 31; limit 5 ms)",
          waits[15] / 1000.0, waits[30] / 1000.0);

    /* 3. Idle: the worker sleeps. 300 ms without keys cost next to no CPU time. */
    usleep(50000);
    uint64_t cpu0 = clock_us(CLOCK_PROCESS_CPUTIME_ID);
    usleep(300000);
    uint64_t cpu = clock_us(CLOCK_PROCESS_CPUTIME_ID) - cpu0;
    CHECK(cpu < 30000, "an idle worker uses no CPU (%.1f ms of CPU in 300 ms; limit 30 ms)", cpu / 1000.0);

    g_running = false;
    pthread_join(th, NULL);

    if (s_failures) {
        printf("[-] test_rf_key_queue: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("[+] test_rf_key_queue passed\n");
    return 0;
}
