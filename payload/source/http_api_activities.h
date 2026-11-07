#ifndef HTTP_API_ACTIVITIES_H
#define HTTP_API_ACTIVITIES_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Help flow, shared by POST /api/activity/help[/answer] and the command channel. req is the
 * REST body (activity, dry_run / device, on) or NULL; *out is the reply object, or on failure
 * {"success":false,"error":...}; the return value is the HTTP code (200, 404, 409, 500). */
int activity_help_start(const cJSON *req, cJSON **out);
int activity_help_answer(const cJSON *req, cJSON **out);

int http_api_activities_handle(int client_fd, const char *method, const char *clean_path,
                               const char *query, const char *body, const char *req);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_API_ACTIVITIES_H */
