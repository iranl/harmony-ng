/* hub_rf frame handler on frames it does not expect: every message type, from the two remote
 * slots and a foreign one, with random payload bytes (strings without a terminator, counts and
 * indexes out of range), 0xFF everywhere and all zeros. Each frame sits alone in a heap block
 * of exactly its size, so AddressSanitizer reports any read past it. The test passes when the
 * handler neither crashes nor reads or writes out of bounds. Runs the real hub_rf sources (main
 * renamed) with /dev/rfspi replaced by a socket pair (drained by a thread) and /data on a
 * private mock root. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include "rf_state.h"
#include "rf_spi.h"

static uint32_t s_rng = 0x2545F491u;
static uint8_t rnd8(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (uint8_t)s_rng;
}

static void send_frame(uint8_t slot, uint8_t type, uint8_t flags, int fill) {
    rf_frame_t *f = malloc(sizeof(*f));
    if (!f) exit(1);
    f->report_id = 0x20;
    f->dev_slot = slot;
    f->sub_id = 0x12;
    f->seq = rnd8();
    f->hdr_flag = 0x40 | (f->seq & 0x0F);
    f->msg_type = type;
    f->flags = flags;
    for (size_t i = 0; i < sizeof(f->payload); i++)
        f->payload[i] = fill == 0 ? rnd8() : (fill == 1 ? 0xFF : 0x00);
    handle_rf_frame(f);
    free(f);
}

/* The radio end: reads and drops what hub_rf sends, so its writes never block */
static void *drain(void *arg) {
    int fd = *(int *)arg;
    uint8_t buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {}
    return NULL;
}

int main(void) {
    static int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0];
    pthread_t th;
    if (pthread_create(&th, NULL, drain, &sv[1]) != 0) return 1;

    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = true;
    }
    g_has_paired_device = true;

    static const uint8_t slots[] = {1, 2, 3, 0x81};
    int frames = 0;
    for (int round = 0; round < 3; round++) {
        for (size_t s = 0; s < sizeof(slots); s++) {
            for (int type = 0; type < 0x40; type++) {
                /* the OTA and icon transfers start sender threads on request: kept out, their
                 * frame layouts are covered by test_rf_ota_lifecycle and test_rf_push_targets */
                if (type == RF_MSG_OTA_REQUEST) continue;
                send_frame(slots[s], (uint8_t)type, (uint8_t)(round == 0 ? 0 : rnd8() & 0x03), round);
                frames++;
            }
        }
    }
    for (int i = 0; i < 2000; i++) {
        uint8_t type = rnd8() & 0x3F;
        if (type == RF_MSG_OTA_REQUEST) continue;
        send_frame((uint8_t)(1 + (rnd8() & 1)), type, rnd8(), 0);
        frames++;
    }
    printf("[+] test_rf_frame_fuzz: %d frames handled without a memory error\n", frames);
    return 0;
}
