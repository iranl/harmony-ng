#ifndef HTTP_API_ACTIVITIES_H
#define HTTP_API_ACTIVITIES_H

#ifdef __cplusplus
extern "C" {
#endif

int http_api_activities_handle(int client_fd, const char *method, const char *clean_path,
                               const char *query, const char *body, const char *req);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_API_ACTIVITIES_H */
