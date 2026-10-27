#ifndef HTTP_API_CUSTOM_ICONS_H
#define HTTP_API_CUSTOM_ICONS_H

#ifdef __cplusplus
extern "C" {
#endif

void http_ensure_icons_extracted(void);

int http_api_custom_icons_handle(int client_fd, const char *method, const char *clean_path,
                                 const char *query, const char *body, const char *req);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_API_CUSTOM_ICONS_H */
