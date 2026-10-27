#define _GNU_SOURCE
#include "http_api_custom_icons.h"
#include "http_server.h"
#include "http_util.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int b64_char_to_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static size_t b64_decode_buffer(const char *src, size_t src_len, uint8_t *dst, size_t dst_max) {
    if (!src || !dst || dst_max == 0) return 0;
    const char *comma = strchr(src, ',');
    if (comma && comma - src < 64) {
        src_len -= (size_t)(comma + 1 - src);
        src = comma + 1;
    }
    size_t out_len = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < src_len && src[i] != '\0' && src[i] != '='; i++) {
        int v = b64_char_to_val(src[i]);
        if (v < 0) continue;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len < dst_max) {
                dst[out_len++] = (uint8_t)((buf >> bits) & 0xFF);
            }
        }
    }
    return out_len;
}

static volatile int g_icons_extracting = 0;

static void *extract_icons_thread(void *arg) {
    const char *tar_path = (const char *)arg;
    mkdir("/tmp/www", 0755);
    mkdir("/tmp/www/assets", 0755);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
        "tar -xzf \"%s\" -C /tmp/www/assets/ 2>/dev/null && "
        "for d in /tmp/www/assets/icons/*/; do "
        "  if [ -d \"$d\" ]; then "
        "    for f in \"$d\"*.png; do [ -f \"$f\" ] && ln -sf \"$f\" /tmp/www/assets/icons/ 2>/dev/null; done; "
        "  fi; "
        "done",
        tar_path);
    system(cmd);
    g_icons_extracting = 0;
    return NULL;
}

void http_ensure_icons_extracted(void) {
    if (access("/tmp/www/assets/icons", F_OK) == 0) return;
    if (g_icons_extracting) return;

    static char s_tar_path[128];
    s_tar_path[0] = '\0';
    if (access("/data/harmony/icons.tar.gz", F_OK) == 0) {
        strncpy(s_tar_path, "/data/harmony/icons.tar.gz", sizeof(s_tar_path) - 1);
    } else if (access("/cache/icons.tar.gz", F_OK) == 0) {
        strncpy(s_tar_path, "/cache/icons.tar.gz", sizeof(s_tar_path) - 1);
    } else if (access("/cache/www/assets/icons.tar.gz", F_OK) == 0) {
        strncpy(s_tar_path, "/cache/www/assets/icons.tar.gz", sizeof(s_tar_path) - 1);
    }

    if (s_tar_path[0]) {
        g_icons_extracting = 1;
        pthread_t th;
        if (pthread_create(&th, NULL, extract_icons_thread, (void *)s_tar_path) == 0) {
            pthread_detach(th);
        } else {
            g_icons_extracting = 0;
        }
    }
}

int http_api_custom_icons_handle(int client_fd, const char *method, const char *clean_path,
                                 const char *query, const char *body, const char *req) {
    (void)req;

    /* Route: GET /api/icons */
    if (strcmp(clean_path, "/api/icons") == 0 && strcmp(method, "GET") == 0) {
        http_ensure_icons_extracted();
        cJSON *arr = cJSON_CreateArray();
        const char *base_dir = "/tmp/www/assets/icons";
        if (access(base_dir, F_OK) != 0) {
            base_dir = "/cache/www/assets/icons";
            if (access(base_dir, F_OK) != 0) {
                base_dir = "/data/harmony/www/assets/icons";
            }
        }
        DIR *d = opendir(base_dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                size_t nlen = strlen(de->d_name);
                if (nlen > 4 && strcmp(de->d_name + nlen - 4, ".png") == 0) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(de->d_name));
                }
            }
            closedir(d);
        }
        const char *cust_dir = "/data/harmony/custom_icons";
        DIR *cd = opendir(cust_dir);
        if (cd) {
            struct dirent *de;
            while ((de = readdir(cd)) != NULL) {
                if (de->d_name[0] == '.') continue;
                size_t nlen = strlen(de->d_name);
                if (nlen > 4 && strcmp(de->d_name + nlen - 4, ".png") == 0) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(de->d_name));
                }
            }
            closedir(cd);
        }
        send_cjson_resp(client_fd, 200, arr);
        cJSON_Delete(arr);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/custom-icons */
    if (strcmp(clean_path, "/api/custom-icons") == 0 && strcmp(method, "GET") == 0) {
        cJSON *arr = cJSON_CreateArray();
        const char *cdir = "/data/harmony/custom_icons";
        DIR *d = opendir(cdir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                size_t nlen = strlen(de->d_name);
                if (nlen > 4 && strcmp(de->d_name + nlen - 4, ".png") == 0) {
                    char full_path[300];
                    snprintf(full_path, sizeof(full_path), "%s/%s", cdir, de->d_name);
                    struct stat st;
                    long fsz = 0;
                    if (stat(full_path, &st) == 0) fsz = (long)st.st_size;

                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "filename", de->d_name);
                    char url[128];
                    snprintf(url, sizeof(url), "/assets/icons/%s", de->d_name);
                    cJSON_AddStringToObject(item, "url", url);
                    cJSON_AddNumberToObject(item, "size", fsz);

                    if (strncmp(de->d_name, "activity_", 9) == 0) {
                        cJSON_AddStringToObject(item, "type", "activity");
                        char id_buf[64] = {0};
                        strncpy(id_buf, de->d_name + 9, sizeof(id_buf) - 1);
                        char *dot = strrchr(id_buf, '.');
                        if (dot) *dot = '\0';
                        cJSON_AddStringToObject(item, "id", id_buf);
                    } else if (strncmp(de->d_name, "device_", 7) == 0) {
                        cJSON_AddStringToObject(item, "type", "device");
                        char id_buf[64] = {0};
                        strncpy(id_buf, de->d_name + 7, sizeof(id_buf) - 1);
                        char *dot = strrchr(id_buf, '.');
                        if (dot) *dot = '\0';
                        cJSON_AddStringToObject(item, "id", id_buf);
                    } else {
                        char id_buf[64] = {0};
                        strncpy(id_buf, de->d_name, sizeof(id_buf) - 1);
                        char *dot = strrchr(id_buf, '.');
                        if (dot) *dot = '\0';
                        cJSON_AddStringToObject(item, "id", id_buf);
                    }
                    cJSON_AddItemToArray(arr, item);
                }
            }
            closedir(d);
        }
        send_cjson_resp(client_fd, 200, arr);
        cJSON_Delete(arr);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/custom-icons */
    if (strcmp(clean_path, "/api/custom-icons") == 0 && strcmp(method, "POST") == 0) {
        cJSON *root = body ? cJSON_Parse(body) : NULL;
        if (!root) {
            send_json_error(client_fd, 400, "Invalid JSON body");
            close(client_fd);
            return 1;
        }

        cJSON *j_type = cJSON_GetObjectItem(root, "type");
        cJSON *j_id = cJSON_GetObjectItem(root, "id");
        cJSON *j_name = cJSON_GetObjectItem(root, "name");
        cJSON *j_b64 = cJSON_GetObjectItem(root, "image_base64");
        if (!j_b64) j_b64 = cJSON_GetObjectItem(root, "data");

        char type_str[32] = "activity";
        char id_str[64] = {0};
        char name_str[64] = {0};

        if (j_type && j_type->valuestring && j_type->valuestring[0]) {
            snprintf(type_str, sizeof(type_str), "%s", j_type->valuestring);
        }
        if (j_id && j_id->valuestring && j_id->valuestring[0]) {
            snprintf(id_str, sizeof(id_str), "%s", j_id->valuestring);
        } else if (j_id && cJSON_IsNumber(j_id)) {
            snprintf(id_str, sizeof(id_str), "%d", j_id->valueint);
        }
        if (j_name && j_name->valuestring && j_name->valuestring[0]) {
            snprintf(name_str, sizeof(name_str), "%s", j_name->valuestring);
        }

        if ((!id_str[0] && !name_str[0]) || !j_b64 || !j_b64->valuestring) {
            cJSON_Delete(root);
            send_json_error(client_fd, 400, "Missing required fields: id/name and image_base64");
            close(client_fd);
            return 1;
        }

        for (char *p = type_str; *p; p++) {
            if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') *p = '_';
        }
        for (char *p = id_str; *p; p++) {
            if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') *p = '_';
        }
        for (char *p = name_str; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
            if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') *p = '_';
        }
        if (!id_str[0] && name_str[0]) {
            snprintf(id_str, sizeof(id_str), "%s", name_str);
        }

        uint8_t png_data[16384];
        size_t png_len = b64_decode_buffer(j_b64->valuestring, strlen(j_b64->valuestring), png_data, sizeof(png_data));
        cJSON_Delete(root);

        if (png_len < 8) {
            send_json_error(client_fd, 400, "Decoded image data too small");
            close(client_fd);
            return 1;
        }

        const uint8_t png_sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
        if (memcmp(png_data, png_sig, 8) != 0) {
            send_json_error(client_fd, 400, "Uploaded data is not a valid PNG image");
            close(client_fd);
            return 1;
        }

        mkdir("/data/harmony", 0755);
        mkdir("/data/harmony/custom_icons", 0755);
        mkdir("/tmp/www/assets/icons", 0755);
        mkdir("/cache/www/assets/icons", 0755);

        char main_name[64];
        if (name_str[0]) {
            if (strncmp(name_str, "custom_", 7) == 0) {
                snprintf(main_name, sizeof(main_name), "%s.png", name_str);
            } else {
                snprintf(main_name, sizeof(main_name), "custom_%s.png", name_str);
            }
            snprintf(type_str, sizeof(type_str), "custom");
        } else {
            snprintf(main_name, sizeof(main_name), "%s_%s.png", type_str, id_str);
        }

        char p_data[256], p_cache[256], p_tmp[256];
        snprintf(p_data, sizeof(p_data), "/data/harmony/custom_icons/%s", main_name);
        snprintf(p_cache, sizeof(p_cache), "/cache/www/assets/icons/%s", main_name);
        snprintf(p_tmp, sizeof(p_tmp), "/tmp/www/assets/icons/%s", main_name);

        FILE *f = fopen(p_data, "wb");
        if (!f) {
            send_json_error(client_fd, 500, "Failed to write custom icon to flash storage");
            close(client_fd);
            return 1;
        }
        fwrite(png_data, 1, png_len, f);
        fclose(f);
        FILE *fc = fopen(p_cache, "wb");
        if (fc) { fwrite(png_data, 1, png_len, fc); fclose(fc); }
        FILE *ft = fopen(p_tmp, "wb");
        if (ft) { fwrite(png_data, 1, png_len, ft); fclose(ft); }

        static_cache_flush();

        /* Notify RF daemon on local port 8092 to push icon over RF */
        int rfsock = socket(AF_INET, SOCK_STREAM, 0);
        if (rfsock >= 0) {
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(8092);
            sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
            setsockopt(rfsock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(rfsock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (connect(rfsock, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
                char req_buf[256];
                int qlen = snprintf(req_buf, sizeof(req_buf),
                    "POST /api/rf/push-icon?id=%s&type=%s HTTP/1.1\r\nHost: 127.0.0.1:8092\r\nConnection: close\r\n\r\n",
                    id_str, type_str);
                if (qlen > 0) write(rfsock, req_buf, (size_t)qlen);
            }
            close(rfsock);
        }

        cJSON *res = cJSON_CreateObject();
        cJSON_AddBoolToObject(res, "ok", 1);
        cJSON_AddStringToObject(res, "filename", main_name);
        char url[128];
        snprintf(url, sizeof(url), "/assets/icons/%s", main_name);
        cJSON_AddStringToObject(res, "url", url);
        cJSON_AddNumberToObject(res, "size", (double)png_len);
        send_cjson_resp(client_fd, 200, res);
        cJSON_Delete(res);
        close(client_fd);
        return 1;
    }

    /* Route: DELETE /api/custom-icons */
    if (strcmp(clean_path, "/api/custom-icons") == 0 && strcmp(method, "DELETE") == 0) {
        char fn[64] = {0};
        if (query) {
            const char *qfn = strstr(query, "filename=");
            if (qfn) {
                strncpy(fn, qfn + 9, sizeof(fn) - 1);
                char *amp = strchr(fn, '&');
                if (amp) *amp = '\0';
            } else {
                const char *qid = strstr(query, "id=");
                const char *qtype = strstr(query, "type=");
                char id_val[32] = {0}, type_val[32] = "activity";
                if (qid) {
                    strncpy(id_val, qid + 3, sizeof(id_val) - 1);
                    char *amp = strchr(id_val, '&');
                    if (amp) *amp = '\0';
                }
                if (qtype) {
                    strncpy(type_val, qtype + 5, sizeof(type_val) - 1);
                    char *amp = strchr(type_val, '&');
                    if (amp) *amp = '\0';
                }
                if (id_val[0]) {
                    snprintf(fn, sizeof(fn), "%s_%s.png", type_val, id_val);
                }
            }
        }

        if (!fn[0] || strstr(fn, "..") || strchr(fn, '/')) {
            send_json_error(client_fd, 400, "Invalid or missing icon identifier");
            close(client_fd);
            return 1;
        }

        char path_buf[256];
        snprintf(path_buf, sizeof(path_buf), "/data/harmony/custom_icons/%s", fn);
        unlink(path_buf);
        snprintf(path_buf, sizeof(path_buf), "/cache/www/assets/icons/%s", fn);
        unlink(path_buf);
        snprintf(path_buf, sizeof(path_buf), "/tmp/www/assets/icons/%s", fn);
        unlink(path_buf);

        const char *under = strchr(fn, '_');
        if (under) {
            snprintf(path_buf, sizeof(path_buf), "/data/harmony/custom_icons/%s", under + 1);
            unlink(path_buf);
        }

        static_cache_flush();
        cJSON *res = cJSON_CreateObject();
        cJSON_AddBoolToObject(res, "ok", 1);
        cJSON_AddStringToObject(res, "deleted", fn);
        send_cjson_resp(client_fd, 200, res);
        cJSON_Delete(res);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/custom-icons/proxy-fetch */
    if (strcmp(clean_path, "/api/custom-icons/proxy-fetch") == 0 && strcmp(method, "GET") == 0) {
        if (!query) {
            send_json_error(client_fd, 400, "Missing query string");
            close(client_fd);
            return 1;
        }
        const char *qurl = strstr(query, "url=");
        if (!qurl || !*(qurl + 4)) {
            send_json_error(client_fd, 400, "Missing url parameter");
            close(client_fd);
            return 1;
        }
        char raw_url[512] = {0};
        strncpy(raw_url, qurl + 4, sizeof(raw_url) - 1);
        char *amp = strchr(raw_url, '&');
        if (amp) *amp = '\0';
        char *space = strchr(raw_url, ' ');
        if (space) *space = '\0';

        char *decoded_url = url_decode(raw_url);
        if (!decoded_url) {
            send_json_error(client_fd, 400, "Failed to decode url parameter");
            close(client_fd);
            return 1;
        }

        if (strncmp(decoded_url, "http://", 7) != 0 && strncmp(decoded_url, "https://", 8) != 0) {
            free(decoded_url);
            send_json_error(client_fd, 400, "Only http:// and https:// URLs allowed");
            close(client_fd);
            return 1;
        }

        char cmd[768];
        snprintf(cmd, sizeof(cmd),
            "/cache/bin/busybox wget -q --no-check-certificate -T 6 -O /tmp/proxy_icon.tmp '%s' 2>/dev/null || "
            "/data/harmony/bin/busybox wget -q --no-check-certificate -T 6 -O /tmp/proxy_icon.tmp '%s' 2>/dev/null",
            decoded_url, decoded_url);
        free(decoded_url);
        system(cmd);

        FILE *f = fopen("/tmp/proxy_icon.tmp", "rb");
        if (!f) {
            send_json_error(client_fd, 502, "Failed to download image from external URL");
            close(client_fd);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (fsz <= 0 || fsz > 2 * 1024 * 1024) {
            fclose(f);
            unlink("/tmp/proxy_icon.tmp");
            send_json_error(client_fd, 413, "Image file empty or exceeds 2 MB limit");
            close(client_fd);
            return 1;
        }

        char header[256];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/png\r\n"
            "Content-Length: %ld\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n",
            fsz);
        send(client_fd, header, (size_t)hlen, 0);

        char buf[8192];
        size_t nr;
        while ((nr = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (send(client_fd, buf, nr, 0) < 0) break;
        }
        fclose(f);
        unlink("/tmp/proxy_icon.tmp");
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/icons/archive */
    if ((strcmp(clean_path, "/api/icons/archive") == 0 || strcmp(clean_path, "/assets/icons.tar.gz") == 0) &&
        (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0)) {
        const char *tar_path = NULL;
        if (access("/cache/icons.tar.gz", F_OK) == 0) {
            tar_path = "/cache/icons.tar.gz";
        } else if (access("/data/harmony/icons.tar.gz", F_OK) == 0) {
            tar_path = "/data/harmony/icons.tar.gz";
        } else if (access("/cache/www/assets/icons.tar.gz", F_OK) == 0) {
            tar_path = "/cache/www/assets/icons.tar.gz";
        }
        if (tar_path) {
            FILE *tf = fopen(tar_path, "rb");
            if (tf) {
                fseek(tf, 0, SEEK_END);
                long tsz = ftell(tf);
                fseek(tf, 0, SEEK_SET);
                char hdr[256];
                int hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/gzip\r\n"
                    "Content-Length: %ld\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Cache-Control: public, max-age=604800, immutable\r\n"
                    "Connection: close\r\n\r\n",
                    tsz);
                send(client_fd, hdr, (size_t)hlen, 0);
                if (strcmp(method, "GET") == 0) {
                    char buf[16384];
                    size_t nr;
                    while ((nr = fread(buf, 1, sizeof(buf), tf)) > 0) {
                        if (send(client_fd, buf, nr, 0) < 0) break;
                    }
                }
                fclose(tf);
                close(client_fd);
                return 1;
            }
        }
        send_json_error(client_fd, 404, "Icons archive not available");
        close(client_fd);
        return 1;
    }

    return 0;
}
