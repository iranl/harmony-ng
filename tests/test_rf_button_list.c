/* rf_build_button_list(): what the config push sends a remote in activity mode (#96) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rf_button_list.h"
#include "rf_proto.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  [PASS] "); } else { printf("  [FAIL] "); failures++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static rf_button_entry_t g_out[RF_BUTTON_LIST_MAX + 8];

/* {"activities": ...} built from a JSON string */
static cJSON *acts(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        printf("bad JSON in test\n");
        exit(2);
    }
    return root;
}

/* an activity with n hard keys mapped to device commands, plus extra members */
static char *hard_activity(const char *id, int n, const char *extra) {
    static char buf[16384];
    int w = snprintf(buf, sizeof(buf), "\"%s\":{", id);
    for (int i = 0; i < n; i++) {
        w += snprintf(buf + w, sizeof(buf) - (size_t)w, "%s\"Key%02d\":{\"type\":\"command\",\"action\":\"device_cmd\",\"deviceId\":\"3\",\"command\":\"C%d\"}",
                      i ? "," : "", i, i);
    }
    if (extra && extra[0]) w += snprintf(buf + w, sizeof(buf) - (size_t)w, "%s%s", n ? "," : "", extra);
    snprintf(buf + w, sizeof(buf) - (size_t)w, "}");
    return buf;
}

static void test_issue96(void) {
    printf("#96: an activity with many hard keys, on-screen buttons for Off / Idle\n");
    char json[20000];
    snprintf(json, sizeof(json), "{%s,\"-1\":{\"screen_buttons\":[{\"label\":\"A\"},{\"label\":\"B\"},{\"label\":\"C\"},{\"label\":\"D\"},{\"label\":\"E\"}]}}",
             hard_activity("1", 50, "\"PwrOff\":{\"type\":\"command\",\"action\":\"activity_stop\"}"));
    cJSON *a = acts(json);
    int n = rf_build_button_list(a, g_out, RF_BUTTON_LIST_MAX);
    CHECK(n == 6, "5 idle buttons and the PwrOff stop key, none of the 50 device-command keys (%d entries)", n);
    CHECK(n == 6 && g_out[0].ctype == 0x00 && g_out[0].atype == 0x03 && strcmp(g_out[0].label, "PwrOff") == 0 && g_out[0].idx == 50,
          "PwrOff first, as a stop key (its position in the activity's map, %d)", n == 6 ? g_out[0].idx : -1);
    int idle = n == 6;
    for (int i = 1; i < 6 && i < n; i++) {
        if (g_out[i].ctype != 0x01 || strcmp(g_out[i].ctx, "-1") != 0 || g_out[i].idx != i - 1) idle = 0;
    }
    CHECK(idle, "then the idle on-screen buttons, under \"-1\", indexed 0..4");
    cJSON_Delete(a);
}

static void test_triggers_with_screen_buttons(void) {
    printf("Start/stop keys of an activity that also has on-screen buttons\n");
    cJSON *a = acts("{\"10\":{\"screen_buttons\":[{\"label\":\"Netflix\"}],"
                    "\"Menu\":{\"type\":\"command\",\"action\":\"device_cmd\",\"deviceId\":\"2\",\"command\":\"Home\"},"
                    "\"Red\":{\"type\":\"command\",\"action\":\"activity_start\",\"activityId\":\"11\"}},"
                    "\"11\":{\"screen_buttons\":[{\"label\":\"Guide\"}]}}");
    int n = rf_build_button_list(a, g_out, RF_BUTTON_LIST_MAX);
    CHECK(n == 4, "two on-screen buttons, the start key, the PwrOff fallback (%d entries)", n);
    CHECK(n == 4 && g_out[0].atype == 0x02 && strcmp(g_out[0].ctx, "11") == 0 && strcmp(g_out[0].label, "Red") == 0,
          "the start key is sent although the activity has on-screen buttons, targeting activity 11");
    CHECK(n == 4 && g_out[1].atype == 0x03 && strcmp(g_out[1].label, "PwrOff") == 0 && strcmp(g_out[1].ctx, "-1") == 0,
          "no PwrOff in the map: the fallback stop key is added");
    CHECK(n == 4 && strcmp(g_out[2].label, "Netflix") == 0 && strcmp(g_out[3].label, "Guide") == 0 && strcmp(g_out[3].ctx, "11") == 0,
          "then the on-screen buttons of both activities, in map order");
    cJSON_Delete(a);
}

static void test_cap(void) {
    printf("The limit (%d)\n", RF_BUTTON_LIST_MAX);
    size_t cap = 400 * 24 + 4096;
    char *json = (char *)malloc(cap);
    int w = snprintf(json, cap, "{\"10\":{\"screen_buttons\":[");
    for (int i = 0; i < 300; i++) w += snprintf(json + w, cap - (size_t)w, "%s{\"label\":\"B%d\"}", i ? "," : "", i);
    snprintf(json + w, cap - (size_t)w, "],\"PwrOff\":{\"action\":\"activity_stop\"}}}");
    cJSON *a = acts(json);
    free(json);
    int n = rf_build_button_list(a, g_out, RF_BUTTON_LIST_MAX);
    int all_screen = 1;
    for (int i = 0; i < n; i++) if (g_out[i].ctype != 0x01) all_screen = 0;
    int screen = 0;
    for (int i = 0; i < n; i++) if (g_out[i].ctype == 0x01) screen++;
    (void)all_screen;
    CHECK(n == RF_BUTTON_LIST_MAX && screen == RF_BUTTON_LIST_MAX - 1 && g_out[0].atype == 0x03 && strcmp(g_out[0].label, "PwrOff") == 0,
          "300 on-screen buttons: %d of them fit, the PwrOff stop key keeps its place (%d entries)", RF_BUTTON_LIST_MAX - 1, n);
    n = rf_build_button_list(a, g_out, 3);
    CHECK(n == 3 && g_out[0].atype == 0x03, "max is honoured, the stop key still in (%d entries)", n);
    cJSON_Delete(a);
    CHECK(RF_BUTTON_LIST_MAX <= 255, "the limit fits the one-byte entry index");
}

static void test_many_triggers(void) {
    printf("More start/stop keys than the remote keeps\n");
    char json[8192];
    int w = snprintf(json, sizeof(json), "{\"10\":{");
    for (int i = 0; i < 20; i++) w += snprintf(json + w, sizeof(json) - (size_t)w, "%s\"K%d\":{\"action\":\"activity_start\",\"activityId\":\"11\"}", i ? "," : "", i);
    snprintf(json + w, sizeof(json) - (size_t)w, ",\"screen_buttons\":[{\"label\":\"X\"}]}}");
    cJSON *a = acts(json);
    int n = rf_build_button_list(a, g_out, RF_BUTTON_LIST_MAX);
    CHECK(n == RF_BUTTON_TRIGGERS_MAX + 2, "%d start keys sent, plus the PwrOff fallback and the on-screen button (%d entries)", RF_BUTTON_TRIGGERS_MAX, n);
    cJSON_Delete(a);
}

static void test_empty(void) {
    printf("No map\n");
    int n = rf_build_button_list(NULL, g_out, RF_BUTTON_LIST_MAX);
    CHECK(n == 1 && strcmp(g_out[0].label, "PwrOff") == 0 && g_out[0].atype == 0x03, "only the PwrOff stop key");
    CHECK(rf_build_button_list(NULL, g_out, 0) == 0, "max 0: nothing");
}

int main(void) {
    test_issue96();
    test_triggers_with_screen_buttons();
    test_cap();
    test_many_triggers();
    test_empty();
    if (failures) {
        printf("test_rf_button_list: %d FAILED\n", failures);
        return 1;
    }
    printf("test_rf_button_list: all passed\n");
    return 0;
}
