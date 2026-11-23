/* A remote paired into a slot the user picked (POST /api/rf/pair?slot=N), while the CC2544 put
 * it on another pipe. Runs the real hub_rf sources (main renamed, /dev/rfspi a socket pair):
 * - frames and pipe-table replies from the pipe arrive as the chosen slot, and sends to the slot
 *   go out on the pipe; the unpair command names the pipe
 * - the slot table is saved and read back; a broken file means slots and pipes are the same
 * - a pipe of a slot that holds a remote, and a pipe seen with no pairing pending, stay put
 * - only the pipe table decides (a frame before the 4A moves nothing); a cancel drops the
 *   choice; a second request in the same window replaces it */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include "rf_state.h"
#include "rf_spi.h"
#include "rf_pairing.h"

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

static int s_radio = -1; /* the CC2544 side of the socket pair */

static void radio_says(const uint8_t *pkt, size_t len) {
    if (write(s_radio, pkt, len) != (ssize_t)len) printf("  [FAIL] radio write\n");
    /* rx_worker handles it; a pairing success first reads the whole pipe table (about 0.6 s) */
    usleep(len == 7 && pkt[2] == 0x4A ? 1200000 : 150000);
}

/* The next Hub write whose first bytes are prefix[0..n-1]; earlier ones are dropped */
static int hub_wrote(const uint8_t *prefix, size_t n, uint8_t *out) {
    for (;;) {
        ssize_t r = recv(s_radio, out, 64, MSG_DONTWAIT);
        if (r <= 0) return 0;
        if ((size_t)r >= n && memcmp(out, prefix, n) == 0) return (int)r;
    }
}

static void read_file(const char *path, char *buf, size_t len) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(buf, 1, len - 1, f);
    buf[n] = '\0';
    fclose(f);
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0];
    s_radio = sv[1];
    char path[64];
    snprintf(path, sizeof(path), "/tmp/test_rf_slot_map_%d.txt", (int)getpid());
    unlink(path);
    g_rf_slot_map_path = path;

    printf("[-] RF slot table: pairing into a chosen slot\n");
    rf_slot_map_load(); /* no file: the same numbers */
    CHECK(rf_slot_to_pipe(4) == 4 && rf_pipe_to_slot(2) == 2, "no file: slots and pipes the same");

    pthread_t rx;
    pthread_create(&rx, NULL, rx_worker, NULL);

    /* A remote in slot 1 already: the pipe table at start shows it */
    g_paired_remotes[0].active = true;
    uint8_t equad1[20] = {0x11, 0xFF, 0x83, 0xB5, 0x20, 0x00, 0x00, 0x40, 0x5A};
    radio_says(equad1, sizeof(equad1));

    /* No pairing pending: a frame from a pipe without a remote keeps its slot */
    uint8_t link3[7] = {0x10, 0x03, 0x41, 0x04, 0x00, 0x00, 0x00};
    radio_says(link3, sizeof(link3));
    CHECK(rf_slot_to_pipe(3) == 3, "pipe 3 stays slot 3 without a pending pairing");

    /* The user pairs into slot 4; the radio pairs the remote on pipe 2. Its first frame comes
     * before the 4A: the frame alone moves nothing, the pipe-table reply does */
    rf_slot_map_set_pair_target(4, true);
    uint8_t link2[7] = {0x10, 0x02, 0x41, 0x04, 0x00, 0x00, 0x00};
    radio_says(link2, sizeof(link2));
    CHECK(rf_slot_to_pipe(2) == 2, "a frame before the table reply moves nothing");
    uint8_t ok[7] = {0x10, 0xFF, 0x4A, 0x00, 0x00, 0x00, 0x00};
    radio_says(ok, sizeof(ok));
    uint8_t equad2[20] = {0x11, 0xFF, 0x83, 0xB5, 0x21, 0x00, 0x00, 0x40, 0x5A};
    radio_says(equad2, sizeof(equad2));
    CHECK(rf_slot_to_pipe(4) == 2 && rf_pipe_to_slot(2) == 4, "pipe 2 went to slot 4 (pipe of 4: %d)", rf_slot_to_pipe(4));
    CHECK(rf_slot_to_pipe(2) == 4 && rf_pipe_to_slot(4) == 2, "slot 2 took the free pipe 4");
    CHECK(g_paired_remotes[3].equad_id == 0x405A && g_paired_remotes[1].equad_id == 0, "EquadID of pipe 2 is slot 4's");
    CHECK(!g_paired_remotes[1].active, "slot 2, marked by the early frame, holds no remote");
    char text[64];
    read_file(path, text, sizeof(text));
    CHECK(strcmp(text, "1 4 3 2 5 6\n") == 0, "slot table saved (got '%s')", text);

    /* A data frame from pipe 2 is the remote of slot 4 */
    uint8_t data2[30] = {0x12, 0x02, 0x41, RF_MSG_ACK};
    radio_says(data2, sizeof(data2));
    CHECK(g_paired_remotes[3].active && !g_paired_remotes[1].active, "frame from pipe 2 marks slot 4, not 2");

    /* Sends to slot 4 go out on pipe 2 */
    uint8_t out[64];
    while (recv(s_radio, out, sizeof(out), MSG_DONTWAIT) > 0) {}
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.report_id = 0x20; f.sub_id = 0x12; f.dev_slot = 4; f.msg_type = RF_MSG_ACK;
    rf_send_frame(&f);
    uint8_t to_hub2[2] = {0x12, 0x02};
    CHECK(hub_wrote(to_hub2, 2, out) == 30, "frame to slot 4 sent on pipe 2");

    /* A second pairing into slot 4, now taken, moves nothing */
    rf_slot_map_set_pair_target(4, true);
    radio_says(ok, sizeof(ok));
    uint8_t equad5[20] = {0x11, 0xFF, 0x83, 0xB5, 0x24, 0x00, 0x00, 0x40, 0x5A};
    radio_says(equad5, sizeof(equad5));
    CHECK(rf_slot_to_pipe(5) == 5 && rf_slot_to_pipe(4) == 2, "a taken slot does not move");

    /* A cancel (or a timeout) drops the choice: a lock closing with no pairing places nothing */
    rf_slot_map_set_pair_target(3, true);
    stop_pairing_mode();
    uint8_t equad6[20] = {0x11, 0xFF, 0x83, 0xB5, 0x25, 0x00, 0x00, 0x40, 0x5A};
    radio_says(equad6, sizeof(equad6));
    CHECK(rf_slot_to_pipe(6) == 6 && rf_slot_to_pipe(3) == 3, "a cancelled choice moves nothing");

    /* A second request in the same window: its slot wins (slot 3, not 2) */
    rf_slot_map_set_pair_target(2, true);
    rf_slot_map_set_pair_target(3, false);
    uint8_t equad4[20] = {0x11, 0xFF, 0x83, 0xB5, 0x23, 0x00, 0x00, 0x40, 0x5A};
    radio_says(equad4, sizeof(equad4));
    CHECK(rf_slot_to_pipe(3) == 4 && rf_slot_to_pipe(2) == 3, "the second request's slot is used (pipe of 3: %d)",
          rf_slot_to_pipe(3));

    /* Unpairing slot 4 clears pipe 2 */
    while (recv(s_radio, out, sizeof(out), MSG_DONTWAIT) > 0) {}
    unpair_device(4);
    uint8_t unpair2[6] = {0x10, 0xFF, 0x80, 0xB2, 0x03, 0x02};
    CHECK(hub_wrote(unpair2, 6, out) == 7, "unpair of slot 4 names pipe 2");

    /* Read back at start; a broken table is ignored */
    rf_slot_map_load();
    CHECK(rf_slot_to_pipe(4) == 2 && rf_slot_to_pipe(3) == 4 && rf_slot_to_pipe(2) == 3, "slot table read back");
    FILE *bad = fopen(path, "w");
    if (bad) { fputs("1 1 3 4 5 6\n", bad); fclose(bad); }
    rf_slot_map_load();
    CHECK(rf_slot_to_pipe(4) == 4 && rf_slot_to_pipe(2) == 2, "a broken table means the same numbers");

    g_running = false;
    radio_says(link3, sizeof(link3));
    pthread_join(rx, NULL);
    unlink(path);
    if (s_failures) {
        printf("[-] test_rf_slot_map: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] RF slot table: all checks passed\n");
    return 0;
}
