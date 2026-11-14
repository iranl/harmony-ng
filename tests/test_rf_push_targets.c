/* config_push_worker() picks the remotes to push to. With both touchscreen remotes asleep it falls
 * back to the last slot the radio reported plus slots 1 and 2; that slot could be 3 or higher (a
 * link report for another CC2544 slot), and the fallback then wrote three entries into two-element
 * arrays. Runs the real hub_rf sources (main renamed) with /dev/rfspi replaced by a socket pair and
 * a radio thread that ACKs every frame; built with AddressSanitizer by run_mock_suite.sh. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include "rf_state.h"
#include "rf_spi.h"
#include "rf_config_push.h"

static int s_radio_fd = -1;
static int s_frames_per_slot[16];
static volatile int s_stop;

static void *radio_worker(void *arg) {
    (void)arg;
    uint8_t buf[64];
    while (!s_stop) {
        ssize_t n = recv(s_radio_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (n >= 30 && buf[0] == 0x12) {
            s_frames_per_slot[buf[1] & 0x0F]++;
            usleep(1000);
            rf_signal_ack(buf[2] & 0x0F, 0, NULL);
        }
    }
    return NULL;
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) { perror("socketpair"); return 1; }
    g_rf_fd = sv[0];
    s_radio_fd = sv[1];
    pthread_t th;
    pthread_create(&th, NULL, radio_worker, NULL);

    /* Two touchscreen remotes, paired and asleep (link lost), and a link report from slot 3 */
    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = true;
        g_paired_remotes[i].link_lost = true;
        g_paired_remotes[i].model = REMOTE_MODEL_JUNIPER;
        g_paired_remotes[i].equad_id = RF_EQUAD_JUNIPER;
    }
    g_has_paired_device = true;
    g_remote_slot = 3;
    g_config_version = 7;

    config_push_worker(NULL);

    s_stop = 1;
    shutdown(sv[0], SHUT_RDWR);
    pthread_join(th, NULL);

    int fail = 0;
    printf("frames: slot1=%d slot2=%d slot3=%d\n", s_frames_per_slot[1], s_frames_per_slot[2], s_frames_per_slot[3]);
    if (s_frames_per_slot[1] == 0 || s_frames_per_slot[2] == 0) {
        printf("[-] the push did not reach both paired remotes\n");
        fail = 1;
    }
    for (int s = 3; s < 16; s++) {
        if (s_frames_per_slot[s]) {
            printf("[-] the push sent %d frames to slot %d, which has no remote\n", s_frames_per_slot[s], s);
            fail = 1;
        }
    }
    if (fail) return 1;
    printf("[+] test_rf_push_targets: config push goes to slots 1 and 2 only\n");
    return 0;
}
