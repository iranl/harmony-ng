#include "ir_i2s.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

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

int ir_i2s_blast_words(uint32_t carrier_hz, uint8_t duty,
                       const uint16_t *words, size_t num_words,
                       uint8_t port_mask) {
    if (!words || num_words == 0) return -1;
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

    if (s_i2s_tx_fd < 0) {
        s_i2s_tx_fd = open(I2S_DEVICE, O_WRONLY);
    }
    int fd = s_i2s_tx_fd;
    if (fd < 0) {
        i2s_lock_release(lock_fd);
        free(bs.buf);
        return -3;
    }

    if (ioctl(fd, I2S_FREQ, (int)stereo_clk) < 0) {
        close(s_i2s_tx_fd);
        s_i2s_tx_fd = -1;
        i2s_lock_release(lock_fd);
        free(bs.buf);
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
        i2s_lock_release(lock_fd);
        free(bs.buf);
        return -5;
    }

    uint32_t sel = (port_mask & 0x07);
    if (sel == 0) sel = IR_I2S_EMITTER_ALL;

    int start_arg = (int)(((sel & 0xffff) << 16) | ((uint32_t)bs.len & 0xffff));
    if (ioctl(fd, I2S_START, start_arg) < 0) {
        close(fd);
        s_i2s_tx_fd = -1;
        i2s_lock_release(lock_fd);
        free(bs.buf);
        return -6;
    }

    /* Wait for transmission duration calculated from sample rate and word count */
    usleep(duration_us + 20000);

    /* Complete transmission and stop DMA ring loop (matches stock hal hornet_i2s_write_complete).
     * DO NOT call close(fd): ath_i2s_close enters an infinite while (desc->own) loop locking CPU core. */
    ioctl(fd, I2S_DRAIN, 0);

    i2s_lock_release(lock_fd);
    free(bs.buf);
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

int ir_i2s_capture(char *out, size_t outlen, unsigned int timeout_sec) {
    if (!out || outlen < 32) return -1;
    out[0] = '\0';
    if (timeout_sec == 0) timeout_sec = 5;

    int lock_fd = i2s_lock_acquire();
    if (lock_fd < 0) {
        snprintf(out, outlen, "I2S hardware is busy (locked)");
        return -EBUSY;
    }

    /* Release persistent TX handle before switching to RX capture */
    ir_i2s_shutdown();

    int fd = open(I2S_DEVICE, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fd = open(I2S_DEVICE, O_RDONLY);
    }
    if (fd < 0) {
        snprintf(out, outlen, "Failed to open " I2S_DEVICE ": %s", strerror(errno));
        i2s_lock_release(lock_fd);
        return -2;
    }

    if (ioctl(fd, I2S_DSIZE, 16) < 0 ||
        ioctl(fd, I2S_MODE, 2) < 0 ||
        ioctl(fd, I2S_VOLUME, 15) < 0 ||
        ioctl(fd, I2S_FREQ, I2S_FREQ_CAP) < 0) {
        snprintf(out, outlen, "Failed to configure I2S RX ioctls: %s", strerror(errno));
        close(fd);
        i2s_lock_release(lock_fd);
        return -3;
    }

    /* Kick RX DMA (best-effort) */
    ioctl(fd, I2S_START, 0);

    const size_t cap = 64 * 1024;
    uint8_t *raw_buf = (uint8_t *)malloc(cap);
    if (!raw_buf) {
        close(fd);
        i2s_lock_release(lock_fd);
        snprintf(out, outlen, "Out of memory allocating capture buffer");
        return -4;
    }
    size_t raw_len = 0;

    struct timeval start_tv, now_tv, act_tv;
    gettimeofday(&start_tv, NULL);
    int active = 0;
    uint8_t chunk[192];

    while (1) {
        gettimeofday(&now_tv, NULL);
        long elapsed_ms = (now_tv.tv_sec - start_tv.tv_sec) * 1000 +
                          (now_tv.tv_usec - start_tv.tv_usec) / 1000;

        if (!active && elapsed_ms >= (long)timeout_sec * 1000) {
            close(fd);
            i2s_lock_release(lock_fd);
            free(raw_buf);
            snprintf(out, outlen, "No IR signal received: timeout waiting for remote button press");
            return -5;
        }

        if (active) {
            long act_ms = (now_tv.tv_sec - act_tv.tv_sec) * 1000 +
                          (now_tv.tv_usec - act_tv.tv_usec) / 1000;
            if (act_ms >= 300 || raw_len >= cap - sizeof(chunk)) {
                break;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0) {
            continue;
        }

        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                continue;
            }
            break;
        }

        /* Invert every byte: TSOP demodulator line is active-low */
        for (ssize_t i = 0; i < n; i++) {
            chunk[i] = (uint8_t)~chunk[i];
        }

        int chunk_has_act = 0;
        for (ssize_t i = 0; i < n; i++) {
            if (chunk[i] != 0x00) {
                chunk_has_act = 1;
                break;
            }
        }

        if (!active) {
            if (chunk_has_act) {
                active = 1;
                gettimeofday(&act_tv, NULL);
                if (raw_len + (size_t)n <= cap) {
                    memcpy(raw_buf + raw_len, chunk, (size_t)n);
                    raw_len += (size_t)n;
                }
            }
        } else {
            if (raw_len + (size_t)n <= cap) {
                memcpy(raw_buf + raw_len, chunk, (size_t)n);
                raw_len += (size_t)n;
            }
        }
    }

    close(fd);
    i2s_lock_release(lock_fd);

    if (raw_len == 0) {
        free(raw_buf);
        snprintf(out, outlen, "No IR signal received: timeout waiting for remote button press");
        return -5;
    }

    int ret = ir_i2s_decode_capture(raw_buf, raw_len, out, outlen);
    free(raw_buf);
    return ret;
}

int ir_i2s_decode_capture(const uint8_t *raw_buf, size_t raw_len, char *out, size_t outlen) {
    if (!raw_buf || raw_len == 0 || !out || outlen < 32) return -1;
    out[0] = '\0';

    /* Bit-by-bit MSB-first: find first Mark run with >= 300 samples (~28 us) to skip optical glitches */
    size_t bit_total = raw_len * 8;
    size_t bit_idx = 0;
    int found_start = 0;

    size_t scan_idx = 0;
    while (scan_idx < bit_total) {
        while (scan_idx < bit_total) {
            uint8_t byte = raw_buf[scan_idx / 8];
            int bit = (byte >> (7 - (scan_idx % 8))) & 1;
            if (bit == 1) break;
            scan_idx++;
        }
        if (scan_idx >= bit_total) break;

        size_t m_len = 0;
        size_t m_scan = scan_idx;
        while (m_scan < bit_total) {
            uint8_t byte = raw_buf[m_scan / 8];
            int bit = (byte >> (7 - (m_scan % 8))) & 1;
            if (bit == 0) break;
            m_len++;
            m_scan++;
        }

        if (m_len >= 300) {
            found_start = 1;
            bit_idx = scan_idx;
            break;
        }
        scan_idx = m_scan;
    }

    if (!found_start) {
        /* Fallback: accept any mark if no large pulse found */
        for (size_t i = 0; i < bit_total; i++) {
            uint8_t byte = raw_buf[i / 8];
            int bit = (byte >> (7 - (i % 8))) & 1;
            if (bit == 1) {
                found_start = 1;
                bit_idx = i;
                break;
            }
        }
    }

    if (!found_start) {
        snprintf(out, outlen, "No IR signal detected (idle bitstream)");
        return -6;
    }

    /* Run-length decode */
    uint32_t *runs = (uint32_t *)malloc(4096 * sizeof(uint32_t));
    if (!runs) {
        snprintf(out, outlen, "Out of memory allocating runs buffer");
        return -7;
    }
    size_t run_count = 0;
    int cur_state = 1; /* 1 = mark, 0 = space */
    uint32_t cur_run = 0;

    for (; bit_idx < bit_total; bit_idx++) {
        uint8_t byte = raw_buf[bit_idx / 8];
        int bit = (byte >> (7 - (bit_idx % 8))) & 1;
        if (bit == cur_state) {
            cur_run++;
        } else {
            if (run_count < 4096) {
                runs[run_count++] = cur_run;
            }
            cur_state = bit;
            cur_run = 1;
        }
    }
    if (cur_run > 0 && run_count < 4096) {
        runs[run_count++] = cur_run;
    }

    if (run_count < 8) {
        free(runs);
        snprintf(out, outlen, "No valid IR signal detected (too few transitions: %zu)", run_count);
        return -8;
    }

    /* Format as Harmony raw string: F9470P<hex>S<hex>... */
    char *p = out;
    size_t rem = outlen;
    int written = snprintf(p, rem, "F9470");
    if (written > 0) { p += written; rem -= (size_t)written; }

    for (size_t i = 0; i < run_count && rem > 16; i++) {
        /* US_PER_SAMPLE = 0.09375 = 3 / 32 */
        uint32_t dur = (uint32_t)(((uint64_t)runs[i] * 3 + 16) / 32);
        if (dur == 0) dur = 1;
        char prefix = (i % 2 == 0) ? 'P' : 'S';
        written = snprintf(p, rem, "%c%X", prefix, dur);
        if (written > 0) { p += written; rem -= (size_t)written; }

        /* If space exceeds 20ms, end of frame */
        if (i % 2 == 1 && dur > 20000) {
            break;
        }
    }

    free(runs);
    return 0;
}
