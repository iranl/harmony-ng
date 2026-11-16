/*
 * test_remote_shared_state.c - state that two threads of remote_ui share.
 *
 *  1. The 60 s OTA timeout of the main loop (ota_check_timeout) and the RF thread's frame
 *     handlers: the timeout must not close the transfer while a handler holds g_ota_lock, nor a
 *     transfer that a new OTA_START began after the main loop read its clock; a real silence of
 *     60 s still ends OTA mode.
 *  2. Pairing stopped and started again within a second: only one worker may go on sending
 *     pairing requests (the old one used to wake from its sleep, find "pairing" again and go on
 *     next to the new one). The CC2544 is a pipe here; the requests written to it are counted.
 *
 * Run with the mock library and a fresh mock root:
 *   HARMONY_MOCK_DIR=<dir> LD_PRELOAD=libmock_hardware.so ./test_remote_shared_state
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include "remote/remote_state.h"
#include "remote/remote_rf.h"
#include "remote/remote_rf_ota.h"

static int s_fail;
#define CHECK(cond, msg) do { if (!(cond)) { printf("[-] %s\n", msg); s_fail = 1; } else printf("[+] %s\n", msg); } while (0)

static FILE *open_transfer_file(void) {
    return fopen("/dev/null", "w");
}

static void *hold_ota_lock(void *arg) {
    pthread_mutex_lock(&g_ota_lock);
    usleep(300000);
    pthread_mutex_unlock(&g_ota_lock);
    (void)arg;
    return NULL;
}

/* Pairing requests (10 FF 80 B2 01) among the bytes written to the CC2544 pipe */
static int count_pair_requests(int rd) {
    uint8_t buf[4096];
    int n = 0;
    ssize_t got;
    size_t len = 0;
    while ((got = read(rd, buf + len, sizeof(buf) - len)) > 0) len += (size_t)got;
    for (size_t i = 0; i + 5 <= len; i++) {
        if (buf[i] == 0x10 && buf[i + 1] == 0xFF && buf[i + 2] == 0x80 && buf[i + 3] == 0xB2 && buf[i + 4] == 0x01) n++;
    }
    return n;
}

int main(void) {
    g_rf_fd = -1;

    /* 1. OTA timeout */
    g_is_ota_updating = true;
    g_ota_file = open_transfer_file();
    g_ota_last_packet_ms = get_monotonic_ms() > 61000 ? get_monotonic_ms() - 61000 : 1;
    pthread_t th;
    pthread_create(&th, NULL, hold_ota_lock, NULL);
    usleep(50000);
    ota_check_timeout();
    CHECK(g_is_ota_updating && g_ota_file, "no timeout while a frame handler holds the OTA lock");
    pthread_join(th, NULL);

    g_ota_last_packet_ms = get_monotonic_ms(); /* an OTA_START was handled meanwhile */
    ota_check_timeout();
    CHECK(g_is_ota_updating && g_ota_file, "a transfer started after the main loop read its clock is kept");

    g_ota_last_packet_ms = get_monotonic_ms() > 61000 ? get_monotonic_ms() - 61000 : 1;
    ota_check_timeout();
    CHECK(!g_is_ota_updating && !g_ota_file, "60 s without a chunk ends OTA mode and closes the file");

    /* 2. Pairing workers */
    int p[2];
    if (pipe2(p, O_NONBLOCK) != 0) return 1;
    g_rf_fd = p[1];
    g_running = true;
    rf_start_pairing();     /* worker A: one request, then a 1 s sleep */
    usleep(100000);
    rf_stop_pairing();
    usleep(100000);
    rf_start_pairing();     /* worker B */
    usleep(1500000);        /* A wakes at 1.0 s, B at 1.2 s */
    int requests = count_pair_requests(p[0]);
    rf_stop_pairing();
    usleep(1100000);        /* let B end */
    g_rf_fd = -1;
    printf("[*] pairing requests in 1.5 s: %d\n", requests);
    CHECK(requests == 3, "a stop and a new start leave one pairing worker (3 requests in 1.5 s, not 4)");

    if (s_fail) {
        printf("[-] test_remote_shared_state: FAILED\n");
        return 1;
    }
    printf("[+] test_remote_shared_state: all checks passed\n");
    return 0;
}
