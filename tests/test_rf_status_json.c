/*
 * Unit test of hub_rf's status builder (rf_api.c rf_build_status_json): the full form is the
 * GET /api/rf/status object; the stable form (RF_STATUS_STABLE_ONLY) leaves the link counters and
 * the battery voltage out, so it changes only when the status hub_daemon pushes as rf_status did.
 *
 * Build: gcc -Wall -O2 -Ipayload/source tests/test_rf_status_json.c payload/source/rf_state.c \
 *            payload/source/rf_api.c payload/source/cJSON.c -lpthread -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "rf_state.h"
#include "rf_api.h"
#include "cJSON.h"

/* Stubs for the parts of hub_rf that rf_api.c calls (same as test_rf_loopback.c) */
bool is_ota_busy(void) { return false; }
bool is_slot_direct_mode(int slot) { (void)slot; return false; }
const char *get_current_activity(void) { return "-1"; }
void start_pairing_mode(int slot) { (void)slot; }
void stop_pairing_mode(void) {}
void unpair_device(int slot) { (void)slot; }
uint8_t g_tx_seq = 0;
static int g_radio_writes = 0;
void rf_write_cmd(const uint8_t *b, size_t l) { (void)b; (void)l; g_radio_writes++; }
int rf_send_frame(uint8_t pipe, uint8_t msg_type, uint8_t flags, const uint8_t *payload, size_t len) {
    (void)pipe; (void)msg_type; (void)flags; (void)payload; (void)len; return 0;
}
int rf_send_frame_acked(uint8_t pipe, uint8_t msg_type, uint8_t flags, const uint8_t *payload, size_t len, int retries) {
    (void)pipe; (void)msg_type; (void)flags; (void)payload; (void)len; (void)retries; return 0;
}
void rf_send_hub_activity_sync(void) {}
void rf_send_settings_push_slot(uint8_t slot, int bright, int timeout, int touch, int gyro,
                               int nav_key, int nav_mode, int confirm_act, int touch_sens,
                               int remote_mode, int gyro_rate, int btn_backlight,
                               int night_start, int night_end, int usbeth, int no_screen,
                               int cap_buttons, int power_debug, int language, int theme, int layout) {
    (void)slot; (void)bright; (void)timeout; (void)touch; (void)gyro;
    (void)nav_key; (void)nav_mode; (void)confirm_act; (void)touch_sens;
    (void)remote_mode; (void)gyro_rate; (void)btn_backlight;
    (void)night_start; (void)night_end; (void)usbeth; (void)no_screen;
    (void)cap_buttons; (void)power_debug; (void)language;
}
void rf_notify_update_available(int slot) { (void)slot; }
void rf_push_custom_icon(const char *id, const char *type) { (void)id; (void)type; }
void record_config_diff(void) {}
void rf_sync_cache_invalidate(void) {}
int rf_wake_remote(uint8_t slot, int attempts) { (void)slot; (void)attempts; return 0; }
void *config_push_worker(void *arg) { (void)arg; return NULL; }
void *rf_ota_sender_worker(void *arg) { (void)arg; return NULL; }

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

static cJSON *parse(const char *s) {
    cJSON *j = cJSON_Parse(s);
    CHECK(j != NULL, "status is valid JSON");
    return j;
}

int main(void) {
    printf("rf_build_status_json test\n");
    /* A paired Elite in slot 1 */
    g_paired_remotes[0].active = true;
    g_paired_remotes[0].rf_addr[0] = 0xE5; g_paired_remotes[0].rf_addr[1] = 0xF6;
    g_paired_remotes[0].rf_addr[2] = 0x07; g_paired_remotes[0].rf_addr[3] = 0x18;
    g_paired_remotes[0].model = REMOTE_MODEL_ELITE;
    g_paired_remotes[0].last_seen_ms = 1;
    snprintf(g_remote_versions[0].sw_version, sizeof(g_remote_versions[0].sw_version), "2.2.0");
    g_remote_versions[0].config_version = 7;
    g_config_version = 7;
    g_remote_batteries[0].percent = 80;
    g_remote_batteries[0].charging = 0;
    g_remote_batteries[0].voltage_mv = 3950;
    g_remote_link[0].frames_rx = 10;

    char full[3584], stable_a[3584], stable_b[3584];
    int n = rf_build_status_json(full, sizeof(full), 0);
    CHECK(n > 0 && n == (int)strlen(full), "full form built");
    CHECK(g_radio_writes == 0, "no radio query without RF_STATUS_QUERY_RADIO");
    cJSON *j = parse(full);
    if (j) {
        cJSON *slots = cJSON_GetObjectItem(j, "slots");
        cJSON *s1 = cJSON_GetArrayItem(slots, 0);
        cJSON *bat = cJSON_GetObjectItem(s1, "battery");
        cJSON *link = cJSON_GetObjectItem(s1, "link");
        CHECK(cJSON_GetObjectItem(bat, "voltage_mv")->valueint == 3950, "full form has the voltage");
        CHECK(cJSON_GetObjectItem(link, "frames_rx")->valueint == 10, "full form has the link counters");
        CHECK(cJSON_GetObjectItem(s1, "in_sync")->type == cJSON_True, "in_sync when versions match");
        CHECK(cJSON_GetObjectItem(bat, "percent")->valueint == 80, "percent");
        cJSON_Delete(j);
    }

    CHECK(rf_build_status_json(stable_a, sizeof(stable_a), RF_STATUS_STABLE_ONLY) > 0, "stable form built");
    j = parse(stable_a);
    if (j) cJSON_Delete(j);

    /* Link counters and voltage change: stable form identical */
    g_remote_link[0].frames_rx = 500;
    g_remote_link[0].acked_ok = 20;
    g_remote_batteries[0].voltage_mv = 3900;
    CHECK(rf_build_status_json(stable_b, sizeof(stable_b), RF_STATUS_STABLE_ONLY) > 0, "stable form rebuilt");
    CHECK(strcmp(stable_a, stable_b) == 0, "link counters and voltage are not a change");

    /* Battery percent: a change */
    g_remote_batteries[0].percent = 79;
    rf_build_status_json(stable_b, sizeof(stable_b), RF_STATUS_STABLE_ONLY);
    CHECK(strcmp(stable_a, stable_b) != 0, "battery percent is a change");
    strcpy(stable_a, stable_b);

    /* Charging flag, config sync, pairing mode, slot going inactive: changes */
    g_remote_batteries[0].charging = 1;
    rf_build_status_json(stable_b, sizeof(stable_b), RF_STATUS_STABLE_ONLY);
    CHECK(strcmp(stable_a, stable_b) != 0, "charging is a change");
    strcpy(stable_a, stable_b);
    g_config_version = 8;
    rf_build_status_json(stable_b, sizeof(stable_b), RF_STATUS_STABLE_ONLY);
    CHECK(strcmp(stable_a, stable_b) != 0, "hub config version is a change");
    strcpy(stable_a, stable_b);
    g_pairing_mode = true;
    rf_build_status_json(stable_b, sizeof(stable_b), RF_STATUS_STABLE_ONLY);
    CHECK(strcmp(stable_a, stable_b) != 0, "pairing mode is a change");
    strcpy(stable_a, stable_b);
    g_paired_remotes[0].active = false;
    rf_build_status_json(stable_b, sizeof(stable_b), RF_STATUS_STABLE_ONLY);
    CHECK(strcmp(stable_a, stable_b) != 0, "slot active is a change");

    /* Unit id (RF_MSG_UNIT_ID): absent until reported, then in both forms, and a change */
    g_paired_remotes[0].active = true;
    rf_build_status_json(full, sizeof(full), 0);
    CHECK(strstr(full, "remote_id") == NULL, "no remote_id before the remote reported one");
    rf_build_status_json(stable_a, sizeof(stable_a), RF_STATUS_STABLE_ONLY);
    snprintf(g_remote_versions[0].remote_id, sizeof(g_remote_versions[0].remote_id), "20140101120000-000002");
    rf_build_status_json(full, sizeof(full), 0);
    j = parse(full);
    if (j) {
        cJSON *s1 = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "slots"), 0);
        cJSON *rid = cJSON_GetObjectItem(s1, "remote_id");
        CHECK(cJSON_IsString(rid) && strcmp(rid->valuestring, "20140101120000-000002") == 0, "remote_id in slot 1");
        cJSON *s2 = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "slots"), 1);
        CHECK(cJSON_GetObjectItem(s2, "remote_id") == NULL, "slot 2 without a remote_id");
        cJSON_Delete(j);
    }
    rf_build_status_json(stable_b, sizeof(stable_b), RF_STATUS_STABLE_ONLY);
    CHECK(strcmp(stable_a, stable_b) != 0, "a reported remote_id is a change (pushed)");
    char vjson[256];
    CHECK(rf_state_version_json(1, vjson, sizeof(vjson)) > 0 && strstr(vjson, "\"remote_id\":\"20140101120000-000002\"") != NULL,
          "version file JSON carries the remote_id");
    CHECK(rf_state_version_json(2, vjson, sizeof(vjson)) > 0 && strstr(vjson, "remote_id") == NULL,
          "version file JSON without a remote_id when none");
    CHECK(rf_state_version_json(3, vjson, sizeof(vjson)) == -1, "version file JSON rejects slot 3");
    g_remote_versions[0].remote_id[0] = '\0';
    rf_build_status_json(stable_b, sizeof(stable_b), RF_STATUS_STABLE_ONLY);
    CHECK(strcmp(stable_a, stable_b) == 0, "clearing the remote_id (unpair) restores the previous form");

    /* Radio query only when asked and the Hub address is unknown */
    rf_build_status_json(full, sizeof(full), RF_STATUS_QUERY_RADIO);
    CHECK(g_radio_writes == 1, "radio queried with RF_STATUS_QUERY_RADIO and no address");

    /* Too small a buffer fails cleanly */
    char tiny[64];
    CHECK(rf_build_status_json(tiny, sizeof(tiny), 0) == -1, "small buffer gives -1");

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All rf_build_status_json checks passed\n");
    return 0;
}
