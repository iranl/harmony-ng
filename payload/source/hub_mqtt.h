#ifndef HUB_MQTT_H
#define HUB_MQTT_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

extern char g_mqtt_base_topic[128];

int hub_mqtt_init(void);
void hub_mqtt_tick(void);
void hub_mqtt_shutdown(void);
void hub_mqtt_publish_progress(const char *msg);
void daemon_publish_mqtt_state(void);
void daemon_sync_mqtt(void);
void daemon_get_mqtt_info(char *host, size_t hlen, int *port, char *base, size_t blen, int *connected);
int daemon_mqtt_button_handler(const char *device_id, const char *dev_name, const char *command);
int mqtt_dispatch_button_pulse_topic(const char *dev_id, const char *dev_name_in, const char *cmd_name,
                                     const char *topic_in, int req_pulse_ms,
                                     char *out_topic, size_t out_topic_len, int *out_pulse_ms);
void mqtt_cmd_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* HUB_MQTT_H */
