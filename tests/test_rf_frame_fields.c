/* Fields hub_rf takes from RF frames and radio reports, run through the real hub_rf sources (main
 * renamed, /dev/rfspi a socket pair, /data under HARMONY_MOCK_DIR):
 * - DEVICE_CMD_REQUEST: a screen button the Hub does not know was parsed as a device command with
 *   a layout no remote sends; a direct device command ([0..3] id, [4..24] command) was misread
 * - ids and names from frames went unescaped into the JSON bodies posted to hub_daemon
 * - a key report or a link report from a CC2544 slot above 2 was taken as slot 1, or made that
 *   slot the default for sends
 * - the CC2544 slot name went unescaped into the RF status object
 * - the remote update source was copied without a terminator when it was 255 bytes or longer
 * The calls hub_rf makes to the dispatch queue, the companion key handler and hub_daemon are
 * replaced with -Wl,--wrap, so the test sees what would have been sent. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include "rf_state.h"
#include "rf_spi.h"
#include "rf_keymap.h"
#include "rf_ota.h"
#include "rf_api.h"
#include "cJSON.h"

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

static int s_cmds;
static char s_cmd_dev[64], s_cmd_cmd[64];
void __wrap_enqueue_remote_device_cmd(const char *dev, const char *cmd) {
    s_cmds++;
    snprintf(s_cmd_dev, sizeof(s_cmd_dev), "%s", dev);
    snprintf(s_cmd_cmd, sizeof(s_cmd_cmd), "%s", cmd);
}

static int s_companion;
void __wrap_handle_companion_packet(const uint8_t *pkt, size_t len) { (void)pkt; (void)len; s_companion++; }

static char s_post_body[1024];
int __wrap_http_post_local(int port, const char *endpoint, const char *body) {
    (void)port; (void)endpoint;
    snprintf(s_post_body, sizeof(s_post_body), "%s", body ? body : "");
    return 200;
}

static void frame(uint8_t slot, uint8_t type, uint8_t flags, const uint8_t *payload, size_t len) {
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.report_id = 0x20;
    f.dev_slot = slot;
    f.sub_id = 0x12;
    f.hdr_flag = 0x41;
    f.seq = 1;
    f.msg_type = type;
    f.flags = flags;
    memcpy(f.payload, payload, len);
    handle_rf_frame(&f);
}

static int s_radio = -1;
static void radio_report(const uint8_t *r, size_t len) {
    send(s_radio, r, len, 0);
    usleep(50000); /* rx_worker handles it */
}

static const char *body_str(const char *key) {
    static char out[256];
    out[0] = '\0';
    cJSON *o = cJSON_Parse(s_post_body);
    cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    if (cJSON_IsString(v)) snprintf(out, sizeof(out), "%s", v->valuestring);
    else snprintf(out, sizeof(out), "%s", o ? "(missing)" : "(body is not JSON)");
    cJSON_Delete(o);
    return out;
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0];
    s_radio = sv[1];
    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = true;
    }
    g_has_paired_device = true;
    pthread_t rx;
    pthread_create(&rx, NULL, rx_worker, NULL);

    /* 1. DEVICE_CMD_REQUEST: a screen button that is not in the map (flags bit 0) sends nothing */
    uint8_t sb[25] = {0};
    memcpy(sb, "12345678", 8);
    sb[10] = 3;
    memcpy(&sb[11], "Netflix", 7);
    s_cmds = 0;
    frame(1, RF_MSG_DEVICE_CMD_REQUEST, 0x01, sb, sizeof(sb));
    CHECK(s_cmds == 0, "an unknown screen button was sent as device '%s' command '%s'", s_cmd_dev, s_cmd_cmd);

    /* ... and a direct device command is read as [0..3] device id, [4..24] command */
    uint8_t dc[25] = {0};
    memcpy(dc, "7", 1);
    memcpy(&dc[4], "PowerOn", 7);
    s_cmds = 0;
    frame(1, RF_MSG_DEVICE_CMD_REQUEST, 0x00, dc, sizeof(dc));
    CHECK(s_cmds == 1 && strcmp(s_cmd_dev, "7") == 0 && strcmp(s_cmd_cmd, "PowerOn") == 0,
          "a direct device command was read as device '%s' command '%s' (%d sent)", s_cmd_dev, s_cmd_cmd, s_cmds);

    /* 2. Ids and names from frames are escaped in the bodies posted to hub_daemon */
    dispatch_activity_to_daemon("12\"34\\");
    CHECK(strcmp(body_str("activity"), "12\"34\\") == 0, "activity body %s", s_post_body);
    dispatch_device_command_to_webui("dev\"1:Power\"On");
    CHECK(strcmp(body_str("deviceId"), "dev\"1") == 0 && strcmp(body_str("command"), "Power\"On") == 0,
          "device command body %s", s_post_body);

    /* 3. A key report from CC2544 slot 3 is not a key of the remote in slot 1 */
    uint8_t key3[8] = {0x20, 0x03, 0x01, 0x00, 0x00, 0x52, 0x00, 0x00};
    s_companion = 0;
    radio_report(key3, sizeof(key3));
    CHECK(s_companion == 0, "a key report from slot 3 went to the key handler");
    uint8_t key1[8] = {0x20, 0x01, 0x01, 0x00, 0x00, 0x52, 0x00, 0x00};
    radio_report(key1, sizeof(key1));
    CHECK(s_companion == 1, "control: a key report from slot 1 must reach the key handler (%d)", s_companion);

    /* ... and a link report from slot 3 does not make slot 3 the default for sends */
    g_remote_slot = 1;
    uint8_t link3[7] = {0x10, 0x03, 0x41, 0x04, 0x00, 0x00, 0x00};
    radio_report(link3, sizeof(link3));
    CHECK(g_remote_slot == 1, "a link report from slot 3 made slot %u the default", g_remote_slot);

    /* 4. The CC2544 slot name keeps the RF status object valid JSON */
    uint8_t name[20] = {0x11, 0xFF, 0x83, 0xB5, 0x40, 6, 'A', '"', 'B', '\\', 'C', 'D'};
    radio_report(name, sizeof(name));
    static char status[4096];
    int blen = rf_build_status_json(status, sizeof(status), 0);
    cJSON *st = blen > 0 ? cJSON_Parse(status) : NULL;
    CHECK(st != NULL, "the RF status object is not valid JSON with slot name '%s'", g_paired_remotes[0].name);
    cJSON_Delete(st);

    /* 5. An update source of 300 characters is not cut into the download command */
    FILE *uf = fopen("/data/harmony/update_state.conf", "w");
    if (uf) {
        fprintf(uf, "source=https://");
        for (int i = 0; i < 300; i++) fputc('a', uf);
        fprintf(uf, "\n");
        fclose(uf);
    }
    char repo[256];
    memset(repo, 'X', sizeof(repo));
    get_ota_repo_source(repo, sizeof(repo));
    CHECK(memchr(repo, '\0', sizeof(repo)) != NULL && strncmp(repo, "https://raw.githubusercontent.com/", 34) == 0,
          "a 300-character update source gave '%.40s...' (terminated: %s)", repo, memchr(repo, '\0', sizeof(repo)) ? "yes" : "no");
    uf = fopen("/data/harmony/update_state.conf", "w");
    if (uf) { fprintf(uf, "source=https://example.com/a'b\n"); fclose(uf); }
    get_ota_repo_source(repo, sizeof(repo));
    CHECK(strchr(repo, '\'') == NULL, "an update source with a quote went into the command: %s", repo);
    uf = fopen("/data/harmony/update_state.conf", "w");
    if (uf) { fprintf(uf, "source=https://example.com/fork/ota\n"); fclose(uf); }
    get_ota_repo_source(repo, sizeof(repo));
    CHECK(strcmp(repo, "https://example.com/fork/ota") == 0, "control: a normal update source was not kept: %s", repo);
    unlink("/data/harmony/update_state.conf");

    g_running = false;
    shutdown(sv[0], SHUT_RDWR);
    shutdown(sv[1], SHUT_RDWR);

    if (s_failures) {
        printf("[-] test_rf_frame_fields: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] test_rf_frame_fields: frame fields are read in their layout and stay valid JSON\n");
    return 0;
}
