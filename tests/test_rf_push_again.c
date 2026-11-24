/* Config pushes asked for while one runs. Real hub_rf sources (main renamed), /dev/rfspi a
 * socket pair, the push starter set as hub_rf's main() sets it.
 * - B: a remote paired into a chosen slot (4) lands on pipe 2. Its version report, unit id and
 *   full sync request come before the Hub's 4A and the pipe-table replies: the push it asks for
 *   starts under slot 2, the reply then moves pipe 2 to slot 4 and asks again while that push
 *   runs. That request was dropped ("already in progress"): no CONFIG_END reached pipe 2.
 * - D: the 4A first, so the first push already goes to slot 4: still exactly one push.
 * - A push skipped while a remote update is prepared (status 3) is pushed once the update ended
 *   without installing; after an installed update it is left to the remote's own request.
 * BASE_CONTROL builds it against a tree without the fix (stubs for the new calls). */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "rf_state.h"
#include "rf_spi.h"
#include "rf_pairing.h"
#include "rf_config_push.h"

#ifdef BASE_CONTROL
int rf_config_push_retry_skipped(void) { return 0; }
static bool rf_config_push_active(void) { return g_config_push_running; }
#endif

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } \
                              else { printf("  [PASS] " __VA_ARGS__); printf("\n"); } } while (0)

static int s_radio = -1;
static int s_cfg_end[8]; /* CONFIG_END frames the Hub wrote, by pipe */

static void drain(void) {
    uint8_t buf[64];
    ssize_t n;
    while ((n = recv(s_radio, buf, sizeof(buf), MSG_DONTWAIT)) > 0)
        if (n == 30 && buf[0] == 0x12 && buf[3] == RF_MSG_CONFIG_END && buf[1] < 8) s_cfg_end[buf[1]]++;
}

static void wait_drain(int ms) {
    for (int i = 0; i < ms / 100; i++) { usleep(100000); drain(); }
}

/* At least 2 s, then until no push has run for 2 s (40 s at most): a slow machine must not cut
 * a push (or the second one) short */
static void wait_pushes_done(void) {
    int idle_ms = 0;
    wait_drain(2000);
    for (int ms = 0; ms < 40000 && idle_ms < 2000; ms += 100) {
        usleep(100000);
        drain();
        idle_ms = rf_config_push_active() ? 0 : idle_ms + 100;
    }
}

static void put(const uint8_t *pkt, size_t len) {
    if (write(s_radio, pkt, len) != (ssize_t)len) printf("  radio write failed\n");
}

/* a frame from the remote on pipe, as the radio hands it to the Hub */
static void remote_frame(int pipe, uint8_t seq, uint8_t type, const uint8_t *payload, size_t plen) {
    uint8_t d[32] = {0x20, (uint8_t)pipe, 0x12, 0x00, seq, type, 0x00};
    if (payload) memcpy(&d[7], payload, plen > 25 ? 25 : plen);
    put(d, sizeof(d));
}

static void table_reply(int pipe) {
    uint8_t r[20] = {0x11, 0xFF, 0x83, 0xB5, (uint8_t)(0x20 + pipe - 1), 0x00, 0x00, 0x40, 0x21};
    put(r, sizeof(r));
}

/* a pairing into slot 4 that the radio puts on pipe 2; 4A first (D) or after the frames (B) */
static void pairing(int hub_4a_first) {
    static const uint8_t ok[7] = {0x10, 0xFF, 0x4A, 0x00, 0x00, 0x00, 0x00};
    uint8_t ver[5] = {2, 5, 3, 1, 0x7F};
    uint8_t uid[25] = {21};
    memcpy(&uid[1], "20200101000000-000001", 21);
    uid[24] = 111;
    uint8_t sync_full[1] = {0x02};
    start_pairing_mode(4);
    usleep(100000);
    drain();
    memset(s_cfg_end, 0, sizeof(s_cfg_end));
    if (hub_4a_first) put(ok, sizeof(ok));
    remote_frame(2, 0x41, RF_MSG_VERSION_REPORT, ver, sizeof(ver));
    remote_frame(2, 0x42, RF_MSG_UNIT_ID, uid, sizeof(uid));
    remote_frame(2, 0x43, RF_MSG_SYNC_REQUEST, sync_full, sizeof(sync_full));
    if (!hub_4a_first) put(ok, sizeof(ok));
    table_reply(1);
    table_reply(2);
    wait_pushes_done();
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : "B"; /* one scenario per run: B, D or U */
    setvbuf(stdout, NULL, _IOLBF, 0);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0];
    s_radio = sv[1];
    char dir[96], path[128];
    snprintf(dir, sizeof(dir), "/tmp/test_rf_push_again_%d", (int)getpid());
    mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/rf_slot_map.txt", dir);
    g_rf_slot_map_path = path;
    g_rf_version_dir = dir;
    g_rf_battery_cache_dir = dir;
    rf_slot_map_load();
    g_rf_push_starter = rf_request_config_push; /* as hub_rf's main() */

    pthread_t rx;
    pthread_create(&rx, NULL, rx_worker, NULL);
    table_reply(1); /* the remote of slot 1, seen at the start of hub_rf */
    uint8_t ver1[5] = {2, 5, 3, 1, 0x7F};
    remote_frame(1, 0x44, RF_MSG_VERSION_REPORT, ver1, sizeof(ver1)); /* and heard */
    wait_pushes_done(); /* a push the version report may start */

    if (strchr(only, 'B')) {
        printf("[-] Config push: a push asked for while a slot moved runs again (order B)\n");
        pairing(0);
        CHECK(rf_pipe_to_slot(2) == 4, "B: the new remote (pipe 2) is in slot 4 (got %d)", rf_pipe_to_slot(2));
        CHECK(s_cfg_end[2] > 0, "B: the new remote (pipe 2) got a config push (CONFIG_END frames: %d)", s_cfg_end[2]);
    }
    if (strchr(only, 'D')) {
        printf("[-] Config push: order D (4A first) pushes once\n");
        pairing(1);
        CHECK(rf_pipe_to_slot(2) == 4, "D: the new remote (pipe 2) is in slot 4 (got %d)", rf_pipe_to_slot(2));
        CHECK(s_cfg_end[2] > 0, "D: the new remote got a config push (CONFIG_END frames: %d)", s_cfg_end[2]);
        int during = s_cfg_end[2];
        /* one push on its own, for the number of CONFIG_END frames (with resends) a push sends */
        memset(s_cfg_end, 0, sizeof(s_cfg_end));
        rf_request_config_push();
        wait_pushes_done();
        CHECK(s_cfg_end[2] > 0 && during == s_cfg_end[2], "D: pushed once, not twice (CONFIG_END to pipe 2: %d, one push: %d)",
              during, s_cfg_end[2]);
    }
    if (strchr(only, 'U')) {
        printf("[-] Config push skipped during a remote update: pushed when the update ends\n");
        wait_drain(500);
        g_rf_ota.status_code = 3; /* preparing (download) */
        config_push_worker(NULL);
        wait_drain(300);
        memset(s_cfg_end, 0, sizeof(s_cfg_end));
        CHECK(rf_config_push_retry_skipped() == 0, "no push while the update is prepared");
        g_rf_ota.status_code = 1;
        CHECK(rf_config_push_retry_skipped() == 0, "no push while the update is sent");
        g_rf_ota.status_code = -1; /* the update failed: the remote keeps its software */
        int started = rf_config_push_retry_skipped();
        wait_pushes_done();
        CHECK(started == 1 && s_cfg_end[1] > 0, "the skipped push runs once the update failed (CONFIG_END to slot 1: %d)",
              s_cfg_end[1]);
        CHECK(rf_config_push_retry_skipped() == 0, "and only once");
        g_rf_ota.status_code = 3;
        config_push_worker(NULL);
        g_rf_ota.status_code = 2; /* installed: the remote restarts and asks itself */
        memset(s_cfg_end, 0, sizeof(s_cfg_end));
        CHECK(rf_config_push_retry_skipped() == 0, "after an installed update the remote's own request pushes");
        wait_drain(1000);
        CHECK(s_cfg_end[1] == 0, "nothing pushed then (CONFIG_END to slot 1: %d)", s_cfg_end[1]);
        g_rf_ota.status_code = 0;
    }

    g_running = false;
    uint8_t link[7] = {0x10, 0x02, 0x41, 0x04, 0x00, 0x00, 0x00};
    put(link, sizeof(link));
    pthread_join(rx, NULL);
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) printf("  could not remove %s\n", dir);
    printf("%s\n", s_failures ? "[-] test_rf_push_again FAILED" : "[+] test_rf_push_again passed");
    return s_failures ? 1 : 0;
}
