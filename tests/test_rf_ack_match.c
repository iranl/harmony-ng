/* hub_rf waits for a remote's ACK in rf_send_frame_acked(). Several threads send ACKed frames
 * (config push, icon push, settings, sleep, wake, Help, OTA), and the wait used one pending
 * sequence number and one flag for all of them, matched on 4 bits of the sequence number only:
 * a second sender took the first one's ACK, so a wake to a sleeping remote was reported as
 * acknowledged, and a frame the remote never ACKed as delivered. Runs the real hub_rf sources
 * (main renamed) with /dev/rfspi replaced by a socket pair: rx_worker() reads the "radio" end,
 * where a thread plays two remotes, slot 1 awake (ACKs after 20 ms) and slot 2 asleep.
 * Also checks the link counters: a resend is a retry of the thread that sends it, whatever
 * other threads send in between. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include "rf_state.h"
#include "rf_spi.h"

static int s_radio_fd = -1;
static volatile int s_stop;
static volatile int s_lossy;          /* slot 1 does not ACK frames whose payload[0] % 5 == 0 */
static volatile int s_wrong_slot_ack; /* slot 2 answers each frame to slot 1 with its own ACK */
static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

/* The ACK as the Hub's CC2544 reports it: 20 <slot> 12 <slot> <0x40|seq> 1E 02 <seq> <status> */
static void send_ack(uint8_t slot, uint8_t seq, uint8_t status) {
    uint8_t r[32] = {0};
    r[0] = 0x20; r[1] = slot; r[2] = 0x12; r[3] = slot;
    r[4] = (uint8_t)(0x40 | (seq & 0x0F));
    r[5] = RF_MSG_ACK; r[6] = RF_FLAG_IS_ACK;
    r[7] = seq; r[8] = status;
    send(s_radio_fd, r, sizeof(r), 0);
}

static void *radio_worker(void *arg) {
    (void)arg;
    uint8_t buf[64];
    while (!s_stop) {
        ssize_t n = recv(s_radio_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (n < 30 || buf[0] != 0x12) continue;
        uint8_t slot = buf[1] & 0x0F, seq = buf[2] & 0x0F;
        if (slot != 1) continue; /* slot 2 sleeps: it never answers */
        if (s_lossy && buf[5] % 5 == 0) continue;
        usleep(20000);
        if (s_wrong_slot_ack) send_ack(2, seq, 0);
        else send_ack(1, seq, 0);
    }
    return NULL;
}

typedef struct { int frames, delivered, failed, lost_reported_ok; volatile int done; } stream_t;

static void *stream_worker(void *arg) {
    stream_t *st = arg;
    for (int i = 0; i < st->frames; i++) {
        rf_frame_t f;
        uint8_t pl[4] = {(uint8_t)i, 0, 0, 0};
        rf_build_frame(&f, 1, RF_MSG_DEVICE_CMD_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, pl, sizeof(pl));
        int r = rf_send_frame_acked(&f, 150);
        if (r == 0) st->delivered++;
        else st->failed++;
        if (r == 0 && s_lossy && i % 5 == 0) st->lost_reported_ok++;
    }
    st->done = 1;
    return NULL;
}

/* Sends one frame to the sleeping remote in slot 2 twice, as a sender resends a frame that got
 * no ACK */
static volatile int s_resend_done;
static void *resend_worker(void *arg) {
    (void)arg;
    rf_frame_t f;
    uint8_t pl[1] = {7};
    rf_build_frame(&f, 2, RF_MSG_SETTINGS_PUSH, ++g_tx_seq, RF_FLAG_ACK_REQ, pl, sizeof(pl));
    rf_send_frame_acked(&f, 200);
    rf_send_frame_acked(&f, 200);
    s_resend_done = 1;
    return NULL;
}

static volatile int s_other_done;
static void *other_worker(void *arg) {
    (void)arg;
    rf_frame_t f;
    uint8_t pl[1] = {8};
    rf_build_frame(&f, 2, RF_MSG_SETTINGS_PUSH, ++g_tx_seq, RF_FLAG_ACK_REQ, pl, sizeof(pl));
    rf_send_frame_acked(&f, 200);
    s_other_done = 1;
    return NULL;
}

static volatile int s_wake_done;
static void *wake_worker(void *arg) {
    int *woke = arg;
    *woke = rf_wake_remote(2, 6);
    s_wake_done = 1;
    return NULL;
}

/* Waits in sleeps, not in pthread_join(): under tests/no_futex_exec (a kernel without futex, as
 * on the Hub) a join spins, and the test checks that hub_rf itself does not */
static void wait_flag(volatile int *flag) {
    while (!*flag) usleep(5000);
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) { perror("socketpair"); return 1; }
    g_rf_fd = sv[0];
    s_radio_fd = sv[1];
    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = true;
    }
    pthread_t radio, rx;
    pthread_create(&radio, NULL, radio_worker, NULL);
    pthread_create(&rx, NULL, rx_worker, NULL);

    /* 1. A frame stream to the awake remote and a wake to the sleeping one at the same time */
    stream_t a = { .frames = 40 };
    int woke = -1;
    pthread_t ta, tb;
    pthread_create(&ta, NULL, stream_worker, &a);
    usleep(30000);
    pthread_create(&tb, NULL, wake_worker, &woke);
    wait_flag(&a.done);
    wait_flag(&s_wake_done);
    CHECK(woke == 0, "a wake to the sleeping remote in slot 2 was reported as acknowledged (attempt %d)", woke);
    CHECK(a.delivered == a.frames, "%d of %d frames to the awake remote were reported as delivered", a.delivered, a.frames);

    /* 2. Frames the remote never ACKs, while another stream gets its ACKs */
    s_lossy = 1;
    stream_t b = { .frames = 40 }, c = { .frames = 40 };
    pthread_create(&ta, NULL, stream_worker, &b);
    pthread_create(&tb, NULL, stream_worker, &c);
    wait_flag(&b.done);
    wait_flag(&c.done);
    CHECK(b.lost_reported_ok == 0 && c.lost_reported_ok == 0,
          "frames the remote never ACKed were reported as delivered (%d and %d)", b.lost_reported_ok, c.lost_reported_ok);
    CHECK(b.failed == 8 && c.failed == 8, "8 + 8 frames were never ACKed, reported failed: %d and %d", b.failed, c.failed);
    s_lossy = 0;

    /* 3. An ACK from slot 2 with the sequence number of a frame waiting on slot 1 */
    s_wrong_slot_ack = 1;
    rf_frame_t f;
    uint8_t pl[1] = {1};
    rf_build_frame(&f, 1, RF_MSG_SETTINGS_PUSH, ++g_tx_seq, RF_FLAG_ACK_REQ, pl, sizeof(pl));
    CHECK(rf_send_frame_acked(&f, 200) != 0, "an ACK from slot 2 completed a frame sent to slot 1");
    s_wrong_slot_ack = 0;

    /* 4. Control: the right ACK still completes the wait, and its status reaches this thread */
    rf_build_frame(&f, 1, RF_MSG_SETTINGS_PUSH, ++g_tx_seq, RF_FLAG_ACK_REQ, pl, sizeof(pl));
    CHECK(rf_send_frame_acked(&f, 500) == 0 && g_ack_status == 0, "the ACK of the remote in slot 1 was not taken");

    /* 5. Link counters: one thread resends its frame to slot 2 while another thread's frame
     * to slot 2 waits for its turn. The resend is a retry; the other frame is not. With one
     * shared record of the last frame, the waiting thread overwrote it and the retry was missed. */
    uint32_t sent0 = g_remote_link[1].acked_sent, retries0 = g_remote_link[1].retries;
    pthread_create(&ta, NULL, resend_worker, NULL);
    usleep(50000);
    pthread_create(&tb, NULL, other_worker, NULL);
    wait_flag(&s_resend_done);
    wait_flag(&s_other_done);
    CHECK(g_remote_link[1].acked_sent - sent0 == 3, "3 ACKed sends to slot 2, counted %u",
          g_remote_link[1].acked_sent - sent0);
    CHECK(g_remote_link[1].retries - retries0 == 1, "1 resend to slot 2, counted %u retries",
          g_remote_link[1].retries - retries0);

    s_stop = 1;
    g_running = false;
    shutdown(sv[1], SHUT_RDWR);
    shutdown(sv[0], SHUT_RDWR);

    if (s_failures) {
        printf("[-] test_rf_ack_match: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] test_rf_ack_match: each ACKed send gets its own ACK, from its own slot, and counts its own resends\n");
    return 0;
}
