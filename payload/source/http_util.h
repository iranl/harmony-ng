#ifndef HTTP_UTIL_H
#define HTTP_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

void send_cjson_resp(int fd, int code, cJSON *obj);
void send_json_error(int fd, int code, const char *msg);
/* send() until every byte is out. A send that takes only part of the data (the socket's send
 * timeout passed while a slow client read) is continued; returns -1 when the peer is gone or
 * takes nothing within the send timeout. */
int http_send_all(int fd, const void *buf, size_t len);
char *url_decode(const char *src);
cJSON *parse_request_params(const char *body);
/* The decoded value of one query parameter (split at '&', matched by its whole name), or "" */
void http_query_param(const char *query, const char *name, char *out, size_t out_sz);
int write_file_content(const char *filepath, const char *content);
int write_file_atomic_sized(const char *filepath, const char *content, size_t len);
char *read_file_text_alloc(const char *path, size_t max_sz, size_t *out_len);
char *b64_encode(const unsigned char *src, size_t len);
unsigned char *b64_decode(const char *src, size_t *out_len);
uint32_t crc32_simple(const unsigned char *buf, size_t len);
int is_safe_param(const char *str, const char *allowed_extra);
int is_safe_ipv4(const char *ip);
int get_client_ip(int fd, char *ip_buf, size_t buf_len);
int http_header_value(const char *req, const char *name, char *out, size_t out_len);
int http_origin_is_allowed(const char *origin, const char *host);
int http_request_is_cross_site(const char *req);
/* Runs argv[0] without a shell and waits for it, but kills it as soon as watch_path is larger
 * than max_bytes or max_ms have passed. Returns the exit status, -1 when it could not run,
 * RUN_TOO_BIG or RUN_TOO_SLOW. */
#define RUN_TOO_BIG (-2)
#define RUN_TOO_SLOW (-3)
int run_argv_limited(char *const argv[], const char *watch_path, long max_bytes, unsigned int max_ms);
/* hub_fetch (hub_fetch.c): the Hub's one TLS client, dynamic against libbearssl.so.0. The
 * stock ld-uClibc ignores rpath: hub_daemon and hub_rf set LD_LIBRARY_PATH=HUB_FETCH_LIB_DIR at
 * start, so every program they run finds the library. */
#define HUB_FETCH_BIN "/data/harmony/bin/hub_fetch"
#define HUB_FETCH_LIB_DIR "/data/harmony/lib"
const char *hub_fetch_path(void);
/* 1 when hub_fetch and its library are installed */
int hub_fetch_available(void);
/* Downloads url to out_path with hub_fetch (server certificate checked against the CA
 * bundle), run through run_argv_limited. RUN_NO_FETCH when hub_fetch, its library or the
 * bundle is missing: only then may a caller fall back to busybox wget, which checks no
 * certificate. */
#define RUN_NO_FETCH (-4)
int fetch_url_checked(const char *url, const char *out_path, long max_bytes, unsigned int max_ms,
                      int timeout_s);
int is_proc_running(const char *proc_name);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_UTIL_H */
