#define _GNU_SOURCE
#include "http_auth.h"
#include "http_util.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <strings.h>

struct auth_session {
    char token[68];
    char username[MAX_USERNAME_LEN];
    enum http_role role;
    time_t created_at;
    time_t expires_at;
    time_t last_seen;
    char client_ip[40];
    int active;
};

static struct auth_session s_sessions[MAX_AUTH_SESSIONS];
static pthread_mutex_t s_session_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_accounts_unreadable_logged;

void http_auth_init(void) {
}

/* Random hex from /dev/urandom. Returns 0, or -1 when no random bytes can be read: a token
 * from a predictable generator would be worse than no token. */
int generate_random_hex(char *out, size_t num_bytes) {
    uint8_t rnd[32];
    if (num_bytes > sizeof(rnd)) num_bytes = sizeof(rnd);
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < num_bytes) {
        ssize_t n = read(fd, rnd + got, num_bytes - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got < num_bytes) return -1;
    for (size_t i = 0; i < num_bytes; i++) {
        snprintf(out + (i * 2), 3, "%02x", rnd[i]);
    }
    return 0;
}

/* Comparison that does not stop at the first different byte */
int http_auth_secret_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la != lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/* ---- Failed-login limiter, shared by every password check (token login and Basic) ---- */
struct auth_rate_limit_entry {
    char ip[48];
    int fail_count;
    time_t lock_until;
    time_t last_attempt;
};
#define MAX_RATE_LIMIT_ENTRIES 32
static struct auth_rate_limit_entry g_rate_limits[MAX_RATE_LIMIT_ENTRIES];
static pthread_mutex_t g_rate_limit_mutex = PTHREAD_MUTEX_INITIALIZER;

int http_auth_lock_remaining(const char *client_ip) {
    if (!client_ip || !client_ip[0]) return 0;
    time_t now = time(NULL);
    int remaining = 0;
    pthread_mutex_lock(&g_rate_limit_mutex);
    for (int i = 0; i < MAX_RATE_LIMIT_ENTRIES; i++) {
        if (g_rate_limits[i].ip[0] && strcmp(g_rate_limits[i].ip, client_ip) == 0) {
            if (now < g_rate_limits[i].lock_until) remaining = (int)(g_rate_limits[i].lock_until - now);
            break;
        }
    }
    pthread_mutex_unlock(&g_rate_limit_mutex);
    return remaining;
}

void http_auth_note_failure(const char *client_ip) {
    if (!client_ip || !client_ip[0]) return;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_rate_limit_mutex);
    int target_idx = -1;
    int free_idx = -1;
    time_t oldest_time = now;
    int oldest_idx = 0;
    for (int i = 0; i < MAX_RATE_LIMIT_ENTRIES; i++) {
        if (g_rate_limits[i].ip[0] && strcmp(g_rate_limits[i].ip, client_ip) == 0) {
            target_idx = i;
            break;
        }
        if (!g_rate_limits[i].ip[0] && free_idx < 0) free_idx = i;
        if (g_rate_limits[i].last_attempt < oldest_time) {
            oldest_time = g_rate_limits[i].last_attempt;
            oldest_idx = i;
        }
    }
    if (target_idx < 0) {
        /* a new address starts clean, also in a reused slot */
        target_idx = free_idx >= 0 ? free_idx : oldest_idx;
        memset(&g_rate_limits[target_idx], 0, sizeof(g_rate_limits[target_idx]));
        strncpy(g_rate_limits[target_idx].ip, client_ip, sizeof(g_rate_limits[target_idx].ip) - 1);
    }
    g_rate_limits[target_idx].fail_count++;
    g_rate_limits[target_idx].last_attempt = now;
    if (g_rate_limits[target_idx].fail_count >= 3) {
        /* 3 failures = 2 s, 4 = 4 s, 5 = 8 s ... at most 30 s */
        int shift = g_rate_limits[target_idx].fail_count - 2;
        int delay_sec = (shift <= 4) ? (1 << shift) : 30;
        g_rate_limits[target_idx].lock_until = now + delay_sec;
    }
    pthread_mutex_unlock(&g_rate_limit_mutex);
}

void http_auth_note_success(const char *client_ip) {
    if (!client_ip || !client_ip[0]) return;
    pthread_mutex_lock(&g_rate_limit_mutex);
    for (int i = 0; i < MAX_RATE_LIMIT_ENTRIES; i++) {
        if (g_rate_limits[i].ip[0] && strcmp(g_rate_limits[i].ip, client_ip) == 0) {
            g_rate_limits[i].fail_count = 0;
            g_rate_limits[i].lock_until = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_rate_limit_mutex);
}

/* Password check for one account (stored HA1, or a legacy plain password) */
int http_auth_password_matches(const struct user_account *u, const char *password) {
    if (!u || !password) return 0;
    if (u->ha1[0]) {
        char ha1_calc[65];
        compute_ha1(u->username, HARMONY_AUTH_REALM, password, ha1_calc);
        return http_auth_secret_equal(u->ha1, ha1_calc);
    }
    if (u->password[0]) return http_auth_secret_equal(u->password, password);
    return 0;
}

void compute_ha1(const char *username, const char *realm, const char *password, char ha1_out[65]) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s:%s:%s", username, realm ? realm : HARMONY_AUTH_REALM, password ? password : "");
    sha256_hex((const uint8_t *)buf, strlen(buf), ha1_out);
}

int create_auth_session(const char *username, enum http_role role, const char *client_ip, char *out_token, size_t out_len) {
    pthread_mutex_lock(&s_session_mutex);
    time_t now = time(NULL);

    int slot = -1;
    time_t oldest_time = now;
    int oldest_idx = 0;

    for (int i = 0; i < MAX_AUTH_SESSIONS; i++) {
        if (!s_sessions[i].active || s_sessions[i].expires_at < now) {
            slot = i;
            break;
        }
        if (s_sessions[i].last_seen < oldest_time) {
            oldest_time = s_sessions[i].last_seen;
            oldest_idx = i;
        }
    }
    if (slot < 0) slot = oldest_idx;

    char hex[65] = {0};
    if (generate_random_hex(hex, 32) != 0) {
        pthread_mutex_unlock(&s_session_mutex);
        return 0;
    }

    snprintf(s_sessions[slot].token, sizeof(s_sessions[slot].token), "hht_%s", hex);
    strncpy(s_sessions[slot].username, username ? username : "admin", sizeof(s_sessions[slot].username) - 1);
    s_sessions[slot].role = role;
    s_sessions[slot].created_at = now;
    s_sessions[slot].expires_at = now + AUTH_SESSION_TTL;
    s_sessions[slot].last_seen = now;
    if (client_ip) strncpy(s_sessions[slot].client_ip, client_ip, sizeof(s_sessions[slot].client_ip) - 1);
    s_sessions[slot].active = 1;

    if (out_token && out_len > 0) {
        strncpy(out_token, s_sessions[slot].token, out_len - 1);
        out_token[out_len - 1] = '\0';
    }

    pthread_mutex_unlock(&s_session_mutex);
    return 1;
}

static const struct user_account *find_account(const struct user_account *users, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(users[i].username, name) == 0) return &users[i];
    }
    return NULL;
}

/* A session counts only while its account exists, with the account's current role: deleting
 * or demoting an account takes effect at once. */
static enum http_role validate_auth_session(const char *token, const struct user_account *users, int count,
                                            char *out_user, size_t out_user_len) {
    if (!token || !*token) return ROLE_NONE;
    pthread_mutex_lock(&s_session_mutex);
    time_t now = time(NULL);

    for (int i = 0; i < MAX_AUTH_SESSIONS; i++) {
        if (s_sessions[i].active && http_auth_secret_equal(s_sessions[i].token, token)) {
            const struct user_account *acc = find_account(users, count, s_sessions[i].username);
            if (s_sessions[i].expires_at < now || !acc) {
                s_sessions[i].active = 0;
                pthread_mutex_unlock(&s_session_mutex);
                return ROLE_NONE;
            }
            s_sessions[i].last_seen = now;
            enum http_role role = acc->role;
            if (out_user && out_user_len > 0) {
                strncpy(out_user, s_sessions[i].username, out_user_len - 1);
                out_user[out_user_len - 1] = '\0';
            }
            pthread_mutex_unlock(&s_session_mutex);
            return role;
        }
    }

    pthread_mutex_unlock(&s_session_mutex);
    return ROLE_NONE;
}

/* End every session of username (account deleted, renamed, demoted or new password). */
void http_auth_drop_user_sessions(const char *username) {
    if (!username || !*username) return;
    pthread_mutex_lock(&s_session_mutex);
    for (int i = 0; i < MAX_AUTH_SESSIONS; i++) {
        if (s_sessions[i].active && strcmp(s_sessions[i].username, username) == 0) s_sessions[i].active = 0;
    }
    pthread_mutex_unlock(&s_session_mutex);
}

/* End every session (authentication turned on or off). */
void http_auth_drop_all_sessions(void) {
    pthread_mutex_lock(&s_session_mutex);
    for (int i = 0; i < MAX_AUTH_SESSIONS; i++) s_sessions[i].active = 0;
    pthread_mutex_unlock(&s_session_mutex);
}

int destroy_auth_session(const char *token) {
    if (!token || !*token) return 0;
    pthread_mutex_lock(&s_session_mutex);
    for (int i = 0; i < MAX_AUTH_SESSIONS; i++) {
        if (s_sessions[i].active && strcmp(s_sessions[i].token, token) == 0) {
            s_sessions[i].active = 0;
            pthread_mutex_unlock(&s_session_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&s_session_mutex);
    return 0;
}

static enum http_role validate_pat_token(const char *token, const struct user_account *users, int count,
                                        char *out_user, size_t out_user_len) {
    if (!token || !*token) return ROLE_NONE;

    char hash[65];
    sha256_hex((const uint8_t *)token, strlen(token), hash);

    const char *path = HARMONY_TOKENS_JSON;
    size_t flen = 0;
    char *json_txt = read_file_text_alloc(path, 65536, &flen);
    if (!json_txt) return ROLE_NONE;

    cJSON *root = cJSON_Parse(json_txt);
    free(json_txt);
    if (!root) return ROLE_NONE;

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "tokens");
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return ROLE_NONE;
    }

    enum http_role matched_role = ROLE_NONE;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jh = cJSON_GetObjectItemCaseSensitive(it, "token_hash");
        if (jh && cJSON_IsString(jh) && http_auth_secret_equal(jh->valuestring, hash)) {
            cJSON *jr = cJSON_GetObjectItemCaseSensitive(it, "role");
            cJSON *jn = cJSON_GetObjectItemCaseSensitive(it, "name");
            cJSON *jo = cJSON_GetObjectItemCaseSensitive(it, "owner");
            const char *rstr = (jr && cJSON_IsString(jr)) ? jr->valuestring : "admin";
            matched_role = (strcasecmp(rstr, "user") == 0) ? ROLE_USER : ROLE_ADMIN;
            /* A token created by an account lives and dies with it, and never has more rights
             * than the account has now. Tokens from before owners were recorded keep their role. */
            if (jo && cJSON_IsString(jo) && jo->valuestring[0]) {
                const struct user_account *acc = find_account(users, count, jo->valuestring);
                if (!acc) {
                    matched_role = ROLE_NONE;
                    break;
                }
                if (acc->role < matched_role) matched_role = acc->role;
            }
            if (out_user && out_user_len > 0) {
                if (jn && cJSON_IsString(jn)) {
                    snprintf(out_user, out_user_len, "pat:%s", jn->valuestring);
                } else {
                    strncpy(out_user, "pat", out_user_len - 1);
                }
            }
            break;
        }
    }

    cJSON_Delete(root);
    return matched_role;
}

int load_user_accounts(struct user_account *users, int max_users, int *auth_enabled) {
    if (auth_enabled) *auth_enabled = 0;
    int count = 0;

    const char *path = HARMONY_USERS_JSON;
    size_t flen = 0;
    /* users.json present but unreadable or broken (cut by a power loss, full partition) must not
     * read as "authentication off": fail closed unless the legacy file still has accounts. */
    int users_file_broken = (access(path, F_OK) == 0);
    char *json_txt = read_file_text_alloc(path, 65536, &flen);
    if (json_txt) {
        cJSON *root = cJSON_Parse(json_txt);
        free(json_txt);
        if (root) {
            users_file_broken = 0;
            cJSON *j_en = cJSON_GetObjectItemCaseSensitive(root, "auth_enabled");
            int is_en = j_en ? (cJSON_IsTrue(j_en) || (cJSON_IsNumber(j_en) && j_en->valueint != 0)) : 1;
            int needs_resave = 0;
            cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "users");
            if (arr && cJSON_IsArray(arr)) {
                const cJSON *it = NULL;
                cJSON_ArrayForEach(it, arr) {
                    if (users && count >= max_users) break;
                    cJSON *ju = cJSON_GetObjectItemCaseSensitive(it, "username");
                    cJSON *jp = cJSON_GetObjectItemCaseSensitive(it, "password");
                    cJSON *jha = cJSON_GetObjectItemCaseSensitive(it, "ha1");
                    cJSON *jr = cJSON_GetObjectItemCaseSensitive(it, "role");
                    if (ju && cJSON_IsString(ju) && strlen(ju->valuestring) >= MAX_USERNAME_LEN) {
                        printf("[-] AUTH: skipping account with a name of %u bytes or more\n", (unsigned)MAX_USERNAME_LEN);
                        continue;
                    }
                    if (ju && cJSON_IsString(ju)) {
                        if (users && count < max_users) {
                            /* The caller's array is not initialized: a 64-character ha1 copied
                             * with strncpy would have no terminator */
                            memset(&users[count], 0, sizeof(users[count]));
                            strncpy(users[count].username, ju->valuestring, sizeof(users[count].username) - 1);
                            users[count].password[0] = '\0';
                            if (jha && cJSON_IsString(jha) && (strlen(jha->valuestring) == 64 || strlen(jha->valuestring) == 32)) {
                                strncpy(users[count].ha1, jha->valuestring, sizeof(users[count].ha1) - 1);
                            } else if (jp && cJSON_IsString(jp) && jp->valuestring[0]) {
                                compute_ha1(users[count].username, HARMONY_AUTH_REALM, jp->valuestring, users[count].ha1);
                                needs_resave = 1;
                            } else {
                                users[count].ha1[0] = '\0';
                            }
                            if (jp && cJSON_IsString(jp)) needs_resave = 1;
                            const char *rstr = (jr && cJSON_IsString(jr)) ? jr->valuestring : "admin";
                            users[count].role = (strcasecmp(rstr, "user") == 0) ? ROLE_USER : ROLE_ADMIN;
                        }
                        count++;
                    }
                }
            }
            /* Enabled with no account left also fails closed (only loopback and SSH get in) */
            if (auth_enabled) *auth_enabled = is_en;
            cJSON_Delete(root);
            if (needs_resave && users && count > 0) {
                save_user_accounts(users, count, (auth_enabled ? *auth_enabled : 1));
            }
            return count;
        }
    }

    const char *conf_path = HARMONY_AUTH_CONF;
    FILE *f = fopen(conf_path, "r");
    if (!f) {
        if (users_file_broken) {
            if (auth_enabled) *auth_enabled = 1;
            if (!s_accounts_unreadable_logged) {
                printf("[-] AUTH: %s cannot be read: refusing every non-loopback request\n", path);
                s_accounts_unreadable_logged = 1;
            }
        }
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (users && count >= max_users) break;
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        nl = strchr(line, '\r'); if (nl) *nl = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;

        char *c1 = strchr(p, ':');
        if (!c1) continue;
        *c1 = '\0';
        char *user = p;
        char *pass = c1 + 1;
        char *role_str = NULL;
        char *c2 = strchr(pass, ':');
        if (c2) {
            *c2 = '\0';
            role_str = c2 + 1;
        }

        if (strlen(user) >= MAX_USERNAME_LEN) {
            printf("[-] AUTH: skipping account with a name of %u bytes or more in %s\n", (unsigned)MAX_USERNAME_LEN, conf_path);
            continue;
        }
        if (users && count < max_users) {
            memset(&users[count], 0, sizeof(users[count]));
            strncpy(users[count].username, user, sizeof(users[count].username) - 1);
            size_t pass_len = strlen(pass);
            int is_hex_hash = (pass_len == 64 || pass_len == 32);
            if (is_hex_hash) {
                for (size_t k = 0; k < pass_len; k++) {
                    if (!isxdigit((unsigned char)pass[k])) { is_hex_hash = 0; break; }
                }
            }
            if (is_hex_hash) {
                strncpy(users[count].ha1, pass, sizeof(users[count].ha1) - 1);
            } else {
                compute_ha1(user, HARMONY_AUTH_REALM, pass, users[count].ha1);
            }
            if (role_str && strcasecmp(role_str, "user") == 0) {
                users[count].role = ROLE_USER;
            } else {
                users[count].role = ROLE_ADMIN;
            }
        }
        count++;
    }
    fclose(f);

    if (auth_enabled) *auth_enabled = (count > 0) || users_file_broken;
    if (users_file_broken && count == 0 && !s_accounts_unreadable_logged) {
        printf("[-] AUTH: %s cannot be read: refusing every non-loopback request\n", path);
        s_accounts_unreadable_logged = 1;
    }
    return count;
}

int save_user_accounts(const struct user_account *users, int count, int auth_enabled) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return 0;
    cJSON_AddBoolToObject(root, "auth_enabled", auth_enabled ? 1 : 0);
    cJSON_AddStringToObject(root, "realm", HARMONY_AUTH_REALM);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "username", users[i].username);
        /* Plaintext password is NEVER saved to JSON; only HA1 */
        if (users[i].ha1[0]) {
            cJSON_AddStringToObject(u, "ha1", users[i].ha1);
        } else if (users[i].password[0]) {
            char ha1_calc[65];
            compute_ha1(users[i].username, HARMONY_AUTH_REALM, users[i].password, ha1_calc);
            cJSON_AddStringToObject(u, "ha1", ha1_calc);
        }
        cJSON_AddStringToObject(u, "role", users[i].role == ROLE_USER ? "user" : "admin");
        cJSON_AddItemToArray(arr, u);
    }
    cJSON_AddItemToObject(root, "users", arr);

    char *json_out = cJSON_Print(root);
    cJSON_Delete(root);
    if (json_out) {
        mkdir("/data/harmony", 0755);
        mkdir("/data/harmony/config", 0755);
        write_file_atomic_sized(HARMONY_USERS_JSON, json_out, strlen(json_out));
        free(json_out);
    }

    if (!auth_enabled || count == 0) {
        write_file_atomic_sized(HARMONY_AUTH_CONF, "# auth disabled\n", 16);
    } else {
        char conf_buf[2048] = {0};
        int offset = 0;
        for (int i = 0; i < count; i++) {
            const char *cred = users[i].ha1[0] ? users[i].ha1 : "";
            char ha1_buf[65];
            if (!cred[0] && users[i].password[0]) {
                compute_ha1(users[i].username, HARMONY_AUTH_REALM, users[i].password, ha1_buf);
                cred = ha1_buf;
            }
            int written = snprintf(conf_buf + offset, sizeof(conf_buf) - offset,
                "%s:%s:%s\n", users[i].username, cred,
                users[i].role == ROLE_USER ? "user" : "admin");
            if (written > 0 && offset + written < (int)sizeof(conf_buf)) {
                offset += written;
            }
        }
        write_file_atomic_sized(HARMONY_AUTH_CONF, conf_buf, (size_t)offset);
    }
    return 1;
}

enum http_role http_auth_check_request(const char *req, const char *client_ip,
                                       const char *method, char *out_user, size_t out_user_len) {
    if (out_user && out_user_len > 0) out_user[0] = '\0';

    /* Local loopback IPC (hub_rf, hub_btstack) is always trusted as admin */
    if (client_ip && (strcmp(client_ip, "127.0.0.1") == 0 || strcmp(client_ip, "::1") == 0)) {
        if (out_user && out_user_len > 0) strncpy(out_user, "admin", out_user_len - 1);
        return ROLE_ADMIN;
    }

    struct user_account users[MAX_HTTP_USERS];
    int auth_enabled = 0;
    int count = load_user_accounts(users, MAX_HTTP_USERS, &auth_enabled);

    if (!auth_enabled) {
        if (out_user && out_user_len > 0) strncpy(out_user, "admin", out_user_len - 1);
        return ROLE_ADMIN;
    }
    if (count == 0) return ROLE_NONE; /* enabled but no readable account: fail closed */

    const char *hdr = strcasestr(req, "Authorization:");
    if (!hdr) return ROLE_NONE;
    hdr += 14;
    while (*hdr == ' ' || *hdr == '\t') hdr++;

    /* 1. Bearer: session token or personal access token (random 128-bit values, not limited) */
    if (strncasecmp(hdr, "Bearer ", 7) == 0) {
        hdr += 7;
        while (*hdr == ' ' || *hdr == '\t') hdr++;
        char token[128] = {0};
        int ti = 0;
        while (*hdr && *hdr != '\r' && *hdr != '\n' && *hdr != ' ' && ti < (int)sizeof(token) - 1) {
            token[ti++] = *hdr++;
        }
        token[ti] = '\0';

        if (strncmp(token, "pat_", 4) == 0) {
            return validate_pat_token(token, users, count, out_user, out_user_len);
        }
        return validate_auth_session(token, users, count, out_user, out_user_len);
    }

    /* 2. Basic: a password check like the login route, under the same failed-login limiter */
    if (strncasecmp(hdr, "Basic ", 6) == 0) {
        if (http_auth_lock_remaining(client_ip) > 0) return ROLE_NONE;
        hdr += 6;
        while (*hdr == ' ' || *hdr == '\t') hdr++;

        char b64[256] = {0};
        int bi = 0;
        while (*hdr && *hdr != '\r' && *hdr != '\n' && *hdr != ' ' && bi < (int)sizeof(b64) - 1) {
            b64[bi++] = *hdr++;
        }
        b64[bi] = '\0';

        size_t dlen = 0;
        unsigned char *decoded = b64_decode(b64, &dlen);
        if (!decoded) {
            http_auth_note_failure(client_ip);
            return ROLE_NONE;
        }
        decoded[dlen] = '\0';

        enum http_role matched_role = ROLE_NONE;
        char *c = strchr((char *)decoded, ':');
        if (c) {
            *c = '\0';
            const struct user_account *acc = find_account(users, count, (const char *)decoded);
            if (acc && http_auth_password_matches(acc, c + 1)) {
                matched_role = acc->role;
                if (out_user && out_user_len > 0) strncpy(out_user, acc->username, out_user_len - 1);
            }
        }
        free(decoded);
        if (matched_role == ROLE_NONE) http_auth_note_failure(client_ip);
        else http_auth_note_success(client_ip);
        return matched_role;
    }

    return ROLE_NONE;
}

enum http_role http_auth_check_ws_upgrade(const char *req, const char *client_ip,
                                          char *out_user, size_t out_user_len) {
    if (!req) return ROLE_NONE;
    if (strcasestr(req, "Authorization:")) {
        return http_auth_check_request(req, client_ip, "GET", out_user, out_user_len);
    }
    /* token in the query string of the request line only (never read it from a body) */
    const char *line_end = strpbrk(req, "\r\n");
    size_t line_len = line_end ? (size_t)(line_end - req) : strlen(req);
    const char *q = NULL;
    for (const char *p = req; p + 6 <= req + line_len; p++) {
        if ((p == req || *(p - 1) == '?' || *(p - 1) == '&') && strncmp(p, "token=", 6) == 0) {
            q = p + 6;
            break;
        }
    }
    char token[128] = {0};
    if (q) {
        size_t ti = 0;
        while (q < req + line_len && *q != '&' && *q != ' ' && ti < sizeof(token) - 1) token[ti++] = *q++;
        token[ti] = '\0';
    }
    /* Reuse the header path: a synthetic request with the token as a Bearer header keeps the
     * loopback, auth-disabled, PAT and session rules in one place */
    char synth[256];
    snprintf(synth, sizeof(synth), "GET /ws HTTP/1.1\r\nAuthorization: Bearer %s\r\n\r\n", token);
    return http_auth_check_request(token[0] ? synth : "GET /ws HTTP/1.1\r\n\r\n", client_ip, "GET", out_user, out_user_len);
}

int http_auth_is_user_role_permitted(const char *method, const char *clean_path) {
    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        if (strcmp(clean_path, "/api/status") == 0 ||
            strcmp(clean_path, "/api/auth/me") == 0 ||
            strcmp(clean_path, "/api/activities") == 0 ||
            strcmp(clean_path, "/api/activity") == 0 ||
            strcmp(clean_path, "/api/current_activity") == 0 ||
            strcmp(clean_path, "/api/devices") == 0 ||
            strcmp(clean_path, "/api/inventory") == 0 ||
            strcmp(clean_path, "/api/icons") == 0 ||
            strcmp(clean_path, "/api/icons/archive") == 0 ||
            strcmp(clean_path, "/api/custom-icons") == 0 ||
            strcmp(clean_path, "/api/ui-remote-layout") == 0 ||
            strcmp(clean_path, "/assets/icons.tar.gz") == 0 ||
            strncmp(clean_path, "/api/icons/", 11) == 0 ||
            strncmp(clean_path, "/assets/", 8) == 0) {
            return 1;
        }
        return 0;
    }

    /* Control only: start and stop activities, send stored or raw commands and keys. Saving
     * commands (commands-batch), broadcasting progress frames (activity-progress) and every
     * other route need the admin role. */
    if (strcmp(method, "POST") == 0) {
        if (strcmp(clean_path, "/api/auth/logout") == 0 ||
            strcmp(clean_path, "/api/activity") == 0 ||
            strcmp(clean_path, "/api/activity-start") == 0 ||
            strcmp(clean_path, "/api/activity-stop") == 0 ||
            strcmp(clean_path, "/api/poweroff") == 0 ||
            strcmp(clean_path, "/api/activity-cancel") == 0 ||
            strcmp(clean_path, "/api/ir-send") == 0 ||
            strcmp(clean_path, "/api/ir-raw") == 0 ||
            strcmp(clean_path, "/ir/send") == 0 ||
            strcmp(clean_path, "/api/button-press") == 0 ||
            strcmp(clean_path, "/api/bt-key") == 0 ||
            strcmp(clean_path, "/api/bt-text") == 0 ||
            strcmp(clean_path, "/api/bt-mouse") == 0 ||
            strcmp(clean_path, "/api/v1/bt/mouse") == 0) {
            return 1;
        }
        return 0;
    }

    return 0;
}

void http_auth_send_challenge(int client_fd, const char *client_ip) {
    (void)client_ip;
    char unauth[512];
    snprintf(unauth, sizeof(unauth),
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Bearer realm=\"%s\"\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: Authorization, Content-Type, X-Requested-With, Accept, Origin\r\n"
        "Content-Length: 35\r\n"
        "Connection: close\r\n\r\n"
        "{\"ok\":false,\"error\":\"Unauthorized\"}",
        HARMONY_AUTH_REALM);
    send(client_fd, unauth, strlen(unauth), 0);
    close(client_fd);
}
