#ifndef HTTP_AUTH_H
#define HTTP_AUTH_H

#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define MAX_HTTP_USERS 16
#define MAX_USERNAME_LEN 32
#define MAX_PASSWORD_LEN 64
#define MAX_AUTH_SESSIONS 32
#define AUTH_SESSION_TTL 86400  /* 24 hours */
#define HARMONY_AUTH_REALM "Harmony Hub"

#define HARMONY_USERS_JSON "/data/harmony/config/users.json"
#define HARMONY_AUTH_CONF  "/data/harmony/webui_auth.conf"
#define HARMONY_TOKENS_JSON "/data/harmony/config/tokens.json"

enum http_role {
    ROLE_NONE = 0,
    ROLE_USER = 1,
    ROLE_ADMIN = 2
};

struct user_account {
    char username[MAX_USERNAME_LEN];
    char password[MAX_PASSWORD_LEN];
    char ha1[65];
    enum http_role role;
};

#ifdef __cplusplus
extern "C" {
#endif

void http_auth_init(void);
void compute_ha1(const char *username, const char *realm, const char *password, char ha1_out[65]);
int load_user_accounts(struct user_account *users, int max_users, int *auth_enabled);
int save_user_accounts(const struct user_account *users, int count, int auth_enabled);
int create_auth_session(const char *username, enum http_role role, const char *client_ip, char *out_token, size_t out_len);
int destroy_auth_session(const char *token);

enum http_role http_auth_check_request(const char *req, const char *client_ip,
                                       const char *method, char *out_user, size_t out_user_len);
int http_auth_is_user_role_permitted(const char *method, const char *clean_path);
void http_auth_send_challenge(int client_fd, const char *client_ip);

/* Handles /api/auth/me, /api/auth/token, /api/auth/logout, /api/auth/tokens, /api/users */
int http_api_auth_handle(int client_fd, const char *method, const char *clean_path,
                         const char *query, const char *body, const char *req,
                         const char *client_ip, enum http_role current_role);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_AUTH_H */
