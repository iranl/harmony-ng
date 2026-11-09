#define _GNU_SOURCE
#include "config_store.h"
#include "orchestrator.h"
#include "ir_i2s.h"
#include "hw_action.h"
#include "http_server.h"
#include "ws_server.h"
#include "state_watch.h"
#include "hub_events.h"
#include "hub_info.h"
#include "hub_mdns.h"
#include "hbus_server.h"
#include "hub_ssdp.h"
#include "hub_ws.h"
#include "hub_mqtt.h"
#include "hub_cmd.h"
#include "hub_ntp.h"
#include "http_api_system.h"
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

#define MAX_HTTP_CLIENT_THREADS 8
static int g_active_http_threads = 0;
static pthread_mutex_t g_http_thread_lock = PTHREAD_MUTEX_INITIALIZER;

static void *http_client_worker(void *arg) {
    int cfd = (int)(intptr_t)arg;
    http_handle_client(cfd);
    pthread_mutex_lock(&g_http_thread_lock);
    g_active_http_threads--;
    pthread_mutex_unlock(&g_http_thread_lock);
    return NULL;
}

/* Port 8088 (Logitech protocol shim): same worker pool, no auth, hbus_server.c routes */
static void *hbus_client_worker(void *arg) {
    int cfd = (int)(intptr_t)arg;
    hbus_handle_client(cfd);
    pthread_mutex_lock(&g_http_thread_lock);
    g_active_http_threads--;
    pthread_mutex_unlock(&g_http_thread_lock);
    return NULL;
}

/* Accepts one connection on a listening socket and hands it to a detached worker thread */
static void accept_client(int listen_fd, void *(*worker)(void *)) {
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
    if (cfd < 0) return;
    if (cfd >= FD_SETSIZE) {
        fprintf(stderr, "[-] Accepted fd %d exceeds FD_SETSIZE\n", cfd);
        close(cfd);
        return;
    }
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

    int reject = 0;
    pthread_mutex_lock(&g_http_thread_lock);
    if (g_active_http_threads >= MAX_HTTP_CLIENT_THREADS) {
        reject = 1;
    } else {
        g_active_http_threads++;
    }
    pthread_mutex_unlock(&g_http_thread_lock);

    if (reject) {
        const char *busy = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        send(cfd, busy, strlen(busy), MSG_NOSIGNAL);
        close(cfd);
        return;
    }

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /*
     * Keep 512 KB stack size: cJSON recursive parsing and formatting
     * on large backup bundles (up to 2 MB) requires deep recursion headroom.
     * Dynamic HTTP request buffers are malloc'd on the heap in http_server.c.
     */
    pthread_attr_setstacksize(&attr, 512 * 1024);
    if (pthread_create(&th, &attr, worker, (void *)(intptr_t)cfd) != 0) {
        fprintf(stderr, "[-] pthread_create failed for client fd %d\n", cfd);
        pthread_mutex_lock(&g_http_thread_lock);
        g_active_http_threads--;
        pthread_mutex_unlock(&g_http_thread_lock);
        close(cfd);
    }
    pthread_attr_destroy(&attr);
}

int main(int argc, char **argv) {
    int port = 8089;
    const char *mfg_file = NULL; /* test harness: a file with the mfg partition layout */
    int mdns_port = MDNS_PORT;   /* test harness: --mdns-port N */
    int legacy_port = HBUS_PORT; /* test harness: --legacy-port N */
    int ssdp_port = SSDP_PORT;   /* test harness: --ssdp-port N */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            g_dry_run = 1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mfg-file") == 0 && i + 1 < argc) {
            mfg_file = argv[++i];
        } else if (strcmp(argv[i], "--mdns-port") == 0 && i + 1 < argc) {
            mdns_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--legacy-port") == 0 && i + 1 < argc) {
            legacy_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ssdp-port") == 0 && i + 1 < argc) {
            ssdp_port = atoi(argv[++i]);
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
    /* Rewrite VERSION only when it differs: every daemon start wrote it to flash */
    char cur_ver[64] = {0};
    FILE *cvf = fopen("/data/harmony/VERSION", "r");
    if (cvf) {
        if (!fgets(cur_ver, sizeof(cur_ver), cvf)) cur_ver[0] = '\0';
        fclose(cvf);
        cur_ver[strcspn(cur_ver, "\r\n")] = '\0';
    }
    if (strcmp(cur_ver, HARMONY_VERSION) != 0) {
        FILE *vf = fopen("/data/harmony/VERSION", "w");
        if (vf) {
            fprintf(vf, "%s\n", HARMONY_VERSION);
            fclose(vf);
        }
    }
    hub_info_init(mfg_file);
    struct hub_settings settings;
    config_store_get_settings(&settings);
    apply_hub_settings(&settings);
    if (settings.dry_run) g_dry_run = 1;
    if (g_dry_run) {
        printf("[!] DRY-RUN MODE ACTIVE: All physical IR, BT, and MQTT outputs will be simulated\n");
    }

    hub_events_init();
    orch_init();
    ir_i2s_init();
    ir_i2s_set_capture_state_listener(hub_cmd_ir_capture_state_changed, NULL);
    orch_set_progress_callback(on_orch_progress, NULL);
    printf("[+] Hardware core & Activity Orchestrator initialized (current: %s)\n",
           orch_get_current_activity());

    /* 2. Web & WebSocket Server */
    ws_server_init();
    if (hub_events_subscribe(hub_ws_event_sink, NULL) != 0) {
        fprintf(stderr, "[-] Could not subscribe the WebSocket sink to the event bus\n");
    }
    if (hub_events_subscribe(hbus_event_sink, NULL) != 0) {
        fprintf(stderr, "[-] Could not subscribe the Logitech shim sink to the event bus\n");
    }
    if (state_watch_start() != 0) {
        fprintf(stderr, "[-] Could not start the state watcher: web UIs will not update live\n");
    }
    int srv_fd = http_server_init(port);
    if (srv_fd < 0) {
        fprintf(stderr, "[-] Could not bind HTTP server on port %d: %s\n", port, strerror(errno));
        return 1;
    }
    printf("[+] HTTP & WebSocket server listening on port %d\n", port);

    /* mDNS responder (_harmony-ng._tcp), on this loop; mdns_enabled in the settings */
    if (hub_mdns_init(mdns_port) != 0) {
        fprintf(stderr, "[-] mDNS responder not started\n");
    }
    /* Logitech protocol shim (port 8088) and its SSDP responder; legacy_enabled in the settings */
    if (hbus_init(legacy_port) != 0) {
        fprintf(stderr, "[-] Logitech protocol shim not started\n");
    }
    if (hub_ssdp_init(ssdp_port, legacy_port) != 0) {
        fprintf(stderr, "[-] SSDP responder not started\n");
    }

    /* 3. MQTT Client */
    hw_set_local_api_port(port);
    hub_mqtt_init();

    printf("[+] Entering master non-blocking event loop...\n");

    /* A key held when the previous hub_daemon died (a deploy, the OOM killer) stays pressed on
     * the Bluetooth host: release everything once hub_btstack takes the pipe (it may start
     * after this daemon), trying once a second for a minute */
    time_t bt_release_last = 0;
    int bt_release_tries = 0; /* a counter, not the clock: SNTP may step it by years right after start */
    int bt_release_done = 0;

    /* Initial SNTP network time sync attempt */
    const char *ntp_host = (settings.ntp_server[0]) ? settings.ntp_server : "pool.ntp.org";
    sntp_sync_time(ntp_host, 1);

    while (g_running) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(srv_fd, &rset);
        int max_fd = srv_fd;
        int mdns_fd = hub_mdns_fd();
        if (mdns_fd >= 0 && mdns_fd < FD_SETSIZE) {
            FD_SET(mdns_fd, &rset);
            if (mdns_fd > max_fd) max_fd = mdns_fd;
        }
        int hbus_fd = hbus_listen_fd();
        if (hbus_fd >= 0 && hbus_fd < FD_SETSIZE) {
            FD_SET(hbus_fd, &rset);
            if (hbus_fd > max_fd) max_fd = hbus_fd;
        }
        int ssdp_fd = hub_ssdp_fd();
        if (ssdp_fd >= 0 && ssdp_fd < FD_SETSIZE) {
            FD_SET(ssdp_fd, &rset);
            if (ssdp_fd > max_fd) max_fd = ssdp_fd;
        }

        int client_fds[WS_MAX_CLIENTS];
        int client_count = ws_get_clients(client_fds, WS_MAX_CLIENTS);
        for (int i = 0; i < client_count; i++) {
            if (client_fds[i] < FD_SETSIZE) {
                FD_SET(client_fds[i], &rset);
                if (client_fds[i] > max_fd) max_fd = client_fds[i];
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 40000; /* 40 ms tick */

        int nready = select(max_fd + 1, &rset, NULL, NULL, &tv);
        if (nready > 0) {
            if (FD_ISSET(srv_fd, &rset)) accept_client(srv_fd, http_client_worker);
            if (hbus_fd >= 0 && FD_ISSET(hbus_fd, &rset)) accept_client(hbus_fd, hbus_client_worker);

            for (int i = 0; i < client_count; i++) {
                int cfd = client_fds[i];
                if (FD_ISSET(cfd, &rset)) {
                    if (ws_is_client_legacy(cfd)) hbus_handle_ws_message(cfd);
                    else handle_ws_client_message(cfd);
                }
            }
            if (mdns_fd >= 0 && FD_ISSET(mdns_fd, &rset)) hub_mdns_handle();
            if (ssdp_fd >= 0 && FD_ISSET(ssdp_fd, &rset)) hub_ssdp_handle();
        }

        if (!bt_release_done && time(NULL) != bt_release_last) {
            bt_release_last = time(NULL);
            if (hw_bt_release_all() == 0) {
                bt_release_done = 1;
                printf("[*] BT: release-all sent to hub_btstack after start\n");
            } else if (++bt_release_tries >= 60) {
                bt_release_done = 1;
            }
        }

        /* Tick Activity Orchestrator state machine & delays */
        orch_tick();
        /* Deliver queued events to the sinks (WebSocket, later MQTT) on this thread */
        hub_events_dispatch();
        /* Snapshot for clients that upgraded since the last iteration */
        hub_ws_tick();
        /* Keepalive pings and dead-client reaping */
        ws_server_tick();
        /* mDNS probing, announcements, IP and TXT changes */
        hub_mdns_tick();
        /* Logitech shim: digest for new clients, channel digits; SSDP periodic alive */
        hbus_tick();
        hub_ssdp_tick();

        /* Tick SNTP network time sync */
        sntp_tick(ntp_host);

        /* Tick MQTT client packet reader, keepalive, and periodic state poll */
        hub_mqtt_tick();
        hub_cmd_poll();     /* replies of slow command-channel actions (update_check) */
    }

    printf("\n[*] Shutting down hub_daemon...\n");
    orch_cancel();
    hub_mdns_shutdown();
    hub_ssdp_shutdown();
    hbus_shutdown();
    ir_i2s_capture_mode_set(0, NULL, NULL, NULL, 0);
    ir_i2s_shutdown();
    ws_close_all();
    http_server_close(srv_fd);
    hub_mqtt_shutdown();
    unlink(HARMONY_PID_FILE);

    printf("[+] Shutdown complete.\n");
    return 0;
}
