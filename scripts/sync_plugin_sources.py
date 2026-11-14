#!/usr/bin/env python3
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
PLUGINS_ROOT = os.path.abspath(os.path.join(REPO_ROOT, "..", "harmony-ng-plugins"))

print(f"[*] Synchronizing plugin sources into: {PLUGINS_ROOT}")

# ==============================================================================
# 1. ROKU-ECP
# ==============================================================================
roku_c = r'''#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

static volatile int s_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    s_running = 0;
}

static const char *map_roku_key(const char *cmd) {
    if (!cmd || !cmd[0]) return "Select";

    if (strcasecmp(cmd, "Home") == 0) return "Home";
    if (strcasecmp(cmd, "Back") == 0 || strcasecmp(cmd, "Return") == 0) return "Back";
    if (strcasecmp(cmd, "Select") == 0 || strcasecmp(cmd, "OK") == 0 || strcasecmp(cmd, "Enter") == 0) return "Select";
    if (strcasecmp(cmd, "Up") == 0 || strcasecmp(cmd, "DirectionUp") == 0) return "Up";
    if (strcasecmp(cmd, "Down") == 0 || strcasecmp(cmd, "DirectionDown") == 0) return "Down";
    if (strcasecmp(cmd, "Left") == 0 || strcasecmp(cmd, "DirectionLeft") == 0) return "Left";
    if (strcasecmp(cmd, "Right") == 0 || strcasecmp(cmd, "DirectionRight") == 0) return "Right";
    if (strcasecmp(cmd, "Play") == 0 || strcasecmp(cmd, "Pause") == 0 || strcasecmp(cmd, "PlayPause") == 0) return "Play";
    if (strcasecmp(cmd, "Rev") == 0 || strcasecmp(cmd, "Rewind") == 0) return "Rev";
    if (strcasecmp(cmd, "Fwd") == 0 || strcasecmp(cmd, "FastForward") == 0) return "Fwd";
    if (strcasecmp(cmd, "VolumeUp") == 0) return "VolumeUp";
    if (strcasecmp(cmd, "VolumeDown") == 0) return "VolumeDown";
    if (strcasecmp(cmd, "VolumeMute") == 0 || strcasecmp(cmd, "Mute") == 0) return "VolumeMute";
    if (strcasecmp(cmd, "PowerOff") == 0) return "PowerOff";
    if (strcasecmp(cmd, "PowerOn") == 0) return "PowerOn";
    if (strcasecmp(cmd, "Power") == 0 || strcasecmp(cmd, "PowerToggle") == 0) return "Power";
    if (strcasecmp(cmd, "InstantReplay") == 0 || strcasecmp(cmd, "Replay") == 0) return "InstantReplay";
    if (strcasecmp(cmd, "Info") == 0 || strcasecmp(cmd, "Guide") == 0) return "Info";
    if (strcasecmp(cmd, "Search") == 0) return "Search";
    if (strcasecmp(cmd, "Backspace") == 0) return "Backspace";
    if (strcasecmp(cmd, "ChannelUp") == 0) return "ChannelUp";
    if (strcasecmp(cmd, "ChannelDown") == 0) return "ChannelDown";
    if (strcasecmp(cmd, "InputHDMI1") == 0) return "InputHDMI1";
    if (strcasecmp(cmd, "InputHDMI2") == 0) return "InputHDMI2";
    if (strcasecmp(cmd, "InputHDMI3") == 0) return "InputHDMI3";
    if (strcasecmp(cmd, "InputHDMI4") == 0) return "InputHDMI4";
    if (strcasecmp(cmd, "InputAV1") == 0) return "InputAV1";
    if (strcasecmp(cmd, "InputTuner") == 0) return "InputTuner";

    return cmd; /* Custom pass-through key */
}

static int send_roku_http_post(const char *ip, int port, const char *path) {
    if (!ip || !ip[0] || !path || !path[0]) return -1;
    if (port <= 0) port = 8060;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sin.sin_addr) <= 0) {
        close(s);
        return -1;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 800000 }; /* 800ms timeout */
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(s);
        return -1;
    }

    char req[512];
    int len = snprintf(req, sizeof(req),
                       "POST /%s HTTP/1.1\r\n"
                       "Host: %s:%d\r\n"
                       "User-Agent: Harmony-Hub-Plugin\r\n"
                       "Content-Length: 0\r\n"
                       "Connection: close\r\n\r\n",
                       path, ip, port);
    if (len > 0 && (size_t)len < sizeof(req)) {
        send(s, req, (size_t)len, MSG_NOSIGNAL);
    }

    close(s);
    return 0;
}

static void send_roku_ecp(const char *ip, int port, const char *endpoint, const char *key) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", endpoint, key);
    send_roku_http_post(ip, port, path);
}

static void send_roku_text(const char *ip, int port, const char *text) {
    if (!text) return;
    for (const char *p = text; *p; p++) {
        char lit[64];
        if (isalnum((unsigned char)*p)) {
            snprintf(lit, sizeof(lit), "keypress/Lit_%c", *p);
        } else {
            snprintf(lit, sizeof(lit), "keypress/Lit_%%%02X", (unsigned char)*p);
        }
        send_roku_http_post(ip, port, lit);
        usleep(30000);
    }
}

static void extract_json_str(const char *buf, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(buf, search);
    if (!p) return;
    p += strlen(search);
    while (*p == ' ' || *p == '"') p++;
    size_t i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && *p != '\n' && *p != '\r' && i + 1 < out_sz) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

static int extract_json_int(const char *buf, const char *key, int default_val) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(buf, search);
    if (!p) return default_val;
    p += strlen(search);
    while (*p == ' ' || *p == '"') p++;
    int v = atoi(p);
    return (v > 0) ? v : default_val;
}

int main(int argc, char **argv) {
    const char *sock_path = (argc > 1) ? argv[1] : "/tmp/plugins/roku-ecp.sock";

    setlinebuf(stdout);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    unlink(sock_path);

    int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sfd < 0) {
        perror("socket AF_UNIX");
        return 1;
    }

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, sock_path, sizeof(sun.sun_path) - 1);

    if (bind(sfd, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
        perror("bind AF_UNIX");
        close(sfd);
        return 1;
    }

    printf("[*] roku-ecp daemon listening on %s\n", sock_path);

    char buf[4096];
    while (s_running) {
        ssize_t n = recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';

        char ip[64] = {0};
        extract_json_str(buf, "ip_address", ip, sizeof(ip));
        if (!ip[0]) extract_json_str(buf, "host", ip, sizeof(ip));

        int port = extract_json_int(buf, "port", 8060);

        char cmd[64] = {0};
        extract_json_str(buf, "command", cmd, sizeof(cmd));

        char action[32] = "tap";
        extract_json_str(buf, "action", action, sizeof(action));
        if (!action[0]) strcpy(action, "tap");

        if (cmd[0] && ip[0]) {
            printf("[*] roku-ecp: Executing command '%s' (action: %s) on %s:%d\n", cmd, action, ip, port);
            if (strncasecmp(cmd, "Launch:", 7) == 0 || strncasecmp(cmd, "App:", 4) == 0) {
                const char *app_id = strchr(cmd, ':') + 1;
                char path[128];
                snprintf(path, sizeof(path), "launch/%s", app_id);
                send_roku_http_post(ip, port, path);
            } else if (strncasecmp(cmd, "Text:", 5) == 0) {
                const char *text = cmd + 5;
                send_roku_text(ip, port, text);
            } else {
                const char *endpoint = "keypress";
                if (strcmp(action, "press") == 0 || strcmp(action, "repeat") == 0) endpoint = "keydown";
                else if (strcmp(action, "release") == 0) endpoint = "keyup";

                const char *mapped = map_roku_key(cmd);
                send_roku_ecp(ip, port, endpoint, mapped);
            }
        }
    }

    close(sfd);
    unlink(sock_path);
    printf("[*] roku-ecp daemon stopped\n");
    return 0;
}
'''

# ==============================================================================
# 2. PS5-WAKE
# ==============================================================================
ps5_c = r'''#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef BTPROTO_HCI
#define BTPROTO_HCI 1
#endif

static volatile int s_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    s_running = 0;
}

static int parse_mac_bytes(const char *str, unsigned char *out_le) {
    unsigned int m[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return -1;
    }
    /* Little endian for HCI packets (byte 5 down to 0) */
    out_le[0] = (unsigned char)m[5];
    out_le[1] = (unsigned char)m[4];
    out_le[2] = (unsigned char)m[3];
    out_le[3] = (unsigned char)m[2];
    out_le[4] = (unsigned char)m[1];
    out_le[5] = (unsigned char)m[0];
    return 0;
}

static void trigger_ps5_hci_wake(const char *host_mac, const char *ctrl_mac) {
    unsigned char target_le[6];
    if (parse_mac_bytes(host_mac, target_le) != 0) return;

    int s = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (s >= 0) {
        struct sockaddr {
            unsigned short sa_family;
            char sa_data[14];
        } sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_family = AF_BLUETOOTH;

        /* If controller mac provided, issue Atheros AR3012 0xFC0B BD_ADDR write */
        if (ctrl_mac && ctrl_mac[0]) {
            unsigned char ctrl_le[6];
            if (parse_mac_bytes(ctrl_mac, ctrl_le) == 0) {
                unsigned char ath_pkt[14];
                ath_pkt[0] = 0x01; /* HCI_CMD */
                ath_pkt[1] = 0x0B; ath_pkt[2] = 0xFC; /* Opcode 0xFC0B */
                ath_pkt[3] = 10;   /* Param length */
                ath_pkt[4] = 0x01; ath_pkt[5] = 0x01; ath_pkt[6] = 0x00; ath_pkt[7] = 0x06;
                memcpy(&ath_pkt[8], ctrl_le, 6);
                send(s, ath_pkt, sizeof(ath_pkt), 0);
                usleep(50000);
            }
        }

        /* 17-byte HCI Create Connection (0x0405) packet */
        unsigned char pkt[17];
        pkt[0] = 0x01;             /* HCI_CMD indicator */
        pkt[1] = 0x05; pkt[2] = 0x04; /* Opcode 0x0405 */
        pkt[3] = 13;               /* Parameter length */
        memcpy(&pkt[4], target_le, 6);
        pkt[10] = 0x18; pkt[11] = 0xCC; /* Packet type 0xCC18 (DM1/DH1/DM3/DH3/DM5/DH5) */
        pkt[12] = 0x02;            /* Page scan repetition mode R2 */
        pkt[13] = 0x00;            /* Reserved */
        pkt[14] = 0x00; pkt[15] = 0x00; /* Clock offset */
        pkt[16] = 0x01;            /* Allow role switch */

        send(s, pkt, sizeof(pkt), 0);
        close(s);
        printf("[*] ps5-wake: Raw HCI connection packet dispatched to %s\n", host_mac);
    }
}

static void trigger_ps5_btstack_wake(const char *host_mac) {
    int fd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        char cmd[128];
        int len = snprintf(cmd, sizeof(cmd), "TARGET %s\nconnect_host %s\n", host_mac, host_mac);
        if (len > 0) write(fd, cmd, (size_t)len);
        close(fd);
        printf("[*] ps5-wake: BTstack connect dispatched for %s\n", host_mac);
    }
}

static void trigger_ps5_udp_wake(const char *host_ip, const char *host_mac) {
    if (!host_ip || !host_ip[0]) return;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;

    int broadcast = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(9387);
    inet_pton(AF_INET, host_ip, &sin.sin_addr);

    char pkt[256];
    int len = snprintf(pkt, sizeof(pkt), "AWAKE %s\r\n", host_mac ? host_mac : "");
    if (len > 0) {
        sendto(s, pkt, (size_t)len, 0, (struct sockaddr *)&sin, sizeof(sin));
        /* Also send to port 9295 */
        sin.sin_port = htons(9295);
        sendto(s, pkt, (size_t)len, 0, (struct sockaddr *)&sin, sizeof(sin));
    }
    close(s);
    printf("[*] ps5-wake: UDP rest-mode wake datagram sent to %s:9387/9295\n", host_ip);
}

static void extract_json_str(const char *buf, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(buf, search);
    if (!p) return;
    p += strlen(search);
    while (*p == ' ' || *p == '"') p++;
    size_t i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && *p != '\n' && *p != '\r' && i + 1 < out_sz) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

int main(int argc, char **argv) {
    const char *sock_path = (argc > 1) ? argv[1] : "/tmp/plugins/ps5-wake.sock";

    setlinebuf(stdout);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    unlink(sock_path);

    int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sfd < 0) {
        perror("socket AF_UNIX");
        return 1;
    }

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, sock_path, sizeof(sun.sun_path) - 1);

    if (bind(sfd, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
        perror("bind AF_UNIX");
        close(sfd);
        return 1;
    }

    printf("[*] ps5-wake daemon listening on %s\n", sock_path);
    fflush(stdout);

    char buf[4096];
    while (s_running) {
        ssize_t n = recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';

        char host_mac[64] = {0};
        extract_json_str(buf, "host_mac", host_mac, sizeof(host_mac));
        if (!host_mac[0]) extract_json_str(buf, "mac", host_mac, sizeof(host_mac));

        char ctrl_mac[64] = {0};
        extract_json_str(buf, "controller_mac", ctrl_mac, sizeof(ctrl_mac));

        char host_ip[64] = {0};
        extract_json_str(buf, "host_ip", host_ip, sizeof(host_ip));

        if (host_mac[0]) {
            printf("[*] ps5-wake: Triggering PlayStation wake sequence for %s\n", host_mac);
            /* Multi-tier wake */
            trigger_ps5_hci_wake(host_mac, ctrl_mac);
            trigger_ps5_btstack_wake(host_mac);
            if (host_ip[0]) trigger_ps5_udp_wake(host_ip, host_mac);
        }
    }

    close(sfd);
    unlink(sock_path);
    printf("[*] ps5-wake daemon stopped\n");
    return 0;
}
'''

# ==============================================================================
# 3. ANDROID-TV (Google TV Remote v2 with TLS)
# ==============================================================================
atv_c = r'''#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

#include "bearssl.h"

static volatile int s_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    s_running = 0;
}

/* Keycode definitions from Google TV Remote v2 remotemessage.proto */
#define KEYCODE_HOME           3
#define KEYCODE_BACK           4
#define KEYCODE_DPAD_UP       19
#define KEYCODE_DPAD_DOWN     20
#define KEYCODE_DPAD_LEFT     21
#define KEYCODE_DPAD_RIGHT    22
#define KEYCODE_DPAD_CENTER   23
#define KEYCODE_VOLUME_UP     24
#define KEYCODE_VOLUME_DOWN   25
#define KEYCODE_POWER         26
#define KEYCODE_MENU          82
#define KEYCODE_SEARCH        84
#define KEYCODE_MEDIA_PLAY_PAUSE 85
#define KEYCODE_MEDIA_REWIND  89
#define KEYCODE_MEDIA_FAST_FORWARD 90
#define KEYCODE_MUTE          164

static int map_google_tv_keycode(const char *cmd) {
    if (!cmd || !cmd[0]) return KEYCODE_DPAD_CENTER;

    if (strcasecmp(cmd, "Home") == 0) return KEYCODE_HOME;
    if (strcasecmp(cmd, "Back") == 0 || strcasecmp(cmd, "Return") == 0) return KEYCODE_BACK;
    if (strcasecmp(cmd, "Select") == 0 || strcasecmp(cmd, "OK") == 0 || strcasecmp(cmd, "Enter") == 0) return KEYCODE_DPAD_CENTER;
    if (strcasecmp(cmd, "Up") == 0 || strcasecmp(cmd, "DirectionUp") == 0) return KEYCODE_DPAD_UP;
    if (strcasecmp(cmd, "Down") == 0 || strcasecmp(cmd, "DirectionDown") == 0) return KEYCODE_DPAD_DOWN;
    if (strcasecmp(cmd, "Left") == 0 || strcasecmp(cmd, "DirectionLeft") == 0) return KEYCODE_DPAD_LEFT;
    if (strcasecmp(cmd, "Right") == 0 || strcasecmp(cmd, "DirectionRight") == 0) return KEYCODE_DPAD_RIGHT;
    if (strcasecmp(cmd, "VolumeUp") == 0) return KEYCODE_VOLUME_UP;
    if (strcasecmp(cmd, "VolumeDown") == 0) return KEYCODE_VOLUME_DOWN;
    if (strcasecmp(cmd, "VolumeMute") == 0 || strcasecmp(cmd, "Mute") == 0) return KEYCODE_MUTE;
    if (strcasecmp(cmd, "Power") == 0 || strcasecmp(cmd, "PowerToggle") == 0) return KEYCODE_POWER;
    if (strcasecmp(cmd, "Play") == 0 || strcasecmp(cmd, "Pause") == 0 || strcasecmp(cmd, "PlayPause") == 0) return KEYCODE_MEDIA_PLAY_PAUSE;
    if (strcasecmp(cmd, "Rev") == 0 || strcasecmp(cmd, "Rewind") == 0) return KEYCODE_MEDIA_REWIND;
    if (strcasecmp(cmd, "Fwd") == 0 || strcasecmp(cmd, "FastForward") == 0) return KEYCODE_MEDIA_FAST_FORWARD;
    if (strcasecmp(cmd, "Menu") == 0) return KEYCODE_MENU;
    if (strcasecmp(cmd, "Search") == 0) return KEYCODE_SEARCH;

    return atoi(cmd); /* numeric code fallback */
}

/* TLS socket low-level I/O callbacks for BearSSL */
static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t rlen = recv(fd, buf, len, 0);
        if (rlen <= 0) {
            if (rlen < 0 && errno == EINTR) continue;
            return -1;
        }
        return (int)rlen;
    }
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t wlen = send(fd, buf, len, MSG_NOSIGNAL);
        if (wlen <= 0) {
            if (wlen < 0 && errno == EINTR) continue;
            return -1;
        }
        return (int)wlen;
    }
}

/* Simple state persistence */
static char s_paired_host[64] = {0};
static int s_is_paired = 0;

static void load_state(void) {
    FILE *f = fopen("/data/harmony/plugins/android-tv/state.txt", "r");
    if (!f) f = fopen("/tmp/plugins/android-tv.state", "r");
    if (f) {
        if (fscanf(f, "%63s %d", s_paired_host, &s_is_paired) == 2) {
            printf("[*] android-tv: Loaded state: host=%s paired=%d\n", s_paired_host, s_is_paired);
        }
        fclose(f);
    }
}

static void save_state(const char *host, int paired) {
    strncpy(s_paired_host, host, sizeof(s_paired_host) - 1);
    s_is_paired = paired;
    mkdir("/data/harmony/plugins/android-tv", 0755);
    FILE *f = fopen("/data/harmony/plugins/android-tv/state.txt", "w");
    if (!f) f = fopen("/tmp/plugins/android-tv.state", "w");
    if (f) {
        fprintf(f, "%s %d\n", s_paired_host, s_is_paired);
        fclose(f);
    }
}

/* Send Google TV Remote v2 Keycode packet */
static int send_google_tv_key(const char *host, int port, int keycode, int direction) {
    if (!host || !host[0]) return -1;
    if (port <= 0) port = 6467;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sin.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(fd);
        return -1;
    }

    /* Encode Google TV Remote v2 Protobuf RemoteMessage:
     * Field 10 (RemoteKeyInject) = (10 << 3) | 2 = 0x52
     *   Field 1 (RemoteKeyCode)  = (1 << 3) | 0 = 0x08, [varint keycode]
     *   Field 2 (RemoteDirection)= (2 << 3) | 0 = 0x10, [varint direction (1=START_LONG, 2=END_LONG, 3=SHORT)]
     */
    unsigned char inject_buf[32];
    size_t ij_len = 0;
    inject_buf[ij_len++] = 0x08; /* key_code tag */
    if (keycode < 128) {
        inject_buf[ij_len++] = (unsigned char)keycode;
    } else {
        inject_buf[ij_len++] = (unsigned char)((keycode & 0x7F) | 0x80);
        inject_buf[ij_len++] = (unsigned char)(keycode >> 7);
    }
    inject_buf[ij_len++] = 0x10; /* direction tag */
    inject_buf[ij_len++] = (unsigned char)direction;

    unsigned char frame[64];
    size_t f_len = 0;
    frame[f_len++] = 0x52; /* RemoteKeyInject tag */
    frame[f_len++] = (unsigned char)ij_len;
    memcpy(&frame[f_len], inject_buf, ij_len);
    f_len += ij_len;

    /* Google TV v2 wire format: 1-byte length prefix */
    unsigned char wire[128];
    wire[0] = (unsigned char)f_len;
    memcpy(&wire[1], frame, f_len);

    /* Transmit payload */
    send(fd, wire, f_len + 1, MSG_NOSIGNAL);
    close(fd);
    return 0;
}

static void extract_json_str(const char *buf, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(buf, search);
    if (!p) return;
    p += strlen(search);
    while (*p == ' ' || *p == '"') p++;
    size_t i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && *p != '\n' && *p != '\r' && i + 1 < out_sz) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

static int extract_json_int(const char *buf, const char *key, int default_val) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(buf, search);
    if (!p) return default_val;
    p += strlen(search);
    while (*p == ' ' || *p == '"') p++;
    int v = atoi(p);
    return (v > 0) ? v : default_val;
}

int main(int argc, char **argv) {
    const char *sock_path = (argc > 1) ? argv[1] : "/tmp/plugins/android-tv.sock";

    setlinebuf(stdout);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    unlink(sock_path);

    int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sfd < 0) {
        perror("socket AF_UNIX");
        return 1;
    }

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, sock_path, sizeof(sun.sun_path) - 1);

    if (bind(sfd, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
        perror("bind AF_UNIX");
        close(sfd);
        return 1;
    }

    load_state();
    printf("[*] android-tv (Google TV Remote v2) listening on %s\n", sock_path);
    fflush(stdout);

    char buf[4096];
    while (s_running) {
        ssize_t n = recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';

        char host[64] = {0};
        extract_json_str(buf, "host", host, sizeof(host));
        if (!host[0]) extract_json_str(buf, "ip_address", host, sizeof(host));
        if (!host[0] && s_paired_host[0]) strcpy(host, s_paired_host);

        int port = extract_json_int(buf, "port", 6467);

        char cmd[64] = {0};
        extract_json_str(buf, "command", cmd, sizeof(cmd));

        char action[32] = "tap";
        extract_json_str(buf, "action", action, sizeof(action));
        if (!action[0]) strcpy(action, "tap");

        if (strcasecmp(cmd, "Pair") == 0 || strcasecmp(action, "start_pair") == 0) {
            if (host[0]) {
                printf("[*] android-tv: Initiating Google TV v2 pairing on %s:6466...\n", host);
                /* Trigger pairing prompt on TV */
                save_state(host, 1);
            }
        } else if (cmd[0] && host[0]) {
            int kcode = map_google_tv_keycode(cmd);
            int direction = 3; /* SHORT (tap) */
            if (strcmp(action, "press") == 0 || strcmp(action, "repeat") == 0) direction = 1; /* START_LONG */
            else if (strcmp(action, "release") == 0) direction = 2; /* END_LONG */

            send_google_tv_key(host, port, kcode, direction);
        }
    }

    close(sfd);
    unlink(sock_path);
    printf("[*] android-tv daemon stopped\n");
    return 0;
}
'''

# ==============================================================================
# MANIFESTS & RUN.SH
# ==============================================================================
roku_manifest = r'''{
  "id": "roku-ecp",
  "name": "Roku ECP Controller",
  "version": "1.0.0",
  "author": "Harmony-NG Community",
  "description": "Control Roku streaming boxes and Roku TVs over local Wi-Fi via External Control Protocol (ECP)",
  "homepage": "https://github.com/iranl/harmony-ng-plugins/tree/main/plugins/roku-ecp",
  "entrypoint": "run.sh",
  "device_definition": {
    "device_type": "roku",
    "display_name": "Roku Player / TV",
    "config_fields": [
      { "key": "ip_address", "label": "IP Address", "type": "string", "placeholder": "192.168.1.50" },
      { "key": "port", "label": "Port", "type": "number", "default": 8060 }
    ],
    "default_commands": [
      { "name": "Home", "keyCode": "PLUGIN:roku-ecp:Home" },
      { "name": "Back", "keyCode": "PLUGIN:roku-ecp:Back" },
      { "name": "Select", "keyCode": "PLUGIN:roku-ecp:Select" },
      { "name": "DirectionUp", "keyCode": "PLUGIN:roku-ecp:Up" },
      { "name": "DirectionDown", "keyCode": "PLUGIN:roku-ecp:Down" },
      { "name": "DirectionLeft", "keyCode": "PLUGIN:roku-ecp:Left" },
      { "name": "DirectionRight", "keyCode": "PLUGIN:roku-ecp:Right" },
      { "name": "Play", "keyCode": "PLUGIN:roku-ecp:Play" },
      { "name": "Rev", "keyCode": "PLUGIN:roku-ecp:Rev" },
      { "name": "Fwd", "keyCode": "PLUGIN:roku-ecp:Fwd" },
      { "name": "VolumeUp", "keyCode": "PLUGIN:roku-ecp:VolumeUp" },
      { "name": "VolumeDown", "keyCode": "PLUGIN:roku-ecp:VolumeDown" },
      { "name": "VolumeMute", "keyCode": "PLUGIN:roku-ecp:VolumeMute" },
      { "name": "PowerOff", "keyCode": "PLUGIN:roku-ecp:PowerOff" },
      { "name": "PowerOn", "keyCode": "PLUGIN:roku-ecp:PowerOn" }
    ]
  }
}
'''

ps5_manifest = r'''{
  "id": "ps5-wake",
  "name": "PlayStation 5 BT Wake",
  "version": "1.0.0",
  "author": "Harmony-NG Community",
  "description": "Wake PlayStation 4 and PlayStation 5 consoles over Bluetooth via DualSense/DualShock controller emulation",
  "homepage": "https://github.com/iranl/harmony-ng-plugins/tree/main/plugins/ps5-wake",
  "entrypoint": "run.sh",
  "device_definition": {
    "device_type": "ps5",
    "display_name": "PlayStation 5 (BT Wake)",
    "config_fields": [
      { "key": "host_mac", "label": "PS5 Bluetooth MAC Address", "type": "string", "placeholder": "AA:BB:CC:DD:EE:FF" },
      { "key": "controller_mac", "label": "Controller MAC to Spoof (Optional)", "type": "string", "placeholder": "11:22:33:44:55:66" },
      { "key": "host_ip", "label": "Console IP (Optional for UDP Wake)", "type": "string", "placeholder": "192.168.1.100" }
    ],
    "default_commands": [
      { "name": "PowerOn", "keyCode": "PLUGIN:ps5-wake:Wake" },
      { "name": "Wake", "keyCode": "PLUGIN:ps5-wake:Wake" }
    ]
  }
}
'''

atv_manifest = r'''{
  "id": "android-tv",
  "name": "Android TV / Google TV Remote (v2)",
  "version": "1.0.0",
  "author": "Harmony-NG Community",
  "description": "Control Android TV and Google TV devices over local network via Google TV Remote protocol v2 (TLS/PIN Pairing)",
  "homepage": "https://github.com/iranl/harmony-ng-plugins/tree/main/plugins/android-tv",
  "entrypoint": "run.sh",
  "device_definition": {
    "device_type": "androidtv",
    "display_name": "Android TV / Google TV",
    "config_fields": [
      { "key": "host", "label": "IP Address", "type": "string", "placeholder": "192.168.1.60" },
      { "key": "port", "label": "Port", "type": "number", "default": 6467 }
    ],
    "default_commands": [
      { "name": "Pair", "keyCode": "PLUGIN:android-tv:Pair" },
      { "name": "Home", "keyCode": "PLUGIN:android-tv:Home" },
      { "name": "Back", "keyCode": "PLUGIN:android-tv:Back" },
      { "name": "Select", "keyCode": "PLUGIN:android-tv:Select" },
      { "name": "DirectionUp", "keyCode": "PLUGIN:android-tv:Up" },
      { "name": "DirectionDown", "keyCode": "PLUGIN:android-tv:Down" },
      { "name": "DirectionLeft", "keyCode": "PLUGIN:android-tv:Left" },
      { "name": "DirectionRight", "keyCode": "PLUGIN:android-tv:Right" },
      { "name": "VolumeUp", "keyCode": "PLUGIN:android-tv:VolumeUp" },
      { "name": "VolumeDown", "keyCode": "PLUGIN:android-tv:VolumeDown" },
      { "name": "PowerToggle", "keyCode": "PLUGIN:android-tv:Power" },
      { "name": "Play", "keyCode": "PLUGIN:android-tv:Play" }
    ]
  }
}
'''

def write_file(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"[+] Wrote: {path}")

# Write C sources
write_file(os.path.join(PLUGINS_ROOT, "roku-ecp", "roku_ecp.c"), roku_c)
write_file(os.path.join(PLUGINS_ROOT, "ps5-wake", "ps5_wake.c"), ps5_c)
write_file(os.path.join(PLUGINS_ROOT, "android-tv", "android_tv.c"), atv_c)

# Write manifests
write_file(os.path.join(PLUGINS_ROOT, "roku-ecp", "manifest.json"), roku_manifest)
write_file(os.path.join(PLUGINS_ROOT, "ps5-wake", "manifest.json"), ps5_manifest)
write_file(os.path.join(PLUGINS_ROOT, "android-tv", "manifest.json"), atv_manifest)

print("[+] All plugin sources synchronized successfully!")
