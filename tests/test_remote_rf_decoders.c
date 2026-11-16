/* The remote's RF decoders and OTA receiver, run through the real remote sources with the Hub's
 * frames handed to handle_hub_frame_remote() and the remote's answers read from a socket pair
 * that stands for /dev/rfspi:
 * - a Hub config version lower than the remote's (the 8-bit number wraps 255 -> 1) was taken as
 *   "nothing to do", so a remote that missed the push kept the old config
 * - step and button-trigger chunks were appended, icon chunks appended to the file: a chunk sent
 *   again after a lost ACK was stored twice (the icon then failed its CRC check)
 * - a stop-and-wait OTA chunk was written at any index, past the announced image
 * - OTA_END without the digest marker installed the image after a 4-byte ELF check only
 * - a full /tmp was noticed only at the digest check after the whole transfer
 * - a checkpoint holding every chunk was not offered for resume (the whole image again)
 * - the screen timeout from the Hub was taken as is: 0 kept the screen on until the battery died
 * - the version report now announces device browsing (RF_CAP_DEVICE_CMDS)
 * /tmp paths are redirected to the test's own folder and system() is replaced (-Wl,--wrap), so
 * no install command runs on the host. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "remote/remote_state.h"
#include "remote/remote_rf_sync.h"
#include "remote/remote_rf.h"
#include "rf_proto.h"
#include "sha256.h"

#ifndef RF_CAP_DEVICE_CMDS
#define RF_CAP_DEVICE_CMDS 0x08 /* builds against a tree without the bit */
#endif

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

static char s_dir[256];
static const char *redir(const char *p, char *b, size_t n) {
    if (p && (strncmp(p, "/tmp/remote_", 12) == 0 || strncmp(p, "/tmp/custom_icon", 16) == 0)) {
        snprintf(b, n, "%s/%s", s_dir, p + 5);
        return b;
    }
    return p;
}
FILE *__real_fopen(const char *p, const char *m);
FILE *__wrap_fopen(const char *p, const char *m) { char b[512]; return __real_fopen(redir(p, b, sizeof(b)), m); }
int __real_open(const char *p, int flags, ...);
int __wrap_open(const char *p, int flags, ...) {
    char b[512];
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    return __real_open(redir(p, b, sizeof(b)), flags, mode);
}
int __real_unlink(const char *p);
int __wrap_unlink(const char *p) { char b[512]; return __real_unlink(redir(p, b, sizeof(b))); }
int __real_stat(const char *p, struct stat *st);
int __wrap_stat(const char *p, struct stat *st) { char b[512]; return __real_stat(redir(p, b, sizeof(b)), st); }
static int s_tmp_full;
int __real_statvfs(const char *p, struct statvfs *v);
int __wrap_statvfs(const char *p, struct statvfs *v) {
    int r = __real_statvfs(p, v);
    if (r == 0 && s_tmp_full && strcmp(p, "/tmp") == 0) v->f_bavail = 1;
    return r;
}
static int s_system_calls;
int __wrap_system(const char *cmd) { (void)cmd; s_system_calls++; return -1; }

static int s_hub = -1;

/* The status of the last ACK the remote sent (-1 none); also its payload[2..3] */
static int last_ack(uint16_t *p23) {
    uint8_t r[64];
    int st = -1;
    ssize_t n;
    while ((n = recv(s_hub, r, sizeof(r), MSG_DONTWAIT)) > 0) {
        if (n >= 32 && r[0] == 0x20 && r[5] == RF_MSG_ACK) {
            st = r[8];
            if (p23) *p23 = (uint16_t)(r[9] | (r[10] << 8));
        }
    }
    return st;
}

/* Frame types the remote sent since the last call */
static int sent_type(uint8_t type, uint8_t *p4) {
    uint8_t r[64];
    int found = 0;
    ssize_t n;
    while ((n = recv(s_hub, r, sizeof(r), MSG_DONTWAIT)) > 0) {
        if (n >= 32 && r[0] == 0x20 && r[5] == type) { found = 1; if (p4) *p4 = r[11]; }
    }
    return found;
}

static void hub(uint8_t type, uint8_t flags, const uint8_t *pl, size_t len) {
    static uint8_t seq;
    rf_frame_t f;
    rf_build_frame(&f, 1, type, ++seq, flags, pl, len);
    handle_hub_frame_remote(&f);
}

int main(void) {
    snprintf(s_dir, sizeof(s_dir), "%s", getenv("RF_TEST_TMP") ? getenv("RF_TEST_TMP") : "/tmp/hng_remote_rf");
    mkdir(s_dir, 0755);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0];
    s_hub = sv[1];
    g_remote_slot = 1;
    g_rf_link_connected = true;
    g_pairing_state = 2;
    g_has_hub_config = true;
    g_running = true;

    /* 1. The Hub's config version wrapped to 1 while the remote has 255 */
    g_config_version = 255;
    uint8_t ts[25] = {0};
    ts[19] = 0x01;
    ts[20] = 1;
    hub(RF_MSG_TIME_SYNC, 0, ts, sizeof(ts));
    CHECK(g_update_config_available && sent_type(RF_MSG_SYNC_REQUEST, NULL),
          "Hub config version 1 vs remote 255: update flag %d, no sync request", g_update_config_available);
    g_config_version = 1;
    hub(RF_MSG_TIME_SYNC, 0, ts, sizeof(ts));
    CHECK(!g_update_config_available, "control: the same version is not an update");
    last_ack(NULL);

    /* 2. Chunks sent again after a lost ACK */
    uint8_t step[25] = {0};
    for (int i = 0; i < 3; i++) {
        step[0] = (uint8_t)(i == 2 ? 1 : i); /* 0, 1, then 1 again */
        step[1] = (uint8_t)(i == 2 ? 1 : i);
        memcpy(&step[3], "5", 1);
        memcpy(&step[14], "PowerOn", 7);
        hub(RF_MSG_STEP_LIST_CHUNK, RF_FLAG_ACK_REQ, step, sizeof(step));
    }
    CHECK(g_rf_step_count == 2, "steps 0, 1, 1 (resent) stored as %d steps", g_rf_step_count);

    uint8_t btn[25] = {0};
    btn[0] = 0;
    btn[2] = 0x00;
    memcpy(&btn[3], "5", 1);
    memcpy(&btn[12], "Watch", 5);
    btn[24] = 0x02;
    int before = g_act_trigger_count;
    hub(RF_MSG_BUTTON_LIST_CHUNK, RF_FLAG_ACK_REQ, btn, sizeof(btn));
    hub(RF_MSG_BUTTON_LIST_CHUNK, RF_FLAG_ACK_REQ, btn, sizeof(btn));
    CHECK(g_act_trigger_count == before + 1, "a trigger chunk sent twice added %d triggers", g_act_trigger_count - before);

    char cdir[512];
    snprintf(cdir, sizeof(cdir), "%s", getenv("HARMONY_MOCK_DIR") ? getenv("HARMONY_MOCK_DIR") : "/tmp/harmony_mock");
    mkdir(cdir, 0755);
    strncat(cdir, "/data", sizeof(cdir) - strlen(cdir) - 1);
    mkdir(cdir, 0755);
    strncat(cdir, "/assets", sizeof(cdir) - strlen(cdir) - 1);
    mkdir(cdir, 0755);
    strncat(cdir, "/custom", sizeof(cdir) - strlen(cdir) - 1);
    mkdir(cdir, 0755);
    uint8_t icon[30];
    for (int i = 0; i < 30; i++) icon[i] = (uint8_t)(0x40 + i);
    uint8_t is[25] = {0};
    is[1] = 30; is[3] = 2;
    memcpy(&is[5], "ic9", 3);
    hub(RF_MSG_ICON_START, RF_FLAG_ACK_REQ, is, sizeof(is));
    uint8_t ic[25] = {0};
    ic[0] = 0; ic[2] = 22; memcpy(&ic[3], icon, 22);
    hub(RF_MSG_ICON_CHUNK, RF_FLAG_ACK_REQ, ic, sizeof(ic));
    hub(RF_MSG_ICON_CHUNK, RF_FLAG_ACK_REQ, ic, sizeof(ic)); /* resent after a lost ACK */
    memset(ic, 0, sizeof(ic));
    ic[0] = 1; ic[2] = 8; memcpy(&ic[3], icon + 22, 8);
    hub(RF_MSG_ICON_CHUNK, RF_FLAG_ACK_REQ, ic, sizeof(ic));
    last_ack(NULL);
    uint16_t crc = rf_crc16(icon, sizeof(icon));
    uint8_t ie[25] = {(uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8)};
    hub(RF_MSG_ICON_END, RF_FLAG_ACK_REQ, ie, sizeof(ie));
    int st = last_ack(NULL);
    CHECK(st == 0, "an icon with a chunk sent twice failed its check (ACK status %d)", st);

    /* 3. OTA: 100 bytes, stop-and-wait chunks */
    uint8_t img[100];
    for (int i = 0; i < 100; i++) img[i] = (uint8_t)(i * 7);
    uint8_t digest[32];
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, img, sizeof(img));
    sha256_final(digest, &c);
    uint32_t crc32 = rf_crc32(img, sizeof(img));
    uint8_t os[25] = {100, 0, 0, 0, 5, 0, (uint8_t)crc32, (uint8_t)(crc32 >> 8), (uint8_t)(crc32 >> 16), (uint8_t)(crc32 >> 24)};
    g_battery_info.percent = 90;

    s_tmp_full = 1;
    hub(RF_MSG_OTA_START, RF_FLAG_ACK_REQ, os, sizeof(os));
    st = last_ack(NULL);
    CHECK(st == 0x03, "OTA_START with /tmp full answered status %d", st);
    s_tmp_full = 0;

    hub(RF_MSG_OTA_START, RF_FLAG_ACK_REQ, os, sizeof(os));
    last_ack(NULL);
    for (int i = 0; i < 5; i++) {
        uint8_t ch[25] = {0};
        ch[0] = (uint8_t)i;
        ch[2] = (uint8_t)(i == 4 ? 12 : 22);
        memcpy(&ch[3], img + i * 22, ch[2]);
        hub(RF_MSG_OTA_CHUNK, i == 4 ? RF_FLAG_ACK_REQ : 0, ch, sizeof(ch));
    }
    uint8_t far[25] = {0xE7, 0x03, 22}; /* chunk 999 */
    hub(RF_MSG_OTA_CHUNK, 0, far, sizeof(far));
    struct stat ost;
    char opath[512];
    snprintf(opath, sizeof(opath), "%s/remote_ui.ota.gz", s_dir);
    fflush(NULL);
    __real_stat(opath, &ost);
    CHECK(ost.st_size <= 100, "a chunk outside the image made the file %ld bytes long", (long)ost.st_size);
    last_ack(NULL);

    /* the Hub's OTA_END was lost; it starts again: every chunk is here, only OTA_END is needed */
    hub(RF_MSG_OTA_START, RF_FLAG_ACK_REQ, os, sizeof(os));
    uint16_t resume = 0;
    st = last_ack(&resume);
    CHECK(st == 0 && resume == 5, "a complete checkpoint was offered as resume chunk %u (status %d)", resume, st);

    /* OTA_END without the digest marker is refused, nothing is unpacked */
    uint8_t oe[25] = {0};
    s_system_calls = 0;
    hub(RF_MSG_OTA_END, RF_FLAG_ACK_REQ, oe, sizeof(oe));
    st = last_ack(NULL);
    CHECK(st == 0x06 && s_system_calls <= 1, "OTA_END without a digest: status %d, %d shell commands", st, s_system_calls);

    /* 4. Screen timeout 0 from the Hub */
    uint8_t sp[25] = {0};
    sp[0] = 2; sp[1] = 1; sp[3] = 0; sp[4] = 0;
    sp[17] = 1; /* no-screen mode: no drawing in the test */
    g_fb_mem = NULL;
    hub(RF_MSG_SETTINGS_PUSH, RF_FLAG_ACK_REQ, sp, sizeof(sp));
    CHECK(g_timeout_seconds >= 5, "screen timeout 0 from the Hub stored as %d s", g_timeout_seconds);

    /* 5. The version report announces device browsing */
    last_ack(NULL);
    send_version_report_rf();
    uint8_t caps = 0;
    CHECK(sent_type(RF_MSG_VERSION_REPORT, &caps) && (caps & RF_CAP_DEVICE_CMDS), "version report caps 0x%02X", caps);

    g_running = false;
    if (s_failures) {
        printf("[-] test_remote_rf_decoders: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] test_remote_rf_decoders: resent chunks stored once, OTA receiver checks its input\n");
    return 0;
}
