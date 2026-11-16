/* hub_rf threads and radio I/O, run through the real hub_rf sources (main renamed, /dev/rfspi a
 * socket pair, /data under HARMONY_MOCK_DIR, the RF API on a test port):
 * - every battery report made the radio's receive thread open a blocking connection to hub_daemon
 *   for a route hub_daemon does not have; the receive thread must not connect anywhere
 * - a /dev/rfspi that returns an error on every read was logged ten times a second for ever;
 *   the receive thread now stops hub_rf so its supervisor restarts it
 * - POST /api/rf/push-icon ran the whole icon transfer on the only API thread before answering
 *   (2.4 s per sleeping remote, half a minute for a large icon), so the API stopped answering */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rf_state.h"
#include "rf_spi.h"
#include "rf_api.h"

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

int __real_connect(int fd, const struct sockaddr *addr, socklen_t len);
static volatile int s_daemon_connects;
int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    if (addr && addr->sa_family == AF_INET && ntohs(in->sin_port) == DAEMON_PORT) {
        s_daemon_connects++;
        errno = ECONNREFUSED;
        return -1;
    }
    return __real_connect(fd, addr, len);
}

/* The Hub's CC2544 driver returns 0 from a complete write(): the fake device does the same */
ssize_t __real_write(int fd, const void *buf, size_t len);
static volatile int s_rf_write_returns_0;
ssize_t __wrap_write(int fd, const void *buf, size_t len) {
    ssize_t r = __real_write(fd, buf, len);
    if (s_rf_write_returns_0 && fd == g_rf_fd && r == (ssize_t)len) return 0;
    return r;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One request to the RF API; returns the time to the first byte of the answer, -1 on error */
static long api_request(const char *req, char *resp, size_t resp_sz) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons(RF_API_PORT) };
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < 50 && connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0; i++) {
        close(s);
        usleep(50000);
        s = socket(AF_INET, SOCK_STREAM, 0);
    }
    uint64_t t0 = now_ms();
    if (write(s, req, strlen(req)) < 0) { close(s); return -1; }
    ssize_t n = read(s, resp, resp_sz - 1);
    long dt = (long)(now_ms() - t0);
    resp[n > 0 ? n : 0] = '\0';
    close(s);
    return n > 0 ? dt : -1;
}

static volatile int s_rx_returned;
static void *rx_thread(void *arg) { (void)arg; rx_worker(NULL); s_rx_returned = 1; return NULL; }

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0]; /* the radio end (sv[1]) never answers: the remotes are asleep */
    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = (i == 0);
    }
    g_has_paired_device = true;

    /* 0. A write the driver answers with 0 is a sent report (the CC2544 driver on the Hub) */
    s_rf_write_returns_0 = 1;
    uint8_t cmd[7] = {0x10, 0xFF, 0x81, 0xF1, 0x01, 0x00, 0x00};
    rf_frame_t wf;
    rf_build_frame(&wf, 1, RF_MSG_PING, 1, 0, NULL, 0);
    int wr = rf_write_cmd(cmd, sizeof(cmd));
    int fr = rf_send_frame(&wf);
    CHECK(wr == 0 && fr == 0, "a write the driver answered with 0 counted as failed (command %d, frame %d)", wr, fr);
    s_rf_write_returns_0 = 0;
    uint8_t drain[64];
    while (recv(sv[1], drain, sizeof(drain), MSG_DONTWAIT) > 0) {}

    /* 1. A battery report: handled on the receive thread without a connection to hub_daemon */
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.report_id = 0x20; f.dev_slot = 1; f.sub_id = 0x12; f.hdr_flag = 0x41; f.seq = 1;
    f.msg_type = RF_MSG_BATTERY_STATUS;
    f.payload[0] = 80; f.payload[2] = 0x10; f.payload[3] = 0x0F;
    s_daemon_connects = 0;
    handle_rf_frame(&f);
    CHECK(s_daemon_connects == 0, "a battery report opened %d connection(s) to hub_daemon from the receive thread", s_daemon_connects);

    /* 2. The RF API answers a push-icon request before the transfer is done */
    char icdir[512];
    snprintf(icdir, sizeof(icdir), "%s/data/harmony/custom_icons", getenv("HARMONY_MOCK_DIR") ? getenv("HARMONY_MOCK_DIR") : "/tmp/harmony_mock");
    access("/data/harmony/x", F_OK); /* the mock creates its data/harmony folder */
    mkdir(icdir, 0755);
    FILE *ic = fopen("/data/harmony/custom_icons/activity_ic1.png", "wb");
    if (ic) { for (int i = 0; i < 300; i++) fputc(i & 0xFF, ic); fclose(ic); }
    pthread_t api;
    pthread_create(&api, NULL, api_server_worker, NULL);
    char resp[512];
    long dt = api_request("POST /api/rf/push-icon?id=ic1&type=activity&slot=1 HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(dt >= 0 && dt < 1000, "push-icon answered after %ld ms (the transfer to a sleeping remote takes 2.4 s)", dt);
    /* and the next request is answered too while the transfer still runs */
    dt = api_request("GET /api/rf/status HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    CHECK(dt >= 0 && dt < 1000, "a status request during an icon transfer waited %ld ms", dt);
    sleep(3); /* let the transfer give up */

    /* 3. A /dev/rfspi whose every read fails stops hub_rf (its supervisor restarts it) */
    int bad = open("/dev/null", O_WRONLY); /* read() fails with EBADF */
    g_rf_fd = bad;
    pthread_t rx;
    pthread_create(&rx, NULL, rx_thread, NULL);
    pthread_detach(rx);
    for (int i = 0; i < 100 && !s_rx_returned; i++) usleep(100000);
    CHECK(s_rx_returned && g_rf_read_failed && !g_running,
          "the receive thread kept running on a device that fails every read (returned: %s)", s_rx_returned ? "yes" : "no");

    if (s_failures) {
        printf("[-] test_rf_threads_io: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] test_rf_threads_io: the receive thread only handles frames, the API answers during transfers\n");
    return 0;
}
