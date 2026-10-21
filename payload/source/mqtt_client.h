#ifndef CODEX_MQTT_CLIENT_H
#define CODEX_MQTT_CLIENT_H

#include <stddef.h>
#include <stdint.h>

typedef void (*mqtt_msg_cb)(const char *topic, const char *payload, size_t payload_len, void *user_data);

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize MQTT client parameters */
int mqtt_init(const char *host, int port, const char *client_id, const char *user, const char *pass);

/* Configure LWT (Last Will and Testament) */
void mqtt_set_lwt(const char *topic, const char *payload, int retain);

/* Configure Birth message (published automatically upon successful connection) */
void mqtt_set_birth(const char *topic, const char *payload, int retain);

/* Connect to configured broker (non-blocking / fast handshake) */
int mqtt_connect(void);

/* Check if connection is active */
int mqtt_is_connected(void);

/* Publish message (QoS 0) */
int mqtt_publish(const char *topic, const char *payload, int retain);

/* Subscribe to topic with callback */
int mqtt_subscribe(const char *topic, mqtt_msg_cb cb, void *user_data);

/*
 * Non-blocking tick called from main event loop.
 * Processes incoming messages and sends keepalive PINGREQs.
 */
int mqtt_tick(void);

/* Disconnect from broker */
void mqtt_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* CODEX_MQTT_CLIENT_H */
