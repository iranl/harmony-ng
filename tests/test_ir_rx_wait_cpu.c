/*
 * The IR capture mode is turned off while the receiver's read is stalled (here for 1 s): the
 * caller waits for the capture thread, which waits for its stall watchdog. The Hub's kernel has
 * no futex system call, so a wait built on it (pthread_join, pthread_cond_wait) returns at once
 * there and the waiting thread spun for the whole wait. Run under tests/no_futex_exec (futex
 * waits never sleep, as on the Hub) and the mock library (<mock>/i2s_rx gives the receiver
 * data, <mock>/i2s_rx_stall stalls its reads): the caller must sleep while it waits, and
 * turning the mode on and off, also when the receiver does not open, must not leave threads
 * or file descriptors behind.
 */
#include "ir_i2s.h"

#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char g_stall[512];
static int g_fail = 0;

static void check(int ok, const char *what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

static double now_s(clockid_t clk) {
    struct timespec ts;
    clock_gettime(clk, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (len) fwrite(data, 1, len, f);
    fclose(f);
}

static int count_fds(void) {
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    while (readdir(d)) n++;
    closedir(d);
    return n;
}

static int count_threads(void) {
    int n = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) return -1;
    while (readdir(d)) n++;
    closedir(d);
    return n - 2;   /* . and .. */
}

static void *unstall_later(void *arg) {
    (void)arg;
    usleep(1000 * 1000);
    unlink(g_stall);
    return NULL;
}

static void stuck(int sig) {
    (void)sig;
    static const char msg[] = "  [FAIL] the capture mode did not turn off within 10 s\n[-] test_ir_rx_wait_cpu: a wait never ended\n";
    ssize_t w = write(1, msg, sizeof(msg) - 1);
    (void)w;
    _exit(1);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *md = getenv("HARMONY_MOCK_DIR");
    if (!md || !md[0]) {
        fprintf(stderr, "HARMONY_MOCK_DIR not set\n");
        return 1;
    }
    char rx[512], err[160], what[200];
    snprintf(rx, sizeof(rx), "%s/i2s_rx", md);
    snprintf(g_stall, sizeof(g_stall), "%s/i2s_rx_stall", md);
    static uint8_t quiet[8192]; /* no IR light: the mode thread just keeps reading */
    write_file(rx, quiet, sizeof(quiet));
    unlink(g_stall);
    signal(SIGALRM, stuck);

    /* 1. on, then off while the read is stalled for 1 s */
    int fds0 = count_fds(), thr0 = count_threads();
    int rc = ir_i2s_capture_mode_set(1, NULL, NULL, err, sizeof(err));
    snprintf(what, sizeof(what), "capture mode on (rc %d %s)", rc, err);
    check(rc == 0, what);
    if (rc != 0) return 1;
    write_file(g_stall, NULL, 0);
    usleep(200 * 1000);   /* the thread is now in a stalled read */
    pthread_t unstaller;
    pthread_create(&unstaller, NULL, unstall_later, NULL);
    pthread_detach(unstaller);
    alarm(10);
    double w0 = now_s(CLOCK_MONOTONIC), c0 = now_s(CLOCK_THREAD_CPUTIME_ID);
    ir_i2s_capture_mode_set(0, NULL, NULL, NULL, 0);
    double wall = now_s(CLOCK_MONOTONIC) - w0, cpu = now_s(CLOCK_THREAD_CPUTIME_ID) - c0;
    alarm(0);
    snprintf(what, sizeof(what), "off during a stalled read: done after %.0f ms, the caller used %.0f ms CPU (at most 50)",
             wall * 1000, cpu * 1000);
    check(!ir_i2s_capture_mode_enabled() && wall > 0.5 && cpu < 0.05, what);
    usleep(100 * 1000);   /* the unstaller ends */

    /* 2. the receiver does not open: the request reports it, the thread is gone */
    unlink(rx);
    alarm(10);
    rc = ir_i2s_capture_mode_set(1, NULL, NULL, err, sizeof(err));
    alarm(0);
    snprintf(what, sizeof(what), "on without a receiver fails and leaves the mode off (rc %d)", rc);
    check(rc != 0 && !ir_i2s_capture_mode_enabled(), what);

    /* 3. ten more on/off cycles */
    write_file(rx, quiet, sizeof(quiet));
    alarm(20);
    int ok = 1;
    for (int i = 0; i < 10; i++) {
        if (ir_i2s_capture_mode_set(1, NULL, NULL, err, sizeof(err)) != 0) ok = 0;
        usleep(20 * 1000);
        ir_i2s_capture_mode_set(0, NULL, NULL, NULL, 0);
    }
    alarm(0);
    usleep(100 * 1000);   /* detached threads finish their exit */
    int fds1 = count_fds(), thr1 = count_threads();
    snprintf(what, sizeof(what), "10 on/off cycles: file descriptors %d -> %d, threads %d -> %d", fds0, fds1, thr0, thr1);
    check(ok && fds1 == fds0 && thr1 == thr0, what);

    unlink(rx);
    if (g_fail) {
        printf("[-] test_ir_rx_wait_cpu: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("[+] test_ir_rx_wait_cpu: IR capture waits sleep, also without futex\n");
    return 0;
}
