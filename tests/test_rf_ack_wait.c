/* hub_rf waits up to a timeout for the remote's ACK of a frame. The Hub's kernel has no futex
 * system call, so a wait on a condition variable returns at once there and the sender spun at
 * full CPU for the whole wait, on every frame that wants an ACK. Run under tests/no_futex_exec
 * (futex waits never sleep, as on the Hub): the waiting thread must sleep through the timeout,
 * wake when the ACK comes, and not be woken early by an ACK that came after the previous wait.
 * Runs the real hub_rf sources (main renamed) with /dev/rfspi replaced by a socket pair. */
#undef main
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "rf_state.h"
#include "rf_spi.h"

static int s_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } \
                              else { printf("  [PASS] " __VA_ARGS__); printf("\n"); } } while (0)

static double now_s(clockid_t clk) {
    struct timespec ts;
    clock_gettime(clk, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* One acked send: wall time and this thread's CPU time it took, its result */
static int timed_send(uint8_t seq, int timeout_ms, double *wall, double *cpu) {
    rf_frame_t f;
    uint8_t pl[1] = {0};
    rf_build_frame(&f, 1, RF_MSG_WAKE, seq, RF_FLAG_ACK_REQ, pl, 1);
    double w0 = now_s(CLOCK_MONOTONIC), c0 = now_s(CLOCK_THREAD_CPUTIME_ID);
    int rc = rf_send_frame_acked(&f, timeout_ms);
    *wall = now_s(CLOCK_MONOTONIC) - w0;
    *cpu = now_s(CLOCK_THREAD_CPUTIME_ID) - c0;
    return rc;
}

/* A wait that spins can also never end (glibc's condition variable never sees its timeout
 * when no futex wait sleeps): give up after a few seconds instead of hanging */
static void stuck(int sig) {
    (void)sig;
    char msg[160];
    int n = snprintf(msg, sizeof(msg), "  [FAIL] the send did not return within 5 s; the process used %.1f s CPU meanwhile\n"
                     "[-] test_rf_ack_wait: the ACK wait spins without futex\n", now_s(CLOCK_PROCESS_CPUTIME_ID));
    if (n > 0) {
        ssize_t w = write(1, msg, (size_t)n);
        (void)w;
    }
    _exit(1);
}

typedef struct { uint8_t seq; int delay_ms; } ack_job_t;

static void *ack_later(void *arg) {
    ack_job_t *j = (ack_job_t *)arg;
    usleep((useconds_t)j->delay_ms * 1000);
    rf_signal_ack(j->seq, 0, NULL);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0]; /* the frames go to the other end, which nobody reads */

    /* Under tests/no_futex_exec a futex wait returns at once instead of timing out */
    int word = 0;
    struct timespec one_ms = { 0, 1000000 };
    long r = syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, &one_ms, NULL, 0);
    printf("  futex waits %s here\n", (r < 0 && errno == ETIMEDOUT) ? "sleep (not like the Hub)" : "never sleep (like the Hub)");

    signal(SIGALRM, stuck);
    alarm(5);   /* the three sends take about 1 s */
    double wall, cpu;
    /* 1. no ACK: the whole timeout passes, the thread sleeps through it */
    int rc = timed_send(1, 500, &wall, &cpu);
    CHECK(rc == -1 && wall >= 0.45 && wall < 1.0, "no ACK: the send fails after its 500 ms timeout (%.0f ms)", wall * 1000);
    CHECK(cpu < 0.05, "no ACK: the waiting thread used %.0f ms CPU in %.0f ms (at most 50)", cpu * 1000, wall * 1000);

    /* 2. the ACK after 100 ms ends the wait at once */
    ack_job_t j = { 2, 100 };
    pthread_t t;
    pthread_create(&t, NULL, ack_later, &j);
    rc = timed_send(2, 2000, &wall, &cpu);
    pthread_join(t, NULL);
    CHECK(rc == 0 && wall >= 0.08 && wall < 0.5, "ACK after 100 ms: the send succeeds after %.0f ms", wall * 1000);
    CHECK(cpu < 0.05, "ACK after 100 ms: the waiting thread used %.0f ms CPU (at most 50)", cpu * 1000);

    /* 3. a second ACK for frame 2, late: it must not cut the next wait short */
    rf_signal_ack(2, 0, NULL);
    rc = timed_send(3, 300, &wall, &cpu);
    CHECK(rc == -1 && wall >= 0.27, "a late ACK of the previous frame does not end the next wait (%.0f ms of 300)", wall * 1000);

    alarm(0);
    if (s_failures) {
        printf("[-] test_rf_ack_wait: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] test_rf_ack_wait: hub_rf sleeps while it waits for an ACK, also without futex\n");
    return 0;
}
