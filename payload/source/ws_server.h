#ifndef CODEX_WS_SERVER_H
#define CODEX_WS_SERVER_H

#include <stddef.h>
#include <stdint.h>

#define WS_MAX_CLIENTS 16
#define WS_OP_TEXT   0x01
#define WS_OP_BINARY 0x02
#define WS_OP_CLOSE  0x08
#define WS_OP_PING   0x09
#define WS_OP_PONG   0x0A

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize WebSocket client tracking */
int ws_server_init(void);

/* Check if HTTP request headers contain a WebSocket Upgrade request */
int ws_is_upgrade_req(const char *http_req);

/*
 * Complete RFC 6455 handshake: computes Sec-WebSocket-Accept and sends 101 Switching Protocols.
 * Registers client_fd for broadcasts.
 * Returns 0 on success, negative on error.
 */
int ws_handle_upgrade(int client_fd, const char *http_req);

/* Send a text frame to a specific client */
int ws_send_text(int client_fd, const char *text);

/* Broadcast a text frame to all active WebSocket clients */
void ws_broadcast_text(const char *text);

/*
 * Non-blocking read and unmask of an incoming WebSocket frame.
 * Returns payload length (or negative on error/closed).
 */
int ws_read_frame(int client_fd, char *out_buf, size_t max_len, int *out_opcode);

/* Retrieve active client socket descriptors for event loop polling */
int ws_get_clients(int *out_fds, size_t max_count);

/* Send a pong frame responding to ping */
int ws_send_pong(int client_fd);

/* Remove client upon disconnect */
void ws_remove_client(int client_fd);

/* Close all connected clients */
void ws_close_all(void);

#ifdef __cplusplus
}
#endif

#endif /* CODEX_WS_SERVER_H */
