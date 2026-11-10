/*
 * Unit test for RF OTA lifecycle and no-flash storage policy:
 * 1. Checks that rf_sync can advertise update flags from remote_ui.version
 *    even when no .ota.gz binary is stored on flash.
 * 2. Checks that rf_ota_sender_worker purges stale flash copies and removes
 *    /tmp/remote_ui.ota.gz after completion.
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
void rf_set_power_mode(int mode) { (void)mode; }
int rf_send_frame(rf_frame_t *f) { (void)f; return 0; }
int rf_send_frame_acked(rf_frame_t *f, int timeout_ms) { (void)f; (void)timeout_ms; return 0; }
int rf_write_cmd(const uint8_t *cmd, size_t len) { (void)cmd; (void)len; return 0; }
int rf_wake_remote(uint8_t slot) { (void)slot; return 0; }
int rf_send_ota_data(uint8_t slot, uint16_t idx, bool ack, const uint8_t *d, size_t sz) {
    (void)slot; (void)idx; (void)ack; (void)d; (void)sz; return 0;
}
const char *timezone_get_rf_prefix(void) { return "UTC"; }
const char *get_activity_name_by_id(const char *id) { (void)id; return "Watch TV"; }
int get_current_activity(void) { return 1; }
int is_activity_transitioning(void) { return 0; }

int main(void) {
    printf("[*] Starting test_rf_ota_lifecycle...\n");

    /* 1. Ensure clean slate in /tmp */
    unlink("/tmp/remote_ui.ota.gz");

    /* Create dummy /tmp/remote_ui.ota.gz (synthetic small payload) */
    FILE *f = fopen("/tmp/remote_ui.ota.gz", "wb");
    assert(f != NULL);
    uint8_t dummy[1024];
    memset(dummy, 0xAB, sizeof(dummy));
    fwrite(dummy, 1, sizeof(dummy), f);
    fclose(f);

    CHECK(access("/tmp/remote_ui.ota.gz", F_OK) == 0, "/tmp/remote_ui.ota.gz created for test");

    /* Create temporary version dir in /tmp (works unprivileged in CI) */
    char vdir[] = "/tmp/rf_ota_test_XXXXXX";
    if (!mkdtemp(vdir)) {
        perror("mkdtemp");
        return 1;
    }
    g_rf_version_dir = vdir;

    /* Create mock stale flash file in vdir to verify purging */
    char stale_path[256];
    snprintf(stale_path, sizeof(stale_path), "%s/remote_ui.ota.gz", vdir);
    FILE *f_stale = fopen(stale_path, "wb");
    if (f_stale) { fwrite("stale", 1, 5, f_stale); fclose(f_stale); }
    CHECK(access(stale_path, F_OK) == 0, "mock stale file created");

    /* 2. Set up paired remote in slot 1 with software version 2.0.0 */
    g_paired_remotes[0].active = true;
    g_paired_remotes[0].model = REMOTE_MODEL_JUNIPER;
    strncpy(g_remote_versions[0].sw_version, "2.0.0", sizeof(g_remote_versions[0].sw_version));

    /* Write remote_ui.version with 2.1.0 in temp vdir */
    char ver_path[256];
    snprintf(ver_path, sizeof(ver_path), "%s/remote_ui.version", vdir);
    FILE *vf = fopen(ver_path, "w");
    assert(vf != NULL);
    fputs("2.1.0\n", vf);
    fclose(vf);

    /* Verify rf_check_update_flags advertises flag 0x02 even without flash .ota.gz */
    uint8_t flags = rf_check_update_flags(1);
    CHECK((flags & 0x02) != 0, "rf_check_update_flags reports update available (0x02) from version file");

    /* 3. Run rf_ota_sender_worker: target slot 1, with rejection to test cleanup */
    g_ack_status = 0x02; /* Low battery rejection */
    rf_ota_sender_worker((void *)(uintptr_t)1);

    /* 4. Verify stale flash files were purged */
    CHECK(access(stale_path, F_OK) != 0, "stale remote_ui.ota.gz was purged from version dir");

    /* 5. Verify /tmp/remote_ui.ota.gz was unlinked upon worker exit */
    CHECK(access("/tmp/remote_ui.ota.gz", F_OK) != 0, "/tmp/remote_ui.ota.gz unlinked after OTA worker finished");

    /* Clean up temporary test directory */
    unlink(ver_path);
    rmdir(vdir);
    g_rf_version_dir = "/data/harmony";

    if (g_failures) {
        printf("[-] %d tests failed!\n", g_failures);
        return 1;
    }

    printf("[+] All RF OTA lifecycle tests passed successfully!\n");
    return 0;
}
