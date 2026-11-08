#define _GNU_SOURCE
#include "http_server.h"
#include "http_util.h"
#include "http_auth.h"
#include "http_api_activities.h"
#include "http_api_devices.h"
#include "http_api_commands.h"
#include "http_api_custom_icons.h"
#include "http_api_network.h"
#include "http_api_remotes.h"
#include "http_api_ota.h"
#include "http_api_backup.h"
#include "http_api_system.h"
#include "hub_ws.h"
#include "ws_server.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTTP_REQ_BUF_SIZE 8192
#define STATIC_CACHE_MAX_ENTRIES 32
#define STATIC_CACHE_MAX_SIZE (384 * 1024)

/* Files served from flash, kept in memory. An entry remembers the file it was read from and
 * is dropped when that file changes, so a Web UI deploy is served without a restart. A
 * request holds an entry while it sends it; a dropped entry is freed by its last user. */
struct static_cache_entry {
    char rel_path[128];
    int is_gzip;
    char *data;
    size_t size;
    const char *mime;
    char src_path[520];
    time_t src_mtime;
    off_t src_size;
    int users;
    int stale;
};

static struct static_cache_entry g_static_cache[STATIC_CACHE_MAX_ENTRIES];
static int g_static_cache_count = 0;
static pthread_mutex_t g_static_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Frees an entry's data now, or marks it for its last user to free; called with the lock held */
static void static_cache_drop(struct static_cache_entry *e) {
    if (e->users > 0) {
        e->stale = 1;
        return;
    }
    free(e->data);
    e->data = NULL;
    e->stale = 0;
    e->rel_path[0] = '\0';
    e->size = 0;
}

void static_cache_flush(void) {
    pthread_mutex_lock(&g_static_cache_mutex);
    for (int i = 0; i < g_static_cache_count; i++) {
        if (g_static_cache[i].data) static_cache_drop(&g_static_cache[i]);
    }
    pthread_mutex_unlock(&g_static_cache_mutex);
}

/* The entry for rel_path, held for the caller until static_cache_release(); NULL when there is
   none or its file changed */
static struct static_cache_entry *static_cache_find(const char *rel_path, int is_gzip) {
    struct static_cache_entry *found = NULL;
    pthread_mutex_lock(&g_static_cache_mutex);
    for (int i = 0; i < g_static_cache_count; i++) {
        struct static_cache_entry *e = &g_static_cache[i];
        if (!e->data || e->stale || e->is_gzip != is_gzip || strcmp(e->rel_path, rel_path) != 0) continue;
        struct stat st;
        if (stat(e->src_path, &st) != 0 || st.st_mtime != e->src_mtime || st.st_size != e->src_size) {
            static_cache_drop(e);
            break;
        }
        e->users++;
        found = e;
        break;
    }
    pthread_mutex_unlock(&g_static_cache_mutex);
    return found;
}

static void static_cache_release(struct static_cache_entry *e) {
    pthread_mutex_lock(&g_static_cache_mutex);
    if (--e->users == 0 && e->stale) static_cache_drop(e);
    pthread_mutex_unlock(&g_static_cache_mutex);
}

static void static_cache_insert(const char *rel_path, int is_gzip, const char *mime, const char *data, size_t size,
                                const char *src_path, const struct stat *src) {
    if (!rel_path || !data || size == 0 || size > STATIC_CACHE_MAX_SIZE || !src_path || !src) return;
    pthread_mutex_lock(&g_static_cache_mutex);
    struct static_cache_entry *slot = NULL;
    for (int i = 0; i < g_static_cache_count; i++) {
        struct static_cache_entry *e = &g_static_cache[i];
        if (e->data && !e->stale && e->is_gzip == is_gzip && strcmp(e->rel_path, rel_path) == 0) {
            pthread_mutex_unlock(&g_static_cache_mutex);
            return;
        }
        if (!slot && !e->data && e->users == 0) slot = e;
    }
    if (!slot && g_static_cache_count < STATIC_CACHE_MAX_ENTRIES) slot = &g_static_cache[g_static_cache_count++];
    char *copy = slot ? (char *)malloc(size) : NULL;
    if (copy) {
        memcpy(copy, data, size);
        snprintf(slot->rel_path, sizeof(slot->rel_path), "%s", rel_path);
        snprintf(slot->src_path, sizeof(slot->src_path), "%s", src_path);
        slot->is_gzip = is_gzip;
        slot->data = copy;
        slot->size = size;
        slot->mime = mime;
        slot->src_mtime = src->st_mtime;
        slot->src_size = src->st_size;
        slot->stale = 0;
        slot->users = 0;
    }
    pthread_mutex_unlock(&g_static_cache_mutex);
}

static const char *get_mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "application/javascript";
    if (strcmp(dot, ".css") == 0) return "text/css";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".woff2") == 0) return "font/woff2";
    if (strcmp(dot, ".woff") == 0) return "font/woff";
    if (strcmp(dot, ".ttf") == 0) return "font/ttf";
    if (strcmp(dot, ".wasm") == 0) return "application/wasm";
    return "application/octet-stream";
}

static int is_http_keep_alive(const char *req) {
    if (!req) return 0;
    if (strstr(req, "HTTP/1.1")) {
        if (strcasestr(req, "Connection: close")) return 0;
        return 1;
    }
    if (strcasestr(req, "Connection: keep-alive")) return 1;
    return 0;
}

static int serve_static_file(int client_fd, const char *req, const char *path, int keep_alive) {
    if (strstr(path, "..")) return 0;

    char rel_path[256];
    if (strcmp(path, "/") == 0 || path[0] == '\0' || !strchr(path, '.')) {
        strncpy(rel_path, "/index.html", sizeof(rel_path) - 1);
        rel_path[sizeof(rel_path) - 1] = '\0';
    } else {
        strncpy(rel_path, path, sizeof(rel_path) - 1);
        rel_path[sizeof(rel_path) - 1] = '\0';
    }

    if (strncmp(rel_path, "/assets/icons/", 14) == 0) {
        http_ensure_icons_extracted();
    }

    int client_gzip = (strstr(req, "Accept-Encoding") && strstr(req, "gzip")) ? 1 : 0;

    struct static_cache_entry *cached = NULL;
    if (client_gzip) {
        cached = static_cache_find(rel_path, 1);
    }
    if (!cached) {
        cached = static_cache_find(rel_path, 0);
    }

    if (cached) {
        const char *cache_hdr = (strstr(rel_path, "/assets/") != NULL) ?
            "Cache-Control: public, max-age=31536000, immutable\r\n" :
            "Cache-Control: no-cache, no-store, must-revalidate\r\nPragma: no-cache\r\nExpires: 0\r\n";
        char header[512];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "%s"
            "Access-Control-Allow-Origin: *\r\n"
            "%s"
            "Connection: %s\r\n"
            "%s\r\n",
            cached->mime, cached->size,
            cached->is_gzip ? "Content-Encoding: gzip\r\n" : "",
            cache_hdr,
            keep_alive ? "keep-alive" : "close",
            keep_alive ? "Keep-Alive: timeout=2, max=64\r\n" : "");

        send(client_fd, header, (size_t)hlen, 0);
        if (strncmp(req, "HEAD ", 5) != 0 && cached->size > 0) {
            size_t sent = 0;
            while (sent < cached->size) {
                ssize_t s = send(client_fd, cached->data + sent, cached->size - sent, 0);
                if (s <= 0) break;
                sent += (size_t)s;
            }
        }
        static_cache_release(cached);
        return 1;
    }

    char file_path[512];
    snprintf(file_path, sizeof(file_path), "/tmp/www%s", rel_path);
    if (access(file_path, F_OK) != 0) {
        snprintf(file_path, sizeof(file_path), "/data/harmony/www%s", rel_path);
        if (access(file_path, F_OK) != 0) {
            snprintf(file_path, sizeof(file_path), "/cache/www%s", rel_path);
            if (access(file_path, F_OK) != 0 && strncmp(rel_path, "/assets/icons/", 14) == 0) {
                snprintf(file_path, sizeof(file_path), "/data/harmony/custom_icons/%s", rel_path + 14);
                if (access(file_path, F_OK) != 0) {
                    const char *ifn = rel_path + 14;
                    const char *mapped = NULL;
                    if (strcmp(ifn, "watchtv.png") == 0 || strcmp(ifn, "smarttv.png") == 0) mapped = "tv.png";
                    else if (strcmp(ifn, "movie.png") == 0) mapped = "BAB09C4A-E61D-4284-B1F3-84135F562217_small.png";
                    else if (strcmp(ifn, "music.png") == 0) mapped = "72D0AAB4-2ED8-4644-84D2-0031BDAE356E_small.png";
                    else if (strcmp(ifn, "apple.png") == 0) mapped = "0F40F257-68C8-4F24-9561-3CB3441DA395_small.png";
                    else if (strcmp(ifn, "roku.png") == 0) mapped = "6EBD03B8-C815-48BE-ADBB-D36E06CAAC5E_small.png";
                    else if (strcmp(ifn, "pc.png") == 0) mapped = "computer.png";
                    else if (strcmp(ifn, "receiver.png") == 0) mapped = "heos.png";
                    else if (strcmp(ifn, "light.png") == 0) mapped = "bulb.png";
                    else if (strcmp(ifn, "poweroff.png") == 0) mapped = "power_off.png";
                    else if (strcmp(ifn, "generic.png") == 0) mapped = "default.png";

                    if (mapped) {
                        snprintf(file_path, sizeof(file_path), "/tmp/www/assets/icons/%s", mapped);
                        if (access(file_path, F_OK) != 0) {
                            snprintf(file_path, sizeof(file_path), "/cache/www/assets/icons/%s", mapped);
                        }
                    }
                }
            }
        }
    }

    int is_gzip = 0;
    FILE *f = NULL;
    char src_path[520] = "";
    struct stat st_raw, st_gz;
    int has_raw = (stat(file_path, &st_raw) == 0);

    if (client_gzip || !has_raw) {
        char gz_path[520];
        snprintf(gz_path, sizeof(gz_path), "%s.gz", file_path);
        if (stat(gz_path, &st_gz) == 0) {
            if (!has_raw || st_gz.st_mtime >= st_raw.st_mtime) {
                f = fopen(gz_path, "rb");
                if (f) {
                    is_gzip = 1;
                    snprintf(src_path, sizeof(src_path), "%s", gz_path);
                }
            }
        }
    }

    if (!f && has_raw) {
        f = fopen(file_path, "rb");
        if (f) snprintf(src_path, sizeof(src_path), "%s", file_path);
    }

    if (!f && !strchr(rel_path, '.')) {
        const char *idx_raw = "/data/harmony/www/index.html";
        const char *idx_gz = "/data/harmony/www/index.html.gz";
        if (access(idx_raw, F_OK) != 0 && access(idx_gz, F_OK) != 0) {
            idx_raw = "/cache/www/index.html";
            idx_gz = "/cache/www/index.html.gz";
        }
        int has_idx_raw = (stat(idx_raw, &st_raw) == 0);
        int has_idx_gz = (stat(idx_gz, &st_gz) == 0);

        if (client_gzip || !has_idx_raw) {
            if (has_idx_gz) {
                if (!has_idx_raw || st_gz.st_mtime >= st_raw.st_mtime) {
                    f = fopen(idx_gz, "rb");
                    if (f) {
                        is_gzip = 1;
                        snprintf(src_path, sizeof(src_path), "%s", idx_gz);
                    }
                }
            }
        }
        if (!f && has_idx_raw) {
            f = fopen(idx_raw, "rb");
            if (f) snprintf(src_path, sizeof(src_path), "%s", idx_raw);
        }
        if (f) {
            strncpy(rel_path, "/index.html", sizeof(rel_path) - 1);
            rel_path[sizeof(rel_path) - 1] = '\0';
        }
    }

    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    const char *mime = get_mime_type(rel_path);
    const char *cache_hdr = (strstr(rel_path, "/assets/") != NULL) ?
        "Cache-Control: public, max-age=31536000, immutable\r\n" :
        "Cache-Control: no-cache, no-store, must-revalidate\r\nPragma: no-cache\r\nExpires: 0\r\n";
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "%s"
        "Access-Control-Allow-Origin: *\r\n"
        "%s"
        "Connection: %s\r\n"
        "%s\r\n",
        mime, fsize,
        is_gzip ? "Content-Encoding: gzip\r\n" : "",
        cache_hdr,
        keep_alive ? "keep-alive" : "close",
        keep_alive ? "Keep-Alive: timeout=2, max=64\r\n" : "");

    send(client_fd, header, (size_t)hlen, 0);

    if (fsize > 0 && fsize <= STATIC_CACHE_MAX_SIZE) {
        char *file_buf = (char *)malloc((size_t)fsize);
        if (file_buf) {
            size_t nr = fread(file_buf, 1, (size_t)fsize, f);
            if (nr > 0) {
                if (strncmp(req, "HEAD ", 5) != 0) {
                    size_t sent = 0;
                    while (sent < nr) {
                        ssize_t s = send(client_fd, file_buf + sent, nr - sent, 0);
                        if (s <= 0) break;
                        sent += (size_t)s;
                    }
                }
                struct stat src;
                if (strncmp(file_path, "/tmp/", 5) != 0 && fstat(fileno(f), &src) == 0) {
                    static_cache_insert(rel_path, is_gzip, mime, file_buf, nr, src_path, &src);
                }
            }
            free(file_buf);
            fclose(f);
            return 1;
        }
    }

    if (strncmp(req, "HEAD ", 5) != 0) {
        char chunk[16384];
        size_t nr;
        while ((nr = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            if (send(client_fd, chunk, nr, 0) < 0) break;
        }
    }
    fclose(f);
    return 1;
}

static void http_process_request(int client_fd, const char *req) {
    char method[16] = {0}, path[256] = {0};
    if (sscanf(req, "%15s %255s", method, path) != 2) {
        close(client_fd);
        return;
    }

    char clean_path[256] = {0};
    strncpy(clean_path, path, sizeof(clean_path) - 1);
    char *qmark = strchr(clean_path, '?');
    const char *query = NULL;
    if (qmark) {
        *qmark = '\0';
        query = path + (qmark - clean_path) + 1;
    }

    const char *body = strstr(req, "\r\n\r\n");
    if (body) body += 4;

    /* Handle CORS OPTIONS preflight */
    if (strcmp(method, "OPTIONS") == 0) {
        char origin_buf[128] = "*";
        const char *orig_hdr = strcasestr(req, "Origin:");
        if (orig_hdr) {
            orig_hdr += 7;
            while (*orig_hdr == ' ' || *orig_hdr == '\t') orig_hdr++;
            const char *eol = strpbrk(orig_hdr, "\r\n");
            size_t olen = eol ? (size_t)(eol - orig_hdr) : strlen(orig_hdr);
            if (olen > 0 && olen < sizeof(origin_buf) - 1) {
                memcpy(origin_buf, orig_hdr, olen);
                origin_buf[olen] = '\0';
            }
        }
        char opt_resp[512];
        snprintf(opt_resp, sizeof(opt_resp),
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: %s\r\n"
            "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Authorization, Content-Type, X-Requested-With, Accept, Origin\r\n"
            "Access-Control-Max-Age: 86400\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n",
            origin_buf);
        send(client_fd, opt_resp, strlen(opt_resp), 0);
        close(client_fd);
        return;
    }

    char client_ip[48] = {0};
    get_client_ip(client_fd, client_ip, sizeof(client_ip));
    char auth_user[MAX_USERNAME_LEN] = {0};
    enum http_role auth_role = http_auth_check_request(req, client_ip, method, auth_user, sizeof(auth_user));

    /* Check HTTP authentication & RBAC permissions */
    if (strcmp(clean_path, "/api/auth/token") == 0) {
        /* Open login endpoint */
    } else if (strncmp(clean_path, "/api/", 5) == 0 || strcmp(clean_path, "/wifi") == 0 ||
        strcmp(clean_path, "/ethernet") == 0 || strcmp(clean_path, "/import") == 0 ||
        strncmp(clean_path, "/export/", 8) == 0 || strncmp(clean_path, "/ir/", 4) == 0 ||
        strncmp(clean_path, "/bt/", 4) == 0 || strcmp(clean_path, "/system") == 0 ||
        strcmp(clean_path, "/mqtt") == 0) {
        if (auth_role == ROLE_NONE) {
            http_auth_send_challenge(client_fd, client_ip);
            return;
        }
        if (auth_role == ROLE_USER && !http_auth_is_user_role_permitted(method, clean_path)) {
            const char *forbidden =
                "HTTP/1.1 403 Forbidden\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Headers: Authorization, Content-Type, X-Requested-With, Accept, Origin\r\n"
                "Content-Length: 53\r\n"
                "Connection: close\r\n\r\n"
                "{\"ok\":false,\"error\":\"Forbidden: Admin access required\"}";
            send(client_fd, forbidden, strlen(forbidden), 0);
            close(client_fd);
            return;
        }
    }

    /* Fallback static file serving */
    if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) &&
        strncmp(clean_path, "/api/", 5) != 0 &&
        strcmp(clean_path, "/ws") != 0 &&
        strcmp(clean_path, "/health") != 0 &&
        strncmp(clean_path, "/export/", 8) != 0) {
        if (serve_static_file(client_fd, req, clean_path, 0)) {
            close(client_fd);
            return;
        }
    }

    /* Dispatch to modular route handlers */
    if (http_api_auth_handle(client_fd, method, clean_path, query, body, req, client_ip, auth_role)) return;
    if (http_api_activities_handle(client_fd, method, clean_path, query, body, req)) return;
    if (http_api_devices_handle(client_fd, method, clean_path, query, body, req)) return;
    if (http_api_commands_handle(client_fd, method, clean_path, query, body, req)) return;
    if (http_api_custom_icons_handle(client_fd, method, clean_path, query, body, req)) return;
    if (http_api_network_handle(client_fd, method, clean_path, query, body, req)) return;
    if (http_api_remotes_handle(client_fd, method, clean_path, query, body, req)) return;
    if (http_api_ota_handle(client_fd, method, clean_path, query, body, req)) return;
    if (http_api_backup_handle(client_fd, method, clean_path, query, body, req)) return;
    if (http_api_system_handle(client_fd, method, clean_path, query, body, req, auth_role, auth_user)) return;

    /* Unmatched route: 404 */
    send_json_error(client_fd, 404, "Not Found");
    close(client_fd);
}

void http_handle_client(int client_fd) {
    if (client_fd < 0) return;

    struct timeval rcv_tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));

    int req_count = 0;
    while (req_count < 64) {
        char *init_buf = (char *)calloc(1, HTTP_REQ_BUF_SIZE);
        if (!init_buf) {
            close(client_fd);
            return;
        }

        size_t total_hdr = 0;
        while (total_hdr < HTTP_REQ_BUF_SIZE - 1) {
            ssize_t n = recv(client_fd, init_buf + total_hdr, HTTP_REQ_BUF_SIZE - 1 - total_hdr, 0);
            if (n <= 0) break;
            total_hdr += (size_t)n;
            init_buf[total_hdr] = '\0';
            if (strstr(init_buf, "\r\n\r\n") != NULL || ws_is_upgrade_req(init_buf)) {
                break;
            }
        }
        if (total_hdr == 0) {
            free(init_buf);
            close(client_fd);
            return;
        }
        req_count++;

        /* Check for WebSocket upgrade. With auth enabled the upgrade needs a PAT or session
         * token, as "Authorization: Bearer" or "?token=" in the request line; loopback callers
         * (hub_rf, hub_btstack) are admin without one, as for every request. */
        if (ws_is_upgrade_req(init_buf)) {
            char ws_ip[48] = {0};
            get_client_ip(client_fd, ws_ip, sizeof(ws_ip));
            char ws_user[MAX_USERNAME_LEN] = {0};
            enum http_role ws_role = http_auth_check_ws_upgrade(init_buf, ws_ip, ws_user, sizeof(ws_user));
            if (ws_role == ROLE_NONE) {
                http_auth_send_challenge(client_fd, ws_ip); /* 401, closes the socket */
                free(init_buf);
                return;
            }
            /* A failed upgrade (bad request, no free client slot, send error) leaves the socket
             * unregistered: close it here, nobody else will */
            if (ws_handle_upgrade(client_fd, init_buf) != 0) {
                close(client_fd);
            } else {
                ws_set_client_role(client_fd, (int)ws_role);
            }
            free(init_buf);
            return;
        }

        const char *req = init_buf;
        char *dyn_buf = NULL;

        /* Check Content-Length for large bodies (e.g. bundle import up to 2MB) */
        char *hdr_end = strstr(init_buf, "\r\n\r\n");
        if (hdr_end) {
            char *cl_pos = strcasestr(init_buf, "Content-Length:");
            if (cl_pos && cl_pos < hdr_end) {
                cl_pos += 15;
                while (*cl_pos == ' ' || *cl_pos == '\t') cl_pos++;
                long cl = atol(cl_pos);
                size_t hdr_len = (size_t)(hdr_end + 4 - init_buf);
                size_t body_recvd = (total_hdr >= hdr_len) ? (total_hdr - hdr_len) : 0;
                if (cl > 0 && cl < 2 * 1024 * 1024 && (size_t)cl > body_recvd) {
                    size_t total_needed = hdr_len + (size_t)cl;
                    dyn_buf = (char *)malloc(total_needed + 1);
                    if (dyn_buf) {
                        memcpy(dyn_buf, init_buf, total_hdr);
                        size_t total_read = total_hdr;
                        while (total_read < total_needed) {
                            ssize_t r = recv(client_fd, dyn_buf + total_read, total_needed - total_read, 0);
                            if (r <= 0) break;
                            total_read += (size_t)r;
                        }
                        dyn_buf[total_read] = '\0';
                        req = dyn_buf;
                    }
                }
            }
        }

        int keep_alive = is_http_keep_alive(req);
        int handled_static = 0;

        char method[16] = {0}, path[256] = {0};
        if (sscanf(req, "%15s %255s", method, path) == 2) {
            char clean_path[256] = {0};
            strncpy(clean_path, path, sizeof(clean_path) - 1);
            char *qmark = strchr(clean_path, '?');
            if (qmark) *qmark = '\0';

            if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) &&
                strncmp(clean_path, "/api/", 5) != 0 &&
                strcmp(clean_path, "/ws") != 0 &&
                strcmp(clean_path, "/health") != 0 &&
                strncmp(clean_path, "/export/", 8) != 0) {
                if (serve_static_file(client_fd, req, clean_path, keep_alive)) {
                    handled_static = 1;
                }
            }
        }

        if (!handled_static) {
            http_process_request(client_fd, req);
            if (dyn_buf) free(dyn_buf);
            free(init_buf);
            return;
        }

        if (dyn_buf) free(dyn_buf);
        free(init_buf);

        if (!keep_alive) {
            close(client_fd);
            return;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        fd_set rfd;
        FD_ZERO(&rfd);
        FD_SET(client_fd, &rfd);
        int sel = select(client_fd + 1, &rfd, NULL, NULL, &tv);
        if (sel <= 0) {
            close(client_fd);
            return;
        }
    }
    close(client_fd);
}

int http_server_init(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    fcntl(s, F_SETFD, FD_CLOEXEC);

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port > 0 ? port : HTTP_SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s);
        return -1;
    }

    if (listen(s, HTTP_MAX_CLIENTS) != 0) {
        close(s);
        return -1;
    }

    http_ensure_icons_extracted();

    return s;
}

void http_server_close(int server_fd) {
    if (server_fd >= 0) close(server_fd);
}
