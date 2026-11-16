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
#include <sys/stat.h>
#include <strings.h>

#include <pthread.h>

#define MAX_PASSWORD_BYTES 128 /* compute_ha1 hashes "user:realm:password" from a 256-byte buffer */

/* Delete the personal access tokens created by owner. Returns how many were removed. */
static int remove_tokens_of(const char *owner) {
    /* below 0 when the token file was busy, or could not be read or written (left as it was) */
    return http_tokens_remove_owner(owner);
}

/* The Bearer token of the request, or "" */
static void request_bearer(const char *req, char *out, size_t out_sz) {
    out[0] = '\0';
    const char *hdr = req ? strcasestr(req, "Authorization:") : NULL;
    if (!hdr) return;
    hdr += 14;
    while (*hdr == ' ' || *hdr == '\t') hdr++;
    if (strncasecmp(hdr, "Bearer ", 7) != 0) return;
    hdr += 7;
    while (*hdr == ' ' || *hdr == '\t') hdr++;
    size_t ti = 0;
    while (*hdr && *hdr != '\r' && *hdr != '\n' && *hdr != ' ' && ti < out_sz - 1) out[ti++] = *hdr++;
    out[ti] = '\0';
}

/* The account whose tokens an operator call may list and delete. The credential decides:
 * - a login (session or Basic): that account;
 * - a token: its owner, only when the token's role is user and the owner account is an operator
 *   (an operator's app). Any other token (a role user token an admin made, an unowned one) gets "",
 *   and may then see and delete only itself (own_hash, the calling token's hash).
 * root is the loaded token list. */
static void token_caller_account(const char *req, const char *client_ip, const char *method,
                                 const cJSON *root, char *out, size_t out_sz, char own_hash[65]) {
    out[0] = '\0';
    own_hash[0] = '\0';
    char btok[128];
    request_bearer(req, btok, sizeof(btok));
    if (strncmp(btok, "pat_", 4) != 0) {
        http_auth_check_request(req, client_ip, method, out, out_sz);
        return;
    }
    sha256_hex((const uint8_t *)btok, strlen(btok), own_hash);
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "tokens")) {
        const cJSON *jh = cJSON_GetObjectItemCaseSensitive(it, "token_hash");
        if (!cJSON_IsString(jh) || !http_auth_secret_equal(jh->valuestring, own_hash)) continue;
        const cJSON *jo = cJSON_GetObjectItemCaseSensitive(it, "owner");
        const cJSON *jr = cJSON_GetObjectItemCaseSensitive(it, "role");
        if (!cJSON_IsString(jo) || !cJSON_IsString(jr) || strcasecmp(jr->valuestring, "user") != 0) return;
        struct user_account ulist[MAX_HTTP_USERS];
        int on = 0;
        int n = load_user_accounts(ulist, MAX_HTTP_USERS, &on);
        for (int i = 0; i < n; i++) {
            if (strcmp(ulist[i].username, jo->valuestring) == 0 && ulist[i].role == ROLE_USER) {
                snprintf(out, out_sz, "%s", jo->valuestring);
                break;
            }
        }
        return;
    }
}

int http_api_auth_handle(int client_fd, const char *method, const char *clean_path,
                         const char *query, const char *body, const char *req,
                         const char *client_ip, enum http_role current_role) {
    /* GET /api/auth/me */
    if (strcmp(clean_path, "/api/auth/me") == 0 && strcmp(method, "GET") == 0) {
        int auth_on = 0;
        load_user_accounts(NULL, 0, &auth_on);
        char current_user[MAX_USERNAME_LEN] = {0};
        http_auth_check_request(req, client_ip, method, current_user, sizeof(current_user));

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddBoolToObject(resp, "auth_enabled", auth_on);
        cJSON_AddBoolToObject(resp, "authenticated", current_role != ROLE_NONE);
        cJSON_AddStringToObject(resp, "username", current_user[0] ? current_user : (current_role != ROLE_NONE ? "admin" : "anonymous"));
        cJSON_AddStringToObject(resp, "role", current_role == ROLE_USER ? "user" : (current_role == ROLE_ADMIN ? "admin" : "anonymous"));
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* POST /api/auth/token */
    if (strcmp(clean_path, "/api/auth/token") == 0 && strcmp(method, "POST") == 0) {
        int lock_secs = http_auth_lock_remaining(client_ip);
        if (lock_secs > 0) {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddBoolToObject(err, "ok", 0);
            char lock_msg[96];
            snprintf(lock_msg, sizeof(lock_msg), "Too many failed attempts. Try again in %d seconds", lock_secs);
            cJSON_AddStringToObject(err, "error", lock_msg);
            cJSON_AddNumberToObject(err, "retry_after", lock_secs);
            send_cjson_resp(client_fd, 429, err);
            cJSON_Delete(err);
            close(client_fd);
            return 1;
        }

        cJSON *params = parse_request_params(body);
        const char *username = "";
        const char *password = "";
        if (params) {
            cJSON *ju = cJSON_GetObjectItemCaseSensitive(params, "username");
            cJSON *jp = cJSON_GetObjectItemCaseSensitive(params, "password");
            if (ju && cJSON_IsString(ju)) username = ju->valuestring;
            if (jp && cJSON_IsString(jp)) password = jp->valuestring;
        }

        struct user_account ulist[MAX_HTTP_USERS];
        int auth_on = 0;
        int ucnt = load_user_accounts(ulist, MAX_HTTP_USERS, &auth_on);

        enum http_role found_role = ROLE_NONE;
        char matched_user[MAX_USERNAME_LEN] = {0};

        if (!auth_on) {
            found_role = ROLE_ADMIN;
            strncpy(matched_user, (username && username[0]) ? username : "admin", sizeof(matched_user) - 1);
        } else {
            for (int i = 0; i < ucnt; i++) {
                if (username && strcmp(ulist[i].username, username) == 0) {
                    if (http_auth_password_matches(&ulist[i], password)) {
                        found_role = ulist[i].role;
                        strncpy(matched_user, ulist[i].username, sizeof(matched_user) - 1);
                    }
                    break;
                }
            }
        }

        if (params) cJSON_Delete(params);

        if (found_role == ROLE_NONE) {
            http_auth_note_failure(client_ip);
            cJSON *err = cJSON_CreateObject();
            cJSON_AddBoolToObject(err, "ok", 0);
            cJSON_AddStringToObject(err, "error", "Invalid username or password");
            send_cjson_resp(client_fd, 401, err);
            cJSON_Delete(err);
            close(client_fd);
            return 1;
        }

        http_auth_note_success(client_ip);
        char token_buf[68] = {0};
        if (!create_auth_session(matched_user, found_role, client_ip, token_buf, sizeof(token_buf))) {
            send_json_error(client_fd, 500, "Cannot create a session token (no random source)");
            close(client_fd);
            return 1;
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "access_token", token_buf);
        cJSON_AddStringToObject(resp, "token_type", "Bearer");
        cJSON_AddNumberToObject(resp, "expires_in", AUTH_SESSION_TTL);
        cJSON_AddStringToObject(resp, "username", matched_user);
        cJSON_AddStringToObject(resp, "role", found_role == ROLE_USER ? "user" : "admin");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* POST /api/auth/logout */
    if (strcmp(clean_path, "/api/auth/logout") == 0 && strcmp(method, "POST") == 0) {
        const char *hdr = strcasestr(req, "Authorization:");
        if (hdr) {
            hdr += 14;
            while (*hdr == ' ' || *hdr == '\t') hdr++;
            if (strncasecmp(hdr, "Bearer ", 7) == 0) {
                hdr += 7;
                while (*hdr == ' ' || *hdr == '\t') hdr++;
                char token[128] = {0};
                int ti = 0;
                while (*hdr && *hdr != '\r' && *hdr != '\n' && *hdr != ' ' && ti < (int)sizeof(token) - 1) {
                    token[ti++] = *hdr++;
                }
                token[ti] = '\0';
                destroy_auth_session(token);
            }
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "message", "Logged out");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* POST /api/auth/tokens-reset {"confirm": true}: an unreadable token file is moved aside
     * (tokens.json.bad) and an empty list started. Admin, logged in with an account. */
    if (strcmp(clean_path, "/api/auth/tokens-reset") == 0 && strcmp(method, "POST") == 0) {
        char btok[128];
        request_bearer(req, btok, sizeof(btok));
        cJSON *params = body ? cJSON_Parse(body) : NULL;
        int confirm_ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "confirm"));
        cJSON_Delete(params);
        int code = 0;
        const char *msg = NULL;
        if (current_role != ROLE_ADMIN || strncmp(btok, "pat_", 4) == 0) {
            code = 403;
            msg = "An admin logged in with an account (not a token) can reset the token file";
        } else if (!confirm_ok) {
            code = 400;
            msg = "Send {\"confirm\": true}: every access token has to be created again";
        } else if (http_tokens_lock() != 0) {
            code = 503;
            msg = "The token list is busy, try again";
        } else {
            int rc = http_tokens_reset_unreadable();
            http_tokens_unlock();
            if (rc == 1) {
                code = 409;
                msg = "The token file is readable: delete tokens one by one instead";
            } else if (rc != 0) {
                code = 500;
                msg = "Could not move the token file aside (flash full?)";
            }
        }
        if (code) {
            send_json_error(client_fd, code, msg);
        } else {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "moved_to", "config/tokens.json.bad");
            cJSON_AddStringToObject(resp, "message", "Token file moved aside; every access token has to be created again");
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
        }
        close(client_fd);
        return 1;
    }

    /* GET /api/auth/tokens */
    if (strcmp(clean_path, "/api/auth/tokens") == 0 && strcmp(method, "GET") == 0) {
        cJSON *root = NULL;
        if (http_tokens_load(&root) != 0) {
            send_json_error(client_fd, 500, HTTP_TOKENS_UNREADABLE_MSG);
            close(client_fd);
            return 1;
        }
        /* An operator sees the tokens of its own account only (to free a slot at the limit) */
        char mine[MAX_USERNAME_LEN] = {0};
        char self_hash[65] = {0};
        if (current_role == ROLE_USER) token_caller_account(req, client_ip, method, root, mine, sizeof(mine), self_hash);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON *out_arr = cJSON_CreateArray();
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "tokens")) {
            if (current_role == ROLE_USER) {
                const cJSON *jo = cJSON_GetObjectItemCaseSensitive(it, "owner");
                const cJSON *jh = cJSON_GetObjectItemCaseSensitive(it, "token_hash");
                int of_account = mine[0] && cJSON_IsString(jo) && strcmp(jo->valuestring, mine) == 0;
                int itself = self_hash[0] && cJSON_IsString(jh) && http_auth_secret_equal(jh->valuestring, self_hash);
                if (!of_account && !itself) continue;
            }
            cJSON *item = cJSON_CreateObject();
            static const char *const str_keys[] = { "id", "name", "role", "owner" };
            for (size_t k = 0; k < sizeof(str_keys) / sizeof(str_keys[0]); k++) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(it, str_keys[k]);
                if (cJSON_IsString(v) && v->valuestring) cJSON_AddStringToObject(item, str_keys[k], v->valuestring);
            }
            cJSON *jca = cJSON_GetObjectItemCaseSensitive(it, "created_at");
            cJSON *jla = cJSON_GetObjectItemCaseSensitive(it, "last_used_at");
            if (jca && cJSON_IsNumber(jca)) cJSON_AddNumberToObject(item, "created_at", jca->valuedouble);
            if (jla && cJSON_IsNumber(jla)) cJSON_AddNumberToObject(item, "last_used_at", jla->valuedouble);
            cJSON_AddItemToArray(out_arr, item);
        }
        cJSON_Delete(root);
        cJSON_AddItemToObject(resp, "tokens", out_arr);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* POST /api/auth/tokens */
    if (strcmp(clean_path, "/api/auth/tokens") == 0 && strcmp(method, "POST") == 0) {
        /* While authentication is off every client is admin: a token made then would be a
         * standing admin key that survives turning authentication on. */
        int auth_now = 0;
        load_user_accounts(NULL, 0, &auth_now);
        if (!auth_now) {
            send_json_error(client_fd, 409, "Turn on authentication before creating access tokens");
            close(client_fd);
            return 1;
        }
        char owner[MAX_USERNAME_LEN] = {0};
        http_auth_check_request(req, client_ip, method, owner, sizeof(owner));
        cJSON *params = parse_request_params(body);
        /* Copied: the reply is built after params is freed, and the name pointed into it */
        char tname[64] = "Personal Access Token";
        const char *trole = "admin";
        if (params) {
            cJSON *jn = cJSON_GetObjectItemCaseSensitive(params, "name");
            cJSON *jr = cJSON_GetObjectItemCaseSensitive(params, "role");
            if (jn && cJSON_IsString(jn) && jn->valuestring[0]) snprintf(tname, sizeof(tname), "%s", jn->valuestring);
            if (jr && cJSON_IsString(jr) && strcasecmp(jr->valuestring, "user") == 0) trole = "user";
        }

        /* Decided by the credential, not by the reported name: a call made with a token */
        char btok[128];
        request_bearer(req, btok, sizeof(btok));
        int with_token = strncmp(btok, "pat_", 4) == 0;
        if (with_token) owner[0] = '\0'; /* "pat:<name>": the owner comes from the token entry */
        if (current_role == ROLE_USER) {
            trole = "user"; /* never more rights than the creator */
            /* An operator may create its own tokens (the app keeps one instead of the password),
             * only from an account login, so each token ends with that account. */
            if (with_token || !owner[0]) {
                if (params) cJSON_Delete(params);
                send_json_error(client_fd, 403, "Log in with an account to create a token");
                close(client_fd);
                return 1;
            }
        }
        char hex_rand[33] = {0};
        char id_rand[17] = {0};
        /* Random id: one from time(NULL) repeats for tokens created in the same second */
        if (generate_random_hex(hex_rand, 16) != 0 || generate_random_hex(id_rand, 4) != 0) {
            if (params) cJSON_Delete(params);
            send_json_error(client_fd, 500, "Cannot create a token (no random source)");
            close(client_fd);
            return 1;
        }
        char raw_token[64];
        snprintf(raw_token, sizeof(raw_token), "pat_%s", hex_rand);

        char tok_id[32];
        snprintf(tok_id, sizeof(tok_id), "tok_%s", id_rand);

        char token_hash[65];
        sha256_hex((const uint8_t *)raw_token, strlen(raw_token), token_hash);

        if (params) cJSON_Delete(params);
        if (http_tokens_lock() != 0) {
            send_json_error(client_fd, 503, "The token list is busy, try again");
            close(client_fd);
            return 1;
        }
        cJSON *root = NULL;
        if (http_tokens_load(&root) != 0) {
            http_tokens_unlock();
            send_json_error(client_fd, 500, HTTP_TOKENS_UNREADABLE_MSG);
            close(client_fd);
            return 1;
        }
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "tokens");

        /* A token made with a token belongs to the same account, and ends with it */
        if (with_token) {
            char call_hash[65];
            sha256_hex((const uint8_t *)btok, strlen(btok), call_hash);
            int found = 0;
            const cJSON *it;
            cJSON_ArrayForEach(it, arr) {
                const cJSON *jh = cJSON_GetObjectItemCaseSensitive(it, "token_hash");
                if (cJSON_IsString(jh) && http_auth_secret_equal(jh->valuestring, call_hash)) {
                    const cJSON *jo = cJSON_GetObjectItemCaseSensitive(it, "owner");
                    if (cJSON_IsString(jo)) snprintf(owner, sizeof(owner), "%s", jo->valuestring);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                /* deleted since the request was let in: no token without its account */
                http_tokens_unlock();
                cJSON_Delete(root);
                send_json_error(client_fd, 401, "The calling access token no longer exists");
                close(client_fd);
                return 1;
            }
        }

        int total = 0, of_owner = 0;
        const cJSON *cit;
        cJSON_ArrayForEach(cit, arr) {
            total++;
            const cJSON *jo = cJSON_GetObjectItemCaseSensitive(cit, "owner");
            if (owner[0] && cJSON_IsString(jo) && strcmp(jo->valuestring, owner) == 0) of_owner++;
        }
        const char *limit_msg = NULL;
        char limit_buf[128];
        if (total >= HTTP_TOKENS_MAX) {
            snprintf(limit_buf, sizeof(limit_buf), "The Hub already has %d access tokens: delete unused ones first",
                     HTTP_TOKENS_MAX);
            limit_msg = limit_buf;
        } else if (current_role == ROLE_USER && of_owner >= HTTP_TOKENS_PER_OPERATOR) {
            snprintf(limit_buf, sizeof(limit_buf),
                     "This account already has %d access tokens: delete one first (GET /api/auth/tokens lists them)",
                     HTTP_TOKENS_PER_OPERATOR);
            limit_msg = limit_buf;
        }
        if (limit_msg) {
            http_tokens_unlock();
            cJSON_Delete(root);
            send_json_error(client_fd, 409, limit_msg);
            close(client_fd);
            return 1;
        }
        int retry_after = 0;
        if (current_role == ROLE_USER && http_tokens_rate_check(owner, &retry_after) != 0) {
            http_tokens_unlock();
            cJSON_Delete(root);
            snprintf(limit_buf, sizeof(limit_buf), "Too many token changes for this account, try again in %d s", retry_after);
            send_json_error(client_fd, 429, limit_buf);
            close(client_fd);
            return 1;
        }

        cJSON *new_tok = cJSON_CreateObject();
        cJSON_AddStringToObject(new_tok, "id", tok_id);
        cJSON_AddStringToObject(new_tok, "name", tname);
        cJSON_AddStringToObject(new_tok, "token_hash", token_hash);
        cJSON_AddStringToObject(new_tok, "role", trole);
        /* the token ends with this account (loopback callers create unowned tokens) */
        if (owner[0]) cJSON_AddStringToObject(new_tok, "owner", owner);
        cJSON_AddNumberToObject(new_tok, "created_at", (double)time(NULL));
        cJSON_AddNumberToObject(new_tok, "last_used_at", 0);
        cJSON_AddItemToArray(arr, new_tok);

        int saved = http_tokens_save(root);
        http_tokens_unlock();
        cJSON_Delete(root);
        if (saved != 0) {
            send_json_error(client_fd, 500, "Could not save the token (flash full?)");
            close(client_fd);
            return 1;
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "id", tok_id);
        cJSON_AddStringToObject(resp, "name", tname);
        cJSON_AddStringToObject(resp, "role", trole);
        cJSON_AddStringToObject(resp, "token", raw_token);
        send_cjson_resp(client_fd, 201, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* DELETE /api/auth/tokens */
    if (strcmp(clean_path, "/api/auth/tokens") == 0 && strcmp(method, "DELETE") == 0) {
        cJSON *params = parse_request_params(body);
        char del_id[64] = {0};
        if (params) {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(params, "id");
            if (jid && cJSON_IsString(jid)) snprintf(del_id, sizeof(del_id), "%s", jid->valuestring);
        }
        if (!del_id[0]) http_query_param(query, "id", del_id, sizeof(del_id));

        if (!del_id[0]) {
            if (params) cJSON_Delete(params);
            send_json_error(client_fd, 400, "Token id is required");
            close(client_fd);
            return 1;
        }

        /* Admins delete any token. An operator deletes only a token it created or the token it
         * calls with (the app's log out); anything else answers as not found. The account is
         * the logged-in one, or the owner of the token the operator calls with. */
        char caller[MAX_USERNAME_LEN] = {0};
        char own_hash[65] = {0};

        int deleted = 0, failed = 0, retry_after = 0;
        cJSON *root = NULL;
        if (http_tokens_lock() != 0) {
            failed = 503;
        } else {
            if (http_tokens_load(&root) != 0) {
                failed = 500;
            } else if (current_role == ROLE_USER) {
                token_caller_account(req, client_ip, method, root, caller, sizeof(caller), own_hash);
                if (caller[0] && http_tokens_rate_check(caller, &retry_after) != 0) failed = 429;
            }
            if (!failed) {
                cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "tokens");
                int sz = cJSON_GetArraySize(arr);
                for (int i = 0; i < sz; i++) {
                    cJSON *it = cJSON_GetArrayItem(arr, i);
                    cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
                    if (jid && cJSON_IsString(jid) && strcmp(jid->valuestring, del_id) == 0) {
                        if (current_role == ROLE_USER) {
                            cJSON *jo = cJSON_GetObjectItemCaseSensitive(it, "owner");
                            cJSON *jh = cJSON_GetObjectItemCaseSensitive(it, "token_hash");
                            int mine = (caller[0] && cJSON_IsString(jo) && strcmp(jo->valuestring, caller) == 0) ||
                                       (own_hash[0] && cJSON_IsString(jh) &&
                                        http_auth_secret_equal(jh->valuestring, own_hash));
                            if (!mine) break;
                        }
                        cJSON_DeleteItemFromArray(arr, i);
                        deleted = 1;
                        break;
                    }
                }
                if (deleted && http_tokens_save(root) != 0) failed = 507;
            }
            http_tokens_unlock();
        }
        cJSON_Delete(root);

        if (failed) {
            if (params) cJSON_Delete(params);
            char msg[96];
            snprintf(msg, sizeof(msg), "Too many token changes for this account, try again in %d s", retry_after);
            send_json_error(client_fd, failed == 507 ? 500 : failed,
                            failed == 503 ? "The token list is busy, try again" :
                            failed == 429 ? msg :
                            failed == 507 ? "Could not save the token list (flash full?)" : HTTP_TOKENS_UNREADABLE_MSG);
            close(client_fd);
            return 1;
        }

        if (params) cJSON_Delete(params);
        if (!deleted) {
            send_json_error(client_fd, 404, "Token not found");
            close(client_fd);
            return 1;
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "message", "Token deleted");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* GET /api/users */
    if (strcmp(clean_path, "/api/users") == 0 && strcmp(method, "GET") == 0) {
        struct user_account ulist[MAX_HTTP_USERS];
        int auth_on = 0;
        int ucnt = load_user_accounts(ulist, MAX_HTTP_USERS, &auth_on);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddBoolToObject(resp, "auth_enabled", auth_on);
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < ucnt; i++) {
            cJSON *u = cJSON_CreateObject();
            cJSON_AddStringToObject(u, "username", ulist[i].username);
            cJSON_AddStringToObject(u, "role", ulist[i].role == ROLE_USER ? "user" : "admin");
            cJSON_AddItemToArray(arr, u);
        }
        cJSON_AddItemToObject(resp, "users", arr);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* POST /api/users */
    if (strcmp(clean_path, "/api/users") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        if (!params) {
            send_json_error(client_fd, 400, "Invalid JSON payload");
            close(client_fd);
            return 1;
        }
        cJSON *ju = cJSON_GetObjectItemCaseSensitive(params, "username");
        cJSON *jp = cJSON_GetObjectItemCaseSensitive(params, "password");
        cJSON *jr = cJSON_GetObjectItemCaseSensitive(params, "role");
        const char *new_user = (ju && cJSON_IsString(ju)) ? ju->valuestring : "";
        const char *new_pass = (jp && cJSON_IsString(jp)) ? jp->valuestring : "";
        const char *new_role_str = (jr && cJSON_IsString(jr)) ? jr->valuestring : "user";

        if (!new_user[0]) {
            cJSON_Delete(params);
            send_json_error(client_fd, 400, "Username cannot be empty");
            close(client_fd);
            return 1;
        }
        if (strlen(new_user) >= MAX_USERNAME_LEN || strchr(new_user, ':') || strchr(new_user, '\n')) {
            cJSON_Delete(params);
            send_json_error(client_fd, 400, "Username must be shorter than 32 bytes, without ':' or line breaks");
            close(client_fd);
            return 1;
        }
        if (strlen(new_pass) > MAX_PASSWORD_BYTES) {
            cJSON_Delete(params);
            send_json_error(client_fd, 400, "Password longer than 128 bytes");
            close(client_fd);
            return 1;
        }

        struct user_account ulist[MAX_HTTP_USERS];
        int auth_on = 0;
        int ucnt = load_user_accounts(ulist, MAX_HTTP_USERS, &auth_on);
        int auth_before = auth_on;

        int found_idx = -1;
        for (int i = 0; i < ucnt; i++) {
            if (strcmp(ulist[i].username, new_user) == 0) {
                found_idx = i;
                break;
            }
        }

        int ends_sessions = 0;
        if (found_idx >= 0) {
            if (new_pass[0]) {
                ulist[found_idx].password[0] = '\0';
                compute_ha1(ulist[found_idx].username, HARMONY_AUTH_REALM, new_pass, ulist[found_idx].ha1);
                ends_sessions = 1;
            }
            if (jr && cJSON_IsString(jr)) {
                enum http_role nr = (strcasecmp(new_role_str, "admin") == 0) ? ROLE_ADMIN : ROLE_USER;
                if (nr != ulist[found_idx].role) ends_sessions = 1;
                ulist[found_idx].role = nr;
            }
        } else {
            if (ucnt >= MAX_HTTP_USERS) {
                cJSON_Delete(params);
                send_json_error(client_fd, 400, "Maximum user count reached");
                close(client_fd);
                return 1;
            }
            if (!new_pass[0]) {
                cJSON_Delete(params);
                send_json_error(client_fd, 400, "Password is required for new user");
                close(client_fd);
                return 1;
            }
            /* Tokens left by an earlier account of this name (its delete could not change the
             * token file) must not come back with the new account */
            int stale = remove_tokens_of(new_user);
            if (stale < 0) {
                cJSON_Delete(params);
                if (stale == HTTP_TOKENS_BUSY) send_json_error(client_fd, 503, "The token list is busy, try again");
                else send_json_error(client_fd, 500, "Old access tokens of this name could not be removed: " HTTP_TOKENS_UNREADABLE_MSG);
                close(client_fd);
                return 1;
            }
            memset(&ulist[ucnt], 0, sizeof(ulist[ucnt]));
            strncpy(ulist[ucnt].username, new_user, sizeof(ulist[ucnt].username) - 1);
            compute_ha1(ulist[ucnt].username, HARMONY_AUTH_REALM, new_pass, ulist[ucnt].ha1);
            ulist[ucnt].role = (strcasecmp(new_role_str, "admin") == 0) ? ROLE_ADMIN : ROLE_USER;
            ucnt++;
        }

        cJSON *j_en = cJSON_GetObjectItemCaseSensitive(params, "auth_enabled");
        if (j_en) {
            if (cJSON_IsBool(j_en)) auth_on = cJSON_IsTrue(j_en) ? 1 : 0;
            else if (cJSON_IsNumber(j_en)) auth_on = j_en->valueint != 0;
        }

        /* Never invent an account with a known password: refuse a change that leaves no admin */
        int admin_count = 0;
        for (int i = 0; i < ucnt; i++) {
            if (ulist[i].role == ROLE_ADMIN) admin_count++;
        }
        if (admin_count == 0) {
            cJSON_Delete(params);
            send_json_error(client_fd, 400, found_idx >= 0 ? "Cannot demote the last admin account"
                                                           : "Create an admin account first");
            close(client_fd);
            return 1;
        }

        int was_on = 0;
        load_user_accounts(NULL, 0, &was_on);
        if (!save_user_accounts(ulist, ucnt, auth_on)) {
            cJSON_Delete(params);
            send_json_error(client_fd, 500, "Could not save the user accounts (flash full?)");
            close(client_fd);
            return 1;
        }
        if (ends_sessions) http_auth_drop_user_sessions(new_user);
        if (was_on != auth_on) http_auth_drop_all_sessions();
        cJSON_Delete(params);

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "message", "User account saved");
        if (!auth_before && auth_on) {
            /* turned on: the accounts and tokens to check, but the one named here */
            cJSON_AddBoolToObject(resp, "auth_enabled", 1);
            /* new_user pointed into the request, freed by now: the account list has the name */
            http_auth_add_switch_check(resp, ulist, ucnt, ulist[found_idx >= 0 ? found_idx : ucnt - 1].username);
        }
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* DELETE /api/users or POST /api/users/delete */
    if (((strcmp(clean_path, "/api/users") == 0 && strcmp(method, "DELETE") == 0) ||
         (strcmp(clean_path, "/api/users/delete") == 0 && strcmp(method, "POST") == 0))) {
        cJSON *params = parse_request_params(body);
        char del_user[MAX_USERNAME_LEN + 8] = {0};
        if (params) {
            cJSON *ju = cJSON_GetObjectItemCaseSensitive(params, "username");
            if (ju && cJSON_IsString(ju)) snprintf(del_user, sizeof(del_user), "%s", ju->valuestring);
        }
        if (!del_user[0]) http_query_param(query, "username", del_user, sizeof(del_user));

        if (!del_user[0]) {
            if (params) cJSON_Delete(params);
            send_json_error(client_fd, 400, "Username is required");
            close(client_fd);
            return 1;
        }

        struct user_account ulist[MAX_HTTP_USERS];
        int auth_on = 0;
        int ucnt = load_user_accounts(ulist, MAX_HTTP_USERS, &auth_on);

        int target_idx = -1;
        int admin_count = 0;
        for (int i = 0; i < ucnt; i++) {
            if (ulist[i].role == ROLE_ADMIN) admin_count++;
            if (strcmp(ulist[i].username, del_user) == 0) {
                target_idx = i;
            }
        }

        if (target_idx < 0) {
            if (params) cJSON_Delete(params);
            send_json_error(client_fd, 404, "User not found");
            close(client_fd);
            return 1;
        }

        if (ulist[target_idx].role == ROLE_ADMIN && admin_count <= 1) {
            if (params) cJSON_Delete(params);
            send_json_error(client_fd, 400, "Cannot delete the sole admin account");
            close(client_fd);
            return 1;
        }

        for (int i = target_idx; i < ucnt - 1; i++) {
            ulist[i] = ulist[i + 1];
        }
        ucnt--;
        if (!save_user_accounts(ulist, ucnt, ucnt > 0 ? auth_on : 0)) {
            if (params) cJSON_Delete(params);
            send_json_error(client_fd, 500, "Could not save the user accounts (flash full?)");
            close(client_fd);
            return 1;
        }
        http_auth_drop_user_sessions(del_user);
        int tokens_gone = remove_tokens_of(del_user);

        if (params) cJSON_Delete(params);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "message", "User deleted");
        if (tokens_gone < 0) {
            /* They no longer work (no account behind them), but they stay in the file */
            fprintf(stderr, "[!] Auth: tokens of a deleted account not removed (token file unreadable or not written)\n");
            cJSON_AddStringToObject(resp, "warning", "The account's access tokens could not be removed from the token file");
        }
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    return 0;
}
