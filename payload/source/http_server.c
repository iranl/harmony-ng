#define _GNU_SOURCE
#include "http_server.h"
#include "ws_server.h"
#include "orchestrator.h"
#include "hw_action.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "resource_cache.h"
#include "config_store.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>

#define CODEX_VERSION "1.6.0"
#ifndef CODEX_COMMIT
#define CODEX_COMMIT "dev"
#endif
#define HTTP_REQ_BUF_SIZE 4096

#define MAX_PENDING_PULSES 8
struct pending_pulse {
    char topic[256];
    uint64_t off_deadline_ms;
    int active;
};
static struct pending_pulse g_pulses[MAX_PENDING_PULSES];

void pulse_tick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    for (int i = 0; i < MAX_PENDING_PULSES; i++) {
        if (g_pulses[i].active && now >= g_pulses[i].off_deadline_ms) {
            mqtt_publish(g_pulses[i].topic, "0", 0);
            g_pulses[i].active = 0;
        }
    }
}

static void get_local_ip(int sock_fd, char *buf, size_t max_len) {
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    if (getsockname(sock_fd, (struct sockaddr *)&sin, &len) == 0) {
        char *ip = inet_ntoa(sin.sin_addr);
        if (ip && strcmp(ip, "0.0.0.0") != 0) {
            strncpy(buf, ip, max_len - 1);
            buf[max_len - 1] = '\0';
            return;
        }
    }
    strncpy(buf, "127.0.0.1", max_len - 1);
    buf[max_len - 1] = '\0';
}

static void get_network_details(char *iface_out, size_t iface_len,
                                char *gw_out, size_t gw_len,
                                char *mask_out, size_t mask_len,
                                char *mac_out, size_t mac_len) {
    if (iface_out && iface_len) strncpy(iface_out, "ath0", iface_len - 1);
    if (gw_out && gw_len) strncpy(gw_out, "", gw_len - 1);
    if (mask_out && mask_len) strncpy(mask_out, "255.255.255.0", mask_len - 1);
    if (mac_out && mac_len) mac_out[0] = 0;

    FILE *f = fopen("/proc/net/route", "r");
    char default_iface[32] = {0};
    if (f) {
        char line[256];
        if (fgets(line, sizeof(line), f)) { // skip header
            while (fgets(line, sizeof(line), f)) {
                char ifname[32] = {0};
                unsigned long dest = 0, gw = 0, mask = 0;
                int flags = 0, ref = 0, use = 0, metric = 0, mtu = 0, win = 0, irtt = 0;
                if (sscanf(line, "%31s %lx %lx %x %d %d %d %lx %d %d %d",
                           ifname, &dest, &gw, &flags, &ref, &use, &metric, &mask, &mtu, &win, &irtt) >= 3) {
                    if (dest == 0) { // default route
                        strncpy(default_iface, ifname, sizeof(default_iface) - 1);
                        if (iface_out && iface_len) strncpy(iface_out, ifname, iface_len - 1);
                        if (gw_out && gw_len) {
                            unsigned char *b = (unsigned char *)&gw;
                            snprintf(gw_out, gw_len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                        }
                        if (mask_out && mask_len && mask != 0) {
                            unsigned char *m = (unsigned char *)&mask;
                            snprintf(mask_out, mask_len, "%u.%u.%u.%u", m[0], m[1], m[2], m[3]);
                        }
                        break;
                    }
                }
            }
        }
        fclose(f);
    }

    const char *active_if = default_iface[0] ? default_iface : "ath0";
    char mac_path[64];
    snprintf(mac_path, sizeof(mac_path), "/sys/class/net/%s/address", active_if);
    FILE *mf = fopen(mac_path, "r");
    if (!mf && strcmp(active_if, "ath0") != 0) {
        mf = fopen("/sys/class/net/ath0/address", "r");
    }
    if (mf) {
        if (fgets(mac_out, mac_len, mf)) {
            char *nl = strchr(mac_out, '\n'); if (nl) *nl = 0;
            nl = strchr(mac_out, '\r'); if (nl) *nl = 0;
        }
        fclose(mf);
    }
}

static void get_iface_ip_mask_mac(const char *ifname,
                                  char *ip_out, size_t ip_len,
                                  char *mask_out, size_t mask_len,
                                  char *mac_out, size_t mac_len) {
    if (ip_out && ip_len) ip_out[0] = '\0';
    if (mask_out && mask_len) mask_out[0] = '\0';
    if (mac_out && mac_len) mac_out[0] = '\0';

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);

    if (ip_out && ip_len && ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        strncpy(ip_out, inet_ntoa(sin->sin_addr), ip_len - 1);
    }
    if (mask_out && mask_len && ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_netmask;
        strncpy(mask_out, inet_ntoa(sin->sin_addr), mask_len - 1);
    }
    if (mac_out && mac_len && ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
        unsigned char *hw = (unsigned char *)ifr.ifr_hwaddr.sa_data;
        snprintf(mac_out, mac_len, "%02x:%02x:%02x:%02x:%02x:%02x",
                 hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
    }
    close(sock);
}

static int find_usb_eth_interface(char *ifname_out, size_t ifname_len) {
    DIR *d = opendir("/sys/class/net");
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (strcmp(de->d_name, "eth0") == 0 || strcmp(de->d_name, "eth1") == 0 ||
            strcmp(de->d_name, "lo") == 0 || strncmp(de->d_name, "ath", 3) == 0 ||
            strncmp(de->d_name, "wifi", 4) == 0) continue;
        if (strncmp(de->d_name, "eth", 3) == 0 || strncmp(de->d_name, "usb", 3) == 0) {
            if (ifname_out && ifname_len) {
                strncpy(ifname_out, de->d_name, ifname_len - 1);
                ifname_out[ifname_len - 1] = '\0';
            }
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int get_iface_carrier(const char *ifname) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ifname);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char c = fgetc(f);
    fclose(f);
    return (c == '1');
}

static int get_usb_host_mode(void) {
    return (access("/sys/bus/usb/devices/usb1", F_OK) == 0);
}

static void get_wifi_ssid(char *ssid_out, size_t ssid_len) {
    if (!ssid_out || !ssid_len) return;
    ssid_out[0] = '\0';
    FILE *f = popen("/usr/sbin/wpa_cli -i ath0 status 2>/dev/null", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "ssid=", 5) == 0) {
                char *s = line + 5;
                char *nl = strchr(s, '\n'); if (nl) *nl = '\0';
                nl = strchr(s, '\r'); if (nl) *nl = '\0';
                strncpy(ssid_out, s, ssid_len - 1);
                break;
            }
        }
        pclose(f);
    }
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

    return s;
}

void http_server_close(int server_fd) {
    if (server_fd >= 0) close(server_fd);
}

static void send_cjson_resp(int fd, int code, cJSON *obj) {
    const char *status_str = (code == 200) ? "OK" : (code == 400 ? "Bad Request" : (code == 404 ? "Not Found" : (code == 409 ? "Conflict" : (code == 503 ? "Service Unavailable" : "Internal Server Error"))));
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

static char *url_decode(const char *src) {
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

static cJSON *parse_request_params(const char *body) {
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

static int write_file_content(const char *filepath, const char *content) {
    if (!filepath || !content) return -1;
    FILE *f = fopen(filepath, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

static int write_file_atomic(const char *filepath, const char *content, size_t len) {
    if (!filepath || !content) return -1;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.new", filepath);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    if (len > 0) fwrite(content, 1, len, f);
    else fputs(content, f);
    fclose(f);
    rename(tmp, filepath);
    return 0;
}

static char *read_file_text_alloc(const char *path, size_t max_sz, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (max_sz > 0 && (size_t)sz > max_sz)) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

static char *b64_encode(const unsigned char *src, size_t len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!src || len == 0) return NULL;
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t a = src[i];
        uint32_t b = (i + 1 < len) ? src[i + 1] : 0;
        uint32_t c = (i + 2 < len) ? src[i + 2] : 0;
        uint32_t t = (a << 16) | (b << 8) | c;
        out[j++] = tbl[(t >> 18) & 0x3F];
        out[j++] = tbl[(t >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? tbl[(t >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? tbl[t & 0x3F] : '=';
    }
    out[j] = '\0';
    return out;
}

static unsigned char *b64_decode(const char *src, size_t *out_len) {
    if (!src) return NULL;
    size_t len = strlen(src);
    if (len % 4 != 0) return NULL;
    size_t pad = 0;
    if (len > 0 && src[len - 1] == '=') pad++;
    if (len > 1 && src[len - 2] == '=') pad++;
    size_t alloc_len = (len / 4) * 3 - pad;
    unsigned char *out = (unsigned char *)malloc(alloc_len + 1);
    if (!out) return NULL;

    static const int inv[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };

    size_t j = 0;
    for (size_t i = 0; i < len; i += 4) {
        int c0 = inv[(unsigned char)src[i]];
        int c1 = inv[(unsigned char)src[i+1]];
        int c2 = (src[i+2] == '=') ? 0 : inv[(unsigned char)src[i+2]];
        int c3 = (src[i+3] == '=') ? 0 : inv[(unsigned char)src[i+3]];
        if (c0 < 0 || c1 < 0) { free(out); return NULL; }
        uint32_t t = (c0 << 18) | (c1 << 12) | (c2 << 6) | c3;
        if (j < alloc_len) out[j++] = (t >> 16) & 0xFF;
        if (j < alloc_len && src[i+2] != '=') out[j++] = (t >> 8) & 0xFF;
        if (j < alloc_len && src[i+3] != '=') out[j++] = t & 0xFF;
    }
    if (out_len) *out_len = j;
    return out;
}

static uint32_t crc32_simple(const unsigned char *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

static cJSON *sanitize_activity_steps_array(const cJSON *arr) {
    cJSON *out = cJSON_CreateArray();
    if (!out) return NULL;
    if (!arr || !cJSON_IsArray(arr)) return out;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        cJSON *step = cJSON_CreateObject();
        const cJSON *stype = cJSON_GetObjectItemCaseSensitive(it, "type");
        if (!stype) stype = cJSON_GetObjectItemCaseSensitive(it, "Type");
        const cJSON *st_type = cJSON_GetObjectItemCaseSensitive(it, "__type");
        const char *tstr = (stype && stype->valuestring) ? stype->valuestring :
                           ((st_type && strstr(st_type->valuestring, "Delay")) ? "Delay" : "IRCommand");

        const cJSON *sdid = cJSON_GetObjectItemCaseSensitive(it, "deviceId");
        if (!sdid) sdid = cJSON_GetObjectItemCaseSensitive(it, "DeviceId-");
        if (!sdid) sdid = cJSON_GetObjectItemCaseSensitive(it, "DeviceId");
        char sd_str[32] = {0};
        if (sdid && cJSON_IsNumber(sdid)) snprintf(sd_str, sizeof(sd_str), "%ld", (long)sdid->valuedouble);
        else if (sdid && cJSON_IsString(sdid) && sdid->valuestring) strncpy(sd_str, sdid->valuestring, sizeof(sd_str) - 1);
        if (!sd_str[0]) strcpy(sd_str, "0");

        const cJSON *scmd = cJSON_GetObjectItemCaseSensitive(it, "command");
        if (!scmd) scmd = cJSON_GetObjectItemCaseSensitive(it, "CommandName");
        if (!scmd) scmd = cJSON_GetObjectItemCaseSensitive(it, "IRCommandName");
        if (!scmd) scmd = cJSON_GetObjectItemCaseSensitive(it, "Command");
        const char *cmd_str = (scmd && scmd->valuestring) ? scmd->valuestring : "";

        const cJSON *sdel = cJSON_GetObjectItemCaseSensitive(it, "delay");
        if (!sdel) sdel = cJSON_GetObjectItemCaseSensitive(it, "Duration");
        if (!sdel) sdel = cJSON_GetObjectItemCaseSensitive(it, "Delay");
        int del_val = (sdel && cJSON_IsNumber(sdel)) ? (int)sdel->valuedouble : 0;

        if (strcasecmp(tstr, "Delay") == 0) {
            int dms = del_val > 0 ? del_val : 1000;
            cJSON_AddStringToObject(step, "type", "Delay");
            cJSON_AddStringToObject(step, "deviceId", "0");
            cJSON_AddStringToObject(step, "command", "");
            cJSON_AddNumberToObject(step, "delay", dms);
        } else if (strcasecmp(tstr, "Switched") == 0 || strcasecmp(tstr, "ActivitySwitched") == 0 || strcasecmp(cmd_str, "Switched") == 0) {
            cJSON_AddStringToObject(step, "type", "Switched");
            cJSON_AddStringToObject(step, "deviceId", "0");
            cJSON_AddStringToObject(step, "command", "Switched");
            cJSON_AddNumberToObject(step, "delay", 0);
        } else if (strcasecmp(tstr, "BTCommand") == 0) {
            cJSON_AddStringToObject(step, "type", "BTCommand");
            cJSON_AddStringToObject(step, "deviceId", sd_str);
            cJSON_AddStringToObject(step, "command", cmd_str);
            cJSON_AddNumberToObject(step, "delay", del_val >= 0 ? del_val : 0);
        } else {
            cJSON_AddStringToObject(step, "type", "IRCommand");
            cJSON_AddStringToObject(step, "deviceId", sd_str);
            cJSON_AddStringToObject(step, "command", cmd_str);
            cJSON_AddNumberToObject(step, "delay", del_val >= 0 ? del_val : 0);
        }
        cJSON_AddItemToArray(out, step);
    }
    return out;
}

static cJSON *build_activity_steps_cjson(const char *seq_text) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    if (!seq_text || !seq_text[0]) return arr;

    const char *p = seq_text;
    char line[512];

    while (*p) {
        size_t l = 0;
        while (*p && *p != '\n' && *p != '\r' && l < sizeof(line) - 1) {
            line[l++] = *p++;
        }
        line[l] = '\0';
        while (*p == '\n' || *p == '\r') p++;
        if (!line[0]) continue;

        char type[32] = {0}, dev_id[32] = {0}, cmd[80] = {0}, del_str[32] = {0};
        char *f[4] = { type, dev_id, cmd, del_str };
        int fi = 0;
        char *lp = line;
        while (fi < 4 && *lp) {
            char *sep = strchr(lp, '|');
            if (sep) {
                *sep = '\0';
                strncpy(f[fi], lp, (fi == 2) ? 79 : 31);
                lp = sep + 1;
            } else {
                strncpy(f[fi], lp, (fi == 2) ? 79 : 31);
                break;
            }
            fi++;
        }

        if (strcasecmp(type, "Delay") == 0) {
            if (!del_str[0] && dev_id[0] && atoi(dev_id) > 0) {
                strncpy(del_str, dev_id, sizeof(del_str) - 1);
            } else if (!del_str[0] && cmd[0] && atoi(cmd) > 0) {
                strncpy(del_str, cmd, sizeof(del_str) - 1);
            }
        }
        int delay_ms = atoi(del_str);

        cJSON *step = cJSON_CreateObject();
        if (strcasecmp(type, "Delay") == 0) {
            int dms = delay_ms > 0 ? delay_ms : 1000;
            cJSON_AddStringToObject(step, "type", "Delay");
            cJSON_AddStringToObject(step, "deviceId", "0");
            cJSON_AddStringToObject(step, "command", "");
            cJSON_AddNumberToObject(step, "delay", dms);
        } else if (strcasecmp(type, "Switched") == 0 || strcasecmp(type, "ActivitySwitched") == 0 || strcasecmp(cmd, "Switched") == 0) {
            cJSON_AddStringToObject(step, "type", "Switched");
            cJSON_AddStringToObject(step, "deviceId", "0");
            cJSON_AddStringToObject(step, "command", "Switched");
            cJSON_AddNumberToObject(step, "delay", 0);
        } else if (strcasecmp(type, "BTCommand") == 0) {
            cJSON_AddStringToObject(step, "type", "BTCommand");
            cJSON_AddStringToObject(step, "deviceId", dev_id[0] ? dev_id : "0");
            cJSON_AddStringToObject(step, "command", cmd);
            cJSON_AddNumberToObject(step, "delay", delay_ms >= 0 ? delay_ms : 0);
        } else {
            cJSON_AddStringToObject(step, "type", "IRCommand");
            cJSON_AddStringToObject(step, "deviceId", dev_id);
            cJSON_AddStringToObject(step, "command", cmd);
            cJSON_AddNumberToObject(step, "delay", delay_ms >= 0 ? delay_ms : 0);
        }
        cJSON_AddItemToArray(arr, step);
    }
    return arr;
}

extern char g_mqtt_base_topic[128];

int mqtt_dispatch_button_pulse_topic(const char *dev_id, const char *dev_name_in, const char *cmd_name,
                                     const char *topic_in, int req_pulse_ms,
                                     char *out_topic, size_t out_topic_len, int *out_pulse_ms) {
    if (!cmd_name || !cmd_name[0]) return -1;

    char dev_name[128] = {0};
    if (dev_name_in && dev_name_in[0]) {
        strncpy(dev_name, dev_name_in, sizeof(dev_name) - 1);
    } else if (dev_id && dev_id[0]) {
        strncpy(dev_name, dev_id, sizeof(dev_name) - 1);
    } else {
        strcpy(dev_name, "device");
    }

    char base_topic[128] = "harmony/hub";
    if (g_mqtt_base_topic[0]) {
        strncpy(base_topic, g_mqtt_base_topic, sizeof(base_topic) - 1);
    }
    char topic_pattern[256] = "{root}/button/{device}/{command}";
    int pulse_ms = 1000;

    FILE *f = fopen("/data/codexmqtt/config.json", "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsz > 0 && fsz < 65536) {
            char *mcfg = malloc(fsz + 1);
            if (mcfg) {
                size_t nr = fread(mcfg, 1, fsz, f);
                mcfg[nr] = '\0';
                cJSON *cfg_j = cJSON_Parse(mcfg);
                free(mcfg);
                if (cfg_j) {
                    cJSON *bt = cJSON_GetObjectItemCaseSensitive(cfg_j, "baseTopic");
                    if (bt && cJSON_IsString(bt) && bt->valuestring && bt->valuestring[0]) {
                        strncpy(base_topic, bt->valuestring, sizeof(base_topic) - 1);
                    }
                    cJSON *def_p = cJSON_GetObjectItemCaseSensitive(cfg_j, "defaultPulseMs");
                    if (!def_p) def_p = cJSON_GetObjectItemCaseSensitive(cfg_j, "pulseMs");
                    if (def_p && cJSON_IsNumber(def_p) && def_p->valueint > 0) {
                        pulse_ms = def_p->valueint;
                    }
                    cJSON_Delete(cfg_j);
                }
            }
        }
        fclose(f);
    }

    if (topic_in && topic_in[0]) {
        strncpy(topic_pattern, topic_in, sizeof(topic_pattern) - 1);
    } else if (dev_id && dev_id[0]) {
        cJSON *droot = config_store_get_devices_json();
        if (droot) {
            cJSON *darr = cJSON_GetObjectItemCaseSensitive(droot, "devices");
            if (!darr) darr = cJSON_GetObjectItemCaseSensitive(droot, "DevicesWithFeatures");
            if (darr && cJSON_IsArray(darr)) {
                cJSON *di = NULL;
                cJSON_ArrayForEach(di, darr) {
                    cJSON *d = cJSON_GetObjectItemCaseSensitive(di, "Device");
                    if (!d) d = di;
                    cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                    if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                    char d_id_str[32] = {0};
                    if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                    else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                    if (strcmp(d_id_str, dev_id) == 0) {
                        cJSON *jtop = cJSON_GetObjectItemCaseSensitive(d, "mqttTopic");
                        if (!jtop) jtop = cJSON_GetObjectItemCaseSensitive(d, "MQTTTopic");
                        if (jtop && cJSON_IsString(jtop) && jtop->valuestring[0]) {
                            strncpy(topic_pattern, jtop->valuestring, sizeof(topic_pattern) - 1);
                        }
                        cJSON *jp = cJSON_GetObjectItemCaseSensitive(d, "mqttPulseMs");
                        if (!jp) jp = cJSON_GetObjectItemCaseSensitive(d, "MQTTPulseMs");
                        if (jp && cJSON_IsNumber(jp) && jp->valueint > 0) pulse_ms = jp->valueint;
                        break;
                    }
                }
            }
        }
    }

    if (req_pulse_ms > 0) {
        pulse_ms = req_pulse_ms;
    }

    char final_topic[256];
    char temp[256];
    strncpy(temp, topic_pattern, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    /* Replace {base_topic}, {base}, or {root} */
    char *bt_pos = strstr(temp, "{base_topic}");
    int bt_len = 12;
    if (!bt_pos) {
        bt_pos = strstr(temp, "{base}");
        bt_len = 6;
    }
    if (!bt_pos) {
        bt_pos = strstr(temp, "{root}");
        bt_len = 6;
    }
    if (bt_pos) {
        char buf[256];
        *bt_pos = '\0';
        snprintf(buf, sizeof(buf), "%s%s%s", temp, base_topic, bt_pos + bt_len);
        strncpy(temp, buf, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
    } else if (temp[0] == '/') {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s%s", base_topic, temp);
        strncpy(temp, buf, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
    }

    /* Replace {device} or {device_id} */
    char *d_pos = strstr(temp, "{device_id}");
    int d_len = 11;
    if (!d_pos) {
        d_pos = strstr(temp, "{device}");
        d_len = 8;
    }
    if (d_pos) {
        char buf[256];
        *d_pos = '\0';
        snprintf(buf, sizeof(buf), "%s%s%s", temp, dev_name, d_pos + d_len);
        strncpy(temp, buf, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
    }

    /* Replace {command} or {button} */
    char *c_pos = strstr(temp, "{command}");
    int c_len = 9;
    if (!c_pos) {
        c_pos = strstr(temp, "{button}");
        c_len = 8;
    }
    if (c_pos) {
        char buf[256];
        *c_pos = '\0';
        snprintf(buf, sizeof(buf), "%s%s%s", temp, cmd_name, c_pos + c_len);
        strncpy(final_topic, buf, sizeof(final_topic) - 1);
        final_topic[sizeof(final_topic) - 1] = '\0';
    } else {
        strncpy(final_topic, temp, sizeof(final_topic) - 1);
        final_topic[sizeof(final_topic) - 1] = '\0';
    }

    if (out_topic && out_topic_len > 0) {
        strncpy(out_topic, final_topic, out_topic_len - 1);
        out_topic[out_topic_len - 1] = '\0';
    }
    if (out_pulse_ms) {
        *out_pulse_ms = pulse_ms;
    }

    if (!mqtt_is_connected()) {
        return -1;
    }

    mqtt_publish(final_topic, "1", 0);
    int queued = 0;
    for (int i = 0; i < MAX_PENDING_PULSES; i++) {
        if (!g_pulses[i].active) {
            strncpy(g_pulses[i].topic, final_topic, sizeof(g_pulses[i].topic) - 1);
            g_pulses[i].topic[sizeof(g_pulses[i].topic) - 1] = '\0';
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            g_pulses[i].off_deadline_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000 + (pulse_ms > 0 ? pulse_ms : 50);
            g_pulses[i].active = 1;
            queued = 1;
            break;
        }
    }
    if (!queued) {
        mqtt_publish(final_topic, "0", 0);
    }
    return 0;
}

int mqtt_dispatch_button_pulse(const char *dev_id, const char *dev_name_in, const char *cmd_name,
                               int req_pulse_ms, char *out_topic, size_t out_topic_len, int *out_pulse_ms) {
    return mqtt_dispatch_button_pulse_topic(dev_id, dev_name_in, cmd_name, NULL, req_pulse_ms,
                                            out_topic, out_topic_len, out_pulse_ms);
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
    return "application/octet-stream";
}

static void ensure_icons_extracted(void) {
    if (access("/tmp/www/assets/icons", F_OK) == 0) return;

    const char *tar_path = NULL;
    if (access("/data/codex/icons.tar.gz", F_OK) == 0) {
        tar_path = "/data/codex/icons.tar.gz";
    } else if (access("/cache/icons.tar.gz", F_OK) == 0) {
        tar_path = "/cache/icons.tar.gz";
    } else if (access("/cache/www/assets/icons.tar.gz", F_OK) == 0) {
        tar_path = "/cache/www/assets/icons.tar.gz";
    }
    if (tar_path) {
        mkdir("/tmp/www", 0755);
        mkdir("/tmp/www/assets", 0755);
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "tar -xzf \"%s\" -C /tmp/www/assets/ 2>/dev/null && "
            "for d in /tmp/www/assets/icons/*/; do "
            "  if [ -d \"$d\" ]; then "
            "    for f in \"$d\"*.png; do [ -f \"$f\" ] && ln -sf \"$f\" /tmp/www/assets/icons/ 2>/dev/null; done; "
            "  fi; "
            "done",
            tar_path);
        system(cmd);
    }
}

static int serve_static_file(int client_fd, const char *req, const char *path) {
    if (strstr(path, "..")) return 0;

    char rel_path[256];
    if (strcmp(path, "/") == 0 || path[0] == '\0') {
        strcpy(rel_path, "/index.html");
    } else {
        strncpy(rel_path, path, sizeof(rel_path) - 1);
        rel_path[sizeof(rel_path) - 1] = '\0';
    }

    if (strncmp(rel_path, "/assets/icons/", 14) == 0) {
        ensure_icons_extracted();
    }

    char file_path[512];
    snprintf(file_path, sizeof(file_path), "/tmp/www%s", rel_path);
    if (access(file_path, F_OK) != 0) {
        snprintf(file_path, sizeof(file_path), "/data/codex/www%s", rel_path);
        if (access(file_path, F_OK) != 0) {
            snprintf(file_path, sizeof(file_path), "/cache/www%s", rel_path);
        }
    }

    int client_gzip = (strstr(req, "Accept-Encoding") && strstr(req, "gzip")) ? 1 : 0;
    int is_gzip = 0;
    FILE *f = NULL;
    struct stat st_raw, st_gz;
    int has_raw = (stat(file_path, &st_raw) == 0);

    if (client_gzip || !has_raw) {
        char gz_path[520];
        snprintf(gz_path, sizeof(gz_path), "%s.gz", file_path);
        if (stat(gz_path, &st_gz) == 0) {
            if (!has_raw || st_gz.st_mtime >= st_raw.st_mtime) {
                f = fopen(gz_path, "rb");
                if (f) is_gzip = 1;
            }
        }
    }

    if (!f && has_raw) {
        f = fopen(file_path, "rb");
    }

    if (!f && !strchr(rel_path, '.')) {
        const char *idx_raw = "/data/codex/www/index.html";
        const char *idx_gz = "/data/codex/www/index.html.gz";
        if (access(idx_raw, F_OK) != 0 && access(idx_gz, F_OK) != 0) {
            idx_raw = "/cache/www/index.html";
            idx_gz = "/cache/www/index.html.gz";
        }
        int has_idx_raw = (stat(idx_raw, &st_raw) == 0);
        int has_idx_gz = (stat(idx_gz, &st_gz) == 0);

        if (client_gzip || !has_idx_raw) {
            if (stat(idx_gz, &st_gz) == 0) {
                if (!has_idx_raw || st_gz.st_mtime >= st_raw.st_mtime) {
                    f = fopen(idx_gz, "rb");
                    if (f) is_gzip = 1;
                }
            }
        }
        if (!f && has_idx_raw) {
            f = fopen(idx_raw, "rb");
        }
        if (f) {
            strcpy(rel_path, "/index.html");
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
        "Connection: close\r\n\r\n",
        mime, fsize,
        is_gzip ? "Content-Encoding: gzip\r\n" : "",
        cache_hdr);

    send(client_fd, header, (size_t)hlen, 0);

    if (strncmp(req, "HEAD ", 5) != 0) {
        char chunk[4096];
        size_t nr;
        while ((nr = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            if (send(client_fd, chunk, nr, 0) < 0) break;
        }
    }
    fclose(f);
    return 1;
}

static void apply_hub_settings(const struct codex_settings *s) {
    if (!s) return;
    /* 1. Hub LED */
    int lfd = open("/sys/class/input/input0/led", O_WRONLY);
    if (lfd >= 0) {
        const char *val = "2\n";
        if (strcasecmp(s->led_mode, "always_off") == 0 || strcasecmp(s->led_mode, "off") == 0) {
            val = "0\n";
        } else if (strcasecmp(s->led_mode, "red") == 0) {
            val = "1\n";
        } else if (strcasecmp(s->led_mode, "amber") == 0 || strcasecmp(s->led_mode, "orange") == 0) {
            val = "3\n";
        } else {
            val = "2\n"; /* Solid Green */
        }
        ssize_t w = write(lfd, val, strlen(val));
        (void)w;
        close(lfd);
    }
    /* 2. Timezone */
    if (s->timezone[0]) {
        FILE *tzf = fopen("/etc/TZ", "w");
        if (tzf) {
            fprintf(tzf, "%s\n", s->timezone);
            fclose(tzf);
        }
        setenv("TZ", s->timezone, 1);
        tzset();
    }
    /* 3. NTP server */
    if (s->ntp_server[0] && !g_dry_run) {
        FILE *ntpf = fopen("/etc/ntp.conf", "w");
        if (ntpf) {
            fprintf(ntpf, "server %s\n", s->ntp_server);
            fclose(ntpf);
        }
    }
}

static void http_process_request(int client_fd, const char *req);

void http_handle_client(int client_fd) {
    if (client_fd < 0) return;

    char init_buf[HTTP_REQ_BUF_SIZE] = {0};
    ssize_t n = recv(client_fd, init_buf, sizeof(init_buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    init_buf[n] = '\0';

    /* Check if WebSocket upgrade */
    if (ws_is_upgrade_req(init_buf)) {
        if (ws_handle_upgrade(client_fd, init_buf) != 0) {
            close(client_fd);
        }
        return; /* Keep client_fd open for WebSocket frames */
    }

    const char *req = init_buf;
    char *dyn_buf = NULL;

    /* Check Content-Length for large bodies (e.g. backup restore) */
    char *hdr_end = strstr(init_buf, "\r\n\r\n");
    if (hdr_end) {
        char *cl_pos = strstr(init_buf, "Content-Length:");
        if (!cl_pos) cl_pos = strstr(init_buf, "content-length:");
        if (cl_pos && cl_pos < hdr_end) {
            long cl = atol(cl_pos + 15);
            size_t hdr_len = (size_t)(hdr_end + 4 - init_buf);
            size_t body_recvd = (size_t)n - hdr_len;
            if (cl > 0 && cl < 2 * 1024 * 1024 && (size_t)cl > body_recvd) {
                size_t total_needed = hdr_len + (size_t)cl;
                dyn_buf = (char *)malloc(total_needed + 1);
                if (dyn_buf) {
                    memcpy(dyn_buf, init_buf, (size_t)n);
                    size_t total_read = (size_t)n;
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

    http_process_request(client_fd, req);
    if (dyn_buf) free(dyn_buf);
}

static void http_process_request(int client_fd, const char *req) {
    char method[16] = {0}, path[128] = {0};
    sscanf(req, "%15s %127s", method, path);

    char clean_path[128] = {0};
    strncpy(clean_path, path, sizeof(clean_path) - 1);
    char *qmark = strchr(clean_path, '?');
    if (qmark) *qmark = '\0';

    const char *body = strstr(req, "\r\n\r\n");
    if (body) body += 4;

    /* Static file server: if GET or HEAD and not an /api/ or /ws route, serve from /data/codex/www */
    if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) && strncmp(clean_path, "/api/", 5) != 0 && strcmp(clean_path, "/ws") != 0 && strcmp(clean_path, "/health") != 0 && strncmp(clean_path, "/export/", 8) != 0) {
        if (serve_static_file(client_fd, req, clean_path)) {
            close(client_fd);
            return;
        }
    }

    /* Route: GET /api/status */
    if (strcmp(clean_path, "/api/status") == 0 && strcmp(method, "GET") == 0) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        struct codex_settings cs;
        config_store_get_settings(&cs);
        cJSON_AddStringToObject(st, "hub_name", cs.hub_name[0] ? cs.hub_name : "Harmony Hub");
        cJSON_AddStringToObject(st, "firmware", "4.15.600");
        cJSON_AddStringToObject(st, "activity", orch_get_current_activity());
        cJSON_AddNumberToObject(st, "state", (int)orch_get_state());
        cJSON_AddBoolToObject(st, "busy", orch_is_busy() ? 1 : 0);
        char local_ip[64];
        get_local_ip(client_fd, local_ip, sizeof(local_ip));
        cJSON_AddStringToObject(st, "ip", local_ip);
        cJSON_AddNumberToObject(st, "total_kb", 5120);
        cJSON_AddNumberToObject(st, "free_kb", 1372);

        FILE *uf = fopen("/proc/uptime", "r");
        if (uf) {
            double up_sec = 0;
            if (fscanf(uf, "%lf", &up_sec) == 1) {
                cJSON_AddNumberToObject(st, "uptime_sec", (long)up_sec);
            }
            fclose(uf);
        }

        char mhost[128] = {0}, mbase[128] = {0};
        int mport = 1883, mconn = 0;
        daemon_get_mqtt_info(mhost, sizeof(mhost), &mport, mbase, sizeof(mbase), &mconn);

        cJSON *mj = cJSON_CreateObject();
        cJSON_AddBoolToObject(mj, "connected", mconn);
        cJSON_AddStringToObject(mj, "broker", mhost);
        cJSON_AddNumberToObject(mj, "port", mport);
        cJSON_AddStringToObject(mj, "baseTopic", mbase);
        cJSON_AddItemToObject(st, "mqtt", mj);

        ensure_icons_extracted();
        int icons_present = 0;
        struct stat st_icon;
        const char *icon_dir = "/tmp/www/assets/icons";
        if (stat(icon_dir, &st_icon) != 0) {
            icon_dir = "/cache/www/assets/icons";
        }
        if (stat(icon_dir, &st_icon) == 0 && S_ISDIR(st_icon.st_mode)) {
            DIR *d = opendir(icon_dir);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (de->d_name[0] != '.') {
                        icons_present = 1;
                        break;
                    }
                }
                closedir(d);
            }
        }
        cJSON_AddBoolToObject(st, "has_icons", icons_present);

        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    /* Compatibility Routes */
    if (strcmp(clean_path, "/api/mqtt-status") == 0 && strcmp(method, "GET") == 0) {
        char mhost[128] = {0}, mbase[128] = {0};
        int mport = 1883, mconn = 0;
        daemon_get_mqtt_info(mhost, sizeof(mhost), &mport, mbase, sizeof(mbase), &mconn);

        char user[128] = {0}, cid[64] = {0}, disc[128] = {0}, name[128] = {0};
        int en = mconn || (mhost[0] != '\0');
        int keepalive = 60, ha_disc = 1, bt_events = 0;

        FILE *f = fopen("/data/codexmqtt/config.json", "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 65536) {
                char *b = (char *)malloc((size_t)sz + 1);
                if (b) {
                    size_t nr = fread(b, 1, (size_t)sz, f);
                    b[nr] = '\0';
                    cJSON *cfg = cJSON_Parse(b);
                    free(b);
                    if (cfg) {
                        cJSON *broker = cJSON_GetObjectItemCaseSensitive(cfg, "broker");
                        cJSON *j_en = cJSON_GetObjectItemCaseSensitive(cfg, "enabled");
                        if (j_en) en = cJSON_IsTrue(j_en) || (cJSON_IsNumber(j_en) && j_en->valueint > 0);

                        cJSON *j_host = cJSON_GetObjectItemCaseSensitive(cfg, "host");
                        if (!j_host && broker) j_host = cJSON_GetObjectItemCaseSensitive(broker, "host");
                        if (j_host && cJSON_IsString(j_host) && j_host->valuestring && mhost[0] == '\0') {
                            strncpy(mhost, j_host->valuestring, sizeof(mhost) - 1);
                        }

                        cJSON *j_port = cJSON_GetObjectItemCaseSensitive(cfg, "port");
                        if (!j_port && broker) j_port = cJSON_GetObjectItemCaseSensitive(broker, "port");
                        if (j_port && cJSON_IsNumber(j_port) && mport == 1883) {
                            mport = (int)j_port->valuedouble;
                        }

                        cJSON *j_user = cJSON_GetObjectItemCaseSensitive(cfg, "username");
                        if (!j_user && broker) j_user = cJSON_GetObjectItemCaseSensitive(broker, "username");
                        if (j_user && cJSON_IsString(j_user) && j_user->valuestring) {
                            strncpy(user, j_user->valuestring, sizeof(user) - 1);
                        }

                        cJSON *j_cid = cJSON_GetObjectItemCaseSensitive(cfg, "clientId");
                        if (!j_cid && broker) j_cid = cJSON_GetObjectItemCaseSensitive(broker, "clientId");
                        if (j_cid && cJSON_IsString(j_cid) && j_cid->valuestring) {
                            strncpy(cid, j_cid->valuestring, sizeof(cid) - 1);
                        }

                        cJSON *j_disc = cJSON_GetObjectItemCaseSensitive(cfg, "discoveryPrefix");
                        if (!j_disc && broker) j_disc = cJSON_GetObjectItemCaseSensitive(broker, "discoveryPrefix");
                        if (j_disc && cJSON_IsString(j_disc) && j_disc->valuestring) {
                            strncpy(disc, j_disc->valuestring, sizeof(disc) - 1);
                        }

                        cJSON *j_name = cJSON_GetObjectItemCaseSensitive(cfg, "name");
                        if (j_name && cJSON_IsString(j_name) && j_name->valuestring) {
                            strncpy(name, j_name->valuestring, sizeof(name) - 1);
                        }

                        cJSON *j_ka = cJSON_GetObjectItemCaseSensitive(cfg, "keepAlive");
                        if (j_ka && cJSON_IsNumber(j_ka)) keepalive = j_ka->valueint;

                        cJSON *j_ha = cJSON_GetObjectItemCaseSensitive(cfg, "haDiscovery");
                        if (j_ha) ha_disc = cJSON_IsTrue(j_ha) || (cJSON_IsNumber(j_ha) && j_ha->valueint > 0);

                        cJSON *j_btr = cJSON_GetObjectItemCaseSensitive(cfg, "btRemoteEvents");
                        if (j_btr) bt_events = cJSON_IsTrue(j_btr) || (cJSON_IsNumber(j_btr) && j_btr->valueint > 0);

                        cJSON_Delete(cfg);
                    }
                }
            }
            fclose(f);
        }

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "connected", mconn);
        cJSON_AddBoolToObject(st, "enabled", en);
        cJSON_AddStringToObject(st, "broker", mhost);
        cJSON_AddStringToObject(st, "host", mhost);
        cJSON_AddNumberToObject(st, "port", mport);
        cJSON_AddStringToObject(st, "username", user);
        cJSON_AddStringToObject(st, "baseTopic", mbase[0] ? mbase : "harmony/hub");
        cJSON_AddStringToObject(st, "clientId", cid[0] ? cid : "harmony_hub");
        cJSON_AddStringToObject(st, "discoveryPrefix", disc[0] ? disc : "homeassistant");
        cJSON_AddStringToObject(st, "name", name[0] ? name : "Harmony Hub");
        cJSON_AddNumberToObject(st, "keepAlive", keepalive > 0 ? keepalive : 60);
        cJSON_AddBoolToObject(st, "haDiscovery", ha_disc);
        cJSON_AddBoolToObject(st, "btRemoteEvents", bt_events);

        cJSON *cfg_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(cfg_obj, "enabled", en);
        cJSON_AddStringToObject(cfg_obj, "host", mhost);
        cJSON_AddNumberToObject(cfg_obj, "port", mport);
        cJSON_AddStringToObject(cfg_obj, "username", user);
        cJSON_AddStringToObject(cfg_obj, "baseTopic", mbase[0] ? mbase : "harmony/hub");
        cJSON_AddStringToObject(cfg_obj, "clientId", cid[0] ? cid : "harmony_hub");
        cJSON_AddStringToObject(cfg_obj, "discoveryPrefix", disc[0] ? disc : "homeassistant");
        cJSON_AddStringToObject(cfg_obj, "name", name[0] ? name : "Harmony Hub");
        cJSON_AddNumberToObject(cfg_obj, "keepAlive", keepalive > 0 ? keepalive : 60);
        cJSON_AddBoolToObject(cfg_obj, "haDiscovery", ha_disc);
        cJSON_AddBoolToObject(cfg_obj, "btRemoteEvents", bt_events);
        cJSON_AddItemToObject(st, "config", cfg_obj);

        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    if (strcmp(clean_path, "/api/icons") == 0 && strcmp(method, "GET") == 0) {
        ensure_icons_extracted();
        cJSON *arr = cJSON_CreateArray();
        const char *base_dir = "/tmp/www/assets/icons";
        if (access(base_dir, F_OK) != 0) {
            base_dir = "/cache/www/assets/icons";
            if (access(base_dir, F_OK) != 0) {
                base_dir = "/data/codex/www/assets/icons";
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
        send_cjson_resp(client_fd, 200, arr);
        cJSON_Delete(arr);
        close(client_fd);
        return;
    }

    if (strcmp(clean_path, "/api/bt-status") == 0 && strcmp(method, "GET") == 0) {
        FILE *bf = fopen("/tmp/codex_btstack_status.json", "r");
        if (bf) {
            fseek(bf, 0, SEEK_END);
            long sz = ftell(bf);
            fseek(bf, 0, SEEK_SET);
            if (sz > 0 && sz < 32768) {
                char *buf = (char *)malloc((size_t)sz + 1);
                if (buf) {
                    size_t rd = fread(buf, 1, (size_t)sz, bf);
                    buf[rd] = '\0';
                    fclose(bf);
                    cJSON *bj = cJSON_Parse(buf);
                    free(buf);
                    if (bj) {
                        send_cjson_resp(client_fd, 200, bj);
                        cJSON_Delete(bj);
                        close(client_fd);
                        return;
                    }
                } else {
                    fclose(bf);
                }
            } else {
                fclose(bf);
            }
        }
        int bt_running = (access("/tmp/codex_btstack.pid", F_OK) == 0 || system("pidof codex_btstack >/dev/null 2>&1") == 0);
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddBoolToObject(st, "running", bt_running);
        cJSON_AddStringToObject(st, "state", bt_running ? "running" : "stopped");
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    /* Route: GET /api/services */
    if (strcmp(clean_path, "/api/services") == 0 && strcmp(method, "GET") == 0) {
        int rf_disabled = (access("/data/codex/rf_disabled.conf", F_OK) == 0);
        int rf_running = (system("pidof codex_rf >/dev/null 2>&1") == 0);
        int bt_running = (access("/tmp/codex_btstack.pid", F_OK) == 0 || system("pidof codex_btstack >/dev/null 2>&1") == 0);

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddBoolToObject(st, "rf_enabled", !rf_disabled);
        cJSON_AddBoolToObject(st, "rf_running", rf_running);
        cJSON_AddBoolToObject(st, "bt_enabled", bt_running);
        cJSON_AddBoolToObject(st, "bt_running", bt_running);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    /* Route: POST /api/services/rf-toggle */
    if ((strcmp(clean_path, "/api/services/rf-toggle") == 0 || strcmp(clean_path, "/api/rf-toggle") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int enable = 0;
        if (params) {
            cJSON *item = cJSON_GetObjectItem(params, "enable");
            if (item) {
                if (cJSON_IsBool(item)) enable = cJSON_IsTrue(item);
                else if (cJSON_IsNumber(item)) enable = item->valueint;
                else if (cJSON_IsString(item)) enable = (atoi(item->valuestring) != 0 || strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0);
            }
            cJSON_Delete(params);
        }
        if (enable) {
            unlink("/data/codex/rf_disabled.conf");
            system("killall -9 codex_rf 2>/dev/null; /data/codex/bin/codex_rf &");
        } else {
            write_file_content("/data/codex/rf_disabled.conf", "1\n");
            system("killall -9 codex_rf 2>/dev/null");
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddBoolToObject(st, "rf_enabled", enable);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    /* Route: POST /api/services/bt-toggle */
    if ((strcmp(clean_path, "/api/services/bt-toggle") == 0 || strcmp(clean_path, "/api/bt-toggle") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int enable = 0;
        if (params) {
            cJSON *item = cJSON_GetObjectItem(params, "enable");
            if (item) {
                if (cJSON_IsBool(item)) enable = cJSON_IsTrue(item);
                else if (cJSON_IsNumber(item)) enable = item->valueint;
                else if (cJSON_IsString(item)) enable = (atoi(item->valuestring) != 0 || strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0);
            }
            cJSON_Delete(params);
        }
        if (enable) {
            system("killall -9 codex_btstack 2>/dev/null; /data/codex/bin/codex_btstack &");
        } else {
            system("killall -9 codex_btstack 2>/dev/null");
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddBoolToObject(st, "bt_running", enable);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    /* Route: POST /api/bt-pairing */
    if (strcmp(clean_path, "/api/bt-pairing") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int enable = 0;
        char name[64] = "Harmony Keyboard";
        char target_dev[64] = {0};
        if (params) {
            cJSON *je = cJSON_GetObjectItem(params, "enable");
            if (je) {
                if (cJSON_IsBool(je)) enable = cJSON_IsTrue(je);
                else if (cJSON_IsNumber(je)) enable = je->valueint;
                else if (cJSON_IsString(je)) enable = (atoi(je->valuestring) != 0 || strcmp(je->valuestring, "true") == 0 || strcmp(je->valuestring, "1") == 0);
            }
            cJSON *jn = cJSON_GetObjectItem(params, "name");
            if (jn && cJSON_IsString(jn) && jn->valuestring[0]) {
                strncpy(name, jn->valuestring, sizeof(name) - 1);
            }
            cJSON *jt = cJSON_GetObjectItem(params, "targetDev");
            if (!jt) jt = cJSON_GetObjectItem(params, "deviceId");
            if (jt) {
                if (cJSON_IsString(jt) && jt->valuestring[0]) strncpy(target_dev, jt->valuestring, sizeof(target_dev) - 1);
                else if (cJSON_IsNumber(jt)) snprintf(target_dev, sizeof(target_dev), "%ld", (long)jt->valuedouble);
            }
            cJSON_Delete(params);
        }
        if (enable && !target_dev[0]) {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddBoolToObject(err, "ok", 0);
            cJSON_AddStringToObject(err, "error", "Target device is required for Bluetooth pairing");
            send_cjson_resp(client_fd, 400, err);
            cJSON_Delete(err);
            close(client_fd);
            return;
        }
        if (enable && target_dev[0]) {
            FILE *pf = fopen("/tmp/bt_pending_pair_device", "w");
            if (pf) {
                fprintf(pf, "%s\n", target_dev);
                fclose(pf);
            }
        } else if (!enable) {
            unlink("/tmp/bt_pending_pair_device");
        }
        int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
        if (bfd >= 0) {
            char pipe_cmd[128];
            if (enable) snprintf(pipe_cmd, sizeof(pipe_cmd), "pair_host_on %s\n", name);
            else snprintf(pipe_cmd, sizeof(pipe_cmd), "pair_host_off\n");
            write(bfd, pipe_cmd, strlen(pipe_cmd));
            close(bfd);
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddBoolToObject(st, "pairing", enable);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    /* Route: POST /api/bt-connect */
    if (strcmp(clean_path, "/api/bt-connect") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        char bdaddr[32] = {0};
        if (params) {
            cJSON *ja = cJSON_GetObjectItem(params, "bdaddr");
            if (ja && cJSON_IsString(ja)) strncpy(bdaddr, ja->valuestring, sizeof(bdaddr) - 1);
            cJSON_Delete(params);
        }
        int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
        if (bfd >= 0) {
            char pipe_cmd[128];
            if (bdaddr[0]) snprintf(pipe_cmd, sizeof(pipe_cmd), "connect_host %s\n", bdaddr);
            else snprintf(pipe_cmd, sizeof(pipe_cmd), "connect_host\n");
            write(bfd, pipe_cmd, strlen(pipe_cmd));
            close(bfd);
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    /* Route: POST /api/bt-disconnect */
    if (strcmp(clean_path, "/api/bt-disconnect") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        char bdaddr[32] = {0};
        if (params) {
            cJSON *ja = cJSON_GetObjectItem(params, "bdaddr");
            if (ja && cJSON_IsString(ja)) strncpy(bdaddr, ja->valuestring, sizeof(bdaddr) - 1);
            cJSON_Delete(params);
        }
        int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
        if (bfd >= 0) {
            char pipe_cmd[128];
            if (bdaddr[0]) snprintf(pipe_cmd, sizeof(pipe_cmd), "disconnect_host %s\n", bdaddr);
            else snprintf(pipe_cmd, sizeof(pipe_cmd), "disconnect_host\n");
            write(bfd, pipe_cmd, strlen(pipe_cmd));
            close(bfd);
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    /* Route: POST /api/bt-link-device */
    if (strcmp(clean_path, "/api/bt-link-device") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        char dev_id[64] = {0};
        char bdaddr[32] = {0};
        if (params) {
            cJSON *jd = cJSON_GetObjectItem(params, "deviceId");
            if (!jd) jd = cJSON_GetObjectItem(params, "id");
            if (jd) {
                if (cJSON_IsString(jd)) strncpy(dev_id, jd->valuestring, sizeof(dev_id) - 1);
                else if (cJSON_IsNumber(jd)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jd->valuedouble);
            }
            cJSON *ja = cJSON_GetObjectItem(params, "bdaddr");
            if (ja && cJSON_IsString(ja)) strncpy(bdaddr, ja->valuestring, sizeof(bdaddr) - 1);
            cJSON_Delete(params);
        }
        int success = 0;
        if (dev_id[0]) {
            cJSON *root = config_store_get_devices_json();
            if (root) {
                cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                if (devs && cJSON_IsArray(devs)) {
                    cJSON *dev_item = NULL;
                    cJSON_ArrayForEach(dev_item, devs) {
                        cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
                        if (!d) d = dev_item;
                        cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                        char d_id_str[32] = {0};
                        if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                        else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                        if (strcmp(d_id_str, dev_id) == 0) {
                            cJSON_DeleteItemFromObjectCaseSensitive(d, "BTAddress");
                            cJSON_DeleteItemFromObjectCaseSensitive(d, "bt_host");
                            cJSON_DeleteItemFromObjectCaseSensitive(d, "btHost");
                            if (bdaddr[0]) {
                                cJSON_AddStringToObject(d, "BTAddress", bdaddr);
                                cJSON_AddStringToObject(d, "bt_host", bdaddr);
                            }
                            config_store_save_devices(root);
                            success = 1;
                            break;
                        }
                    }
                }
            }
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", success);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }


    /* Route: GET /api/logs */
    if (strcmp(clean_path, "/api/logs") == 0 && strcmp(method, "GET") == 0) {
        const char *src = "daemon";
        if (strstr(path, "source=btstack")) src = "btstack";
        else if (strstr(path, "source=rf")) src = "rf";
        else if (strstr(path, "source=system")) src = "system";
        else if (strstr(path, "source=network")) src = "network";

        const char *filepath = "/tmp/codex_daemon.log";
        if (strcmp(src, "btstack") == 0) filepath = "/tmp/codex-btstack.log";
        else if (strcmp(src, "rf") == 0) filepath = "/data/codex/codex_rf.log";
        else if (strcmp(src, "network") == 0) filepath = "/tmp/codex-network.log";

        char log_buf[16384] = {0};
        if (strcmp(src, "system") == 0) {
            FILE *p = popen("logread 2>/dev/null", "r");
            if (!p) p = popen("dmesg 2>/dev/null", "r");
            if (p) {
                size_t rd = fread(log_buf, 1, sizeof(log_buf) - 1, p);
                log_buf[rd] = '\0';
                pclose(p);
            }
        } else {
            FILE *f = fopen(filepath, "r");
            if (!f && strcmp(src, "rf") == 0) f = fopen("/tmp/codex_rf.log", "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                long off = (sz > 12288) ? (sz - 12288) : 0;
                fseek(f, off, SEEK_SET);
                size_t rd = fread(log_buf, 1, sizeof(log_buf) - 1, f);
                log_buf[rd] = '\0';
                fclose(f);
            } else {
                snprintf(log_buf, sizeof(log_buf), "Log file %s is empty or not yet created.", filepath);
            }
        }

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddStringToObject(st, "source", src);
        cJSON_AddStringToObject(st, "log", log_buf);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    /* Route: GET /api/system-info */
    if (strcmp(clean_path, "/api/system-info") == 0 && strcmp(method, "GET") == 0) {
        char uptime_str[128] = "Unknown";
        FILE *uf = fopen("/proc/uptime", "r");
        if (uf) {
            double up_sec = 0;
            if (fscanf(uf, "%lf", &up_sec) == 1) {
                long s = (long)up_sec;
                long d = s / 86400;
                long h = (s % 86400) / 3600;
                long m = (s % 3600) / 60;
                snprintf(uptime_str, sizeof(uptime_str), "%ldd %ldh %ldm (%lds)", d, h, m, s);
            }
            fclose(uf);
        }

        char load_str[64] = "N/A";
        FILE *lf = fopen("/proc/loadavg", "r");
        if (lf) {
            if (fgets(load_str, sizeof(load_str), lf)) {
                char *nl = strchr(load_str, '\n');
                if (nl) *nl = '\0';
            }
            fclose(lf);
        }

        char kernel_str[256] = "Linux mips (Atheros AR9344)";
        FILE *kf = fopen("/proc/version", "r");
        if (kf) {
            if (fgets(kernel_str, sizeof(kernel_str), kf)) {
                char *nl = strchr(kernel_str, '\n');
                if (nl) *nl = '\0';
            }
            fclose(kf);
        }

        long total = 0, free_mem = 0, buffers = 0, cached = 0;
        FILE *mf = fopen("/proc/meminfo", "r");
        if (mf) {
            char line[128];
            while (fgets(line, sizeof(line), mf)) {
                long val = 0;
                if (sscanf(line, "MemTotal: %ld kB", &val) == 1) total = val;
                else if (sscanf(line, "MemFree: %ld kB", &val) == 1) free_mem = val;
                else if (sscanf(line, "Buffers: %ld kB", &val) == 1) buffers = val;
                else if (sscanf(line, "Cached: %ld kB", &val) == 1) cached = val;
            }
            fclose(mf);
        }
        long avail = free_mem + buffers + cached;
        char mem_str[256];
        snprintf(mem_str, sizeof(mem_str), "Total: %ld MB, Free: %ld MB (Avail: %ld MB)",
                 total / 1024, free_mem / 1024, avail / 1024);

        /* Read network details from /proc/net/dev and /proc/net/route */
        char net_details[4096] = "=== Network Interfaces (/proc/net/dev) ===\n";
        FILE *ndf = fopen("/proc/net/dev", "r");
        if (ndf) {
            size_t cur_len = strlen(net_details);
            size_t r = fread(net_details + cur_len, 1, sizeof(net_details) - cur_len - 512, ndf);
            net_details[cur_len + r] = '\0';
            fclose(ndf);
        }
        strncat(net_details, "\n=== Routing Table (/proc/net/route) ===\n", sizeof(net_details) - strlen(net_details) - 1);
        FILE *nrf = fopen("/proc/net/route", "r");
        if (nrf) {
            size_t cur_len = strlen(net_details);
            size_t r = fread(net_details + cur_len, 1, sizeof(net_details) - cur_len - 1, nrf);
            net_details[cur_len + r] = '\0';
            fclose(nrf);
        }

        char fw_str[128] = "4.15.600";
        FILE *vf = fopen("/etc/version", "r");
        if (vf) {
            if (fgets(fw_str, sizeof(fw_str), vf)) {
                char *nl = strchr(fw_str, '\n');
                if (nl) *nl = '\0';
                nl = strchr(fw_str, '\r');
                if (nl) *nl = '\0';
            }
            fclose(vf);
        }

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        struct codex_settings cs;
        config_store_get_settings(&cs);
        cJSON_AddStringToObject(st, "hub_name", cs.hub_name[0] ? cs.hub_name : "Harmony Hub");
        cJSON_AddStringToObject(st, "uptime", uptime_str);
        cJSON_AddStringToObject(st, "loadavg", load_str);
        cJSON_AddStringToObject(st, "kernel", kernel_str);
        cJSON_AddStringToObject(st, "firmware", fw_str);
        cJSON_AddStringToObject(st, "version", CODEX_VERSION);
        cJSON_AddStringToObject(st, "commit", CODEX_COMMIT);
        cJSON_AddStringToObject(st, "build_time", __DATE__ " " __TIME__);
        cJSON_AddStringToObject(st, "memory", mem_str);
        cJSON_AddNumberToObject(st, "mem_total_kb", total);
        cJSON_AddNumberToObject(st, "mem_free_kb", free_mem);
        cJSON_AddNumberToObject(st, "mem_available_kb", avail);
        cJSON_AddStringToObject(st, "network_details", net_details);

        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    if (strcmp(clean_path, "/api/disk-space") == 0 && strcmp(method, "GET") == 0) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        struct statvfs sv;
        uint64_t total_kb = 5120;
        uint64_t free_kb = 0;
        if (statvfs("/data", &sv) == 0 && sv.f_frsize > 0) {
            total_kb = ((uint64_t)sv.f_blocks * sv.f_frsize) / 1024;
            free_kb = ((uint64_t)sv.f_bavail * sv.f_frsize) / 1024;
        } else if (statvfs("/", &sv) == 0 && sv.f_frsize > 0) {
            total_kb = ((uint64_t)sv.f_blocks * sv.f_frsize) / 1024;
            free_kb = ((uint64_t)sv.f_bavail * sv.f_frsize) / 1024;
        }
        cJSON_AddNumberToObject(st, "total_kb", (double)total_kb);
        cJSON_AddNumberToObject(st, "free_kb", (double)free_kb);

        struct statvfs sv_cache;
        uint64_t cache_total_kb = 0;
        uint64_t cache_free_kb = 0;
        if (statvfs("/cache", &sv_cache) == 0 && sv_cache.f_frsize > 0) {
            cache_total_kb = ((uint64_t)sv_cache.f_blocks * sv_cache.f_frsize) / 1024;
            cache_free_kb = ((uint64_t)sv_cache.f_bavail * sv_cache.f_frsize) / 1024;
        }
        cJSON_AddNumberToObject(st, "cache_total_kb", (double)cache_total_kb);
        cJSON_AddNumberToObject(st, "cache_free_kb", (double)cache_free_kb);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    if (strcmp(clean_path, "/api/network-status") == 0 && strcmp(method, "GET") == 0) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);

        int usb_host = get_usb_host_mode();
        cJSON_AddBoolToObject(st, "usb_host_mode", usb_host);

        // Detect USB Ethernet
        char eth_if[32] = {0};
        int eth_present = find_usb_eth_interface(eth_if, sizeof(eth_if));
        int eth_carrier = eth_present ? get_iface_carrier(eth_if) : 0;
        char eth_ip[32] = {0}, eth_mask[32] = {0}, eth_mac[32] = {0};
        if (eth_present) {
            get_iface_ip_mask_mac(eth_if, eth_ip, sizeof(eth_ip), eth_mask, sizeof(eth_mask), eth_mac, sizeof(eth_mac));
        }

        // Detect Wi-Fi (ath0)
        char wifi_ip[32] = {0}, wifi_mask[32] = {0}, wifi_mac[32] = {0}, wifi_ssid[64] = {0};
        get_iface_ip_mask_mac("ath0", wifi_ip, sizeof(wifi_ip), wifi_mask, sizeof(wifi_mask), wifi_mac, sizeof(wifi_mac));
        int wifi_connected = (wifi_ip[0] != '\0');
        if (wifi_connected) {
            get_wifi_ssid(wifi_ssid, sizeof(wifi_ssid));
        }

        // Active Default Gateway & Route
        char gw[64] = "", default_iface[32] = "";
        char mask_dummy[64] = "", mac_dummy[64] = "";
        get_network_details(default_iface, sizeof(default_iface), gw, sizeof(gw), mask_dummy, sizeof(mask_dummy), mac_dummy, sizeof(mac_dummy));

        // Determine active interface
        const char *active_if = default_iface[0] ? default_iface : (eth_carrier && eth_ip[0] ? eth_if : "ath0");
        cJSON_AddStringToObject(st, "active_interface", active_if);

        // Determine Connection Type
        const char *conn_type = "Disconnected";
        if (eth_carrier && eth_ip[0] && wifi_connected) {
            conn_type = (strcmp(active_if, eth_if) == 0) ? "Dual (Ethernet Active + Wi-Fi Standby)" : "Dual (Wi-Fi Active + Ethernet Standby)";
        } else if (eth_carrier && eth_ip[0]) {
            conn_type = "Ethernet";
        } else if (wifi_connected && eth_carrier) {
            conn_type = "Dual (Wi-Fi Active + Ethernet Link Up)";
        } else if (wifi_connected && eth_present) {
            conn_type = "Dual (Wi-Fi Active + Ethernet Standby)";
        } else if (wifi_connected) {
            conn_type = "Wi-Fi";
        } else if (eth_carrier) {
            conn_type = "Ethernet (Link Up, No IP)";
        }
        cJSON_AddStringToObject(st, "connection_type", conn_type);
        cJSON_AddStringToObject(st, "mode", (strncmp(active_if, "eth", 3) == 0) ? "ethernet" : "wifi");

        // Primary IP/Mask/MAC corresponding to active interface
        if (strcmp(active_if, eth_if) == 0 && eth_ip[0]) {
            cJSON_AddStringToObject(st, "ip", eth_ip);
            cJSON_AddStringToObject(st, "netmask", eth_mask);
            cJSON_AddStringToObject(st, "mac", eth_mac);
        } else {
            char local_ip[64];
            get_local_ip(client_fd, local_ip, sizeof(local_ip));
            cJSON_AddStringToObject(st, "ip", wifi_ip[0] ? wifi_ip : local_ip);
            cJSON_AddStringToObject(st, "netmask", wifi_mask[0] ? wifi_mask : "255.255.255.0");
            cJSON_AddStringToObject(st, "mac", wifi_mac[0] ? wifi_mac : mac_dummy);
        }
        cJSON_AddStringToObject(st, "gateway", gw);

        // Ethernet Specifics
        cJSON_AddBoolToObject(st, "eth_present", eth_present);
        cJSON_AddBoolToObject(st, "eth_carrier", eth_carrier);
        cJSON_AddStringToObject(st, "eth_interface", eth_if[0] ? eth_if : "none");
        cJSON_AddStringToObject(st, "eth_ip", eth_ip);
        cJSON_AddStringToObject(st, "eth_mask", eth_mask);
        cJSON_AddStringToObject(st, "eth_mac", eth_mac);

        // Wi-Fi Specifics
        cJSON_AddBoolToObject(st, "wifi_connected", wifi_connected);
        cJSON_AddStringToObject(st, "wifi_interface", "ath0");
        cJSON_AddStringToObject(st, "wifi_ip", wifi_ip);
        cJSON_AddStringToObject(st, "wifi_mask", wifi_mask);
        cJSON_AddStringToObject(st, "wifi_mac", wifi_mac);
        cJSON_AddStringToObject(st, "wifi_ssid", wifi_ssid);

        // Configuration from /data/codex/ethernet.conf
        cJSON *cfg = cJSON_CreateObject();
        FILE *cff = fopen("/data/codex/ethernet.conf", "r");
        if (cff) {
            char cline[128];
            while (fgets(cline, sizeof(cline), cff)) {
                char *eq = strchr(cline, '=');
                if (!eq) continue;
                *eq = '\0';
                char *k = cline;
                char *v = eq + 1;
                while (*k == ' ' || *k == '\t') k++;
                char *ken = k + strlen(k) - 1;
                while (ken > k && (*ken == ' ' || *ken == '\t' || *ken == '\r' || *ken == '\n')) *ken-- = '\0';
                while (*v == ' ' || *v == '\t' || *v == '"') v++;
                char *ven = v + strlen(v) - 1;
                while (ven >= v && (*ven == ' ' || *ven == '\t' || *ven == '\r' || *ven == '\n' || *ven == '"')) *ven-- = '\0';
                if (strcmp(k, "ETH_ENABLED") == 0) cJSON_AddBoolToObject(cfg, "eth_enabled", atoi(v) == 1);
                else if (strcmp(k, "ETH_FALLBACK_WIFI") == 0) cJSON_AddBoolToObject(cfg, "eth_fallback_wifi", atoi(v) == 1);
                else if (strcmp(k, "ETH_MODE") == 0) cJSON_AddStringToObject(cfg, "eth_mode", v);
                else if (strcmp(k, "ETH_IP") == 0) cJSON_AddStringToObject(cfg, "eth_ip", v);
                else if (strcmp(k, "ETH_NETMASK") == 0) cJSON_AddStringToObject(cfg, "eth_netmask", v);
                else if (strcmp(k, "ETH_GATEWAY") == 0) cJSON_AddStringToObject(cfg, "eth_gateway", v);
                else if (strcmp(k, "ETH_DNS") == 0) cJSON_AddStringToObject(cfg, "eth_dns", v);
            }
            fclose(cff);
        }
        cJSON_AddItemToObject(st, "config", cfg);

        char ifconfig_buf[8192] = {0};
        FILE *pif = popen("/sbin/ifconfig 2>/dev/null", "r");
        if (pif) {
            size_t r = fread(ifconfig_buf, 1, sizeof(ifconfig_buf) - 1, pif);
            ifconfig_buf[r] = '\0';
            pclose(pif);
        }
        cJSON_AddStringToObject(st, "ifconfig", ifconfig_buf);

        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    if (strcmp(clean_path, "/api/update-status") == 0 && strcmp(method, "GET") == 0) {
        char fw_str[128] = "4.15.600";
        FILE *vf = fopen("/etc/version", "r");
        if (vf) {
            if (fgets(fw_str, sizeof(fw_str), vf)) {
                char *nl = strchr(fw_str, '\n');
                if (nl) *nl = '\0';
                nl = strchr(fw_str, '\r');
                if (nl) *nl = '\0';
            }
            fclose(vf);
        }
        char remote_ver[64] = "v" CODEX_VERSION;
        int has_update = (access("/data/codex/update_available.flag", F_OK) == 0);
        FILE *rf = fopen("/data/codex/latest_version", "r");
        if (rf) {
            char lbuf[64] = {0};
            if (fgets(lbuf, sizeof(lbuf), rf)) {
                char *nl = strchr(lbuf, '\n'); if (nl) *nl = '\0';
                nl = strchr(lbuf, '\r'); if (nl) *nl = '\0';
                if (strlen(lbuf) > 0) {
                    snprintf(remote_ver, sizeof(remote_ver), "%s", lbuf);
                    if (strcmp(lbuf, CODEX_VERSION) != 0 && strcmp(lbuf, "v" CODEX_VERSION) != 0) {
                        has_update = 1;
                    }
                }
            }
            fclose(rf);
        }

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddStringToObject(st, "current_version", CODEX_VERSION);
        cJSON_AddStringToObject(st, "version", CODEX_VERSION);
        cJSON_AddStringToObject(st, "local_version", "v" CODEX_VERSION " (" CODEX_COMMIT ")");
        cJSON_AddStringToObject(st, "remote_version", remote_ver);
        cJSON_AddStringToObject(st, "commit", CODEX_COMMIT);
        cJSON_AddStringToObject(st, "build_time", __DATE__ " " __TIME__);
        cJSON_AddStringToObject(st, "fw_version", fw_str);
        cJSON_AddStringToObject(st, "firmware", fw_str);
        cJSON_AddStringToObject(st, "rf_version", "0x20 CC2544");
        cJSON_AddStringToObject(st, "bt_version", "BTstack v1.5 (UART)");
        cJSON_AddStringToObject(st, "arch", "mips (AR9344)");
        cJSON_AddBoolToObject(st, "update_available", has_update);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    if ((strcmp(clean_path, "/api/update-check-state") == 0 || strcmp(clean_path, "/api/update-check") == 0) &&
        (strcmp(method, "POST") == 0 || strcmp(method, "GET") == 0)) {
        cJSON *p = parse_request_params(body);
        char repo[256] = {0};
        char token[128] = {0};
        char client_remote_ver[64] = {0};
        if (p) {
            cJSON *j_repo = cJSON_GetObjectItemCaseSensitive(p, "repo");
            if (j_repo && cJSON_IsString(j_repo) && j_repo->valuestring && j_repo->valuestring[0]) {
                strncpy(repo, j_repo->valuestring, sizeof(repo) - 1);
            }
            cJSON *j_tok = cJSON_GetObjectItemCaseSensitive(p, "token");
            if (j_tok && cJSON_IsString(j_tok) && j_tok->valuestring && j_tok->valuestring[0]) {
                strncpy(token, j_tok->valuestring, sizeof(token) - 1);
            }
            cJSON *j_ver = cJSON_GetObjectItemCaseSensitive(p, "remote_version");
            if (!j_ver) j_ver = cJSON_GetObjectItemCaseSensitive(p, "version");
            if (j_ver && cJSON_IsString(j_ver) && j_ver->valuestring && j_ver->valuestring[0]) {
                strncpy(client_remote_ver, j_ver->valuestring, sizeof(client_remote_ver) - 1);
            }
            cJSON_Delete(p);
        }

        if (client_remote_ver[0]) {
            char clean_ver[64] = {0};
            const char *src = client_remote_ver;
            if (src[0] == 'v' || src[0] == 'V') src++;
            strncpy(clean_ver, src, sizeof(clean_ver) - 1);
            FILE *rf = fopen("/data/codex/latest_version", "w");
            if (rf) {
                fprintf(rf, "%s\n", clean_ver);
                fclose(rf);
            }
            if (strcmp(clean_ver, CODEX_VERSION) != 0) {
                FILE *ff = fopen("/data/codex/update_available.flag", "w");
                if (ff) fclose(ff);
            } else {
                unlink("/data/codex/update_available.flag");
            }
        } else {
            char cmd[512] = {0};
            if (access("/data/codex/update.sh", X_OK) == 0) {
                snprintf(cmd, sizeof(cmd), "/data/codex/update.sh check '%s' '%s' >/tmp/update_check.log 2>&1", repo, token);
            } else if (access("/data/codex/scripts/update.sh", X_OK) == 0) {
                snprintf(cmd, sizeof(cmd), "/data/codex/scripts/update.sh check '%s' '%s' >/tmp/update_check.log 2>&1", repo, token);
            } else {
                snprintf(cmd, sizeof(cmd),
                    "/data/codex/bin/busybox wget --no-check-certificate -q -T 6 -O /data/codex/latest_version 'https://raw.githubusercontent.com/iranl/harmony-ng/ota/VERSION' 2>/dev/null || "
                    "/cache/bin/busybox wget --no-check-certificate -q -T 6 -O /data/codex/latest_version 'https://raw.githubusercontent.com/iranl/harmony-ng/ota/VERSION' 2>/dev/null");
            }
            system(cmd);
        }

        char remote_ver[64] = CODEX_VERSION;
        int has_update = 0;
        FILE *rf = fopen("/data/codex/latest_version", "r");
        if (rf) {
            char lbuf[64] = {0};
            if (fgets(lbuf, sizeof(lbuf), rf)) {
                char *nl = strchr(lbuf, '\n'); if (nl) *nl = '\0';
                nl = strchr(lbuf, '\r'); if (nl) *nl = '\0';
                if (strlen(lbuf) > 0) {
                    const char *clean = (lbuf[0] == 'v' || lbuf[0] == 'V') ? lbuf + 1 : lbuf;
                    snprintf(remote_ver, sizeof(remote_ver), "%s", clean);
                    if (strcmp(clean, CODEX_VERSION) != 0) {
                        has_update = 1;
                        FILE *ff = fopen("/data/codex/update_available.flag", "w");
                        if (ff) fclose(ff);
                    } else {
                        unlink("/data/codex/update_available.flag");
                    }
                }
            }
            fclose(rf);
        }

        char msg[256];
        if (has_update) {
            snprintf(msg, sizeof(msg), "Update available: v%s (Current: v%s)", remote_ver, CODEX_VERSION);
        } else {
            snprintf(msg, sizeof(msg), "Hub binaries and WebUI are up to date (v%s).", CODEX_VERSION);
        }

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddStringToObject(st, "current_version", CODEX_VERSION);
        cJSON_AddStringToObject(st, "remote_version", remote_ver);
        cJSON_AddStringToObject(st, "message", msg);
        cJSON_AddBoolToObject(st, "has_update", has_update);
        cJSON_AddBoolToObject(st, "update_available", has_update);
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    if (strcmp(clean_path, "/api/update-apply") == 0 && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        char repo[256] = {0};
        char token[128] = {0};
        if (p) {
            cJSON *j_repo = cJSON_GetObjectItemCaseSensitive(p, "repo");
            if (j_repo && cJSON_IsString(j_repo) && j_repo->valuestring && j_repo->valuestring[0]) {
                strncpy(repo, j_repo->valuestring, sizeof(repo) - 1);
            }
            cJSON *j_tok = cJSON_GetObjectItemCaseSensitive(p, "token");
            if (j_tok && cJSON_IsString(j_tok) && j_tok->valuestring && j_tok->valuestring[0]) {
                strncpy(token, j_tok->valuestring, sizeof(token) - 1);
            }
            cJSON_Delete(p);
        }

        char cmd[512] = {0};
        snprintf(cmd, sizeof(cmd),
            "nohup /bin/sh -c 'sleep 1; if [ -x /data/codex/update.sh ]; then /data/codex/update.sh apply \"%s\" \"%s\"; elif [ -x /data/codex/scripts/update.sh ]; then /data/codex/scripts/update.sh apply \"%s\" \"%s\"; else /data/codex/init.sh restart; fi' >/tmp/update_apply.log 2>&1 &",
            repo, token, repo, token);
        system(cmd);

        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddStringToObject(st, "status", "applying");
        cJSON_AddStringToObject(st, "message", "Software update triggered. Hub services downloading and applying in background.");
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    if (strcmp(clean_path, "/api/remote-mapping") == 0 && strcmp(method, "GET") == 0) {
        FILE *f = fopen("/data/codex/bt_remote_map.json", "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 200000) {
                char *buf = (char *)malloc((size_t)sz + 1);
                if (buf) {
                    size_t nr = fread(buf, 1, (size_t)sz, f);
                    buf[nr] = '\0';
                    cJSON *mj = cJSON_Parse(buf);
                    free(buf);
                    if (mj) {
                        send_cjson_resp(client_fd, 200, mj);
                        cJSON_Delete(mj);
                        fclose(f);
                        close(client_fd);
                        return;
                    }
                }
            }
            fclose(f);
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON *rem = cJSON_CreateObject();
        cJSON_AddStringToObject(rem, "name", "Homatics B25");
        cJSON_AddStringToObject(rem, "bdaddr", "");
        cJSON_AddItemToObject(st, "remote", rem);
        cJSON_AddItemToObject(st, "activities", cJSON_CreateObject());
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    if (strcmp(clean_path, "/api/elite-mapping") == 0 && strcmp(method, "GET") == 0) {
        FILE *f = fopen("/data/codex/elite_remote_map.json", "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 200000) {
                char *buf = (char *)malloc((size_t)sz + 1);
                if (buf) {
                    size_t nr = fread(buf, 1, (size_t)sz, f);
                    buf[nr] = '\0';
                    cJSON *mj = cJSON_Parse(buf);
                    free(buf);
                    if (mj) {
                        send_cjson_resp(client_fd, 200, mj);
                        cJSON_Delete(mj);
                        fclose(f);
                        close(client_fd);
                        return;
                    }
                }
            }
            fclose(f);
        }
        cJSON *st = cJSON_CreateObject();
        cJSON_AddBoolToObject(st, "ok", 1);
        cJSON_AddArrayToObject(st, "mappings");
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    /* Route: GET /api/inventory */
    if (strcmp(clean_path, "/api/inventory") == 0 && strcmp(method, "GET") == 0) {
        cJSON *devs = get_device_list_cached();
        if (devs) {
            send_cjson_resp(client_fd, 200, devs);
        } else {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddBoolToObject(err, "ok", 0);
            cJSON_AddStringToObject(err, "error", "DeviceList not found");
            send_cjson_resp(client_fd, 404, err);
            cJSON_Delete(err);
        }
        close(client_fd);
        return;
    }

    /* Route: GET /api/activities */
    if (strcmp(clean_path, "/api/activities") == 0 && strcmp(method, "GET") == 0) {
        cJSON *act_j = config_store_get_activities_json();
        if (act_j) {
            cJSON *resp = cJSON_Duplicate(act_j, 1);
            if (resp) {
                cJSON_AddStringToObject(resp, "currentId", orch_get_current_activity());
                cJSON_AddNumberToObject(resp, "state", (int)orch_get_state());
                send_cjson_resp(client_fd, 200, resp);
                cJSON_Delete(resp);
                close(client_fd);
                return;
            }
        }
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", 0);
        cJSON_AddStringToObject(err, "error", "Activities not found");
        send_cjson_resp(client_fd, 404, err);
        cJSON_Delete(err);
        close(client_fd);
        return;
    }

    /* Route: GET /api/activity */
    if ((strcmp(clean_path, "/api/activity") == 0 || strcmp(clean_path, "/api/current_activity") == 0) &&
        strcmp(method, "GET") == 0) {
        cJSON *aj = cJSON_CreateObject();
        cJSON_AddStringToObject(aj, "activity", orch_get_current_activity());
        cJSON_AddNumberToObject(aj, "state", (int)orch_get_state());
        cJSON_AddBoolToObject(aj, "busy", orch_is_busy() ? 1 : 0);
        send_cjson_resp(client_fd, 200, aj);
        cJSON_Delete(aj);
        close(client_fd);
        return;
    }

    /* Route: POST /api/activity-progress */
    if (strcmp(clean_path, "/api/activity-progress") == 0 && strcmp(method, "POST") == 0) {
        if (body && body[0]) {
            ws_broadcast_text(body);
            char act_status_top[160];
            snprintf(act_status_top, sizeof(act_status_top), "%s/activity/status", g_mqtt_base_topic);
            mqtt_publish(act_status_top, body, 1);
            extern void daemon_publish_mqtt_state(void);
            daemon_publish_mqtt_state();
        }
        cJSON *ok = cJSON_CreateObject();
        cJSON_AddBoolToObject(ok, "ok", 1);
        send_cjson_resp(client_fd, 200, ok);
        cJSON_Delete(ok);
        close(client_fd);
        return;
    }

    /* Route: POST /api/sync or POST /api/reload */
    if ((strcmp(clean_path, "/api/sync") == 0 || strcmp(clean_path, "/api/reload") == 0) && strcmp(method, "POST") == 0) {
        extern void daemon_sync_mqtt(void);
        daemon_sync_mqtt();
        cJSON *ok = cJSON_CreateObject();
        cJSON_AddBoolToObject(ok, "ok", 1);
        send_cjson_resp(client_fd, 200, ok);
        cJSON_Delete(ok);
        close(client_fd);
        return;
    }

    /* Route: POST /api/activity or POST /api/activity-start */
    if ((strcmp(clean_path, "/api/activity") == 0 || strcmp(clean_path, "/api/activity-start") == 0) && strcmp(method, "POST") == 0) {
        char act_id[32] = {0};
        int dry_run = 0;
        cJSON *req_j = body ? cJSON_Parse(body) : NULL;
        if (req_j) {
            cJSON *ja = cJSON_GetObjectItemCaseSensitive(req_j, "activity");
            if (!ja) ja = cJSON_GetObjectItemCaseSensitive(req_j, "id");
            if (ja && cJSON_IsString(ja) && ja->valuestring) strncpy(act_id, ja->valuestring, sizeof(act_id) - 1);
            else if (ja && cJSON_IsNumber(ja)) snprintf(act_id, sizeof(act_id), "%ld", (long)ja->valuedouble);

            cJSON *jd = cJSON_GetObjectItemCaseSensitive(req_j, "dry_run");
            if (jd) {
                if (cJSON_IsTrue(jd)) dry_run = 1;
                else if (cJSON_IsNumber(jd) && jd->valuedouble > 0) dry_run = 1;
                else if (cJSON_IsString(jd) && (strcmp(jd->valuestring, "true") == 0 || strcmp(jd->valuestring, "1") == 0)) dry_run = 1;
            }
            cJSON_Delete(req_j);
        }

        if (!act_id[0]) {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddStringToObject(err, "error", "Missing activity parameter");
            send_cjson_resp(client_fd, 400, err);
            cJSON_Delete(err);
            close(client_fd);
            return;
        }

        if (dry_run) {
            char preview[4096] = {0};
            int rc = orch_switch_activity(act_id, 1, preview, sizeof(preview));
            if (rc == 0) {
                cJSON *pj = preview[0] ? cJSON_Parse(preview) : NULL;
                if (!pj) {
                    pj = cJSON_CreateObject();
                    cJSON_AddStringToObject(pj, "target", act_id);
                    cJSON_AddArrayToObject(pj, "steps");
                }
                send_cjson_resp(client_fd, 200, pj);
                cJSON_Delete(pj);
            } else {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "error", "Interlock busy");
                send_cjson_resp(client_fd, 409, err);
                cJSON_Delete(err);
            }
        } else {
            int rc = orch_switch_activity(act_id, 0, NULL, 0);
            if (rc == 0) {
                cJSON *suc = cJSON_CreateObject();
                cJSON_AddBoolToObject(suc, "success", 1);
                cJSON_AddStringToObject(suc, "state", "starting");
                send_cjson_resp(client_fd, 200, suc);
                cJSON_Delete(suc);
            } else {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "error", "Interlock busy");
                send_cjson_resp(client_fd, 409, err);
                cJSON_Delete(err);
            }
        }
        close(client_fd);
        return;
    }

    /* Route: POST /api/poweroff or POST /api/activity-stop */
    if ((strcmp(clean_path, "/api/poweroff") == 0 || strcmp(clean_path, "/api/activity-stop") == 0) && strcmp(method, "POST") == 0) {
        int dry_run = 0;
        cJSON *req_j = body ? cJSON_Parse(body) : NULL;
        if (req_j) {
            cJSON *jd = cJSON_GetObjectItemCaseSensitive(req_j, "dry_run");
            if (jd) {
                if (cJSON_IsTrue(jd)) dry_run = 1;
                else if (cJSON_IsNumber(jd) && jd->valuedouble > 0) dry_run = 1;
                else if (cJSON_IsString(jd) && (strcmp(jd->valuestring, "true") == 0 || strcmp(jd->valuestring, "1") == 0)) dry_run = 1;
            }
            cJSON_Delete(req_j);
        }

        if (dry_run) {
            char preview[4096] = {0};
            int rc = orch_power_off(1, preview, sizeof(preview));
            if (rc == 0) {
                cJSON *pj = preview[0] ? cJSON_Parse(preview) : NULL;
                if (!pj) {
                    pj = cJSON_CreateObject();
                    cJSON_AddStringToObject(pj, "target", ORCH_POWEROFF_ID);
                    cJSON_AddArrayToObject(pj, "steps");
                }
                send_cjson_resp(client_fd, 200, pj);
                cJSON_Delete(pj);
            } else {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "error", "busy");
                send_cjson_resp(client_fd, 409, err);
                cJSON_Delete(err);
            }
        } else {
            int rc = orch_power_off(0, NULL, 0);
            if (rc == 0) {
                cJSON *suc = cJSON_CreateObject();
                cJSON_AddBoolToObject(suc, "success", 1);
                cJSON_AddStringToObject(suc, "state", "stopping");
                send_cjson_resp(client_fd, 200, suc);
                cJSON_Delete(suc);
            } else {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "error", "busy");
                send_cjson_resp(client_fd, 409, err);
                cJSON_Delete(err);
            }
        }
        close(client_fd);
        return;
    }

    /* Route: POST /api/activity-cancel or POST /api/activity-reset or POST /api/interlock-reset */
    if ((strcmp(clean_path, "/api/activity-cancel") == 0 ||
         strcmp(clean_path, "/api/activity-reset") == 0 ||
         strcmp(clean_path, "/api/interlock-reset") == 0) && strcmp(method, "POST") == 0) {
        orch_cancel();
        cJSON *suc = cJSON_CreateObject();
        cJSON_AddBoolToObject(suc, "success", 1);
        cJSON_AddStringToObject(suc, "message", "Activity cancelled / interlock reset");
        send_cjson_resp(client_fd, 200, suc);
        cJSON_Delete(suc);
        close(client_fd);
        return;
    }

    /* Route: POST /api/pronto-blast */
    if (strcmp(clean_path, "/api/pronto-blast") == 0 && strcmp(method, "POST") == 0) {
        char pronto[512] = {0};
        uint8_t ports = IR_PORT_ALL;
        uint8_t repeats = 3;

        cJSON *req_j = body ? cJSON_Parse(body) : NULL;
        if (req_j) {
            cJSON *jp = cJSON_GetObjectItemCaseSensitive(req_j, "pronto");
            if (jp && cJSON_IsString(jp) && jp->valuestring) strncpy(pronto, jp->valuestring, sizeof(pronto) - 1);

            cJSON *jport = cJSON_GetObjectItemCaseSensitive(req_j, "ports");
            if (jport && cJSON_IsNumber(jport)) ports = (uint8_t)jport->valuedouble;
            else if (jport && cJSON_IsString(jport) && jport->valuestring) ports = (uint8_t)atoi(jport->valuestring);

            cJSON *jrep = cJSON_GetObjectItemCaseSensitive(req_j, "repeats");
            if (jrep && cJSON_IsNumber(jrep)) repeats = (uint8_t)jrep->valuedouble;
            else if (jrep && cJSON_IsString(jrep) && jrep->valuestring) repeats = (uint8_t)atoi(jrep->valuestring);

            cJSON_Delete(req_j);
        }

        if (!pronto[0]) {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddStringToObject(err, "error", "Missing pronto parameter");
            send_cjson_resp(client_fd, 400, err);
            cJSON_Delete(err);
            close(client_fd);
            return;
        }

        int rc = hw_ir_send_pronto(pronto, ports, repeats);
        if (rc == 0) {
            cJSON *suc = cJSON_CreateObject();
            cJSON_AddBoolToObject(suc, "success", 1);
            send_cjson_resp(client_fd, 200, suc);
            cJSON_Delete(suc);
        } else {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddStringToObject(err, "error", "IR send failed");
            send_cjson_resp(client_fd, 500, err);
            cJSON_Delete(err);
        }
        close(client_fd);
        return;
    }

    /* Route: GET /api/remote-status or GET /api/remote-pair-status */
    if ((strcmp(clean_path, "/api/remote-status") == 0 || strcmp(clean_path, "/api/remote-pair-status") == 0) &&
        strcmp(method, "GET") == 0) {
        cJSON *st = NULL;
        FILE *f = fopen("/tmp/codex_btstack_status.json", "r");
        if (f) {
            char stat_buf[2048] = {0};
            fread(stat_buf, 1, sizeof(stat_buf) - 1, f);
            fclose(f);
            if (stat_buf[0]) st = cJSON_Parse(stat_buf);
        }
        if (!st) {
            st = cJSON_CreateObject();
            cJSON_AddStringToObject(st, "state", "idle");
        }
        cJSON_AddBoolToObject(st, "ok", 1);

        int running = (system("pidof codex_btstack >/dev/null 2>&1") == 0);
        cJSON_AddBoolToObject(st, "running", running);

        if (!cJSON_GetObjectItemCaseSensitive(st, "bdaddr")) {
            FILE *mf = fopen("/data/codex/bt_remote_map.json", "r");
            if (mf) {
                char mbuf[2048] = {0};
                fread(mbuf, 1, sizeof(mbuf) - 1, mf);
                fclose(mf);
                cJSON *mj = cJSON_Parse(mbuf);
                if (mj) {
                    cJSON *rem = cJSON_GetObjectItemCaseSensitive(mj, "remote");
                    cJSON *addr = rem ? cJSON_GetObjectItemCaseSensitive(rem, "bdaddr") : cJSON_GetObjectItemCaseSensitive(mj, "bdaddr");
                    if (addr && cJSON_IsString(addr) && addr->valuestring[0]) {
                        cJSON_AddStringToObject(st, "bdaddr", addr->valuestring);
                        cJSON_AddStringToObject(st, "name", "Homatics B25");
                        cJSON_AddBoolToObject(st, "connected", running);
                    }
                    cJSON_Delete(mj);
                }
            }
        }
        send_cjson_resp(client_fd, 200, st);
        cJSON_Delete(st);
        close(client_fd);
        return;
    }

    /* Route: POST /api/remote-scan */
    if (strcmp(clean_path, "/api/remote-scan") == 0 && strcmp(method, "POST") == 0) {
        FILE *cf = fopen("/tmp/codex_btstack_cmd", "w");
        if (cf) {
            fputs("SCAN\n", cf);
            fclose(cf);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "state", "scanning");
        cJSON_AddStringToObject(resp, "message", "Bluetooth remote scan started");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /api/remote-pair */
    if (strcmp(clean_path, "/api/remote-pair") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        char bdaddr[32] = {0};
        if (params) {
            cJSON *ja = cJSON_GetObjectItemCaseSensitive(params, "bdaddr");
            if (ja && cJSON_IsString(ja)) strncpy(bdaddr, ja->valuestring, sizeof(bdaddr) - 1);
            cJSON_Delete(params);
        }
        if (bdaddr[0]) {
            FILE *cf = fopen("/tmp/codex_btstack_cmd", "w");
            if (cf) {
                fprintf(cf, "PAIR %s\n", bdaddr);
                fclose(cf);
            }
            FILE *mf = fopen("/data/codex/bt_remote_map.json", "r");
            if (mf) {
                fseek(mf, 0, SEEK_END);
                long sz = ftell(mf);
                fseek(mf, 0, SEEK_SET);
                if (sz > 0 && sz < 200000) {
                    char *b = (char *)malloc((size_t)sz + 1);
                    if (b) {
                        size_t nr = fread(b, 1, (size_t)sz, mf);
                        b[nr] = '\0';
                        cJSON *mj = cJSON_Parse(b);
                        free(b);
                        if (mj) {
                            cJSON *rem = cJSON_GetObjectItemCaseSensitive(mj, "remote");
                            if (!rem) {
                                rem = cJSON_CreateObject();
                                cJSON_AddItemToObject(mj, "remote", rem);
                            }
                            cJSON_DeleteItemFromObject(rem, "bdaddr");
                            cJSON_AddStringToObject(rem, "bdaddr", bdaddr);
                            char *up = cJSON_Print(mj);
                            if (up) {
                                write_file_atomic("/data/codex/bt_remote_map.json", up, strlen(up));
                                free(up);
                            }
                            cJSON_Delete(mj);
                        }
                    }
                }
                fclose(mf);
            }
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "message", "Pair command issued to BTstack");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /api/ir-send or POST /ir/send */
    if ((strcmp(clean_path, "/api/ir-send") == 0 || strcmp(clean_path, "/ir/send") == 0) && strcmp(method, "POST") == 0) {
        char dev_id[64] = {0};
        char cmd_name[128] = {0};
        char pronto_buf[2048] = {0};
        int port = 7;
        int repeats = 1;
        cJSON *req_j = body ? parse_request_params(body) : NULL;
        if (req_j) {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(req_j, "deviceId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(req_j, "id");
            if (jid && cJSON_IsString(jid) && jid->valuestring) strncpy(dev_id, jid->valuestring, sizeof(dev_id) - 1);
            else if (jid && cJSON_IsNumber(jid)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jid->valuedouble);

            cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "command");
            if (!jcmd) jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "name");
            if (jcmd && cJSON_IsString(jcmd) && jcmd->valuestring) strncpy(cmd_name, jcmd->valuestring, sizeof(cmd_name) - 1);

            cJSON *jpronto = cJSON_GetObjectItemCaseSensitive(req_j, "pronto");
            if (!jpronto) jpronto = cJSON_GetObjectItemCaseSensitive(req_j, "code");
            if (!jpronto) jpronto = cJSON_GetObjectItemCaseSensitive(req_j, "raw");
            if (jpronto && cJSON_IsString(jpronto) && jpronto->valuestring) strncpy(pronto_buf, jpronto->valuestring, sizeof(pronto_buf) - 1);

            cJSON *jport = cJSON_GetObjectItemCaseSensitive(req_j, "port");
            if (jport && cJSON_IsNumber(jport) && jport->valueint > 0) port = jport->valueint;

            cJSON *jrep = cJSON_GetObjectItemCaseSensitive(req_j, "repeats");
            if (jrep && cJSON_IsNumber(jrep) && jrep->valueint > 0) repeats = jrep->valueint;

            cJSON_Delete(req_j);
        }

        int rc = -1;
        if (pronto_buf[0]) {
            rc = hw_ir_send_pronto(pronto_buf, (uint8_t)port, (uint8_t)repeats);
            if (rc != 0 && strncmp(pronto_buf, "G:", 2) == 0) {
                rc = hw_ir_send_harmony_keycode(pronto_buf, (uint8_t)port, (uint8_t)repeats);
            }
        } else if (dev_id[0] && cmd_name[0]) {
            rc = hw_device_command_send(dev_id, cmd_name);
        } else {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddBoolToObject(err, "ok", 0);
            cJSON_AddStringToObject(err, "error", "Missing deviceId/command or pronto code");
            send_cjson_resp(client_fd, 400, err);
            cJSON_Delete(err);
            close(client_fd);
            return;
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", rc == 0);
        if (dev_id[0]) cJSON_AddStringToObject(resp, "deviceId", dev_id);
        if (cmd_name[0]) cJSON_AddStringToObject(resp, "command", cmd_name);
        if (rc != 0) cJSON_AddStringToObject(resp, "error", "IR transmit failed");
        send_cjson_resp(client_fd, rc == 0 ? 200 : 500, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: GET /api/device-commands */
    if (strncmp(clean_path, "/api/device-commands", 20) == 0 && strcmp(method, "GET") == 0) {
        char dev_id[64] = {0};
        const char *q = strchr(path, '?');
        if (q) {
            cJSON *qp = parse_request_params(q + 1);
            if (qp) {
                cJSON *jid = cJSON_GetObjectItemCaseSensitive(qp, "deviceId");
                if (!jid) jid = cJSON_GetObjectItemCaseSensitive(qp, "id");
                if (jid && cJSON_IsString(jid)) strncpy(dev_id, jid->valuestring, sizeof(dev_id) - 1);
                cJSON_Delete(qp);
            }
        }
        cJSON *res_cmds = cJSON_CreateArray();
        if (dev_id[0]) {
            cJSON *root = config_store_get_devices_json();
            if (root) {
                cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                if (devs && cJSON_IsArray(devs)) {
                    cJSON *di = NULL;
                    cJSON_ArrayForEach(di, devs) {
                        cJSON *d = cJSON_GetObjectItemCaseSensitive(di, "Device");
                        if (!d) d = di;
                        cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                        char d_id_str[64] = {0};
                        if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                        else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                        if (strcmp(d_id_str, dev_id) == 0) {
                            cJSON *cmds = cJSON_GetObjectItemCaseSensitive(di, "commands");
                            if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(di, "Commands");
                            if (cmds && cJSON_IsArray(cmds)) {
                                cJSON *ci = NULL;
                                cJSON_ArrayForEach(ci, cmds) {
                                    cJSON_AddItemToArray(res_cmds, cJSON_Duplicate(ci, 1));
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddItemToObject(resp, "commands", res_cmds);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /api/device-mqtt-test */
    if (strcmp(clean_path, "/api/device-mqtt-test") == 0 && strcmp(method, "POST") == 0) {
        char dev_id[64] = {0};
        char dev_name[128] = {0};
        char cmd_name[128] = "PowerToggle";
        char req_topic[256] = {0};
        int req_pulse_ms = 1000;

        cJSON *req_j = body ? parse_request_params(body) : NULL;
        if (req_j) {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(req_j, "deviceId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(req_j, "id");
            if (jid && cJSON_IsString(jid)) strncpy(dev_id, jid->valuestring, sizeof(dev_id) - 1);
            else if (jid && cJSON_IsNumber(jid)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jid->valuedouble);

            cJSON *jdev = cJSON_GetObjectItemCaseSensitive(req_j, "device");
            if (jdev && cJSON_IsString(jdev)) strncpy(dev_name, jdev->valuestring, sizeof(dev_name) - 1);

            cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "command");
            if (jcmd && cJSON_IsString(jcmd) && jcmd->valuestring[0]) strncpy(cmd_name, jcmd->valuestring, sizeof(cmd_name) - 1);

            cJSON *jtop = cJSON_GetObjectItemCaseSensitive(req_j, "topic");
            if (!jtop) jtop = cJSON_GetObjectItemCaseSensitive(req_j, "mqttTopic");
            if (jtop && cJSON_IsString(jtop) && jtop->valuestring[0]) strncpy(req_topic, jtop->valuestring, sizeof(req_topic) - 1);

            cJSON *jp = cJSON_GetObjectItemCaseSensitive(req_j, "pulseMs");
            if (!jp) jp = cJSON_GetObjectItemCaseSensitive(req_j, "mqttPulse");
            if (jp && cJSON_IsNumber(jp) && jp->valueint > 0) req_pulse_ms = jp->valueint;
            cJSON_Delete(req_j);
        }

        if (!dev_name[0] && dev_id[0]) strcpy(dev_name, dev_id);

        char final_topic[256] = {0};
        int final_pulse_ms = 0;
        int mrc = mqtt_dispatch_button_pulse_topic(dev_id, dev_name, cmd_name, req_topic[0] ? req_topic : NULL, req_pulse_ms,
                                                  final_topic, sizeof(final_topic), &final_pulse_ms);
        if (mrc == 0) {
            cJSON *resp_j = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp_j, "ok", 1);
            cJSON_AddNumberToObject(resp_j, "pulseMs", final_pulse_ms);
            cJSON_AddStringToObject(resp_j, "topic", final_topic);
            cJSON_AddStringToObject(resp_j, "message", "MQTT test publish sent");
            send_cjson_resp(client_fd, 200, resp_j);
            cJSON_Delete(resp_j);
        } else {
            cJSON *resp_j = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp_j, "ok", 0);
            cJSON_AddStringToObject(resp_j, "error", "MQTT broker not connected");
            send_cjson_resp(client_fd, 503, resp_j);
            cJSON_Delete(resp_j);
        }
        close(client_fd);
        return;
    }

    /* Route: /api/rf/* proxy to codex_rf (127.0.0.1:8092) */
    if (strncmp(clean_path, "/api/rf", 7) == 0) {
        int rfsock = socket(AF_INET, SOCK_STREAM, 0);
        if (rfsock >= 0) {
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(8092);
            sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(rfsock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(rfsock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            if (connect(rfsock, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
                char proxy_req[512];
                int body_len = (body && *body) ? (int)strlen(body) : 0;
                if (body_len > 0) {
                    int prlen = snprintf(proxy_req, sizeof(proxy_req),
                        "%s %s HTTP/1.1\r\nHost: 127.0.0.1:8092\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                        method, path, body_len);
                    if (prlen > 0) write(rfsock, proxy_req, (size_t)prlen);
                    write(rfsock, body, (size_t)body_len);
                } else {
                    int prlen = snprintf(proxy_req, sizeof(proxy_req),
                        "%s %s HTTP/1.1\r\nHost: 127.0.0.1:8092\r\nConnection: close\r\n\r\n",
                        method, path);
                    if (prlen > 0) write(rfsock, proxy_req, (size_t)prlen);
                }

                char resp_buf[1024];
                ssize_t rn;
                while ((rn = read(rfsock, resp_buf, sizeof(resp_buf))) > 0) {
                    write(client_fd, resp_buf, rn);
                }
                close(rfsock);
                close(client_fd);
                return;
            }
            close(rfsock);
        }
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", 1);
        cJSON_AddBoolToObject(err, "running", 0);
        cJSON_AddStringToObject(err, "state", "stopped");
        cJSON_AddBoolToObject(err, "connected", 0);
        cJSON_AddBoolToObject(err, "pairing_active", 0);
        cJSON_AddBoolToObject(err, "paired", 0);
        cJSON_AddStringToObject(err, "rf_address", "None");
        cJSON_AddStringToObject(err, "hub_rf_address", "None");
        cJSON *slots = cJSON_AddArrayToObject(err, "slots");
        cJSON *s1 = cJSON_CreateObject();
        cJSON_AddNumberToObject(s1, "slot", 1);
        cJSON_AddBoolToObject(s1, "active", 0);
        cJSON_AddStringToObject(s1, "address", "None");
        cJSON_AddStringToObject(s1, "rf_address", "None");
        cJSON_AddItemToArray(slots, s1);
        cJSON *s2 = cJSON_CreateObject();
        cJSON_AddNumberToObject(s2, "slot", 2);
        cJSON_AddBoolToObject(s2, "active", 0);
        cJSON_AddStringToObject(s2, "address", "None");
        cJSON_AddStringToObject(s2, "rf_address", "None");
        cJSON_AddItemToArray(slots, s2);
        send_cjson_resp(client_fd, 200, err);
        cJSON_Delete(err);
        close(client_fd);
        return;
    }

    /* Route: POST /api/button-press or POST /api/mqtt-publish */
    if ((strcmp(clean_path, "/api/button-press") == 0 || strcmp(clean_path, "/api/mqtt-publish") == 0) &&
        strcmp(method, "POST") == 0) {
        char dev_id[64] = {0};
        char dev_name[128] = {0};
        char cmd_name[128] = {0};
        int req_pulse_ms = 0;

        char req_topic[256] = {0};
        cJSON *req_j = body ? cJSON_Parse(body) : NULL;
        if (req_j) {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(req_j, "deviceId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(req_j, "id");
            if (jid && cJSON_IsString(jid) && jid->valuestring) strncpy(dev_id, jid->valuestring, sizeof(dev_id) - 1);
            else if (jid && cJSON_IsNumber(jid)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jid->valuedouble);

            cJSON *jdev = cJSON_GetObjectItemCaseSensitive(req_j, "device");
            if (jdev && cJSON_IsString(jdev) && jdev->valuestring) strncpy(dev_name, jdev->valuestring, sizeof(dev_name) - 1);

            cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "command");
            if (jcmd && cJSON_IsString(jcmd) && jcmd->valuestring) strncpy(cmd_name, jcmd->valuestring, sizeof(cmd_name) - 1);

            cJSON *jtop = cJSON_GetObjectItemCaseSensitive(req_j, "topic");
            if (!jtop) jtop = cJSON_GetObjectItemCaseSensitive(req_j, "mqttTopic");
            if (jtop && cJSON_IsString(jtop) && jtop->valuestring[0]) strncpy(req_topic, jtop->valuestring, sizeof(req_topic) - 1);

            cJSON *jp = cJSON_GetObjectItemCaseSensitive(req_j, "pulseMs");
            if (!jp) jp = cJSON_GetObjectItemCaseSensitive(req_j, "pulse_ms");
            if (jp && cJSON_IsNumber(jp) && jp->valueint > 0) req_pulse_ms = jp->valueint;
            cJSON_Delete(req_j);
        }

        if (!cmd_name[0]) {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddStringToObject(err, "error", "Missing command parameter");
            send_cjson_resp(client_fd, 400, err);
            cJSON_Delete(err);
            close(client_fd);
            return;
        }

        if (!dev_name[0]) {
            if (dev_id[0]) strcpy(dev_name, dev_id);
            else strcpy(dev_name, "device");
        }

        char final_topic[256] = {0};
        int final_pulse_ms = 0;
        int mrc = mqtt_dispatch_button_pulse_topic(dev_id, dev_name, cmd_name, req_topic[0] ? req_topic : NULL, req_pulse_ms,
                                                  final_topic, sizeof(final_topic), &final_pulse_ms);
        if (mrc == 0) {
            cJSON *resp_j = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp_j, "ok", 1);
            cJSON_AddNumberToObject(resp_j, "pulseMs", final_pulse_ms);
            cJSON_AddStringToObject(resp_j, "topic", final_topic);
            cJSON_AddStringToObject(resp_j, "reply", "ok");
            send_cjson_resp(client_fd, 200, resp_j);
            cJSON_Delete(resp_j);
        } else {
            cJSON *resp_j = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp_j, "ok", 0);
            cJSON_AddStringToObject(resp_j, "error", "MQTT broker not connected");
            send_cjson_resp(client_fd, 503, resp_j);
            cJSON_Delete(resp_j);
        }
        close(client_fd);
        return;
    }

    /* Route: POST /mqtt or POST /api/mqtt-save */
    if ((strcmp(clean_path, "/mqtt") == 0 || strcmp(clean_path, "/api/mqtt-save") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        if (params) {
            cJSON *cfg = NULL;
            FILE *f = fopen("/data/codexmqtt/config.json", "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < 65536) {
                    char *b = (char *)malloc((size_t)sz + 1);
                    if (b) {
                        size_t nr = fread(b, 1, (size_t)sz, f);
                        b[nr] = '\0';
                        cfg = cJSON_Parse(b);
                        free(b);
                    }
                }
                fclose(f);
            }
            if (!cfg) cfg = cJSON_CreateObject();

            cJSON *p_en = cJSON_GetObjectItemCaseSensitive(params, "enabled");
            if (p_en) cJSON_ReplaceItemInObject(cfg, "enabled", cJSON_CreateBool(cJSON_IsTrue(p_en) || (cJSON_IsNumber(p_en) && p_en->valueint > 0) || (cJSON_IsString(p_en) && strcmp(p_en->valuestring, "1") == 0)));

            cJSON *p_host = cJSON_GetObjectItemCaseSensitive(params, "host");
            if (p_host && cJSON_IsString(p_host) && p_host->valuestring) {
                cJSON_DeleteItemFromObject(cfg, "host");
                cJSON_AddStringToObject(cfg, "host", p_host->valuestring);
            }

            cJSON *p_port = cJSON_GetObjectItemCaseSensitive(params, "port");
            if (p_port) {
                int pnum = cJSON_IsNumber(p_port) ? p_port->valueint : (cJSON_IsString(p_port) ? atoi(p_port->valuestring) : 1883);
                cJSON_DeleteItemFromObject(cfg, "port");
                cJSON_AddNumberToObject(cfg, "port", pnum > 0 ? pnum : 1883);
            }

            cJSON *p_user = cJSON_GetObjectItemCaseSensitive(params, "username");
            if (p_user && cJSON_IsString(p_user)) {
                cJSON_DeleteItemFromObject(cfg, "username");
                cJSON_AddStringToObject(cfg, "username", p_user->valuestring);
            }

            cJSON *p_keep = cJSON_GetObjectItemCaseSensitive(params, "keep_password");
            int keep = (p_keep && (cJSON_IsTrue(p_keep) || (cJSON_IsString(p_keep) && strcmp(p_keep->valuestring, "1") == 0)));
            cJSON *p_pass = cJSON_GetObjectItemCaseSensitive(params, "password");
            if (!keep && p_pass && cJSON_IsString(p_pass) && p_pass->valuestring[0]) {
                cJSON_DeleteItemFromObject(cfg, "password");
                cJSON_AddStringToObject(cfg, "password", p_pass->valuestring);
            }

            cJSON *p_bt = cJSON_GetObjectItemCaseSensitive(params, "baseTopic");
            if (p_bt && cJSON_IsString(p_bt) && p_bt->valuestring[0]) {
                cJSON_DeleteItemFromObject(cfg, "baseTopic");
                cJSON_AddStringToObject(cfg, "baseTopic", p_bt->valuestring);
            }

            cJSON *p_cid = cJSON_GetObjectItemCaseSensitive(params, "clientId");
            if (p_cid && cJSON_IsString(p_cid) && p_cid->valuestring[0]) {
                cJSON_DeleteItemFromObject(cfg, "clientId");
                cJSON_AddStringToObject(cfg, "clientId", p_cid->valuestring);
            }

            cJSON *p_disc = cJSON_GetObjectItemCaseSensitive(params, "discoveryPrefix");
            if (p_disc && cJSON_IsString(p_disc) && p_disc->valuestring[0]) {
                cJSON_DeleteItemFromObject(cfg, "discoveryPrefix");
                cJSON_AddStringToObject(cfg, "discoveryPrefix", p_disc->valuestring);
            }

            cJSON *p_name = cJSON_GetObjectItemCaseSensitive(params, "name");
            if (p_name && cJSON_IsString(p_name) && p_name->valuestring[0]) {
                cJSON_DeleteItemFromObject(cfg, "name");
                cJSON_AddStringToObject(cfg, "name", p_name->valuestring);
            }

            cJSON *p_ka = cJSON_GetObjectItemCaseSensitive(params, "keepAlive");
            if (p_ka) {
                int kanum = cJSON_IsNumber(p_ka) ? p_ka->valueint : (cJSON_IsString(p_ka) ? atoi(p_ka->valuestring) : 60);
                cJSON_DeleteItemFromObject(cfg, "keepAlive");
                cJSON_AddNumberToObject(cfg, "keepAlive", kanum > 0 ? kanum : 60);
            }

            cJSON *p_ha = cJSON_GetObjectItemCaseSensitive(params, "haDiscovery");
            if (p_ha) {
                int ha_val = cJSON_IsTrue(p_ha) || (cJSON_IsNumber(p_ha) && p_ha->valueint > 0) || (cJSON_IsString(p_ha) && strcmp(p_ha->valuestring, "1") == 0);
                cJSON_DeleteItemFromObject(cfg, "haDiscovery");
                cJSON_AddBoolToObject(cfg, "haDiscovery", ha_val);
            }

            cJSON *p_btr = cJSON_GetObjectItemCaseSensitive(params, "btRemoteEvents");
            if (p_btr) {
                int btr_val = cJSON_IsTrue(p_btr) || (cJSON_IsNumber(p_btr) && p_btr->valueint > 0) || (cJSON_IsString(p_btr) && strcmp(p_btr->valuestring, "1") == 0);
                cJSON_DeleteItemFromObject(cfg, "btRemoteEvents");
                cJSON_AddBoolToObject(cfg, "btRemoteEvents", btr_val);
            }

            /* Clean out obsolete pollSeconds and mqttDevices */
            cJSON_DeleteItemFromObject(cfg, "pollSeconds");
            cJSON_DeleteItemFromObject(cfg, "mqttDevices");
            cJSON_DeleteItemFromObject(cfg, "devices");

            /* Keep broker nested object in sync if present */
            cJSON *broker = cJSON_GetObjectItemCaseSensitive(cfg, "broker");
            if (broker && cJSON_IsObject(broker)) {
                cJSON *h = cJSON_GetObjectItemCaseSensitive(cfg, "host");
                if (h && cJSON_IsString(h)) {
                    cJSON_DeleteItemFromObject(broker, "host");
                    cJSON_AddStringToObject(broker, "host", h->valuestring);
                }
                cJSON *pt = cJSON_GetObjectItemCaseSensitive(cfg, "port");
                if (pt && cJSON_IsNumber(pt)) {
                    cJSON_DeleteItemFromObject(broker, "port");
                    cJSON_AddNumberToObject(broker, "port", pt->valueint);
                }
                cJSON *u = cJSON_GetObjectItemCaseSensitive(cfg, "username");
                if (u && cJSON_IsString(u)) {
                    cJSON_DeleteItemFromObject(broker, "username");
                    cJSON_AddStringToObject(broker, "username", u->valuestring);
                }
                cJSON *pw = cJSON_GetObjectItemCaseSensitive(cfg, "password");
                if (pw && cJSON_IsString(pw)) {
                    cJSON_DeleteItemFromObject(broker, "password");
                    cJSON_AddStringToObject(broker, "password", pw->valuestring);
                }
            }

            char *out_json = cJSON_Print(cfg);
            if (out_json) {
                system("mkdir -p /data/codexmqtt 2>/dev/null");
                write_file_content("/data/codexmqtt/config.json", out_json);
                free(out_json);
            }
            cJSON_Delete(cfg);
            cJSON_Delete(params);

            extern void daemon_sync_mqtt(void);
            daemon_sync_mqtt();

            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "message", "MQTT settings saved");
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
            close(client_fd);
            return;
        }
    }

    /* Route: POST /wifi or POST /api/wifi-save */
    if ((strcmp(clean_path, "/wifi") == 0 || strcmp(clean_path, "/api/wifi-save") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        if (params) {
            cJSON *p_ssid = cJSON_GetObjectItemCaseSensitive(params, "ssid");
            cJSON *p_pass = cJSON_GetObjectItemCaseSensitive(params, "password");
            if (p_ssid && cJSON_IsString(p_ssid) && p_ssid->valuestring[0]) {
                char wpa_buf[1024];
                snprintf(wpa_buf, sizeof(wpa_buf),
                    "ctrl_interface=/var/run/wpa_supplicant\n"
                    "update_config=1\n\n"
                    "network={\n"
                    "\tssid=\"%s\"\n"
                    "\tpsk=\"%s\"\n"
                    "}\n",
                    p_ssid->valuestring,
                    (p_pass && cJSON_IsString(p_pass)) ? p_pass->valuestring : "");
                write_file_content("/etc/wpa_supplicant.conf", wpa_buf);
                write_file_content("/etc/wpa_supplicant.ath0.conf", wpa_buf);

                cJSON *p_app = cJSON_GetObjectItemCaseSensitive(params, "apply");
                if (p_app && cJSON_IsString(p_app) && (strcmp(p_app->valuestring, "apply") == 0 || strcmp(p_app->valuestring, "1") == 0)) {
                    system("killall -HUP wpa_supplicant 2>/dev/null &");
                }
            }
            cJSON_Delete(params);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "message", "Wi-Fi settings saved");
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
            close(client_fd);
            return;
        }
    }

    /* Route: POST /ethernet or POST /api/ethernet-save */
    if ((strcmp(clean_path, "/ethernet") == 0 || strcmp(clean_path, "/api/ethernet-save") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        if (params) {
            char conf_buf[1024];
            cJSON *p_en = cJSON_GetObjectItemCaseSensitive(params, "eth_enabled");
            cJSON *p_fb = cJSON_GetObjectItemCaseSensitive(params, "eth_fallback_wifi");
            cJSON *p_mode = cJSON_GetObjectItemCaseSensitive(params, "eth_mode");
            cJSON *p_ip = cJSON_GetObjectItemCaseSensitive(params, "eth_ip");
            cJSON *p_mask = cJSON_GetObjectItemCaseSensitive(params, "eth_netmask");
            cJSON *p_gw = cJSON_GetObjectItemCaseSensitive(params, "eth_gateway");
            cJSON *p_dns = cJSON_GetObjectItemCaseSensitive(params, "eth_dns");

            snprintf(conf_buf, sizeof(conf_buf),
                "ETH_ENABLED=1\n"
                "ETH_FALLBACK_WIFI=1\n"
                "ETH_MODE=\"%s\"\n"
                "ETH_IP=\"%s\"\n"
                "ETH_NETMASK=\"%s\"\n"
                "ETH_GATEWAY=\"%s\"\n"
                "ETH_DNS=\"%s\"\n",
                (p_mode && cJSON_IsString(p_mode)) ? p_mode->valuestring : "dhcp",
                (p_ip && cJSON_IsString(p_ip)) ? p_ip->valuestring : "",
                (p_mask && cJSON_IsString(p_mask)) ? p_mask->valuestring : "255.255.255.0",
                (p_gw && cJSON_IsString(p_gw)) ? p_gw->valuestring : "",
                (p_dns && cJSON_IsString(p_dns)) ? p_dns->valuestring : "");

            write_file_content("/data/codex/ethernet.conf", conf_buf);

            cJSON *p_app = cJSON_GetObjectItemCaseSensitive(params, "apply");
            if (p_app && cJSON_IsString(p_app) && (strcmp(p_app->valuestring, "apply") == 0 || strcmp(p_app->valuestring, "1") == 0 || strcmp(p_app->valuestring, "reconfigure") == 0 || strcmp(p_app->valuestring, "restart") == 0)) {
                system("/data/codex/bin/network_manager.sh apply 2>/dev/null || /data/codex/network_manager.sh apply 2>/dev/null &");
            }
            cJSON_Delete(params);

            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "message", "Ethernet configuration saved");
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
            close(client_fd);
            return;
        }
    }

    /* Route: POST /system or POST /api/system-action */
    if ((strcmp(clean_path, "/system") == 0 || strcmp(clean_path, "/api/system-action") == 0) && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        if (params) {
            cJSON *p_act = cJSON_GetObjectItemCaseSensitive(params, "action");
            if (p_act && cJSON_IsString(p_act) && strcmp(p_act->valuestring, "reboot") == 0) {
                cJSON_Delete(params);
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "ok", 1);
                cJSON_AddStringToObject(resp, "message", "Rebooting hub now");
                send_cjson_resp(client_fd, 200, resp);
                cJSON_Delete(resp);
                close(client_fd);
                system("reboot 2>/dev/null &");
                return;
            } else if (p_act && cJSON_IsString(p_act) && strcmp(p_act->valuestring, "factory_reset") == 0) {
                cJSON *p_conf = cJSON_GetObjectItemCaseSensitive(params, "confirm");
                if (p_conf && cJSON_IsString(p_conf) && strcmp(p_conf->valuestring, "RESET") == 0) {
                    cJSON_Delete(params);
                    cJSON *resp = cJSON_CreateObject();
                    cJSON_AddBoolToObject(resp, "ok", 1);
                    cJSON_AddStringToObject(resp, "message", "Factory reset initiated. Reverting to stock firmware and rebooting.");
                    send_cjson_resp(client_fd, 200, resp);
                    cJSON_Delete(resp);
                    close(client_fd);
                    system("/data/codex/bin/factory_reset.sh --confirm 2>/dev/null || /data/codex/factory_reset.sh --confirm 2>/dev/null &");
                    return;
                } else {
                    cJSON_Delete(params);
                    cJSON *resp = cJSON_CreateObject();
                    cJSON_AddBoolToObject(resp, "ok", 0);
                    cJSON_AddStringToObject(resp, "error", "Confirmation parameter 'RESET' required");
                    send_cjson_resp(client_fd, 400, resp);
                    cJSON_Delete(resp);
                    return;
                }
            } else if (p_act && cJSON_IsString(p_act) && strcmp(p_act->valuestring, "mqtt_publish") == 0) {
                cJSON *j_top = cJSON_GetObjectItemCaseSensitive(params, "topic");
                cJSON *j_pay = cJSON_GetObjectItemCaseSensitive(params, "payload");
                cJSON *j_ret = cJSON_GetObjectItemCaseSensitive(params, "retain");
                const char *top = (j_top && cJSON_IsString(j_top)) ? j_top->valuestring : "";
                const char *pay = (j_pay && cJSON_IsString(j_pay)) ? j_pay->valuestring : "";
                int retain = 0;
                if (j_ret) {
                    retain = (cJSON_IsTrue(j_ret) || (cJSON_IsNumber(j_ret) && j_ret->valueint > 0)) ? 1 : 0;
                } else if (strstr(top, "/status") || strstr(top, "/state") || strstr(top, "/activity")) {
                    retain = 1;
                }
                if (top[0]) {
                    mqtt_publish(top, pay, retain);
                }
                cJSON_Delete(params);
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "ok", 1);
                send_cjson_resp(client_fd, 200, resp);
                cJSON_Delete(resp);
                close(client_fd);
                return;
            }
            cJSON_Delete(params);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: GET /export/<target> */
    if (strncmp(clean_path, "/export/", 8) == 0 && strcmp(method, "GET") == 0) {
        const char *tgt = path + 8;
        if (strcmp(tgt, "bundle") == 0) {
            cJSON *bundle = cJSON_CreateObject();
            cJSON_AddNumberToObject(bundle, "version", 1);
            cJSON_AddNumberToObject(bundle, "timestamp", (double)time(NULL));

            static const struct { const char *key; const char *path; const char *fallback; } b_files[] = {
                { "devices", "/data/codex/config/devices.json", NULL },
                { "activities", "/data/codex/config/activities.json", NULL },
                { "settings", "/data/codex/config/settings.json", NULL },

                { "mqtt", "/data/codexmqtt/config.json", NULL },
                { "network", "/data/codex/ethernet.conf", NULL },
                { "wifi", "/etc/wpa_supplicant.conf", NULL },
                { "bluetooth", "/data/codex/bt_devices.json", NULL },
                { "bt_remote_map", "/data/codex/bt_remote_map.json", NULL },
                { "elite_remote_map", "/data/codex/elite_remote_map.json", NULL },
                { "ui_remote_layout", "/data/codex/web_remote_layouts.json", NULL },
                { "auth", "/data/codex/webui_auth.conf", NULL },
                { "hub_id", "/data/codex/hub_id", NULL },
                { NULL, NULL, NULL }
            };

            for (int i = 0; b_files[i].key; i++) {
                const char *use_path = b_files[i].path;
                if (access(use_path, F_OK) != 0 && b_files[i].fallback && access(b_files[i].fallback, F_OK) == 0) {
                    use_path = b_files[i].fallback;
                }
                size_t flen = 0;
                char *txt = read_file_text_alloc(use_path, 2000000, &flen);
                if (txt) {
                    cJSON *parsed = cJSON_Parse(txt);
                    if (parsed) {
                        cJSON_AddItemToObject(bundle, b_files[i].key, parsed);
                    } else {
                        cJSON_AddStringToObject(bundle, b_files[i].key, txt);
                    }
                    free(txt);
                }
            }

            // Also include binary btstack keys in base64 if present
            FILE *kf = fopen("/data/codex/btstack_keys.bin", "rb");
            if (!kf) kf = fopen("/data/codex/btstack_keys.tlv", "rb");
            if (kf) {
                fseek(kf, 0, SEEK_END);
                long ksz = ftell(kf);
                fseek(kf, 0, SEEK_SET);
                if (ksz > 0 && ksz < 200000) {
                    unsigned char *kbuf = (unsigned char *)malloc((size_t)ksz);
                    if (kbuf) {
                        size_t kr = fread(kbuf, 1, (size_t)ksz, kf);
                        char *b64 = b64_encode(kbuf, kr);
                        if (b64) {
                            cJSON_AddStringToObject(bundle, "btstack_keys", b64);
                            free(b64);
                        }
                        free(kbuf);
                    }
                }
                fclose(kf);
            }

            char *json_out = cJSON_Print(bundle);
            cJSON_Delete(bundle);
            if (json_out) {
                size_t jlen = strlen(json_out);
                uint32_t crc = crc32_simple((const unsigned char *)json_out, jlen);
                char header[512];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json; charset=utf-8\r\n"
                    "Content-Disposition: attachment; filename=\"harmony_backup_bundle.json\"\r\n"
                    "X-Checksum-CRC32: %08x\r\n"
                    "Content-Length: %zu\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n",
                    crc, jlen);
                send(client_fd, header, (size_t)hlen, 0);
                send(client_fd, json_out, jlen, 0);
                free(json_out);
                close(client_fd);
                return;
            }
        }

        const char *file_to_send = NULL;
        const char *download_name = "backup.json";
        const char *ctype = "application/json; charset=utf-8";

        if (strcmp(tgt, "devices") == 0) {
            file_to_send = "/data/codex/config/devices.json";
            download_name = "devices.json";
        }
        else if (strcmp(tgt, "activities") == 0) {
            file_to_send = "/data/codex/config/activities.json";
            download_name = "activities.json";
        }

        else if (strcmp(tgt, "settings") == 0) {
            file_to_send = "/data/codex/config/settings.json";
            download_name = "settings.json";
        }
        else if (strcmp(tgt, "mqtt") == 0) { file_to_send = "/data/codexmqtt/config.json"; download_name = "mqtt_config.json"; }
        else if (strcmp(tgt, "network") == 0 || strcmp(tgt, "ethernet") == 0) {
            file_to_send = "/data/codex/ethernet.conf";
            download_name = "ethernet.conf";
            ctype = "text/plain; charset=utf-8";
        }
        else if (strcmp(tgt, "wifi") == 0) {
            file_to_send = "/etc/wpa_supplicant.conf";
            download_name = "wpa_supplicant.conf";
            ctype = "text/plain; charset=utf-8";
        }
        else if (strcmp(tgt, "bluetooth") == 0) { file_to_send = "/data/codex/bt_devices.json"; download_name = "bt_devices.json"; }
        else if (strcmp(tgt, "remote-mapping") == 0 || strcmp(tgt, "bt_remote_map") == 0) {
            file_to_send = "/data/codex/bt_remote_map.json";
            download_name = "bt_remote_map.json";
        }
        else if (strcmp(tgt, "elite_remote_map") == 0) {
            file_to_send = "/data/codex/elite_remote_map.json";
            download_name = "elite_remote_map.json";
        }
        else if (strcmp(tgt, "ui-remote-layout") == 0 || strcmp(tgt, "ui_remote_layout") == 0) {
            file_to_send = "/data/codex/web_remote_layouts.json";
            download_name = "web_remote_layouts.json";
        }
        else if (strcmp(tgt, "auth") == 0) {
            file_to_send = "/data/codex/webui_auth.conf";
            download_name = "webui_auth.conf";
            ctype = "text/plain; charset=utf-8";
        }
        else if (strcmp(tgt, "hub-id") == 0 || strcmp(tgt, "hub_id") == 0) {
            file_to_send = "/data/codex/hub_id";
            download_name = "hub_id";
            ctype = "text/plain; charset=utf-8";
        }

        if (file_to_send) {
            size_t flen = 0;
            char *buf = read_file_text_alloc(file_to_send, 2000000, &flen);
            if (buf) {
                char header[512];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Disposition: attachment; filename=\"%s\"\r\n"
                    "Content-Length: %zu\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n",
                    ctype, download_name, flen);
                send(client_fd, header, (size_t)hlen, 0);
                send(client_fd, buf, flen, 0);
                free(buf);
                close(client_fd);
                return;
            }
        }
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Export target not found");
        send_cjson_resp(client_fd, 404, err);
        cJSON_Delete(err);
        close(client_fd);
        return;
    }

    /* Route: POST /import */
    if (strcmp(clean_path, "/import") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int success = 0;
        if (params) {
            cJSON *p_tgt = cJSON_GetObjectItemCaseSensitive(params, "target");
            cJSON *p_pay = cJSON_GetObjectItemCaseSensitive(params, "payload");
            const char *tgt = p_tgt ? (cJSON_IsString(p_tgt) ? p_tgt->valuestring : "") : "";

            if (strcmp(tgt, "bundle") == 0) {
                cJSON *bundle = NULL;
                if (p_pay && cJSON_IsString(p_pay)) bundle = cJSON_Parse(p_pay->valuestring);
                else if (p_pay && cJSON_IsObject(p_pay)) bundle = p_pay;
                else bundle = params;

                if (bundle) {
                    static const struct { const char *key; const char *path; } b_files[] = {
                        { "devices", "/data/codex/config/devices.json" },
                        { "activities", "/data/codex/config/activities.json" },
                        { "settings", "/data/codex/config/settings.json" },
                        { "mqtt", "/data/codexmqtt/config.json" },
                        { "network", "/data/codex/ethernet.conf" },
                        { "wifi", "/etc/wpa_supplicant.conf" },
                        { "bluetooth", "/data/codex/bt_devices.json" },
                        { "bt_remote_map", "/data/codex/bt_remote_map.json" },
                        { "elite_remote_map", "/data/codex/elite_remote_map.json" },
                        { "ui_remote_layout", "/data/codex/web_remote_layouts.json" },
                        { "auth", "/data/codex/webui_auth.conf" },
                        { "hub_id", "/data/codex/hub_id" },
                        { NULL, NULL }
                    };

                    for (int i = 0; b_files[i].key; i++) {
                        cJSON *sec = cJSON_GetObjectItemCaseSensitive(bundle, b_files[i].key);
                        if (sec) {
                            if (cJSON_IsString(sec)) {
                                write_file_atomic(b_files[i].path, sec->valuestring, strlen(sec->valuestring));
                            } else {
                                char *pstr = cJSON_PrintUnformatted(sec);
                                if (pstr) {
                                    write_file_atomic(b_files[i].path, pstr, strlen(pstr));
                                    free(pstr);
                                }
                            }
                        }
                    }

                    cJSON *j_keys = cJSON_GetObjectItemCaseSensitive(bundle, "btstack_keys");
                    if (j_keys && cJSON_IsString(j_keys) && j_keys->valuestring[0]) {
                        size_t klen = 0;
                        unsigned char *raw_keys = b64_decode(j_keys->valuestring, &klen);
                        if (raw_keys) {
                            write_file_atomic("/data/codex/btstack_keys.bin", (const char *)raw_keys, klen);
                            free(raw_keys);
                        }
                    }

                    invalidate_device_list_cache();
                    config_store_init();
                    notify_rf_push_config();
                    success = 1;
                }
                if (bundle && bundle != p_pay && bundle != params) cJSON_Delete(bundle);
            } else if (tgt[0] && p_pay) {
                const char *file_to_write = NULL;
                if (strcmp(tgt, "devices") == 0) file_to_write = "/data/codex/config/devices.json";
                else if (strcmp(tgt, "activities") == 0) file_to_write = "/data/codex/config/activities.json";
                else if (strcmp(tgt, "settings") == 0) file_to_write = "/data/codex/config/settings.json";
                else if (strcmp(tgt, "mqtt") == 0) file_to_write = "/data/codexmqtt/config.json";
                else if (strcmp(tgt, "network") == 0 || strcmp(tgt, "ethernet") == 0) file_to_write = "/data/codex/ethernet.conf";
                else if (strcmp(tgt, "wifi") == 0) file_to_write = "/etc/wpa_supplicant.conf";
                else if (strcmp(tgt, "bluetooth") == 0) file_to_write = "/data/codex/bt_devices.json";
                else if (strcmp(tgt, "remote-mapping") == 0 || strcmp(tgt, "bt_remote_map") == 0) file_to_write = "/data/codex/bt_remote_map.json";
                else if (strcmp(tgt, "elite_remote_map") == 0) file_to_write = "/data/codex/elite_remote_map.json";
                else if (strcmp(tgt, "ui-remote-layout") == 0 || strcmp(tgt, "ui_remote_layout") == 0) file_to_write = "/data/codex/web_remote_layouts.json";
                else if (strcmp(tgt, "auth") == 0) file_to_write = "/data/codex/webui_auth.conf";
                else if (strcmp(tgt, "hub_id") == 0 || strcmp(tgt, "hub-id") == 0) file_to_write = "/data/codex/hub_id";

                if (file_to_write) {
                    if (cJSON_IsString(p_pay)) {
                        write_file_atomic(file_to_write, p_pay->valuestring, strlen(p_pay->valuestring));
                        success = 1;
                    } else {
                        char *pstr = cJSON_PrintUnformatted(p_pay);
                        if (pstr) {
                            write_file_atomic(file_to_write, pstr, strlen(pstr));
                            free(pstr);
                            success = 1;
                        }
                    }
                    if (success) {
                        invalidate_device_list_cache();
                        config_store_init();
                        if (strcmp(tgt, "devices") == 0 || strcmp(tgt, "activities") == 0 || strcmp(tgt, "elite_remote_map") == 0) {
                            notify_rf_push_config();
                        }
                    }
                }
            }
            cJSON_Delete(params);
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        cJSON_AddStringToObject(resp, "message", success ? "Restored successfully" : "Import failed");
        send_cjson_resp(client_fd, success ? 200 : 400, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /api/remote-mapping-save (Homatics B25 BT remote) */
    if (strcmp(clean_path, "/api/remote-mapping-save") == 0 && strcmp(method, "POST") == 0) {
        if (body && body[0]) {
            system("mkdir -p /data/codex 2>/dev/null");
            write_file_atomic("/data/codex/bt_remote_map.json", body, strlen(body));
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /api/elite-mapping-save (Harmony Elite RF remote) */
    if (strcmp(clean_path, "/api/elite-mapping-save") == 0 && strcmp(method, "POST") == 0) {
        if (body && body[0]) {
            system("mkdir -p /data/codex 2>/dev/null");
            write_file_atomic("/data/codex/elite_remote_map.json", body, strlen(body));
            notify_rf_push_config();
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: GET /api/ui-remote-layout and POST /api/ui-remote-layout */
    if (strcmp(clean_path, "/api/ui-remote-layout") == 0) {
        if (strcmp(method, "POST") == 0) {
            if (body && body[0]) {
                system("mkdir -p /data/codex 2>/dev/null");
                write_file_atomic("/data/codex/web_remote_layouts.json", body, strlen(body));
            }
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", 1);
            send_cjson_resp(client_fd, 200, resp);
            cJSON_Delete(resp);
            close(client_fd);
            return;
        } else if (strcmp(method, "GET") == 0) {
            size_t flen = 0;
            char *buf = read_file_text_alloc("/data/codex/web_remote_layouts.json", 200000, &flen);
            if (buf) {
                cJSON *mj = cJSON_Parse(buf);
                free(buf);
                if (mj) {
                    send_cjson_resp(client_fd, 200, mj);
                    cJSON_Delete(mj);
                    close(client_fd);
                    return;
                }
            }
            cJSON *def = cJSON_CreateObject();
            cJSON_AddBoolToObject(def, "ok", 1);
            cJSON_AddItemToObject(def, "layouts", cJSON_CreateObject());
            send_cjson_resp(client_fd, 200, def);
            cJSON_Delete(def);
            close(client_fd);
            return;
        }
    }

    /* Route: POST /api/activity-save */
    if (strcmp(clean_path, "/api/activity-save") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int success = 0;
        if (params) {
            cJSON *p_id = cJSON_GetObjectItemCaseSensitive(params, "id");
            if (!p_id) p_id = cJSON_GetObjectItemCaseSensitive(params, "activityId");
            const char *act_id_str = p_id ? (cJSON_IsString(p_id) ? p_id->valuestring : "") : "";
            char id_buf[32] = {0};
            if (!act_id_str[0] && p_id && cJSON_IsNumber(p_id)) {
                snprintf(id_buf, sizeof(id_buf), "%ld", (long)p_id->valuedouble);
                act_id_str = id_buf;
            }

            cJSON *p_name = cJSON_GetObjectItemCaseSensitive(params, "name");
            const char *name_str = (p_name && cJSON_IsString(p_name)) ? p_name->valuestring : "";

            cJSON *p_type = cJSON_GetObjectItemCaseSensitive(params, "type");
            const char *type_str = (p_type && cJSON_IsString(p_type)) ? p_type->valuestring : "Custom";

            cJSON *p_icon = cJSON_GetObjectItemCaseSensitive(params, "icon");
            const char *icon_str = (p_icon && cJSON_IsString(p_icon)) ? p_icon->valuestring : "Default";

            cJSON *p_order = cJSON_GetObjectItemCaseSensitive(params, "order");
            if (!p_order) p_order = cJSON_GetObjectItemCaseSensitive(params, "ActivityOrder");
            int order_val = (p_order && cJSON_IsNumber(p_order)) ? p_order->valueint :
                            (p_order && cJSON_IsString(p_order) ? atoi(p_order->valuestring) : 1);

            cJSON *p_enter = cJSON_GetObjectItemCaseSensitive(params, "startSequence");
            if (!p_enter) p_enter = cJSON_GetObjectItemCaseSensitive(params, "startSteps");
            if (!p_enter) p_enter = cJSON_GetObjectItemCaseSensitive(params, "enterSequence");
            if (!p_enter) p_enter = cJSON_GetObjectItemCaseSensitive(params, "EnterActions");

            cJSON *p_leave = cJSON_GetObjectItemCaseSensitive(params, "stopSequence");
            if (!p_leave) p_leave = cJSON_GetObjectItemCaseSensitive(params, "stopSteps");
            if (!p_leave) p_leave = cJSON_GetObjectItemCaseSensitive(params, "leaveSequence");
            if (!p_leave) p_leave = cJSON_GetObjectItemCaseSensitive(params, "LeaveActions");

            cJSON *enter_steps = NULL;
            if (p_enter && cJSON_IsArray(p_enter)) enter_steps = sanitize_activity_steps_array(p_enter);
            else if (p_enter && cJSON_IsString(p_enter)) enter_steps = build_activity_steps_cjson(p_enter->valuestring);
            else enter_steps = cJSON_CreateArray();

            cJSON *leave_steps = NULL;
            if (p_leave && cJSON_IsArray(p_leave)) leave_steps = sanitize_activity_steps_array(p_leave);
            else if (p_leave && cJSON_IsString(p_leave)) leave_steps = build_activity_steps_cjson(p_leave->valuestring);
            else leave_steps = cJSON_CreateArray();

            cJSON *dev_ids_arr = cJSON_CreateArray();
            cJSON *p_devs = cJSON_GetObjectItemCaseSensitive(params, "deviceIds");
            if (!p_devs) p_devs = cJSON_GetObjectItemCaseSensitive(params, "devices");
            int dev_ids_provided = (p_devs != NULL);
            if (p_devs && cJSON_IsArray(p_devs)) {
                const cJSON *ditem = NULL;
                cJSON_ArrayForEach(ditem, p_devs) {
                    char dstr[32] = {0};
                    if (cJSON_IsString(ditem) && ditem->valuestring) strncpy(dstr, ditem->valuestring, sizeof(dstr) - 1);
                    else if (cJSON_IsNumber(ditem)) snprintf(dstr, sizeof(dstr), "%ld", (long)ditem->valuedouble);
                    if (dstr[0] && strcmp(dstr, "0") != 0) {
                        cJSON_AddItemToArray(dev_ids_arr, cJSON_CreateString(dstr));
                    }
                }
            } else if (p_devs && cJSON_IsString(p_devs) && p_devs->valuestring[0]) {
                char dev_buf[512];
                strncpy(dev_buf, p_devs->valuestring, sizeof(dev_buf) - 1);
                dev_buf[sizeof(dev_buf) - 1] = '\0';
                char *tok = strtok(dev_buf, ",; ");
                while (tok) {
                    if (tok[0] && strcmp(tok, "0") != 0) {
                        cJSON_AddItemToArray(dev_ids_arr, cJSON_CreateString(tok));
                    }
                    tok = strtok(NULL, ",; ");
                }
            }
            if (!dev_ids_provided && cJSON_GetArraySize(dev_ids_arr) == 0) {
                cJSON *arrs[2] = { enter_steps, leave_steps };
                for (int ai_idx = 0; ai_idx < 2; ai_idx++) {
                    const cJSON *st = NULL;
                    cJSON_ArrayForEach(st, arrs[ai_idx]) {
                        const cJSON *did = cJSON_GetObjectItemCaseSensitive(st, "deviceId");
                        if (did && cJSON_IsString(did) && did->valuestring[0] && strcmp(did->valuestring, "0") != 0) {
                            int exists = 0;
                            const cJSON *ex = NULL;
                            cJSON_ArrayForEach(ex, dev_ids_arr) {
                                if (ex->valuestring && strcmp(ex->valuestring, did->valuestring) == 0) { exists = 1; break; }
                            }
                            if (!exists) cJSON_AddItemToArray(dev_ids_arr, cJSON_CreateString(did->valuestring));
                        }
                    }
                }
            }

            cJSON *cached = config_store_get_activities_json();
            cJSON *root = cached ? cJSON_Duplicate(cached, 1) : cJSON_CreateObject();
            if (root) {
                cJSON *acts = cJSON_GetObjectItemCaseSensitive(root, "activities");
                if (!acts) acts = cJSON_GetObjectItemCaseSensitive(root, "Activities");
                if (!acts) acts = cJSON_AddArrayToObject(root, "activities");
                if (acts && cJSON_IsArray(acts)) {
                        cJSON *matched = NULL;
                        cJSON *ai = NULL;
                        cJSON_ArrayForEach(ai, acts) {
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(ai, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(ai, "Id-");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(ai, "Id");
                            char cur_id[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(cur_id, sizeof(cur_id), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(cur_id, jid->valuestring, sizeof(cur_id) - 1);
                            cJSON *jname = cJSON_GetObjectItemCaseSensitive(ai, "name");
                            if (!jname) jname = cJSON_GetObjectItemCaseSensitive(ai, "Name");
                            const char *cur_name = (jname && cJSON_IsString(jname)) ? jname->valuestring : "";

                            if ((act_id_str[0] && strcmp(cur_id, act_id_str) == 0) ||
                                (strcmp(act_id_str, "-1") == 0 && strcasecmp(cur_name, "PowerOff") == 0) ||
                                (name_str[0] && strcasecmp(cur_name, name_str) == 0)) {
                                matched = ai;
                                break;
                            }
                        }

                        if (matched) {
                            if (name_str[0]) {
                                cJSON_DeleteItemFromObject(matched, "name");
                                cJSON_DeleteItemFromObject(matched, "Name");
                                cJSON_AddStringToObject(matched, "name", name_str);
                            }
                            if (type_str[0]) {
                                cJSON_DeleteItemFromObject(matched, "type");
                                cJSON_DeleteItemFromObject(matched, "Type");
                                cJSON_DeleteItemFromObject(matched, "SuggestedDisplay");
                                cJSON_AddStringToObject(matched, "type", type_str);
                            }
                            if (icon_str[0]) {
                                cJSON_DeleteItemFromObject(matched, "icon");
                                cJSON_DeleteItemFromObject(matched, "Icon");
                                cJSON_AddStringToObject(matched, "icon", icon_str);
                            }
                            cJSON_DeleteItemFromObject(matched, "order");
                            cJSON_DeleteItemFromObject(matched, "Order");
                            cJSON_DeleteItemFromObject(matched, "ActivityOrder");
                            cJSON_AddNumberToObject(matched, "order", order_val);

                            cJSON_DeleteItemFromObject(matched, "deviceIds");
                            cJSON_DeleteItemFromObject(matched, "Roles");
                            cJSON_AddItemToObject(matched, "deviceIds", dev_ids_arr);

                            cJSON_DeleteItemFromObject(matched, "startSequence");
                            cJSON_DeleteItemFromObject(matched, "EnterActions");
                            cJSON_DeleteItemFromObject(matched, "StartSequence");
                            cJSON_AddItemToObject(matched, "startSequence", enter_steps);

                            cJSON_DeleteItemFromObject(matched, "stopSequence");
                            cJSON_DeleteItemFromObject(matched, "LeaveActions");
                            cJSON_DeleteItemFromObject(matched, "StopSequence");
                            cJSON_AddItemToObject(matched, "stopSequence", leave_steps);
                        } else {
                            matched = cJSON_CreateObject();
                            char new_id_str[16];
                            if (act_id_str[0]) {
                                strncpy(new_id_str, act_id_str, sizeof(new_id_str) - 1);
                                new_id_str[sizeof(new_id_str) - 1] = '\0';
                            } else {
                                int cand = 1;
                                for (; cand <= 99; cand++) {
                                    char cand_s[8];
                                    snprintf(cand_s, sizeof(cand_s), "%d", cand);
                                    int used = 0;
                                    cJSON *chk = NULL;
                                    cJSON_ArrayForEach(chk, acts) {
                                        cJSON *cid = cJSON_GetObjectItemCaseSensitive(chk, "id");
                                        if (cid && cJSON_IsString(cid) && cid->valuestring) {
                                            if (strcmp(cid->valuestring, cand_s) == 0) { used = 1; break; }
                                        }
                                    }
                                    if (!used) break;
                                }
                                snprintf(new_id_str, sizeof(new_id_str), "%d", cand);
                            }
                            cJSON_AddStringToObject(matched, "id", new_id_str);
                            cJSON_AddStringToObject(matched, "name", name_str[0] ? name_str : "Custom Activity");
                            cJSON_AddStringToObject(matched, "type", type_str);
                            cJSON_AddStringToObject(matched, "icon", icon_str);
                            cJSON_AddNumberToObject(matched, "order", order_val);
                            cJSON_AddItemToObject(matched, "deviceIds", dev_ids_arr);
                            cJSON_AddItemToObject(matched, "startSequence", enter_steps);
                            cJSON_AddItemToObject(matched, "stopSequence", leave_steps);
                            cJSON_AddItemToArray(acts, matched);
                        }

                        if (config_store_save_activities(root) == 0) {
                            success = 1;
                        } else {
                            cJSON_Delete(root);
                        }
                    } else {
                        cJSON_Delete(dev_ids_arr);
                        cJSON_Delete(enter_steps);
                        cJSON_Delete(leave_steps);
                        cJSON_Delete(root);
                    }
                } else {
                    cJSON_Delete(dev_ids_arr);
                    cJSON_Delete(enter_steps);
                    cJSON_Delete(leave_steps);
                }

            cJSON_Delete(params);
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        cJSON_AddStringToObject(resp, "message", success ? "Activity saved successfully" : "Failed to save activity");
        send_cjson_resp(client_fd, success ? 200 : 400, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /api/activity-test-sequence */
    if (strcmp(clean_path, "/api/activity-test-sequence") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int executed = 0;
        if (params) {
            cJSON *p_steps = cJSON_GetObjectItemCaseSensitive(params, "steps");
            if (!p_steps) p_steps = cJSON_GetObjectItemCaseSensitive(params, "sequence");
            cJSON *step_arr = NULL;
            if (p_steps && cJSON_IsArray(p_steps)) step_arr = p_steps;
            else if (p_steps && cJSON_IsString(p_steps)) step_arr = build_activity_steps_cjson(p_steps->valuestring);

            if (step_arr && cJSON_IsArray(step_arr)) {
                cJSON *st = NULL;
                cJSON_ArrayForEach(st, step_arr) {
                    cJSON *jtype = cJSON_GetObjectItemCaseSensitive(st, "Type");
                    if (!jtype) jtype = cJSON_GetObjectItemCaseSensitive(st, "type");
                    const char *type = (jtype && cJSON_IsString(jtype)) ? jtype->valuestring : "";

                    if (strcasecmp(type, "Switched") == 0 || strcasecmp(type, "ActivitySwitched") == 0) {
                        executed++;
                        continue;
                    }

                    cJSON *jdelay = cJSON_GetObjectItemCaseSensitive(st, "Delay");
                    if (!jdelay) jdelay = cJSON_GetObjectItemCaseSensitive(st, "Duration");
                    if (!jdelay) jdelay = cJSON_GetObjectItemCaseSensitive(st, "delay");
                    int dms = (jdelay && cJSON_IsNumber(jdelay)) ? (int)jdelay->valuedouble : 0;

                    if (strcasecmp(type, "Delay") == 0 || dms > 0) {
                        if (dms > 3000) dms = 3000;
                        if (dms > 0) usleep(dms * 1000);
                        executed++;
                        if (strcasecmp(type, "Delay") == 0) continue;
                    }

                    cJSON *jdev = cJSON_GetObjectItemCaseSensitive(st, "DeviceId");
                    if (!jdev) jdev = cJSON_GetObjectItemCaseSensitive(st, "DeviceId-");
                    if (!jdev) jdev = cJSON_GetObjectItemCaseSensitive(st, "deviceId");
                    char dev_buf[64] = {0};
                    if (jdev && cJSON_IsNumber(jdev)) snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)jdev->valuedouble);
                    else if (jdev && cJSON_IsString(jdev)) strncpy(dev_buf, jdev->valuestring, sizeof(dev_buf) - 1);

                    cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(st, "Command");
                    if (!jcmd) jcmd = cJSON_GetObjectItemCaseSensitive(st, "CommandName");
                    if (!jcmd) jcmd = cJSON_GetObjectItemCaseSensitive(st, "command");
                    const char *cmd = (jcmd && cJSON_IsString(jcmd)) ? jcmd->valuestring : "";

                    if (dev_buf[0] && cmd[0]) {
                        hw_device_command_send(dev_buf, cmd);
                        executed++;
                    }
                }
            }
            if (step_arr && step_arr != p_steps) cJSON_Delete(step_arr);
            cJSON_Delete(params);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddNumberToObject(resp, "executed", executed);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /api/activity-delete */
    if (strcmp(clean_path, "/api/activity-delete") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int success = 0;
        if (params) {
            cJSON *p_id = cJSON_GetObjectItemCaseSensitive(params, "id");
            if (p_id) {
                const char *act_id_str = cJSON_IsString(p_id) ? p_id->valuestring : "";
                char id_buf[32] = {0};
                if (!act_id_str[0] && cJSON_IsNumber(p_id)) {
                    snprintf(id_buf, sizeof(id_buf), "%ld", (long)p_id->valuedouble);
                    act_id_str = id_buf;
                }
                cJSON *root = config_store_get_activities_json();
                if (root) {
                    cJSON *acts = cJSON_GetObjectItemCaseSensitive(root, "activities");
                    if (!acts) acts = cJSON_GetObjectItemCaseSensitive(root, "Activities");
                    if (acts && cJSON_IsArray(acts)) {
                        int count = cJSON_GetArraySize(acts);
                        for (int i = 0; i < count; i++) {
                            cJSON *item = cJSON_GetArrayItem(acts, i);
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "Id-");
                            char item_id[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(item_id, sizeof(item_id), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(item_id, jid->valuestring, sizeof(item_id) - 1);
                            if (strcmp(item_id, act_id_str) == 0) {
                                cJSON_DeleteItemFromArray(acts, i);
                                config_store_save_activities(root);
                                success = 1;
                                break;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(params);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /ir/command, POST /api/command-save, or POST /bt/command */
    if ((strcmp(clean_path, "/ir/command") == 0 || strcmp(clean_path, "/api/command-save") == 0 ||
         strcmp(clean_path, "/bt/command") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        int success = 0;
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            cJSON *j_name = cJSON_GetObjectItemCaseSensitive(p, "name");
            cJSON *j_old = cJSON_GetObjectItemCaseSensitive(p, "oldName");
            cJSON *j_raw = cJSON_GetObjectItemCaseSensitive(p, "raw");
            if (!j_raw) j_raw = cJSON_GetObjectItemCaseSensitive(p, "code");
            cJSON *j_script = cJSON_GetObjectItemCaseSensitive(p, "script");
            cJSON *j_rep = cJSON_GetObjectItemCaseSensitive(p, "repeats");
            cJSON *j_port = cJSON_GetObjectItemCaseSensitive(p, "port");

            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[32] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }
            const char *name = (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : "";
            const char *old_name = (j_old && cJSON_IsString(j_old)) ? j_old->valuestring : "";
            const char *raw_data = (j_raw && cJSON_IsString(j_raw)) ? j_raw->valuestring : "";
            char macro_buf[1024] = {0};
            int is_macro = 0;
            if (j_script && cJSON_IsString(j_script) && j_script->valuestring[0]) {
                snprintf(macro_buf, sizeof(macro_buf), "BT:SCRIPT:%s", j_script->valuestring);
                raw_data = macro_buf;
                is_macro = 1;
            }
            int repeats = (j_rep && cJSON_IsNumber(j_rep)) ? (int)j_rep->valuedouble : 3;
            int port = (j_port && cJSON_IsNumber(j_port)) ? (int)j_port->valuedouble : 0;

            if (dev_id[0] && name[0]) {
                cJSON *root = config_store_get_devices_json();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *dev_item = NULL;
                        cJSON_ArrayForEach(dev_item, devs) {
                            cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
                            if (!d) d = dev_item;
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                            char d_id_str[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "commands");
                                if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "Commands");
                                if (!cmds) {
                                    cmds = cJSON_CreateArray();
                                    cJSON_AddItemToObject(dev_item, "commands", cmds);
                                }
                                cJSON *cmd_match = NULL;
                                cJSON *ci = NULL;
                                cJSON_ArrayForEach(ci, cmds) {
                                    cJSON *cn = cJSON_GetObjectItemCaseSensitive(ci, "name");
                                    if (!cn) cn = cJSON_GetObjectItemCaseSensitive(ci, "Name");
                                    if (cn && cJSON_IsString(cn) && cn->valuestring) {
                                        if ((old_name[0] && strcmp(cn->valuestring, old_name) == 0) ||
                                            strcmp(cn->valuestring, name) == 0) {
                                            cmd_match = ci;
                                            break;
                                        }
                                    }
                                }
                                cJSON *dtype = cJSON_GetObjectItemCaseSensitive(d, "type");
                                if (!dtype) dtype = cJSON_GetObjectItemCaseSensitive(d, "DeviceTypeDisplayName");
                                const char *tstr = (dtype && dtype->valuestring) ? dtype->valuestring : "";
                                cJSON *dmfg = cJSON_GetObjectItemCaseSensitive(d, "manufacturer");
                                if (!dmfg) dmfg = cJSON_GetObjectItemCaseSensitive(d, "Manufacturer");
                                const char *mstr = (dmfg && dmfg->valuestring) ? dmfg->valuestring : "";
                                int dev_is_bt = (strcasestr(tstr, "bluetooth") != NULL || strcasestr(tstr, "bt") != NULL || strcasestr(mstr, "bluetooth") != NULL);
                                int dev_is_mqtt = (strcasestr(tstr, "mqtt") != NULL || strcasestr(mstr, "mqtt") != NULL);
                                int is_bt_cmd = dev_is_bt || (strncmp(raw_data, "G:HID", 5) == 0) || is_macro;
                                int is_mqtt_cmd = dev_is_mqtt || (strncmp(raw_data, "G:MQTT", 6) == 0);

                                if (cmd_match) {
                                    if (cJSON_GetObjectItemCaseSensitive(cmd_match, "name"))
                                        cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "name", cJSON_CreateString(name));
                                    else
                                        cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "Name", cJSON_CreateString(name));

                                    if (raw_data[0]) {
                                        if (cJSON_GetObjectItemCaseSensitive(cmd_match, "keyCode"))
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "keyCode", cJSON_CreateString(raw_data));
                                        else
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "KeyCode", cJSON_CreateString(raw_data));

                                        if (cJSON_GetObjectItemCaseSensitive(cmd_match, "raw"))
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "raw", cJSON_CreateString(raw_data));
                                        else
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "Raw", cJSON_CreateString(raw_data));
                                    }
                                    if (is_macro) {
                                        if (cJSON_GetObjectItemCaseSensitive(cmd_match, "isMacro"))
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "isMacro", cJSON_CreateBool(1));
                                        else
                                            cJSON_AddBoolToObject(cmd_match, "isMacro", 1);
                                    }
                                    if (is_bt_cmd || is_mqtt_cmd) {
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "repeats");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "Repeats");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "controlPort");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "ControlPort");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "port");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "Port");
                                    } else {
                                        if (cJSON_GetObjectItemCaseSensitive(cmd_match, "repeats"))
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "repeats", cJSON_CreateNumber(repeats));
                                        else
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "Repeats", cJSON_CreateNumber(repeats));

                                        if (port > 0) {
                                            if (cJSON_GetObjectItemCaseSensitive(cmd_match, "controlPort"))
                                                cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "controlPort", cJSON_CreateNumber(port));
                                            else
                                                cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "ControlPort", cJSON_CreateNumber(port));
                                        }
                                    }
                                } else {
                                    cJSON *new_cmd = cJSON_CreateObject();
                                    cJSON_AddStringToObject(new_cmd, "id", name);
                                    cJSON_AddStringToObject(new_cmd, "name", name);
                                    cJSON_AddStringToObject(new_cmd, "keyCode", raw_data);
                                    cJSON_AddStringToObject(new_cmd, "raw", raw_data);
                                    if (is_macro) {
                                        cJSON_AddBoolToObject(new_cmd, "isMacro", 1);
                                    }
                                    if (!is_bt_cmd && !is_mqtt_cmd) {
                                        cJSON_AddNumberToObject(new_cmd, "repeats", repeats);
                                        if (port > 0) cJSON_AddNumberToObject(new_cmd, "controlPort", port);
                                    }
                                    cJSON_AddBoolToObject(new_cmd, "isLearned", 1);
                                    cJSON_AddItemToArray(cmds, new_cmd);
                                }
                                config_store_save_devices(root);
                                success = 1;
                                break;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /api/commands-batch or POST /ir/commands-batch */
    if ((strcmp(clean_path, "/api/commands-batch") == 0 || strcmp(clean_path, "/ir/commands-batch") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        int saved_count = 0;
        int success = 0;
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            cJSON *j_cmds = cJSON_GetObjectItemCaseSensitive(p, "commands");

            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[32] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }

            if (dev_id[0] && j_cmds && cJSON_IsArray(j_cmds)) {
                cJSON *root = config_store_get_devices_json();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *dev_item = NULL;
                        cJSON_ArrayForEach(dev_item, devs) {
                            cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
                            if (!d) d = dev_item;
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                            char d_id_str[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "commands");
                                if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "Commands");
                                if (!cmds) {
                                    cmds = cJSON_CreateArray();
                                    cJSON_AddItemToObject(dev_item, "commands", cmds);
                                }
                                cJSON *cmd_in = NULL;
                                cJSON_ArrayForEach(cmd_in, j_cmds) {
                                    cJSON *jn = cJSON_GetObjectItemCaseSensitive(cmd_in, "name");
                                    cJSON *jr = cJSON_GetObjectItemCaseSensitive(cmd_in, "raw");
                                    if (!jr) jr = cJSON_GetObjectItemCaseSensitive(cmd_in, "code");
                                    cJSON *jrep = cJSON_GetObjectItemCaseSensitive(cmd_in, "repeats");
                                    cJSON *jport = cJSON_GetObjectItemCaseSensitive(cmd_in, "port");
                                    const char *name = (jn && cJSON_IsString(jn)) ? jn->valuestring : "";
                                    const char *raw_data = (jr && cJSON_IsString(jr)) ? jr->valuestring : "";
                                    int repeats = (jrep && cJSON_IsNumber(jrep)) ? (int)jrep->valuedouble : 3;
                                    int port = (jport && cJSON_IsNumber(jport)) ? (int)jport->valuedouble : 0;
                                    if (!name[0] || !raw_data[0]) continue;

                                    cJSON *cmd_match = NULL;
                                    cJSON *ci = NULL;
                                    cJSON_ArrayForEach(ci, cmds) {
                                        cJSON *cn = cJSON_GetObjectItemCaseSensitive(ci, "name");
                                        if (!cn) cn = cJSON_GetObjectItemCaseSensitive(ci, "Name");
                                        if (cn && cJSON_IsString(cn) && cn->valuestring && strcmp(cn->valuestring, name) == 0) {
                                            cmd_match = ci;
                                            break;
                                        }
                                    }
                                    if (cmd_match) {
                                        if (cJSON_GetObjectItemCaseSensitive(cmd_match, "keyCode"))
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "keyCode", cJSON_CreateString(raw_data));
                                        else
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "KeyCode", cJSON_CreateString(raw_data));

                                        if (cJSON_GetObjectItemCaseSensitive(cmd_match, "raw"))
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "raw", cJSON_CreateString(raw_data));
                                        else
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "Raw", cJSON_CreateString(raw_data));

                                        if (cJSON_GetObjectItemCaseSensitive(cmd_match, "repeats"))
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "repeats", cJSON_CreateNumber(repeats));
                                        else
                                            cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "Repeats", cJSON_CreateNumber(repeats));

                                        if (port > 0) {
                                            if (cJSON_GetObjectItemCaseSensitive(cmd_match, "controlPort"))
                                                cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "controlPort", cJSON_CreateNumber(port));
                                            else
                                                cJSON_ReplaceItemInObjectCaseSensitive(cmd_match, "ControlPort", cJSON_CreateNumber(port));
                                        }
                                    } else {
                                        cJSON *new_cmd = cJSON_CreateObject();
                                        cJSON_AddStringToObject(new_cmd, "id", name);
                                        cJSON_AddStringToObject(new_cmd, "name", name);
                                        cJSON_AddStringToObject(new_cmd, "keyCode", raw_data);
                                        cJSON_AddStringToObject(new_cmd, "raw", raw_data);
                                        cJSON_AddNumberToObject(new_cmd, "repeats", repeats);
                                        if (port > 0) cJSON_AddNumberToObject(new_cmd, "controlPort", port);
                                        cJSON_AddBoolToObject(new_cmd, "isLearned", 1);
                                        cJSON_AddItemToArray(cmds, new_cmd);
                                    }
                                    saved_count++;
                                }
                                config_store_save_devices(root);
                                success = 1;
                                break;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        cJSON_AddNumberToObject(resp, "count", saved_count);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /ir/delete-command, POST /api/command-delete, or POST /bt/delete-command */
    if ((strcmp(clean_path, "/ir/delete-command") == 0 || strcmp(clean_path, "/api/command-delete") == 0 ||
         strcmp(clean_path, "/bt/delete-command") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        int success = 0;
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(p, "command");
            if (!j_cmd) j_cmd = cJSON_GetObjectItemCaseSensitive(p, "name");

            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[32] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }
            const char *cmd_name = (j_cmd && cJSON_IsString(j_cmd)) ? j_cmd->valuestring : "";

            if (dev_id[0] && cmd_name[0]) {
                cJSON *root = config_store_get_devices_json();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *dev_item = NULL;
                        cJSON_ArrayForEach(dev_item, devs) {
                            cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
                            if (!d) d = dev_item;
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                            char d_id_str[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "commands");
                                if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "Commands");
                                if (cmds && cJSON_IsArray(cmds)) {
                                    int c_count = cJSON_GetArraySize(cmds);
                                    for (int ci = 0; ci < c_count; ci++) {
                                        cJSON *c_item = cJSON_GetArrayItem(cmds, ci);
                                        cJSON *cn = cJSON_GetObjectItemCaseSensitive(c_item, "name");
                                        if (!cn) cn = cJSON_GetObjectItemCaseSensitive(c_item, "Name");
                                        if (cn && cJSON_IsString(cn) && strcmp(cn->valuestring, cmd_name) == 0) {
                                            cJSON_DeleteItemFromArray(cmds, ci);
                                            config_store_save_devices(root);
                                            success = 1;
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /ir/device, POST /api/device-save, POST /ir/device-mqtt, or POST /ir/device-power */
    if ((strcmp(clean_path, "/ir/device") == 0 || strcmp(clean_path, "/api/device-save") == 0 ||
         strcmp(clean_path, "/ir/device-mqtt") == 0 || strcmp(clean_path, "/ir/device-power") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        int success = 0;
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            cJSON *j_name = cJSON_GetObjectItemCaseSensitive(p, "name");
            cJSON *j_type = cJSON_GetObjectItemCaseSensitive(p, "type");
            cJSON *j_mfg = cJSON_GetObjectItemCaseSensitive(p, "manufacturer");
            cJSON *j_model = cJSON_GetObjectItemCaseSensitive(p, "model");
            cJSON *j_cp = cJSON_GetObjectItemCaseSensitive(p, "controlPort");
            cJSON *j_mqtt = cJSON_GetObjectItemCaseSensitive(p, "mqttTopic");
            if (!j_mqtt) j_mqtt = cJSON_GetObjectItemCaseSensitive(p, "MQTTTopic");
            cJSON *j_en = cJSON_GetObjectItemCaseSensitive(p, "mqttEnabled");
            if (!j_en) j_en = cJSON_GetObjectItemCaseSensitive(p, "MQTTEnabled");

            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[32] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }
            int cp_val = 0;
            if (j_cp) {
                if (cJSON_IsNumber(j_cp)) cp_val = (int)j_cp->valuedouble;
                else if (cJSON_IsString(j_cp)) cp_val = atoi(j_cp->valuestring);
            }
            const char *mqtt_top = (j_mqtt && cJSON_IsString(j_mqtt)) ? j_mqtt->valuestring : NULL;

            cJSON *j_pulse = cJSON_GetObjectItemCaseSensitive(p, "mqttPulse");
            if (!j_pulse) j_pulse = cJSON_GetObjectItemCaseSensitive(p, "MQTTPulseMs");
            if (!j_pulse) j_pulse = cJSON_GetObjectItemCaseSensitive(p, "pulseMs");
            int pulse_val = (j_pulse && cJSON_IsNumber(j_pulse)) ? (int)j_pulse->valuedouble : 0;
            if (!pulse_val && j_pulse && cJSON_IsString(j_pulse)) pulse_val = atoi(j_pulse->valuestring);

            int has_en = (j_en != NULL);
            int en_val = 0;
            if (has_en) {
                if (cJSON_IsTrue(j_en)) en_val = 1;
                else if (cJSON_IsNumber(j_en)) en_val = (j_en->valuedouble > 0) ? 1 : 0;
                else if (cJSON_IsString(j_en)) en_val = (strcmp(j_en->valuestring, "1") == 0 || strcasecmp(j_en->valuestring, "true") == 0) ? 1 : 0;
            }

            if (dev_id[0]) {
                cJSON *root = config_store_get_devices_json();
                if (!root) {
                    root = cJSON_CreateObject();
                    cJSON_AddItemToObject(root, "devices", cJSON_CreateArray());
                }
                cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                if (!devs) {
                    devs = cJSON_CreateArray();
                    cJSON_AddItemToObject(root, "devices", devs);
                }
                cJSON *found_dev = NULL;
                cJSON *dev_item = NULL;
                cJSON_ArrayForEach(dev_item, devs) {
                    cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
                    if (!d) d = dev_item;
                    cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                    if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                    char d_id_str[32] = {0};
                    if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                    else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                    if (strcmp(d_id_str, dev_id) == 0) {
                        found_dev = d;
                        break;
                    }
                }
                if (!found_dev) {
                    found_dev = cJSON_CreateObject();
                    cJSON_AddStringToObject(found_dev, "id", dev_id);
                    cJSON_AddStringToObject(found_dev, "name", (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : dev_id);
                    cJSON_AddStringToObject(found_dev, "type", (j_type && cJSON_IsString(j_type)) ? j_type->valuestring : "Generic");
                    cJSON_AddStringToObject(found_dev, "manufacturer", (j_mfg && cJSON_IsString(j_mfg)) ? j_mfg->valuestring : "");
                    cJSON_AddStringToObject(found_dev, "model", (j_model && cJSON_IsString(j_model)) ? j_model->valuestring : "");
                    cJSON_AddNumberToObject(found_dev, "controlPort", cp_val > 0 ? cp_val : 7);
                    cJSON_AddItemToObject(found_dev, "commands", cJSON_CreateArray());
                    cJSON_AddItemToArray(devs, found_dev);
                }
                if (j_cp != NULL) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "ControlPort");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "control_port");
                    if (cJSON_GetObjectItemCaseSensitive(found_dev, "controlPort"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "controlPort", cJSON_CreateNumber(cp_val));
                    else
                        cJSON_AddNumberToObject(found_dev, "controlPort", cp_val);
                }
                cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqtt");
                if (has_en) {
                    if (cJSON_GetObjectItemCaseSensitive(found_dev, "mqttEnabled"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "mqttEnabled", cJSON_CreateNumber(en_val));
                    else if (cJSON_GetObjectItemCaseSensitive(found_dev, "MQTTEnabled"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "MQTTEnabled", cJSON_CreateNumber(en_val));
                    else
                        cJSON_AddNumberToObject(found_dev, "mqttEnabled", en_val);
                }
                if (mqtt_top) {
                    if (cJSON_GetObjectItemCaseSensitive(found_dev, "mqttTopic"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "mqttTopic", cJSON_CreateString(mqtt_top));
                    else if (cJSON_GetObjectItemCaseSensitive(found_dev, "MQTTTopic"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "MQTTTopic", cJSON_CreateString(mqtt_top));
                    else
                        cJSON_AddStringToObject(found_dev, "mqttTopic", mqtt_top);
                }
                if (pulse_val > 0) {
                    if (cJSON_GetObjectItemCaseSensitive(found_dev, "mqttPulseMs"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "mqttPulseMs", cJSON_CreateNumber(pulse_val));
                    else if (cJSON_GetObjectItemCaseSensitive(found_dev, "MQTTPulseMs"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "MQTTPulseMs", cJSON_CreateNumber(pulse_val));
                    else
                        cJSON_AddNumberToObject(found_dev, "mqttPulseMs", pulse_val);
                }
                if (j_name && cJSON_IsString(j_name) && j_name->valuestring[0]) {
                    if (cJSON_GetObjectItemCaseSensitive(found_dev, "name"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "name", cJSON_CreateString(j_name->valuestring));
                    else if (cJSON_GetObjectItemCaseSensitive(found_dev, "Name"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "Name", cJSON_CreateString(j_name->valuestring));
                    else
                        cJSON_AddStringToObject(found_dev, "name", j_name->valuestring);
                }
                if (j_type && cJSON_IsString(j_type)) {
                    if (cJSON_GetObjectItemCaseSensitive(found_dev, "type"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "type", cJSON_CreateString(j_type->valuestring));
                    else if (cJSON_GetObjectItemCaseSensitive(found_dev, "DeviceTypeDisplayName"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "DeviceTypeDisplayName", cJSON_CreateString(j_type->valuestring));
                    else
                        cJSON_AddStringToObject(found_dev, "type", j_type->valuestring);
                }
                if (j_mfg && cJSON_IsString(j_mfg)) {
                    if (cJSON_GetObjectItemCaseSensitive(found_dev, "manufacturer"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "manufacturer", cJSON_CreateString(j_mfg->valuestring));
                    else if (cJSON_GetObjectItemCaseSensitive(found_dev, "Manufacturer"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "Manufacturer", cJSON_CreateString(j_mfg->valuestring));
                    else
                        cJSON_AddStringToObject(found_dev, "manufacturer", j_mfg->valuestring);
                }
                if (j_model && cJSON_IsString(j_model)) {
                    if (cJSON_GetObjectItemCaseSensitive(found_dev, "model"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "model", cJSON_CreateString(j_model->valuestring));
                    else if (cJSON_GetObjectItemCaseSensitive(found_dev, "Model"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "Model", cJSON_CreateString(j_model->valuestring));
                    else
                        cJSON_AddStringToObject(found_dev, "model", j_model->valuestring);
                }
                cJSON *j_icon = cJSON_GetObjectItemCaseSensitive(p, "icon");
                if (!j_icon) j_icon = cJSON_GetObjectItemCaseSensitive(p, "Icon");
                if (j_icon && cJSON_IsString(j_icon)) {
                    if (cJSON_GetObjectItemCaseSensitive(found_dev, "icon"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "icon", cJSON_CreateString(j_icon->valuestring));
                    else if (cJSON_GetObjectItemCaseSensitive(found_dev, "Icon"))
                        cJSON_ReplaceItemInObjectCaseSensitive(found_dev, "Icon", cJSON_CreateString(j_icon->valuestring));
                    else
                        cJSON_AddStringToObject(found_dev, "icon", j_icon->valuestring);
                }
                cJSON *j_idd = cJSON_GetObjectItemCaseSensitive(p, "interDeviceDelay");
                if (!j_idd) j_idd = cJSON_GetObjectItemCaseSensitive(p, "inter_device_delay");
                if (j_idd) {
                    int val = cJSON_IsNumber(j_idd) ? (int)j_idd->valuedouble : atoi(j_idd->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "inter_device_delay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "interDeviceDelay");
                    cJSON_AddNumberToObject(found_dev, "inter_device_delay", val);
                }

                cJSON *j_ikd = cJSON_GetObjectItemCaseSensitive(p, "interKeyDelay");
                if (!j_ikd) j_ikd = cJSON_GetObjectItemCaseSensitive(p, "inter_key_delay");
                if (j_ikd) {
                    int val = cJSON_IsNumber(j_ikd) ? (int)j_ikd->valuedouble : atoi(j_ikd->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "inter_key_delay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "interKeyDelay");
                    cJSON_AddNumberToObject(found_dev, "inter_key_delay", val);
                }

                cJSON *j_hikd = cJSON_GetObjectItemCaseSensitive(p, "holdInterKeyDelay");
                if (!j_hikd) j_hikd = cJSON_GetObjectItemCaseSensitive(p, "hold_inter_key_delay");
                if (j_hikd) {
                    int val = cJSON_IsNumber(j_hikd) ? (int)j_hikd->valuedouble : atoi(j_hikd->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "hold_inter_key_delay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "holdInterKeyDelay");
                    cJSON_AddNumberToObject(found_dev, "hold_inter_key_delay", val);
                }

                cJSON *j_pod = cJSON_GetObjectItemCaseSensitive(p, "powerOnDelay");
                if (!j_pod) j_pod = cJSON_GetObjectItemCaseSensitive(p, "power_on_delay");
                if (j_pod) {
                    int val = cJSON_IsNumber(j_pod) ? (int)j_pod->valuedouble : atoi(j_pod->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "power_on_delay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "powerOnDelay");
                    cJSON_AddNumberToObject(found_dev, "power_on_delay", val);
                }

                cJSON *j_pao = cJSON_GetObjectItemCaseSensitive(p, "isPowerAlwaysOn");
                if (!j_pao) j_pao = cJSON_GetObjectItemCaseSensitive(p, "is_power_always_on");
                if (j_pao) {
                    int val = cJSON_IsTrue(j_pao) || (cJSON_IsNumber(j_pao) && j_pao->valuedouble > 0) || (cJSON_IsString(j_pao) && (strcmp(j_pao->valuestring, "1") == 0 || strcasecmp(j_pao->valuestring, "true") == 0));
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "is_power_always_on");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "isPowerAlwaysOn");
                    cJSON_AddBoolToObject(found_dev, "is_power_always_on", val);
                    cJSON_AddNumberToObject(found_dev, "isPowerAlwaysOn", val);
                }

                cJSON *j_bthost = cJSON_GetObjectItemCaseSensitive(p, "btHost");
                if (!j_bthost) j_bthost = cJSON_GetObjectItemCaseSensitive(p, "bt_host");
                if (!j_bthost) j_bthost = cJSON_GetObjectItemCaseSensitive(p, "BTAddress");
                if (j_bthost && cJSON_IsString(j_bthost)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bt_host");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btHost");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "BTAddress");
                    cJSON_AddStringToObject(found_dev, "bt_host", j_bthost->valuestring);
                    cJSON_AddStringToObject(found_dev, "BTAddress", j_bthost->valuestring);
                }

                cJSON *j_btrec = cJSON_GetObjectItemCaseSensitive(p, "btAutoReconnect");
                if (!j_btrec) j_btrec = cJSON_GetObjectItemCaseSensitive(p, "bt_auto_reconnect");
                if (j_btrec) {
                    int val = cJSON_IsTrue(j_btrec) || (cJSON_IsNumber(j_btrec) && j_btrec->valuedouble > 0) || (cJSON_IsString(j_btrec) && (strcmp(j_btrec->valuestring, "1") == 0 || strcasecmp(j_btrec->valuestring, "true") == 0));
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bt_auto_reconnect");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btAutoReconnect");
                    cJSON_AddBoolToObject(found_dev, "bt_auto_reconnect", val);
                }

                cJSON *j_btdur = cJSON_GetObjectItemCaseSensitive(p, "btReconnectDurationSec");
                if (!j_btdur) j_btdur = cJSON_GetObjectItemCaseSensitive(p, "bt_reconnect_duration_sec");
                if (j_btdur) {
                    int val = cJSON_IsNumber(j_btdur) ? (int)j_btdur->valuedouble : atoi(j_btdur->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bt_reconnect_duration_sec");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btReconnectDurationSec");
                    cJSON_AddNumberToObject(found_dev, "bt_reconnect_duration_sec", val);
                }

                cJSON *j_btdly = cJSON_GetObjectItemCaseSensitive(p, "btReconnectDelaySec");
                if (!j_btdly) j_btdly = cJSON_GetObjectItemCaseSensitive(p, "bt_reconnect_delay_sec");
                if (j_btdly) {
                    int val = cJSON_IsNumber(j_btdly) ? (int)j_btdly->valuedouble : atoi(j_btdly->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bt_reconnect_delay_sec");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btReconnectDelaySec");
                    cJSON_AddNumberToObject(found_dev, "bt_reconnect_delay_sec", val);
                }

                cJSON *j_po = cJSON_GetObjectItemCaseSensitive(p, "powerOnSequence");
                if (!j_po) j_po = cJSON_GetObjectItemCaseSensitive(p, "power_on_sequence");
                if (!j_po) j_po = cJSON_GetObjectItemCaseSensitive(p, "PowerOnActions");
                if (j_po && cJSON_IsArray(j_po)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "powerOnSequence");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "PowerOnActions");
                    cJSON_AddItemToObject(found_dev, "powerOnSequence", cJSON_Duplicate(j_po, 1));
                }

                cJSON *j_pf = cJSON_GetObjectItemCaseSensitive(p, "powerOffSequence");
                if (!j_pf) j_pf = cJSON_GetObjectItemCaseSensitive(p, "power_off_sequence");
                if (!j_pf) j_pf = cJSON_GetObjectItemCaseSensitive(p, "PowerOffActions");
                if (j_pf && cJSON_IsArray(j_pf)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "powerOffSequence");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "PowerOffActions");
                    cJSON_AddItemToObject(found_dev, "powerOffSequence", cJSON_Duplicate(j_pf, 1));
                }

                config_store_save_devices(root);
                success = 1;
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST/DELETE /ir/delete-device or /api/device-delete */
    if ((strcmp(clean_path, "/ir/delete-device") == 0 || strcmp(clean_path, "/api/device-delete") == 0) &&
        (strcmp(method, "POST") == 0 || strcmp(method, "DELETE") == 0)) {
        cJSON *p = parse_request_params(body);
        if (!p) {
            const char *q = strchr(path, '?');
            if (q) p = parse_request_params(q + 1);
        }
        int success = 0;
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[64] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }
            if (!dev_id[0]) {
                const char *q = strchr(path, '?');
                if (q) {
                    cJSON *qp = parse_request_params(q + 1);
                    if (qp) {
                        cJSON *q_dev = cJSON_GetObjectItemCaseSensitive(qp, "deviceId");
                        if (!q_dev) q_dev = cJSON_GetObjectItemCaseSensitive(qp, "id");
                        if (q_dev && cJSON_IsString(q_dev) && q_dev->valuestring) {
                            strncpy(dev_buf, q_dev->valuestring, sizeof(dev_buf) - 1);
                            dev_id = dev_buf;
                        } else if (q_dev && cJSON_IsNumber(q_dev)) {
                            snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)q_dev->valuedouble);
                            dev_id = dev_buf;
                        }
                        cJSON_Delete(qp);
                    }
                }
            }
            if (dev_id[0]) {
                cJSON *root = config_store_get_devices_json();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                    if (devs && cJSON_IsArray(devs)) {
                        int d_count = cJSON_GetArraySize(devs);
                        for (int di = 0; di < d_count; di++) {
                            cJSON *dev_item = cJSON_GetArrayItem(devs, di);
                            cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
                            if (!d) d = dev_item;
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id");
                            char d_id_str[64] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON_DeleteItemFromArray(devs, di);
                                config_store_save_devices(root);
                                success = 1;
                                break;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: GET /api/settings */
    if (strcmp(clean_path, "/api/settings") == 0 && strcmp(method, "GET") == 0) {
        struct codex_settings s;
        config_store_get_settings(&s);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "hub_name", s.hub_name[0] ? s.hub_name : "Harmony Hub");
        cJSON_AddBoolToObject(resp, "bt_auto_reconnect", s.bt_auto_reconnect);
        cJSON_AddNumberToObject(resp, "bt_reconnect_duration_sec", s.bt_reconnect_duration_sec);
        cJSON_AddNumberToObject(resp, "bt_reconnect_delay_sec", s.bt_reconnect_delay_sec);
        cJSON_AddBoolToObject(resp, "dry_run", s.dry_run || g_dry_run);
        cJSON_AddStringToObject(resp, "ntp_server", s.ntp_server);
        cJSON_AddStringToObject(resp, "timezone", s.timezone[0] ? s.timezone : "UTC");
        cJSON_AddStringToObject(resp, "led_mode", s.led_mode[0] ? s.led_mode : "always_on");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    /* Route: POST /api/settings */
    if (strcmp(clean_path, "/api/settings") == 0 && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        struct codex_settings s;
        config_store_get_settings(&s);
        if (p) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(p, "bt_auto_reconnect");
            if (item) {
                if (cJSON_IsBool(item)) s.bt_auto_reconnect = cJSON_IsTrue(item) ? 1 : 0;
                else if (cJSON_IsNumber(item)) s.bt_auto_reconnect = (int)item->valuedouble;
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "bt_reconnect_duration_sec");
            if (item && cJSON_IsNumber(item)) s.bt_reconnect_duration_sec = (int)item->valuedouble;
            item = cJSON_GetObjectItemCaseSensitive(p, "bt_reconnect_delay_sec");
            if (item && cJSON_IsNumber(item)) s.bt_reconnect_delay_sec = (int)item->valuedouble;
            item = cJSON_GetObjectItemCaseSensitive(p, "dry_run");
            if (item) {
                if (cJSON_IsBool(item)) s.dry_run = cJSON_IsTrue(item) ? 1 : 0;
                else if (cJSON_IsNumber(item)) s.dry_run = (int)item->valuedouble;
                g_dry_run = s.dry_run;
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "hub_name");
            if (item && cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
                strncpy(s.hub_name, item->valuestring, sizeof(s.hub_name) - 1);
                s.hub_name[sizeof(s.hub_name) - 1] = '\0';
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "ntp_server");
            if (item && cJSON_IsString(item) && item->valuestring) {
                strncpy(s.ntp_server, item->valuestring, sizeof(s.ntp_server) - 1);
                s.ntp_server[sizeof(s.ntp_server) - 1] = '\0';
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "timezone");
            if (item && cJSON_IsString(item) && item->valuestring) {
                strncpy(s.timezone, item->valuestring, sizeof(s.timezone) - 1);
                s.timezone[sizeof(s.timezone) - 1] = '\0';
            }
            item = cJSON_GetObjectItemCaseSensitive(p, "led_mode");
            if (item && cJSON_IsString(item) && item->valuestring) {
                strncpy(s.led_mode, item->valuestring, sizeof(s.led_mode) - 1);
                s.led_mode[sizeof(s.led_mode) - 1] = '\0';
            }
            config_store_save_settings(&s);
            apply_hub_settings(&s);
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "hub_name", s.hub_name[0] ? s.hub_name : "Harmony Hub");
        cJSON_AddBoolToObject(resp, "bt_auto_reconnect", s.bt_auto_reconnect);
        cJSON_AddNumberToObject(resp, "bt_reconnect_duration_sec", s.bt_reconnect_duration_sec);
        cJSON_AddNumberToObject(resp, "bt_reconnect_delay_sec", s.bt_reconnect_delay_sec);
        cJSON_AddBoolToObject(resp, "dry_run", s.dry_run || g_dry_run);
        cJSON_AddStringToObject(resp, "ntp_server", s.ntp_server);
        cJSON_AddStringToObject(resp, "timezone", s.timezone[0] ? s.timezone : "UTC");
        cJSON_AddStringToObject(resp, "led_mode", s.led_mode[0] ? s.led_mode : "always_on");
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return;
    }

    if (strcmp(clean_path, "/") == 0 || strcmp(clean_path, "/health") == 0) {
        cJSON *hj = cJSON_CreateObject();
        cJSON_AddStringToObject(hj, "status", "ok");
        cJSON_AddStringToObject(hj, "service", "codex_daemon");
        cJSON_AddStringToObject(hj, "version", CODEX_VERSION);
        cJSON_AddStringToObject(hj, "min_app_version", "1.3.0");
        cJSON_AddNumberToObject(hj, "api_version", 2);
        send_cjson_resp(client_fd, 200, hj);
        cJSON_Delete(hj);
        close(client_fd);
        return;
    }

    /* 404 */
    cJSON *nf = cJSON_CreateObject();
    cJSON_AddStringToObject(nf, "error", "Not Found");
    send_cjson_resp(client_fd, 404, nf);
    cJSON_Delete(nf);
    close(client_fd);
}
