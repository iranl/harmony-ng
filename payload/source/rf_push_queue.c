#define _GNU_SOURCE
#include "rf_push_queue.h"
#include "rf_spi.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Streamed push (remotes with RF_CAP_STREAM_PUSH), the config counterpart of the windowed OTA:
 * both radios in RF_PM_TRANSFER, frames sent without waiting, an ACK asked for every
 * PUSH_WIN_ACK_EVERY frames that carries the remote's count of frames received in order. The
 * frame index travels in the 4-bit hdr_flag sequence, so go-back-N keeps fewer than 16 frames in
 * flight. In RF_PM_ACTIVE the remote gets a Hub frame only with the ACK of one it sends, which
 * held the stop-and-wait push to about 7 frames a second. */
#define PUSH_WIN_IN_FLIGHT  15
#define PUSH_WIN_ACK_EVERY  4
#define PUSH_WIN_GAP_US     2000
/* An ACK is due every few ms (4 frames at 2 ms); a resend that was not needed is dropped by
 * the remote as out of order, so a short wait costs only air time. Tune on the hardware. */
#define PUSH_WIN_REWIND_MS  100
#define PUSH_WIN_STALL_MS   10000

static volatile uint16_t s_stream_contig;
static volatile uint8_t s_stream_slot; /* slot of the streamed push in progress, 0 = none */

void push_note_stream_ack(uint8_t slot, const uint8_t *payload) {
    if (!payload || payload[4] != RF_PUSH_STREAMED) return;
    if (!s_stream_slot || (slot & 0x0F) != s_stream_slot) return;
    uint16_t c = (uint16_t)(payload[2] | (payload[3] << 8));
    if (c > s_stream_contig) s_stream_contig = c; /* ACKs can arrive out of order */
}

/* 0 when the remote has every frame, -1 when it stopped answering */
static int push_stream(uint8_t slot, push_queue_t *q) {
    int n = q->count;
    s_stream_contig = 0;
    s_stream_slot = slot & 0x0F;
    rf_set_power_mode(RF_PM_TRANSFER);
    int next = 0, last_contig = 0, rewinds = 0, rc = -1;
    uint64_t t0 = get_monotonic_ms(), last_progress = t0, last_rewind = t0;
    while (g_running) {
        int contig = s_stream_contig;
        uint64_t now = get_monotonic_ms();
        if (contig >= n) { rc = 0; break; }
        if (contig > last_contig) {
            last_contig = contig;
            last_progress = now;
        }
        if (now - last_progress > PUSH_WIN_STALL_MS) {
            printf("[-] Streamed push to slot %d: no progress for %d s at frame %d/%d\n",
                   slot, PUSH_WIN_STALL_MS / 1000, contig, n);
            break;
        }
        if (now - last_progress > PUSH_WIN_REWIND_MS && now - last_rewind > PUSH_WIN_REWIND_MS && next > contig) {
            next = contig; /* go back to the first frame the remote is missing */
            rewinds++;
            last_rewind = now;
        }
        if (next < contig) next = contig;
        if (next < n && next - contig < PUSH_WIN_IN_FLIGHT) {
            rf_frame_t *qf = &q->frames[next];
            bool want_ack = next + 1 == n || next % PUSH_WIN_ACK_EVERY == PUSH_WIN_ACK_EVERY - 1 ||
                            next - contig == PUSH_WIN_IN_FLIGHT - 1;
            qf->seq = (uint8_t)(next & 0x0F);
            qf->flags = RF_FLAG_STREAM | (want_ack ? RF_FLAG_ACK_REQ : 0);
            qf->hdr_flag = (uint8_t)((want_ack ? 0x50 : 0x40) | qf->seq);
            rf_send_frame(qf);
            next++;
            usleep(PUSH_WIN_GAP_US);
        } else {
            usleep(1000); /* window full or everything sent: wait for ACKs */
        }
    }
    s_stream_slot = 0;
    /* an OTA that started meanwhile keeps its transfer mode */
    if (!is_ota_busy()) rf_set_power_mode(RF_PM_ACTIVE);
    if (rc == 0) {
        printf("[+] Streamed push to slot %d: %d frames in %llu ms (%d resends from a gap)\n", slot, n,
               (unsigned long long)(get_monotonic_ms() - t0), rewinds);
    }
    return rc;
}

/* Config frames are lost when the CC2544 NAKs a send; retry until the remote ACKs. */
static int rf_send_config_frame(rf_frame_t *frame) {
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (rf_send_frame_acked(frame, 300) == 0) return 0;
    }
    printf("[-] No ACK for config frame type=0x%02X seq=%d after 3 attempts\n", frame->msg_type, frame->seq);
    return -1;
}

void push_queue_add(push_queue_t *q, const rf_frame_t *f) {
    if (q->count == q->cap) {
        int cap = q->cap ? q->cap * 2 : 128;
        rf_frame_t *grown = realloc(q->frames, (size_t)cap * sizeof(*grown));
        if (!grown) { q->oom = true; return; }
        q->frames = grown;
        q->cap = cap;
    }
    q->frames[q->count++] = *f;
}

static bool is_config_chunk(uint8_t msg_type) {
    return msg_type == RF_MSG_ACTIVITY_LIST_CHUNK || msg_type == RF_MSG_DEVICE_LIST_CHUNK ||
           msg_type == RF_MSG_DEVICE_CMD_CHUNK || msg_type == RF_MSG_BUTTON_LIST_CHUNK ||
           msg_type == RF_MSG_STEP_LIST_CHUNK;
}

/* Send a remote its queued config, opened by CONFIG_END clear carrying the chunk total.
   Stops at the first frame the remote never ACKs, so it never gets CONFIG_END complete
   (and the new version) for a partial config. A remote that reports RF_CAP_DEVICE_CMDS ACKs
   device command frames like any other. One without the bit may predate device browsing and
   drop them, so they are skipped if it never ACKs the first one.
   Returns the number of device commands delivered, or -1 if the push failed. */
int push_queue_send(uint8_t slot, uint8_t config_ver, push_queue_t *q) {
    if (q->oom) return -1;
    int total = 0;
    for (int i = 0; i < q->count; i++) {
        if (is_config_chunk(q->frames[i].msg_type)) total++;
    }

    bool stream = slot >= 1 && slot <= MAX_PAIRED_REMOTES &&
                  (g_remote_versions[slot - 1].caps & RF_CAP_STREAM_PUSH);
    rf_frame_t f;
    /* payload[4] = 1: the frames up to CONFIG_END complete follow streamed */
    uint8_t payload[5] = { 0x00 /* clear */, config_ver, (uint8_t)(total & 0xFF), (uint8_t)(total >> 8),
                           (uint8_t)(stream ? 1 : 0) };
    rf_build_frame(&f, slot, RF_MSG_CONFIG_END, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, sizeof(payload));
    if (rf_send_config_frame(&f) != 0) return -1;
    usleep(20000);

    if (stream) {
        if (push_stream(slot, q) != 0) return -1;
        int dev_cmds = 0;
        for (int i = 0; i < q->count; i++) {
            if (q->frames[i].msg_type == RF_MSG_DEVICE_CMD_CHUNK) dev_cmds++;
        }
        return dev_cmds;
    }

    int dev_cmds = 0;
    bool skip_dev_cmds = false;
    for (int i = 0; i < q->count; i++) {
        rf_frame_t *qf = &q->frames[i];
        bool is_dev_cmd = qf->msg_type == RF_MSG_DEVICE_CMD_CHUNK;
        if (is_dev_cmd && skip_dev_cmds) continue;
        qf->seq = ++g_tx_seq;
        qf->hdr_flag = 0x50 | (qf->seq & 0x0F);
        if (rf_send_config_frame(qf) == 0) {
            if (is_dev_cmd) dev_cmds++;
        } else if (is_dev_cmd && dev_cmds == 0 && !(slot >= 1 && slot <= MAX_PAIRED_REMOTES &&
                                                    (g_remote_versions[slot - 1].caps & RF_CAP_DEVICE_CMDS))) {
            printf("[-] Slot %d did not ACK device commands; remote has no device browsing\n", slot);
            skip_dev_cmds = true;
            continue;
        } else {
            return -1;
        }
        usleep(15000);
    }
    return dev_cmds;
}
