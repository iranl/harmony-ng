/*
 * Unit test for RF OTA lifecycle, manifest verification, and no-flash storage policy:
 * 1. Verifies that rf_migrate_legacy_names() purges stale flash copies from /data/harmony
 *    and /cache.
 * 2. Checks that rf_sync can advertise update flags from remote_ui.version
 *    even when no .ota.gz binary is stored on flash.
 * 3. Verifies that fatal errors unlink /tmp/remote_ui.ota.gz, while transient
 *    failures (asleep remote, low battery) preserve it for resume/retry.
 * 4. Verifies manifest verification edge cases (missing, corrupt, HTML, version mismatch,
 *    SHA-256 mismatch, valid).
 * 5. Verifies pre-staged builds purge stale manifest and skip manifest check.
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
static int s_mock_ack_res = 0;

void hub_led_show(int mode) { (void)mode; }
void hub_led_show_failure(void) {}
void hub_led_restore(void) {}
int rf_set_power_mode(uint8_t mode) { (void)mode; return 0; }
int rf_send_frame(rf_frame_t *f) { (void)f; return 0; }
int rf_send_frame_acked(rf_frame_t *f, int timeout_ms) {
    (void)f; (void)timeout_ms;
    return s_mock_ack_res;
}
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

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
}

int main(void) {
    printf("[*] Starting test_rf_ota_lifecycle...\n");

    /* 1. Verify legacy flash copies purging under mock hardware (/data and /cache) */
    write_file("/data/harmony/remote_ui.ota.gz", "stale_data");
    write_file("/cache/remote_ui.ota.gz", "stale_cache");

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
    write_file("/data/harmony/remote_ui.version", "2.1.0\n");

    /* Verify rf_check_update_flags advertises flag 0x02 even without flash .ota.gz */
    uint8_t flags = rf_check_update_flags(1);
    CHECK((flags & 0x02) != 0, "rf_check_update_flags reports update available (0x02) from remote_ui.version");

    /* 3. Test fatal error cleanup: zero-length file in /tmp */
    unlink("/tmp/remote_ui.ota.gz");
    write_file("/tmp/remote_ui.ota.gz", "");
    CHECK(access("/tmp/remote_ui.ota.gz", F_OK) == 0, "empty /tmp/remote_ui.ota.gz created");

    rf_ota_sender_worker((void *)(uintptr_t)1);
    CHECK(access("/tmp/remote_ui.ota.gz", F_OK) != 0, "invalid/empty /tmp/remote_ui.ota.gz unlinked on size error");

    /* 4. Test manifest verification edge cases via rf_ota_verify_manifest */
    uint8_t test_digest[32];
    memset(test_digest, 0x11, sizeof(test_digest));
    char test_sha_hex[65];
    for (int i = 0; i < 32; i++) snprintf(test_sha_hex + (i * 2), 3, "%02x", test_digest[i]);

    char err_buf[128];

    /* Case 4A: Missing manifest */
    unlink("/tmp/remote_manifest.json");
    bool m_res = rf_ota_verify_manifest(test_digest, err_buf, sizeof(err_buf));
    CHECK(!m_res, "rf_ota_verify_manifest fails when manifest missing");
    CHECK(strstr(err_buf, "missing or invalid") != NULL, "error message indicates missing or invalid manifest");

    /* Case 4B: Truncated / invalid JSON */
    write_file("/tmp/remote_manifest.json", "{\"version\": \"2.1.0\", \"files\": ");
    m_res = rf_ota_verify_manifest(test_digest, err_buf, sizeof(err_buf));
    CHECK(!m_res, "rf_ota_verify_manifest fails on truncated JSON");
    CHECK(access("/tmp/remote_manifest.json", F_OK) != 0, "truncated manifest unlinked");

    /* Case 4C: HTML error page */
    write_file("/tmp/remote_manifest.json", "<html><body>502 Bad Gateway</body></html>");
    m_res = rf_ota_verify_manifest(test_digest, err_buf, sizeof(err_buf));
    CHECK(!m_res, "rf_ota_verify_manifest fails on HTML error page");
    CHECK(access("/tmp/remote_manifest.json", F_OK) != 0, "HTML manifest unlinked");

    /* Case 4D: Missing version and file entry */
    write_file("/tmp/remote_manifest.json", "{\"status\": \"ok\"}");
    m_res = rf_ota_verify_manifest(test_digest, err_buf, sizeof(err_buf));
    CHECK(!m_res, "rf_ota_verify_manifest fails when version/files missing");

    /* Case 4E: Version mismatch against Hub (manifest 2.5.0 vs Hub 2.1.0) */
    char bad_ver_json[512];
    snprintf(bad_ver_json, sizeof(bad_ver_json),
             "{\"version\":\"2.5.0\",\"files\":{\"bin/arm/remote_ui.ota.gz\":{\"sha256\":\"%s\"}}}",
             test_sha_hex);
    write_file("/tmp/remote_manifest.json", bad_ver_json);
    m_res = rf_ota_verify_manifest(test_digest, err_buf, sizeof(err_buf));
    CHECK(!m_res, "rf_ota_verify_manifest fails on version mismatch");
    CHECK(strstr(err_buf, "version mismatch") != NULL, "error message indicates version mismatch");

    /* Case 4F: File entry without sha256 */
    write_file("/tmp/remote_manifest.json", "{\"version\":\"2.1.0\",\"files\":{\"bin/arm/remote_ui.ota.gz\":{}}}");
    m_res = rf_ota_verify_manifest(test_digest, err_buf, sizeof(err_buf));
    CHECK(!m_res, "rf_ota_verify_manifest fails when sha256 missing in file entry");

    /* Case 4G: SHA-256 mismatch */
    char bad_sha_json[512];
    snprintf(bad_sha_json, sizeof(bad_sha_json),
             "{\"version\":\"2.1.0\",\"files\":{\"bin/arm/remote_ui.ota.gz\":{\"sha256\":\"%s\"}}}",
             "0000000000000000000000000000000000000000000000000000000000000000");
    write_file("/tmp/remote_manifest.json", bad_sha_json);
    m_res = rf_ota_verify_manifest(test_digest, err_buf, sizeof(err_buf));
    CHECK(!m_res, "rf_ota_verify_manifest fails on SHA-256 mismatch");
    CHECK(strstr(err_buf, "SHA-256 checksum mismatch") != NULL, "error message indicates SHA mismatch");

    /* Case 4H: Valid manifest with matching version and matching SHA-256 */
    char good_json[512];
    snprintf(good_json, sizeof(good_json),
             "{\"version\":\"2.1.0\",\"files\":{\"bin/arm/remote_ui.ota.gz\":{\"sha256\":\"%s\"}}}",
             test_sha_hex);
    write_file("/tmp/remote_manifest.json", good_json);
    m_res = rf_ota_verify_manifest(test_digest, err_buf, sizeof(err_buf));
    CHECK(m_res, "rf_ota_verify_manifest succeeds on matching version and SHA-256");
    CHECK(access("/tmp/remote_manifest.json", F_OK) != 0, "manifest unlinked after verification");

    /* 5. Test manifest marker behavior when image is present in /tmp */
    /* Case 5A: Image in /tmp + manifest with version mismatch (9.9.9) */
    write_file("/tmp/remote_manifest.json", "{\"version\":\"9.9.9\"}");
    FILE *f_mismatch = fopen("/tmp/remote_ui.ota.gz", "wb");
    assert(f_mismatch != NULL);
    uint8_t dummy_mismatch[512];
    memset(dummy_mismatch, 0xCD, sizeof(dummy_mismatch));
    fwrite(dummy_mismatch, 1, sizeof(dummy_mismatch), f_mismatch);
    fclose(f_mismatch);

    rf_ota_sender_worker((void *)(uintptr_t)1);
    CHECK(access("/tmp/remote_ui.ota.gz", F_OK) != 0, "image in /tmp unlinked when manifest in /tmp has version mismatch");
    CHECK(access("/tmp/remote_manifest.json", F_OK) != 0, "mismatched manifest unlinked");

    /* Case 5B: Image in /tmp + manifest with SHA mismatch */
    write_file("/tmp/remote_manifest.json",
               "{\"version\":\"2.1.0\",\"files\":{\"bin/arm/remote_ui.ota.gz\":{\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}}}");
    FILE *f_shamismatch = fopen("/tmp/remote_ui.ota.gz", "wb");
    assert(f_shamismatch != NULL);
    fwrite(dummy_mismatch, 1, sizeof(dummy_mismatch), f_shamismatch);
    fclose(f_shamismatch);

    rf_ota_sender_worker((void *)(uintptr_t)1);
    CHECK(access("/tmp/remote_ui.ota.gz", F_OK) != 0, "image in /tmp unlinked when manifest in /tmp has SHA mismatch");
    CHECK(access("/tmp/remote_manifest.json", F_OK) != 0, "SHA-mismatched manifest unlinked");

    /* Case 5C: Image in /tmp WITHOUT manifest (staged build via --stage-remote-ota) */
    FILE *f_stage = fopen("/tmp/remote_ui.ota.gz", "wb");
    assert(f_stage != NULL);
    fwrite(dummy_mismatch, 1, sizeof(dummy_mismatch), f_stage);
    fclose(f_stage);
    unlink("/tmp/remote_manifest.json");

    /* Remote asleep: simulate OTA_START ACK timeout (s_mock_ack_res = -1) */
    s_mock_ack_res = -1;
    rf_ota_sender_worker((void *)(uintptr_t)1);

    /* Verify staged image reached OTA_START and was retained on wake timeout */
    CHECK(access("/tmp/remote_ui.ota.gz", F_OK) == 0, "/tmp/remote_ui.ota.gz retained when remote asleep");

    /* 6. Test low battery rejection retention */
    s_mock_ack_res = 0;
    g_ack_status = 0x02; /* Remote rejected OTA: Low battery (<40%) */
    rf_ota_sender_worker((void *)(uintptr_t)1);
    CHECK(access("/tmp/remote_ui.ota.gz", F_OK) == 0, "/tmp/remote_ui.ota.gz retained on low battery rejection");

    /* 7. Clean up */
    unlink("/tmp/remote_ui.ota.gz");
    unlink("/tmp/remote_manifest.json");
    unlink("/data/harmony/remote_ui.version");

    if (g_failures) {
        printf("[-] %d tests failed!\n", g_failures);
        return 1;
    }

    printf("[+] All RF OTA lifecycle tests passed successfully!\n");
    return 0;
}
