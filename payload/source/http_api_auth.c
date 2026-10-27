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
        cJSON_AddStringToObject(resp, "username", current_user[0] ? current_user : "admin");
        cJSON_AddStringToObject(resp, "role", current_role == ROLE_USER ? "user" : "admin");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* POST /api/auth/token */
    if (strcmp(clean_path, "/api/auth/token") == 0 && strcmp(method, "POST") == 0) {
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

        if (!auth_on || ucnt == 0) {
            found_role = ROLE_ADMIN;
            strncpy(matched_user, (username && username[0]) ? username : "admin", sizeof(matched_user) - 1);
        } else {
            for (int i = 0; i < ucnt; i++) {
                if (username && strcmp(ulist[i].username, username) == 0) {
                    int ok = 0;
                    if (password && ulist[i].password[0] && strcmp(ulist[i].password, password) == 0) {
                        ok = 1;
                    } else if (password && ulist[i].ha1[0]) {
                        char ha1_calc[65];
                        compute_ha1(username, HARMONY_AUTH_REALM, password, ha1_calc);
                        if (strcmp(ulist[i].ha1, ha1_calc) == 0) {
                            ok = 1;
                        }
                    }
                    if (ok) {
                        found_role = ulist[i].role;
                        strncpy(matched_user, ulist[i].username, sizeof(matched_user) - 1);
                        break;
                    }
                }
            }
        }

        if (params) cJSON_Delete(params);

        if (found_role == ROLE_NONE) {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddBoolToObject(err, "ok", 0);
            cJSON_AddStringToObject(err, "error", "Invalid username or password");
            send_cjson_resp(client_fd, 401, err);
            cJSON_Delete(err);
            close(client_fd);
            return 1;
        }

        char token_buf[68] = {0};
        create_auth_session(matched_user, found_role, client_ip, token_buf, sizeof(token_buf));

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

    /* GET /api/auth/tokens */
    if (strcmp(clean_path, "/api/auth/tokens") == 0 && strcmp(method, "GET") == 0) {
        const char *path = HARMONY_TOKENS_JSON;
        size_t flen = 0;
        char *json_txt = read_file_text_alloc(path, 65536, &flen);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON *out_arr = cJSON_CreateArray();

        if (json_txt) {
            cJSON *root = cJSON_Parse(json_txt);
            free(json_txt);
            if (root) {
                cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "tokens");
                if (arr && cJSON_IsArray(arr)) {
                    const cJSON *it = NULL;
                    cJSON_ArrayForEach(it, arr) {
                        cJSON *item = cJSON_CreateObject();
                        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
                        cJSON *jname = cJSON_GetObjectItemCaseSensitive(it, "name");
                        cJSON *jrole = cJSON_GetObjectItemCaseSensitive(it, "role");
                        cJSON *jca = cJSON_GetObjectItemCaseSensitive(it, "created_at");
                        cJSON *jla = cJSON_GetObjectItemCaseSensitive(it, "last_used_at");
                        if (jid && cJSON_IsString(jid)) cJSON_AddStringToObject(item, "id", jid->valuestring);
                        if (jname && cJSON_IsString(jname)) cJSON_AddStringToObject(item, "name", jname->valuestring);
                        if (jrole && cJSON_IsString(jrole)) cJSON_AddStringToObject(item, "role", jrole->valuestring);
                        if (jca && cJSON_IsNumber(jca)) cJSON_AddNumberToObject(item, "created_at", jca->valuedouble);
                        if (jla && cJSON_IsNumber(jla)) cJSON_AddNumberToObject(item, "last_used_at", jla->valuedouble);
                        cJSON_AddItemToArray(out_arr, item);
                    }
                }
                cJSON_Delete(root);
            }
        }
        cJSON_AddItemToObject(resp, "tokens", out_arr);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* POST /api/auth/tokens */
    if (strcmp(clean_path, "/api/auth/tokens") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        const char *tname = "Personal Access Token";
        const char *trole = "admin";
        if (params) {
            cJSON *jn = cJSON_GetObjectItemCaseSensitive(params, "name");
            cJSON *jr = cJSON_GetObjectItemCaseSensitive(params, "role");
            if (jn && cJSON_IsString(jn) && jn->valuestring[0]) tname = jn->valuestring;
            if (jr && cJSON_IsString(jr) && strcasecmp(jr->valuestring, "user") == 0) trole = "user";
        }

        char hex_rand[33] = {0};
        generate_random_hex(hex_rand, 16);
        char raw_token[64];
        snprintf(raw_token, sizeof(raw_token), "pat_%s", hex_rand);

        char tok_id[32];
        snprintf(tok_id, sizeof(tok_id), "tok_%08x", (unsigned int)time(NULL));

        char token_hash[65];
        sha256_hex((const uint8_t *)raw_token, strlen(raw_token), token_hash);

        const char *path = HARMONY_TOKENS_JSON;
        size_t flen = 0;
        char *json_txt = read_file_text_alloc(path, 65536, &flen);
        cJSON *root = json_txt ? cJSON_Parse(json_txt) : NULL;
        if (json_txt) free(json_txt);
        if (!root) root = cJSON_CreateObject();

        cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "tokens");
        if (!arr || !cJSON_IsArray(arr)) {
            arr = cJSON_CreateArray();
            cJSON_AddItemToObject(root, "tokens", arr);
        }

        cJSON *new_tok = cJSON_CreateObject();
        cJSON_AddStringToObject(new_tok, "id", tok_id);
        cJSON_AddStringToObject(new_tok, "name", tname);
        cJSON_AddStringToObject(new_tok, "token_hash", token_hash);
        cJSON_AddStringToObject(new_tok, "role", trole);
        cJSON_AddNumberToObject(new_tok, "created_at", (double)time(NULL));
        cJSON_AddNumberToObject(new_tok, "last_used_at", 0);
        cJSON_AddItemToArray(arr, new_tok);

        char *json_out = cJSON_Print(root);
        cJSON_Delete(root);
        if (json_out) {
            mkdir("/data/harmony", 0755);
            mkdir("/data/harmony/config", 0755);
            write_file_atomic_sized(HARMONY_TOKENS_JSON, json_out, strlen(json_out));
            free(json_out);
        }

        if (params) cJSON_Delete(params);

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

    /* DELETE /api/auth/tokens or POST /api/auth/tokens/delete */
    if (((strcmp(clean_path, "/api/auth/tokens") == 0 && strcmp(method, "DELETE") == 0) ||
         (strcmp(clean_path, "/api/auth/tokens/delete") == 0 && strcmp(method, "POST") == 0))) {
        cJSON *params = parse_request_params(body);
        const char *del_id = "";
        if (params) {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(params, "id");
            if (jid && cJSON_IsString(jid)) del_id = jid->valuestring;
        }
        if (!del_id[0] && query) {
            const char *qid = strstr(query, "id=");
            if (qid) del_id = qid + 3;
        }

        if (!del_id || !del_id[0]) {
            if (params) cJSON_Delete(params);
            send_json_error(client_fd, 400, "Token id is required");
            close(client_fd);
            return 1;
        }

        const char *path = HARMONY_TOKENS_JSON;
        size_t flen = 0;
        char *json_txt = read_file_text_alloc(path, 65536, &flen);
        if (json_txt) {
            cJSON *root = cJSON_Parse(json_txt);
            free(json_txt);
            if (root) {
                cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "tokens");
                if (arr && cJSON_IsArray(arr)) {
                    int sz = cJSON_GetArraySize(arr);
                    for (int i = 0; i < sz; i++) {
                        cJSON *it = cJSON_GetArrayItem(arr, i);
                        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
                        if (jid && cJSON_IsString(jid) && strcmp(jid->valuestring, del_id) == 0) {
                            cJSON_DeleteItemFromArray(arr, i);
                            break;
                        }
                    }
                    char *json_out = cJSON_Print(root);
                    if (json_out) {
                        write_file_atomic_sized(HARMONY_TOKENS_JSON, json_out, strlen(json_out));
                        free(json_out);
                    }
                }
                cJSON_Delete(root);
            }
        }

        if (params) cJSON_Delete(params);
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

        struct user_account ulist[MAX_HTTP_USERS];
        int auth_on = 0;
        int ucnt = load_user_accounts(ulist, MAX_HTTP_USERS, &auth_on);

        int found_idx = -1;
        for (int i = 0; i < ucnt; i++) {
            if (strcmp(ulist[i].username, new_user) == 0) {
                found_idx = i;
                break;
            }
        }

        if (found_idx >= 0) {
            if (new_pass[0]) {
                strncpy(ulist[found_idx].password, new_pass, sizeof(ulist[found_idx].password) - 1);
                compute_ha1(ulist[found_idx].username, HARMONY_AUTH_REALM, new_pass, ulist[found_idx].ha1);
            }
            if (jr && cJSON_IsString(jr)) {
                ulist[found_idx].role = (strcasecmp(new_role_str, "admin") == 0) ? ROLE_ADMIN : ROLE_USER;
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
            strncpy(ulist[ucnt].username, new_user, sizeof(ulist[ucnt].username) - 1);
            strncpy(ulist[ucnt].password, new_pass, sizeof(ulist[ucnt].password) - 1);
            compute_ha1(new_user, HARMONY_AUTH_REALM, new_pass, ulist[ucnt].ha1);
            ulist[ucnt].role = (strcasecmp(new_role_str, "admin") == 0) ? ROLE_ADMIN : ROLE_USER;
            ucnt++;
        }

        int admin_count = 0;
        for (int i = 0; i < ucnt; i++) {
            if (ulist[i].role == ROLE_ADMIN) admin_count++;
        }
        if (admin_count == 0) {
            ulist[0].role = ROLE_ADMIN;
        }

        save_user_accounts(ulist, ucnt, auth_on ? 1 : (ucnt > 0 ? 1 : 0));
        cJSON_Delete(params);

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "message", "User account saved");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* DELETE /api/users or POST /api/users/delete */
    if (((strcmp(clean_path, "/api/users") == 0 && strcmp(method, "DELETE") == 0) ||
         (strcmp(clean_path, "/api/users/delete") == 0 && strcmp(method, "POST") == 0))) {
        cJSON *params = parse_request_params(body);
        const char *del_user = "";
        if (params) {
            cJSON *ju = cJSON_GetObjectItemCaseSensitive(params, "username");
            if (ju && cJSON_IsString(ju)) del_user = ju->valuestring;
        }
        if (!del_user[0] && query) {
            const char *qu = strstr(query, "username=");
            if (qu) del_user = qu + 9;
        }

        if (!del_user || !del_user[0]) {
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
        save_user_accounts(ulist, ucnt, ucnt > 0 ? auth_on : 0);

        if (params) cJSON_Delete(params);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "message", "User deleted");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    return 0;
}
