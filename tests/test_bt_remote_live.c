/* hub_btstack's BLE remote (bt_remote.c, the real one, against the fake BTstack): a key mapped to
 * a device command that is not Bluetooth goes to hub_daemon as POST /api/ir-send with
 * "live": true, for the press and the release. A person's press that arrives late is dropped by
 * hub_daemon instead of sent late, as a key from the RF remote (API.md 3.9); without the field it
 * waited in the send queue of an HTTP or TCP device and went out seconds later.
 *
 * hub_daemon is a listener on 127.0.0.1:DAEMON_PORT here that keeps every request body. */
#define TEST_DIR "/tmp/hng_bt_remote_live_test"
#define DAEMON_PORT 9689
#define BT_FAKE_WITH_REMOTE
#include "bt_fake_btstack.h"

#include "../payload/source/hub_btstack/bt_state.c"
#include "../payload/source/hub_btstack/bt_hid.c"
#if __has_include("../payload/source/hub_btstack/bt_hid_queue.c")
#include "../payload/source/hub_btstack/bt_hid_queue.c"
#endif
#include "../payload/source/hub_btstack/bt_cmd.c"
#include "../payload/source/hub_btstack/bt_pairing.c"
/* the GATT client part of bt_remote.c, which this test does not run: just enough to build it */
#define HCI_EVENT_PACKET 0x04
#define GATT_EVENT_NOTIFICATION 0xA7
#define HCI_EVENT_GATTSERVICE_META 0xEC
#define GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED 0x01
#define GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED 0x02
#define GATTSERVICE_SUBEVENT_HID_REPORT 0x03
static uint8_t hci_event_packet_get_type(const uint8_t *p) { return p[0]; }
static uint16_t gatt_event_notification_get_value_length(const uint8_t *p) { (void)p; return 0; }
static const uint8_t *gatt_event_notification_get_value(const uint8_t *p) { return p; }
static uint8_t hci_event_gattservice_meta_get_subevent_code(const uint8_t *p) { (void)p; return 0; }
static uint8_t gattservice_subevent_hid_service_connected_get_status(const uint8_t *p) { (void)p; return 0; }
static const uint8_t *gattservice_subevent_hid_report_get_report(const uint8_t *p) { return p; }
static uint16_t gattservice_subevent_hid_report_get_report_len(const uint8_t *p) { (void)p; return 0; }
#include "../payload/source/hub_btstack/bt_remote.c"

#include <pthread.h>

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("[-] FAIL: " __VA_ARGS__); printf("\n"); s_fail = 1; } else { printf("  ok  " __VA_ARGS__); printf("\n"); } } while (0)

#define MAX_REQ 16
static char s_req[MAX_REQ][1024];
static int s_req_count;
static pthread_mutex_t s_req_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_listen_fd = -1;

static void *listener(void *arg) {
    (void)arg;
    for (;;) {
        int c = accept(s_listen_fd, NULL, NULL);
        if (c < 0) return NULL;
        char buf[1024];
        size_t got = 0;
        ssize_t n;
        while (got < sizeof(buf) - 1 && (n = read(c, buf + got, sizeof(buf) - 1 - got)) > 0) got += (size_t)n;
        buf[got] = '\0';
        close(c);
        pthread_mutex_lock(&s_req_lock);
        if (s_req_count < MAX_REQ) snprintf(s_req[s_req_count++], sizeof(s_req[0]), "%s", buf);
        pthread_mutex_unlock(&s_req_lock);
    }
}

/* The body of the n-th POST to path, or NULL */
static const char *find_body(const char *path, int nth) {
    const char *found = NULL;
    pthread_mutex_lock(&s_req_lock);
    for (int i = 0; i < s_req_count; i++) {
        char line[96];
        snprintf(line, sizeof(line), "POST %s HTTP", path);
        if (strncmp(s_req[i], line, strlen(line)) != 0) continue;
        if (nth-- > 0) continue;
        const char *b = strstr(s_req[i], "\r\n\r\n");
        found = b ? b + 4 : NULL;
        break;
    }
    pthread_mutex_unlock(&s_req_lock);
    return found;
}

static void wait_requests(int n) {
    for (int i = 0; i < 200; i++) {
        pthread_mutex_lock(&s_req_lock);
        int c = s_req_count;
        pthread_mutex_unlock(&s_req_lock);
        if (c >= n) return;
        usleep(10000);
    }
}

int main(void) {
    mkdir(TEST_DIR, 0755);
    FILE *f = fopen(MAP_FILE, "w");
    if (!f) return 2;
    fprintf(f, "{\"activities\":{\"-1\":{\"up\":{\"action\":\"device_cmd\",\"deviceId\":\"63\",\"command\":\"Down\"}}}}\n");
    fclose(f);
    unlink(CURRENT_ACT_FILE);

    s_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(DAEMON_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s_listen_fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s_listen_fd, 8) != 0) {
        printf("[-] FAIL: cannot listen on %d\n", DAEMON_PORT);
        return 1;
    }
    pthread_t th;
    pthread_create(&th, NULL, listener, NULL);

    dispatch_button_event("up", 1);
    dispatch_button_event("up", 0);
    wait_requests(4);

    const char *press = find_body("/api/ir-send", 0);
    const char *release = find_body("/api/ir-send", 1);
    CHECK(press && strstr(press, "\"deviceId\":\"63\"") && strstr(press, "\"command\":\"Down\"") &&
          strstr(press, "\"action\":\"press\""), "the press goes to /api/ir-send: %s", press ? press : "(none)");
    CHECK(press && strstr(press, "\"live\":true"), "the press carries \"live\": true");
    CHECK(release && strstr(release, "\"action\":\"release\"") && strstr(release, "\"live\":true"),
          "the release carries \"live\": true: %s", release ? release : "(none)");
    CHECK(find_body("/api/button-press", 0) != NULL, "the button event still goes to /api/button-press");

    shutdown(s_listen_fd, SHUT_RDWR);
    close(s_listen_fd);
    unlink(MAP_FILE);
    if (s_fail) {
        printf("BLE remote live: FAILED\n");
        return 1;
    }
    printf("all BLE remote live checks passed\n");
    return 0;
}
