/*
 * Turning the IR capture mode off waits for the capture thread (up to 3 s for a stalled read).
 * A request that turns the mode on meanwhile must win once the stop is done: it used to see the
 * mode still on, return at once, and then the stop marked the mode off, so the last request was
 * lost. Requests to change the mode now run one after the other.
 * Runs with the mock library: <mock>/i2s_rx gives the receiver data, <mock>/i2s_rx_stall stalls
 * its reads (removed after 1 s here).
 */
#include "ir_i2s.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_stall[512];

static void write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (len) fwrite(data, 1, len, f);
    fclose(f);
}

static void *stop_mode(void *arg) {
    (void)arg;
    ir_i2s_capture_mode_set(0, NULL, NULL, NULL, 0);
    return NULL;
}

static void *unstall_later(void *arg) {
    (void)arg;
    usleep(1000 * 1000);
    unlink(g_stall);
    return NULL;
}

int main(void) {
    const char *md = getenv("HARMONY_MOCK_DIR");
    if (!md || !md[0]) {
        fprintf(stderr, "HARMONY_MOCK_DIR not set\n");
        return 1;
    }
    char rx[512], err[128];
    snprintf(rx, sizeof(rx), "%s/i2s_rx", md);
    snprintf(g_stall, sizeof(g_stall), "%s/i2s_rx_stall", md);
    static uint8_t quiet[8192]; /* no IR light: the thread just keeps reading */
    write_file(rx, quiet, sizeof(quiet));
    unlink(g_stall);

    int fails = 0;
    int rc = ir_i2s_capture_mode_set(1, NULL, NULL, err, sizeof(err));
    printf("%s capture mode on (rc %d %s)\n", rc == 0 ? "PASS" : "FAIL", rc, err);
    if (rc != 0) return 1;

    write_file(g_stall, NULL, 0); /* the next read stalls: the stop takes a while */
    usleep(200 * 1000);
    pthread_t stopper, unstaller;
    pthread_create(&stopper, NULL, stop_mode, NULL);
    pthread_create(&unstaller, NULL, unstall_later, NULL);
    usleep(300 * 1000);
    rc = ir_i2s_capture_mode_set(1, NULL, NULL, err, sizeof(err)); /* asked while the stop runs */
    pthread_join(stopper, NULL);
    pthread_join(unstaller, NULL);
    int on = ir_i2s_capture_mode_enabled();
    printf("%s on asked during the stop: rc %d, the mode ends %s\n", (rc == 0 && on) ? "PASS" : "FAIL", rc, on ? "on" : "off");
    if (rc != 0 || !on) fails++;

    ir_i2s_capture_mode_set(0, NULL, NULL, NULL, 0);
    printf("%s off at the end\n", ir_i2s_capture_mode_enabled() ? "FAIL" : "PASS");
    if (ir_i2s_capture_mode_enabled()) fails++;
    unlink(rx);
    if (fails) {
        printf("%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("All capture mode race checks passed\n");
    return 0;
}
