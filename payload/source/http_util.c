#define _GNU_SOURCE
#include "http_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

void send_cjson_resp(int fd, int code, cJSON *obj) {
    const char *status_str = (code == 200) ? "OK" :
                            (code == 201) ? "Created" :
                            (code == 400) ? "Bad Request" :
                            (code == 401) ? "Unauthorized" :
                            (code == 403) ? "Forbidden" :
                            (code == 404) ? "Not Found" :
                            (code == 409) ? "Conflict" :
                            (code == 503) ? "Service Unavailable" : "Internal Server Error";
    char *json = obj ? cJSON_PrintUnformatted(obj) : NULL;
    size_t body_len = json ? strlen(json) : 0;
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: application/json; charset=utf-8\r\n"
                       "Content-Length: %zu\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "Connection: close\r\n\r\n",
                       code, status_str, body_len);
    send(fd, header, (size_t)hlen, 0);
    if (body_len && json) send(fd, json, body_len, 0);
    if (json) free(json);
}

void send_json_error(int fd, int code, const char *msg) {
    cJSON *obj = cJSON_CreateObject();
    if (obj) {
        cJSON_AddBoolToObject(obj, "ok", 0);
        cJSON_AddStringToObject(obj, "error", msg ? msg : "Error");
        send_cjson_resp(fd, code, obj);
        cJSON_Delete(obj);
    }
}

char *url_decode(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    char *dst = out;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return out;
}

cJSON *parse_request_params(const char *body) {
    if (!body || !body[0]) return NULL;
    while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n') body++;
    if (*body == '{' || *body == '[') {
        return cJSON_Parse(body);
    }
    cJSON *obj = cJSON_CreateObject();
    char *copy = strdup(body);
    if (!copy) return obj;
    char *p = copy;
    while (p && *p) {
        char *amp = strchr(p, '&');
        if (amp) *amp = '\0';
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            char *k = url_decode(p);
            char *v = url_decode(eq + 1);
            if (k && v) {
                cJSON_AddStringToObject(obj, k, v);
            }
            if (k) free(k);
            if (v) free(v);
        }
        p = amp ? amp + 1 : NULL;
    }
    free(copy);
    return obj;
}

int write_file_content(const char *filepath, const char *content) {
    if (!filepath || !content) return -1;
    FILE *f = fopen(filepath, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

int write_file_atomic_sized(const char *filepath, const char *content, size_t len) {
    if (!filepath || !content) return -1;
    /* Unique per call: two threads of the daemon may write the same file at once */
    static unsigned int s_seq;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%u", filepath, (int)getpid(), __sync_fetch_and_add(&s_seq, 1));
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t written = fwrite(content, 1, len, f);
    /* Data on flash before the rename: otherwise a power cut or a full partition can leave
     * the new name pointing at an empty or cut file */
    int failed = (written != len) || fflush(f) != 0 || fsync(fileno(f)) != 0;
    if (fclose(f) != 0) failed = 1;
    if (failed) {
        unlink(tmp);
        return -1;
    }
    return rename(tmp, filepath);
}

char *read_file_text_alloc(const char *path, size_t max_sz, size_t *out_len) {
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz > max_sz) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *b64_encode(const unsigned char *src, size_t len) {
    if (!src && len > 0) return NULL;
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? src[i++] : 0;
        uint32_t octet_b = i < len ? src[i++] : 0;
        uint32_t octet_c = i < len ? src[i++] : 0;
        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : b64_table[(triple >> 6) & 0x3F];
        out[j++] = (i > len) ? '=' : b64_table[triple & 0x3F];
    }
    out[j] = '\0';
    return out;
}

unsigned char *b64_decode(const char *src, size_t *out_len) {
    if (!src) return NULL;
    size_t len = strlen(src);
    if (len % 4 != 0) return NULL;
    size_t pad = 0;
    if (len > 0 && src[len - 1] == '=') pad++;
    if (len > 1 && src[len - 2] == '=') pad++;
    size_t o_len = (len / 4) * 3 - pad;
    unsigned char *out = (unsigned char *)malloc(o_len + 1);
    if (!out) return NULL;

    int8_t d_table[256];
    memset(d_table, -1, sizeof(d_table));
    for (int i = 0; i < 64; i++) d_table[(unsigned char)b64_table[i]] = (int8_t)i;

    size_t i = 0, j = 0;
    while (i < len) {
        int8_t b0 = d_table[(unsigned char)src[i++]];
        int8_t b1 = d_table[(unsigned char)src[i++]];
        int8_t b2 = (src[i] == '=') ? 0 : d_table[(unsigned char)src[i]]; i++;
        int8_t b3 = (src[i] == '=') ? 0 : d_table[(unsigned char)src[i]]; i++;
        if (b0 < 0 || b1 < 0 || b2 < 0 || b3 < 0) {
            free(out);
            return NULL;
        }
        uint32_t triple = ((uint32_t)b0 << 18) | ((uint32_t)b1 << 12) | ((uint32_t)b2 << 6) | (uint32_t)b3;
        if (j < o_len) out[j++] = (unsigned char)((triple >> 16) & 0xFF);
        if (j < o_len) out[j++] = (unsigned char)((triple >> 8) & 0xFF);
        if (j < o_len) out[j++] = (unsigned char)(triple & 0xFF);
    }
    out[o_len] = '\0';
    if (out_len) *out_len = o_len;
    return out;
}

uint32_t crc32_simple(const unsigned char *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int)(crc & 1)));
        }
    }
    return ~crc;
}

int is_safe_param(const char *str, const char *allowed_extra) {
    if (!str || !str[0]) return 0;
    while (*str) {
        char c = *str++;
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') continue;
        if (allowed_extra && strchr(allowed_extra, c)) continue;
        return 0;
    }
    return 1;
}

int is_safe_ipv4(const char *ip) {
    if (!ip || !ip[0]) return 0;
    int dots = 0, digits = 0, val = 0;
    const char *p = ip;
    while (*p) {
        if (*p == '.') {
            if (digits == 0 || digits > 3 || val > 255) return 0;
            dots++;
            digits = 0;
            val = 0;
        } else if (isdigit((unsigned char)*p)) {
            digits++;
            val = val * 10 + (*p - '0');
        } else {
            return 0;
        }
        p++;
    }
    if (dots != 3 || digits == 0 || digits > 3 || val > 255) return 0;
    return 1;
}

void escape_wpa_str(const char *src, char *dst, size_t dst_len) {
    if (!src || !dst || dst_len == 0) return;
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_len; i++) {
        if (src[i] == '"' || src[i] == '\\' || src[i] == '$' || src[i] == '`') {
            dst[j++] = '\\';
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* Peer address of an accepted connection. Fails closed: when getpeername() fails (for
 * example the client reset the connection right after sending its request) ip_buf is left
 * empty and -1 is returned. Never fall back to a loopback address: loopback callers are
 * admin in http_auth, so a fallback would turn a reset connection into an admin request. */
int get_client_ip(int fd, char *ip_buf, size_t buf_len) {
    if (!ip_buf || buf_len == 0) return -1;
    ip_buf[0] = '\0';
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &len) != 0) return -1;
    if (addr.ss_family == AF_INET) {
        if (!inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, ip_buf, (socklen_t)buf_len)) {
            ip_buf[0] = '\0';
            return -1;
        }
        return 0;
    }
    if (addr.ss_family == AF_INET6) {
        if (!inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&addr)->sin6_addr, ip_buf, (socklen_t)buf_len)) {
            ip_buf[0] = '\0';
            return -1;
        }
        return 0;
    }
    return -1;
}

/* Value of request header `name` (case-insensitive, only in the header block, only at the
 * start of a line), copied into out without leading or trailing blanks. Returns 1 when found. */
int http_header_value(const char *req, const char *name, char *out, size_t out_len) {
    if (!req || !name || !out || out_len == 0) return 0;
    out[0] = '\0';
    size_t nlen = strlen(name);
    const char *end = strstr(req, "\r\n\r\n");
    if (!end) end = req + strlen(req);
    const char *line = strstr(req, "\r\n"); /* skip the request line */
    while (line && line < end) {
        line += 2;
        if (line >= end) break;
        if ((size_t)(end - line) > nlen && strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
            const char *v = line + nlen + 1;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            const char *eol = strstr(v, "\r\n");
            if (!eol || eol > end) eol = end;
            while (eol > v && (eol[-1] == ' ' || eol[-1] == '\t')) eol--;
            size_t vlen = (size_t)(eol - v);
            if (vlen >= out_len) vlen = out_len - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }
        line = strstr(line, "\r\n");
    }
    return 0;
}

/* Origins the Hub's own clients use: the web UI served by the Hub (same host as the request),
 * the Android app (Capacitor, http://localhost), and local development servers. */
int http_origin_is_allowed(const char *origin, const char *host) {
    if (!origin || !origin[0]) return 0;
    const char *rest = NULL;
    if (strncasecmp(origin, "http://", 7) == 0) rest = origin + 7;
    else if (strncasecmp(origin, "https://", 8) == 0) rest = origin + 8;
    else if (strcasecmp(origin, "capacitor://localhost") == 0 || strcasecmp(origin, "ionic://localhost") == 0) return 1;
    else return 0; /* includes "null" (sandboxed frames, file:// pages) */
    if (strchr(rest, '/')) return 0;
    if (host && host[0] && strcasecmp(rest, host) == 0) return 1;
    const char *locals[] = { "localhost", "127.0.0.1", "[::1]" };
    for (size_t i = 0; i < sizeof(locals) / sizeof(locals[0]); i++) {
        size_t l = strlen(locals[i]);
        if (strncasecmp(rest, locals[i], l) == 0 && (rest[l] == '\0' || rest[l] == ':')) return 1;
    }
    return 0;
}

/* Cross-site request check for the Hub API (port 8089). A request is refused when it carries
 * an Origin that is not one of the Hub's own clients, or, without an Origin, when the browser
 * marks it as coming from another site (Sec-Fetch-Site: cross-site or same-site, for example an
 * <img> or a top-level form post from another page). Clients outside a browser send neither
 * header and are not affected. */
int http_request_is_cross_site(const char *req) {
    char origin[160], host[160], site[32];
    int has_origin = http_header_value(req, "Origin", origin, sizeof(origin));
    if (!http_header_value(req, "Host", host, sizeof(host))) host[0] = '\0';
    if (has_origin && origin[0]) {
        if (http_origin_is_allowed(origin, host)) return 0;
        /* Behind a reverse proxy the browser's host is in X-Forwarded-Host. A page cannot set
         * that header on a request that skips the preflight, and the preflight is refused. */
        char fwd[160];
        if (http_header_value(req, "X-Forwarded-Host", fwd, sizeof(fwd)) && fwd[0] &&
            http_origin_is_allowed(origin, fwd)) return 0;
        return 1;
    }
    if (http_header_value(req, "Sec-Fetch-Site", site, sizeof(site)) &&
        (strcasecmp(site, "cross-site") == 0 || strcasecmp(site, "same-site") == 0)) {
        return 1;
    }
    return 0;
}

/* Runs argv[0] with the given arguments without a shell, stdin/stdout/stderr on /dev/null, and
 * waits for it. Returns the exit status, or -1 when it could not be run. */
int run_argv_wait(char *const argv[]) {
    if (!argv || !argv[0]) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
            if (nul > 2) close(nul);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int is_proc_running(const char *proc_name) {
    static int s_rf_running = 0;
    static time_t s_rf_time = 0;
    static int s_bt_running = 0;
    static time_t s_bt_time = 0;
    time_t now = time(NULL);

    int is_rf = (strcmp(proc_name, "hub_rf") == 0);
    int is_bt = (strcmp(proc_name, "hub_btstack") == 0);

    if (is_rf && s_rf_time > 0 && (now - s_rf_time) < 2) return s_rf_running;
    if (is_bt && s_bt_time > 0 && (now - s_bt_time) < 2) return s_bt_running;

    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *de;
    int running = 0;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            char statline[128] = {0};
            if (fgets(statline, sizeof(statline), f)) {
                char *openp = strchr(statline, '(');
                char *closep = strrchr(statline, ')');
                if (openp && closep && closep > openp) {
                    *closep = '\0';
                    const char *comm = openp + 1;
                    if (strcmp(comm, proc_name) == 0 ||
                        (is_rf && strcmp(comm, "hub_rf") == 0) ||
                        (is_bt && strcmp(comm, "hub_btstack") == 0)) {
                        running = 1;
                        fclose(f);
                        break;
                    }
                }
            }
            fclose(f);
        }
    }
    closedir(d);

    if (is_rf) { s_rf_running = running; s_rf_time = now; }
    if (is_bt) { s_bt_running = running; s_bt_time = now; }
    return running;
}
