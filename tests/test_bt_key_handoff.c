/* hub_daemon's hand-off of a Bluetooth key to hub_btstack (hw_action.c, the key pipe
 * /tmp/bthid_input). open() of the pipe is wrapped (-Wl,--wrap=open): the test gives hw_action.c
 * one end of a datagram socket pair instead, so every write() arrives as one message and the
 * number of writes can be counted.
 *
 *   - the target line and the command go in ONE write. They were two writes: another writer's
 *     command could land between the two, and a full pipe could take the target and lose the key.
 *   - a write the pipe refuses is reported (return value, log line). The result of both writes
 *     was ignored and the key counted as sent. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../payload/source/cJSON.h"
#include "../payload/source/hw_action.h"
#include "../payload/source/orchestrator.h"
#include "../payload/source/hub_events.h"

#define PIPE_PATH "/tmp/bthid_input"
#define HOST_ADDR "00:11:22:33:44:55"

/* ---- what hw_action.c needs from the rest of hub_daemon ---- */
orch_state_t orch_get_state(void) { return ORCH_STATE_RUNNING; }
const char *orch_get_current_activity(void) { return "1"; }
void hub_events_emit(hub_event_type_t type, cJSON *payload) { (void)type; cJSON_Delete(payload); }
/* The device list: hw_action.c takes a copy today; with the shared-snapshot change it takes a
 * reference instead. Both forms are here so this test builds either way. */
static cJSON *device_list_new(void) {
    /* btAutoReconnect 0: the key itself is the subject, not the reconnect it can trigger */
    return cJSON_Parse("{\"devices\":[{\"id\":\"2\",\"name\":\"Box\",\"type\":\"BT\",\"bdaddr\":\"" HOST_ADDR "\","
                       "\"btAutoReconnect\":0,\"interDeviceDelay\":0,\"interKeyDelay\":0,\"holdInterKeyDelay\":0,"
                       "\"commands\":[{\"name\":\"DirectionRight\",\"keyCode\":\"HID:07:4F\"},"
                       "{\"name\":\"Home\",\"keyCode\":\"HID:0C:223\"}]}]}");
}
cJSON *get_device_list_copy(void);
cJSON *get_device_list_copy(void) { return device_list_new(); }
struct cfg_snap;
static cJSON *s_shared_list;
const cJSON *get_device_list_acquire(struct cfg_snap **snap);
const cJSON *get_device_list_acquire(struct cfg_snap **snap) {
    if (!s_shared_list) s_shared_list = device_list_new();
    *snap = (struct cfg_snap *)&s_shared_list;
    return s_shared_list;
}
void release_device_list(struct cfg_snap **snap);
void release_device_list(struct cfg_snap **snap) { *snap = NULL; }
int ir_encode_pronto(const char *pronto, uint8_t repeats, uint32_t gap, uint8_t **out_bin, size_t *out_len) {
    (void)pronto; (void)repeats; (void)gap; (void)out_bin; (void)out_len;
    return -1;
}
int ir_encode_raw(uint32_t freq_hz, const uint32_t *timings_us, size_t count, uint8_t repeats, uint8_t **out_bin, size_t *out_len) {
    (void)freq_hz; (void)timings_us; (void)count; (void)repeats; (void)out_bin; (void)out_len;
    return -1;
}
int ir_encode_harmony_keycode(const char *keycode, uint8_t repeats, uint8_t **out_bin, size_t *out_len) {
    (void)keycode; (void)repeats; (void)out_bin; (void)out_len;
    return -1;
}
int ir_i2s_blast_blob(const uint8_t *blob, size_t len, uint8_t port_mask) { (void)blob; (void)len; (void)port_mask; return 0; }
/* The held-key IR player change adds these two to the transmitter; unused here */
int ir_i2s_blast_blob_part(const uint8_t *blob, size_t blob_len, int part, uint8_t port_mask);
int ir_i2s_blast_blob_part(const uint8_t *blob, size_t blob_len, int part, uint8_t port_mask) { (void)blob; (void)blob_len; (void)part; (void)port_mask; return -1; }
void ir_i2s_tx_hold(int on);
void ir_i2s_tx_hold(int on) { (void)on; }
void ir_free_encoded(uint8_t *bin) { free(bin); }

/* ---- the pipe ---- */
enum { PIPE_TAKES, PIPE_REFUSES, PIPE_MISSING };
static int s_pipe_mode = PIPE_TAKES;
static int s_sock[2] = { -1, -1 }; /* [0] the test reads, [1] hw_action.c writes */

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (path && strcmp(path, PIPE_PATH) == 0) {
        if (s_pipe_mode == PIPE_MISSING) {
            errno = ENXIO; /* a FIFO nobody reads */
            return -1;
        }
        /* a descriptor every write to fails */
        if (s_pipe_mode == PIPE_REFUSES) return __real_open("/dev/null", O_RDONLY);
        return dup(s_sock[1]);
    }
    return __real_open(path, flags, mode);
}

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("[-] FAIL: " __VA_ARGS__); printf("\n"); s_fail = 1; } } while (0)

/* The writes since the last call, one per message; returns their number */
static int take_writes(char out[4][256]) {
    int n = 0;
    for (;;) {
        char buf[256];
        ssize_t r = recv(s_sock[0], buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (r < 0) break;
        buf[r] = '\0';
        if (n < 4) snprintf(out[n], 256, "%s", buf);
        n++;
    }
    return n;
}

static void one_key(const char *command, int action, const char *want) {
    char got[4][256] = {{0}};
    int ret = hw_device_command_send_action("2", 0, command, action);
    int n = take_writes(got);
    CHECK(ret == 0, "%s (action %d): returned %d", command, action, ret);
    CHECK(n == 1, "%s (action %d): %d write(s) to the key pipe, expected 1", command, action, n);
    CHECK(n >= 1 && strcmp(got[0], want) == 0, "%s (action %d): the write does not hold target and command", command, action);
}

int main(void) {
    /* Not a dry run: a dry run prints Bluetooth keys instead of handing them over. The pipe is
     * the test's socket pair (open wrapped), and btAutoReconnect 0 keeps commands to
     * hub_btstack's own socket out of this path. */
    g_dry_run = 0;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, s_sock) != 0) {
        perror("socketpair");
        return 1;
    }

    /* 1. Press, release and tap: target and command in one write */
    one_key("DirectionRight", HW_BTN_ACTION_PRESS, "TARGET " HOST_ADDR "\nKBD_DOWN 4F\n");
    one_key("DirectionRight", HW_BTN_ACTION_RELEASE, "TARGET " HOST_ADDR "\nRELEASE\n");
    one_key("Home", HW_BTN_ACTION_TAP, "TARGET " HOST_ADDR "\nCONSUMER 223\n");

    /* 2. A pipe that refuses the write: the key is not reported as sent, and the log says so */
    char got[4][256] = {{0}};
    char logpath[] = "/tmp/bt_key_handoff_XXXXXX";
    int logfd = mkstemp(logpath);
    fflush(stdout);
    int saved_out = dup(STDOUT_FILENO);
    dup2(logfd, STDOUT_FILENO);
    s_pipe_mode = PIPE_REFUSES;
    int ret = hw_device_command_send_action("2", 0, "DirectionRight", HW_BTN_ACTION_PRESS);
    fflush(stdout);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_out);
    char logbuf[1024] = {0};
    ssize_t ln = pread(logfd, logbuf, sizeof(logbuf) - 1, 0);
    if (ln < 0) logbuf[0] = '\0';
    close(logfd);
    unlink(logpath);
    CHECK(ret != 0, "a refused write is reported as sent (returned %d)", ret);
    CHECK(strstr(logbuf, "not handed to hub_btstack") != NULL, "a refused write is not logged");
    CHECK(take_writes(got) == 0, "a refused write reached the pipe");

    /* 3. No pipe (hub_btstack not running): reported as before */
    s_pipe_mode = PIPE_MISSING;
    usleep(100 * 1000); /* past the 90 ms same-button filter */
    ret = hw_device_command_send_action("2", 0, "DirectionRight", HW_BTN_ACTION_PRESS);
    CHECK(ret != 0, "a key without a pipe is reported as sent (returned %d)", ret);

    if (s_fail) return 1;
    printf("[+] test_bt_key_handoff: target and command in one checked write\n");
    return 0;
}
