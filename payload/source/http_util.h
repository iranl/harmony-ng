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
char *url_decode(const char *src);
cJSON *parse_request_params(const char *body);
int write_file_content(const char *filepath, const char *content);
int write_file_atomic_sized(const char *filepath, const char *content, size_t len);
char *read_file_text_alloc(const char *path, size_t max_sz, size_t *out_len);
char *b64_encode(const unsigned char *src, size_t len);
unsigned char *b64_decode(const char *src, size_t *out_len);
uint32_t crc32_simple(const unsigned char *buf, size_t len);
int is_safe_param(const char *str, const char *allowed_extra);
int is_safe_ipv4(const char *ip);
void escape_wpa_str(const char *src, char *dst, size_t dst_len);
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
int is_proc_running(const char *proc_name);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_UTIL_H */
