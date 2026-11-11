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

static void trigger_ps5_wake(const char *mac) {
    if (!mac || !mac[0]) return;
    printf("[*] ps5-wake: Sending Bluetooth wake pulse to console %s\n", mac);
    fflush(stdout);

    /* Dispatch connection request to Hub BTstack via pipe */
    FILE *f = fopen("/tmp/bthid_input", "w");
    if (f) {
        fprintf(f, "CONNECT %s\n", mac);
        fclose(f);
    }
}

int main(int argc, char **argv) {
    const char *sock_path = (argc > 1) ? argv[1] : "/tmp/plugins/ps5-wake.sock";

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

    char buf[2048];
    while (s_running) {
        ssize_t n = recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';

        char mac[32] = {0};
        char *p_mac = strstr(buf, "\"host_mac\":");
        if (p_mac) {
            p_mac += 11;
            while (*p_mac == ' ' || *p_mac == '"') p_mac++;
            sscanf(p_mac, "%31[^\",}]", mac);
        }

        if (mac[0]) {
            trigger_ps5_wake(mac);
        }
    }

    close(sfd);
    unlink(sock_path);
    return 0;
}
