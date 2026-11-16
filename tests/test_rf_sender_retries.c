/* What hub_rf does when a frame of a transfer gets no ACK, run through the real hub_rf sources
 * (main renamed, /dev/rfspi a socket pair played by a "radio" thread):
 * - OTA_END was sent once, although the remote ACKs it only after the install: one lost frame
 *   reported a failed update, or cost a whole second transfer. It is sent again now (same
 *   sequence number), and a remote whose checkpoint holds every chunk gets only OTA_END
 * - a config push took a missing ACK on the first device command frame for "this remote has no
 *   device browsing" and skipped every command name, yet stamped the new config version. A
 *   remote that reports RF_CAP_DEVICE_CMDS now fails the push instead.
 * The OTA image path in /tmp is redirected to the test's own folder (-Wl,--wrap). */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include "rf_state.h"
#include "rf_spi.h"
#include "rf_ota.h"
#include "rf_push_queue.h"

#ifndef RF_CAP_DEVICE_CMDS
#define RF_CAP_DEVICE_CMDS 0x08 /* builds against a tree without the bit */
#endif

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

static char s_dir[256];
static const char *redir(const char *p, char *buf, size_t n) {
    if (p && strncmp(p, "/tmp/remote_", 12) == 0) {
        snprintf(buf, n, "%s/%s", s_dir, p + 5);
        return buf;
    }
    return p;
}
FILE *__real_fopen(const char *p, const char *m);
FILE *__wrap_fopen(const char *p, const char *m) { char b[512]; return __real_fopen(redir(p, b, sizeof(b)), m); }
int __real_access(const char *p, int m);
int __wrap_access(const char *p, int m) { char b[512]; return __real_access(redir(p, b, sizeof(b)), m); }
int __real_unlink(const char *p);
int __wrap_unlink(const char *p) { char b[512]; return __real_unlink(redir(p, b, sizeof(b))); }

static int s_radio = -1;
static volatile int s_stop;
static volatile int s_ota_end_seen, s_ota_chunks, s_dev_cmd_seen;
static volatile int s_drop_first_end;        /* the first OTA_END is lost */
static volatile int s_resume_all;            /* the OTA_START ACK says: every chunk is here */
static volatile int s_no_dev_cmd_ack;        /* DEVICE_CMD_CHUNK frames are not ACKed */
static volatile uint16_t s_total_chunks;

static void ack(uint8_t slot, uint8_t seq, uint8_t status, const uint8_t *extra4) {
    uint8_t r[32] = {0};
    r[0] = 0x20; r[1] = slot; r[2] = 0x12; r[3] = slot;
    r[4] = (uint8_t)(0x40 | (seq & 0x0F));
    r[5] = RF_MSG_ACK; r[6] = RF_FLAG_IS_ACK;
    r[7] = seq; r[8] = status;
    if (extra4) memcpy(&r[9], extra4, 4); /* payload[2..5] */
    send(s_radio, r, sizeof(r), 0);
}

static void *radio_worker(void *arg) {
    (void)arg;
    uint8_t b[64];
    while (!s_stop) {
        ssize_t n = recv(s_radio, b, sizeof(b), 0);
        if (n <= 0) break;
        if (n < 30 || b[0] != 0x12) continue;
        uint8_t slot = b[1] & 0x0F, seq = b[2] & 0x0F, type = b[3], flags = b[4];
        if (type == RF_MSG_OTA_START) {
            uint8_t ex[4] = {0};
            s_total_chunks = (uint16_t)(b[5 + 4] | (b[5 + 5] << 8));
            if (s_resume_all) { ex[0] = (uint8_t)(s_total_chunks & 0xFF); ex[1] = (uint8_t)(s_total_chunks >> 8); }
            ack(slot, seq, 0, ex);
        } else if (type == RF_MSG_OTA_CHUNK) {
            s_ota_chunks++;
            if (flags & RF_FLAG_ACK_REQ) ack(slot, seq, 0, NULL);
        } else if (type == RF_MSG_OTA_END) {
            if (s_ota_end_seen++ == 0 && s_drop_first_end) continue;
            ack(slot, seq, 0, NULL);
        } else if (type == RF_MSG_DEVICE_CMD_CHUNK) {
            s_dev_cmd_seen++;
            if (!s_no_dev_cmd_ack) ack(slot, seq, 0, NULL);
        } else {
            ack(slot, seq, 0, NULL);
        }
    }
    return NULL;
}

static void stage_image(void) {
    char p[512];
    snprintf(p, sizeof(p), "%s/remote_ui.ota.gz", s_dir);
    FILE *f = __real_fopen(p, "wb");
    for (int i = 0; i < 100; i++) fputc(i, f);
    fclose(f);
}

static void queue_frame(push_queue_t *q, uint8_t type) {
    rf_frame_t f;
    uint8_t pl[4] = {0};
    rf_build_frame(&f, 1, type, 0, RF_FLAG_ACK_REQ, pl, sizeof(pl));
    push_queue_add(q, &f);
}

int main(void) {
    snprintf(s_dir, sizeof(s_dir), "%s", getenv("RF_TEST_TMP") ? getenv("RF_TEST_TMP") : "/tmp/hng_rf_sender");
    mkdir(s_dir, 0755);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0];
    s_radio = sv[1];
    g_paired_remotes[0].slot = 1;
    g_paired_remotes[0].active = true;
    g_paired_remotes[0].model = REMOTE_MODEL_JUNIPER;
    g_has_paired_device = true;
    pthread_t radio, rx;
    pthread_create(&radio, NULL, radio_worker, NULL);
    pthread_create(&rx, NULL, rx_worker, NULL);

    /* 1. The first OTA_END is lost: the update still ends as a success */
    stage_image();
    s_drop_first_end = 1;
    s_ota_end_seen = 0;
    rf_ota_sender_worker((void *)(uintptr_t)1);
    CHECK(g_rf_ota.status_code == 2, "a lost OTA_END ended the update with status %d (%s), OTA_END sent %d time(s)",
          g_rf_ota.status_code, g_rf_ota.error_msg, s_ota_end_seen);
    s_drop_first_end = 0;

    /* 2. The remote already has every chunk: only OTA_END is sent */
    stage_image();
    s_resume_all = 1;
    s_ota_chunks = 0;
    s_ota_end_seen = 0;
    rf_ota_sender_worker((void *)(uintptr_t)1);
    CHECK(s_ota_chunks == 0 && s_ota_end_seen == 1 && g_rf_ota.status_code == 2,
          "with every chunk on the remote: %d chunk frames sent again, OTA_END %d, status %d",
          s_ota_chunks, s_ota_end_seen, g_rf_ota.status_code);
    s_resume_all = 0;

    /* 3. Config push: device command frames without an ACK */
    s_no_dev_cmd_ack = 1;
    push_queue_t q;
    memset(&q, 0, sizeof(q));
    queue_frame(&q, RF_MSG_ACTIVITY_LIST_CHUNK);
    queue_frame(&q, RF_MSG_DEVICE_CMD_CHUNK);
    queue_frame(&q, RF_MSG_DEVICE_CMD_CHUNK);
    queue_frame(&q, RF_MSG_BUTTON_LIST_CHUNK);
    g_remote_versions[0].caps = RF_CAP_ACT_DEVICES | RF_CAP_LONG_NAMES | RF_CAP_UNIT_ID | RF_CAP_DEVICE_CMDS;
    int r = push_queue_send(1, 9, &q);
    CHECK(r < 0, "a remote with device browsing lost a device command frame and the push still succeeded (%d)", r);
    /* control: a remote without the bit keeps the old guess (no device browsing) */
    g_remote_versions[0].caps = RF_CAP_ACT_DEVICES | RF_CAP_LONG_NAMES | RF_CAP_UNIT_ID;
    r = push_queue_send(1, 9, &q);
    CHECK(r == 0, "control: a remote without RF_CAP_DEVICE_CMDS must get the push without commands (%d)", r);
    free(q.frames);

    s_stop = 1;
    g_running = false;
    shutdown(sv[0], SHUT_RDWR);
    shutdown(sv[1], SHUT_RDWR);
    char p[512];
    snprintf(p, sizeof(p), "%s/remote_ui.ota.gz", s_dir);
    __real_unlink(p);

    if (s_failures) {
        printf("[-] test_rf_sender_retries: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] test_rf_sender_retries: OTA_END is sent again, a missing device command ACK fails the push\n");
    return 0;
}
