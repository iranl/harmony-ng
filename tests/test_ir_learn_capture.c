/*
 * Learning an IR press (ir_i2s_capture, POST /api/capture) through the receiver thread, also
 * while the capture mode is on (Home Assistant keeps it on). Runs with the mock library:
 * <mock>/i2s_rx is the receiver (a read at the end of the file returns 0, and a 0 read during a
 * recording ends it, so each press is written with its quiet tail in one write),
 * <mock>/i2s_rx_stall stalls its reads, <mock>/i2s_tx_ms makes a send take that long.
 *
 *   test_ir_learn_capture          the behaviour checks
 *   test_ir_learn_capture race     a result crossing the waiter's limit, 200 rounds (built with
 *                                  -DIR_LEARN_GRACE_MS=0 and AddressSanitizer: the thread must
 *                                  never write the waiter's buffer once it gave up)
 *   test_ir_learn_capture cpu      the waiter sleeps (run under tests/no_futex_exec)
 */
#include "ir_i2s.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char g_rx[512], g_stall[512], g_txms[512];
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

/* The mock plays the file from its start at every open: a test that opens a new session starts
 * with a quiet line (only while no session is open: an open one reads on past the old end) */
static void reset_rx(void) {
    static uint8_t quiet[4096];
    memset(quiet, 0xff, sizeof(quiet));   /* no light */
    write_file(g_rx, quiet, sizeof(quiet));
}

/* One write(): the receiver thread must not read half a press */
static void append_rx(const uint8_t *data, size_t len) {
    int fd = open(g_rx, O_WRONLY | O_APPEND);
    if (fd < 0) { perror(g_rx); exit(1); }
    if (write(fd, data, len) != (ssize_t)len) { perror("write"); exit(1); }
    close(fd);
}

static int count_dir(const char *path) {
    int n = 0;
    DIR *d = opendir(path);
    if (!d) return -1;
    while (readdir(d)) n++;
    closedir(d);
    return n - 2;
}

/* ---- receiver data, as tests/ir_rx_signal.py makes it ---- */

/* Raw /dev/i2s receive data for mark/space durations (starting with a mark): 1.5 us per bit,
 * MSB first, inverted (light = 0 bit), the two bytes of every 16-bit sample swapped */
static size_t bitstream(const unsigned *t, int nt, size_t idle_before_bytes, unsigned idle_after_us,
                        uint8_t **out) {
    const double period = 1e6 / 38000.0, duty = 1.0 / 3;
    size_t bits = idle_before_bytes * 8;
    for (int i = 0; i <= nt; i++) bits += (size_t)lround((i < nt ? t[i] : idle_after_us) / 1.5);
    bits += 16;
    uint8_t *light = calloc(bits, 1);
    size_t pos = idle_before_bytes * 8;
    for (int i = 0; i <= nt; i++) {
        unsigned d = i < nt ? t[i] : idle_after_us;
        long n = lround(d / 1.5), pulses = lround(d / period);
        for (long k = 0; k < n; k++, pos++) {
            double tt = k * 1.5;
            light[pos] = (i % 2 == 0 && tt < pulses * period && fmod(tt, period) < period * duty);
        }
    }
    pos += (16 - pos % 16) % 16;
    size_t len = pos / 8;
    uint8_t *data = malloc(len);
    for (size_t j = 0; j < len; j++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++) byte = (uint8_t)((byte << 1) | light[j * 8 + b]);
        data[j] = (uint8_t)~byte;
    }
    for (size_t j = 0; j + 1 < len; j += 2) {
        uint8_t x = data[j];
        data[j] = data[j + 1];
        data[j + 1] = x;
    }
    free(light);
    *out = data;
    return len;
}

/* Sony 12-bit frame: 2.4 ms header, 1.2 ms (1) or 0.6 ms (0) marks, 0.6 ms spaces */
static int sirc12(unsigned *t, unsigned code) {
    int n = 0;
    t[n++] = 2400;
    t[n++] = 600;
    for (int i = 0; i < 12; i++) {
        t[n++] = (code >> (11 - i)) & 1 ? 1200 : 600;
        if (i < 11) t[n++] = 600;
    }
    return n;
}

/* frames Sony frames 45 ms apart, then 250 ms quiet */
static void append_sony_press(int frames, unsigned code) {
    unsigned t[256];
    int n = 0;
    for (int f = 0; f < frames; f++) {
        int k = sirc12(t + n, code);
        unsigned sum = 0;
        for (int j = 0; j < k; j++) sum += t[n + j];
        n += k;
        if (f < frames - 1) t[n++] = 45000 - sum;
    }
    uint8_t *data;
    size_t len = bitstream(t, n, 1024, 250000, &data);
    append_rx(data, len);
    free(data);
}

/* Frames in a raw string: spaces of 20 ms or more end one (the last space is the gap after it) */
static int raw_frames(const char *raw) {
    int frames = 0;
    for (const char *p = raw; *p; p++) {
        if (*p == 'S' && strtoul(p + 1, NULL, 16) >= 20000) frames++;
    }
    return frames;
}

/* ---- capture mode listeners ---- */

static pthread_mutex_t g_cnt_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_events = 0, g_state_calls = 0;

static void on_signal(const char *raw, uint32_t hz, void *user) {
    (void)raw; (void)hz; (void)user;
    pthread_mutex_lock(&g_cnt_lock);
    g_events++;
    pthread_mutex_unlock(&g_cnt_lock);
}

static void on_state(void *user) {
    (void)user;
    pthread_mutex_lock(&g_cnt_lock);
    g_state_calls++;
    pthread_mutex_unlock(&g_cnt_lock);
}

static int events(void) {
    pthread_mutex_lock(&g_cnt_lock);
    int n = g_events;
    pthread_mutex_unlock(&g_cnt_lock);
    return n;
}

static int state_calls(void) {
    pthread_mutex_lock(&g_cnt_lock);
    int n = g_state_calls;
    pthread_mutex_unlock(&g_cnt_lock);
    return n;
}

static void reset_counts(void) {
    pthread_mutex_lock(&g_cnt_lock);
    g_events = g_state_calls = 0;
    pthread_mutex_unlock(&g_cnt_lock);
}

static int wait_events(int want, double secs) {
    double end = now_s(CLOCK_MONOTONIC) + secs;
    while (events() < want && now_s(CLOCK_MONOTONIC) < end) usleep(10000);
    return events();
}

static int mode_on(void) {
    char err[160];
    return ir_i2s_capture_mode_set(1, on_signal, NULL, err, sizeof(err));
}

/* ---- a learn on its own thread ---- */

typedef struct {
    pthread_t th;
    unsigned timeout_s;
    int rc;
    double took;
    char *raw;
} learn_t;

static void *learn_run(void *arg) {
    learn_t *l = (learn_t *)arg;
    double t0 = now_s(CLOCK_MONOTONIC);
    l->rc = ir_i2s_capture(l->raw, 16384, l->timeout_s);
    l->took = now_s(CLOCK_MONOTONIC) - t0;
    return NULL;
}

static void learn_start(learn_t *l, unsigned timeout_s) {
    memset(l, 0, sizeof(*l));
    l->timeout_s = timeout_s;
    l->raw = calloc(1, 16384);
    pthread_create(&l->th, NULL, learn_run, l);
    usleep(300 * 1000);   /* the receiver thread arms it (within a few ms on the mock) */
}

static void learn_end(learn_t *l) {
    pthread_join(l->th, NULL);
}

typedef struct {
    int rc;
    double took;
} send_t;

static void *send_run(void *arg) {
    send_t *r = (send_t *)arg;
    const uint16_t words[] = {0x8000 | 2400, 600, 0x8000 | 1200, 600, 0x8000 | 600};
    double t0 = now_s(CLOCK_MONOTONIC);
    r->rc = ir_i2s_blast_words(38000, 33, words, 5, 7);
    r->took = now_s(CLOCK_MONOTONIC) - t0;
    return NULL;
}

static void stuck(int sig) {
    (void)sig;
    static const char msg[] = "  [FAIL] a check did not end within its time limit\n[-] test_ir_learn_capture: stuck\n";
    ssize_t w = write(1, msg, sizeof(msg) - 1);
    (void)w;
    _exit(1);
}

/* ---- the behaviour checks ---- */

static void behaviour(void) {
    char what[640];
    learn_t l;
    ir_i2s_set_capture_state_listener(on_state, NULL);

    /* 1. learn while the capture mode is on: the whole press, no event, no state change */
    alarm(60);
    check(mode_on() == 0, "capture mode on");
    usleep(100 * 1000);
    reset_counts();
    learn_start(&l, 5);
    append_sony_press(3, 0x910);
    learn_end(&l);
    snprintf(what, sizeof(what), "learn with the mode on: rc %d, %d frames (want 0 and 3) [%.60s]",
             l.rc, raw_frames(l.raw), l.raw);
    check(l.rc == 0 && raw_frames(l.raw) == 3, what);
    usleep(300 * 1000);
    snprintf(what, sizeof(what), "no ir_received event for the learned press (%d), no state change (%d), mode still on",
             events(), state_calls());
    check(events() == 0 && state_calls() == 0 && ir_i2s_capture_mode_enabled(), what);
    append_sony_press(1, 0x290);
    int ev = wait_events(1, 3);
    usleep(500 * 1000);
    snprintf(what, sizeof(what), "the next press is one event again (%d)", events());
    check(ev == 1 && events() == 1, what);
    free(l.raw);

    /* 2. timeout with the mode on */
    reset_counts();
    learn_start(&l, 1);
    learn_end(&l);
    snprintf(what, sizeof(what), "learn timeout with the mode on: rc %d after %.2f s, the mode stays on, no state change (%d)",
             l.rc, l.took + 0.3, state_calls());
    check(l.rc == -5 && l.took + 0.3 < 2.0 && ir_i2s_capture_mode_enabled() && state_calls() == 0, what);
    free(l.raw);

    /* 3. a second learn while one runs is refused at once */
    learn_start(&l, 2);
    char raw2[256];
    double t0 = now_s(CLOCK_MONOTONIC);
    int rc2 = ir_i2s_capture(raw2, sizeof(raw2), 1);
    double took2 = now_s(CLOCK_MONOTONIC) - t0;
    snprintf(what, sizeof(what), "a second learn while one runs: rc %d (-EBUSY) after %.0f ms (%s)", rc2, took2 * 1000, raw2);
    check(rc2 == -EBUSY && took2 < 0.1 && strstr(raw2, "already running"), what);
    learn_end(&l);
    free(l.raw);

    /* 4. turning the mode off during a learn returns at once, and the learn completes */
    reset_counts();
    learn_start(&l, 4);
    t0 = now_s(CLOCK_MONOTONIC);
    ir_i2s_capture_mode_set(0, NULL, NULL, NULL, 0);
    double off_took = now_s(CLOCK_MONOTONIC) - t0;
    snprintf(what, sizeof(what), "mode off during a learn: returns after %.1f ms (under 100), reported off, one state change (%d)",
             off_took * 1000, state_calls());
    check(off_took < 0.1 && !ir_i2s_capture_mode_enabled() && state_calls() == 1, what);
    append_sony_press(3, 0x910);
    learn_end(&l);
    snprintf(what, sizeof(what), "the learn completes after the mode went off: rc %d, %d frames", l.rc, raw_frames(l.raw));
    check(l.rc == 0 && raw_frames(l.raw) == 3, what);
    free(l.raw);
    usleep(200 * 1000);

    /* 5. mode off: the thread runs for the learn only; nothing is left behind */
    reset_counts();
    reset_rx();
    char warm[256];
    ir_i2s_capture(warm, sizeof(warm), 1);   /* a first learn opens the waiter's wake pipe for good */
    usleep(200 * 1000);
    int thr0 = count_dir("/proc/self/task"), fds0 = count_dir("/proc/self/fd");
    learn_start(&l, 4);
    int thr_mid = count_dir("/proc/self/task");
    int on_mid = ir_i2s_capture_mode_enabled();
    append_sony_press(3, 0x910);
    learn_end(&l);
    usleep(200 * 1000);
    int thr1 = count_dir("/proc/self/task"), fds1 = count_dir("/proc/self/fd");
    snprintf(what, sizeof(what), "mode off: learn rc %d, %d frames; receiver thread during it (threads %d -> %d), reported off",
             l.rc, raw_frames(l.raw), thr0, thr_mid);
    check(l.rc == 0 && raw_frames(l.raw) == 3 && thr_mid >= thr0 + 2 && !on_mid, what);
    snprintf(what, sizeof(what), "after it: threads %d -> %d, file descriptors %d -> %d, no state change (%d), no event (%d)",
             thr0, thr1, fds0, fds1, state_calls(), events());
    check(thr1 == thr0 && fds1 == fds0 && state_calls() == 0 && events() == 0, what);
    free(l.raw);

    /* 6. the mode turned on during a learn-only thread keeps it running and reported on */
    reset_counts();
    reset_rx();
    learn_start(&l, 4);
    t0 = now_s(CLOCK_MONOTONIC);
    int rc = mode_on();
    double on_took = now_s(CLOCK_MONOTONIC) - t0;
    snprintf(what, sizeof(what), "mode on during a learn: rc %d after %.1f ms, reported on, one state change (%d)",
             rc, on_took * 1000, state_calls());
    check(rc == 0 && on_took < 0.1 && ir_i2s_capture_mode_enabled() && state_calls() == 1, what);
    append_sony_press(3, 0x910);
    learn_end(&l);
    usleep(300 * 1000);
    snprintf(what, sizeof(what), "the learn completes (rc %d, %d frames), no event for it (%d), the mode still on",
             l.rc, raw_frames(l.raw), events());
    check(l.rc == 0 && raw_frames(l.raw) == 3 && events() == 0 && ir_i2s_capture_mode_enabled(), what);
    append_sony_press(1, 0x290);
    ev = wait_events(1, 3);
    snprintf(what, sizeof(what), "the receiver thread goes on as the capture mode: next press one event (%d)", ev);
    check(ev == 1, what);
    ir_i2s_capture_mode_set(0, NULL, NULL, NULL, 0);
    free(l.raw);

    /* 7. a send during a learn goes out at once; light read during a send is not learned */
    reset_rx();
    write_file(g_txms, "600", 3);
    learn_start(&l, 3);
    send_t snd;
    pthread_t sth;
    send_run(&snd);                   /* nothing pressed: the learn waits, the send must not */
    snprintf(what, sizeof(what), "a send while a learn waits: rc %d after %.2f s (the 0.6 s drain, not delayed)", snd.rc, snd.took);
    check(snd.rc == 0 && snd.took < 1.2, what);
    pthread_create(&sth, NULL, send_run, &snd);
    usleep(150 * 1000);
    append_sony_press(3, 0x910);      /* the receiver sees light while the Hub sends */
    pthread_join(sth, NULL);
    learn_end(&l);
    unlink(g_txms);
    snprintf(what, sizeof(what), "light read during a send is not learned: rc %d (-5), send rc %d [%.120s]", l.rc, snd.rc, l.raw);
    check(l.rc == -5 && snd.rc == 0, what);
    free(l.raw);

    /* 8. a stall in the middle of a learn ends it with -7 within ~2.5 s */
    reset_rx();
    learn_start(&l, 8);
    write_file(g_stall, NULL, 0);
    t0 = now_s(CLOCK_MONOTONIC);
    learn_end(&l);
    double stall_took = now_s(CLOCK_MONOTONIC) - t0;
    snprintf(what, sizeof(what), "stall during a learn: rc %d after %.2f s (at most 3) [%s]", l.rc, stall_took, l.raw);
    check(l.rc == -7 && stall_took < 3.0 && strstr(l.raw, "stopped delivering"), what);
    free(l.raw);
    unlink(g_stall);
    usleep(600 * 1000);   /* the detached close returns once the stall ends */
    reset_rx();
    char again[256];
    rc = ir_i2s_capture(again, sizeof(again), 1);
    snprintf(what, sizeof(what), "learning works again once the stalled session closed (rc %d, -5)", rc);
    check(rc == -5, what);

    /* 9. while the receiver is down (here stalled), a learn fails at once with the stored error */
    reset_rx();
    check(mode_on() == 0, "capture mode on");
    write_file(g_stall, NULL, 0);
    char merr[160] = "";
    double end = now_s(CLOCK_MONOTONIC) + 5;
    while (!ir_i2s_capture_mode_error(merr, sizeof(merr)) && now_s(CLOCK_MONOTONIC) < end) usleep(20 * 1000);
    t0 = now_s(CLOCK_MONOTONIC);
    char down[256];
    rc = ir_i2s_capture(down, sizeof(down), 5);
    double down_took = now_s(CLOCK_MONOTONIC) - t0;
    snprintf(what, sizeof(what), "receiver down (%s): a learn fails at once, rc %d after %.0f ms [%s], the mode still on",
             merr, rc, down_took * 1000, down);
    check(merr[0] && rc == -7 && down_took < 0.5 && strstr(down, "stopped delivering") && ir_i2s_capture_mode_enabled(), what);
    unlink(g_stall);
    ir_i2s_capture_mode_set(0, NULL, NULL, NULL, 0);
    usleep(600 * 1000);   /* the detached close returns once the stall ends */

    /* 10. a send that begins and ends while a press is read (one read on a Hub): the press is
     * not kept, and the timeout says why. The thread could see the send's mark itself instead
     * of the counter (the two calls are a few ns apart): then try once more. */
    unsigned st[64];
    int sn = sirc12(st, 0x910);
    uint8_t *press;
    size_t plen = bitstream(st, sn, 1024, 250000, &press);
    for (int attempt = 0; attempt < 2; attempt++) {
        reset_rx();
        learn_start(&l, 1);
        ir_i2s_tx_hold(1);
        ir_i2s_tx_hold(0);
        append_rx(press, plen);
        learn_end(&l);
        int said = strstr(l.raw, "not kept") != NULL;
        if (attempt == 1 || said) {
            snprintf(what, sizeof(what), "a press read during a send: rc %d (-5), the message says it was not kept [%s]",
                     l.rc, l.raw);
            check(l.rc == -5 && said, what);
        }
        free(l.raw);
        if (said) break;
    }
    free(press);

    /* 11. shutdown: a learn in progress returns, the mode goes off, the thread ends, no more learns */
    reset_rx();
    usleep(100 * 1000);
    int thr2 = count_dir("/proc/self/task");
    check(mode_on() == 0, "capture mode on");
    learn_start(&l, 5);
    t0 = now_s(CLOCK_MONOTONIC);
    ir_i2s_capture_shutdown();
    learn_end(&l);
    double sd_took = now_s(CLOCK_MONOTONIC) - t0;
    usleep(200 * 1000);
    int thr3 = count_dir("/proc/self/task");
    snprintf(what, sizeof(what), "shutdown during a learn: the learn returns rc %d (-9) after %.0f ms [%s], mode off, threads %d -> %d",
             l.rc, sd_took * 1000, l.raw, thr2, thr3);
    check(l.rc == -9 && sd_took < 0.5 && !ir_i2s_capture_mode_enabled() && thr3 == thr2, what);
    free(l.raw);
    char after[256];
    t0 = now_s(CLOCK_MONOTONIC);
    rc = ir_i2s_capture(after, sizeof(after), 1);
    snprintf(what, sizeof(what), "a learn after the shutdown is refused at once (rc %d, -9)", rc);
    check(rc == -9 && now_s(CLOCK_MONOTONIC) - t0 < 0.1, what);
    alarm(0);
}

/* ---- a result crossing the waiter's limit ---- */

typedef struct {
    unsigned delay_us;
    const uint8_t *press;
    size_t len;
} feeder_t;

static void *feed_later(void *arg) {
    feeder_t *f = (feeder_t *)arg;
    usleep(f->delay_us);
    append_rx(f->press, f->len);
    return NULL;
}

/* Learns with deadlines of 15 to 39 ms; every other round a press arrives near the deadline. The
 * waiter (built with IR_LEARN_GRACE_MS 0) gives up right at the deadline while the thread
 * delivers its result: the buffer is freed at once, so a late write is a use after free. */
static void race(void) {
#if !defined(IR_LEARN_GRACE_MS) || IR_LEARN_GRACE_MS != 0
    printf("  (built without -DIR_LEARN_GRACE_MS=0: the waiter waits 5 s past the deadline, no crossing)\n");
#endif
    char what[200];
    alarm(120);
    check(mode_on() == 0, "capture mode on");
    unsigned t[64];
    int n = sirc12(t, 0x910);
    uint8_t *press;
    size_t plen = bitstream(t, n, 64, 250000, &press);
    int got_ok = 0, got_timeout = 0, got_gave_up = 0, bad = 0;
    for (int i = 0; i < 200; i++) {
        unsigned timeout_ms = 15 + (unsigned)(i % 25);
        pthread_t th;
        feeder_t f = { (timeout_ms + 1) * 1000 + (unsigned)((i * 7) % 9) * 1000 - 4000, press, plen };
        int fed = i % 2;
        if (fed) pthread_create(&th, NULL, feed_later, &f);
        char *raw = malloc(16384);
        memset(raw, 0, 16384);
        int rc = ir_i2s_capture_ms(raw, 16384, timeout_ms);
        if (rc == 0 && raw[0] == 'F') got_ok++;
        else if (rc == -5) got_timeout++;
        else if (rc == -7) got_gave_up++;
        else bad++;
        free(raw);   /* a write by the thread from here on is a use after free */
        char *reuse = malloc(16384);
        memset(reuse, 0xA5, 16384);
        if (fed) pthread_join(th, NULL);
        usleep(5000);
        free(reuse);
    }
    usleep(300 * 1000);   /* a late result would land now */
    snprintf(what, sizeof(what), "200 rounds: %d results, %d timeouts, %d given up by the waiter, %d other",
             got_ok, got_timeout, got_gave_up, bad);
    check(bad == 0 && got_ok + got_timeout + got_gave_up == 200, what);
    free(press);
    ir_i2s_capture_mode_set(0, NULL, NULL, NULL, 0);
    alarm(0);
}

/* ---- the waiter sleeps (under tests/no_futex_exec, futex waits never sleep, as on the Hub) ---- */

static void cpu(void) {
    char what[200], raw[256];
    alarm(20);
    for (int on = 1; on >= 0; on--) {
        if (on) check(mode_on() == 0, "capture mode on");
        double w0 = now_s(CLOCK_MONOTONIC), c0 = now_s(CLOCK_THREAD_CPUTIME_ID);
        int rc = ir_i2s_capture(raw, sizeof(raw), 2);
        double wall = now_s(CLOCK_MONOTONIC) - w0, used = now_s(CLOCK_THREAD_CPUTIME_ID) - c0;
        snprintf(what, sizeof(what), "learn with the mode %s: rc %d after %.2f s, the waiter used %.0f ms CPU (at most 50)",
                 on ? "on" : "off", rc, wall, used * 1000);
        check(rc == -5 && wall > 1.9 && used < 0.05, what);
        if (on) ir_i2s_capture_mode_set(0, NULL, NULL, NULL, 0);
    }
    alarm(0);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *md = getenv("HARMONY_MOCK_DIR");
    if (!md || !md[0]) {
        fprintf(stderr, "HARMONY_MOCK_DIR not set\n");
        return 1;
    }
    snprintf(g_rx, sizeof(g_rx), "%s/i2s_rx", md);
    snprintf(g_stall, sizeof(g_stall), "%s/i2s_rx_stall", md);
    snprintf(g_txms, sizeof(g_txms), "%s/i2s_tx_ms", md);
    reset_rx();
    unlink(g_stall);
    unlink(g_txms);
    signal(SIGALRM, stuck);

    const char *mode = argc > 1 ? argv[1] : "behaviour";
    if (strcmp(mode, "race") == 0) race();
    else if (strcmp(mode, "cpu") == 0) cpu();
    else behaviour();
    unlink(g_rx);
    if (g_fail) {
        printf("[-] test_ir_learn_capture %s: %d check(s) failed\n", mode, g_fail);
        return 1;
    }
    printf("[+] test_ir_learn_capture %s: all checks passed\n", mode);
    return 0;
}
