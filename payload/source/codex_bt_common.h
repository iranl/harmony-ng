/*
 * codex_bt_common.h — Shared Bluetooth utility functions.
 *
 * Included by webui_bt.c and Bluetooth modules.  All functions are
 * static inline so each translation unit gets its own copy — no linker dependency.
 */
#ifndef CODEX_BT_COMMON_H
#define CODEX_BT_COMMON_H

#include <ctype.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

#define CODEX_BTSTACK_SOCK "/tmp/codex_btstack.sock"
#define CODEX_BTSTACK_CMD_FILE "/tmp/codex_btstack_cmd"

/* Validate a BT address string: "XX:XX:XX:XX:XX:XX" (17 chars, hex+colon). */
static inline int codex_valid_bdaddr(const char *s) {
    int i;
    if (!s || strlen(s) != 17) return 0;
    for (i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (s[i] != ':') return 0; }
        else { if (!isxdigit((unsigned char)s[i])) return 0; }
    }
    return 1;
}

extern int g_dry_run;

/* Send IPC command to codex_btstack: Unix domain datagram socket first, file fallback */
static inline int codex_send_btstack_cmd(const char *cmd) {
    if (!cmd || !cmd[0]) return -1;
    if (g_dry_run) {
        printf("[DRY-RUN] BTstack command simulated: %s", cmd);
        if (cmd[strlen(cmd) - 1] != '\n') printf("\n");
        return 0;
    }
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct sockaddr_un sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        strncpy(sun.sun_path, CODEX_BTSTACK_SOCK, sizeof(sun.sun_path) - 1);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 150000 };
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        ssize_t sent = sendto(s, cmd, strlen(cmd), 0, (struct sockaddr *)&sun, sizeof(sun));
        close(s);
        if (sent > 0) return 0;
    }
    /* Fallback to atomic file write */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.new", CODEX_BTSTACK_CMD_FILE);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "%s\n", cmd);
    fclose(f);
    if (rename(tmp, CODEX_BTSTACK_CMD_FILE) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

#endif /* CODEX_BT_COMMON_H */
