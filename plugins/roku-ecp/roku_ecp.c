#define _GNU_SOURCE
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

static volatile int s_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    s_running = 0;
}

static void send_roku_ecp(const char *ip, int port, const char *endpoint, const char *key) {
    if (!ip || !ip[0] || !key || !key[0]) return;
    if (port <= 0) port = 8060;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sin.sin_addr) <= 0) {
        close(s);
        return;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
        char req[256];
        int len = snprintf(req, sizeof(req),
                           "POST /%s/%s HTTP/1.1\r\n"
                           "Host: %s:%d\r\n"
                           "Content-Length: 0\r\n"
                           "Connection: close\r\n\r\n",
                           endpoint, key, ip, port);
        if (len > 0 && (size_t)len < sizeof(req)) {
            send(s, req, (size_t)len, MSG_NOSIGNAL);
        }
    }
    close(s);
}

int main(int argc, char **argv) {
    const char *sock_path = (argc > 1) ? argv[1] : "/tmp/plugins/roku-ecp.sock";

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
    fflush(stdout);

    char buf[2048];
    while (s_running) {
        ssize_t n = recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';

        char ip[64] = {0};
        int port = 8060;
        char cmd[64] = {0};
        char action[32] = "tap";

        /* Fast key extraction from JSON */
        char *p_ip = strstr(buf, "\"ip_address\":");
        if (p_ip) {
            p_ip += 13;
            while (*p_ip == ' ' || *p_ip == '"') p_ip++;
            sscanf(p_ip, "%63[^\",}]", ip);
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

        if (cmd[0] && ip[0]) {
            const char *ep = "keypress";
            if (strcmp(action, "press") == 0) ep = "keydown";
            else if (strcmp(action, "release") == 0) ep = "keyup";

            send_roku_ecp(ip, port, ep, cmd);
        }
    }

    close(sfd);
    unlink(sock_path);
    return 0;
}
