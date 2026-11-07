#ifndef HBUS_SERVER_H
#define HBUS_SERVER_H

/*
 * Logitech protocol shim on port 8088 (spec section 6, Appendix B): the HTTP provisioning POST,
 * the UPnP description, and the stock WebSocket command set that aioharmony and Home Assistant's
 * core harmony integration speak. Active only while the legacy_enabled setting is on; never
 * authenticated (the stock protocol has none). Legacy WebSocket clients live in ws_server.c's
 * table with the legacy flag, so the 8089 broadcasts never reach them.
 */

#include "config_store.h"
#include "hub_events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HBUS_PORT 8088

/* Reads the remote id once and opens the listener when the setting is on. port: HBUS_PORT or
 * a test port. Returns 0, or -1 when the listener could not be opened. */
int hbus_init(int port);
/* The listening socket, or -1 while the shim is off */
int hbus_listen_fd(void);
/* The HTTP side of one accepted connection (worker thread): TCP probe, POST /, GET /description,
 * WebSocket upgrade. Closes the socket unless it became a WebSocket client. */
void hbus_handle_client(int cfd);
/* A frame from a legacy WebSocket client (main loop) */
void hbus_handle_ws_message(int cfd);
/* Main loop, once per iteration: digest notify for new clients, changeChannel digits */
void hbus_tick(void);
/* Bus sink: stateDigest notifies, startActivity progress and completion */
void hbus_event_sink(const hub_event_t *ev, void *user);
void hbus_apply_settings(const struct hub_settings *s);
void hbus_shutdown(void);

/* activeRemoteId: the stock remoteId when /data/luaworks/provision/preferences has one, else
 * derived from hub_id */
const char *hbus_remote_id(void);
/* The state digest object (6.7), new object */
cJSON *hbus_digest(const char *activity_numeric, int activity_status);

#ifdef __cplusplus
}
#endif

#endif /* HBUS_SERVER_H */
