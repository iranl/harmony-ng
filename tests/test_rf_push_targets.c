/* config_push_worker() picks the remotes to push to, the same way for every slot: the paired
 * touchscreen remotes heard since hub_rf started, the linked ones first and, with all of them
 * asleep, all of them. A slot the radio reported last but that holds no remote gets nothing.
 * A remote not heard yet gets no push, and the push stays pending for it (it asks once it wakes).
 * (With two slots the fallback once wrote three entries into two-element arrays.) Runs the real
 * hub_rf sources (main renamed) with /dev/rfspi replaced by a socket pair and a radio thread that
 * ACKs every frame; built with AddressSanitizer by run_mock_suite.sh. */
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

static void pair_only(int a, int b, bool heard) {
    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = (i + 1 == a || i + 1 == b);
        g_paired_remotes[i].heard = heard;
        g_paired_remotes[i].link_lost = true;
        g_paired_remotes[i].model = REMOTE_MODEL_JUNIPER;
        g_paired_remotes[i].equad_id = RF_EQUAD_JUNIPER;
    }
    memset(s_frames_per_slot, 0, sizeof(s_frames_per_slot));
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) { perror("socketpair"); return 1; }
    g_rf_fd = sv[0];
    s_radio_fd = sv[1];
    pthread_t th;
    pthread_create(&th, NULL, radio_worker, NULL);
    int fail = 0;

    /* Touchscreen remotes in slots 1 and 4, heard and asleep (link lost); a link report from
     * slot 3 */
    pair_only(1, 4, true);
    g_has_paired_device = true;
    g_remote_slot = 3;
    g_config_version = 7;
    config_push_worker(NULL);
    usleep(20000);
    printf("frames: slot1=%d slot3=%d slot4=%d\n", s_frames_per_slot[1], s_frames_per_slot[3], s_frames_per_slot[4]);
    if (s_frames_per_slot[1] == 0 || s_frames_per_slot[4] == 0) {
        printf("[-] the push did not reach both paired remotes (slots 1 and 4)\n");
        fail = 1;
    }
    for (int s = 0; s < 16; s++) {
        if (s != 1 && s != 4 && s_frames_per_slot[s]) {
            printf("[-] the push sent %d frames to slot %d, which has no remote\n", s_frames_per_slot[s], s);
            fail = 1;
        }
    }

    /* A remote in slot 2 not heard since the start: no frames, the push stays pending */
    pair_only(2, 2, false);
    FILE *pf = fopen("/data/harmony/rf_sync_pending.flag", "w");
    if (pf) { fputs("1\n", pf); fclose(pf); }
    config_push_worker(NULL);
    usleep(20000);
    for (int s = 0; s < 16; s++) {
        if (s_frames_per_slot[s]) {
            printf("[-] the push sent %d frames to slot %d before its remote spoke\n", s_frames_per_slot[s], s);
            fail = 1;
        }
    }
    if (access("/data/harmony/rf_sync_pending.flag", F_OK) != 0) {
        printf("[-] the pending push was dropped while a remote had not spoken yet\n");
        fail = 1;
    }

    s_stop = 1;
    shutdown(sv[0], SHUT_RDWR);
    pthread_join(th, NULL);
    if (fail) return 1;
    printf("[+] test_rf_push_targets: config push goes to the heard remotes of any slot only\n");
    return 0;
}
