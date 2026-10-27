#define _GNU_SOURCE
#include "config_store.h"
#include "orchestrator.h"
#include "ir_i2s.h"
#include "hw_action.h"
#include "http_server.h"
#include "ws_server.h"
#include "hub_ws.h"
#include "hub_mqtt.h"
#include "hub_ntp.h"
#include "harmony_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <time.h>

#define HARMONY_PID_FILE "/tmp/harmony_daemon.pid"

static volatile int g_running = 1;

static void handle_sig(int sig) {
    (void)sig;
    g_running = 0;
}

static void write_pid(void) {
    FILE *f = fopen(HARMONY_PID_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", (int)getpid());
        fclose(f);
    }
}

static void *http_client_worker(void *arg) {
    int cfd = (int)(intptr_t)arg;
    http_handle_client(cfd);
    return NULL;
}

int main(int argc, char **argv) {
    int port = 8089;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            g_dry_run = 1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (isdigit((unsigned char)argv[i][0])) {
            port = atoi(argv[i]);
        }
    }
    if (port <= 0) port = 8089;

    signal(SIGTERM, handle_sig);
    signal(SIGINT, handle_sig);
    signal(SIGPIPE, SIG_IGN);

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    write_pid();

    printf("=========================================\n");
    printf("   hub_daemon — Unified Harmony Hub      \n");
    printf("=========================================\n");

    /* 1. Hardware, Config Store & Orchestrator Core */
    config_store_init();
    struct hub_settings settings;
    config_store_get_settings(&settings);
    if (settings.dry_run) g_dry_run = 1;
    if (g_dry_run) {
        printf("[!] DRY-RUN MODE ACTIVE: All physical IR, BT, and MQTT outputs will be simulated\n");
    }

    orch_init();
    ir_i2s_init();
    orch_set_progress_callback(on_orch_progress, NULL);
    hw_set_mqtt_button_handler(daemon_mqtt_button_handler);
    printf("[+] Hardware core & Activity Orchestrator initialized (current: %s)\n",
           orch_get_current_activity());

    /* 2. Web & WebSocket Server */
    ws_server_init();
    int srv_fd = http_server_init(port);
    if (srv_fd < 0) {
        fprintf(stderr, "[-] Could not bind HTTP server on port %d: %s\n", port, strerror(errno));
        return 1;
    }
    printf("[+] HTTP & WebSocket server listening on port %d\n", port);

    /* 3. MQTT Client */
    hub_mqtt_init();

    printf("[+] Entering master non-blocking event loop...\n");

    /* Initial SNTP network time sync attempt */
    const char *ntp_host = (settings.ntp_server[0]) ? settings.ntp_server : "pool.ntp.org";
    sntp_sync_time(ntp_host, 1);

    while (g_running) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(srv_fd, &rset);
        int max_fd = srv_fd;

        int client_fds[WS_MAX_CLIENTS];
        int client_count = ws_get_clients(client_fds, WS_MAX_CLIENTS);
        for (int i = 0; i < client_count; i++) {
            FD_SET(client_fds[i], &rset);
            if (client_fds[i] > max_fd) max_fd = client_fds[i];
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 40000; /* 40 ms tick */

        int nready = select(max_fd + 1, &rset, NULL, NULL, &tv);
        if (nready > 0) {
            if (FD_ISSET(srv_fd, &rset)) {
                struct sockaddr_in caddr;
                socklen_t clen = sizeof(caddr);
                int cfd = accept(srv_fd, (struct sockaddr *)&caddr, &clen);
                if (cfd >= 0) {
                    fcntl(cfd, F_SETFD, FD_CLOEXEC);
                    struct timeval to;
                    to.tv_sec = 2;
                    to.tv_usec = 0;
                    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
                    to.tv_sec = 2;
                    to.tv_usec = 0;
                    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
                    int nodelay = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    int sndbuf = 32768;
                    setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

                    pthread_t th;
                    pthread_attr_t attr;
                    pthread_attr_init(&attr);
                    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                    pthread_attr_setstacksize(&attr, 512 * 1024);
                    if (pthread_create(&th, &attr, http_client_worker, (void *)(intptr_t)cfd) != 0) {
                        http_handle_client(cfd);
                    }
                    pthread_attr_destroy(&attr);
                }
            }

            for (int i = 0; i < client_count; i++) {
                int cfd = client_fds[i];
                if (FD_ISSET(cfd, &rset)) {
                    handle_ws_client_message(cfd);
                }
            }
        }

        /* Tick Activity Orchestrator state machine & delays */
        orch_tick();
        mqtt_cmd_tick();

        /* Tick SNTP network time sync */
        sntp_tick(ntp_host);

        /* Tick MQTT client packet reader, keepalive, and periodic state poll */
        hub_mqtt_tick();
    }

    printf("\n[*] Shutting down hub_daemon...\n");
    orch_cancel();
    ir_i2s_shutdown();
    ws_close_all();
    http_server_close(srv_fd);
    hub_mqtt_shutdown();
    unlink(HARMONY_PID_FILE);

    printf("[+] Shutdown complete.\n");
    return 0;
}
