/* hub_rf's local RF API took its parameters by substring search over the whole request and
 * matched routes by prefix: a slot that was not "slot=1" or "slot=2" in the query (slot=3, a JSON
 * body, a typing error) meant "every remote" for unpair, "GET /api/rf/pairing-state" started
 * pairing, and the POST /api/rf/stop-pair both web UIs send got the status object. Runs the real
 * hub_rf sources (main renamed) with the RF API on a test port and /dev/rfspi a socket pair. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rf_state.h"
#include "rf_spi.h"
#include "rf_api.h"
#include "rf_pairing.h"

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

static int api(const char *req) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons(RF_API_PORT) };
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < 50 && connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0; i++) {
        close(s);
        usleep(50000);
        s = socket(AF_INET, SOCK_STREAM, 0);
    }
    if (write(s, req, strlen(req)) < 0) { close(s); return -1; }
    char resp[4096];
    ssize_t n = read(s, resp, sizeof(resp) - 1);
    close(s);
    if (n < 12) return -1;
    resp[n] = '\0';
    return atoi(resp + 9);
}

static void pair_both(void) {
    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = true;
        g_paired_remotes[i].rf_addr[0] = (uint8_t)(0x10 + i);
    }
    g_has_paired_device = true;
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0];
    pair_both();
    pthread_t th;
    pthread_create(&th, NULL, api_server_worker, NULL);

    int st = api("POST /api/rf/unpair?slot=7 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(st == 400 && g_paired_remotes[0].active && g_paired_remotes[1].active,
          "unpair?slot=7 answered %d, slot 1 %s, slot 2 %s", st,
          g_paired_remotes[0].active ? "paired" : "unpaired", g_paired_remotes[1].active ? "paired" : "unpaired");

    pair_both();
    st = api("POST /api/rf/unpair HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: 10\r\n\r\n{\"slot\":2}");
    CHECK(st == 200 && g_paired_remotes[0].active && !g_paired_remotes[1].active,
          "unpair with {\"slot\":2} answered %d, slot 1 %s, slot 2 %s", st,
          g_paired_remotes[0].active ? "paired" : "unpaired", g_paired_remotes[1].active ? "paired" : "unpaired");

    pair_both();
    st = api("POST /api/rf/unpair?slot=1 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(st == 200 && !g_paired_remotes[0].active && g_paired_remotes[1].active, "control: unpair?slot=1 (%d)", st);

    pair_both();
    st = api("GET /api/rf/pairing-state HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(!g_pairing_mode, "GET /api/rf/pairing-state started pairing (%d)", st);
    stop_pairing_mode();

    st = api("POST /api/rf/pair HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(st == 200 && g_pairing_mode, "control: POST /api/rf/pair (%d)", st);
    st = api("POST /api/rf/stop-pair HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(!g_pairing_mode, "POST /api/rf/stop-pair (the web UIs' call) left pairing on (%d)", st);
    stop_pairing_mode();

    st = api("POST /api/rf/wake?slot=x HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(st == 400, "wake?slot=x answered %d", st);

    st = api("POST /api/rf/push-icon?type=activity HTTP/1.1\r\nHost: x\r\nX-Note: id=abc\r\n\r\n");
    CHECK(st == 400, "push-icon took its id from a header (%d)", st);

    /* restart after a restored slot table: asked for, then only once nothing runs */
    CHECK(!rf_restart_due(), "a restart was due before one was asked for");
    st = api("POST /api/rf/restart HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}");
    CHECK(st == 202 && g_restart_requested, "POST /api/rf/restart answered %d", st);
    g_pairing_mode = true;
    CHECK(!rf_restart_due(), "a restart cut a pairing window short");
    g_pairing_mode = false;
    g_config_push_running = true;
    CHECK(!rf_restart_due(), "a restart cut a config push short");
    g_config_push_running = false;
    g_rf_ota.status_code = 3;
    CHECK(!rf_restart_due(), "a restart cut an update in preparation short");
    g_rf_ota.status_code = 0;
    CHECK(rf_restart_due(), "no restart once idle");
    g_restart_requested = false;

    if (s_failures) {
        printf("[-] test_rf_api_params: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] test_rf_api_params: routes match exactly, the slot comes from the query or the JSON body\n");
    return 0;
}
