#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>
#include "tls_client.h"

/* Minimal HTTP/1.1 client over net_conn (tls_client.h): one request per connection
 * (Connection: close), Content-Length or chunked bodies, redirects. */

#define HTTP_MAX_HEADERS 12
#define HTTP_MAX_REDIRECTS 5

typedef struct {
    const char *method;                 /* NULL = GET */
    const char *headers[HTTP_MAX_HEADERS]; /* "Name: value", NULL-terminated */
    const char *body;                   /* may be NULL */
    size_t body_len;
    int follow_redirects;               /* up to HTTP_MAX_REDIRECTS */
    size_t max_body;                    /* 0 = no limit (out_fd) / out_sz - 1 (out_buf) */
    int out_fd;                         /* >= 0: body written here */
    char *out_buf;                      /* else: body here, NUL-terminated (may be NULL) */
    size_t out_sz;
    net_conn_opts conn;                 /* tls is set from the URL scheme */
} http_req;

typedef struct {
    int status;                         /* HTTP status, 0 when no response */
    size_t body_len;                    /* bytes stored or written */
    int truncated;                      /* body longer than max_body */
    char pin[NET_PIN_HEX_LEN + 1];      /* TLS server key hash of the last connection */
    char err[160];
} http_resp;

/* URLs, paths and redirect targets up to this length (a GitHub release-asset redirect is about
 * 930 characters); a longer one is an error, never cut */
#define HTTP_URL_MAX 4096

typedef struct {
    int tls;
    char host[256];
    int port;
    char path[HTTP_URL_MAX];
} http_url;

/* Parses http(s)://host[:port][/path]. 0 or -1. Refuses control characters and spaces. */
int http_parse_url(const char *url, http_url *out);
/* 1 when s has no CR, LF or other control character (header injection) */
int http_text_safe(const char *s);
/* Runs the request. 0 when a response was read (any status), -1 with resp->err set. */
int http_request(const char *url, const http_req *req, http_resp *resp);

#endif
