#define _GNU_SOURCE
#include "rf_push_queue.h"
#include "rf_spi.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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
   (and the new version) for a partial config. A remote without device browsing drops
   device command frames, so they are skipped if it never ACKs the first one.
   Returns the number of device commands delivered, or -1 if the push failed. */
int push_queue_send(uint8_t slot, uint8_t config_ver, push_queue_t *q) {
    if (q->oom) return -1;
    int total = 0;
    for (int i = 0; i < q->count; i++) {
        if (is_config_chunk(q->frames[i].msg_type)) total++;
    }

    rf_frame_t f;
    uint8_t payload[4] = { 0x00 /* clear */, config_ver, (uint8_t)(total & 0xFF), (uint8_t)(total >> 8) };
    rf_build_frame(&f, slot, RF_MSG_CONFIG_END, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, sizeof(payload));
    if (rf_send_config_frame(&f) != 0) return -1;
    usleep(20000);

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
        } else if (is_dev_cmd && dev_cmds == 0) {
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
