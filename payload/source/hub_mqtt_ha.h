#ifndef HUB_MQTT_HA_H
#define HUB_MQTT_HA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct daemon_activity {
    char id[32];
    char name[64];
    int order;
};

struct daemon_activity_list {
    struct daemon_activity items[32];
    int count;
};

void safe_id(const char *in, char *out, size_t outlen);
int load_daemon_activities(struct daemon_activity_list *list);
void get_local_ip(const char *mqtt_host, char *buf, size_t buflen);
void get_device_and_command_counts(int *dev_count, int *cmd_count);
/* Home Assistant MQTT discovery on the v2 topics: the entities read <base>/activity_state and
 * <base>/hub and write the alias command topics (<base>/activity/set, sync, remote/<n>/sleep).
 * The object ids are those of the first MQTT layer, so entities created before v2 are kept. */
void publish_ha_discovery(const char *cid, const char *name, const char *hub_id,
                          const char *base_topic, const char *discovery_prefix,
                          const char *status_topic, const char *mqtt_host,
                          int ha_discovery_enabled);
/* Empty retained payloads on every discovery topic (discovery turned off) */
void clear_ha_discovery(const char *cid, const char *discovery_prefix);

#ifdef __cplusplus
}
#endif

#endif /* HUB_MQTT_HA_H */
