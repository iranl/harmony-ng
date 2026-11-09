#ifndef HTTP_API_OTA_H
#define HTTP_API_OTA_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The update endpoints' logic, shared with the command channel (hub_cmd.c). Each returns a new
 * object (the REST reply). ota_update_check blocks while it checks (seconds). */
cJSON *ota_update_status(void);
cJSON *ota_update_check(const cJSON *params);
cJSON *ota_update_apply(const cJSON *params);

int http_api_ota_handle(int client_fd, const char *method, const char *clean_path,
                        const char *query, const char *body, const char *req);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_API_OTA_H */
