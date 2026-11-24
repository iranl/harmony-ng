/* Two hub_rf handlers that kept stale or lost state, run through the real hub_rf sources (main
 * renamed, /dev/rfspi a socket pair, /data under HARMONY_MOCK_DIR):
 * - an unpair the remote asked for cleared only the slot's pairing: the gone remote's version,
 *   config version, capabilities, identity and battery stayed for the next remote in the slot,
 *   and the ACK went out after the slot was removed from the CC2544, so it could not arrive
 * - the remote answers every settings push with a config backup, and the Hub replaced the stored
 *   settings with it: noScreen and powerDebug, which the backup does not carry, were lost and the
 *   next config push switched no-screen mode and power logging off again */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "rf_state.h"
#include "rf_spi.h"
#include "rf_pairing.h"
#include "rf_keymap.h"
#include "cJSON.h"

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

static void frame(uint8_t slot, uint8_t type, const uint8_t *payload, size_t len) {
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.report_id = 0x20; f.dev_slot = slot; f.sub_id = 0x12; f.hdr_flag = 0x43; f.seq = 3;
    f.msg_type = type;
    if (payload) memcpy(f.payload, payload, len);
    handle_rf_frame(&f);
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0];

    /* "Unpair all" must clear each physical CC2544 slot, not send the NAKed slot-0 request. The
     * notice goes only to slots that hold a remote (here 1 and 3); the radio command to all six.
     * unpair_device() has written everything when it returns: MSG_DONTWAIT makes a missing
     * write a failure instead of a hang. */
    g_paired_remotes[0].active = true;
    g_paired_remotes[2].active = true;
    unpair_device(0);
    for (uint8_t slot = 1; slot <= 6; slot++) {
        uint8_t out[64];
        ssize_t n;
        if (slot == 1 || slot == 3) {
            n = recv(sv[1], out, sizeof(out), MSG_DONTWAIT);
            CHECK(n == 30 && out[0] == 0x12 && out[1] == slot && out[3] == RF_MSG_UNPAIR_NOTIFY,
                  "slot %u unpair notification was not sent (%zd bytes)", slot, n);
        }
        n = recv(sv[1], out, sizeof(out), MSG_DONTWAIT);
        CHECK(n == 7 && out[0] == 0x10 && out[1] == 0xFF && out[2] == 0x80 &&
              out[3] == 0xB2 && out[4] == 0x03 && out[5] == slot && out[6] == 0x00,
              "slot %u hardware unpair command was not sent (%zd bytes)", slot, n);
    }

    /* A Smart Control (no screen) gets no unpair notice, only the radio command, and a push
     * waiting for its slot goes with it: the status no longer knew the slot as screenless and
     * showed the push as pending */
    g_paired_remotes[1].active = true;
    g_paired_remotes[1].model = REMOTE_MODEL_SMART_CONTROL;
    FILE *pf = fopen("/data/harmony/rf_sync_pending.flag", "w");
    if (pf) { fputs("1\n", pf); fclose(pf); }
    pf = fopen("/data/harmony/rf_sync_pending_details.json", "w");
    if (pf) { fputs("{\"slot\":2,\"diff\":\"Remote Slot 2 button mappings updated.\"}\n", pf); fclose(pf); }
    unpair_device(2);
    {
        uint8_t o2[64];
        ssize_t n = recv(sv[1], o2, sizeof(o2), MSG_DONTWAIT);
        CHECK(n == 7 && o2[0] == 0x10 && o2[2] == 0x80 && o2[3] == 0xB2,
              "Smart Control unpair: the first write was not the radio command (%zd bytes, type %02X)", n, n > 3 ? o2[3] : 0);
        n = recv(sv[1], o2, sizeof(o2), MSG_DONTWAIT);
        CHECK(n < 0, "Smart Control unpair wrote %zd more bytes", n);
    }
    CHECK(access("/data/harmony/rf_sync_pending.flag", F_OK) != 0, "the push pending for the unpaired slot 2 stayed");

    /* hub_rf reads a screenless slot's key map again when hub_daemon saved a new one */
    g_paired_remotes[2].model = REMOTE_MODEL_COMPANION;
    FILE *mf = fopen("/data/harmony/remote_slot3_map.json", "w");
    if (mf) { fputs("{\"activities\":{\"-1\":{\"Play\":{\"type\":\"command\",\"deviceId\":\"1\",\"command\":\"Play\"}}}}", mf); fclose(mf); }
    cJSON *m = get_remote_map_unlocked(3);
    CHECK(m && cJSON_GetObjectItem(cJSON_GetObjectItem(m, "activities"), "-1"), "slot 3 map not read");
    rename("/data/harmony/remote_slot3_map.json", "/data/harmony/remote_slot3_map.old");
    mf = fopen("/data/harmony/remote_slot3_map.json", "w");
    if (mf) { fputs("{\"activities\":{\"7\":{}}}", mf); fclose(mf); }
    m = get_remote_map_unlocked(3);
    CHECK(m && cJSON_GetObjectItem(cJSON_GetObjectItem(m, "activities"), "7"), "slot 3 map saved again but hub_rf kept the old one");

    /* A touchscreen slot keeps the pushed map: its on-screen buttons are indexes into it */
    g_paired_remotes[3].model = REMOTE_MODEL_JUNIPER;
    mf = fopen("/data/harmony/remote_slot4_map.json", "w");
    if (mf) { fputs("{\"activities\":{\"5\":{}}}", mf); fclose(mf); }
    m = get_remote_map_unlocked(4);
    CHECK(m && cJSON_GetObjectItem(cJSON_GetObjectItem(m, "activities"), "5"), "slot 4 map not read");
    rename("/data/harmony/remote_slot4_map.json", "/data/harmony/remote_slot4_map.old");
    mf = fopen("/data/harmony/remote_slot4_map.json", "w");
    if (mf) { fputs("{\"activities\":{\"6\":{}}}", mf); fclose(mf); }
    m = get_remote_map_unlocked(4);
    CHECK(m && cJSON_GetObjectItem(cJSON_GetObjectItem(m, "activities"), "5"), "touchscreen slot 4 took a saved map before the push");
    rf_keymap_invalidate_map(4);
    m = get_remote_map_unlocked(4);
    CHECK(m && cJSON_GetObjectItem(cJSON_GetObjectItem(m, "activities"), "6"), "slot 4 kept the old map after the push");

    /* 1. The remote in slot 1 unpairs itself */
    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = true;
        g_paired_remotes[i].rf_addr[0] = 0x11;
    }
    g_has_paired_device = true;
    snprintf(g_remote_versions[0].sw_version, sizeof(g_remote_versions[0].sw_version), "%s", "2.4.0");
    g_remote_versions[0].config_version = 5;
    g_remote_versions[0].caps = 0x07;
    snprintf(g_remote_versions[0].remote_id, sizeof(g_remote_versions[0].remote_id), "%s", "20200101000000-000001");
    g_remote_batteries[0].percent = 80;
    frame(1, RF_MSG_UNPAIR_NOTIFY, NULL, 0);

    uint8_t out[64];
    ssize_t n1 = recv(sv[1], out, sizeof(out), MSG_DONTWAIT);
    int ack_first = (n1 == 30 && out[0] == 0x12 && out[3] == RF_MSG_ACK);
    CHECK(ack_first, "the first write after UNPAIR_NOTIFY was not the ACK (%zd bytes, %02X %02X %02X %02X)",
          n1, out[0], out[1], out[2], n1 > 3 ? out[3] : 0);
    CHECK(!g_paired_remotes[0].active, "slot 1 still paired");
    CHECK(strcmp(g_remote_versions[0].sw_version, "unknown") == 0 && g_remote_versions[0].config_version == 0 &&
          g_remote_versions[0].caps == 0, "slot 1 kept version %s, config %u, caps 0x%02X of the gone remote",
          g_remote_versions[0].sw_version, g_remote_versions[0].config_version, g_remote_versions[0].caps);
    CHECK(g_remote_versions[0].remote_id[0] == '\0' && g_remote_batteries[0].percent == 0,
          "slot 1 kept unit id '%s' and battery %u%%", g_remote_versions[0].remote_id, g_remote_batteries[0].percent);
    CHECK(g_paired_remotes[1].active, "control: slot 2 must stay paired");

    /* 2. The settings API stored noScreen and powerDebug; the remote's backup follows the push */
    FILE *cf = fopen("/data/harmony/remote_slot1_config.json", "w");
    if (cf) {
        fputs("{\"slot\":1,\"brightness\":3,\"timeout\":20,\"noScreen\":true,\"no_screen\":1,"
              "\"powerDebug\":true,\"power_debug\":1,\"debugLogging\":true,\"debug_logging\":1}", cf);
        fclose(cf);
    }
    static const uint8_t backup[25] = { 0x03, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0C, 0x02, 0x00, 0x01,
                                        0x32, 0x02, 0x16, 0x07, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00 };
    frame(1, RF_MSG_REMOTE_CONFIG_BACKUP, backup, sizeof(backup));
    char txt[4096] = {0};
    cf = fopen("/data/harmony/remote_slot1_config.json", "r");
    if (cf) { size_t r = fread(txt, 1, sizeof(txt) - 1, cf); txt[r] = '\0'; fclose(cf); }
    cJSON *o = cJSON_Parse(txt);
    CHECK(o && cJSON_IsTrue(cJSON_GetObjectItem(o, "noScreen")), "noScreen lost after the backup: %s", txt);
    CHECK(o && cJSON_IsTrue(cJSON_GetObjectItem(o, "powerDebug")), "powerDebug lost after the backup: %s", txt);
    cJSON *t = o ? cJSON_GetObjectItem(o, "timeout") : NULL;
    CHECK(t && t->valueint == 20, "control: the backup's own values are stored (timeout %d)", t ? t->valueint : -1);
    cJSON_Delete(o);

    if (s_failures) {
        printf("[-] test_rf_unpair_backup: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] test_rf_unpair_backup: unpair clears the slot, the settings backup keeps what it does not carry\n");
    return 0;
}
