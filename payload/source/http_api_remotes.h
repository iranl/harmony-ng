#ifndef HTTP_API_REMOTES_H
#define HTTP_API_REMOTES_H

#ifdef __cplusplus
extern "C" {
#endif

int http_api_remotes_handle(int client_fd, const char *method, const char *clean_path,
                            const char *query, const char *body, const char *req);

/* The button event of a physical key on the bus (WebSocket, MQTT): slot 0 = none, device_id and
 * command may be empty. Shared by POST /api/button-press and the key channel. */
void http_api_remotes_button_event(int slot, const char *button, const char *action, const char *source,
                                   const char *device_id, const char *command);

/* Key channel from hub_rf (hub_key_channel.h): binds the datagram socket at path and reads it on
 * its own thread, one event after the other. 0 on success. */
int http_api_remotes_key_channel_start(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_API_REMOTES_H */
