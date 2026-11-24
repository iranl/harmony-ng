#define _GNU_SOURCE
#include "http_api_custom_icons.h"
#include "http_server.h"
#include "http_util.h"
#include "cJSON.h"
#include "icon_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
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

/* Custom icons on flash: the remote takes at most RF_ICON_MAX_BYTES per icon (the web UIs make
 * 40 x 40 images of a few KB), and /data is small */
#define CUSTOM_ICON_MAX_BYTES 16384
#define CUSTOM_ICONS_MAX_COUNT 128
#define CUSTOM_ICONS_MAX_TOTAL (512 * 1024)

/* 1 when adding a file named name of size bytes keeps /data/harmony/custom_icons within its limits
 * (a file of the same name is replaced, so it does not count) */
static int custom_icons_room_for(const char *name, size_t size) {
    DIR *d = opendir("/data/harmony/custom_icons");
    if (!d) return 1;
    size_t count = 0, total = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' || strcmp(de->d_name, name) == 0) continue;
        char p[320];
        struct stat st;
        snprintf(p, sizeof(p), "/data/harmony/custom_icons/%s", de->d_name);
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        count++;
        total += (size_t)st.st_size;
    }
    closedir(d);
    return count + 1 <= CUSTOM_ICONS_MAX_COUNT && total + size <= CUSTOM_ICONS_MAX_TOTAL;
}

/* An icon file name the upload route can make: letters, digits, '-', '_', ending in ".png" */
static int custom_icon_name_ok(const char *fn) {
    size_t n = strlen(fn);
    if (n <= 4 || strcmp(fn + n - 4, ".png") != 0) return 0;
    for (size_t i = 0; i < n - 4; i++) {
        if (!isalnum((unsigned char)fn[i]) && fn[i] != '-' && fn[i] != '_') return 0;
    }
    return 1;
}

/* An extraction runs: set by the one HTTP worker that wins the compare-and-swap, cleared by the
 * extraction thread when it ends */
static volatile int g_icons_extracting = 0;
/* The copy in /tmp was removed since the pack became readable; cleared when an extraction starts,
 * so a copy made while the pack was not readable goes once it is again. Several HTTP workers
 * test and set it: atomic, so only one of them removes the copy */
static int s_tmp_copy_removed;

static void *extract_icons_thread(void *arg) {
    char *tar_path = (char *)arg; /* the starting worker's copy, freed here */
    printf("[+] Unpacking icon archive %s into " HARMONY_TMP_WWW "\n", tar_path);
    fflush(stdout);
    mkdir(HARMONY_TMP_WWW, 0755);
    mkdir(HARMONY_TMP_WWW "/assets", 0755);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
        "tar -xzf \"%s\" -C " HARMONY_TMP_WWW "/assets/ 2>/dev/null && "
        "for d in " HARMONY_TMP_WWW "/assets/icons/*/; do "
        "  if [ -d \"$d\" ]; then "
        "    for f in \"$d\"*.png; do [ -f \"$f\" ] && ln -sf \"$f\" " HARMONY_TMP_WWW "/assets/icons/ 2>/dev/null; done; "
        "  fi; "
        "done",
        tar_path);
    system(cmd);
    free(tar_path);
    __sync_lock_release(&g_icons_extracting);
    return NULL;
}

/* Free RAM left after unpacking a pack of another version into /tmp, for the daemons */
#define ICON_UNPACK_RESERVE_KB 8192UL

static unsigned long free_ram_kb(void) {
#ifdef TEST_HARNESS
    const char *t = getenv("HARMONY_TEST_FREE_RAM_KB");
    if (t && *t) return strtoul(t, NULL, 10);
#endif
    struct sysinfo si;
    if (sysinfo(&si) != 0) return 0;
    unsigned long long b = ((unsigned long long)si.freeram + si.bufferram) * (si.mem_unit ? si.mem_unit : 1);
    return (unsigned long)(b / 1024);
}

/* A pack of another version (after a downgrade to a firmware that reads an older pack version;
 * an OTA package carries no icon archive, so the newer one stays) is a valid tar.gz: it is
 * unpacked into /tmp as a plain archive when the RAM allows it, as before the pack existed.
 * Needed: three times the file (the stock icons: 1.6 MB packed, 4.3 MB in RAM pages) plus a
 * reserve. Free = MemFree + Buffers (2.6.31 has no MemAvailable, and Cached holds tmpfs pages),
 * read at every call until it fits. One log line per file and outcome. */
static int other_pack_fits_in_ram(const char *path, const struct stat *st, int ver) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static struct stat logged;
    static int logged_fit = -1;
    unsigned long need = (unsigned long)(st->st_size / 1024) * 3 + ICON_UNPACK_RESERVE_KB;
    unsigned long have = free_ram_kb();
    int fit = have >= need;
    pthread_mutex_lock(&lock);
    int same = logged_fit == fit && logged.st_ino == st->st_ino && logged.st_dev == st->st_dev &&
               logged.st_size == st->st_size && logged.st_mtime == st->st_mtime;
    if (!same) {
        logged = *st;
        logged_fit = fit;
        if (fit) {
            printf("[!] Icon pack %s: pack version %d, this firmware reads version %d: unpacked into /tmp "
                   "as a plain archive (%lu KB free RAM)\n", path, ver, ICON_PACK_VERSION, have);
        } else {
            printf("[!] Icon pack %s: pack version %d, this firmware reads version %d: not unpacked into "
                   "/tmp (needs %lu KB free RAM, %lu KB free): no stock icons until a matching archive "
                   "is deployed\n", path, ver, ICON_PACK_VERSION, need, have);
        }
        fflush(stdout);
    }
    pthread_mutex_unlock(&lock);
    return fit;
}

void http_ensure_icons_extracted(void) {
    if (icon_pack_available()) {
        /* Icons are read from the packed archive on flash. A copy unpacked into /tmp (RAM) by
         * this or an older firmware before the packed archive was deployed is removed once;
         * uploaded icons there are copies of /data/harmony/custom_icons, which is served too. */
        if (!g_icons_extracting && !__sync_lock_test_and_set(&s_tmp_copy_removed, 1)) {
            if (access(HARMONY_TMP_WWW "/assets/icons", F_OK) == 0) {
                system("rm -rf " HARMONY_TMP_WWW "/assets/icons");
                printf("[+] Unpacked icon copy in /tmp removed: icons are read from the packed archive\n");
            }
        }
        return;
    }
    if (access(HARMONY_TMP_WWW "/assets/icons", F_OK) == 0) return;
    if (g_icons_extracting) return;

    char path[256];
    int ver = -1;
    struct stat st;
    /* the file icon_pack looks at, so a pack it cannot read is the one judged here */
    int kind = icon_pack_archive(path, sizeof(path), &st, &ver);
    if (kind == ICON_ARCHIVE_NONE) return;
    /* a packed archive whose index is not readable right now (being copied, cut): unpacking it
     * into /tmp cannot help, it is read from flash once it is whole again */
    if (kind == ICON_ARCHIVE_PACK) return;
    if (kind == ICON_ARCHIVE_PACK_OTHER && !other_pack_fits_in_ram(path, &st, ver)) return;
#ifdef TEST_HARNESS
    /* widens the window between the checks above and the claim, so the route test can see two
     * workers in it */
    const char *d = getenv("HARMONY_TEST_EXTRACT_DELAY_MS");
    if (d && *d) usleep((useconds_t)strtoul(d, NULL, 10) * 1000);
#endif
    /* several HTTP workers get here at once: only the one that sets the flag starts the
     * extraction, and it looks again whether one finished in the meantime */
    if (!__sync_bool_compare_and_swap(&g_icons_extracting, 0, 1)) return;
    char *arg = strdup(path);
    if (!arg || access(HARMONY_TMP_WWW "/assets/icons", F_OK) == 0) {
        free(arg);
        __sync_lock_release(&g_icons_extracting);
        return;
    }
    __sync_lock_release(&s_tmp_copy_removed);
    pthread_t th;
    if (pthread_create(&th, NULL, extract_icons_thread, arg) == 0) {
        pthread_detach(th);
    } else {
        free(arg);
        __sync_lock_release(&g_icons_extracting);
    }
}

/* Limits of the icon proxy download. wget's -T only ends a stalled transfer, and the size was
 * looked at after the download: a server that keeps sending filled /tmp (RAM) and kept the
 * worker for as long as it sent. */
#define PROXY_FETCH_MAX_BYTES (2 * 1024 * 1024)
#ifndef PROXY_FETCH_MAX_MS
#define PROXY_FETCH_MAX_MS 20000 /* the whole download */
#endif

static void add_icon_name(const char *name, void *ctx) {
    size_t n = strlen(name);
    if (name[0] != '.' && n > 4 && strcmp(name + n - 4, ".png") == 0) {
        cJSON_AddItemToArray((cJSON *)ctx, cJSON_CreateString(name));
    }
}

int http_api_custom_icons_handle(int client_fd, const char *method, const char *clean_path,
                                 const char *query, const char *body, const char *req) {
    (void)req;

    /* Route: GET /api/icons */
    if (strcmp(clean_path, "/api/icons") == 0 && strcmp(method, "GET") == 0) {
        http_ensure_icons_extracted();
        cJSON *arr = cJSON_CreateArray();
        int packed = icon_pack_available();
        if (packed) icon_pack_list(add_icon_name, arr);
        const char *base_dir = HARMONY_TMP_WWW "/assets/icons";
        if (access(base_dir, F_OK) != 0) {
            base_dir = "/cache/www/assets/icons";
            if (access(base_dir, F_OK) != 0) {
                base_dir = "/data/harmony/www/assets/icons";
            }
        }
        /* packed: the folders hold only copies of the uploaded icons, listed below */
        DIR *d = packed ? NULL : opendir(base_dir);
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

        /* One byte more than allowed: an image that fills it is too large (it used to be saved
         * cut at the buffer size and answered ok) */
        uint8_t png_data[CUSTOM_ICON_MAX_BYTES + 1];
        size_t png_len = b64_decode_buffer(j_b64->valuestring, strlen(j_b64->valuestring), png_data, sizeof(png_data));
        cJSON_Delete(root);

        if (png_len > CUSTOM_ICON_MAX_BYTES) {
            send_json_error(client_fd, 413, "Icon image too large (16 KB at most)");
            close(client_fd);
            return 1;
        }

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
        mkdir(HARMONY_TMP_WWW "/assets/icons", 0755);
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

        if (!custom_icons_room_for(main_name, png_len)) {
            send_json_error(client_fd, 507, "No room for more custom icons (128 icons or 512 KB): delete some first");
            close(client_fd);
            return 1;
        }

        char p_data[256], p_cache[256], p_tmp[256];
        snprintf(p_data, sizeof(p_data), "/data/harmony/custom_icons/%s", main_name);
        snprintf(p_cache, sizeof(p_cache), "/cache/www/assets/icons/%s", main_name);
        snprintf(p_tmp, sizeof(p_tmp), HARMONY_TMP_WWW "/assets/icons/%s", main_name);

        if (write_file_atomic_sized(p_data, (const char *)png_data, png_len) != 0) {
            send_json_error(client_fd, 500, "Failed to write custom icon to flash storage");
            close(client_fd);
            return 1;
        }
        if (write_file_atomic_sized(p_cache, (const char *)png_data, png_len) != 0) {
            fprintf(stderr, "[-] Could not copy custom icon to %s\n", p_cache);
        }
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
        char id_val[32] = {0};
        http_query_param(query, "filename", fn, sizeof(fn));
        if (!fn[0]) {
            char type_val[32] = {0};
            http_query_param(query, "id", id_val, sizeof(id_val));
            http_query_param(query, "type", type_val, sizeof(type_val));
            if (id_val[0]) snprintf(fn, sizeof(fn), "%s_%s.png", type_val[0] ? type_val : "activity", id_val);
        }

        /* Only names the upload route makes, and only files it made: the stock icons share
         * /cache/www/assets/icons, and any of them could be deleted by name */
        if (!fn[0] || !custom_icon_name_ok(fn)) {
            send_json_error(client_fd, 400, "Invalid or missing icon identifier");
            close(client_fd);
            return 1;
        }

        char path_buf[256];
        snprintf(path_buf, sizeof(path_buf), "/data/harmony/custom_icons/%s", fn);
        if (unlink(path_buf) == 0) {
            snprintf(path_buf, sizeof(path_buf), "/cache/www/assets/icons/%s", fn);
            unlink(path_buf);
            snprintf(path_buf, sizeof(path_buf), HARMONY_TMP_WWW "/assets/icons/%s", fn);
            unlink(path_buf);
        }

        /* The same icon under its older name: "<id>.png" for an icon deleted by id and type (older
         * firmware saved it so), "<name>.png" for "custom_<name>.png" (the icon registry takes
         * both). The part after the first '_' of any other name can be a different icon:
         * deleting "activity_watch_tv.png" by file name removed "watch_tv.png". */
        char legacy[64] = {0};
        if (id_val[0]) snprintf(legacy, sizeof(legacy), "%s.png", id_val);
        else if (strncmp(fn, "custom_", 7) == 0) snprintf(legacy, sizeof(legacy), "%s", fn + 7);
        if (legacy[0] && strcmp(legacy, fn) != 0 && custom_icon_name_ok(legacy)) {
            snprintf(path_buf, sizeof(path_buf), "/data/harmony/custom_icons/%s", legacy);
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
        char raw_url[512] = {0};
        http_query_param(query, "url", raw_url, sizeof(raw_url)); /* decoded */
        if (!raw_url[0]) {
            send_json_error(client_fd, 400, "Missing url parameter");
            close(client_fd);
            return 1;
        }

        char *decoded_url = strdup(raw_url);
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

        /* The URL goes to wget as one argument, never through a shell. Control characters and
         * blanks have no place in a URL; refuse them instead of passing them on. */
        for (const unsigned char *c = (const unsigned char *)decoded_url; *c; c++) {
            if (*c <= 0x20 || *c == 0x7f) {
                free(decoded_url);
                send_json_error(client_fd, 400, "Invalid character in url");
                close(client_fd);
                return 1;
            }
        }
        /* A file of its own for each request: two fetches at once used to write, send and
         * delete the same /tmp/proxy_icon.tmp */
        char tmp_path[] = "/tmp/proxy_icon.XXXXXX";
        int tmp_fd = mkstemp(tmp_path);
        if (tmp_fd < 0) {
            free(decoded_url);
            send_json_error(client_fd, 500, "No room for the downloaded image");
            close(client_fd);
            return 1;
        }
        close(tmp_fd);
        int fetched = fetch_url_checked(decoded_url, tmp_path, PROXY_FETCH_MAX_BYTES, PROXY_FETCH_MAX_MS, 6);
        static const char *const wget_paths[] = { "/cache/bin/busybox", "/data/harmony/bin/busybox" };
        for (size_t wi = 0; fetched == RUN_NO_FETCH && wi < sizeof(wget_paths) / sizeof(wget_paths[0]); wi++) {
            char *argv[] = { (char *)wget_paths[wi], "wget", "-q", "--no-check-certificate", "-T", "6",
                             "-O", tmp_path, decoded_url, NULL };
            if (access(wget_paths[wi], X_OK) != 0) continue;
            fetched = run_argv_limited(argv, tmp_path, PROXY_FETCH_MAX_BYTES, PROXY_FETCH_MAX_MS);
            if (fetched == 0 || fetched == RUN_TOO_BIG || fetched == RUN_TOO_SLOW) break;
        }
        free(decoded_url);

        FILE *f = (fetched == 0) ? fopen(tmp_path, "rb") : NULL;
        if (!f) {
            unlink(tmp_path);
            if (fetched == RUN_TOO_BIG) {
                send_json_error(client_fd, 413, "Image file empty or exceeds 2 MB limit");
            } else if (fetched == RUN_TOO_SLOW) {
                send_json_error(client_fd, 502, "Image download took too long");
            } else {
                send_json_error(client_fd, 502, "Failed to download image from external URL");
            }
            close(client_fd);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (fsz <= 0 || fsz > PROXY_FETCH_MAX_BYTES) {
            fclose(f);
            unlink(tmp_path);
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
        http_send_all(client_fd, header, (size_t)hlen);

        char buf[8192];
        size_t nr;
        while ((nr = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (http_send_all(client_fd, buf, nr) != 0) break;
        }
        fclose(f);
        unlink(tmp_path);
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
                http_send_all(client_fd, hdr, (size_t)hlen);
                if (strcmp(method, "GET") == 0) {
                    char buf[16384];
                    size_t nr;
                    while ((nr = fread(buf, 1, sizeof(buf), tf)) > 0) {
                        if (http_send_all(client_fd, buf, nr) != 0) break;
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
