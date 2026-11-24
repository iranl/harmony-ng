/* HTTP and TCP device commands on a worker thread. See net_action.h and net_device.h. */
#define _GNU_SOURCE
#include "net_action.h"
#include "net_device.h"
#include "http_client.h"
#include "tls_client.h"
#include "hw_action.h"
#include "config_store.h"
#include "atomic_file.h"
#include "http_util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NET_QUEUE_LEN 16
#define NET_RESPONSE_HEAD 512
/* A key press still queued after this is dropped: sent late, it acts long after the user
 * pressed (and a toggle may then undo a later press) */
#define NET_QUEUE_MAX_AGE_MS 2000
/* A live key that finds the queue full takes the place of a queued key press older than this:
 * the key channel already skips a remote key older than 1 s (HUB_KEY_STALE_MS) */
#define NET_QUEUE_ROOM_AGE_MS 1000
/* After a device timed out, its queued presses are dropped for this long instead of each
 * waiting out the timeout again in front of the other devices */
#define NET_SLOW_HOLD_MS 10000
#define NET_SLOW_SLOTS 4

typedef struct {
    int kind;
    char host[256];
    int port;
    int tls;
    int verify;
    int timeout_ms;
    int connect_only;
    char method[8];
    char *url;                              /* HTTP */
    char *headers[NET_MAX_HEADERS + 3];     /* "Name: value", NULL-terminated */
    char *payload;                          /* HTTP body or TCP bytes */
    size_t payload_len;
    char label[200];                        /* device / command, for the log */
    /* POST /api/device-net-test waits for the result (done and abandoned under s_lock) */
    int sync;
    int done;
    int abandoned;
    cJSON *result;
    uint64_t queued_ms;                     /* monotonic, set by enqueue() */
    int live_key;                           /* a remote key press: dropped when late */
} net_job;

/* No condition variables: the Hub's kernel has no futex, so pthread_cond_wait() returns at
 * once there and the waiter spins at full CPU (hw_action.c, ir_i2s.c). The worker sleeps in
 * poll() on a pipe; the test waits by looking at its job every 20 ms. s_lock is held only
 * for a few instructions (a contended mutex spins too). */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
/* tls_pins.json: written by the worker (trust on first use) and by net_action_forget_pin */
static pthread_mutex_t s_pins_lock = PTHREAD_MUTEX_INITIALIZER;
static net_job *s_queue[NET_QUEUE_LEN];
static int s_head, s_count;
static int s_started;
static int s_wake[2] = { -1, -1 };
/* Devices that timed out recently (worker thread only) */
static struct { char host[256]; int port; uint64_t until_ms; } s_slow[NET_SLOW_SLOTS];

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int slow_slot(const net_job *j)
{
    for (int i = 0; i < NET_SLOW_SLOTS; i++)
        if (s_slow[i].port == j->port && strcasecmp(s_slow[i].host, j->host) == 0) return i;
    return -1;
}

static void slow_note(const net_job *j, int timed_out, uint64_t now)
{
    int i = slow_slot(j);
    if (!timed_out) {
        if (i >= 0) s_slow[i].until_ms = 0;
        return;
    }
    if (i < 0) {
        i = 0; /* the slot that expires first */
        for (int k = 1; k < NET_SLOW_SLOTS; k++)
            if (s_slow[k].until_ms < s_slow[i].until_ms) i = k;
        snprintf(s_slow[i].host, sizeof s_slow[i].host, "%s", j->host);
        s_slow[i].port = j->port;
    }
    s_slow[i].until_ms = now + NET_SLOW_HOLD_MS;
}

static void job_free(net_job *j)
{
    if (!j) return;
    free(j->url);
    for (int i = 0; j->headers[i]; i++) free(j->headers[i]);
    free(j->payload);
    cJSON_Delete(j->result);
    free(j);
}

/* ---- Building a job from the stored device and command ---- */

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *j = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsString(j) && j->valuestring ? j->valuestring : "";
}

static int jint(const cJSON *o, const char *key, int def)
{
    const cJSON *j = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsNumber(j) ? j->valueint : def;
}

typedef struct {
    char *buf;
    size_t len, cap;
} sbuf;

static int sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *nb = realloc(b->buf, cap);
        if (!nb) return -1;
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return 0;
}

static int sb_put_value(sbuf *b, const char *v, int url_encode)
{
    if (!url_encode) return sb_put(b, v, strlen(v));
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        char e[4];
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            e[0] = (char)*p;
            if (sb_put(b, e, 1) != 0) return -1;
        } else {
            snprintf(e, sizeof e, "%%%02X", *p);
            if (sb_put(b, e, 3) != 0) return -1;
        }
    }
    return 0;
}

/* {device} and {command} replaced (percent-encoded in a URL path) */
static char *expand(const char *tmpl, const char *dev_name, const char *command, int url_encode)
{
    sbuf b = {0};
    if (sb_put(&b, "", 0) != 0) return NULL;
    for (const char *p = tmpl; *p;) {
        if (strncmp(p, "{device}", 8) == 0) {
            if (sb_put_value(&b, dev_name, url_encode) != 0) goto fail;
            p += 8;
        } else if (strncmp(p, "{command}", 9) == 0) {
            if (sb_put_value(&b, command, url_encode) != 0) goto fail;
            p += 9;
        } else {
            if (sb_put(&b, p, 1) != 0) goto fail;
            p++;
        }
    }
    return b.buf;
fail:
    free(b.buf);
    return NULL;
}

static void base64(const unsigned char *in, size_t n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0) |
                     (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = '\0';
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* TCP payload: "hex" digits (spaces and ':' between bytes ignored), or text with the
 * escapes \r \n \t \\ \xHH */
static char *tcp_bytes(const char *src, int hex, size_t *out_len)
{
    size_t n = strlen(src);
    char *out = malloc(n + 1);
    size_t o = 0;
    if (!out) return NULL;
    if (hex) {
        int hi = -1;
        for (const char *p = src; *p; p++) {
            int v = hexval((unsigned char)*p);
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else { out[o++] = (char)(hi << 4 | v); hi = -1; }
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            if (src[i] == '\\' && i + 1 < n) {
                char c = src[i + 1];
                if (c == 'r') { out[o++] = '\r'; i++; continue; }
                if (c == 'n') { out[o++] = '\n'; i++; continue; }
                if (c == 't') { out[o++] = '\t'; i++; continue; }
                if (c == '\\') { out[o++] = '\\'; i++; continue; }
                if (c == 'x' && i + 3 < n && hexval((unsigned char)src[i + 2]) >= 0 &&
                        hexval((unsigned char)src[i + 3]) >= 0) {
                    out[o++] = (char)(hexval((unsigned char)src[i + 2]) << 4 | hexval((unsigned char)src[i + 3]));
                    i += 3;
                    continue;
                }
            }
            out[o++] = src[i];
        }
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

static net_job *job_build(const cJSON *dev, const cJSON *cmd, const char *dev_id, const char *dev_name,
                          const char *command, char *err, size_t err_sz)
{
    int kind = net_device_kind(dev);
    if (!kind) {
        snprintf(err, err_sz, "not an HTTP or TCP device");
        return NULL;
    }
    net_job *j = calloc(1, sizeof *j);
    if (!j) {
        snprintf(err, err_sz, "out of memory");
        return NULL;
    }
    j->kind = kind;
    snprintf(j->host, sizeof j->host, "%s", jstr(dev, "netHost"));
    j->port = jint(dev, "netPort", 0);
    j->tls = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(dev, "netTls"));
    const char *v = jstr(dev, "netTlsVerify");
    j->verify = strcmp(v, "pin") == 0 ? NET_TLS_PIN : strcmp(v, "insecure") == 0 ? NET_TLS_INSECURE : NET_TLS_VERIFY;
    j->timeout_ms = jint(dev, "netTimeoutMs", NET_TIMEOUT_DEFAULT_MS);
    if (j->timeout_ms < NET_TIMEOUT_MIN_MS || j->timeout_ms > NET_TIMEOUT_MAX_MS) j->timeout_ms = NET_TIMEOUT_DEFAULT_MS;
    snprintf(j->label, sizeof j->label, "%s (%s) %s", dev_name && dev_name[0] ? dev_name : "device",
             dev_id ? dev_id : "", cmd ? command : "connection test");
    if (!j->host[0] || j->port <= 0 || j->port > 65535) {
        snprintf(err, err_sz, "the device has no host or port");
        job_free(j);
        return NULL;
    }
    if (!cmd) {
        j->connect_only = 1;
        return j;
    }
    const char *name = dev_name ? dev_name : "";
    if (kind == NET_KIND_TCP) {
        j->payload = tcp_bytes(jstr(cmd, "tcpPayload"), strcmp(jstr(cmd, "tcpFormat"), "hex") == 0, &j->payload_len);
        char *expanded = NULL;
        if (j->payload && strcmp(jstr(cmd, "tcpFormat"), "hex") != 0) {
            /* placeholders only in text; the decoded bytes have no NUL unless \x00 was asked for */
            if (memchr(j->payload, '\0', j->payload_len) == NULL) {
                expanded = expand(j->payload, name, command, 0);
                if (expanded) {
                    free(j->payload);
                    j->payload = expanded;
                    j->payload_len = strlen(expanded);
                }
            }
        }
        if (!j->payload) {
            snprintf(err, err_sz, "out of memory");
            job_free(j);
            return NULL;
        }
        return j;
    }

    /* HTTP */
    snprintf(j->method, sizeof j->method, "%s", jstr(cmd, "httpMethod")[0] ? jstr(cmd, "httpMethod") : "GET");
    char *path = expand(jstr(cmd, "httpPath")[0] ? jstr(cmd, "httpPath") : "/", name, command, 1);
    j->payload = expand(jstr(cmd, "httpBody"), name, command, 0);
    if (!path || !j->payload) {
        free(path);
        snprintf(err, err_sz, "out of memory");
        job_free(j);
        return NULL;
    }
    j->payload_len = strlen(j->payload);
    size_t ul = strlen(path) + strlen(j->host) + 32;
    j->url = malloc(ul);
    if (j->url) {
        const char *br_l = strchr(j->host, ':') ? "[" : "", *br_r = strchr(j->host, ':') ? "]" : "";
        snprintf(j->url, ul, "%s://%s%s%s:%d%s", j->tls ? "https" : "http", br_l, j->host, br_r, j->port,
                 path[0] == '/' ? path : "/");
    }
    free(path);
    if (!j->url) {
        snprintf(err, err_sz, "out of memory");
        job_free(j);
        return NULL;
    }

    int nh = 0, has_ct = 0;
    const cJSON *hdrs = cJSON_GetObjectItemCaseSensitive(dev, "httpHeaders");
    const cJSON *h = NULL;
    cJSON_ArrayForEach(h, hdrs) {
        if (nh >= NET_MAX_HEADERS) break;
        const char *hn = jstr(h, "name");
        if (!hn[0]) continue;
        if (strcasecmp(hn, "Content-Type") == 0) has_ct = 1;
        if (asprintf(&j->headers[nh], "%s: %s", hn, jstr(h, "value")) < 0) j->headers[nh] = NULL;
        else nh++;
    }
    const char *auth = jstr(dev, "httpAuthType");
    if (strcmp(auth, "basic") == 0) {
        char up[400], b64[540];
        snprintf(up, sizeof up, "%s:%s", jstr(dev, "httpUser"), jstr(dev, "httpPassword"));
        base64((const unsigned char *)up, strlen(up), b64);
        if (asprintf(&j->headers[nh], "Authorization: Basic %s", b64) >= 0) nh++;
        else j->headers[nh] = NULL;
    } else if (strcmp(auth, "bearer") == 0 && jstr(dev, "httpToken")[0]) {
        if (asprintf(&j->headers[nh], "Authorization: Bearer %s", jstr(dev, "httpToken")) >= 0) nh++;
        else j->headers[nh] = NULL;
    }
    if (j->payload_len > 0 && !has_ct) {
        const char *ct = jstr(cmd, "httpContentType");
        if (!ct[0]) ct = (j->payload[0] == '{' || j->payload[0] == '[') ? "application/json" : "text/plain; charset=utf-8";
        if (asprintf(&j->headers[nh], "Content-Type: %s", ct) >= 0) nh++;
        else j->headers[nh] = NULL;
    }
    return j;
}

/* ---- Pinned keys (pin mode): /data/harmony/config/tls_pins.json, {"host:port": "hex"} ---- */

static const char *pins_path(void)
{
    return config_store_path("config/" NET_PINS_FILE_NAME);
}

static cJSON *pins_load(void)
{
    FILE *f = fopen(pins_path(), "rb");
    cJSON *o = NULL;
    if (f) {
        char buf[16384];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        fclose(f);
        o = cJSON_Parse(buf);
    }
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        o = cJSON_CreateObject();
    }
    return o;
}

static int pins_save(cJSON *o)
{
    char *txt = cJSON_Print(o);
    int rc = txt ? atomic_file_write_str(pins_path(), txt) : -1;
    free(txt);
    return rc;
}

static void pin_key(const char *host, int port, char *out, size_t sz)
{
    snprintf(out, sz, "%s:%d", host, port);
}

/* ---- Running a job (worker thread only) ---- */

static void result_set(net_job *j, int ok, int status, const char *error, const char *response,
                       const char *pin, int pinned)
{
    if (!j->sync) return;
    cJSON *r = cJSON_CreateObject();
    if (!r) return;
    cJSON_AddBoolToObject(r, "ok", ok);
    if (status > 0) cJSON_AddNumberToObject(r, "status", status);
    if (error && error[0]) cJSON_AddStringToObject(r, "error", error);
    if (response) cJSON_AddStringToObject(r, "response", response);
    if (pin && pin[0]) cJSON_AddStringToObject(r, "tlsFingerprint", pin);
    if (pinned) cJSON_AddBoolToObject(r, "pinned", 1);
    j->result = r;
}

/* printable text of a response head for the test result */
static void printable(char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f) s[i] = '.';
    }
    s[n] = '\0';
}

/* ---- TLS: hub_fetch does it (hub_daemon carries no BearSSL, tls_client.c is built with
 * NET_NO_TLS here). The request goes in files of a private folder in /tmp: the headers hold
 * credentials, which stay out of the process list. ---- */

static int write_text_file(const char *path, const char *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    return close(fd);
}

/* 0 when the device answered (any HTTP status, in *status), -1 with err. addrs: the checked
 * addresses of j->host (hub_fetch connects there; the name stays for SNI and Host) */
static int run_fetch(net_job *j, const char *addrs, const char *pin_expect, int *status, char *err, size_t err_sz,
                     char *head, size_t head_sz, char *pin_out)
{
    *status = 0;
    head[0] = '\0';
    if (!hub_fetch_available()) {
        snprintf(err, err_sz, "TLS needs hub_fetch and libbearssl.so.0 (update the Hub software)");
        return -1;
    }
    char dir[] = "/tmp/hub_net.XXXXXX";
    if (!mkdtemp(dir)) {
        snprintf(err, err_sz, "no room in /tmp");
        return -1;
    }
    char hpath[64], dpath[64], opath[64], spath[64];
    snprintf(hpath, sizeof hpath, "%s/h", dir);
    snprintf(dpath, sizeof dpath, "%s/d", dir);
    snprintf(opath, sizeof opath, "%s/o", dir);
    snprintf(spath, sizeof spath, "%s/s", dir);

    char hdrs[8192]; /* what hub_fetch --header-file reads at most */
    size_t hl = 0;
    hdrs[0] = '\0';
    int rc = -1;
    for (int i = 0; j->headers[i]; i++) {
        int n = snprintf(hdrs + hl, sizeof hdrs - hl, "%s\n", j->headers[i]);
        if (n < 0 || (size_t)n >= sizeof hdrs - hl) {
            /* Authorization comes last: never send the request without it */
            snprintf(err, err_sz, "headers too long");
            goto out;
        }
        hl += (size_t)n;
    }
    if (write_text_file(hpath, hdrs, hl) != 0 ||
            write_text_file(dpath, j->payload ? j->payload : "", j->connect_only ? 0 : j->payload_len) != 0) {
        snprintf(err, err_sz, "no room in /tmp");
        goto out;
    }
    char tsec[12], port[8];
    snprintf(tsec, sizeof tsec, "%d", (j->timeout_ms + 999) / 1000);
    snprintf(port, sizeof port, "%d", j->port);
    char *argv[32];
    int a = 0;
    argv[a++] = (char *)hub_fetch_path();
    argv[a++] = "-T";
    argv[a++] = tsec;
    if (j->verify == NET_TLS_INSECURE) {
        argv[a++] = "--insecure";
    } else if (j->verify == NET_TLS_PIN) {
        if (pin_expect[0]) { argv[a++] = "--pin"; argv[a++] = (char *)pin_expect; }
        else argv[a++] = "--pin-any";
    } else {
        argv[a++] = "--ca";
        argv[a++] = TLS_DEFAULT_CA_BUNDLE;
    }
    argv[a++] = "--connect-to";
    argv[a++] = (char *)addrs;
    argv[a++] = "--data-file";
    argv[a++] = dpath;
    argv[a++] = "--status-file";
    argv[a++] = spath;
    argv[a++] = "-o";
    argv[a++] = opath;
    if (j->kind == NET_KIND_HTTP && !j->connect_only) {
        argv[a++] = "--header-file";
        argv[a++] = hpath;
        argv[a++] = "--no-redirect";
        argv[a++] = "--max";
        argv[a++] = "512";
        argv[a++] = "-X";
        argv[a++] = j->method;
        argv[a++] = j->url;
    } else {
        /* a TCP device, or a connection test: connect, TLS handshake, send, one read */
        argv[a++] = "--tcp";
        argv[a++] = "--tls";
        if (j->sync && !j->connect_only) { argv[a++] = "--read"; argv[a++] = "512"; }
        argv[a++] = j->host;
        argv[a++] = port;
    }
    argv[a] = NULL;
    int run = run_argv_limited(argv, opath, 65536, (unsigned)j->timeout_ms * 3 + 2000);
    if (run == RUN_TOO_SLOW) {
        snprintf(err, err_sz, "timed out");
        goto out;
    }
    FILE *f = fopen(spath, "r");
    if (!f) {
        snprintf(err, err_sz, "hub_fetch did not run (exit %d)", run);
        goto out;
    }
    char line[256];
    char ferr[160] = "";
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "status=", 7) == 0) *status = atoi(line + 7);
        else if (strncmp(line, "fingerprint=", 12) == 0) snprintf(pin_out, NET_PIN_HEX_LEN + 1, "%s", line + 12);
        else if (strncmp(line, "error=", 6) == 0) snprintf(ferr, sizeof ferr, "%s", line + 6);
    }
    fclose(f);
    FILE *o = fopen(opath, "rb");
    if (o) {
        size_t n = fread(head, 1, head_sz - 1, o);
        fclose(o);
        printable(head, n);
    }
    if (*status > 0 || !ferr[0]) {
        rc = 0;
        if (*status >= 400) snprintf(err, err_sz, "HTTP %d", *status);
    } else {
        snprintf(err, err_sz, "%s", ferr);
    }
out:
    unlink(hpath);
    unlink(dpath);
    unlink(opath);
    unlink(spath);
    rmdir(dir);
    return rc;
}

/* Up to this many addresses of a device are kept, in the resolver's order; an address with an
 * IPv6 scope fits in 64 characters */
#define NET_ADDRS_MAX 4
#define NET_ADDR_LEN 64
#define NET_ADDRS_LEN (NET_ADDRS_MAX * (NET_ADDR_LEN + 1))

/* The Hub itself: loopback or unspecified, also IPv4-mapped (::ffff:127.0.0.1) */
static int addr_is_local(const struct addrinfo *ai)
{
    if (ai->ai_family == AF_INET) {
        uint32_t a = ntohl(((const struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr);
        return (a >> 24) == 127 || a == 0;
    }
    if (ai->ai_family == AF_INET6) {
        const struct in6_addr *a6 = &((const struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;
        return IN6_IS_ADDR_LOOPBACK(a6) || IN6_IS_ADDR_UNSPECIFIED(a6) ||
               (IN6_IS_ADDR_V4MAPPED(a6) && (a6->s6_addr[12] == 127 ||
                (a6->s6_addr[12] | a6->s6_addr[13] | a6->s6_addr[14] | a6->s6_addr[15]) == 0));
    }
    return 0;
}

#ifdef TEST_HARNESS
/* The host tests run their devices on 127.0.0.1: a loopback device is allowed only on a port
 * listed in HARMONY_TEST_NET_LOOPBACK_PORTS (comma separated), so the refusal stays tested */
static int test_loopback_port_allowed(int port)
{
    const char *p = getenv("HARMONY_TEST_NET_LOOPBACK_PORTS");
    while (p && *p) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) {
            p++;
            continue;
        }
        if (v == port) return 1;
        p = end;
    }
    return 0;
}
#else
#define test_loopback_port_allowed(port) ((void)(port), 0)
#endif

/* Resolves the device's host once per send. The check and the connection use the same
 * addresses: a second lookup took another resolver timeout (the device timeout does not cover
 * it) and could answer differently. 0 with the addresses, numeric and comma separated, in out;
 * -1 when the host does not resolve; -2 when an address is the Hub itself (also as 2130706433,
 * 0x7f000001, ::ffff:127.0.0.1 or a DNS name that points there): a device there could call the
 * Hub's own API. */
static int resolve_checked(const net_job *j, char *out, size_t out_sz)
{
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    out[0] = '\0';
    if (getaddrinfo(j->host, NULL, &hints, &res) != 0 || !res) return -1;
    int rc = 0, n = 0;
    size_t len = 0;
    for (ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        if (addr_is_local(ai) && !test_loopback_port_allowed(j->port)) {
            rc = -2;
            break;
        }
        if (n >= NET_ADDRS_MAX) continue; /* every address is still checked */
        char a[NET_ADDR_LEN];
        if (getnameinfo(ai->ai_addr, ai->ai_addrlen, a, sizeof a, NULL, 0, NI_NUMERICHOST) != 0) continue;
        int w = snprintf(out + len, out_sz - len, "%s%s", len ? "," : "", a);
        if (w < 0 || (size_t)w >= out_sz - len) {
            out[len] = '\0';
            continue;
        }
        len += (size_t)w;
        n++;
    }
    freeaddrinfo(res);
    if (rc == 0 && n == 0) rc = -1;
    return rc;
}

/* Returns 1 when the device did not answer in time (connect, handshake or reply) */
static int job_run(net_job *j)
{
    char addrs[NET_ADDRS_LEN];
    int rr = resolve_checked(j, addrs, sizeof addrs);
    if (rr == -2) {
        printf("[!] %s refused: %s is the Hub itself\n", j->label, j->host);
        result_set(j, 0, 0, "the host is the Hub itself (loopback): not a device", NULL, NULL, 0);
        return 0;
    }
    if (rr != 0) {
        char rerr[300];
        snprintf(rerr, sizeof rerr, "cannot resolve %s", j->host);
        printf("[!] %s: %s\n", j->label, rerr);
        result_set(j, 0, 0, rerr, NULL, NULL, 0);
        return 0;
    }
    char pin_expect[NET_PIN_HEX_LEN + 1] = "";
    char key[300];
    pin_key(j->host, j->port, key, sizeof key);
    if (j->tls && j->verify == NET_TLS_PIN) {
        cJSON *pins = pins_load();
        snprintf(pin_expect, sizeof pin_expect, "%s", jstr(pins, key));
        cJSON_Delete(pins);
    }
    net_conn_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.tls = j->tls;
    opts.verify = j->verify;
    opts.pin = pin_expect;
    opts.timeout_ms = j->timeout_ms;
    opts.connect_addrs = addrs;   /* the checked addresses, no second lookup */

    char pin[NET_PIN_HEX_LEN + 1] = "";
    char err[160] = "";
    char head[NET_RESPONSE_HEAD + 1] = "";
    int ok = 0, status = 0;

    if (j->tls) {
        int rc = run_fetch(j, addrs, pin_expect, &status, err, sizeof err, head, sizeof head, pin);
        ok = rc == 0 && status < 400;
        const char *kind = j->kind == NET_KIND_HTTP ? "HTTPS" : "TCP/TLS";
        if (j->kind == NET_KIND_HTTP && !j->connect_only && rc == 0) {
            /* the log names host and path, never the query (it may hold a key) */
            const char *path = strchr(j->url + 8, '/');
            size_t plen = path ? strcspn(path, "?") : 0;
            printf("[*] %s %s: %s %.*s -> %d\n", kind, j->label, j->method, (int)plen, path ? path : "", status);
        } else if (ok) {
            printf("[*] %s %s: %s:%d ok\n", kind, j->label, j->host, j->port);
        } else {
            printf("[!] %s %s: %s:%d failed: %s\n", kind, j->label, j->host, j->port, err);
        }
    } else if (j->kind == NET_KIND_HTTP && !j->connect_only) {
        http_req req;
        memset(&req, 0, sizeof req);
        req.method = j->method;
        for (int i = 0; i < HTTP_MAX_HEADERS - 1 && j->headers[i]; i++) req.headers[i] = j->headers[i];
        req.body = j->payload;
        req.body_len = j->payload_len;
        req.out_fd = -1;
        req.out_buf = head;
        req.out_sz = sizeof head;
        req.conn = opts;
        http_resp resp;
        int rc = http_request(j->url, &req, &resp);
        snprintf(pin, sizeof pin, "%s", resp.pin);
        status = resp.status;
        ok = rc == 0 && status < 400;
        if (rc != 0) snprintf(err, sizeof err, "%s", resp.err);
        else if (status >= 400) snprintf(err, sizeof err, "HTTP %d", status);
        printable(head, resp.body_len);
        /* the log names host and path, never the query (it may hold a key) */
        const char *path = strchr(j->url + 8, '/');
        size_t plen = path ? strcspn(path, "?") : 0;
        if (rc == 0) printf("[*] HTTP %s: %s %.*s -> %d\n", j->label, j->method, (int)plen, path ? path : "", status);
        else printf("[!] HTTP %s: %s %s:%d failed: %s\n", j->label, j->method, j->host, j->port, err);
    } else {
        net_conn *c = net_conn_open(j->host, j->port, &opts, pin, err, sizeof err);
        if (c) {
            ok = 1;
            if (!j->connect_only && j->payload_len > 0 && net_conn_write(c, j->payload, j->payload_len) != 0) {
                ok = 0;
                snprintf(err, sizeof err, "%s", net_conn_error(c));
            }
            if (ok && j->sync && !j->connect_only && j->kind == NET_KIND_TCP) {
                ssize_t n = net_conn_read(c, head, NET_RESPONSE_HEAD);
                if (n > 0) printable(head, (size_t)n);
                else head[0] = '\0';
            }
            net_conn_close(c);
        }
        if (j->connect_only) printf("[*] %s %s: connection to %s:%d %s %s\n", j->kind == NET_KIND_HTTP ? "HTTP" : "TCP",
                                    j->label, j->host, j->port, ok ? "ok" : "failed:", ok ? "" : err);
        else if (ok) printf("[*] TCP %s: %lu bytes to %s:%d\n", j->label, (unsigned long)j->payload_len, j->host, j->port);
        else printf("[!] TCP %s: %s:%d failed: %s\n", j->label, j->host, j->port, err);
    }

    /* Trust on first use: the first key seen for this host:port is kept */
    int pinned = 0;
    if (j->tls && j->verify == NET_TLS_PIN && !pin_expect[0] && pin[0] && (ok || status > 0)) {
        pthread_mutex_lock(&s_pins_lock);
        cJSON *pins = pins_load();
        cJSON_DeleteItemFromObjectCaseSensitive(pins, key);
        cJSON_AddStringToObject(pins, key, pin);
        if (pins_save(pins) == 0) {
            pinned = 1;
            printf("[*] TLS key of %s pinned (%s)\n", key, pin);
        }
        cJSON_Delete(pins);
        pthread_mutex_unlock(&s_pins_lock);
    }
    result_set(j, ok, status, err, (j->sync && !j->connect_only) ? head : NULL, pin, pinned);
    return !ok && strstr(err, "timed out") != NULL;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_lock);
        net_job *j = NULL;
        if (s_count > 0) {
            j = s_queue[s_head];
            s_head = (s_head + 1) % NET_QUEUE_LEN;
            s_count--;
        }
        pthread_mutex_unlock(&s_lock);
        if (!j) {
            /* the timeout only guards against a lost wake byte */
            struct pollfd p = { s_wake[0], POLLIN, 0 };
            if (poll(&p, 1, 1000) > 0) {
                char b[16];
                while (read(s_wake[0], b, sizeof b) > 0) {}
            }
            continue;
        }

        uint64_t now = mono_ms();
        if (!j->sync && j->live_key) {
            /* a live key press: late or behind a device that just timed out, it is dropped */
            int si = slow_slot(j);
            const char *why = now - j->queued_ms > NET_QUEUE_MAX_AGE_MS ? "waited too long in the queue" :
                              (si >= 0 && now < s_slow[si].until_ms) ? "the device timed out just before" : NULL;
            if (why) {
                printf("[!] %s dropped: %s\n", j->label, why);
                fflush(stdout);
                job_free(j);
                continue;
            }
        }
        slow_note(j, job_run(j), mono_ms());
        fflush(stdout);

        if (!j->sync) {
            job_free(j);
            continue;
        }
        pthread_mutex_lock(&s_lock);
        j->done = 1;
        int abandoned = j->abandoned;
        pthread_mutex_unlock(&s_lock);
        if (abandoned) job_free(j);
    }
    return NULL;
}

static int enqueue(net_job *j)
{
    pthread_mutex_lock(&s_lock);
    if (!s_started) {
        if (pipe(s_wake) != 0) {
            pthread_mutex_unlock(&s_lock);
            return -1;
        }
        for (int i = 0; i < 2; i++) {
            fcntl(s_wake[i], F_SETFL, fcntl(s_wake[i], F_GETFL) | O_NONBLOCK);
            fcntl(s_wake[i], F_SETFD, FD_CLOEXEC);
        }
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 128 * 1024);
        if (pthread_create(&t, &attr, worker, NULL) == 0) s_started = 1;
        pthread_attr_destroy(&attr);
        if (!s_started) {
            close(s_wake[0]);
            close(s_wake[1]);
            s_wake[0] = s_wake[1] = -1;
            pthread_mutex_unlock(&s_lock);
            return -1;
        }
    }
    net_job *late = NULL;
    if (s_count >= NET_QUEUE_LEN && j->live_key && !j->sync) {
        /* Full, and this is a key pressed now: the oldest key press that is over 1 s old makes
         * room (it is late for a key pressed now). Never a sync job or any other send. */
        uint64_t now = mono_ms();
        for (int k = 0; k < s_count; k++) {
            net_job *q = s_queue[(s_head + k) % NET_QUEUE_LEN];
            if (!q->live_key || q->sync || now - q->queued_ms <= NET_QUEUE_ROOM_AGE_MS) continue;
            late = q;
            for (int m = k; m < s_count - 1; m++)
                s_queue[(s_head + m) % NET_QUEUE_LEN] = s_queue[(s_head + m + 1) % NET_QUEUE_LEN];
            s_count--;
            break;
        }
    }
    if (s_count >= NET_QUEUE_LEN) {
        pthread_mutex_unlock(&s_lock);
        return -3;
    }
    j->queued_ms = mono_ms();
    s_queue[(s_head + s_count) % NET_QUEUE_LEN] = j;
    s_count++;
    pthread_mutex_unlock(&s_lock);
    if (late) {
        printf("[!] %s dropped: waited too long in the queue (queue full)\n", late->label);
        fflush(stdout);
        job_free(late);
    }
    ssize_t w = write(s_wake[1], "w", 1);   /* a full pipe already holds a wake */
    (void)w;
    return 0;
}

int net_action_send(const cJSON *dev, const cJSON *cmd, const char *dev_id, const char *dev_name,
                    const char *command, int live_key)
{
    char err[160];
    net_job *j = job_build(dev, cmd, dev_id, dev_name, command, err, sizeof err);
    if (!j) {
        printf("[!] %s %s (%s) '%s' skipped: %s\n", net_device_kind(dev) == NET_KIND_TCP ? "TCP" : "HTTP",
               dev_name ? dev_name : "", dev_id ? dev_id : "", command, err);
        return -2;
    }
    if (g_dry_run) {
        int nh = 0;
        while (j->headers[nh]) nh++;
        if (j->kind == NET_KIND_HTTP)
            printf("[DRY-RUN] HTTP %s %s (%d headers, %lu body bytes) for %s\n", j->method, j->url, nh,
                   (unsigned long)j->payload_len, j->label);
        else
            printf("[DRY-RUN] TCP %s:%d%s %lu bytes for %s\n", j->host, j->port, j->tls ? " (TLS)" : "",
                   (unsigned long)j->payload_len, j->label);
        fflush(stdout);
        job_free(j);
        return 0;
    }
    j->live_key = live_key;
    int rc = enqueue(j);
    if (rc != 0) {
        printf("[!] %s: send queue full, command dropped\n", j->label);
        fflush(stdout);
        job_free(j);
        return -3;
    }
    return 0;
}

cJSON *net_action_test(const cJSON *dev, const cJSON *cmd, const char *dev_id, const char *dev_name)
{
    char err[160];
    const char *command = cmd ? jstr(cmd, "name") : "";
    net_job *j = job_build(dev, cmd, dev_id, dev_name, command, err, sizeof err);
    if (!j) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 0);
        cJSON_AddStringToObject(r, "error", err);
        return r;
    }
    j->sync = 1;
    /* connect + handshake + one read, each up to the timeout, plus the queue ahead */
    int wait_ms = j->timeout_ms * 4 + 2000;
    if (enqueue(j) != 0) {
        job_free(j);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 0);
        cJSON_AddStringToObject(r, "error", "send queue full");
        return r;
    }
    for (int waited = 0;; waited += 20) {
        pthread_mutex_lock(&s_lock);
        int done = j->done;
        if (done || waited >= wait_ms) break;   /* s_lock stays held */
        pthread_mutex_unlock(&s_lock);
        usleep(20000);
    }
    cJSON *r = NULL;
    if (j->done) {
        r = j->result;
        j->result = NULL;
        pthread_mutex_unlock(&s_lock);
        job_free(j);
    } else {
        j->abandoned = 1; /* the worker frees it */
        pthread_mutex_unlock(&s_lock);
    }
    if (!r) {
        r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 0);
        cJSON_AddStringToObject(r, "error", "timed out");
    }
    return r;
}

int net_action_forget_pin(const cJSON *dev)
{
    char key[300];
    pin_key(jstr(dev, "netHost"), jint(dev, "netPort", 0), key, sizeof key);
    pthread_mutex_lock(&s_pins_lock);
    cJSON *pins = pins_load();
    int rc = 1;
    if (cJSON_GetObjectItemCaseSensitive(pins, key)) {
        cJSON_DeleteItemFromObjectCaseSensitive(pins, key);
        rc = pins_save(pins) == 0 ? 0 : -1;
    }
    cJSON_Delete(pins);
    pthread_mutex_unlock(&s_pins_lock);
    return rc;
}
