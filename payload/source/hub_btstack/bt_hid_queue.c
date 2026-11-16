#include "bt_hid_queue.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t len;
    uint8_t data[HIDQ_REPORT_MAX];
    uint32_t queued_ms;
} hidq_report_t;

typedef struct {
    uint16_t cid; /* 0 = free */
    uint8_t head;
    uint8_t count;
    uint8_t refused_logged; /* the current wait has been logged */
    hidq_report_t q[HIDQ_DEPTH];
} hidq_host_t;

static hidq_host_t s_hosts[HIDQ_HOSTS];
static hidq_send_fn s_send;
static hidq_request_fn s_request;
static hidq_log_fn s_log;
static unsigned s_waits;
static unsigned s_dropped;

static void logf_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_line(const char *fmt, ...) {
    if (!s_log) return;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    s_log(line);
}

void hidq_init(hidq_send_fn send, hidq_request_fn request, hidq_log_fn log) {
    memset(s_hosts, 0, sizeof(s_hosts));
    s_send = send;
    s_request = request;
    s_log = log;
    s_waits = 0;
    s_dropped = 0;
}

int hidq_is_release(const uint8_t *report, uint16_t len) {
    if (!report || len < 3) return 0;
    for (uint16_t i = 2; i < len; i++) {
        if (report[i] != 0) return 0;
    }
    return 1;
}

static hidq_host_t *find_host(uint16_t cid, int create) {
    if (cid == 0) return NULL;
    for (int i = 0; i < HIDQ_HOSTS; i++) {
        if (s_hosts[i].cid == cid) return &s_hosts[i];
    }
    if (!create) return NULL;
    /* a free slot, or else one with nothing waiting (more channels than host instances) */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < HIDQ_HOSTS; i++) {
            if ((pass == 0 && s_hosts[i].cid == 0) || (pass == 1 && s_hosts[i].count == 0)) {
                memset(&s_hosts[i], 0, sizeof(s_hosts[i]));
                s_hosts[i].cid = cid;
                return &s_hosts[i];
            }
        }
    }
    return NULL;
}

static hidq_report_t *at(hidq_host_t *h, int i) {
    return &h->q[(h->head + i) % HIDQ_DEPTH];
}

/* Full queue: drop the oldest press (never a release); if only releases wait, the oldest one */
static void make_room(hidq_host_t *h) {
    int victim = 0;
    for (int i = 0; i < h->count; i++) {
        hidq_report_t *r = at(h, i);
        if (!hidq_is_release(r->data, r->len)) {
            victim = i;
            break;
        }
    }
    for (int i = victim; i < h->count - 1; i++) {
        *at(h, i) = *at(h, i + 1);
    }
    h->count--;
    s_dropped++;
    logf_line("HID reports for cid 0x%04x: queue full, dropped one waiting report (%u dropped since start)",
              (unsigned)h->cid, s_dropped);
}

/* Short name of a report for the log: which key or which release */
static void describe(const uint8_t *r, uint16_t len, char *out, size_t n) {
    if (len < 2) {
        snprintf(out, n, "report (%u bytes)", (unsigned)len);
        return;
    }
    const char *kind = r[1] == 0x01 ? "keyboard" : r[1] == 0x02 ? "consumer" : r[1] == 0x03 ? "mouse" : "report";
    if (hidq_is_release(r, len)) {
        snprintf(out, n, "%s release", kind);
    } else if (r[1] == 0x01 && len >= 5) {
        snprintf(out, n, "keyboard press 0x%02x", r[4]);
    } else if (r[1] == 0x02 && len >= 4) {
        snprintf(out, n, "consumer press 0x%04x", (unsigned)(r[2] | (r[3] << 8)));
    } else {
        snprintf(out, n, "%s 0x%02x", kind, r[1]);
    }
}

/* Sends what waits, oldest first, until the stack refuses one; then asks for a notice */
static void flush(hidq_host_t *h, uint32_t now_ms) {
    while (h->count > 0) {
        hidq_report_t *r = at(h, 0);
        uint8_t status = s_send(h->cid, r->data, r->len);
        if (status != 0) {
            if (!h->refused_logged) {
                char what[40];
                describe(r->data, r->len, what, sizeof(what));
                h->refused_logged = 1;
                logf_line("HID %s for cid 0x%04x refused by the stack (status 0x%02x): waiting to send %u report(s)",
                          what, (unsigned)h->cid, (unsigned)status, (unsigned)h->count);
            }
            if (s_request) s_request(h->cid);
            return;
        }
        if (h->refused_logged) {
            char what[40];
            describe(r->data, r->len, what, sizeof(what));
            logf_line("HID %s for cid 0x%04x sent after waiting %u ms (%u still waiting)",
                      what, (unsigned)h->cid, (unsigned)(now_ms - r->queued_ms), (unsigned)(h->count - 1));
        }
        h->head = (uint8_t)((h->head + 1) % HIDQ_DEPTH);
        h->count--;
    }
    h->refused_logged = 0;
}

void hidq_send(uint16_t cid, const uint8_t *report, uint16_t len, uint32_t now_ms) {
    if (!s_send || !report || len == 0) return;
    hidq_host_t *h = (len <= HIDQ_REPORT_MAX) ? find_host(cid, 1) : NULL;
    if (!h) {
        /* Cannot be queued (too large, or no slot): one attempt, as before */
        if (s_send(cid, report, len) != 0) {
            logf_line("HID report for cid 0x%04x refused by the stack and not queued (%u bytes)",
                      (unsigned)cid, (unsigned)len);
        }
        return;
    }
    /* Behind any waiting report, so the host gets them in order */
    if (h->count == HIDQ_DEPTH) make_room(h);
    hidq_report_t *r = at(h, h->count);
    memcpy(r->data, report, len);
    r->len = (uint8_t)len;
    r->queued_ms = now_ms;
    h->count++;
    flush(h, now_ms);
    if (h->count > 0) s_waits++;
}

void hidq_service(uint16_t cid, uint32_t now_ms) {
    hidq_host_t *h = find_host(cid, 0);
    if (h) flush(h, now_ms);
}

void hidq_service_all(uint32_t now_ms) {
    for (int i = 0; i < HIDQ_HOSTS; i++) {
        if (s_hosts[i].cid != 0 && s_hosts[i].count > 0) flush(&s_hosts[i], now_ms);
    }
}

void hidq_drop(uint16_t cid) {
    hidq_host_t *h = find_host(cid, 0);
    if (!h) return;
    if (h->count > 0) {
        logf_line("HID link cid 0x%04x closed with %u report(s) waiting: dropped", (unsigned)cid, (unsigned)h->count);
    }
    memset(h, 0, sizeof(*h));
}

int hidq_pending(uint16_t cid) {
    hidq_host_t *h = find_host(cid, 0);
    return h ? h->count : 0;
}

int hidq_pending_any(void) {
    for (int i = 0; i < HIDQ_HOSTS; i++) {
        if (s_hosts[i].cid != 0 && s_hosts[i].count > 0) return 1;
    }
    return 0;
}

unsigned hidq_waits(void) {
    return s_waits;
}

unsigned hidq_dropped(void) {
    return s_dropped;
}
