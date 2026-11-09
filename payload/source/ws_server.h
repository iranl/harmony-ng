#ifndef HARMONY_WS_SERVER_H
#define HARMONY_WS_SERVER_H

#include <stddef.h>
#include <stdint.h>

#define WS_MAX_CLIENTS 16
/* Keepalive: a ping after this many seconds without a frame from the client, and the client is
 * dropped when nothing (pong or other) arrives within the timeout. Overridable at compile time
 * (the test harness uses -DWS_PING_INTERVAL_S=2 -DWS_PONG_TIMEOUT_S=1). */
#ifndef WS_PING_INTERVAL_S
#define WS_PING_INTERVAL_S 30
#endif
#ifndef WS_PONG_TIMEOUT_S
#define WS_PONG_TIMEOUT_S 10
#endif
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
 * Returns 0 on success, negative on error (bad request, all WS_MAX_CLIENTS slots taken: a 503 is
 * sent, or send failure); the socket is then not registered and the caller closes it.
 */
int ws_handle_upgrade(int client_fd, const char *http_req);

/* Send a text frame to a specific client */
int ws_send_text(int client_fd, const char *text);

/* Broadcast a text frame to all active WebSocket clients */
void ws_broadcast_text(const char *text);

/*
 * Non-blocking read and unmask of an incoming WebSocket frame.
 * Returns the payload length (0 for an empty frame such as a pong), WS_READ_AGAIN when there was
 * nothing to read after all (spurious wakeup, EINTR: keep the client), -1 on error or when the
 * peer closed the connection.
 */
#define WS_READ_AGAIN (-2)
int ws_read_frame(int client_fd, char *out_buf, size_t max_len, int *out_opcode);

/* Retrieve active client socket descriptors for event loop polling */
int ws_get_clients(int *out_fds, size_t max_count);
/* Clients that upgraded since the last call and have not received their snapshot yet: they are
 * excluded from broadcasts until taken here (main loop). Returns the count. */
int ws_take_pending_clients(int *out_fds, size_t max_count);
/* Legacy (Logitech protocol, port 8088) clients share the table but never receive the 8089
 * broadcasts; hbus_server.c talks to them. */
void ws_set_client_legacy(int client_fd, int legacy);
int ws_is_client_legacy(int client_fd);
/* Text frame to every legacy client */
void ws_broadcast_legacy_text(const char *text);
/* Close and remove every legacy client (the setting was turned off) */
void ws_close_legacy_clients(void);
/* SHA-1 of a buffer (the handshake's engine), for the SSDP UUID */
void ws_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);
/* Keepalive, main loop once per iteration: pings idle clients, drops the ones that did not
 * answer. Returns the number of clients dropped. */
int ws_server_tick(void);
/* Send a ping frame (the client answers with a pong, which ws_read_frame counts as traffic) */
int ws_send_ping(int client_fd);

/* Send a pong frame responding to ping */
int ws_send_pong(int client_fd);

/* Remove client upon disconnect */
void ws_remove_client(int client_fd);

/* Role of the token the client upgraded with (values of enum http_role: 0 none, 1 user,
 * 2 admin). Unknown clients and clients upgraded without auth count as admin. */
void ws_set_client_role(int client_fd, int role);
int ws_get_client_role(int client_fd);
/* 1 while client_fd is a connected WebSocket client */
int ws_is_client(int client_fd);

/* Close all connected clients */
void ws_close_all(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_WS_SERVER_H */
