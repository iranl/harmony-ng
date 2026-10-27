#ifndef HUB_HTTP_SERVER_H
#define HUB_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

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

/* Close HTTP server socket */
void http_server_close(int server_fd);

/* In-memory static asset cache flush */
void static_cache_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* HUB_HTTP_SERVER_H */
