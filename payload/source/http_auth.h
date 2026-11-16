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
/* Personal access tokens: at most this many in all, and per operator (role user) account. One
 * entry is about 350 bytes in the file, so the list stays far below HTTP_TOKENS_FILE_MAX, the
 * size every reader accepts. */
#define HTTP_TOKENS_MAX 64
#define HTTP_TOKENS_PER_OPERATOR 10
#define HTTP_TOKENS_FILE_MAX 65536
/* Where POST /api/auth/tokens-reset moves an unreadable token file */
#define HTTP_TOKENS_JSON_BAD "/data/harmony/config/tokens.json.bad"
/* A request waits at most this long for the token file lock (503 after) */
#define HTTP_TOKENS_LOCK_WAIT_MS 3000
/* Token creates and deletes per operator account and minute (429 after) */
#define HTTP_TOKENS_WRITES_PER_MIN 30
/* The 500 text for a token file that cannot be read */
#define HTTP_TOKENS_UNREADABLE_MSG "config/tokens.json cannot be read (cut, or larger than 64 KB) and was left unchanged; " \
    "an admin can move it aside with POST /api/auth/tokens-reset (every access token then has to be created again)"

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
/* Authentication went from off to on. While it was off every client was admin, so anybody could
 * have made or changed an account or a token: adds "to_check": {"users": [...], "tokens": [...]}
 * to the reply, every account except owner (the one the switching request names, NULL for none)
 * and every personal access token (no hashes; "owner_missing": true when its account is gone), for
 * the UIs to show at once. */
void http_auth_add_switch_check(cJSON *reply, const struct user_account *users, int count, const char *owner);

/* tokens.json is read, changed and written under one lock. http_tokens_lock() returns 0, or -1
 * when another request held it for 3 s (it never blocks in the kernel: the Hub has no futex). */
int http_tokens_lock(void);
void http_tokens_unlock(void);
/* *root = the file as an object with a "tokens" array (an empty list when there is no file).
 * Returns -1 and leaves *root NULL when the file exists but cannot be read or parsed: the caller
 * must then leave the file alone. */
int http_tokens_load(cJSON **root);
/* Writes the list; 0, or -1 when it was not stored */
int http_tokens_save(const cJSON *root);
/* With the lock held: 0 when this operator account may change one more token now, else -1 and
 * *retry_after seconds */
int http_tokens_rate_check(const char *account, int *retry_after);
/* Deletes the tokens owned by an account (deleted account, or a name used again). Returns how
 * many, HTTP_TOKENS_BUSY when the lock was not free, or -1 when the file could not be read or
 * written (left as it was). Takes the lock itself. */
#define HTTP_TOKENS_BUSY (-3)
int http_tokens_remove_owner(const char *owner);
/* After an import replaced the accounts: removes the tokens left by deleted accounts whose name an
 * imported account now has (names not in before). 0, or below 0 when one could not be removed. */
int http_tokens_remove_new_names(const struct user_account *before, int before_count,
                                 const struct user_account *after, int after_count);
/* With the lock held: moves an unreadable token file aside and starts an empty list. 0 done,
 * 1 the file is readable (nothing done), -1 failed */
int http_tokens_reset_unreadable(void);

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
