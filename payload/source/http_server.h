#ifndef CODEX_HTTP_SERVER_H
#define CODEX_HTTP_SERVER_H

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
 * Handle an incoming client socket:
 *  - Reads request
 *  - If WebSocket Upgrade, delegates to ws_handle_upgrade
 *  - If REST endpoint (/api/activity, /api/devices, /api/pronto-blast, /api/remote-status), handles request
 *  - Otherwise serves static UI or 404
 */
void http_handle_client(int client_fd);

/*
 * Dispatches an MQTT dummy button pulse: publishes "1", schedules non-blocking deferred "0".
 * Returns 0 on success, negative on error.
 */
int mqtt_dispatch_button_pulse(const char *dev_id, const char *dev_name_in, const char *cmd_name,
                               int req_pulse_ms, char *out_topic, size_t out_topic_len, int *out_pulse_ms);

/* Periodic tick to drain pending deferred MQTT pulse off messages */
void pulse_tick(void);

/* Get MQTT runtime info */
void daemon_get_mqtt_info(char *host, size_t hlen, int *port, char *base, size_t blen, int *connected);

/* Close HTTP server socket */
void http_server_close(int server_fd);

/* In-memory static asset cache flush */
void static_cache_flush(void);

/* Asynchronous background icon extraction */
void http_ensure_icons_extracted(void);

#ifdef __cplusplus
}
#endif

#endif /* CODEX_HTTP_SERVER_H */
