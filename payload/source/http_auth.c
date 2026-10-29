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
static uint8_t s_digest_secret[32];
static int s_digest_secret_inited = 0;

static void ensure_digest_secret(void) {
    if (s_digest_secret_inited) return;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, s_digest_secret, sizeof(s_digest_secret)) != sizeof(s_digest_secret)) {
            for (size_t i = 0; i < sizeof(s_digest_secret); i++) {
                s_digest_secret[i] = (uint8_t)(rand() & 0xFF);
            }
        }
        close(fd);
    } else {
        for (size_t i = 0; i < sizeof(s_digest_secret); i++) {
            s_digest_secret[i] = (uint8_t)(rand() & 0xFF);
        }
    }
    s_digest_secret_inited = 1;
}

void http_auth_init(void) {
    ensure_digest_secret();
}

static void generate_random_hex(char *out, size_t num_bytes) {
    int fd = open("/dev/urandom", O_RDONLY);
    uint8_t rnd[32];
    if (num_bytes > sizeof(rnd)) num_bytes = sizeof(rnd);
    if (fd >= 0) {
        if (read(fd, rnd, num_bytes) != (ssize_t)num_bytes) {
            for (size_t i = 0; i < num_bytes; i++) rnd[i] = (uint8_t)(rand() & 0xFF);
        }
        close(fd);
    } else {
        for (size_t i = 0; i < num_bytes; i++) rnd[i] = (uint8_t)(rand() & 0xFF);
    }
    for (size_t i = 0; i < num_bytes; i++) {
        sprintf(out + (i * 2), "%02x", rnd[i]);
    }
}

void compute_ha1(const char *username, const char *realm, const char *password, char ha1_out[65]) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s:%s:%s", username, realm ? realm : HARMONY_AUTH_REALM, password ? password : "");
    sha256_hex((const uint8_t *)buf, strlen(buf), ha1_out);
}

__attribute__((unused)) static void generate_digest_nonce(const char *client_ip, char *out_nonce, size_t out_len) {
    ensure_digest_secret();
    uint32_t ts = (uint32_t)time(NULL);
    char ts_str[16];
    snprintf(ts_str, sizeof(ts_str), "%08x", ts);

    char sign_data[128];
    snprintf(sign_data, sizeof(sign_data), "%s:%s", ts_str, client_ip ? client_ip : "");

    char hmac[65];
    sha256_hmac_hex(s_digest_secret, sizeof(s_digest_secret),
                    (const uint8_t *)sign_data, strlen(sign_data), hmac);
    hmac[32] = '\0';
    snprintf(out_nonce, out_len, "%s.%s", ts_str, hmac);
}

static int validate_digest_nonce(const char *nonce, const char *client_ip) {
    if (!nonce) return 0;
    ensure_digest_secret();

    const char *dot = strchr(nonce, '.');
    if (!dot) return 0;

    char ts_str[16] = {0};
    size_t ts_len = (size_t)(dot - nonce);
    if (ts_len >= sizeof(ts_str)) return 0;
    memcpy(ts_str, nonce, ts_len);
    ts_str[ts_len] = '\0';

    unsigned long ts = strtoul(ts_str, NULL, 16);
    time_t now = time(NULL);
    if (now < (time_t)ts || (now - (time_t)ts) > 300) {
        return 0;
    }

    char sign_data[128];
    snprintf(sign_data, sizeof(sign_data), "%s:%s", ts_str, client_ip ? client_ip : "");

    char hmac[65];
    sha256_hmac_hex(s_digest_secret, sizeof(s_digest_secret),
                    (const uint8_t *)sign_data, strlen(sign_data), hmac);
    hmac[32] = '\0';

    if (strcmp(dot + 1, hmac) == 0) return 1;

    if (client_ip && client_ip[0]) {
        snprintf(sign_data, sizeof(sign_data), "%s:", ts_str);
        sha256_hmac_hex(s_digest_secret, sizeof(s_digest_secret),
                        (const uint8_t *)sign_data, strlen(sign_data), hmac);
        hmac[32] = '\0';
        if (strcmp(dot + 1, hmac) == 0) return 1;
    }

    return 0;
}

static int extract_digest_param(const char *header, const char *key, char *out, size_t out_len) {
    if (!header || !key || !out || out_len == 0) return 0;
    out[0] = '\0';

    size_t klen = strlen(key);
    const char *p = header;
    while ((p = strstr(p, key)) != NULL) {
        if (p != header && *(p - 1) != ' ' && *(p - 1) != ',' && *(p - 1) != '\t') {
            p += klen;
            continue;
        }
        const char *eq = p + klen;
        while (*eq == ' ' || *eq == '\t') eq++;
        if (*eq != '=') {
            p += klen;
            continue;
        }
        eq++;
        while (*eq == ' ' || *eq == '\t') eq++;

        int quoted = (*eq == '"');
        if (quoted) eq++;

        size_t idx = 0;
        while (*eq && idx < out_len - 1) {
            if (quoted) {
                if (*eq == '"') break;
            } else {
                if (*eq == ',' || *eq == ' ' || *eq == '\r' || *eq == '\n' || *eq == '\t') break;
            }
            out[idx++] = *eq++;
        }
        out[idx] = '\0';
        return 1;
    }
    return 0;
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
    generate_random_hex(hex, 32);

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

static enum http_role validate_auth_session(const char *token, char *out_user, size_t out_user_len) {
    if (!token || !*token) return ROLE_NONE;
    pthread_mutex_lock(&s_session_mutex);
    time_t now = time(NULL);

    for (int i = 0; i < MAX_AUTH_SESSIONS; i++) {
        if (s_sessions[i].active && strcmp(s_sessions[i].token, token) == 0) {
            if (s_sessions[i].expires_at < now) {
                s_sessions[i].active = 0;
                pthread_mutex_unlock(&s_session_mutex);
                return ROLE_NONE;
            }
            s_sessions[i].last_seen = now;
            enum http_role role = s_sessions[i].role;
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

static enum http_role validate_pat_token(const char *token, char *out_user, size_t out_user_len) {
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
        if (jh && cJSON_IsString(jh) && strcmp(jh->valuestring, hash) == 0) {
            cJSON *jr = cJSON_GetObjectItemCaseSensitive(it, "role");
            cJSON *jn = cJSON_GetObjectItemCaseSensitive(it, "name");
            const char *rstr = (jr && cJSON_IsString(jr)) ? jr->valuestring : "admin";
            matched_role = (strcasecmp(rstr, "user") == 0) ? ROLE_USER : ROLE_ADMIN;
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
    char *json_txt = read_file_text_alloc(path, 65536, &flen);
    if (json_txt) {
        cJSON *root = cJSON_Parse(json_txt);
        free(json_txt);
        if (root) {
            cJSON *j_en = cJSON_GetObjectItemCaseSensitive(root, "auth_enabled");
            int is_en = j_en ? (cJSON_IsTrue(j_en) || (cJSON_IsNumber(j_en) && j_en->valueint != 0)) : 1;
            cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "users");
            if (arr && cJSON_IsArray(arr)) {
                const cJSON *it = NULL;
                cJSON_ArrayForEach(it, arr) {
                    if (users && count >= max_users) break;
                    cJSON *ju = cJSON_GetObjectItemCaseSensitive(it, "username");
                    cJSON *jp = cJSON_GetObjectItemCaseSensitive(it, "password");
                    cJSON *jha = cJSON_GetObjectItemCaseSensitive(it, "ha1");
                    cJSON *jr = cJSON_GetObjectItemCaseSensitive(it, "role");
                    if (ju && cJSON_IsString(ju)) {
                        if (users && count < max_users) {
                            strncpy(users[count].username, ju->valuestring, sizeof(users[count].username) - 1);
                            if (jp && cJSON_IsString(jp)) {
                                strncpy(users[count].password, jp->valuestring, sizeof(users[count].password) - 1);
                            } else {
                                users[count].password[0] = '\0';
                            }
                            if (jha && cJSON_IsString(jha) && strlen(jha->valuestring) == 64) {
                                strncpy(users[count].ha1, jha->valuestring, sizeof(users[count].ha1) - 1);
                            } else if (users[count].password[0]) {
                                compute_ha1(users[count].username, HARMONY_AUTH_REALM, users[count].password, users[count].ha1);
                            } else {
                                users[count].ha1[0] = '\0';
                            }
                            const char *rstr = (jr && cJSON_IsString(jr)) ? jr->valuestring : "admin";
                            users[count].role = (strcasecmp(rstr, "user") == 0) ? ROLE_USER : ROLE_ADMIN;
                        }
                        count++;
                    }
                }
            }
            if (auth_enabled) *auth_enabled = (count > 0 && is_en);
            cJSON_Delete(root);
            return count;
        }
    }

    const char *conf_path = HARMONY_AUTH_CONF;
    FILE *f = fopen(conf_path, "r");
    if (!f) return 0;

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

        if (users && count < max_users) {
            strncpy(users[count].username, user, sizeof(users[count].username) - 1);
            strncpy(users[count].password, pass, sizeof(users[count].password) - 1);
            compute_ha1(user, HARMONY_AUTH_REALM, pass, users[count].ha1);
            if (role_str && strcasecmp(role_str, "user") == 0) {
                users[count].role = ROLE_USER;
            } else {
                users[count].role = ROLE_ADMIN;
            }
        }
        count++;
    }
    fclose(f);

    if (auth_enabled) *auth_enabled = (count > 0);
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
        if (users[i].password[0]) {
            cJSON_AddStringToObject(u, "password", users[i].password);
        }
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
            int written = snprintf(conf_buf + offset, sizeof(conf_buf) - offset,
                "%s:%s:%s\n", users[i].username, users[i].password,
                users[i].role == ROLE_USER ? "user" : "admin");
            if (written > 0 && offset + written < (int)sizeof(conf_buf)) {
                offset += written;
            }
        }
        write_file_atomic_sized(HARMONY_AUTH_CONF, conf_buf, (size_t)offset);
    }
    return 1;
}

static enum http_role check_digest_auth(const char *digest_hdr, const char *client_ip,
                                        const char *method, const struct user_account *users,
                                        int user_count, char *out_user, size_t out_user_len) {
    char username[MAX_USERNAME_LEN] = {0};
    char realm[64] = {0};
    char nonce[128] = {0};
    char uri[256] = {0};
    char response[128] = {0};
    char qop[32] = {0};
    char nc[32] = {0};
    char cnonce[64] = {0};

    if (!extract_digest_param(digest_hdr, "username", username, sizeof(username))) return ROLE_NONE;
    extract_digest_param(digest_hdr, "realm", realm, sizeof(realm));
    if (!extract_digest_param(digest_hdr, "nonce", nonce, sizeof(nonce))) return ROLE_NONE;
    if (!extract_digest_param(digest_hdr, "uri", uri, sizeof(uri))) return ROLE_NONE;
    if (!extract_digest_param(digest_hdr, "response", response, sizeof(response))) return ROLE_NONE;
    extract_digest_param(digest_hdr, "qop", qop, sizeof(qop));
    extract_digest_param(digest_hdr, "nc", nc, sizeof(nc));
    extract_digest_param(digest_hdr, "cnonce", cnonce, sizeof(cnonce));

    if (realm[0] && strcmp(realm, HARMONY_AUTH_REALM) != 0) return ROLE_NONE;
    if (!validate_digest_nonce(nonce, client_ip)) return ROLE_NONE;

    int user_idx = -1;
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].username, username) == 0) {
            user_idx = i;
            break;
        }
    }
    if (user_idx < 0) return ROLE_NONE;

    char ha1[65] = {0};
    if (users[user_idx].ha1[0]) {
        strncpy(ha1, users[user_idx].ha1, sizeof(ha1) - 1);
    } else if (users[user_idx].password[0]) {
        compute_ha1(users[user_idx].username, HARMONY_AUTH_REALM, users[user_idx].password, ha1);
    } else {
        return ROLE_NONE;
    }

    char ha2_str[512];
    snprintf(ha2_str, sizeof(ha2_str), "%s:%s", method ? method : "GET", uri);
    char ha2[65];
    sha256_hex((const uint8_t *)ha2_str, strlen(ha2_str), ha2);

    char expected_resp[65];
    if (strcmp(qop, "auth") == 0) {
        char resp_str[1024];
        snprintf(resp_str, sizeof(resp_str), "%s:%s:%s:%s:%s:%s",
                 ha1, nonce, nc, cnonce, qop, ha2);
        sha256_hex((const uint8_t *)resp_str, strlen(resp_str), expected_resp);
    } else {
        char resp_str[1024];
        snprintf(resp_str, sizeof(resp_str), "%s:%s:%s", ha1, nonce, ha2);
        sha256_hex((const uint8_t *)resp_str, strlen(resp_str), expected_resp);
    }

    if (strcasecmp(response, expected_resp) == 0) {
        if (out_user && out_user_len > 0) {
            strncpy(out_user, users[user_idx].username, out_user_len - 1);
            out_user[out_user_len - 1] = '\0';
        }
        return users[user_idx].role;
    }

    return ROLE_NONE;
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

    if (!auth_enabled || count == 0) {
        if (out_user && out_user_len > 0) strncpy(out_user, "admin", out_user_len - 1);
        return ROLE_ADMIN;
    }

    const char *hdr = strcasestr(req, "Authorization:");
    if (!hdr) return ROLE_NONE;
    hdr += 14;
    while (*hdr == ' ' || *hdr == '\t') hdr++;

    /* 1. Bearer Token Auth */
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
            return validate_pat_token(token, out_user, out_user_len);
        }
        return validate_auth_session(token, out_user, out_user_len);
    }

    /* 2. Digest Auth */
    if (strncasecmp(hdr, "Digest ", 7) == 0) {
        return check_digest_auth(hdr + 7, client_ip, method, users, count, out_user, out_user_len);
    }

    /* 3. Basic Auth Fallback */
    if (strncasecmp(hdr, "Basic ", 6) == 0) {
        hdr += 6;
        while (*hdr == ' ' || *hdr == '\t') hdr++;

        char b64[128] = {0};
        int bi = 0;
        while (*hdr && *hdr != '\r' && *hdr != '\n' && *hdr != ' ' && bi < (int)sizeof(b64) - 1) {
            b64[bi++] = *hdr++;
        }
        b64[bi] = '\0';

        size_t dlen = 0;
        unsigned char *decoded = b64_decode(b64, &dlen);
        if (!decoded) return ROLE_NONE;
        decoded[dlen] = '\0';

        char *c = strchr((char *)decoded, ':');
        if (!c) {
            free(decoded);
            return ROLE_NONE;
        }
        *c = '\0';
        const char *u = (char *)decoded;
        const char *p = c + 1;

        enum http_role matched_role = ROLE_NONE;
        for (int i = 0; i < count; i++) {
            int match = 0;
            if (strcmp(users[i].username, u) == 0) {
                if (users[i].password[0] && strcmp(users[i].password, p) == 0) {
                    match = 1;
                } else if (users[i].ha1[0]) {
                    char ha1_calc[65];
                    compute_ha1(u, HARMONY_AUTH_REALM, p, ha1_calc);
                    if (strcmp(users[i].ha1, ha1_calc) == 0) {
                        match = 1;
                    }
                }
            }
            if (match) {
                matched_role = users[i].role;
                if (out_user && out_user_len > 0) {
                    strncpy(out_user, users[i].username, out_user_len - 1);
                }
                break;
            }
        }
        free(decoded);
        return matched_role;
    }

    return ROLE_NONE;
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

    if (strcmp(method, "POST") == 0) {
        if (strcmp(clean_path, "/api/auth/logout") == 0 ||
            strcmp(clean_path, "/api/activity") == 0 ||
            strcmp(clean_path, "/api/activity-start") == 0 ||
            strcmp(clean_path, "/api/activity-stop") == 0 ||
            strcmp(clean_path, "/api/poweroff") == 0 ||
            strcmp(clean_path, "/api/activity-cancel") == 0 ||
            strcmp(clean_path, "/api/activity-progress") == 0 ||
            strcmp(clean_path, "/api/ir-send") == 0 ||
            strcmp(clean_path, "/ir/send") == 0 ||
            strcmp(clean_path, "/api/button-press") == 0 ||
            strcmp(clean_path, "/api/bt-saved-command") == 0 ||
            strcmp(clean_path, "/api/bt-key") == 0 ||
            strcmp(clean_path, "/api/bt-text") == 0 ||
            strcmp(clean_path, "/api/bt-mouse") == 0 ||
            strcmp(clean_path, "/api/v1/bt/mouse") == 0 ||
            strcmp(clean_path, "/api/bt-script") == 0 ||
            strcmp(clean_path, "/api/commands-batch") == 0 ||
            strcmp(clean_path, "/api/ir-batch-send") == 0 ||
            strcmp(clean_path, "/api/ir-cancel") == 0) {
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
