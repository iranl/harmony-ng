/* Linked into the hub_rf host build of tests/test_rf_restart_state.py with
 * -Wl,--wrap=rf_spi_close -Wl,--wrap=fsync:
 * - HUB_RF_TEST_HANG_CLOSE set: closing the radio never returns, as a close that waits on a
 *   driver whose read() is stuck would.
 * - HUB_RF_TEST_SLOW_CLOSE_MS=<ms>: the close takes that long, then returns.
 * - HUB_RF_TEST_STALL_FSYNC_AT=<n>, HUB_RF_TEST_STALL_FSYNC_MS=<ms>: the n-th fsync() after the
 *   close (the state move's file writes) takes that long, as a flash write during JFFS2 garbage
 *   collection can. HUB_RF_TEST_LOG_FSYNC set: each fsync() after the close is logged. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

void __real_rf_spi_close(void);
int __real_fsync(int fd);

static volatile int s_closed;
static int s_fsyncs_after_close;

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0) {
    }
}

void __wrap_rf_spi_close(void) {
    if (getenv("HUB_RF_TEST_HANG_CLOSE"))
        for (;;) sleep(60);
    const char *slow = getenv("HUB_RF_TEST_SLOW_CLOSE_MS");
    if (slow) sleep_ms(atol(slow));
    __real_rf_spi_close();
    s_closed = 1;
}

int __wrap_fsync(int fd) {
    if (s_closed) {
        int n = ++s_fsyncs_after_close;
        if (getenv("HUB_RF_TEST_LOG_FSYNC")) fprintf(stderr, "[test] fsync %d after the close\n", n);
        const char *at = getenv("HUB_RF_TEST_STALL_FSYNC_AT");
        const char *ms = getenv("HUB_RF_TEST_STALL_FSYNC_MS");
        if (at && ms && n == atoi(at)) sleep_ms(atol(ms));
    }
    return __real_fsync(fd);
}
