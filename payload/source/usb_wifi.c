/*
 * USB Wi-Fi setup (hub_util usbwifi): set the Hub's Wi-Fi network from a computer over USB,
 * the way the stock firmware did, for a Hub that is off every network.
 *
 * The USB side stays the stock /usr/bin/usbhid (started by the stock boot script): it presents
 * the Hub as the USB HID device the PC tools know (046D:C129) and relays the 64-byte reports
 * to a TCP peer on 127.0.0.1:16716. Stock, that peer was hal, which harmony-ng does not run.
 * This program takes its place, but only for Wi-Fi: it answers the stock file protocol for the
 * one virtual file /sys/wifi/connect (write = set the network, read = the status) and refuses
 * everything else with stock's error answer (no Dropbear upload, no boot trigger, no reset, no
 * firmware). tools/harmony_hid.py provision_wifi() and get_wifi_status() work unchanged.
 *
 * Stream: one connection type byte from usbhid (0x06), then 64-byte frames both ways.
 * Request (primary frame): FF <cmd> <seq> <param count>, then parameters: a type/length byte TL
 * (bits 0-5 length, bit 6 a second length byte follows, bit 7 buffer; a buffer of length 0 is a
 * NUL-terminated string; without bit 7, 1, 2 or 4 bytes are a big-endian number) and the value.
 * Data frames (after a write command): <flags> <TL> <data, up to 62 bytes>.
 * Answers: FF <cmd> <seq|0x80> <param count> <params>; an error is FF <cmd> FF 01 01 <code>
 * (byte 2 = 0xFF). Read data frames: <index> <length> <data>, then FE when fewer were sent than
 * asked for.
 *
 * The write is answered when the Wi-Fi attempt has ended (as stock), and always with success
 * once the request was understood: the result is in the status (connect_status failed and
 * error_code), because the PC tools wait 100 s for a write answer and stop with an exception on
 * an error. The network is tested first and the previous configuration is kept when the test
 * fails (wifi_config.c, shared with the web UI). The password is never sent back or logged.
 *
 * Only WPA2-PSK, WPA-PSK (pass phrase) and open networks are accepted, as stock. Any computer
 * on the port may use it without a login (as stock); USB_WIFI_SETUP=0 in ethernet.conf
 * (POST /api/ethernet-save usb_wifi_setup) turns it off: every request is refused at once.
 *
 * Single-threaded with poll(): the Hub kernel has no futex, so no threads and no lock waits.
 * A new connection from usbhid (it connects again each time a computer configures the device)
 * replaces the old one; the count of connections is in /tmp/harmony_usbwifi.state for
 * network_manager.sh, which keeps the USB port in device mode while a computer uses it.
 */
#include "usb_wifi.h"
#include "wifi_config.h"
#include "atomic_file.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>

#define FRAME 64
#define MAX_HANDLES 4
#define MAX_PAYLOAD 1024
#define WIFI_FILE "/sys/wifi/connect"
#define RESULT_HOLD_S 120
/* data frames one write command may announce: the largest request (MAX_PAYLOAD) needs 17;
 * more is a malformed request, refused before the count is used */
#define MAX_WRITE_FRAMES 1024

/* stock hal error codes */
#define ERR_GENERIC 0x00
#define ERR_HANDLE 0x04
#define ERR_NO_FILE 0x0B
#define ERR_PARSE 0x0C
#define ERR_TOO_MANY 0x0D

struct handle {
    int used;
    uint8_t id;
    int write;
    char text[512];
    size_t size;
    size_t off;
};

struct conn {
    int fd;
    int have_type;
    uint8_t frame[FRAME];
    size_t flen;
    struct handle h[MAX_HANDLES];
    uint8_t next_id;
    /* a write command waiting for its data frames */
    int wr_pending;
    int wr_left;
    uint8_t wr_seq;
    uint8_t wr_handle;
    char payload[MAX_PAYLOAD + 1];
    size_t plen;
    int overflow;
    int told_off;       /* "turned off" logged once per connection */
};

static unsigned long g_connections;
static unsigned long g_requests;
static int g_last_ok = -1;          /* -1 none, 0 failed, 1 ok */
static char g_last_error[160];
static long g_last_time;
static int g_verbose;

static long uptime_s(void) {
    struct sysinfo si;
    if (sysinfo(&si) != 0) return 0;
    return (long)si.uptime;
}

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[usbwifi] ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

/* A network name as it may be logged: control characters (ESC, CR, ...) become '?', so a name
 * sent over USB cannot change the log's lines or a terminal that shows it */
static const char *log_safe(const char *in, char *out, size_t len) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < len; i++) {
        unsigned char c = (unsigned char)in[i];
        out[j++] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    out[j] = '\0';
    return out;
}

/* USB_WIFI_SETUP=0 in ethernet.conf turns the setup off (default on, as stock) */
int usbwifi_enabled(void) {
    char pbuf[256];
    return wifi_usb_setup_setting(wifi_config_path(USBWIFI_SETTING_PATH, pbuf, sizeof(pbuf)));
}

static void write_state(void) {
    char pbuf[256], text[160];
    snprintf(text, sizeof(text), "connections=%lu\nrequests=%lu\nlast_attempt=%s\n", g_connections, g_requests,
             g_last_ok < 0 ? "none" : (g_last_ok ? "ok" : "failed"));
    atomic_file_write_str(wifi_config_path(USBWIFI_STATE_PATH, pbuf, sizeof(pbuf)), text);
}

/* ---- status text (key,value lines, as stock) ---- */

/* stock separates key and value with the first comma and ends a line at \n: keep both out of a
 * value (an SSID may contain any byte) */
static void put_line(char *out, size_t len, const char *key, const char *val) {
    size_t used = strlen(out);
    if (used >= len) return;
    char clean[160];
    size_t j = 0;
    for (size_t i = 0; val && val[i] && j + 1 < sizeof(clean); i++) {
        unsigned char c = (unsigned char)val[i];
        clean[j++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    clean[j] = '\0';
    snprintf(out + used, len - used, "%s,%s\n", key, clean);
}

size_t usbwifi_status_text(char *out, size_t len) {
    if (!out || !len) return 0;
    out[0] = '\0';
    struct wifi_status st;
    wifi_config_status(&st);
    int recent = g_last_ok >= 0 && uptime_s() - g_last_time < RESULT_HOLD_S;
    const char *cs;
    if (recent && g_last_ok == 0) cs = "failed";
    else if (st.completed) cs = "connected";
    else if (recent && g_last_ok == 1) cs = "connecting";
    else cs = "inactive";
    put_line(out, len, "ssid", st.ssid);
    put_line(out, len, "encryption", st.key_mgmt);
    put_line(out, len, "password", "");
    put_line(out, len, "connect_status", cs);
    put_line(out, len, "ip_address", st.ip[0] ? st.ip : "0.0.0.0");
    put_line(out, len, "error_code", (g_last_ok == 0 && g_last_error[0]) ? g_last_error : "none");
    put_line(out, len, "last_attempt", g_last_ok < 0 ? "none" : (g_last_ok ? "ok" : "failed"));
    put_line(out, len, "firmware", "harmony-ng");
    return strlen(out);
}

/* ---- setting the network ---- */

static void set_result(int ok, const char *err) {
    g_last_ok = ok;
    g_last_time = uptime_s();
    snprintf(g_last_error, sizeof(g_last_error), "%s", ok ? "" : (err && err[0] ? err : "failed"));
    write_state();
}

/* udhcpc on ath0 asks for a new lease now (the network monitor starts one when none runs) */
static void renew_lease(void) {
    char pbuf[256];
    FILE *f = fopen(wifi_config_path("/var/run/udhcpc.ath0.pid", pbuf, sizeof(pbuf)), "r");
    if (!f) return;
    long pid = 0;
    if (fscanf(f, "%ld", &pid) != 1) pid = 0;
    fclose(f);
    if (pid > 1) kill((pid_t)pid, SIGUSR1);
}

/* The written text: "ssid,<s>\npassword,<p>\nencryption,<e>\n" (stock: each line split at its
 * first comma, parsing ends at the first line without one) */
static void apply_payload(const char *payload, size_t plen, int overflow) {
    if (overflow) {
        logmsg("Wi-Fi setup refused: the request is longer than %d bytes", MAX_PAYLOAD);
        set_result(0, "The request is too long");
        return;
    }
    char ssid[64] = "", psk[128] = "", enc[32] = "";
    int have_ssid = 0, bad_value = 0;
    const char *p = payload, *end = payload + plen;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *comma = memchr(p, ',', (size_t)(le - p));
        if (!comma) break;
        size_t kl = (size_t)(comma - p), vl = (size_t)(le - comma - 1);
        if (vl > 0 && comma[vl] == '\r') vl--; /* CRLF line ends: the CR is not part of the value */
        char *dst = NULL;
        size_t dsz = 0;
        if (kl == 4 && strncmp(p, "ssid", 4) == 0) { dst = ssid; dsz = sizeof(ssid); have_ssid = 1; }
        else if (kl == 8 && strncmp(p, "password", 8) == 0) { dst = psk; dsz = sizeof(psk); }
        else if (kl == 10 && strncmp(p, "encryption", 10) == 0) { dst = enc; dsz = sizeof(enc); }
        if (dst) {
            if (vl >= dsz || memchr(comma + 1, '\0', vl)) bad_value = 1;
            else { memcpy(dst, comma + 1, vl); dst[vl] = '\0'; }
        }
        p = nl ? nl + 1 : end;
    }
    if (bad_value) {
        logmsg("Wi-Fi setup refused: a value is too long or contains a NUL byte");
        set_result(0, "A value is too long");
        return;
    }
    if (!have_ssid || !ssid[0]) {
        logmsg("Wi-Fi setup refused: no network name");
        set_result(0, "SSID missing");
        return;
    }
    /* only for the log, never the stored value (wifi_config.c refuses control characters) */
    char shown[sizeof(ssid)];
    log_safe(ssid, shown, sizeof(shown));
    int is_open;
    if (strncasecmp(enc, "WEP", 3) == 0) {
        logmsg("Wi-Fi setup refused for '%s': WEP is not supported", shown);
        set_result(0, "WEP is not supported");
        return;
    } else if (strcasecmp(enc, "WPA2-PSK") == 0 || strcasecmp(enc, "WPA-PSK") == 0) {
        /* the two pass phrase types stock accepted; WPA-EAP, enterprise, SAE are not offered */
        is_open = 0;
    } else if (strcasecmp(enc, "NONE") == 0 || strcasecmp(enc, "OPEN") == 0) {
        is_open = 1;
    } else if (enc[0] == '\0') {
        is_open = psk[0] == '\0';
    } else {
        logmsg("Wi-Fi setup refused for '%s': unsupported encryption", shown);
        set_result(0, "Unsupported encryption (use WPA2-PSK, WPA-PSK or NONE; WEP and enterprise networks are not supported)");
        return;
    }
    logmsg("Wi-Fi setup over USB: testing network '%s' (%s)", shown, is_open ? "open" : "WPA/WPA2 pass phrase");
    char err[200] = "";
    /* hidden 1: wpa_supplicant also probes for the name, as stock did (scan_ssid 1) */
    int rc = wifi_config_apply(ssid, is_open ? "" : psk, is_open, 1, err, sizeof(err));
    memset(psk, 0, sizeof(psk));
    if (rc == 0) {
        logmsg("Wi-Fi setup over USB: '%s' works and was saved", shown);
        set_result(1, NULL);
        renew_lease();
    } else {
        logmsg("Wi-Fi setup over USB: '%s' not saved (%s); the previous settings are kept", shown, err[0] ? err : "failed");
        set_result(0, err);
    }
}

/* ---- frames ---- */

static int send_frame(struct conn *c, const uint8_t *data, size_t len) {
    uint8_t f[FRAME];
    memset(f, 0, sizeof(f));
    memcpy(f, data, len > FRAME ? FRAME : len);
    size_t sent = 0;
    while (sent < FRAME) {
        ssize_t w = send(c->fd, f + sent, FRAME - sent, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

static void send_error(struct conn *c, uint8_t cmd, uint8_t code) {
    uint8_t a[6] = {0xFF, cmd, 0xFF, 0x01, 0x01, code};
    if (g_verbose) logmsg("request 0x%02x refused (code 0x%02x)", cmd, code);
    send_frame(c, a, sizeof(a));
}

struct param {
    int is_num;
    uint32_t num;
    const uint8_t *data;
    size_t len;
};

/* the parameters of a primary frame; -1 when they run past the frame */
static int parse_params(const uint8_t *f, struct param *pv, int max) {
    int count = f[3] & 0x3F;
    if (count > max) return -1;
    size_t pos = 4;
    for (int i = 0; i < count; i++) {
        if (pos >= FRAME) return -1;
        uint8_t tl = f[pos++];
        size_t len = tl & 0x3F;
        if (tl & 0x40) {
            if (pos >= FRAME) return -1;
            len = (len << 8) | f[pos++];
        }
        memset(&pv[i], 0, sizeof(pv[i]));
        if ((tl & 0x80) && len == 0) {
            const uint8_t *s = f + pos;
            const uint8_t *z = memchr(s, 0, FRAME - pos);
            if (!z) return -1;
            pv[i].data = s;
            pv[i].len = (size_t)(z - s);
            pos += pv[i].len + 1;
            continue;
        }
        if (pos + len > FRAME) return -1;
        pv[i].data = f + pos;
        pv[i].len = len;
        if (!(tl & 0x80) && (len == 1 || len == 2 || len == 4)) {
            uint32_t v = 0;
            for (size_t k = 0; k < len; k++) v = (v << 8) | f[pos + k];
            pv[i].is_num = 1;
            pv[i].num = v;
        }
        pos += len;
    }
    return count;
}

static struct handle *find_handle(struct conn *c, uint32_t id) {
    for (int i = 0; i < MAX_HANDLES; i++)
        if (c->h[i].used && c->h[i].id == id) return &c->h[i];
    return NULL;
}

static void do_open(struct conn *c, uint8_t seq, const struct param *pv, int n) {
    if (n < 2 || pv[0].is_num || pv[1].is_num) { send_error(c, 0x01, ERR_PARSE); return; }
    if (pv[0].len != strlen(WIFI_FILE) || memcmp(pv[0].data, WIFI_FILE, pv[0].len) != 0) {
        if (g_verbose) logmsg("open of '%.*s' refused (only " WIFI_FILE ")", (int)pv[0].len, (const char *)pv[0].data);
        send_error(c, 0x01, ERR_NO_FILE);
        return;
    }
    if (!usbwifi_enabled()) {
        if (!c->told_off) logmsg("USB Wi-Fi setup is turned off in the settings (USB_WIFI_SETUP=0): request refused");
        c->told_off = 1;
        send_error(c, 0x01, ERR_NO_FILE);
        return;
    }
    int rd = memchr(pv[1].data, 'R', pv[1].len) != NULL;
    int wr = memchr(pv[1].data, 'W', pv[1].len) != NULL || memchr(pv[1].data, 'C', pv[1].len) != NULL;
    if (rd == wr) { send_error(c, 0x01, ERR_PARSE); return; }
    struct handle *h = NULL;
    for (int i = 0; i < MAX_HANDLES; i++)
        if (!c->h[i].used) { h = &c->h[i]; break; }
    if (!h) { send_error(c, 0x01, ERR_TOO_MANY); return; }
    memset(h, 0, sizeof(*h));
    h->used = 1;
    h->write = wr;
    /* ids are one byte: after 255 opens skip one still held by an open handle (at most
     * MAX_HANDLES - 1 others, so this ends) */
    do {
        if (++c->next_id == 0) c->next_id = 1;
    } while (find_handle(c, c->next_id) != NULL);
    h->id = c->next_id;
    if (rd) h->size = usbwifi_status_text(h->text, sizeof(h->text));
    uint8_t a[11] = {0xFF, 0x01, (uint8_t)(seq | 0x80), 0x02, 0x01, h->id, 0x04,
                     (uint8_t)(h->size >> 24), (uint8_t)(h->size >> 16), (uint8_t)(h->size >> 8), (uint8_t)h->size};
    send_frame(c, a, sizeof(a));
}

static void finish_write(struct conn *c) {
    c->wr_pending = 0;
    struct handle *h = find_handle(c, c->wr_handle);
    if (!h || !h->write) {
        send_error(c, 0x03, ERR_HANDLE);
    } else {
        c->payload[c->plen] = '\0';
        apply_payload(c->payload, c->plen, c->overflow);
        uint8_t a[6] = {0xFF, 0x03, (uint8_t)(c->wr_seq | 0x80), 0x01, 0x01, h->id};
        send_frame(c, a, sizeof(a));
    }
    memset(c->payload, 0, sizeof(c->payload));
    c->plen = 0;
    c->overflow = 0;
}

static void do_write(struct conn *c, uint8_t seq, const struct param *pv, int n) {
    if (n < 2 || !pv[0].is_num || !pv[1].is_num || pv[1].num < 1 || pv[1].num > MAX_WRITE_FRAMES) {
        send_error(c, 0x03, ERR_PARSE);
        return;
    }
    c->wr_pending = 1;
    c->wr_seq = seq;
    c->wr_handle = (uint8_t)pv[0].num;
    c->wr_left = (int)pv[1].num - 1;
    c->plen = 0;
    c->overflow = 0;
    if (c->wr_left == 0) finish_write(c);
}

static void data_frame(struct conn *c, const uint8_t *f) {
    size_t len = f[1] & 0x3F;
    const uint8_t *d = f + 2;
    if (f[1] & 0x40) {
        len = (len << 8) | f[2];
        d = f + 3;
    }
    if (len > (size_t)(f + FRAME - d)) len = (size_t)(f + FRAME - d);
    if (c->plen + len > MAX_PAYLOAD) c->overflow = 1;
    else {
        memcpy(c->payload + c->plen, d, len);
        c->plen += len;
    }
    if (--c->wr_left <= 0) finish_write(c);
}

static void do_read(struct conn *c, uint8_t seq, const struct param *pv, int n) {
    if (n < 2 || !pv[0].is_num || !pv[1].is_num) { send_error(c, 0x04, ERR_PARSE); return; }
    struct handle *h = find_handle(c, pv[0].num);
    if (!h || h->write) { send_error(c, 0x04, ERR_HANDLE); return; }
    uint32_t want = pv[1].num;
    uint32_t frames = 0;
    size_t remain = h->size - h->off;
    while (frames < want && (size_t)frames * 62 < remain) frames++;
    if (frames > 255) frames = 255;
    uint8_t a[8] = {0xFF, 0x04, (uint8_t)(seq | 0x80), 0x02, 0x01, h->id, 0x01, (uint8_t)frames};
    if (send_frame(c, a, sizeof(a)) != 0) return;
    for (uint32_t i = 0; i < frames; i++) {
        size_t chunk = h->size - h->off;
        if (chunk > 62) chunk = 62;
        uint8_t df[FRAME];
        memset(df, 0, sizeof(df));
        df[0] = (uint8_t)(i & 0x7F);
        df[1] = (uint8_t)chunk;
        memcpy(df + 2, h->text + h->off, chunk);
        h->off += chunk;
        if (send_frame(c, df, sizeof(df)) != 0) return;
    }
    if (frames < want) {
        uint8_t end = 0xFE;
        send_frame(c, &end, 1);
    }
}

static void do_close(struct conn *c, uint8_t seq, const struct param *pv, int n) {
    if (n < 1 || !pv[0].is_num) { send_error(c, 0x07, ERR_PARSE); return; }
    struct handle *h = find_handle(c, pv[0].num);
    if (!h) { send_error(c, 0x07, ERR_HANDLE); return; }
    uint8_t id = h->id;
    memset(h, 0, sizeof(*h));
    uint8_t a[6] = {0xFF, 0x07, (uint8_t)(seq | 0x80), 0x01, 0x01, id};
    send_frame(c, a, sizeof(a));
}

static void handle_frame(struct conn *c, const uint8_t *f) {
    if (c->wr_pending && f[0] != 0xFF) {
        data_frame(c, f);
        return;
    }
    if (f[0] != 0xFF) return; /* padding or a stray data frame */
    /* any request comes from a computer: network_manager.sh keeps the port a USB device for it
     * (written at the first one; the set writes the state again with its result) */
    if (g_requests++ == 0) write_state();
    if (c->wr_pending) {
        /* a new request before the announced data frames: the write is dropped */
        c->wr_pending = 0;
        c->plen = 0;
        c->overflow = 0;
    }
    uint8_t cmd = f[1], seq = f[2] & 0x7F;
    struct param pv[6];
    int n;
    switch (cmd) {
    case 0x00:
        {
            uint8_t a[4] = {0xFF, 0x00, (uint8_t)(seq | 0x80), 0x00};
            send_frame(c, a, sizeof(a));
        }
        return;
    case 0x01: case 0x03: case 0x04: case 0x07:
        n = parse_params(f, pv, 6);
        if (n < 0) { send_error(c, cmd, ERR_PARSE); return; }
        if (cmd == 0x01) do_open(c, seq, pv, n);
        else if (cmd == 0x03) do_write(c, seq, pv, n);
        else if (cmd == 0x04) do_read(c, seq, pv, n);
        else do_close(c, seq, pv, n);
        return;
    default:
        /* flush, device control, commands, notify, reset (firmware), ...: not offered */
        send_error(c, cmd, ERR_NO_FILE);
        return;
    }
}

static void feed(struct conn *c, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!c->have_type) {
            /* connection type byte (usbhid sends 0x06) */
            c->have_type = 1;
            continue;
        }
        c->frame[c->flen++] = buf[i];
        if (c->flen == FRAME) {
            c->flen = 0;
            handle_frame(c, c->frame);
        }
    }
}

static void conn_reset(struct conn *c, int fd) {
    memset(c, 0, sizeof(*c));
    c->fd = fd;
}

static int usage(void) {
    fprintf(stderr,
            "Usage: hub_util usbwifi [--port N] [--verbose]\n"
            "       hub_util usbwifi --status\n"
            "Answers the stock USB Wi-Fi setup requests of /usr/bin/usbhid on 127.0.0.1:%d\n"
            "(set and read /sys/wifi/connect only). --status prints the status the PC reads.\n",
            USBWIFI_PORT);
    return 2;
}

int usbwifi_main(int argc, char **argv) {
    int port = USBWIFI_PORT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
            if (port <= 0 || port > 65535) return usage();
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "--status") == 0) {
            char text[512];
            usbwifi_status_text(text, sizeof(text));
            fputs(text, stdout);
            return 0;
        } else {
            return usage();
        }
    }
    signal(SIGPIPE, SIG_IGN);

    /* a Wi-Fi test cut short (this program killed, the power cut): previous files back */
    wifi_config_recover();
    if (!usbwifi_enabled())
        logmsg("USB Wi-Fi setup is turned off in the settings (USB_WIFI_SETUP=0): every request is refused");

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        perror("usbwifi: socket");
        return 1;
    }
    /* not inherited by the wpa_cli and udhcpc helpers this program starts */
    fcntl(ls, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* never reachable from the network */
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(ls, 2) != 0) {
        logmsg("cannot listen on 127.0.0.1:%d: %s", port, strerror(errno));
        close(ls);
        return 1;
    }
    logmsg("listening on 127.0.0.1:%d for the USB Wi-Fi setup (usbhid)", port);
    write_state();

    static struct conn c;
    conn_reset(&c, -1);
    for (;;) {
        struct pollfd pf[2];
        int np = 1;
        pf[0].fd = ls;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        if (c.fd >= 0) {
            pf[1].fd = c.fd;
            pf[1].events = POLLIN;
            pf[1].revents = 0;
            np = 2;
        }
        int r = poll(pf, (nfds_t)np, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            logmsg("poll: %s", strerror(errno));
            return 1;
        }
        if (pf[0].revents & POLLIN) {
            int fd = accept(ls, NULL, NULL);
            if (fd >= 0) {
                if (c.fd >= 0) close(c.fd);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                int nd = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
                conn_reset(&c, fd);
                g_connections++;
                if (g_verbose || g_connections <= 2)
                    logmsg("usbhid connected (connection %lu%s)", g_connections,
                           g_connections >= 2 ? ": a computer configured the USB device" : "");
                write_state();
            }
            continue; /* the old client's events are stale */
        }
        if (np == 2 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t buf[256];
            ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                close(c.fd);
                conn_reset(&c, -1);
                continue;
            }
            feed(&c, buf, (size_t)n);
        }
    }
}
