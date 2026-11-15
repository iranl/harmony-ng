#ifndef HTTP_API_DEVICES_H
#define HTTP_API_DEVICES_H

#ifdef __cplusplus
extern "C" {
#endif

int http_api_devices_handle(int client_fd, const char *method, const char *clean_path,
                            const char *query, const char *body, const char *req);

/* Removes map[section][id] from remote_slot1_map.json, remote_slot2_map.json and
 * bt_remote_map.json ("activities" after an activity delete, "devices" after a device delete):
 * bindings no editor shows any more, which would otherwise hold a device */
void remote_maps_drop_section(const char *section, const char *id);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_API_DEVICES_H */
