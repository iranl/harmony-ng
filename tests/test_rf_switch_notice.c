/* The Hub's switch start notice (RF_MSG_ACTIVITY_SYNC with payload[24] RF_ACT_SYNC_STARTING)
 * goes only to remotes that report RF_CAP_SWITCH_START: an older remote takes it for the end of
 * the switch and closes its progress bar. The end notice goes to every remote. A sync query
 * answered mid-switch names the target with RF_ACT_SYNC_QUERY_SWITCHING for such a remote, so a
 * lost end notice does not hold its switch screen. Real hub_rf sources, /dev/rfspi a socket pair;
 * the mock library (HARMONY_MOCK_ACTIVITY_FILES) puts /tmp/activity_* under the mock directory. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "rf_state.h"
#include "rf_spi.h"
#include "rf_sync.h"

static int s_radio_fd = -1;
static int s_fail;
#define CHECK(cond, msg) do { if (!(cond)) { printf("[-] %s\n", msg); s_fail = 1; } else printf("[+] %s\n", msg); } while (0)

/* Activity sync frames per slot in the socket, with the state byte of the last one */
static void drain(int counts[16], int states[16]) {
    memset(counts, 0, 16 * sizeof(int));
    uint8_t buf[64];
    ssize_t n;
    while ((n = recv(s_radio_fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
        if (n >= 30 && buf[0] == 0x12 && buf[3] == RF_MSG_ACTIVITY_SYNC) {
            counts[buf[1] & 0x0F]++;
            states[buf[1] & 0x0F] = buf[5 + 24];
        }
    }
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) { perror("socketpair"); return 1; }
    g_rf_fd = sv[0];
    s_radio_fd = sv[1];

    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = true;
        g_paired_remotes[i].model = REMOTE_MODEL_JUNIPER;
        g_paired_remotes[i].equad_id = RF_EQUAD_JUNIPER;
    }
    g_has_paired_device = true;
    g_remote_versions[0].caps = RF_CAP_LONG_NAMES | RF_CAP_SWITCH_START; /* slot 1: new remote */
    g_remote_versions[1].caps = RF_CAP_LONG_NAMES;                       /* slot 2: older remote */

    if (!getenv("HARMONY_MOCK_ACTIVITY_FILES")) {
        printf("[-] test_rf_switch_notice: run it with HARMONY_MOCK_ACTIVITY_FILES=1 and the mock library\n");
        return 1;
    }
    int counts[16], states[16];
    rf_send_hub_activity_sync_state("3", 1, RF_ACT_SYNC_STARTING);
    drain(counts, states);
    CHECK(counts[1] == 1 && states[1] == RF_ACT_SYNC_STARTING, "start notice sent to the remote that reports it");
    CHECK(counts[2] == 0, "no start notice for the older remote");

    rf_send_hub_activity_sync_state("3", 2, RF_ACT_SYNC_COMPLETE);
    drain(counts, states);
    CHECK(counts[1] == 1 && counts[2] == 1 && states[1] == RF_ACT_SYNC_COMPLETE && states[2] == RF_ACT_SYNC_COMPLETE,
          "end notice sent to both remotes");

    FILE *fs = fopen("/tmp/activity_state", "w");
    FILE *ft = fopen("/tmp/activity_target", "w");
    if (!fs || !ft) { perror("/tmp/activity_*"); return 1; }
    fputs("starting\n", fs);
    fclose(fs);
    fputs("3\n", ft);
    fclose(ft);
    rf_send_hub_activity_query_reply(3, 1);
    rf_send_hub_activity_query_reply(4, 2);
    drain(counts, states);
    CHECK(counts[1] == 1 && states[1] == RF_ACT_SYNC_QUERY_SWITCHING, "query mid-switch: the new remote hears the switch runs");
    CHECK(counts[2] == 1 && states[2] == RF_ACT_SYNC_QUERY, "query mid-switch: the older remote gets the plain answer");
    unlink("/tmp/activity_state");
    unlink("/tmp/activity_target");
    rf_send_hub_activity_query_reply(5, 1);
    drain(counts, states);
    CHECK(counts[1] == 1 && states[1] == RF_ACT_SYNC_QUERY, "query after the switch: plain answer");

    /* A remote whose caps changed keeps a step list built for the old ones: a new config version
       makes it ask for the push */
    snprintf(g_remote_versions[1].sw_version, sizeof(g_remote_versions[1].sw_version), "2.5.2");
    g_config_version = 5;
    uint8_t rep[25] = {2, 5, 3, 5, RF_CAP_LONG_NAMES | RF_CAP_SWITCH_START};
    rf_frame_t vf;
    rf_build_frame(&vf, 2, RF_MSG_VERSION_REPORT, 6, 0, rep, 25);
    handle_rf_frame(&vf);
    CHECK(g_config_version == 6, "caps changed: new config version");
    rf_build_frame(&vf, 2, RF_MSG_VERSION_REPORT, 7, 0, rep, 25);
    handle_rf_frame(&vf);
    CHECK(g_config_version == 6, "the same caps again: no new version");
    drain(counts, states);

    close(sv[0]);
    close(sv[1]);
    if (s_fail) {
        printf("[-] test_rf_switch_notice: FAILED\n");
        return 1;
    }
    printf("[+] test_rf_switch_notice: all checks passed\n");
    return 0;
}
