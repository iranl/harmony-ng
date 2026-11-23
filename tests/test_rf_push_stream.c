/* Streamed config push (rf_push_queue.c): a simulated remote that drops frames and takes them
 * only in order must still get every frame once, in order, ending with CONFIG_END complete. */
#include "rf_push_queue.h"
#include "rf_spi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

volatile bool g_running = true;
uint8_t g_tx_seq;
remote_version_t g_remote_versions[MAX_PAIRED_REMOTES];

uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int s_fail;
#define CHECK(c, m) do { if (c) printf("  PASS %s\n", m); else { printf("  FAIL %s\n", m); s_fail = 1; } } while (0)

/* The simulated remote */
static int s_drop_every;   /* drop every n-th frame sent (0: none) */
static int s_drop_acks;    /* drop every n-th ACK (0: none) */
static int s_sent, s_acks;
static uint16_t s_contig;  /* frames taken in order */
static int s_taken_idx[4096];
static uint8_t s_mode = RF_PM_ACTIVE, s_mode_during = 0;

int rf_set_power_mode(uint8_t mode) { s_mode = mode; return 0; }
bool is_ota_busy(void) { return false; }
int rf_send_frame_acked(rf_frame_t *frame, int timeout_ms) { (void)frame; (void)timeout_ms; return 0; }

int rf_send_frame(rf_frame_t *f) {
    s_sent++;
    if (s_mode == RF_PM_TRANSFER) s_mode_during = 1;
    if (!(f->flags & RF_FLAG_STREAM)) return 0;
    if (s_drop_every && s_sent % s_drop_every == 0) return 0; /* lost on the air */
    if ((f->seq & 0x0F) == (s_contig & 0x0F)) {
        s_taken_idx[s_contig] = f->payload[0] | (f->payload[1] << 8);
        s_contig++;
    }
    if (f->flags & RF_FLAG_ACK_REQ) {
        s_acks++;
        if (s_drop_acks && s_acks % s_drop_acks == 0) return 0;
        uint8_t p[25] = {0};
        p[2] = (uint8_t)(s_contig & 0xFF);
        p[3] = (uint8_t)(s_contig >> 8);
        p[4] = RF_PUSH_STREAMED;
        push_note_stream_ack(1, p);
    }
    return 0;
}

static void run(const char *name, int frames, int drop_every, int drop_acks) {
    printf("[-] %s\n", name);
    s_drop_every = drop_every;
    s_drop_acks = drop_acks;
    s_sent = s_acks = 0;
    s_contig = 0;
    s_mode_during = 0;
    push_queue_t q = {0};
    for (int i = 0; i < frames; i++) {
        rf_frame_t f;
        uint8_t p[2] = { (uint8_t)(i & 0xFF), (uint8_t)(i >> 8) };
        uint8_t type = (i == frames - 1) ? RF_MSG_CONFIG_END : RF_MSG_DEVICE_CMD_CHUNK;
        rf_build_frame(&f, 1, type, 0, RF_FLAG_ACK_REQ, p, sizeof(p));
        push_queue_add(&q, &f);
    }
    int r = push_queue_send(1, 7, &q);
    CHECK(r == frames - 1, "push succeeds and counts the device commands");
    CHECK(s_contig == frames, "the remote took every frame");
    bool order = true;
    for (int i = 0; i < frames; i++) if (s_taken_idx[i] != i) order = false;
    CHECK(order, "in order, each once");
    CHECK(s_mode_during && s_mode == RF_PM_ACTIVE, "TRANSFER mode during the push, ACTIVE after");
    printf("    %d frames, %d sends, %d ACK requests\n", frames, s_sent, s_acks);
    free(q.frames);
}

int main(void) {
    g_remote_versions[0].caps = RF_CAP_STREAM_PUSH | RF_CAP_DEVICE_CMDS;
    run("no loss", 300, 0, 0);
    run("every 7th frame lost", 300, 7, 0);
    run("every 5th ACK lost", 300, 0, 5);
    run("frames and ACKs lost", 500, 11, 3);
    run("one frame", 1, 0, 0);
    if (s_fail) { printf("[-] test_rf_push_stream: FAILED\n"); return 1; }
    printf("[+] test_rf_push_stream: all checks passed\n");
    return 0;
}
