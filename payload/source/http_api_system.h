#ifndef HTTP_API_SYSTEM_H
#define HTTP_API_SYSTEM_H

#include "http_auth.h"
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

void apply_hub_settings(const struct hub_settings *s);

int http_api_system_handle(int client_fd, const char *method, const char *clean_path,
                           const char *query, const char *body, const char *req,
                           enum http_role auth_role, const char *auth_user);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_API_SYSTEM_H */
