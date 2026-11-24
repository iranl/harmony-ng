/* Minimal HTTP/1.1 client. See http_client.h. */
#define _GNU_SOURCE
#include "http_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

int http_text_safe(const char *s)
{
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 0x20 && ch != '\t') return 0;
        if (ch == 0x7f) return 0;
    }
    return 1;
}

int http_parse_url(const char *url, http_url *out)
{
    memset(out, 0, sizeof *out);
    if (!url) return -1;
    for (const char *p = url; *p; p++) {
        if ((unsigned char)*p <= 0x20 || *p == 0x7f) return -1;
    }
    const char *p;
    if (strncasecmp(url, "https://", 8) == 0) {
        out->tls = 1;
        out->port = 443;
        p = url + 8;
    } else if (strncasecmp(url, "http://", 7) == 0) {
        out->port = 80;
        p = url + 7;
    } else {
        return -1;
    }
    const char *slash = strpbrk(p, "/?#");
    size_t hostport_len = slash ? (size_t)(slash - p) : strlen(p);
    if (hostport_len == 0 || hostport_len >= sizeof out->host) return -1;
    char hp[256];
    memcpy(hp, p, hostport_len);
    hp[hostport_len] = '\0';
    if (strchr(hp, '@')) return -1;   /* no user:pass@ in URLs: use the auth settings */
    char *colon = NULL;
    if (hp[0] == '[') {               /* [IPv6]:port */
        char *end = strchr(hp, ']');
        if (!end) return -1;
        *end = '\0';
        snprintf(out->host, sizeof out->host, "%s", hp + 1);
        if (end[1] == ':') colon = end + 1;
        else if (end[1]) return -1;
    } else {
        colon = strrchr(hp, ':');
        if (colon) *colon = '\0';
        snprintf(out->host, sizeof out->host, "%s", hp);
    }
    if (colon) {
        char *e;
        long port = strtol(colon + 1, &e, 10);
        if (*e || port <= 0 || port > 65535) return -1;
        out->port = (int)port;
    }
    if (!out->host[0]) return -1;
    if (!slash) {
        snprintf(out->path, sizeof out->path, "/");
    } else if (*slash == '#') {
        snprintf(out->path, sizeof out->path, "/");
    } else {
        size_t n = strcspn(slash, "#");
        if (n + 2 > sizeof out->path) return -1;
        if (*slash == '?') {
            out->path[0] = '/';
            memcpy(out->path + 1, slash, n);
            out->path[n + 1] = '\0';
        } else {
            memcpy(out->path, slash, n);
            out->path[n] = '\0';
        }
    }
    return 0;
}

/* ---- Buffered reading ---- */

typedef struct {
    net_conn *c;
    unsigned char buf[4096];
    size_t pos, len;
    int eof, err;
} reader;

static int rd_fill(reader *r)
{
    if (r->pos < r->len) return 1;
    if (r->eof || r->err) return 0;
    ssize_t n = net_conn_read(r->c, r->buf, sizeof r->buf);
    if (n < 0) { r->err = 1; return 0; }
    if (n == 0) { r->eof = 1; return 0; }
    r->pos = 0;
    r->len = (size_t)n;
    return 1;
}

/* One line without CR LF, cut to out_sz - 1 (long headers such as a Content-Security-Policy
 * are not needed). -1 at the end of the stream or after 64 KB without a newline. */
static int rd_line(reader *r, char *out, size_t out_sz)
{
    size_t n = 0, total = 0;
    for (;;) {
        if (!rd_fill(r)) return n ? (int)n : -1;
        char ch = (char)r->buf[r->pos++];
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (++total > 65536) return -1;
        if (n + 1 < out_sz) out[n++] = ch;
    }
    out[n] = '\0';
    return (int)n;
}

/* ---- Body sink ---- */

typedef struct {
    const http_req *req;
    http_resp *resp;
    size_t limit;
    int write_err;
} sink;

/* 0 to keep going, 1 when the limit is reached, -1 on a write error */
static int sink_put(sink *s, const unsigned char *p, size_t n)
{
    size_t room = s->limit > s->resp->body_len ? s->limit - s->resp->body_len : 0;
    size_t take = n < room ? n : room;
    if (take > 0) {
        if (s->req->out_fd >= 0) {
            size_t off = 0;
            while (off < take) {
                ssize_t w = write(s->req->out_fd, p + off, take - off);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) { s->write_err = 1; return -1; }
                off += (size_t)w;
            }
        } else if (s->req->out_buf) {
            memcpy(s->req->out_buf + s->resp->body_len, p, take);
            s->req->out_buf[s->resp->body_len + take] = '\0';
        }
        s->resp->body_len += take;
    }
    if (take < n) {
        s->resp->truncated = 1;
        return 1;
    }
    return 0;
}

/* Copies up to want bytes (SIZE_MAX = until the end of the stream). 0, 1 (limit), -1. */
static int copy_body(reader *r, sink *s, size_t want)
{
    while (want > 0) {
        if (!rd_fill(r)) return (want == (size_t)-1 && !r->err) ? 0 : -1;
        size_t avail = r->len - r->pos;
        size_t n = avail < want ? avail : want;
        int rc = sink_put(s, r->buf + r->pos, n);
        r->pos += n;
        if (want != (size_t)-1) want -= n;
        if (rc != 0) return rc;
    }
    return 0;
}

static int copy_chunked(reader *r, sink *s)
{
    char line[128];
    for (;;) {
        if (rd_line(r, line, sizeof line) < 0) return -1;
        char *e;
        unsigned long n = strtoul(line, &e, 16);
        if (e == line) return -1;
        if (n == 0) {
            /* trailers up to the empty line */
            while (rd_line(r, line, sizeof line) > 0) {}
            return 0;
        }
        int rc = copy_body(r, s, n);
        if (rc != 0) return rc;
        if (rd_line(r, line, sizeof line) != 0) return -1;
    }
}

/* ---- Request ---- */

static int header_is(const char *h, const char *name)
{
    size_t n = strlen(name);
    return strncasecmp(h, name, n) == 0 && h[n] == ':';
}

static int append(char *buf, size_t sz, size_t *len, const char *fmt, const char *a, const char *b)
{
    int n = snprintf(buf + *len, sz - *len, fmt, a, b);
    if (n < 0 || (size_t)n >= sz - *len) return -1;
    *len += (size_t)n;
    return 0;
}

int http_request(const char *url_in, const http_req *req, http_resp *resp)
{
    memset(resp, 0, sizeof *resp);
    char url[HTTP_URL_MAX];
    char first_host[256] = "";
    const char *method = req->method ? req->method : "GET";
    const char *body = req->body;
    size_t body_len = req->body ? req->body_len : 0;
    int redirects = 0;
    if ((size_t)snprintf(url, sizeof url, "%s", url_in ? url_in : "") >= sizeof url) {
        snprintf(resp->err, sizeof resp->err, "URL too long");
        return -1;
    }

    for (int i = 0; i < HTTP_MAX_HEADERS && req->headers[i]; i++) {
        const char *h = req->headers[i];
        const char *colon = strchr(h, ':');
        if (!colon || colon == h || !http_text_safe(h)) {
            snprintf(resp->err, sizeof resp->err, "bad header");
            return -1;
        }
        for (const char *q = h; q < colon; q++) {
            if (*q == ' ' || *q == '\t') {
                snprintf(resp->err, sizeof resp->err, "bad header name");
                return -1;
            }
        }
    }
    for (const char *m = method; *m; m++) {
        if (*m < 'A' || *m > 'Z') {
            snprintf(resp->err, sizeof resp->err, "bad method");
            return -1;
        }
    }

    for (;;) {
        http_url u;
        if (http_parse_url(url, &u) != 0) {
            snprintf(resp->err, sizeof resp->err, "bad URL");
            return -1;
        }
        if (!first_host[0]) snprintf(first_host, sizeof first_host, "%s", u.host);
        int same_host = strcasecmp(first_host, u.host) == 0;

        char head[HTTP_URL_MAX + 2048];   /* request line with the path, then the headers */
        size_t hl = 0;
        char hostval[300];
        int default_port = (u.tls && u.port == 443) || (!u.tls && u.port == 80);
        if (strchr(u.host, ':'))
            snprintf(hostval, sizeof hostval, default_port ? "[%s]" : "[%s]:%d", u.host, u.port);
        else
            snprintf(hostval, sizeof hostval, default_port ? "%s" : "%s:%d", u.host, u.port);
        int has_ua = 0;
        if (append(head, sizeof head, &hl, "%s %s HTTP/1.1\r\n", method, u.path) ||
                append(head, sizeof head, &hl, "Host: %s%s\r\nConnection: close\r\n", hostval, "")) {
            snprintf(resp->err, sizeof resp->err, "request too long");
            return -1;
        }
        for (int i = 0; i < HTTP_MAX_HEADERS && req->headers[i]; i++) {
            const char *h = req->headers[i];
            if (header_is(h, "Host") || header_is(h, "Connection") || header_is(h, "Content-Length"))
                continue;
            /* credentials never follow a redirect to another host */
            if (!same_host && (header_is(h, "Authorization") || header_is(h, "Cookie")))
                continue;
            if (header_is(h, "User-Agent")) has_ua = 1;
            if (append(head, sizeof head, &hl, "%s%s\r\n", h, "")) {
                snprintf(resp->err, sizeof resp->err, "request too long");
                return -1;
            }
        }
        if (!has_ua && append(head, sizeof head, &hl, "User-Agent: harmony-hub%s%s\r\n", "", "")) {
            snprintf(resp->err, sizeof resp->err, "request too long");
            return -1;
        }
        if (body_len > 0 || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 ||
                strcmp(method, "PATCH") == 0) {
            char cl[24];
            snprintf(cl, sizeof cl, "%lu", (unsigned long)body_len);
            if (append(head, sizeof head, &hl, "Content-Length: %s%s\r\n", cl, "")) {
                snprintf(resp->err, sizeof resp->err, "request too long");
                return -1;
            }
        }
        if (append(head, sizeof head, &hl, "\r\n%s%s", "", "")) {
            snprintf(resp->err, sizeof resp->err, "request too long");
            return -1;
        }

        net_conn_opts opts = req->conn;
        opts.tls = u.tls;
        if (redirects > 0) opts.connect_addrs = NULL; /* the caller's addresses are for the first URL */
        net_conn *c = net_conn_open(u.host, u.port, &opts, resp->pin, resp->err, sizeof resp->err);
        if (!c) return -1;
        if (net_conn_write(c, head, hl) != 0 || (body_len && net_conn_write(c, body, body_len) != 0)) {
            snprintf(resp->err, sizeof resp->err, "%s", net_conn_error(c));
            net_conn_close(c);
            return -1;
        }

        reader *r = calloc(1, sizeof *r);
        if (!r) {
            net_conn_close(c);
            snprintf(resp->err, sizeof resp->err, "out of memory");
            return -1;
        }
        r->c = c;
        char line[HTTP_URL_MAX + 16];
        int status = 0;
        if (rd_line(r, line, sizeof line) < 0 || sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) {
            snprintf(resp->err, sizeof resp->err, "%s", r->err ? net_conn_error(c) : "no HTTP response");
            free(r);
            net_conn_close(c);
            return -1;
        }
        /* 1xx interim responses (100 Continue) are skipped */
        while (status >= 100 && status < 200) {
            while (rd_line(r, line, sizeof line) > 0) {}
            if (rd_line(r, line, sizeof line) < 0 || sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) {
                snprintf(resp->err, sizeof resp->err, "no HTTP response");
                free(r);
                net_conn_close(c);
                return -1;
            }
        }
        long long content_length = -1;
        int chunked = 0;
        char location[HTTP_URL_MAX] = "";
        int location_too_long = 0;
        for (int lines = 0;; lines++) {
            int n = rd_line(r, line, sizeof line);
            if (n < 0 || lines > 200) {
                snprintf(resp->err, sizeof resp->err, "bad response headers");
                free(r);
                net_conn_close(c);
                return -1;
            }
            if (n == 0) break;
            const char *v = strchr(line, ':');
            if (!v) continue;
            v++;
            while (*v == ' ' || *v == '\t') v++;
            if (header_is(line, "Content-Length")) content_length = strtoll(v, NULL, 10);
            else if (header_is(line, "Transfer-Encoding") && strcasestr(v, "chunked")) chunked = 1;
            else if (header_is(line, "Location")) {
                /* a line that filled the buffer was cut by rd_line */
                size_t w = (size_t)snprintf(location, sizeof location, "%s", v);
                location_too_long = w >= sizeof location || (size_t)n >= sizeof line - 1;
            }
        }
        resp->status = status;

        if (req->follow_redirects && location[0] &&
                (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
            free(r);
            net_conn_close(c);
            if (++redirects > HTTP_MAX_REDIRECTS) {
                snprintf(resp->err, sizeof resp->err, "too many redirects");
                return -1;
            }
            if (location_too_long) {
                snprintf(resp->err, sizeof resp->err, "redirect too long");
                return -1;
            }
            char next[HTTP_URL_MAX];
            int next_len;
            if (location[0] == '/' && location[1] != '/') {
                if (strchr(u.host, ':'))
                    next_len = snprintf(next, sizeof next, "%s://[%s]:%d%s", u.tls ? "https" : "http", u.host, u.port, location);
                else
                    next_len = snprintf(next, sizeof next, "%s://%s:%d%s", u.tls ? "https" : "http", u.host, u.port, location);
            } else if (strncasecmp(location, "http://", 7) == 0 || strncasecmp(location, "https://", 8) == 0) {
                /* never from https down to http */
                if (u.tls && strncasecmp(location, "http://", 7) == 0) {
                    snprintf(resp->err, sizeof resp->err, "redirect from https to http refused");
                    return -1;
                }
                next_len = snprintf(next, sizeof next, "%s", location);
            } else {
                snprintf(resp->err, sizeof resp->err, "unsupported redirect");
                return -1;
            }
            if (next_len < 0 || (size_t)next_len >= sizeof next) {
                snprintf(resp->err, sizeof resp->err, "redirect too long");
                return -1;
            }
            snprintf(url, sizeof url, "%s", next);
            if (status == 303 || ((status == 301 || status == 302) && strcmp(method, "GET") != 0)) {
                method = "GET";
                body = NULL;
                body_len = 0;
            }
            continue;
        }

        sink s = { req, resp, 0, 0 };
        if (req->max_body) s.limit = req->max_body;
        else if (req->out_fd < 0) s.limit = req->out_sz ? req->out_sz - 1 : 0;
        else s.limit = (size_t)-1;
        if (req->out_fd < 0 && req->out_buf && req->out_sz) {
            if (s.limit > req->out_sz - 1) s.limit = req->out_sz - 1;
            req->out_buf[0] = '\0';
        }
        int rc = 0;
        int no_body = strcmp(method, "HEAD") == 0 || status == 204 || status == 304;
        if (!no_body) {
            if (chunked) rc = copy_chunked(r, &s);
            else if (content_length >= 0) rc = copy_body(r, &s, (size_t)content_length);
            else rc = copy_body(r, &s, (size_t)-1);
        }
        if (rc < 0) {
            if (s.write_err) snprintf(resp->err, sizeof resp->err, "cannot write the output");
            else snprintf(resp->err, sizeof resp->err, "%s", r->err ? net_conn_error(c) : "response cut short");
        }
        free(r);
        net_conn_close(c);
        return rc < 0 ? -1 : 0;
    }
}
