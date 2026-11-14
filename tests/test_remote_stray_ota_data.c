/*
 * test_remote_stray_ota_data.c - A windowed OTA data frame (header 0x60) has no message type: the
 * byte where a typed frame has its type is bits 4..11 of the chunk index. The remote takes such
 * frames as OTA data only in OTA mode; outside it (remote_ui restarted while the Hub still sends
 * a transfer) they went on to the typed-frame path, so chunk 0x0E5 was read as a SETTINGS_PUSH and
 * chunk 0x1B5 as an UNPAIR_NOTIFY. The real RF receive thread reads the frames from a socket pair
 * that stands for /dev/rfspi.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>

#include "remote/remote_state.h"
#include "remote/remote_rf_sync.h"
#include "rf_proto.h"

static int s_hub_fd = -1;

/* What the Hub's rf_send_ota_data() puts on the air, as the remote's CC2544 reports it */
static void send_data_frame(uint16_t idx, uint8_t fill) {
    uint8_t pkt[32];
    memset(pkt, fill, sizeof(pkt));
    pkt[0] = 0x20;
    pkt[1] = 0x01;
    pkt[2] = 0x12;
    pkt[3] = 0x00;
    pkt[4] = (uint8_t)(RF_OTA_HDR_DATA | (idx & 0x0F));
    pkt[5] = (uint8_t)((idx >> 4) & 0xFF);
    if (write(s_hub_fd, pkt, sizeof(pkt)) != (ssize_t)sizeof(pkt)) perror("write");
}

/* A typed ACTIVITY_SYNC, to know when the frames before it were handled */
static void send_activity_sync(const char *id) {
    uint8_t pkt[32] = {0};
    pkt[0] = 0x20;
    pkt[1] = 0x01;
    pkt[2] = 0x12;
    pkt[4] = 0x40;
    pkt[5] = RF_MSG_ACTIVITY_SYNC;
    snprintf((char *)&pkt[7], 4, "%s", id);
    pkt[7 + 24] = 1;
    if (write(s_hub_fd, pkt, sizeof(pkt)) != (ssize_t)sizeof(pkt)) perror("write");
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0];
    s_hub_fd = sv[1];

    /* A paired remote with its settings, not in OTA mode */
    g_is_ota_updating = false;
    g_rf_link_connected = true;
    g_pairing_state = 2;
    g_current_view = VIEW_ACTIVITIES;
    g_timeout_seconds = 10;
    g_brightness_val = 2;
    g_no_screen_mode = false;
    g_has_hub_config = true;
    g_config_version = 12;
    snprintf(g_active_activity_id, sizeof(g_active_activity_id), "%s", "-1");

    g_running = true;
    pthread_t th;
    pthread_create(&th, NULL, rf_rx_worker, NULL);

    send_data_frame(0x0E5, 0x05); /* type byte 0x0E = RF_MSG_SETTINGS_PUSH */
    send_data_frame(0x1B5, 0x05); /* type byte 0x1B = RF_MSG_UNPAIR_NOTIFY */
    send_activity_sync("7");
    for (int i = 0; i < 200 && strcmp(g_active_activity_id, "7") != 0; i++) usleep(10000);

    g_running = false;
    shutdown(sv[1], SHUT_RDWR);
    pthread_join(th, NULL);

    int fail = 0;
    if (strcmp(g_active_activity_id, "7") != 0) {
        printf("[-] the receive thread did not handle the frames (activity \"%s\")\n", g_active_activity_id);
        fail = 1;
    }
    if (g_timeout_seconds != 10 || g_brightness_val != 2) {
        printf("[-] a data frame outside OTA mode was taken as a settings push (timeout %d s, brightness %d)\n",
               g_timeout_seconds, g_brightness_val);
        fail = 1;
    }
    if (!g_has_hub_config || g_config_version != 12) {
        printf("[-] a data frame outside OTA mode was taken as an unpair notice (Hub config dropped, version %d)\n",
               (int)g_config_version);
        fail = 1;
    }
    if (fail) return 1;
    printf("[+] test_remote_stray_ota_data: OTA data frames outside OTA mode are dropped\n");
    return 0;
}
