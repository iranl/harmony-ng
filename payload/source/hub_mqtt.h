#ifndef HUB_MQTT_H
#define HUB_MQTT_H

/*
 * MQTT layer on the event bus (spec milestone 3): retained state topics published on change,
 * inventory chunks, a JSON command channel, Home Assistant discovery entities (hub_mqtt_ha.c),
 * the first layer's command topics as aliases and the pulse of MQTT-type devices. The transport
 * is mqtt_client.c; the settings endpoint is POST /api/mqtt-save.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern char g_mqtt_base_topic[128];

int hub_mqtt_init(void);        /* loads mqtt.json, subscribes to the bus, connects in the background */
void hub_mqtt_tick(void);       /* main loop: client I/O, reconnect backoff, (re)connect actions */
void hub_mqtt_shutdown(void);
void daemon_sync_mqtt(void);    /* settings changed: reload and reconnect */
void daemon_get_mqtt_info(char *host, size_t hlen, int *port, char *base, size_t blen, int *connected);
/* Plain publish helper for the system-action "mqtt_publish" (QoS 0, not retained) */
int hub_mqtt_publish_plain(const char *topic, const char *payload);
/* MQTT-type devices: publish "1" on the device's topic template (placeholders {base_topic},
 * {device}, {command}; topic_in overrides the device's), then "0" after the pulse. Returns 0
 * when published, -1 when MQTT is off or disconnected; the resolved topic and pulse are
 * reported even then (used by POST /api/device-mqtt-test). */
int mqtt_dispatch_button_pulse_topic(const char *dev_id, const char *dev_name_in, const char *cmd_name,
                                     const char *topic_in, int req_pulse_ms,
                                     char *out_topic, size_t out_topic_len, int *out_pulse_ms);
int daemon_mqtt_button_handler(const char *device_id, const char *dev_name, const char *command);

#ifdef __cplusplus
}
#endif

#endif /* HUB_MQTT_H */
