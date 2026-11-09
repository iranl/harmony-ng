/*
 * The last battery report per slot survives a hub_rf restart (rf_state.c, rf_api.c): it is kept
 * in a RAM file, restored at start with "restored": true and its "reported_at", and a report
 * made before the clock was set gets its reported_at once it is.
 *
 * Build: gcc -Wall -O2 -Ipayload/source -Itests tests/test_rf_battery.c payload/source/rf_state.c \
 *            payload/source/rf_api.c payload/source/cJSON.c -lpthread -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rf_state.h"
#include "rf_proto.h"
#include "rf_api.h"
#include "cJSON.h"
#include "rf_api_stubs.h"

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

static time_t g_fake_now;
static time_t fake_time(time_t *t) { if (t) *t = g_fake_now; return g_fake_now; }

static cJSON *battery_json(int slot) {
    static char body[4096];
    if (rf_build_status_json(body, sizeof(body), 0) <= 0) return NULL;
    cJSON *root = cJSON_Parse(body);
    if (!root) return NULL;
    cJSON *s = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "slots"), slot - 1);
    cJSON *b = s ? cJSON_DetachItemFromObject(s, "battery") : NULL;
    cJSON_Delete(root);
    return b;
}

static void forget_memory(void) {
    memset(g_remote_batteries, 0, sizeof(g_remote_batteries)); /* a new hub_rf process */
}

int main(void) {
    printf("[*] RF battery restore tests...\n");
    char dir[] = "/tmp/rf_battery_test_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    g_rf_battery_cache_dir = dir;
    g_rf_time_fn = fake_time;
    g_fake_now = 1794000000;

    /* 1. never reported: the placeholder, reported_at null, not restored */
    forget_memory();
    rf_battery_restore();
    cJSON *b = battery_json(1);
    CHECK(b && cJSON_IsNull(cJSON_GetObjectItem(b, "reported_at")), "no report: reported_at null");
    CHECK(b && cJSON_IsFalse(cJSON_GetObjectItem(b, "restored")), "no report: not restored");
    CHECK(b && cJSON_GetObjectItem(b, "percent")->valueint == 0, "no report: the 0 placeholder");
    cJSON_Delete(b);

    /* 2. a report: values, reported_at, not restored, and a cache file */
    rf_battery_note_report(1, 62, 0, 3835);
    b = battery_json(1);
    CHECK(b && cJSON_GetObjectItem(b, "percent")->valueint == 62, "report: percent");
    CHECK(b && cJSON_GetObjectItem(b, "voltage_mv")->valueint == 3835, "report: voltage");
    CHECK(b && cJSON_GetObjectItem(b, "reported_at")->valuedouble == 1794000000.0, "report: reported_at");
    CHECK(b && cJSON_IsFalse(cJSON_GetObjectItem(b, "restored")), "report: not restored");
    cJSON_Delete(b);
    char path[256];
    snprintf(path, sizeof(path), "%s/hub_rf_battery_slot1.json", dir);
    CHECK(access(path, F_OK) == 0, "the report is cached in RAM (/tmp)");

    /* 3. hub_rf restarts: the value comes back, marked restored, with its reported_at */
    forget_memory();
    g_fake_now = 1794000300;
    rf_battery_restore();
    b = battery_json(1);
    CHECK(b && cJSON_GetObjectItem(b, "percent")->valueint == 62, "restart: percent restored");
    CHECK(b && cJSON_GetObjectItem(b, "voltage_mv")->valueint == 3835, "restart: voltage restored");
    CHECK(b && cJSON_IsFalse(cJSON_GetObjectItem(b, "charging")), "restart: charging restored");
    CHECK(b && cJSON_GetObjectItem(b, "reported_at")->valuedouble == 1794000000.0, "restart: the original reported_at");
    CHECK(b && cJSON_IsTrue(cJSON_GetObjectItem(b, "restored")), "restart: marked restored");
    cJSON_Delete(b);
    b = battery_json(2);
    CHECK(b && cJSON_IsNull(cJSON_GetObjectItem(b, "reported_at")) && cJSON_IsFalse(cJSON_GetObjectItem(b, "restored")),
          "restart: a slot that never reported stays unknown");
    cJSON_Delete(b);

    /* 4. the next report replaces it and clears the mark */
    rf_battery_note_report(1, 61, 1, 4010);
    b = battery_json(1);
    CHECK(b && cJSON_GetObjectItem(b, "percent")->valueint == 61 && cJSON_IsTrue(cJSON_GetObjectItem(b, "charging")),
          "new report replaces the restored value");
    CHECK(b && cJSON_IsFalse(cJSON_GetObjectItem(b, "restored")), "new report: not restored");
    CHECK(b && cJSON_GetObjectItem(b, "reported_at")->valuedouble == 1794000300.0, "new report: its reported_at");
    cJSON_Delete(b);

    /* 5. a report before the clock is set (after a reboot): reported_at null, filled once the
     *    clock is set, from the report's age */
    forget_memory();
    g_fake_now = 63; /* 1970: hub_sntp has not run yet */
    rf_battery_note_report(2, 55, 0, 3790);
    b = battery_json(2);
    CHECK(b && cJSON_IsNull(cJSON_GetObjectItem(b, "reported_at")), "clock not set: reported_at null");
    cJSON_Delete(b);
    CHECK(rf_battery_fix_times() == 0, "nothing to fix while the clock is not set");
    g_fake_now = 1794000600;
    CHECK(rf_battery_fix_times() == 1, "the clock is set: the report gets its time");
    b = battery_json(2);
    double ra = b ? cJSON_GetObjectItem(b, "reported_at")->valuedouble : 0;
    CHECK(ra >= 1794000598.0 && ra <= 1794000600.0, "reported_at = now minus the report's age");
    cJSON_Delete(b);
    CHECK(rf_battery_fix_times() == 0, "fixed once");
    /* and the cache file got it too: a restart restores the real time */
    forget_memory();
    rf_battery_restore();
    b = battery_json(2);
    CHECK(b && cJSON_GetObjectItem(b, "reported_at")->valuedouble == ra, "the fixed time is in the cache");
    cJSON_Delete(b);

    /* 6. a cache from another boot (monotonic time ahead of now) is ignored */
    snprintf(path, sizeof(path), "%s/hub_rf_battery_slot1.json", dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "{\"percent\":99,\"charging\":false,\"voltage_mv\":3950,\"reported_at\":1794000000,\"mono_ms\":%llu}\n",
            (unsigned long long)(get_monotonic_ms() + 86400000ULL));
    fclose(f);
    forget_memory();
    rf_battery_restore();
    b = battery_json(1);
    CHECK(b && cJSON_GetObjectItem(b, "percent")->valueint == 0 && cJSON_IsFalse(cJSON_GetObjectItem(b, "restored")),
          "a cache from another boot is not restored");
    cJSON_Delete(b);

    /* 7. the stable key (what hub_rf compares before pushing) carries reported_at: every
     *    report is one rf_status event, the voltage alone is not */
    static char k1[4096], k2[4096];
    forget_memory();
    rf_battery_note_report(1, 50, 0, 3750);
    rf_build_status_json(k1, sizeof(k1), RF_STATUS_STABLE_ONLY);
    g_remote_batteries[0].voltage_mv = 3740;
    rf_build_status_json(k2, sizeof(k2), RF_STATUS_STABLE_ONLY);
    CHECK(strcmp(k1, k2) == 0, "a voltage change alone is not a change");
    g_fake_now += 300;
    rf_battery_note_report(1, 50, 0, 3740);
    rf_build_status_json(k2, sizeof(k2), RF_STATUS_STABLE_ONLY);
    CHECK(strcmp(k1, k2) != 0, "a new report is a change");

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* nothing to do */ }
    if (g_failures) { printf("[-] %d RF battery checks failed\n", g_failures); return 1; }
    printf("[+] All RF battery restore checks passed\n");
    return 0;
}
