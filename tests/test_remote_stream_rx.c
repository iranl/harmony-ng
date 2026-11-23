/* The remote's side of a streamed config push (remote_rf_sync.c): after CONFIG_END clear with
 * the stream flag, a frame is taken only when it is the next in order; stream frames outside a
 * push, or after it went quiet, are dropped. */
#include <stdio.h>
#include <string.h>

#include "remote/remote_state.h"
#include "remote/remote_rf_sync.h"

static int s_fail;
#define CHECK(cond, msg) do { if (!(cond)) { printf("[-] %s\n", msg); s_fail = 1; } else printf("[+] %s\n", msg); } while (0)

static void hub_frame(uint8_t type, uint8_t seq, uint8_t flags, uint8_t p0, uint8_t p4) {
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.dev_slot = 1;
    f.msg_type = type;
    f.seq = seq;
    f.hdr_flag = 0x40 | (seq & 0x0F);
    f.flags = flags;
    f.payload[0] = p0;
    f.payload[1] = 8;   /* activity list: total */
    f.payload[3] = '1'; /* id */
    f.payload[4] = p4;
    handle_hub_frame_remote(&f);
}

int main(void) {
    printf("[-] stream frame without a streamed push\n");
    hub_frame(RF_MSG_ACTIVITY_LIST_CHUNK, 0, RF_FLAG_STREAM, 0, 0);
    CHECK(g_sync_chunk_count == 0, "dropped");

    printf("[-] streamed push\n");
    rf_frame_t clr;
    memset(&clr, 0, sizeof(clr));
    clr.dev_slot = 1;
    clr.msg_type = RF_MSG_CONFIG_END;
    clr.flags = RF_FLAG_ACK_REQ;
    clr.payload[0] = 0x00; /* clear */
    clr.payload[1] = 3;
    clr.payload[2] = 4;    /* total */
    clr.payload[4] = 1;    /* streamed */
    handle_hub_frame_remote(&clr);
    CHECK(g_is_syncing_config, "the clear starts a sync");

    hub_frame(RF_MSG_ACTIVITY_LIST_CHUNK, 0, RF_FLAG_STREAM, 0, 0);
    CHECK(g_sync_chunk_count == 1, "frame 0 taken");
    hub_frame(RF_MSG_ACTIVITY_LIST_CHUNK, 2, RF_FLAG_STREAM | RF_FLAG_ACK_REQ, 2, 0);
    CHECK(g_sync_chunk_count == 1, "frame 2 before frame 1 dropped");
    hub_frame(RF_MSG_ACTIVITY_LIST_CHUNK, 0, RF_FLAG_STREAM, 0, 0);
    CHECK(g_sync_chunk_count == 1, "frame 0 again (a resend) dropped");
    hub_frame(RF_MSG_ACTIVITY_LIST_CHUNK, 1, RF_FLAG_STREAM, 1, 0);
    hub_frame(RF_MSG_ACTIVITY_LIST_CHUNK, 2, RF_FLAG_STREAM, 2, 0);
    CHECK(g_sync_chunk_count == 3, "frames 1 and 2 taken in order");

    printf("[-] after the push went quiet\n");
    remote_stream_tick(get_monotonic_ms() + 4000);
    hub_frame(RF_MSG_ACTIVITY_LIST_CHUNK, 3, RF_FLAG_STREAM, 3, 0);
    CHECK(g_sync_chunk_count == 3, "stream frames dropped");

    if (s_fail) {
        printf("[-] test_remote_stream_rx: FAILED\n");
        return 1;
    }
    printf("[+] test_remote_stream_rx: all checks passed\n");
    return 0;
}
