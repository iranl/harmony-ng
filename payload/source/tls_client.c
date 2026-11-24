/* Outbound TCP and TLS connections (BearSSL). See tls_client.h.
 * Built with -DNET_NO_TLS (hub_daemon): plain TCP only, without BearSSL; a TLS connection
 * fails with an error, and the caller runs hub_fetch for it instead. */
#include "tls_client.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#ifndef NET_NO_TLS
#include "bearssl.h"
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef NET_NO_TLS
/* ---- Trust anchors (PEM bundle, loaded once per path) ---- */

typedef struct {
    unsigned char *buf;
    size_t len, cap;
    int oom;
} bvec;

static void bvec_append(void *ctx, const void *data, size_t len)
{
    bvec *v = ctx;
    if (v->oom) return;
    if (v->len + len > v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 1024;
        while (cap < v->len + len) cap *= 2;
        unsigned char *n = realloc(v->buf, cap);
        if (!n) { v->oom = 1; return; }
        v->buf = n;
        v->cap = cap;
    }
    memcpy(v->buf + v->len, data, len);
    v->len += len;
}

static unsigned char *dup_bytes(const unsigned char *p, size_t n)
{
    unsigned char *d = malloc(n ? n : 1);
    if (d && n) memcpy(d, p, n);
    return d;
}

static br_x509_trust_anchor *g_tas;
static size_t g_ta_count;
static char g_ta_path[256];
static int g_ta_loaded;

static void free_anchor(br_x509_trust_anchor *ta)
{
    free(ta->dn.data);
    if (ta->pkey.key_type == BR_KEYTYPE_RSA) {
        free(ta->pkey.key.rsa.n);
        free(ta->pkey.key.rsa.e);
    } else {
        free(ta->pkey.key.ec.q);
    }
}

static void add_anchor(const unsigned char *der, size_t len, br_x509_trust_anchor **tas,
                       size_t *count, size_t *cap)
{
    br_x509_decoder_context dc;
    bvec dn = {0};
    br_x509_decoder_init(&dc, bvec_append, &dn);
    br_x509_decoder_push(&dc, der, len);
    const br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || dn.oom || br_x509_decoder_last_error(&dc) != 0) {
        free(dn.buf);
        return;
    }
    br_x509_trust_anchor ta;
    memset(&ta, 0, sizeof ta);
    ta.dn.data = dn.buf;
    ta.dn.len = dn.len;
    ta.flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta.pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta.pkey.key.rsa.n = dup_bytes(pk->key.rsa.n, pk->key.rsa.nlen);
        ta.pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta.pkey.key.rsa.e = dup_bytes(pk->key.rsa.e, pk->key.rsa.elen);
        ta.pkey.key.rsa.elen = pk->key.rsa.elen;
        if (!ta.pkey.key.rsa.n || !ta.pkey.key.rsa.e) { free_anchor(&ta); return; }
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta.pkey.key.ec.curve = pk->key.ec.curve;
        ta.pkey.key.ec.q = dup_bytes(pk->key.ec.q, pk->key.ec.qlen);
        ta.pkey.key.ec.qlen = pk->key.ec.qlen;
        if (!ta.pkey.key.ec.q) { free_anchor(&ta); return; }
    } else {
        free(dn.buf);
        return;
    }
    if (*count == *cap) {
        size_t ncap = *cap ? *cap * 2 : 64;
        br_x509_trust_anchor *n = realloc(*tas, ncap * sizeof *n);
        if (!n) { free_anchor(&ta); return; }
        *tas = n;
        *cap = ncap;
    }
    (*tas)[(*count)++] = ta;
}

static int load_anchors(const char *path)
{
    if (!path) path = TLS_DEFAULT_CA_BUNDLE;
    if (g_ta_loaded && strcmp(path, g_ta_path) == 0) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    br_x509_trust_anchor *tas = NULL;
    size_t count = 0, cap = 0;
    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    bvec der = {0};
    int in_cert = 0, bad = 0, done = 0;
    unsigned char buf[2048];
    while (!bad && !done) {
        size_t n = fread(buf, 1, sizeof buf, f);
        const unsigned char *p = buf;
        if (n == 0) {
            /* a last line without a newline still ends the object */
            buf[0] = '\n';
            n = 1;
            done = 1;
        }
        /* BearSSL's PEM decoder fails on CR: a bundle copied from a Windows checkout has CRLF */
        size_t k = 0;
        for (size_t j = 0; j < n; j++) {
            if (buf[j] != '\r') buf[k++] = buf[j];
        }
        n = k;
        while (n > 0 && !bad) {
            size_t used = br_pem_decoder_push(&pc, p, n);
            p += used;
            n -= used;
            switch (br_pem_decoder_event(&pc)) {
            case BR_PEM_BEGIN_OBJ: {
                const char *name = br_pem_decoder_name(&pc);
                in_cert = strcmp(name, "CERTIFICATE") == 0 || strcmp(name, "X509 CERTIFICATE") == 0;
                der.len = 0;
                br_pem_decoder_setdest(&pc, in_cert ? bvec_append : NULL, &der);
                break;
            }
            case BR_PEM_END_OBJ:
                if (in_cert && !der.oom) add_anchor(der.buf, der.len, &tas, &count, &cap);
                in_cert = 0;
                break;
            case BR_PEM_ERROR:
                bad = 1;
                break;
            default:
                break;
            }
        }
    }
    fclose(f);
    free(der.buf);
    if (bad && count == 0) {
        free(tas);
        return -1;
    }
    for (size_t i = 0; i < g_ta_count; i++) free_anchor(&g_tas[i]);
    free(g_tas);
    g_tas = tas;
    g_ta_count = count;
    snprintf(g_ta_path, sizeof g_ta_path, "%s", path);
    g_ta_loaded = 1;
    return 0;
}

/* ---- Certificate check: CA chain, pinned key, or none ---- */

typedef struct {
    const br_x509_class *vtable;
    br_x509_minimal_context mini;
    br_x509_decoder_context dec;
    br_x509_pkey pkey;
    int mode;
    int first;
    int have_key;
    int pin_mismatch;
    const char *expect;
    char pin_hex[NET_PIN_HEX_LEN + 1];
} cert_check;

static void cc_start_chain(const br_x509_class **ctx, const char *server_name)
{
    cert_check *c = (cert_check *)(void *)ctx;
    c->first = 1;
    c->have_key = 0;
    c->pin_mismatch = 0;
    c->pin_hex[0] = '\0';
    if (c->mode == NET_TLS_VERIFY) c->mini.vtable->start_chain(&c->mini.vtable, server_name);
}

static void cc_start_cert(const br_x509_class **ctx, uint32_t length)
{
    cert_check *c = (cert_check *)(void *)ctx;
    if (c->first) br_x509_decoder_init(&c->dec, NULL, NULL);
    if (c->mode == NET_TLS_VERIFY) c->mini.vtable->start_cert(&c->mini.vtable, length);
}

static void cc_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    cert_check *c = (cert_check *)(void *)ctx;
    if (c->first) br_x509_decoder_push(&c->dec, buf, len);
    if (c->mode == NET_TLS_VERIFY) c->mini.vtable->append(&c->mini.vtable, buf, len);
}

static void key_hash_hex(const br_x509_pkey *pk, char *out)
{
    unsigned char h[32];
    br_sha256_context sc;
    br_sha256_init(&sc);
    if (pk->key_type == BR_KEYTYPE_RSA) {
        br_sha256_update(&sc, pk->key.rsa.n, pk->key.rsa.nlen);
        br_sha256_update(&sc, pk->key.rsa.e, pk->key.rsa.elen);
    } else {
        br_sha256_update(&sc, pk->key.ec.q, pk->key.ec.qlen);
    }
    br_sha256_out(&sc, h);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", h[i]);
    out[NET_PIN_HEX_LEN] = '\0';
}

static void cc_end_cert(const br_x509_class **ctx)
{
    cert_check *c = (cert_check *)(void *)ctx;
    if (c->first) {
        const br_x509_pkey *pk = br_x509_decoder_get_pkey(&c->dec);
        if (pk && br_x509_decoder_last_error(&c->dec) == 0 &&
                (pk->key_type == BR_KEYTYPE_RSA || pk->key_type == BR_KEYTYPE_EC)) {
            /* the key points into dec, which is not reset before the next chain */
            c->pkey = *pk;
            c->have_key = 1;
            key_hash_hex(pk, c->pin_hex);
        }
        c->first = 0;
    }
    if (c->mode == NET_TLS_VERIFY) c->mini.vtable->end_cert(&c->mini.vtable);
}

static unsigned cc_end_chain(const br_x509_class **ctx)
{
    cert_check *c = (cert_check *)(void *)ctx;
    if (c->mode == NET_TLS_VERIFY) return c->mini.vtable->end_chain(&c->mini.vtable);
    if (!c->have_key) return BR_ERR_X509_NOT_TRUSTED;
    if (c->mode == NET_TLS_PIN && c->expect && c->expect[0] &&
            strcasecmp(c->expect, c->pin_hex) != 0) {
        c->pin_mismatch = 1;
        return BR_ERR_X509_NOT_TRUSTED;
    }
    return 0;
}

static const br_x509_pkey *cc_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    const cert_check *c = (const cert_check *)(const void *)ctx;
    if (c->mode == NET_TLS_VERIFY) return c->mini.vtable->get_pkey(&c->mini.vtable, usages);
    if (usages) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return &c->pkey;
}

static const br_x509_class cert_check_vtable = {
    sizeof(cert_check),
    cc_start_chain,
    cc_start_cert,
    cc_append,
    cc_end_cert,
    cc_end_chain,
    cc_get_pkey
};
#endif /* NET_NO_TLS */

/* ---- Connection ---- */

struct net_conn {
    int fd;
    int tls;
    int eof;
    int timed_out;
#ifndef NET_NO_TLS
    br_ssl_client_context sc;
    cert_check xc;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
#endif
    char err[160];
};

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    net_conn *c = ctx;
    for (;;) {
        ssize_t r = recv(c->fd, buf, len, 0);
        if (r > 0) return (int)r;
        if (r == 0) { c->eof = 1; return -1; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) c->timed_out = 1;
        return -1;
    }
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    net_conn *c = ctx;
    for (;;) {
        ssize_t w = send(c->fd, buf, len, MSG_NOSIGNAL);
        if (w > 0) return (int)w;
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) c->timed_out = 1;
        return -1;
    }
}

#ifndef NET_NO_TLS
static void set_tls_error(net_conn *c)
{
    int e = br_ssl_engine_last_error(&c->sc.eng);
    const char *m = NULL;
    if (c->xc.pin_mismatch) m = "server key does not match the pinned key";
    else if (c->timed_out) m = "timed out";
    else switch (e) {
    case BR_ERR_X509_NOT_TRUSTED: m = "certificate not trusted (no CA in the bundle)"; break;
    case BR_ERR_X509_BAD_SERVER_NAME: m = "certificate does not match the host name"; break;
    case BR_ERR_X509_EXPIRED: m = "certificate expired or not yet valid"; break;
    case BR_ERR_X509_FORBIDDEN_KEY_USAGE: m = "certificate key usage forbids TLS"; break;
    case BR_ERR_BAD_VERSION:
    case BR_ERR_UNSUPPORTED_VERSION: m = "server needs an unsupported TLS version (TLS 1.2 is needed to check its certificate)"; break;
    case BR_ERR_BAD_CIPHER_SUITE: m = "no common cipher suite"; break;
    case BR_ERR_IO: m = c->eof ? "connection closed by the server" : "network error"; break;
    default: break;
    }
    if (m) {
        snprintf(c->err, sizeof c->err, "TLS: %s", m);
    } else if (e >= BR_ERR_RECV_FATAL_ALERT && e < BR_ERR_RECV_FATAL_ALERT + 256) {
        snprintf(c->err, sizeof c->err, "TLS: server sent alert %d", e - BR_ERR_RECV_FATAL_ALERT);
    } else if (e >= BR_ERR_SEND_FATAL_ALERT && e < BR_ERR_SEND_FATAL_ALERT + 256) {
        snprintf(c->err, sizeof c->err, "TLS: handshake refused (alert %d)", e - BR_ERR_SEND_FATAL_ALERT);
    } else {
        snprintf(c->err, sizeof c->err, "TLS error %d", e);
    }
}
#endif

/* Tries each address of res in turn; the connected socket, or -1 with err */
static int connect_any(const struct addrinfo *res, const char *host, int port, int timeout_ms,
                       char *err, size_t err_sz)
{
    int fd = -1;
    for (const struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pfd = { fd, POLLOUT, 0 };
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            rc = poll(&pfd, 1, timeout_ms);
            if (rc == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) {
                rc = 0;
            } else {
                if (rc == 0) snprintf(err, err_sz, "connection to %s:%d timed out", host, port);
                else if (soerr == ECONNREFUSED) snprintf(err, err_sz, "connection to %s:%d refused", host, port);
                rc = -1;
            }
        }
        if (rc == 0) {
            fcntl(fd, F_SETFL, fl);
            return fd;
        }
        close(fd);
        fd = -1;
    }
    return -1;
}

static int tcp_connect(const char *host, const char *addrs, int port, int timeout_ms, char *err, size_t err_sz)
{
    struct addrinfo hints, *res = NULL;
    char ps[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    int fd = -1;
    if (addrs && addrs[0]) {
        /* addresses the caller resolved and checked: numeric only, never a second lookup */
        char list[512];
        snprintf(list, sizeof list, "%s", addrs);
        hints.ai_flags = AI_NUMERICHOST;
        int parsed = 0;
        snprintf(err, err_sz, "cannot connect to %s:%d", host, port);
        for (char *save = NULL, *a = strtok_r(list, ",", &save); a && fd < 0; a = strtok_r(NULL, ",", &save)) {
            if (getaddrinfo(a, ps, &hints, &res) != 0 || !res) continue;
            parsed = 1;
            fd = connect_any(res, host, port, timeout_ms, err, err_sz);
            freeaddrinfo(res);
            res = NULL;
        }
        if (!parsed) snprintf(err, err_sz, "bad address for %s", host);
    } else {
        int gai = getaddrinfo(host, ps, &hints, &res);
        if (gai != 0 || !res) {
            snprintf(err, err_sz, "cannot resolve %s", host);
            return -1;
        }
        snprintf(err, err_sz, "cannot connect to %s:%d", host, port);
        fd = connect_any(res, host, port, timeout_ms, err, err_sz);
        freeaddrinfo(res);
    }
    if (fd < 0) return -1;
    err[0] = '\0'; /* a failed earlier address left its message: the connection is up */
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

#ifndef NET_NO_TLS
/* Days from 0000-01-01 (BearSSL's epoch) for a Gregorian date */
static uint32_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    /* days since 1970-01-01, then + 719528 (days from year 0 to 1970) */
    return (uint32_t)(era * 146097 + (int)doe - 719468 + 719528);
}

static void set_validation_time(br_x509_minimal_context *mini)
{
    time_t now = time(NULL);
    uint32_t days = (uint32_t)(now / 86400) + 719528;
    uint32_t secs = (uint32_t)(now % 86400);
#ifdef HUB_BUILD_DATE_UTC
    /* a clock before the build date is not set yet (no SNTP answer): use the build date */
    uint32_t build = days_from_civil(HUB_BUILD_DATE_UTC / 10000, (HUB_BUILD_DATE_UTC / 100) % 100,
                                     HUB_BUILD_DATE_UTC % 100);
    if (days < build) {
        days = build;
        secs = 0;
    }
#else
    (void)days_from_civil;
#endif
    br_x509_minimal_set_time(mini, days, secs);
}

static int is_ip_literal(const char *host)
{
    struct in6_addr a6;
    struct in_addr a4;
    return inet_pton(AF_INET, host, &a4) == 1 || inet_pton(AF_INET6, host, &a6) == 1;
}

static void engine_setup(br_ssl_client_context *sc)
{
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
        BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
        BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,
        BR_TLS_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_RSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_RSA_WITH_AES_128_CBC_SHA256,
        BR_TLS_RSA_WITH_AES_128_CBC_SHA,
        BR_TLS_RSA_WITH_AES_256_CBC_SHA
    };
    br_ssl_client_zero(sc);
    /* TLS 1.0 and 1.1 only for old LAN devices (pin and insecure modes; verify mode raises the
     * minimum to 1.2 in net_conn_open) */
    br_ssl_engine_set_versions(&sc->eng, BR_TLS10, BR_TLS12);
    br_ssl_engine_set_suites(&sc->eng, suites, sizeof suites / sizeof suites[0]);
    br_ssl_client_set_default_rsapub(sc);
    br_ssl_engine_set_default_rsavrfy(&sc->eng);
    br_ssl_engine_set_default_ecdsa(&sc->eng);
    br_ssl_engine_set_hash(&sc->eng, br_md5_ID, &br_md5_vtable);
    br_ssl_engine_set_hash(&sc->eng, br_sha1_ID, &br_sha1_vtable);
    br_ssl_engine_set_hash(&sc->eng, br_sha224_ID, &br_sha224_vtable);
    br_ssl_engine_set_hash(&sc->eng, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(&sc->eng, br_sha384_ID, &br_sha384_vtable);
    br_ssl_engine_set_hash(&sc->eng, br_sha512_ID, &br_sha512_vtable);
    br_ssl_engine_set_prf10(&sc->eng, &br_tls10_prf);
    br_ssl_engine_set_prf_sha256(&sc->eng, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(&sc->eng, &br_tls12_sha384_prf);
    br_ssl_engine_set_default_aes_gcm(&sc->eng);
    br_ssl_engine_set_default_aes_cbc(&sc->eng);
    br_ssl_engine_set_default_chapol(&sc->eng);
}

/* Runs the engine until the handshake is done (application data can be sent). */
static int run_handshake(net_conn *c)
{
    br_ssl_engine_context *eng = &c->sc.eng;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        size_t len;
        if (st & BR_SSL_CLOSED) return -1;
        if (st & BR_SSL_SENDREC) {
            unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
            int w = sock_write(c, buf, len);
            if (w < 0) {
                br_ssl_engine_close(eng);
                return -1;
            }
            br_ssl_engine_sendrec_ack(eng, (size_t)w);
            continue;
        }
        if (st & BR_SSL_SENDAPP) return 0;
        if (st & BR_SSL_RECVREC) {
            unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &len);
            int r = sock_read(c, buf, len);
            if (r < 0) {
                br_ssl_engine_close(eng);
                return -1;
            }
            br_ssl_engine_recvrec_ack(eng, (size_t)r);
            continue;
        }
        return -1;
    }
}
#endif /* NET_NO_TLS */

net_conn *net_conn_open(const char *host, int port, const net_conn_opts *opts,
                        char *pin_out, char *err, size_t err_sz)
{
    if (pin_out) pin_out[0] = '\0';
    if (!host || !host[0] || port <= 0 || port > 65535) {
        snprintf(err, err_sz, "bad host or port");
        return NULL;
    }
    int timeout_ms = opts && opts->timeout_ms > 0 ? opts->timeout_ms : 5000;
    int tls = opts && opts->tls;
    int mode = opts ? opts->verify : NET_TLS_VERIFY;
#ifdef NET_NO_TLS
    (void)mode;
    if (tls) {
        snprintf(err, err_sz, "TLS is not built into this program (hub_fetch has it)");
        return NULL;
    }
#else
    if (tls && mode == NET_TLS_VERIFY && load_anchors(opts->ca_bundle) != 0) {
        snprintf(err, err_sz, "TLS: cannot read the CA bundle %s",
                 opts->ca_bundle ? opts->ca_bundle : TLS_DEFAULT_CA_BUNDLE);
        return NULL;
    }
#endif
    net_conn *c = calloc(1, sizeof *c);
    if (!c) {
        snprintf(err, err_sz, "out of memory");
        return NULL;
    }
    c->fd = tcp_connect(host, opts ? opts->connect_addrs : NULL, port, timeout_ms, err, err_sz);
    if (c->fd < 0) {
        free(c);
        return NULL;
    }
    c->tls = tls;
    if (!tls) return c;
#ifdef NET_NO_TLS
    return c;   /* not reached: refused above */
#else
    engine_setup(&c->sc);
    c->xc.vtable = &cert_check_vtable;
    c->xc.mode = mode;
    c->xc.expect = opts->pin;
    if (mode == NET_TLS_VERIFY) {
        /* a checked certificate means a current server: no TLS 1.0 / 1.1 (pin and insecure
         * modes are for old LAN devices) */
        br_ssl_engine_set_versions(&c->sc.eng, BR_TLS12, BR_TLS12);
        br_x509_minimal_init_full(&c->xc.mini, g_tas, g_ta_count);
        set_validation_time(&c->xc.mini);
    }
    br_ssl_engine_set_x509(&c->sc.eng, &c->xc.vtable);
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);
    /* No SNI for an IP address. Verify mode always passes the name: without it BearSSL
     * skips the name check, and a CA certificate for an IP literal is not supported anyway. */
    const char *sni = (mode != NET_TLS_VERIFY && is_ip_literal(host)) ? NULL : host;
    if (!br_ssl_client_reset(&c->sc, sni, 0)) {
        snprintf(err, err_sz, "TLS: engine reset failed");
        net_conn_close(c);
        return NULL;
    }
    br_sslio_init(&c->io, &c->sc.eng, sock_read, c, sock_write, c);
    int hs = run_handshake(c);
    if (pin_out && c->xc.pin_hex[0]) snprintf(pin_out, NET_PIN_HEX_LEN + 1, "%s", c->xc.pin_hex);
    if (hs != 0) {
        set_tls_error(c);
        snprintf(err, err_sz, "%s", c->err);
        net_conn_close(c);
        return NULL;
    }
    return c;
#endif
}

int net_conn_write(net_conn *c, const void *buf, size_t len)
{
    if (!c->tls) {
        const unsigned char *p = buf;
        while (len > 0) {
            int w = sock_write(c, p, len);
            if (w < 0) {
                snprintf(c->err, sizeof c->err, c->timed_out ? "write timed out" : "write failed");
                return -1;
            }
            p += w;
            len -= (size_t)w;
        }
        return 0;
    }
#ifndef NET_NO_TLS
    if (br_sslio_write_all(&c->io, buf, len) != 0 || br_sslio_flush(&c->io) != 0) {
        set_tls_error(c);
        return -1;
    }
#endif
    return 0;
}

ssize_t net_conn_read(net_conn *c, void *buf, size_t len)
{
    if (!c->tls) {
        int r = sock_read(c, buf, len);
        if (r < 0) {
            if (c->eof) return 0;
            snprintf(c->err, sizeof c->err, c->timed_out ? "read timed out" : "read failed");
            return -1;
        }
        return r;
    }
#ifdef NET_NO_TLS
    return -1;
#else
    int r = br_sslio_read(&c->io, buf, len);
    if (r > 0) return r;
    int e = br_ssl_engine_last_error(&c->sc.eng);
    /* close_notify, or a server that just closed the socket (common with Connection: close;
     * the HTTP layer checks Content-Length) */
    if (e == BR_ERR_OK || (e == BR_ERR_IO && c->eof)) return 0;
    set_tls_error(c);
    return -1;
#endif
}

void net_conn_close(net_conn *c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c);
}

const char *net_conn_error(const net_conn *c)
{
    return c->err[0] ? c->err : "error";
}
