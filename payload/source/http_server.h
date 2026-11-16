#ifndef HUB_HTTP_SERVER_H
#define HUB_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define HTTP_MAX_CLIENTS 32
#define HTTP_SERVER_PORT 8080

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize HTTP + WebSocket server listening on port 8080.
 * Returns listening socket fd or -1 on error.
 */
int http_server_init(int port);

/*
 * Handle an incoming client connection:
 *  - Reads HTTP request (handles keep-alive & large bodies)
 *  - Dispatches WebSocket upgrade if applicable
 *  - Checks Authentication & RBAC
 *  - Serves static UI cache
 *  - Dispatches to API modules
 */
void http_handle_client(int client_fd);

/* Time limits of one request (port 8089 and the 8088 shim share the worker pool) */
#define HTTP_HEADER_DEADLINE_MS 10000 /* request line and headers */
#define HTTP_BODY_DEADLINE_MS 20000   /* the body announced by Content-Length */

/* Monotonic milliseconds, for the request deadlines */
uint64_t http_mono_ms(void);
/* recv() that waits at most until deadline_ms (http_mono_ms() time) and at most 5 s for any
 * byte. Returns what recv() returns, or -1 when the time ran out. */
ssize_t http_recv_until(int fd, void *buf, size_t len, uint64_t deadline_ms);

/* Close HTTP server socket */
void http_server_close(int server_fd);

/* In-memory static asset cache flush */
void static_cache_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* HUB_HTTP_SERVER_H */
