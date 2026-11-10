#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* O_CLOEXEC with uClibc */
#endif
#include "remote_flightlog.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char s_file[256] = FLIGHTLOG_FILE;
static char s_enable_file[256] = FLIGHTLOG_ENABLE_FILE;
static long s_max_bytes = FLIGHTLOG_MAX_BYTES;
static unsigned long s_count = 0;
static void (*s_context)(char *buf, size_t buf_sz) = NULL;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

void flightlog_set_paths(const char *file, const char *enable_file, long max_bytes) {
    if (file) snprintf(s_file, sizeof(s_file), "%s", file);
    if (enable_file) snprintf(s_enable_file, sizeof(s_enable_file), "%s", enable_file);
    if (max_bytes > 0) s_max_bytes = max_bytes;
}

void flightlog_set_context(void (*fn)(char *buf, size_t buf_sz)) {
    s_context = fn;
}

bool flightlog_enabled(void) {
    return access(s_enable_file, F_OK) == 0;
}

/* A new name in a directory is only durable once the directory itself is synced */
static void sync_dir_of(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    int dfd = open(dirname(tmp), O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

/* Appends text and makes it durable; call with s_lock held */
static void append_durable(const char *text, size_t len) {
    struct stat st;
    bool is_new = stat(s_file, &st) != 0;
    if (!is_new && st.st_size + (off_t)len > (off_t)s_max_bytes) {
        char old[280];
        snprintf(old, sizeof(old), "%s.1", s_file);
        rename(s_file, old);
        is_new = true;
    }
    int fd = open(s_file, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, text + done, len - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        done += (size_t)w;
    }
    fsync(fd);
    close(fd);
    if (is_new) sync_dir_of(s_file);
}

static size_t format_prefix(char *buf, size_t buf_sz, const char *event, bool in_signal) {
    struct timespec mono = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    time_t now = time(NULL);
    struct tm tmv;
    char stamp[32] = "";
    /* localtime_r() takes the C library's time zone lock: not from a signal handler */
    if (!in_signal && localtime_r(&now, &tmv)) strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S%z", &tmv);
    else snprintf(stamp, sizeof(stamp), "epoch=%ld", (long)now);
    int n = snprintf(buf, buf_sz, "%s up=%ld.%03ld n=%lu %s", stamp, (long)mono.tv_sec,
                     mono.tv_nsec / 1000000L, ++s_count, event);
    return (n < 0) ? 0 : ((size_t)n >= buf_sz ? buf_sz - 1 : (size_t)n);
}

/* One record, plus optional lines after it (tail), in a single write and a single fsync.
 * in_signal: called from a crash signal handler. Then the lock is only tried (the crashed thread
 * may hold it, and waiting for it would keep the handler from ever reaching _exit()), nothing is
 * allocated, and the time is not converted. */
static void write_record(bool in_signal, const char *event, const char *tail, size_t tail_len, const char *fmt,
                         va_list args) {
    char line[640];
    char ctx[256] = "";

    if (in_signal) {
        if (pthread_mutex_trylock(&s_lock) != 0) return;
    } else {
        pthread_mutex_lock(&s_lock);
    }
    if (s_context) s_context(ctx, sizeof(ctx));
    const size_t cap = sizeof(line) - 2; /* room for the newline and the terminator */
    size_t n = format_prefix(line, cap, event, in_signal);
    if (fmt && fmt[0] && n + 2 < cap) {
        line[n++] = ' ';
        int m = vsnprintf(line + n, cap - n, fmt, args);
        if (m > 0) n += ((size_t)m >= cap - n) ? cap - n - 1 : (size_t)m;
    }
    if (ctx[0] && n + 4 < cap) {
        int m = snprintf(line + n, cap - n, " | %s", ctx);
        if (m > 0) n += ((size_t)m >= cap - n) ? cap - n - 1 : (size_t)m;
    }
    for (size_t i = 0; i < n; i++) {
        if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';
    }
    line[n++] = '\n';
    char *all = (tail && tail_len > 0 && !in_signal) ? malloc(n + tail_len) : NULL;
    if (all) {
        memcpy(all, line, n);
        memcpy(all + n, tail, tail_len);
        append_durable(all, n + tail_len);
        free(all);
    } else {
        append_durable(line, n);
    }
    pthread_mutex_unlock(&s_lock);
}

/* For the records this file writes itself; fmt may be NULL for a record without details */
static void record_fmt(bool in_signal, const char *event, const char *tail, size_t tail_len, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static void record_fmt(bool in_signal, const char *event, const char *tail, size_t tail_len, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write_record(in_signal, event, tail, tail_len, fmt, args);
    va_end(args);
}

void flightlog_event(const char *event, const char *fmt, ...) {
    if (!event || !flightlog_enabled()) return;
    va_list args;
    va_start(args, fmt);
    write_record(false, event, NULL, 0, fmt, args);
    va_end(args);
}

void flightlog_mark(const char *event) {
    if (!event || !flightlog_enabled()) return;
    record_fmt(false, event, NULL, 0, NULL);
}

void flightlog_crash(int sig) {
    if (!flightlog_enabled()) return;
    record_fmt(true, "CRASH", NULL, 0, "signal=%d", sig);
}

/* The event and time stamp of the last record (lines that start with a space are kernel lines) */
bool flightlog_last_event(const char *path, char *event, size_t event_sz, char *stamp, size_t stamp_sz) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool found = false;
    char line[700];
    /* The tail is enough: records are short and the last one is what matters */
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        fseek(f, size > 8192 ? size - 8192 : 0, SEEK_SET);
        if (size > 8192 && !fgets(line, sizeof(line), f)) { /* drop the cut line */ }
    }
    while (fgets(line, sizeof(line), f)) {
        char st[40], up[32], cnt[32], ev[40];
        if (line[0] == ' ' || line[0] == '\n') continue;
        if (sscanf(line, "%39s %31s %31s %39s", st, up, cnt, ev) != 4) continue;
        if (strncmp(up, "up=", 3) != 0 || strncmp(cnt, "n=", 2) != 0) continue;
        if (event) snprintf(event, event_sz, "%s", ev);
        if (stamp) snprintf(stamp, stamp_sz, "%s", st);
        found = true;
    }
    fclose(f);
    return found;
}

void flightlog_init(const char *version) {
    if (!flightlog_enabled()) return;
    char prev[40] = "none", prev_t[40] = "-";
    flightlog_last_event(s_file, prev, sizeof(prev), prev_t, sizeof(prev_t));
    /* SUSPEND_ENTER with nothing after it: the remote went into that suspend and this start is
     * the next thing that happened */
    bool in_suspend = strcmp(prev, FLIGHTLOG_EV_SUSPEND_ENTER) == 0;
    char sysup[32] = "?";
    FILE *fu = fopen("/proc/uptime", "r");
    if (fu) {
        if (fscanf(fu, "%31s", sysup) != 1) snprintf(sysup, sizeof(sysup), "?");
        fclose(fu);
    }
    flightlog_event("BOOT", "version=%s sysup=%s prev=%s prev_t=%s%s", version ? version : "?", sysup, prev,
                    prev_t, in_suspend ? " SUSPEND_DID_NOT_RETURN" : "");
}

/* True for a kernel line worth keeping; *start and *len give its text without the "<6>" level and
 * the "[ time]" stamp. The pin and GPIO dump the board code prints before every suspend is not. */
static bool klog_line_kept(const char *p, size_t line_len, const char **start, size_t *len) {
    static const char *const skip[] = { "PINMUX", "DIR", "OUT_DATA", "IN_DATA" };
    const char *s = p;
    const char *end = p + line_len;
    if (s < end && *s == '<') {
        const char *gt = memchr(s, '>', (size_t)(end - s));
        if (gt) s = gt + 1;
    }
    if (s < end && *s == '[') {
        const char *br = memchr(s, ']', (size_t)(end - s));
        if (br) s = br + 1;
    }
    while (s < end && *s == ' ') s++;
    if (s == end) return false;
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
        size_t sl = strlen(skip[i]);
        if ((size_t)(end - s) >= sl && strncmp(s, skip[i], sl) == 0) return false;
    }
    *start = s;
    *len = (size_t)(end - s);
    return true;
}

/* Keeps the newest max_lines kernel lines, each indented by two spaces: what the kernel said last
 * is what explains a halt, and the first read after a boot holds the whole boot log. Returns the
 * bytes written to out. */
size_t flightlog_filter_klog(const char *klog, char *out, size_t out_sz, int max_lines) {
    if (!klog || !out || out_sz < 2) return 0;
    int total = 0;
    for (const char *p = klog; *p;) {
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        const char *s;
        size_t len;
        if (klog_line_kept(p, line_len, &s, &len)) total++;
        if (!eol) break;
        p = eol + 1;
    }
    int skip_first = total > max_lines ? total - max_lines : 0;
    size_t n = 0;
    if (skip_first > 0) n = (size_t)snprintf(out, out_sz, "  (%d earlier kernel lines left out)\n", skip_first);
    if (n >= out_sz) n = out_sz - 1;
    int seen = 0;
    for (const char *p = klog; *p;) {
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        const char *s;
        size_t len;
        if (klog_line_kept(p, line_len, &s, &len) && seen++ >= skip_first) {
            if (len > 160) len = 160;
            if (n + len + 4 >= out_sz) break;
            out[n++] = ' ';
            out[n++] = ' ';
            for (size_t i = 0; i < len; i++) out[n++] = isprint((unsigned char)s[i]) ? s[i] : '?';
            out[n++] = '\n';
        }
        if (!eol) break;
        p = eol + 1;
    }
    out[n] = '\0';
    return n;
}

#define KLOG_READ_CLEAR 4 /* SYSLOG_ACTION_READ_CLEAR */
#define KLOG_BUF_BYTES (128 * 1024)

void flightlog_kernel(void) {
    if (!flightlog_enabled()) return;
    char *raw = malloc(KLOG_BUF_BYTES + 1);
    char *out = malloc(8192);
    if (raw && out) {
        int got = klogctl(KLOG_READ_CLEAR, raw, KLOG_BUF_BYTES);
        if (got > 0) {
            raw[got] = '\0';
            size_t n = flightlog_filter_klog(raw, out, 8192, 45);
            if (n > 0) record_fmt(false, "KLOG", out, n, "bytes=%d", got);
        }
    }
    free(raw);
    free(out);
}
