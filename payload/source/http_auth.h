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
    /* AUTH_MARK_YES: made while authentication was on, or kept since the last switch from off
     * to on (by an admin, or as the account that switch named).
     * AUTH_MARK_NO: waiting: every account but the switch's own after each switch from off to on,
     * and accounts made or changed while authentication is off; listed until kept or deleted.
     * AUTH_MARK_UNKNOWN: no mark in the file (older firmware): counts as kept while authentication
     * stays on, like any other at the next switch. */
    int confirmed;
};

#define AUTH_MARK_NO 0
#define AUTH_MARK_YES 1
#define AUTH_MARK_UNKNOWN (-1)

#ifdef __cplusplus
extern "C" {
#endif

void http_auth_init(void);
void compute_ha1(const char *username, const char *realm, const char *password, char ha1_out[65]);
int load_user_accounts(struct user_account *users, int max_users, int *auth_enabled);
int save_user_accounts(const struct user_account *users, int count, int auth_enabled);
int create_auth_session(const char *username, enum http_role role, const char *client_ip, char *out_token, size_t out_len);
int destroy_auth_session(const char *token);
/* Random hex from /dev/urandom; 0 on success, -1 when it cannot be read (never a weak fallback) */
int generate_random_hex(char *out, size_t num_bytes);
/* Comparison of secrets that does not stop at the first different byte */
int http_auth_secret_equal(const char *a, const char *b);
/* Password check for one account */
int http_auth_password_matches(const struct user_account *u, const char *password);
/* Failed-password limiter per client address, used by the login route and by Basic auth:
 * seconds the address is still locked (0 = not locked), and recording of an attempt */
int http_auth_lock_remaining(const char *client_ip);
void http_auth_note_failure(const char *client_ip);
void http_auth_note_success(const char *client_ip);
/* End the sessions of one account (deleted, renamed, demoted, new password), or all of them */
void http_auth_drop_user_sessions(const char *username);
void http_auth_drop_all_sessions(void);
/* Authentication goes from off to on (in memory; the caller saves the accounts): the owner's
 * account (index owner, -1 for none) is kept when the request sets its password, or when it was not
 * changed while authentication was off; every other account goes back to waiting (an off period
 * voids earlier keeps: anybody was admin then). */
void http_auth_switch_on_accounts(struct user_account *users, int count, int owner, int owner_password_set);
/* 1 when the caller (an account or a personal access token) is itself waiting: it may not keep
 * anything (it could keep itself) */
int http_auth_caller_waiting(const char *req, const char *client_ip);
/* At the same switch, after the accounts were saved: every token gets "confirmed": false,
 * and the reply gets "unconfirmed": {"users": [...], "tokens": [...]} (every account and token
 * marked false), so the UIs can ask to keep or delete each. */
void http_auth_add_unconfirmed(cJSON *reply, const struct user_account *users, int count);
/* Marks accounts (usernames) and tokens (ids) as kept; all = every one. Returns 0, or -1 when a
 * file could not be written. */
int http_auth_confirm(const cJSON *usernames, const cJSON *token_ids, int all, int *users_done, int *tokens_done);
/* After an import replaced users.json or webui_auth.conf (before = the accounts and the state
 * from before it). Made while authentication was off, an import cannot vouch for anybody: every
 * imported account is marked AUTH_MARK_NO, except one with the same name and password digest that
 * was kept before. Saves the accounts. Returns the "unconfirmed" reply object when the import
 * turned authentication on, else NULL. */
cJSON *http_auth_after_import(const struct user_account *before, int before_count, int before_on);

enum http_role http_auth_check_request(const char *req, const char *client_ip,
                                       const char *method, char *out_user, size_t out_user_len);
int http_auth_is_user_role_permitted(const char *method, const char *clean_path);
/* Role for a WebSocket upgrade request: like http_auth_check_request, plus a "?token=<pat_ or
 * hht_ token>" in the request line when there is no Authorization header (browsers cannot set
 * headers on a WebSocket). */
enum http_role http_auth_check_ws_upgrade(const char *req, const char *client_ip,
                                          char *out_user, size_t out_user_len);
void http_auth_send_challenge(int client_fd, const char *client_ip);

/* Handles /api/auth/me, /api/auth/token, /api/auth/logout, /api/auth/tokens, /api/users */
int http_api_auth_handle(int client_fd, const char *method, const char *clean_path,
                         const char *query, const char *body, const char *req,
                         const char *client_ip, enum http_role current_role);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_AUTH_H */
