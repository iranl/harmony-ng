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
 *   - a link hub_daemon refused (404, no such device) is dropped; a 5xx answer, a 200 without its
 *     body or a cut answer is no answer and is tried again
 *   - a newer link of the same device replaces the waiting one; one replaced while its request is
 *     out is sent again after that answer, so the older address cannot land last
 *   - a link dropped while its request is out (host forgotten) is not sent again
 *   - the front link dropped during the pause before its next try: the next link is sent at once
 *     and starts its own pause at 1 s (it waited for the dropped link's pause, up to 60 s)
 *   - {"ok": true} with a space after the colon is taken (it was no answer)
 *   - at most 8 links wait; a 9th replaces the newest waiting one */
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

/* Play hub_daemon: take one connection and read its request into req (-1: none came) */
static int accept_read(int ls, char *req, size_t reqlen) {
    struct pollfd p = { .fd = ls, .events = POLLIN, .revents = 0 };
    req[0] = '\0';
    for (int i = 0; i < 50 && poll(&p, 1, 10) <= 0; i++) pump(5);
    if (!(p.revents & POLLIN)) return -1;
    int c = accept(ls, NULL, NULL);
    if (c < 0) return -1;
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
    if (got == 0) {
        close(c);
        return -1;
    }
    return c;
}

/* ... answer it (NULL: close without answering) and let hub_btstack read the answer */
static void answer(int c, const char *text) {
    if (text && send(c, text, strlen(text), MSG_NOSIGNAL) < 0) s_fail = 1;
    close(c);
    pump(500);
}

static bool serve_one(int ls, const char *text, char *req, size_t reqlen) {
    int c = accept_read(ls, req, reqlen);
    if (c < 0) return false;
    answer(c, text);
    return true;
}

static bool has_request(int ls) {
    struct pollfd p = { .fd = ls, .events = POLLIN, .revents = 0 };
    for (int i = 0; i < 20 && poll(&p, 1, 10) <= 0; i++) pump(5);
    return (p.revents & POLLIN) != 0;
}

#define OK_ANSWER "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}"
#define NO_DEVICE_ANSWER "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 34\r\nConnection: close\r\n\r\n{\"error\":\"no device with that id\"}"
#define HEADERS_ONLY "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n"
#define OK_FALSE_ANSWER "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\n{\"ok\":false}"
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
    CHECK(got && pending_links() == 0 && retry_pause_ms() == 0, "refused with 404 (no such device): dropped, not tried again");
    link_bt_device_to_addr("3", "00:11:22:33:44:01");
    got = serve_one(ls, BUSY_ANSWER, req, sizeof(req));
    CHECK(got && pending_links() == 1 && retry_pause_ms() == 1000, "a 5xx answer is tried again");
    link_bt_device_to_addr("3", "00:11:22:33:44:03");
    got = serve_one(ls, OK_ANSWER, req, sizeof(req));
    CHECK(got && strstr(req, "00:11:22:33:44:03") && pending_links() == 0, "a newer link of the same device replaces the waiting one");

    printf("[-] answers that are no answer\n");
    link_bt_device_to_addr("5", "00:11:22:33:44:05");
    got = serve_one(ls, HEADERS_ONLY, req, sizeof(req));
    CHECK(got && pending_links() == 1 && retry_pause_ms() == 1000, "a 200 cut before its body is tried again (it was dropped)");
    fire_retry();
    got = serve_one(ls, "HTTP/1.", req, sizeof(req));
    CHECK(got && pending_links() == 1 && retry_pause_ms() == 2000, "a 7-byte answer is tried again, not read past");
    fire_retry();
    got = serve_one(ls, OK_FALSE_ANSWER, req, sizeof(req));
    CHECK(got && pending_links() == 1, "a 200 {\"ok\":false} (a failed save on an older hub_daemon) is tried again, not dropped");
    fire_retry();
    got = serve_one(ls, OK_ANSWER, req, sizeof(req));
    CHECK(got && pending_links() == 0, "and the link is taken when hub_daemon saves it");
    link_bt_device_to_addr("9", "00:11:22:33:44:09");
    int okf = 0;
    for (int i = 0; i < 5 && pending_links() > 0; i++) {
        if (i) fire_retry();
        if (serve_one(ls, OK_FALSE_ANSWER, req, sizeof(req))) okf++;
    }
    CHECK(okf == 3 && pending_links() == 0 && retry_pause_ms() == 0,
          "an older hub_daemon's {\"ok\":false} (no such device) ends the link after 3 answers (%d answers)", okf);
    link_bt_device_to_addr("12", "00:11:22:33:44:0C");
    got = serve_one(ls, "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\n{\"ok\": true}", req, sizeof(req));
    CHECK(got && pending_links() == 0 && retry_pause_ms() == 0, "{\"ok\": true} with a space links it (it was no answer)");

    printf("[-] a link replaced while its request is out\n");
    link_bt_device_to_addr("6", "00:11:22:33:44:06");
    int c = accept_read(ls, req, sizeof(req));
    CHECK(c >= 0 && strstr(req, "00:11:22:33:44:06"), "the first request is out");
    link_bt_device_to_addr("6", "00:11:22:33:44:07");
    CHECK(!has_request(ls), "the newer one waits for the answer to the first (two at once could land in any order)");
    if (c >= 0) answer(c, OK_ANSWER);
    got = serve_one(ls, OK_ANSWER, req, sizeof(req));
    CHECK(got && strstr(req, "00:11:22:33:44:07") && pending_links() == 0, "then the newer address is sent, and lands last");

    printf("[-] the same link queued again while its request is out\n");
    link_bt_device_to_addr("7", "00:11:22:33:44:0A");
    c = accept_read(ls, req, sizeof(req));
    link_bt_device_to_addr("7", "00:11:22:33:44:0A");
    if (c >= 0) answer(c, OK_ANSWER);
    CHECK(c >= 0 && pending_links() == 0 && !has_request(ls), "it is not sent a second time");

    printf("[-] a link dropped while its request is out\n");
    link_bt_device_to_addr("8", "00:11:22:33:44:08");
    c = accept_read(ls, req, sizeof(req));
#ifdef LINK_PENDING_MAX
    bd_addr_t a8 = {0x00, 0x11, 0x22, 0x33, 0x44, 0x08};
    link_drop_host(a8);
#endif
    if (c >= 0) close(c);
    pump(200);
    CHECK(c >= 0 && pending_links() == 0 && retry_pause_ms() == 0 && !has_request(ls), "it is not sent again");

    printf("[-] full queue\n");
    close(ls);
    ls = -1;
    for (int i = 0; i < 9; i++) {
        char id[8], ad[18];
        snprintf(id, sizeof(id), "%d", 10 + i);
        snprintf(ad, sizeof(ad), "00:11:22:33:45:%02d", i);
        link_bt_device_to_addr(id, ad);
        pump(100);
    }
#ifdef LINK_PENDING_MAX
    CHECK(pending_links() == 8 && strcmp(s_links[0].dev_id, "10") == 0 && strcmp(s_links[7].dev_id, "18") == 0,
          "8 links wait; the 9th replaces the newest (%d waiting)", pending_links());
    s_links_n = 0;
    btstack_run_loop_remove_timer(&s_link_retry_timer);
    s_link_pause = false;
#else
    CHECK(0, "8 links wait; the 9th replaces the newest");
#endif

    printf("[-] the front link dropped during the pause before its next try\n");
    link_bt_device_to_addr("30", "00:11:22:33:46:01");
    pump(500);
    link_bt_device_to_addr("31", "00:11:22:33:46:02");
    pump(500);
    for (int i = 0; i < 3; i++) {
        fire_retry();
        pump(500);
    }
    CHECK(pending_links() == 2 && retry_pause_ms() == 8000, "two links wait, the front one after several tries without an answer (pause %u ms)",
          retry_pause_ms());
    ls = listen_daemon();
    if (ls < 0) return 1;
#ifdef LINK_PENDING_MAX
    bd_addr_t a30 = {0x00, 0x11, 0x22, 0x33, 0x46, 0x01};
    link_drop_host(a30);
#endif
    c = accept_read(ls, req, sizeof(req));
    CHECK(c >= 0 && strstr(req, "\"deviceId\":\"31\""), "its host forgotten: the next link is sent at once (it waited out the 8 s pause)");
    if (c >= 0) answer(c, BUSY_ANSWER);
    CHECK(pending_links() == 1 && retry_pause_ms() == 1000, "and a miss starts its own pause at 1 s (%u ms)", retry_pause_ms());
    fire_retry();
    got = serve_one(ls, OK_ANSWER, req, sizeof(req));
    CHECK(got && pending_links() == 0, "then it is linked");

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
    c = accept(ls, NULL, NULL);
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
