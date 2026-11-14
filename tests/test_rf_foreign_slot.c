/* hub_rf keeps state for two remote slots, the CC2544 has more pairing slots. A protocol frame
 * from a device in slot 3 or higher was handled as a frame from slot 1: its version report
 * overwrote slot 1's version, its unpair notice unpaired the remote in slot 1. Runs the real
 * hub_rf frame handler (main renamed) with /dev/rfspi replaced by a socket pair. */
#undef main
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "rf_state.h"
#include "rf_spi.h"

static int s_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

static void frame_from(uint8_t slot, uint8_t type, const uint8_t *payload, size_t len) {
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.report_id = 0x20;
    f.dev_slot = slot;
    f.sub_id = 0x12;
    f.hdr_flag = 0x41;
    f.seq = 1;
    f.msg_type = type;
    memcpy(f.payload, payload, len);
    handle_rf_frame(&f);
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0]; /* the replies go to the other end, which nobody reads */

    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = true;
    }
    g_has_paired_device = true;
    snprintf(g_remote_versions[0].sw_version, sizeof(g_remote_versions[0].sw_version), "%s", "2.3.0");
    g_remote_versions[0].config_version = 7;

    const uint8_t ver[5] = {9, 9, 9, 42, 0x07};
    frame_from(3, RF_MSG_VERSION_REPORT, ver, sizeof(ver));
    CHECK(strcmp(g_remote_versions[0].sw_version, "2.3.0") == 0 && g_remote_versions[0].config_version == 7,
          "a version report from slot 3 changed slot 1 to %s, config %u", g_remote_versions[0].sw_version,
          g_remote_versions[0].config_version);

    const uint8_t none[1] = {0};
    frame_from(3, RF_MSG_UNPAIR_NOTIFY, none, sizeof(none));
    CHECK(g_paired_remotes[0].active, "an unpair notice from slot 3 unpaired the remote in slot 1");

    /* Control: the remote in slot 1 itself */
    frame_from(1, RF_MSG_VERSION_REPORT, ver, sizeof(ver));
    CHECK(strcmp(g_remote_versions[0].sw_version, "9.9.9") == 0 && g_remote_versions[0].config_version == 42,
          "a version report from slot 1 must still update slot 1 (%s)", g_remote_versions[0].sw_version);

    if (s_failures) {
        printf("[-] test_rf_foreign_slot: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] test_rf_foreign_slot: frames from slots without a remote are not applied to slot 1\n");
    return 0;
}
