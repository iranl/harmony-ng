#include "ir_i2s.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

#define I2S_DEVICE "/dev/i2s"
#define I2S_LOCK_FILE "/tmp/harmony_i2s.lock"

static int i2s_lock_acquire(void) {
    int lfd = open(I2S_LOCK_FILE, O_RDWR | O_CREAT, 0666);
    if (lfd < 0) return -1;
    for (int i = 0; i < 50; i++) {
        if (flock(lfd, LOCK_EX | LOCK_NB) == 0) {
            return lfd;
        }
        usleep(100000);
    }
    close(lfd);
    return -1;
}

static void i2s_lock_release(int lfd) {
    if (lfd >= 0) {
        flock(lfd, LOCK_UN);
        close(lfd);
    }
}


/* ath_i2s ioctl commands: _IOW('N', nr, int) = 0x80044e00 | nr */
#define I2S_VOLUME 0x80044e20
#define I2S_FREQ   0x80044e21
#define I2S_DSIZE  0x80044e22
#define I2S_MODE   0x80044e23
#define I2S_START  0x80044e30
#define I2S_DRAIN  0x80044e31

/* Nominal ~666kHz sample clock (1.5us/sample before ~16x reference clock quirk) */
#define I2S_FREQ_CAP 0x00258000

static int s_i2s_tx_fd = -1;

/* the capture mode drops what it reads during a send (defined with the receive code) */
static void ir_rx_tx_begin(void);
static void ir_rx_tx_end(void);

int ir_i2s_init(void) {
    if (s_i2s_tx_fd >= 0) return 0;
    s_i2s_tx_fd = open(I2S_DEVICE, O_WRONLY);
    if (s_i2s_tx_fd < 0) return -1;
    if (ioctl(s_i2s_tx_fd, I2S_FREQ, I2S_FREQ_CAP) < 0) {
        close(s_i2s_tx_fd);
        s_i2s_tx_fd = -1;
        return -1;
    }
    return 0;
}

void ir_i2s_shutdown(void) {
    if (s_i2s_tx_fd >= 0) {
        close(s_i2s_tx_fd);
        s_i2s_tx_fd = -1;
    }
}

static inline uint32_t get_spp(uint32_t period_ns) {
    if (period_ns >= 12000) return 12;
    if (period_ns >= 3000)  return 10;
    if (period_ns >= 2501)  return 8;
    if (period_ns >= 2251)  return 4;
    return 2;
}

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    uint8_t cur;
    uint8_t mask;
} bit_stream_t;

static int bs_init(bit_stream_t *bs, size_t initial_cap) {
    bs->cap = initial_cap ? initial_cap : 4096;
    bs->len = 0;
    bs->buf = (uint8_t *)malloc(bs->cap);
    if (!bs->buf) return -1;
    bs->cur = 0;
    bs->mask = 0x80;
    return 0;
}

static inline void bs_push_bits(bit_stream_t *bs, int bit, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (bit) {
            bs->cur |= bs->mask;
        }
        bs->mask >>= 1;
        if (bs->mask == 0) {
            if (bs->len >= bs->cap) {
                size_t ncap = bs->cap * 2;
                uint8_t *nbuf = (uint8_t *)realloc(bs->buf, ncap);
                if (!nbuf) return;
                bs->buf = nbuf;
                bs->cap = ncap;
            }
            bs->buf[bs->len++] = bs->cur;
            bs->cur = 0;
            bs->mask = 0x80;
        }
    }
}

static void bs_finish(bit_stream_t *bs) {
    if (bs->mask != 0x80) {
        if (bs->len < bs->cap) {
            bs->buf[bs->len++] = bs->cur;
        }
    }
    /* Hardware consumes 16-bit samples; ensure even length */
    if (bs->len % 2 != 0) {
        if (bs->len < bs->cap) {
            bs->buf[bs->len++] = 0;
        }
    }
    /* Byte-swap and invert every 16-bit word (matches HAL hornet_i2s_write) */
    for (size_t i = 0; i + 1 < bs->len; i += 2) {
        uint8_t b0 = bs->buf[i];
        uint8_t b1 = bs->buf[i + 1];
        bs->buf[i]     = (uint8_t)~b1;
        bs->buf[i + 1] = (uint8_t)~b0;
    }
}

static int ir_i2s_blast_locked(const bit_stream_t *bsp, uint8_t port_mask, uint32_t stereo_clk, uint32_t pulse_bclk_ns);

/* Set while a stalled receive session is being closed in the background (see the receive
 * code below): the driver may hang in that close, so nothing else touches /dev/i2s meanwhile */
static volatile int s_rx_closing_stalled;

int ir_i2s_blast_words(uint32_t carrier_hz, uint8_t duty,
                       const uint16_t *words, size_t num_words,
                       uint8_t port_mask) {
    if (!words || num_words == 0) return -1;
    if (s_rx_closing_stalled) return -EBUSY; /* a stalled receiver close may hang the driver */
    if (carrier_hz == 0) carrier_hz = 38000;
    if (duty < 1) duty = 1;
    if (duty > 50) duty = 50;

    uint32_t period_ns = 1000000000U / carrier_hz;
    uint32_t spp = get_spp(period_ns);
    uint32_t pulse_bclk_ns = (period_ns + spp - 1) / spp;
    uint32_t stereo_clk = (uint32_t)(((uint64_t)pulse_bclk_ns * 65536ULL) / 40ULL);

    uint32_t on_bits = ((uint32_t)duty * spp + 99) / 100;
    uint32_t off_bits = spp - on_bits;

    bit_stream_t bs;
    if (bs_init(&bs, 8192) != 0) return -2;

    for (size_t i = 0; i < num_words; i++) {
        uint16_t w = words[i];
        int is_mark = (w & 0x8000) != 0;
        uint32_t us = (w & 0x7FFF);

        uint32_t cycles = (uint32_t)(((uint64_t)us * 1000ULL + ((uint64_t)period_ns / 2ULL)) / (uint64_t)period_ns);
        for (uint32_t c = 0; c < cycles; c++) {
            if (is_mark) {
                bs_push_bits(&bs, 1, on_bits);
                bs_push_bits(&bs, 0, off_bits);
            } else {
                bs_push_bits(&bs, 0, spp);
            }
        }
    }

    /* Append trailing silence (idle level) to guarantee DMA FIFO flushes on quiet level */
    bs_push_bits(&bs, 0, spp * 500);
    bs_finish(&bs);

    int lock_fd = i2s_lock_acquire();
    if (lock_fd < 0) {
        free(bs.buf);
        return -EBUSY;
    }
    if (s_rx_closing_stalled) { /* a capture stalled while this send waited for the lock */
        i2s_lock_release(lock_fd);
        free(bs.buf);
        return -EBUSY;
    }
    ir_rx_tx_begin();
    int rc = ir_i2s_blast_locked(&bs, port_mask, stereo_clk, pulse_bclk_ns);
    ir_rx_tx_end();
    i2s_lock_release(lock_fd);
    free(bs.buf);
    return rc;
}

/* The send itself, with the I2S lock held: 0 or -3..-6 */
static int ir_i2s_blast_locked(const bit_stream_t *bsp, uint8_t port_mask, uint32_t stereo_clk, uint32_t pulse_bclk_ns) {
    const bit_stream_t bs = *bsp;
    if (s_i2s_tx_fd < 0) {
        s_i2s_tx_fd = open(I2S_DEVICE, O_WRONLY);
    }
    int fd = s_i2s_tx_fd;
    if (fd < 0) {
        return -3;
    }

    if (ioctl(fd, I2S_FREQ, (int)stereo_clk) < 0) {
        close(s_i2s_tx_fd);
        s_i2s_tx_fd = -1;
        return -4;
    }

    uint32_t samples = (uint32_t)(bs.len / 2);
    uint32_t duration_us = (uint32_t)(((uint64_t)samples * (uint64_t)pulse_bclk_ns) / 1000ULL);
    if (duration_us < 25000) duration_us = 25000;
    if (duration_us > 1500000) duration_us = 1500000;

    ssize_t written = write(fd, bs.buf, bs.len);
    if (written != (ssize_t)bs.len) {
        close(fd);
        s_i2s_tx_fd = -1;
        return -5;
    }

    uint32_t sel = (port_mask & 0x07);
    if (sel == 0) sel = IR_I2S_EMITTER_ALL;

    int start_arg = (int)(((sel & 0xffff) << 16) | ((uint32_t)bs.len & 0xffff));
    if (ioctl(fd, I2S_START, start_arg) < 0) {
        close(fd);
        s_i2s_tx_fd = -1;
        return -6;
    }

    /* Wait for transmission duration calculated from sample rate and word count */
    usleep(duration_us + 20000);

    /* Complete transmission and stop DMA ring loop (matches stock hal hornet_i2s_write_complete).
     * DO NOT call close(fd): ath_i2s_close enters an infinite while (desc->own) loop locking CPU core. */
    ioctl(fd, I2S_DRAIN, 0);

    return 0;
}

int ir_i2s_blast_blob(const uint8_t *blob, size_t blob_len, uint8_t port_mask) {
    if (!blob || blob_len < 16) return -1;

    /* Parse 16-byte Logitech IR header */
    uint32_t period_ns = ((uint32_t)blob[1] << 24) |
                         ((uint32_t)blob[2] << 16) |
                         ((uint32_t)blob[3] << 8)  |
                         ((uint32_t)blob[4]);
    uint8_t duty = blob[5];
    uint16_t pre_silence_us = ((uint16_t)blob[6] << 8) | (uint16_t)blob[7];
    uint16_t start_loc = ((uint16_t)blob[10] << 8) | (uint16_t)blob[11];
    if (start_loc < 16 || start_loc + 2 > blob_len) start_loc = 16;
    uint16_t word_count = ((uint16_t)blob[start_loc] << 8) | (uint16_t)blob[start_loc + 1];

    size_t words_offset = start_loc + 2;
    if (words_offset >= blob_len) return -2;

    size_t actual_words = (blob_len - words_offset) / 2;
    if (word_count > 0 && word_count < actual_words) actual_words = word_count;
    if (actual_words == 0) return -2;

    uint32_t carrier_hz = period_ns ? (1000000000U / period_ns) : 38000;
    if (duty < 1 || duty > 50) duty = 50;

    uint16_t *words = (uint16_t *)malloc((actual_words + 3) * sizeof(uint16_t));
    if (!words) return -3;

    size_t w_idx = 0;
    if (pre_silence_us > 0) {
        words[w_idx++] = (pre_silence_us & 0x7FFF); /* Space */
    }

    for (size_t i = 0; i < actual_words; i++) {
        size_t off = words_offset + (i * 2);
        uint16_t w = ((uint16_t)blob[off] << 8) | (uint16_t)blob[off + 1];
        words[w_idx++] = w;
    }

    /* Ensure waveform terminates with silence so carrier ceases */
    if (w_idx > 0 && (words[w_idx - 1] & 0x8000)) {
        words[w_idx++] = (45000 & 0x7FFF); /* Trailing space: 45ms */
    }

    int rc = ir_i2s_blast_words(carrier_hz, duty, words, w_idx, port_mask);
    free(words);
    return rc;
}

/*
 * IR receive (learning).
 *
 * Measured on a Harmony Hub (ath_i2s, fw 4.15.600) and matching stock hal:
 * - /dev/i2s opened read-only delivers the IR sensor's raw output, carrier
 *   included, one bit per 1.5 us (83,334 bytes/s; stock's IR table says 1500 ns).
 * - read() copies whole 768-byte DMA buffers whatever the count, so every read
 *   asks for a multiple of 768 into a buffer that size. The first read only
 *   starts the DMA and returns 0; later reads block until a buffer is ready
 *   (O_NONBLOCK is ignored).
 * - Stock inverts each byte and swaps the two bytes of every 16-bit sample;
 *   after that a 1 bit is light. Without the swap the carrier periods scatter.
 * - The sensor misses the start of the first burst, so the first frame of a
 *   press is often cut. The decoder keeps the first complete frame after a
 *   frame gap and falls back to the first frame for a very short press.
 */
#define IR_RX_SAMPLE_NS      1500u
#define IR_RX_BLOCK          768u
#define IR_RX_READ_BLOCKS    12u
#define IR_RX_BURST_GAP      60u      /* no light for 90 us ends a carrier burst */
#define IR_RX_MAX_PERIOD     28u      /* carrier periods up to 42 us (24 kHz); longer = a missed pulse */
#define IR_RX_FRAME_GAP_US   20000u   /* a space this long ends a frame */
#define IR_RX_DEFAULT_GAP_US 40000u   /* trailing space when the next frame was not seen */
#define IR_RX_MAX_GAP_US     100000u
#define IR_RX_RECORD_US      400000u  /* recorded after the first light */
#define IR_RX_QUIET_US       150000u  /* recording stops after this long without light */
#define IR_RX_MAX_TIMINGS    600u

static size_t ir_rx_bytes_for_us(unsigned long us) {
    return (size_t)(((unsigned long long)us * 1000u) / (IR_RX_SAMPLE_NS * 8u));
}

static int ir_rx_block_lit(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0xFF) return 1;
    }
    return 0;
}

/*
 * Stall guard. A read normally returns every ~9 ms, but once (2026-11-08, never
 * reproduced in 60+ sessions since) the receive DMA stopped: read() slept in
 * the driver for ever, and so did every later session until a reboot. The
 * driver's read sleeps interruptibly and returns 0 on a signal, so a watchdog
 * thread signals the capture thread when one read takes over 2 s. The driver's
 * close() waits for the DMA to hand back its buffers, which a dead DMA never
 * does, so the close of a stalled session runs in a detached thread; until it
 * returns, captures fail at once instead of queueing behind it.
 */
#define IR_RX_STALL_MS 2000


typedef struct {
    pthread_t target;
    volatile int done;
    volatile int in_read;
    volatile long long read_started_ms;
    volatile int fired;
} ir_rx_watch_t;

static long long ir_rx_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void ir_rx_wake(int sig) {
    (void)sig;   /* only interrupts the driver's sleep */
}

static void *ir_rx_watch(void *arg) {
    ir_rx_watch_t *w = (ir_rx_watch_t *)arg;
    long long last_kick = 0;
    while (!w->done) {
        usleep(100000);
        long long now = ir_rx_now_ms();
        if (w->in_read && now - w->read_started_ms > IR_RX_STALL_MS && now - last_kick >= 1000) {
            w->fired = 1;
            last_kick = now;
            pthread_kill(w->target, SIGUSR2);
        }
    }
    return NULL;
}

static void *ir_rx_close_stalled(void *arg) {
    int fd = (int)(long)arg;
    close(fd);   /* may never return if the DMA is dead */
    s_rx_closing_stalled = 0;
    fprintf(stderr, "[*] IR: stalled receive session closed\n");
    return NULL;
}

static void ir_rx_install_wake_handler(void) {
    static int handler_set = 0;
    if (handler_set) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ir_rx_wake;   /* no SA_RESTART: the read must return */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    handler_set = 1;
}

/* ---- Receive session: open with the setup ioctls, reads under the stall watchdog, close ---- */

typedef struct {
    int fd;
    uint8_t *chunk;              /* IR_RX_BLOCK * IR_RX_READ_BLOCKS: the driver copies whole buffers */
    ir_rx_watch_t watch;
    pthread_t watch_thread;
    int watching;
    int stalled;
} ir_rx_session_t;

#define IR_RX_CHUNK_LEN (IR_RX_BLOCK * IR_RX_READ_BLOCKS)

/* Opens the receiver; the calling thread is the one the watchdog wakes, so it must be the one
 * that reads. Returns 0, -2 (open), -3 (ioctls), -4 (memory) or -7 (a stalled close pending). */
static int ir_rx_session_open(ir_rx_session_t *s, char *err, size_t errlen) {
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    if (s_rx_closing_stalled) {
        snprintf(err, errlen, "IR receiver is not responding (a stalled capture has not closed yet); reboot the Hub if this persists");
        return -7;
    }
    ir_rx_install_wake_handler();
    s->chunk = (uint8_t *)malloc(IR_RX_CHUNK_LEN);
    if (!s->chunk) {
        snprintf(err, errlen, "Out of memory allocating capture buffer");
        return -4;
    }
    /* The transmit handle stays open: receive and transmit coexist (measured on a Hub), and
     * closing the transmit side can hang in ath_i2s_close (see ir_i2s_blast_words). */
    s->fd = open(I2S_DEVICE, O_RDONLY);
    if (s->fd < 0) {
        snprintf(err, errlen, "Failed to open " I2S_DEVICE ": %s", strerror(errno));
        free(s->chunk);
        s->chunk = NULL;
        return -2;
    }
    if (ioctl(s->fd, I2S_DSIZE, 16) < 0 ||
        ioctl(s->fd, I2S_MODE, 2) < 0 ||
        ioctl(s->fd, I2S_VOLUME, 15) < 0 ||
        ioctl(s->fd, I2S_FREQ, I2S_FREQ_CAP) < 0) {
        snprintf(err, errlen, "Failed to configure I2S RX ioctls: %s", strerror(errno));
        close(s->fd);
        s->fd = -1;
        free(s->chunk);
        s->chunk = NULL;
        return -3;
    }
    ioctl(s->fd, I2S_START, 0);
    s->watch.target = pthread_self();
    s->watching = pthread_create(&s->watch_thread, NULL, ir_rx_watch, &s->watch) == 0;
    return 0;
}

/* One read of whole buffers into s->chunk: bytes read, 0 for none yet (the first read only
 * starts the DMA), -1 on a read error, -7 when the watchdog found the DMA stalled. */
static ssize_t ir_rx_session_read(ir_rx_session_t *s) {
    s->watch.read_started_ms = ir_rx_now_ms();
    s->watch.in_read = 1;
    ssize_t n = read(s->fd, s->chunk, IR_RX_CHUNK_LEN);
    s->watch.in_read = 0;
    if (s->watch.fired) {
        s->stalled = 1;
        return -7;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) n = 0;
    return n < 0 ? -1 : n;
}

static void ir_rx_session_close(ir_rx_session_t *s) {
    if (s->watching) {
        s->watch.done = 1;
        pthread_join(s->watch_thread, NULL);
        s->watching = 0;
    }
    if (s->fd >= 0) {
        if (s->stalled) {
            pthread_t closer;
            fprintf(stderr, "[!] IR: receive DMA stalled (no data for %d ms)\n", IR_RX_STALL_MS);
            s_rx_closing_stalled = 1;
            if (pthread_create(&closer, NULL, ir_rx_close_stalled, (void *)(long)s->fd) == 0) {
                pthread_detach(closer);
            } else {
                s_rx_closing_stalled = 0;
                close(s->fd);
            }
        } else {
            close(s->fd);
        }
        s->fd = -1;
    }
    free(s->chunk);
    s->chunk = NULL;
}

/* Waits for light until deadline_ms, then records until IR_RX_QUIET_US without light or
 * IR_RX_RECORD_US after the first light, and decodes. stop (may be NULL) is asked before each
 * read; a 1 ends the wait or the recording. Returns 0 (out holds the raw string, *measured_hz
 * the carrier or 0), -5 no light before the deadline, -6 light that is not a code, -7 stalled,
 * -1 read error, -4 memory, -9 stopped. */
static int ir_rx_record(ir_rx_session_t *s, char *out, size_t outlen, uint32_t *measured_hz,
                        long long deadline_ms, int (*stop)(void)) {
    const size_t record_len = ir_rx_bytes_for_us(IR_RX_RECORD_US);
    const size_t quiet_len = ir_rx_bytes_for_us(IR_RX_QUIET_US);
    const size_t cap = IR_RX_CHUNK_LEN + record_len + IR_RX_CHUNK_LEN;
    uint8_t *prev = (uint8_t *)malloc(IR_RX_CHUNK_LEN);
    uint8_t *raw_buf = (uint8_t *)malloc(cap);
    if (!prev || !raw_buf) {
        free(prev);
        free(raw_buf);
        snprintf(out, outlen, "Out of memory allocating capture buffer");
        return -4;
    }
    size_t raw_len = 0, prev_len = 0, after_light = 0, since_light = 0;
    int active = 0, rc = 0;

    while (1) {
        if (stop && stop()) { rc = -9; break; }
        long long now = ir_rx_now_ms();
        if (!active && now >= deadline_ms) { rc = -5; break; }
        /* the recording itself is bounded by size; this only guards a stalled DMA */
        if (active && now >= deadline_ms + 2000) break;

        ssize_t n = ir_rx_session_read(s);
        if (n == -7) { rc = -7; break; }
        if (n < 0) { rc = -1; break; }
        if (n == 0) {
            /* the first read only starts the DMA; after that the driver blocks until a
             * buffer is ready, so a 0 once recording means the end (the test mock's EOF) */
            if (active) break;
            usleep(2000);
            continue;
        }
        int lit = ir_rx_block_lit(s->chunk, (size_t)n);
        if (!active) {
            if (!lit) {
                memcpy(prev, s->chunk, (size_t)n);
                prev_len = (size_t)n;
                continue;
            }
            active = 1;
            memcpy(raw_buf, prev, prev_len);   /* one quiet buffer before the light */
            raw_len = prev_len;
        }
        size_t c = (size_t)n;
        if (raw_len + c > cap) c = cap - raw_len;
        memcpy(raw_buf + raw_len, s->chunk, c);
        raw_len += c;
        after_light += (size_t)n;
        since_light = lit ? 0 : since_light + (size_t)n;
        if (after_light >= record_len || since_light >= quiet_len || raw_len >= cap) break;
    }
    free(prev);
    if (rc == 0 && !active) rc = -5;
    if (rc == 0) {
        rc = ir_i2s_decode_capture_ex(raw_buf, raw_len, out, outlen, measured_hz);
    } else if (rc == -5) {
        snprintf(out, outlen, "No IR signal received: timeout waiting for remote button press");
    } else if (rc == -7) {
        snprintf(out, outlen, "IR receiver stopped delivering data; try again, and reboot the Hub if it keeps failing");
    } else if (rc == -1) {
        snprintf(out, outlen, "IR receiver read failed: %s", strerror(errno));
    }
    free(raw_buf);
    return rc;
}

/* Reads until the line has been dark for IR_RX_QUIET_US (a held button keeps it busy), so one
 * press gives one signal. Returns 0, -7 stalled, -1 read error, -9 stopped. */
static int ir_rx_wait_quiet(ir_rx_session_t *s, int (*stop)(void)) {
    const size_t quiet_len = ir_rx_bytes_for_us(IR_RX_QUIET_US);
    size_t dark = 0;
    while (dark < quiet_len) {
        if (stop && stop()) return -9;
        ssize_t n = ir_rx_session_read(s);
        if (n == -7) return -7;
        if (n < 0) return -1;
        if (n == 0) return 0;   /* nothing more (the test mock's EOF) */
        dark = ir_rx_block_lit(s->chunk, (size_t)n) ? 0 : dark + (size_t)n;
    }
    return 0;
}

/* ---- Capture mode (spec 3.14) and the one-shot capture exclude each other ---- */

static pthread_mutex_t s_rx_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_rx_cond = PTHREAD_COND_INITIALIZER;
static int s_oneshot_running = 0;
static int s_mode_on = 0;                 /* the mode thread exists */
static volatile int s_mode_stop = 0;
static int s_mode_open_rc = 1;            /* 1 = the thread has not reported its open yet */
static char s_mode_open_err[160];
static char s_mode_error[160];            /* last problem of a running mode, "" when fine */
static pthread_t s_mode_thread;
static ir_rx_signal_cb s_mode_cb = NULL;
static void *s_mode_user = NULL;

/* Sends (ir_i2s_blast_words) while the mode listens: the receiver sees the Hub's own blaster,
 * and a send reprograms the shared I2S clock for its carrier (the receiver then samples at
 * ~2.1 us instead of 1.5 us until its clock is set again; measured on a Hub). The mode thread
 * drops what it reads during a send and IR_RX_TX_GUARD_MS after it, then sets its clock again. */
#define IR_RX_TX_GUARD_MS 100
static volatile int s_tx_active = 0;
static volatile long long s_rx_discard_until_ms = 0;
static volatile int s_rx_refreq = 0;

static void ir_rx_tx_begin(void) {
    s_tx_active = 1;
}

static void ir_rx_tx_end(void) {
    s_rx_discard_until_ms = ir_rx_now_ms() + IR_RX_TX_GUARD_MS;
    s_rx_refreq = 1;
    s_tx_active = 0;
}

static int ir_mode_should_stop(void) {
    return s_mode_stop || s_tx_active;
}

static ir_capture_state_cb s_state_cb = NULL;
static void *s_state_user = NULL;

void ir_i2s_set_capture_state_listener(ir_capture_state_cb cb, void *user) {
    pthread_mutex_lock(&s_rx_lock);
    s_state_cb = cb;
    s_state_user = user;
    pthread_mutex_unlock(&s_rx_lock);
}

/* s_rx_lock must not be held */
static void ir_mode_notify(void) {
    pthread_mutex_lock(&s_rx_lock);
    ir_capture_state_cb cb = s_state_cb;
    void *user = s_state_user;
    pthread_mutex_unlock(&s_rx_lock);
    if (cb) cb(user);
}

static void ir_mode_set_error(const char *msg) {
    if (!msg) msg = "";
    pthread_mutex_lock(&s_rx_lock);
    int changed = strncmp(s_mode_error, msg, sizeof(s_mode_error) - 1) != 0;
    snprintf(s_mode_error, sizeof(s_mode_error), "%s", msg);
    int on = s_mode_on;
    pthread_mutex_unlock(&s_rx_lock);
    if (changed && on) ir_mode_notify();
}

static void *ir_mode_thread(void *arg) {
    (void)arg;
    char out[8192];
    int first = 1;
    while (!s_mode_stop) {
        ir_rx_session_t s;
        char err[160] = {0};
        int orc = ir_rx_session_open(&s, err, sizeof(err));
        if (first) {
            pthread_mutex_lock(&s_rx_lock);
            s_mode_open_rc = orc;
            snprintf(s_mode_open_err, sizeof(s_mode_open_err), "%s", err);
            pthread_cond_broadcast(&s_rx_cond);
            pthread_mutex_unlock(&s_rx_lock);
            first = 0;
            if (orc != 0) return NULL;   /* the enable request reports it */
        }
        if (orc != 0) {
            ir_mode_set_error(err);
            for (int i = 0; i < 50 && !s_mode_stop; i++) usleep(100000);   /* retry in 5 s */
            continue;
        }
        ir_mode_set_error("");
        fprintf(stderr, "[*] IR: capture mode listening\n");
        int rc = 0;
        while (!s_mode_stop) {
            if (s_tx_active || ir_rx_now_ms() < s_rx_discard_until_ms) {
                if (!s_tx_active && s_rx_refreq) {
                    ioctl(s.fd, I2S_FREQ, I2S_FREQ_CAP);   /* the send left its own clock */
                    s_rx_refreq = 0;
                }
                ssize_t n = ir_rx_session_read(&s);   /* dropped: our own blaster */
                if (n == -7 || n < 0) { rc = (int)n; break; }
                if (n == 0) usleep(2000);
                continue;
            }
            if (s_rx_refreq) {
                ioctl(s.fd, I2S_FREQ, I2S_FREQ_CAP);
                s_rx_refreq = 0;
            }
            uint32_t hz = 0;
            rc = ir_rx_record(&s, out, sizeof(out), &hz, ir_rx_now_ms() + 1000, ir_mode_should_stop);
            if (rc == 0) {
                /* the listener pair is replaced under the lock: take both together */
                pthread_mutex_lock(&s_rx_lock);
                ir_rx_signal_cb cb = s_mode_cb;
                void *user = s_mode_user;
                pthread_mutex_unlock(&s_rx_lock);
                if (cb) cb(out, hz, user);
            }
            if (rc == 0 || rc == -6) {
                rc = ir_rx_wait_quiet(&s, ir_mode_should_stop);
            }
            if (rc == -7 || rc == -1 || rc == -4) break;
            rc = 0;   /* -5 (nothing yet) and -9 (a send or stop) just go round */
        }
        ir_rx_session_close(&s);
        if (rc == -7) {
            ir_mode_set_error("IR receiver stopped delivering data (DMA stalled); reboot the Hub if it does not recover");
        } else if (rc == -1 || rc == -4) {
            ir_mode_set_error("IR receiver read failed");
        }
        if (rc != 0 && !s_mode_stop) {
            for (int i = 0; i < 50 && !s_mode_stop; i++) usleep(100000);   /* reopen in 5 s */
        }
    }
    fprintf(stderr, "[*] IR: capture mode stopped\n");
    return NULL;
}

int ir_i2s_capture_mode_set(int enable, ir_rx_signal_cb cb, void *user, char *err, size_t errlen) {
    if (err && errlen) err[0] = '\0';
    pthread_mutex_lock(&s_rx_lock);
    if (!enable) {
        if (!s_mode_on) {
            pthread_mutex_unlock(&s_rx_lock);
            return 0;
        }
        s_mode_stop = 1;
        pthread_t t = s_mode_thread;
        pthread_mutex_unlock(&s_rx_lock);
        pthread_join(t, NULL);   /* a read returns within ~10 ms, a stalled one within 3 s */
        pthread_mutex_lock(&s_rx_lock);
        s_mode_on = 0;
        s_mode_error[0] = '\0';
        pthread_mutex_unlock(&s_rx_lock);
        ir_mode_notify();
        return 0;
    }
    if (s_mode_on) {
        s_mode_cb = cb;
        s_mode_user = user;
        pthread_mutex_unlock(&s_rx_lock);
        return 0;
    }
    if (s_oneshot_running) {
        pthread_mutex_unlock(&s_rx_lock);
        if (err) snprintf(err, errlen, "An IR capture is running");
        return -EBUSY;
    }
    s_mode_cb = cb;
    s_mode_user = user;
    s_mode_stop = 0;
    s_mode_open_rc = 1;
    s_mode_error[0] = '\0';
    if (pthread_create(&s_mode_thread, NULL, ir_mode_thread, NULL) != 0) {
        pthread_mutex_unlock(&s_rx_lock);
        if (err) snprintf(err, errlen, "Could not start the capture thread");
        return -4;
    }
    while (s_mode_open_rc == 1) pthread_cond_wait(&s_rx_cond, &s_rx_lock);
    int rc = s_mode_open_rc;
    if (rc != 0) {
        if (err) snprintf(err, errlen, "%s", s_mode_open_err);
        pthread_mutex_unlock(&s_rx_lock);
        pthread_join(s_mode_thread, NULL);
        return rc;
    }
    s_mode_on = 1;
    pthread_mutex_unlock(&s_rx_lock);
    ir_mode_notify();
    return 0;
}

int ir_i2s_capture_mode_enabled(void) {
    pthread_mutex_lock(&s_rx_lock);
    int on = s_mode_on;
    pthread_mutex_unlock(&s_rx_lock);
    return on;
}

int ir_i2s_capture_mode_error(char *buf, size_t len) {
    pthread_mutex_lock(&s_rx_lock);
    int has = s_mode_on && s_mode_error[0];
    if (buf && len) snprintf(buf, len, "%s", has ? s_mode_error : "");
    pthread_mutex_unlock(&s_rx_lock);
    return has;
}

int ir_i2s_capture(char *out, size_t outlen, unsigned int timeout_sec) {
    if (!out || outlen < 32) return -1;
    out[0] = '\0';
    if (timeout_sec == 0) timeout_sec = 5;

    pthread_mutex_lock(&s_rx_lock);
    if (s_mode_on) {
        pthread_mutex_unlock(&s_rx_lock);
        snprintf(out, outlen, "IR capture mode is on: learned signals arrive as ir_received events");
        return -11;
    }
    s_oneshot_running = 1;
    pthread_mutex_unlock(&s_rx_lock);

    int lock_fd = i2s_lock_acquire();
    if (lock_fd < 0) {
        snprintf(out, outlen, "I2S hardware is busy (locked)");
        pthread_mutex_lock(&s_rx_lock);
        s_oneshot_running = 0;
        pthread_mutex_unlock(&s_rx_lock);
        return -EBUSY;
    }
    ir_rx_session_t s;
    int rc = ir_rx_session_open(&s, out, outlen);
    if (rc == 0) {
        uint32_t hz = 0;
        rc = ir_rx_record(&s, out, outlen, &hz, ir_rx_now_ms() + (long long)timeout_sec * 1000, NULL);
        ir_rx_session_close(&s);
    }
    i2s_lock_release(lock_fd);
    pthread_mutex_lock(&s_rx_lock);
    s_oneshot_running = 0;
    pthread_mutex_unlock(&s_rx_lock);
    return rc;
}

static inline int ir_rx_light(const uint8_t *raw_buf, size_t bit) {
    /* stock order: byte pairs swapped, inverted, MSB first */
    uint8_t byte = (uint8_t)~raw_buf[(bit / 8) ^ 1u];
    return (byte >> (7 - (bit % 8))) & 1;
}

static uint32_t ir_rx_us(size_t samples) {
    return (uint32_t)(((unsigned long long)samples * IR_RX_SAMPLE_NS + 500u) / 1000u);
}

int ir_i2s_decode_capture(const uint8_t *raw_buf, size_t raw_len, char *out, size_t outlen) {
    return ir_i2s_decode_capture_ex(raw_buf, raw_len, out, outlen, NULL);
}

int ir_i2s_decode_capture_ex(const uint8_t *raw_buf, size_t raw_len, char *out, size_t outlen, uint32_t *measured_hz) {
    if (measured_hz) *measured_hz = 0;
    if (!raw_buf || raw_len < 2 || !out || outlen < 32) return -1;
    out[0] = '\0';
    raw_len &= ~(size_t)1;   /* whole 16-bit samples */

    uint32_t *t = (uint32_t *)malloc(IR_RX_MAX_TIMINGS * sizeof(uint32_t));
    if (!t) {
        snprintf(out, outlen, "Out of memory allocating timing buffer");
        return -4;
    }

    /* Envelope: marks and spaces in samples, alternating, starting with a mark */
    size_t n = 0, bits = raw_len * 8;
    size_t burst_start = 0, last_light = 0;
    int in_signal = 0;
    unsigned long long period_sum = 0;
    unsigned long period_n = 0;
    size_t last_rise = 0, on_start = 0;
    unsigned long long on_sum = 0;
    unsigned long on_n = 0;
    int prev = 0;
    for (size_t i = 0; i < bits && n + 2 < IR_RX_MAX_TIMINGS; i++) {
        int v = ir_rx_light(raw_buf, i);
        if (v) {
            if (!in_signal) {
                in_signal = 1;
                burst_start = i;
            } else if (i - last_light - 1 > IR_RX_BURST_GAP) {
                t[n++] = (uint32_t)(last_light + 1 - burst_start);
                t[n++] = (uint32_t)(i - last_light - 1);
                burst_start = i;
            }
            if (!prev) {
                if (last_rise && i - last_rise <= IR_RX_MAX_PERIOD) {
                    period_sum += i - last_rise;
                    period_n++;
                }
                last_rise = i;
                on_start = i;
            }
            last_light = i;
        } else if (prev && i - on_start <= IR_RX_MAX_PERIOD) {
            on_sum += i - on_start;   /* one carrier pulse, light samples */
            on_n++;
        }
        prev = v;
    }
    if (!in_signal) {
        free(t);
        snprintf(out, outlen, "No IR signal received: timeout waiting for remote button press");
        return -5;
    }
    t[n++] = (uint32_t)(last_light + 1 - burst_start);
    /* the quiet tail after the last burst */
    size_t tail = bits - last_light - 1;

    /* Frames end at a space of 20 ms or more. Keep the first complete frame after
     * a frame gap; with a single frame keep that one. */
    size_t frame_samples = (size_t)((unsigned long long)IR_RX_FRAME_GAP_US * 1000u / IR_RX_SAMPLE_NS);
    size_t f_start = 0, f_end = n;   /* [f_start, f_end) timings, f_end at a mark */
    size_t first_gap = n;
    for (size_t k = 1; k < n; k += 2) {
        if (t[k] >= frame_samples) { first_gap = k; break; }
    }
    if (first_gap < n) {
        size_t second_gap = n;
        for (size_t k = first_gap + 2; k < n; k += 2) {
            if (t[k] >= frame_samples) { second_gap = k; break; }
        }
        if (second_gap < n || tail >= frame_samples) {
            f_start = first_gap + 1;
            f_end = second_gap;
        } else {
            f_end = first_gap;
        }
    }
    /* trailing space: the measured frame gap, else a default */
    uint32_t gap_us = IR_RX_DEFAULT_GAP_US;
    if (f_end < n) gap_us = ir_rx_us(t[f_end]);
    else if (first_gap < n) gap_us = ir_rx_us(t[first_gap]);
    if (gap_us > IR_RX_MAX_GAP_US) gap_us = IR_RX_MAX_GAP_US;
    if (gap_us < IR_RX_FRAME_GAP_US) gap_us = IR_RX_FRAME_GAP_US;

    size_t pulses = f_end - f_start;   /* odd: mark, space, ..., mark */
    if (pulses < 3) {
        free(t);
        snprintf(out, outlen, "No valid IR signal detected (only %zu marks); hold the button closer to the Hub", (pulses + 1) / 2);
        return -6;
    }

    uint32_t carrier = 38000;
    uint32_t comp_us = 0;
    if (period_n >= 8) {
        unsigned long long hz = 1000000000ULL * period_n / (period_sum * IR_RX_SAMPLE_NS);
        if (hz >= 20000 && hz <= 60000) {
            carrier = (uint32_t)((hz + 50) / 100 * 100);
            if (measured_hz) *measured_hz = carrier;
        }
        /* A burst is measured from its first to its last light sample, so the
         * last carrier pulse's dark part counts as space: move it to the mark. */
        if (on_n >= 8) {
            unsigned long long per_ns = period_sum * IR_RX_SAMPLE_NS / period_n;
            unsigned long long on_ns = on_sum * IR_RX_SAMPLE_NS / on_n;
            if (per_ns > on_ns) comp_us = (uint32_t)((per_ns - on_ns + 500) / 1000);
            if (comp_us > 30) comp_us = 0;
        }
    }

    char *p = out;
    size_t rem = outlen;
    int w = snprintf(p, rem, "F%X", carrier);
    if (w < 0 || (size_t)w >= rem) { free(t); return -1; }
    p += w; rem -= (size_t)w;
    for (size_t k = f_start; k <= f_end && k < n + 1; k++) {
        uint32_t us = (k == f_end) ? gap_us : ir_rx_us(t[k]);
        char kind = ((k - f_start) % 2 == 0) ? 'P' : 'S';
        if (kind == 'P') us += comp_us;
        else if (k != f_end && us > comp_us) us -= comp_us;
        w = snprintf(p, rem, "%c%X", kind, us);
        if (w < 0 || (size_t)w >= rem) {
            free(t);
            snprintf(out, outlen, "Captured signal too long for the output buffer");
            return -1;
        }
        p += w; rem -= (size_t)w;
    }
    free(t);
    return 0;
}
