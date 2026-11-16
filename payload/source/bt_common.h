/*
 * bt_common.h — Shared Bluetooth utility functions for Harmony Hub.
 *
 * All functions are static inline so each translation unit gets its own copy.
 */
#ifndef BT_COMMON_H
#define BT_COMMON_H

#include <ctype.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef HUB_BTSTACK_SOCK
#define HUB_BTSTACK_SOCK "/tmp/hub_btstack.sock"
#endif
#ifndef HUB_BTSTACK_CMD_FILE
#define HUB_BTSTACK_CMD_FILE "/tmp/hub_btstack_cmd"
#endif

/* Validate a BT address string: "XX:XX:XX:XX:XX:XX" (17 chars, hex+colon). */
static inline int hub_valid_bdaddr(const char *s) {
    int i;
    if (!s || strlen(s) != 17) return 0;
    for (i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (s[i] != ':') return 0; }
        else { if (!isxdigit((unsigned char)s[i])) return 0; }
    }
    return 1;
}

/* A value that goes into a line of the hub_btstack key pipe (/tmp/bthid_input), where every line
 * is a command: no line break or other control character, which would start another command */
static inline int hub_bt_pipe_value_ok(const char *s) {
    if (!s) return 0;
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7f) return 0;
    }
    return 1;
}

/* Longest text accepted for typing on a Bluetooth host: one "TEXT ..." pipe line must stay short
 * enough to be read in one piece */
#define HUB_BT_TEXT_MAX 240

/* One command for hub_btstack on the key pipe, with "TARGET <address>" in front when target is set.
 * Both lines go out in one write() (shorter than PIPE_BUF, so the kernel writes it whole or not at
 * all): with two writes another writer's line could come between them, and when the pipe was
 * full the command went to the previous host. Returns 1 when the whole command was written. */
static inline int hub_bt_pipe_send(const char *target, const char *line) {
    char buf[320];
    int len;
    if (!line) return 0;
    if (target && target[0]) len = snprintf(buf, sizeof(buf), "TARGET %s\n%s", target, line);
    else len = snprintf(buf, sizeof(buf), "%s", line);
    if (len <= 0 || len >= (int)sizeof(buf)) return 0;
    int fd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return 0;
    ssize_t w = write(fd, buf, (size_t)len);
    close(fd);
    return w == (ssize_t)len;
}

extern int g_dry_run;

/* Dry run: print the command instead of sending it (1 when printed) */
static inline int hub_btstack_cmd_dry(const char *cmd) {
    if (!g_dry_run) return 0;
    if (!cmd || !cmd[0]) return 1;
    printf("[DRY-RUN] BTstack command simulated: %s", cmd);
    if (cmd[strlen(cmd) - 1] != '\n') printf("\n");
    return 1;
}

/* One datagram to hub_btstack's command socket: 0 when it took it, -1 when it is not there */
static inline int hub_btstack_cmd_socket(const char *cmd) {
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, HUB_BTSTACK_SOCK, sizeof(sun.sun_path) - 1);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 150000 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ssize_t sent = sendto(s, cmd, strlen(cmd), 0, (struct sockaddr *)&sun, sizeof(sun));
    close(s);
    return sent > 0 ? 0 : -1;
}

/* Send a command to the running hub_btstack over its socket only, no file fallback: -1 when it
 * did not take it (not running, restarting). For commands that must not wait for a later start. */
static inline int hub_send_btstack_cmd_live(const char *cmd) {
    if (!cmd || !cmd[0]) return -1;
#ifdef TEST_HARNESS
    /* the host test daemon runs in dry run: this plays a hub_btstack that is gone */
    if (getenv("HARMONY_TEST_BTSTACK_GONE")) return -1;
#endif
    if (hub_btstack_cmd_dry(cmd)) return 0;
    return hub_btstack_cmd_socket(cmd);
}

/* Send IPC command to hub_btstack: Unix domain datagram socket first, file fallback */
static inline int hub_send_btstack_cmd(const char *cmd) {
    if (!cmd || !cmd[0]) return -1;
    if (hub_btstack_cmd_dry(cmd)) return 0;
    if (hub_btstack_cmd_socket(cmd) == 0) return 0;
    /* Fallback to atomic file write */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.new", HUB_BTSTACK_CMD_FILE);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "%s\n", cmd);
    fclose(f);
    if (rename(tmp, HUB_BTSTACK_CMD_FILE) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

#endif /* BT_COMMON_H */
