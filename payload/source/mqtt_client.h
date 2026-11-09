#ifndef HARMONY_MQTT_CLIENT_H
#define HARMONY_MQTT_CLIENT_H

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

/* Check if connection attempt is in progress */
int mqtt_is_connecting(void);

/* Trigger background connection/reconnect thread */
int mqtt_connect_async(void);

/* Check if connection is active */
int mqtt_is_connected(void);

/* Publish message (QoS 0) */
/* QoS 0 publish of a string payload (kept for the existing callers) */
int mqtt_publish(const char *topic, const char *payload, int retain);
/* Publish with an explicit length (an empty payload clears a retained topic) and QoS 0 or 1.
 * QoS 1 sends a packet id; the broker's PUBACK is consumed by mqtt_tick (no retransmission). */
int mqtt_publish_ex(const char *topic, const char *payload, size_t len, int qos, int retain);
/* Dry run normally only logs publishes; the test harness sets HARMONY_MQTT_LIVE=1 to publish for real */
int mqtt_publishing_live(void);

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

#endif /* HARMONY_MQTT_CLIENT_H */
