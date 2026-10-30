#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>

#include "rf_state.h"
#include "rf_api.h"

uint8_t g_tx_seq = 0;

/* Minimal mock function stubs required by rf_api.c */
bool is_ota_busy(void) { return false; }
bool is_slot_direct_mode(int slot) { (void)slot; return false; }
const char *get_current_activity(void) { return "-1"; }
void start_pairing_mode(int slot) { (void)slot; }
void stop_pairing_mode(void) {}
void unpair_device(int slot) { (void)slot; }
void rf_write_cmd(const uint8_t *b, size_t l) { (void)b; (void)l; }
int rf_send_frame(uint8_t pipe, uint8_t msg_type, uint8_t flags, const uint8_t *payload, size_t len) {
    (void)pipe; (void)msg_type; (void)flags; (void)payload; (void)len; return 0;
}
int rf_send_frame_acked(uint8_t pipe, uint8_t msg_type, uint8_t flags, const uint8_t *payload, size_t len, int retries) {
    (void)pipe; (void)msg_type; (void)flags; (void)payload; (void)len; (void)retries; return 0;
}
void rf_send_hub_activity_sync(void) {}
void rf_send_settings_push_slot(int slot) { (void)slot; }
void rf_notify_update_available(int slot) { (void)slot; }
void rf_push_custom_icon(const char *id, const char *type) { (void)id; (void)type; }
void record_config_diff(void) {}
void *config_push_worker(void *arg) { (void)arg; return NULL; }
void *rf_ota_sender_worker(void *arg) { (void)arg; return NULL; }

int main(void) {
    pthread_t th;
    printf("[-] Starting api_server_worker thread...\n");
    if (pthread_create(&th, NULL, api_server_worker, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    usleep(100000); /* 100ms for server to bind and listen */

    /* 1. Verify loopback connection (127.0.0.1:8092) succeeds */
    int s_loop = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sin_loop;
    memset(&sin_loop, 0, sizeof(sin_loop));
    sin_loop.sin_family = AF_INET;
    sin_loop.sin_port = htons(RF_API_PORT);
    sin_loop.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rc = connect(s_loop, (struct sockaddr *)&sin_loop, sizeof(sin_loop));
    if (rc != 0) {
        fprintf(stderr, "FAIL: Unable to connect to loopback 127.0.0.1:%d: %s\n", RF_API_PORT, strerror(errno));
        return 1;
    }
    printf("[+] PASS: Successfully connected to 127.0.0.1:%d\n", RF_API_PORT);
    close(s_loop);

    /* 2. Verify binding to another IP on port 8092 succeeds because 8092 is NOT bound to INADDR_ANY (0.0.0.0) */
    int s_bind_test = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sin_bind;
    memset(&sin_bind, 0, sizeof(sin_bind));
    sin_bind.sin_family = AF_INET;
    sin_bind.sin_port = htons(RF_API_PORT);
    inet_pton(AF_INET, "127.0.0.2", &sin_bind.sin_addr);

    int bind_rc = bind(s_bind_test, (struct sockaddr *)&sin_bind, sizeof(sin_bind));
    if (bind_rc != 0) {
        fprintf(stderr, "FAIL: Port 8092 is bound to wildcard INADDR_ANY (bind to 127.0.0.2 failed: %s)\n", strerror(errno));
        return 1;
    }
    printf("[+] PASS: Successfully verified 8092 is NOT bound to INADDR_ANY (127.0.0.2 bind succeeded)\n");
    close(s_bind_test);

    g_running = false;
    pthread_cancel(th);
    pthread_join(th, NULL);

    printf("[+] All RF loopback binding checks passed!\n");
    return 0;
}
