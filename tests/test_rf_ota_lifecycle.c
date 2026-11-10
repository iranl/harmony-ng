/*
 * Unit test for RF OTA lifecycle and no-flash storage policy:
 * 1. Verifies that rf_migrate_legacy_names() purges stale flash copies from /data/harmony
 *    and /cache.
 * 2. Checks that rf_sync can advertise update flags from remote_ui.version
 *    even when no .ota.gz binary is stored on flash.
 * 3. Verifies that fatal errors unlink /tmp/remote_ui.ota.gz, while resumable
 *    chunk failures preserve it for resume.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>

#include "rf_state.h"
#include "rf_proto.h"
#include "rf_api.h"
#include "rf_sync.h"
#include "rf_ota.h"
#include "rf_spi.h"
#include "rf_keymap.h"
#include "timezone_db.h"
#include "hub_led.h"
#include "cJSON.h"

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } else { printf("  PASS %s\n", msg); } } while (0)

/* Stubs & globals required by rf_ota.c and rf_sync.c */
uint8_t g_tx_seq = 0;
int g_ack_status = 0;
uint8_t g_ack_payload[25] = {0};

void hub_led_show(int mode) { (void)mode; }
void hub_led_show_failure(void) {}
void hub_led_restore(void) {}
int rf_set_power_mode(uint8_t mode) { (void)mode; return 0; }
int rf_send_frame(rf_frame_t *f) { (void)f; return 0; }
int rf_send_frame_acked(rf_frame_t *f, int timeout_ms) { (void)f; (void)timeout_ms; return 0; }
int rf_write_cmd(const uint8_t *cmd, size_t len) { (void)cmd; (void)len; return 0; }
int rf_wake_remote(uint8_t slot, int attempts) { (void)slot; (void)attempts; return 0; }
int rf_send_ota_data(uint8_t slot, uint32_t idx, bool ack, const uint8_t *d, size_t sz) {
    (void)slot; (void)idx; (void)ack; (void)d; (void)sz; return 0;
}
void timezone_get_rf_prefix(const char *posix, char *out_prefix, size_t out_sz) {
    (void)posix;
    if (out_prefix && out_sz > 0) snprintf(out_prefix, out_sz, "UTC");
}
void get_activity_name_by_id(const char *act_id, char *out_name, size_t out_len) {
    (void)act_id;
    if (out_name && out_len > 0) snprintf(out_name, out_len, "Watch TV");
}
void get_current_activity(char *out, size_t outlen) {
    if (out && outlen > 0) snprintf(out, outlen, "1");
}
int is_activity_transitioning(void) { return 0; }

int main(void) {
    printf("[*] Starting test_rf_ota_lifecycle...\n");

    /* 1. Verify legacy flash copies purging under mock hardware (/data and /cache) */
    FILE *f1 = fopen("/data/harmony/remote_ui.ota.gz", "w");
    if (f1) { fputs("stale_data", f1); fclose(f1); }
    FILE *f2 = fopen("/cache/remote_ui.ota.gz", "w");
    if (f2) { fputs("stale_cache", f2); fclose(f2); }

    CHECK(access("/data/harmony/remote_ui.ota.gz", F_OK) == 0, "/data/harmony/remote_ui.ota.gz created in mock");
    CHECK(access("/cache/remote_ui.ota.gz", F_OK) == 0, "/cache/remote_ui.ota.gz created in mock");

    rf_migrate_legacy_names();

    CHECK(access("/data/harmony/remote_ui.ota.gz", F_OK) != 0, "legacy remote_ui.ota.gz purged from /data/harmony");
    CHECK(access("/cache/remote_ui.ota.gz", F_OK) != 0, "legacy remote_ui.ota.gz purged from /cache");

    /* 2. Set up paired remote in slot 1 with software version 2.0.0 */
    g_paired_remotes[0].active = true;
    g_paired_remotes[0].model = REMOTE_MODEL_JUNIPER;
    strncpy(g_remote_versions[0].sw_version, "2.0.0", sizeof(g_remote_versions[0].sw_version));

    /* Write remote_ui.version with 2.1.0 */
    FILE *vf = fopen("/data/harmony/remote_ui.version", "w");
    assert(vf != NULL);
    fputs("2.1.0\n", vf);
    fclose(vf);

    /* Verify rf_check_update_flags advertises flag 0x02 even without flash .ota.gz */
    uint8_t flags = rf_check_update_flags(1);
    CHECK((flags & 0x02) != 0, "rf_check_update_flags reports update available (0x02) from remote_ui.version");

    /* 3. Test fatal error cleanup: zero-length file in /tmp */
    unlink("/tmp/remote_ui.ota.gz");
    FILE *f_empty = fopen("/tmp/remote_ui.ota.gz", "w");
    assert(f_empty != NULL);
    fclose(f_empty);
    CHECK(access("/tmp/remote_ui.ota.gz", F_OK) == 0, "empty /tmp/remote_ui.ota.gz created");

    rf_ota_sender_worker((void *)(uintptr_t)1);
    CHECK(access("/tmp/remote_ui.ota.gz", F_OK) != 0, "invalid/empty /tmp/remote_ui.ota.gz unlinked on fatal error");

    /* 4. Test resumable failure retention: create valid synthetic payload in /tmp */
    FILE *f_dummy = fopen("/tmp/remote_ui.ota.gz", "wb");
    assert(f_dummy != NULL);
    uint8_t dummy[1024];
    memset(dummy, 0xAB, sizeof(dummy));
    fwrite(dummy, 1, sizeof(dummy), f_dummy);
    fclose(f_dummy);

    /* Setup windowed OTA where send encounters a stall timeout (resumable) */
    g_ack_status = 0x00;
    g_ack_payload[4] = RF_OTA_WINDOWED; /* remote reports windowed transfer support */

    /* Run worker: wake succeeds, OTA_START ACK received, but windowed transfer stalls */
    rf_ota_sender_worker((void *)(uintptr_t)1);

    /* Verify file is KEPT on resumable failure */
    CHECK(access("/tmp/remote_ui.ota.gz", F_OK) == 0, "/tmp/remote_ui.ota.gz retained on resumable ACK timeout");

    unlink("/tmp/remote_ui.ota.gz");
    unlink("/data/harmony/remote_ui.version");

    if (g_failures) {
        printf("[-] %d tests failed!\n", g_failures);
        return 1;
    }

    printf("[+] All RF OTA lifecycle tests passed successfully!\n");
    return 0;
}
