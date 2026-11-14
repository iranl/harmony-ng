/*
 * hub_rf's key path (rf_keymap.c queue and dispatch thread, rf_state.c HTTP client), driven
 * from a test: plays key events read from stdin the way the RF receive thread queues them,
 * and lets the real dispatch thread send them to hub_daemon_test (key channel, or HTTP when
 * hub_daemon has no key channel). tests/test_rf_key_channel.py uses it.
 *
 * Build: gcc -Wall -O2 -Ipayload/source tests/rf_key_client.c payload/source/rf_keymap.c \
 *            payload/source/rf_state.c payload/source/cJSON.c \
 *            -Wl,--wrap=http_post_local -lpthread -lm
 * Usage: rf_key_client <daemon port> <key socket path> <linger ms> < events
 *   one event per line: <ms to wait before it> <press|hold|release> <button> [deviceId]
 * After the last event it gives the dispatch thread <linger ms> to send what is queued.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "rf_keymap.h"

/* Weak: the same client builds against a hub_rf without the key channel (HTTP only) */
extern const char *g_rf_key_sock_path __attribute__((weak));

static int s_port = 8089;

/* rf_keymap.c posts to the fixed DAEMON_PORT: the same call to the test daemon's port */
int __wrap_http_post_local(int port, const char *endpoint, const char *json_body) {
    (void)port;
    return http_request_local(s_port, "POST", endpoint, json_body, NULL, 0, 400);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <daemon port> <key socket path> <linger ms> < events\n", argv[0]);
        return 2;
    }
    s_port = atoi(argv[1]);
    if (&g_rf_key_sock_path) g_rf_key_sock_path = argv[2];
    setvbuf(stdout, NULL, _IOLBF, 0);

    pthread_t th;
    pthread_create(&th, NULL, dispatch_worker, NULL);

    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        int wait_ms = 0;
        char action[16] = {0}, button[64] = {0}, dev[16] = {0};
        if (sscanf(line, "%d %15s %63s %15s", &wait_ms, action, button, dev) < 3) continue;
        if (wait_ms > 0) usleep((useconds_t)wait_ms * 1000);
        enqueue_remote_button(1, button, action, dev[0] ? dev : NULL);
    }
    usleep((useconds_t)atoi(argv[3]) * 1000);
    g_running = false;
    pthread_join(th, NULL);
    return 0;
}
