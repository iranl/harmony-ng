/* hub_btstack hands a new device-to-host link to hub_daemon (POST /api/bt-link-device), which owns
 * devices.json. Built against the fake BTstack (bt_fake_btstack.h); the test plays hub_daemon on a
 * loopback port and drives the run loop by hand.
 *
 * Checks, each against what the old code did:
 *   - hub_btstack never blocks on hub_daemon: with hub_daemon not answering, the call returns at
 *     once (it waited up to 3 s in recv, with every key behind it)
 *   - a link hub_daemon did not answer is kept and sent again once hub_daemon answers (it was
 *     written into devices.json behind hub_daemon's back, and the next device edit undid it)
 *   - the pause between tries doubles from 1 s to at most 60 s
 *   - a link hub_daemon refused (no such device) is dropped, a 5xx answer is tried again
 *   - a newer link of the same device replaces the waiting one */
#define TEST_DIR "/tmp/hng_bt_link_test"
#define DAEMON_PORT 9241
#include "bt_fake_btstack.h"

#include <poll.h>

#include "../payload/source/hub_btstack/bt_state.c"
#include "../payload/source/hub_btstack/bt_hid.c"
#if __has_include("../payload/source/hub_btstack/bt_hid_queue.c")
#include "../payload/source/hub_btstack/bt_hid_queue.c"
#endif
#include "../payload/source/hub_btstack/bt_cmd.c"
#include "../payload/source/hub_btstack/bt_pairing.c"

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("[-] FAIL: " __VA_ARGS__); printf("\n"); s_fail = 1; } else { printf("  ok  " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t elapsed_ms(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (uint32_t)((t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000);
}

/* What the run loop would do: call the link exchange's data source while it waits for its socket */
static void pump(int ms) {
#ifdef LINK_PENDING_MAX
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (s_link_phase != LINK_IDLE && (int)elapsed_ms(&t0) < ms) {
        struct pollfd p = { .fd = s_link_fd, .events = 0, .revents = 0 };
        if (s_link_ds.flags & DATA_SOURCE_CALLBACK_WRITE) p.events |= POLLOUT;
        if (s_link_ds.flags & DATA_SOURCE_CALLBACK_READ) p.events |= POLLIN;
        if (poll(&p, 1, 10) <= 0) continue;
        if (s_link_ds.flags & DATA_SOURCE_CALLBACK_WRITE) s_link_ds.process(&s_link_ds, DATA_SOURCE_CALLBACK_WRITE);
        else s_link_ds.process(&s_link_ds, DATA_SOURCE_CALLBACK_READ);
    }
#else
    (void)ms;
#endif
}

/* The pause before the next try is over */
static bool fire_retry(void) {
#ifdef LINK_PENDING_MAX
    if (!s_link_retry_timer.armed) return false;
    s_link_retry_timer.armed = false;
    s_link_retry_timer.process(&s_link_retry_timer);
    return true;
#else
    return false;
#endif
}

static uint32_t retry_pause_ms(void) {
#ifdef LINK_PENDING_MAX
    return s_link_retry_timer.armed ? s_link_retry_timer.timeout : 0;
#else
    return 0;
#endif
}

static int pending_links(void) {
#ifdef LINK_PENDING_MAX
    return s_links_n;
#else
    return 0;
#endif
}

static int listen_daemon(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(DAEMON_PORT);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) != 0 || listen(s, 4) != 0) {
        printf("[-] FAIL: cannot listen on port %d: %s\n", DAEMON_PORT, strerror(errno));
        s_fail = 1;
        close(s);
        return -1;
    }
    return s;
}

/* Play hub_daemon for one request: read it into req, answer with answer (NULL: close without
 * answering). Returns false when no request came. */
static bool serve_one(int ls, const char *answer, char *req, size_t reqlen) {
    struct pollfd p = { .fd = ls, .events = POLLIN, .revents = 0 };
    req[0] = '\0';
    for (int i = 0; i < 50 && poll(&p, 1, 10) <= 0; i++) pump(5);
    if (!(p.revents & POLLIN)) return false;
    int c = accept(ls, NULL, NULL);
    if (c < 0) return false;
    size_t got = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (got < reqlen - 1 && elapsed_ms(&t0) < 2000) {
        pump(5);
        struct pollfd cp = { .fd = c, .events = POLLIN, .revents = 0 };
        if (poll(&cp, 1, 10) <= 0) continue;
        ssize_t n = recv(c, req + got, reqlen - 1 - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
        req[got] = '\0';
        char *hdr_end = strstr(req, "\r\n\r\n");
        char *cl = strstr(req, "Content-Length: ");
        if (hdr_end && cl && strlen(hdr_end + 4) >= (size_t)atoi(cl + 16)) break;
    }
    if (answer && send(c, answer, strlen(answer), MSG_NOSIGNAL) < 0) s_fail = 1;
    close(c);
    pump(500);
    return got > 0;
}

#define OK_ANSWER "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}"
#define NO_DEVICE_ANSWER "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 12\r\nConnection: close\r\n\r\n{\"ok\":false}"
#define BUSY_ANSWER "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"

int main(void) {
    if (system("rm -rf " TEST_DIR " && mkdir -p " TEST_DIR) != 0) return 1;
    memset(g_hosts, 0, sizeof(g_hosts));
    for (int i = 0; i < MAX_HOST_CONNS; i++) g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
    char req[1024];

    printf("[-] hub_daemon not listening (restarting)\n");
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    link_bt_device_to_addr("3", "00:11:22:33:44:01");
    pump(500);
    CHECK(pending_links() == 1 && retry_pause_ms() == 1000, "the link is kept and tried again in 1 s (pending %d, pause %u)",
          pending_links(), retry_pause_ms());
    CHECK(fire_retry() && (pump(500), retry_pause_ms() == 2000), "no answer again: the pause doubles (%u ms)", retry_pause_ms());
    for (int i = 0; i < 8; i++) {
        fire_retry();
        pump(500);
    }
    CHECK(retry_pause_ms() == 60000, "and stops growing at 60 s (%u ms)", retry_pause_ms());

    printf("[-] hub_daemon answers again\n");
    int ls = listen_daemon();
    if (ls < 0) return 1;
    fire_retry();
    bool got = serve_one(ls, OK_ANSWER, req, sizeof(req));
    CHECK(got && strstr(req, "POST /api/bt-link-device ") && strstr(req, "\"deviceId\":\"3\"") && strstr(req, "\"bdaddr\":\"00:11:22:33:44:01\""),
          "the kept link reaches hub_daemon once it answers (it was written into devices.json behind its back)");
    CHECK(pending_links() == 0 && retry_pause_ms() == 0, "answered: nothing waits any more");

    printf("[-] answers that end or keep a link\n");
    link_bt_device_to_addr("99", "00:11:22:33:44:02");
    got = serve_one(ls, NO_DEVICE_ANSWER, req, sizeof(req));
    CHECK(got && pending_links() == 0 && retry_pause_ms() == 0, "refused (no such device): dropped, not tried again");
    link_bt_device_to_addr("3", "00:11:22:33:44:01");
    got = serve_one(ls, BUSY_ANSWER, req, sizeof(req));
    CHECK(got && pending_links() == 1 && retry_pause_ms() == 1000, "a 5xx answer is tried again");
    link_bt_device_to_addr("3", "00:11:22:33:44:03");
    got = serve_one(ls, OK_ANSWER, req, sizeof(req));
    CHECK(got && strstr(req, "00:11:22:33:44:03") && pending_links() == 0, "a newer link of the same device replaces the waiting one");

    printf("[-] hub_daemon alive but not answering\n");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    link_bt_device_to_addr("4", "00:11:22:33:44:04");
    uint32_t took = elapsed_ms(&t0);
    CHECK(took < 500, "the call returns at once, keys are not held up (%u ms; it waited in recv up to 3 s)", took);
    pump(200);
#ifdef LINK_PENDING_MAX
    CHECK(s_link_phase == LINK_READING && s_link_timeout_timer.armed && s_link_timeout_timer.timeout == LINK_EXCHANGE_TIMEOUT_MS,
          "the request is out and a 3 s timeout runs on the run loop");
    s_link_timeout_timer.armed = false;
    s_link_timeout_timer.process(&s_link_timeout_timer);
#endif
    CHECK(pending_links() == 1 && retry_pause_ms() == 1000, "no answer in time: kept and tried again later");
    /* the connection hub_daemon never took */
    int c = accept(ls, NULL, NULL);
    if (c >= 0) close(c);
    fire_retry();
    got = serve_one(ls, OK_ANSWER, req, sizeof(req));
    CHECK(got && strstr(req, "\"deviceId\":\"4\"") && pending_links() == 0, "and sent when hub_daemon answers");
    close(ls);

    if (system("rm -rf " TEST_DIR) != 0) return 1;
    if (s_fail) return 1;
    printf("[+] test_bt_link_retry: device links wait for hub_daemon instead of being written behind its back\n");
    return 0;
}
