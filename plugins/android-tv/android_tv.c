#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

static volatile int s_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    s_running = 0;
}

static void send_android_tv_cmd(const char *host, int port, const char *cmd, const char *action) {
    if (!host || !host[0] || !cmd || !cmd[0]) return;
    if (port <= 0) port = 5555;
    printf("[*] android-tv: Sending command '%s' (action: %s) to %s:%d\n", cmd, action, host, port);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *sock_path = (argc > 1) ? argv[1] : "/tmp/plugins/android-tv.sock";

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

    printf("[*] android-tv daemon listening on %s\n", sock_path);
    fflush(stdout);

    char buf[2048];
    while (s_running) {
        ssize_t n = recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';

        char host[64] = {0};
        int port = 5555;
        char cmd[64] = {0};
        char action[32] = "tap";

        char *p_host = strstr(buf, "\"host\":");
        if (p_host) {
            p_host += 7;
            while (*p_host == ' ' || *p_host == '"') p_host++;
            sscanf(p_host, "%63[^\",}]", host);
        }

        char *p_port = strstr(buf, "\"port\":");
        if (p_port) {
            p_port += 7;
            while (*p_port == ' ') p_port++;
            port = atoi(p_port);
        }

        char *p_cmd = strstr(buf, "\"command\":");
        if (p_cmd) {
            p_cmd += 10;
            while (*p_cmd == ' ' || *p_cmd == '"') p_cmd++;
            sscanf(p_cmd, "%63[^\",}]", cmd);
        }

        char *p_act = strstr(buf, "\"action\":");
        if (p_act) {
            p_act += 9;
            while (*p_act == ' ' || *p_act == '"') p_act++;
            sscanf(p_act, "%31[^\",}]", action);
        }

        if (cmd[0] && host[0]) {
            send_android_tv_cmd(host, port, cmd, action);
        }
    }

    close(sfd);
    unlink(sock_path);
    return 0;
}
