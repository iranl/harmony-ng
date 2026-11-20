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
/* NULL when an update may start now; else the 409 reply: an update ends with a services restart,
 * which must not cut an activity switch in the middle (callers check it before ota_update_apply) */
cJSON *ota_update_apply_blocked(void);
/* 1 while an update runs or the services restart after it (update_state running or restarting) */
int ota_update_busy(void);
/* At daemon start: a "restarting" state ends here (this start is the restart) */
void ota_update_state_init(void);

int http_api_ota_handle(int client_fd, const char *method, const char *clean_path,
                        const char *query, const char *body, const char *req);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_API_OTA_H */
