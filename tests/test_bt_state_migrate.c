/* Host test of the one-time Bluetooth state migration hub_daemon runs (bt_migrate.c): legacy
 * bt-devices.json and bthid_target folded into config/devices.json and bthid_host_target. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bt_migrate.h"
#include "cJSON.h"

static char g_dir[256];

static void test_log(const char *msg) { printf("    log: %s\n", msg); }

static void path_of(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", g_dir, rel); }

static void put(const char *rel, const char *content) {
    char p[512];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static char *get(const char *rel) {
    char p[512];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    static char buf[16384];
    size_t r = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[r] = '\0';
    return strdup(buf);
}

static int exists(const char *rel) {
    char p[512];
    path_of(p, sizeof(p), rel);
    return access(p, F_OK) == 0;
}

static void reset_dir(void) {
    const char *files[] = {"bt-devices.json", "bt-devices.json.migrated", "bthid_target", "bthid_target.migrated",
                           "bthid_host_target", "config/devices.json", NULL};
    char p[512];
    for (int i = 0; files[i]; i++) {
        path_of(p, sizeof(p), files[i]);
        unlink(p);
    }
    path_of(p, sizeof(p), "config");
    mkdir(p, 0755);
}

/* Host of device id in config/devices.json, "" when it has none, NULL when not found */
static const char *host_of(const char *id) {
    static char out[32];
    char *raw = get("config/devices.json");
    assert(raw);
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    assert(root);
    const char *res = NULL;
    cJSON *d = NULL;
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(root, "devices")) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
        char did[32] = "";
        if (cJSON_IsNumber(jid)) snprintf(did, sizeof(did), "%d", jid->valueint);
        else if (cJSON_IsString(jid)) snprintf(did, sizeof(did), "%s", jid->valuestring);
        if (strcmp(did, id) != 0) continue;
        cJSON *ja = cJSON_GetObjectItemCaseSensitive(d, "bdaddr");
        if (!ja) ja = cJSON_GetObjectItemCaseSensitive(d, "btHost");
        snprintf(out, sizeof(out), "%s", (ja && cJSON_IsString(ja)) ? ja->valuestring : "");
        res = out;
        break;
    }
    cJSON_Delete(root);
    return res;
}

static const char *DEVICES =
    "{\"devices\":[\n"
    " {\"id\":\"1\",\"name\":\"Soundbar\",\"type\":\"IR\",\"commands\":[{\"name\":\"PowerOn\",\"keyCode\":\"G:x\"}]},\n"
    " {\"id\":\"2\",\"name\":\"Shield\",\"type\":\"BT\",\"bdaddr\":\"\",\"btHost\":\"\",\"btAutoReconnect\":1,\"commands\":[]},\n"
    " {\"id\":\"3\",\"name\":\"TV\",\"type\":\"BT\",\"bdaddr\":\"11:22:33:44:55:66\"},\n"
    " {\"id\":\"4\",\"name\":\"Old\",\"type\":\"BT\",\"btHost\":\"22:33:44:55:66:77\"},\n"
    " {\"id\":5,\"name\":\"Numeric\",\"type\":\"BT\"}\n"
    "],\"extra\":{\"keep\":true}}\n";

static const char *LEGACY =
    "{\"version\":1,\"devices\":[\n"
    " {\"id\":\"2\",\"name\":\"Shield\",\"type\":\"btkeyboard\",\"bdaddr\":\"48:B0:2D:04:6F:6E\",\"commands\":[]},\n"
    " {\"id\":\"3\",\"name\":\"TV\",\"bdaddr\":\"AA:AA:AA:AA:AA:AA\"},\n"
    " {\"id\":\"4\",\"name\":\"Old\",\"bdaddr\":\"BB:BB:BB:BB:BB:BB\"},\n"
    " {\"id\":\"5\",\"name\":\"Numeric\",\"bdaddr\":\"CC:CC:CC:CC:CC:CC\"},\n"
    " {\"id\":\"9\",\"name\":\"Gone\",\"bdaddr\":\"DD:DD:DD:DD:DD:DD\"},\n"
    " {\"id\":\"1\",\"name\":\"Bad\",\"bdaddr\":\"not-an-address\"}\n"
    "]}\n";

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/bt_migrate_test_XXXXXX", (tmp && tmp[0]) ? tmp : "/tmp");
    assert(mkdtemp(g_dir));
    char *after;

    printf("[*] case 1: normal migration of bt-devices.json\n");
    reset_dir();
    put("config/devices.json", DEVICES);
    put("bt-devices.json", LEGACY);
    bt_migrate_legacy_state(g_dir, test_log);
    assert(strcmp(host_of("2"), "48:B0:2D:04:6F:6E") == 0);  /* empty bdaddr/btHost filled */
    assert(strcmp(host_of("3"), "11:22:33:44:55:66") == 0);  /* existing host kept */
    assert(strcmp(host_of("4"), "22:33:44:55:66:77") == 0);  /* legacy btHost counts as a host */
    assert(strcmp(host_of("5"), "CC:CC:CC:CC:CC:CC") == 0);  /* numeric id matched */
    assert(strcmp(host_of("1"), "") == 0);                   /* invalid address not applied */
    after = get("config/devices.json");
    assert(strstr(after, "\"keep\"") && strstr(after, "\"PowerOn\"") && strstr(after, "\"btAutoReconnect\""));
    assert(!strstr(after, "\"btHost\":\t\"\""));             /* the empty btHost of device 2 is gone */
    assert(!strstr(after, "\"btHost\""));                    /* device 4's btHost moved to bdaddr */
    assert(!exists("bt-devices.json") && exists("bt-devices.json.migrated"));

    printf("[*] case 2: re-run is a no-op\n");
    bt_migrate_legacy_state(g_dir, test_log);
    char *again = get("config/devices.json");
    assert(strcmp(after, again) == 0);
    free(again);

    printf("[*] case 3: an old backup's bt-devices.json restored later fills only devices without host\n");
    put("bt-devices.json", "{\"devices\":[{\"id\":\"2\",\"bdaddr\":\"EE:EE:EE:EE:EE:EE\"}]}");
    bt_migrate_legacy_state(g_dir, test_log);
    assert(strcmp(host_of("2"), "48:B0:2D:04:6F:6E") == 0);
    again = get("config/devices.json");
    assert(strcmp(after, again) == 0);                       /* nothing to change: file untouched */
    assert(!exists("bt-devices.json"));
    free(again);
    free(after);

    printf("[*] case 4: no legacy files at all\n");
    reset_dir();
    put("config/devices.json", DEVICES);
    bt_migrate_legacy_state(g_dir, test_log);
    after = get("config/devices.json");
    assert(strcmp(after, DEVICES) == 0);
    assert(!exists("bthid_host_target") && !exists("bt-devices.json.migrated"));
    free(after);

    printf("[*] case 5: malformed bt-devices.json is left in place\n");
    reset_dir();
    put("config/devices.json", DEVICES);
    put("bt-devices.json", "{\"devices\":[{\"id\":\"2\",");
    bt_migrate_legacy_state(g_dir, test_log);
    assert(exists("bt-devices.json") && !exists("bt-devices.json.migrated"));
    after = get("config/devices.json");
    assert(strcmp(after, DEVICES) == 0);
    free(after);

    printf("[*] case 6: malformed devices.json: nothing written, bt-devices.json kept\n");
    reset_dir();
    put("config/devices.json", "{\"devices\":[");
    put("bt-devices.json", LEGACY);
    bt_migrate_legacy_state(g_dir, test_log);
    assert(exists("bt-devices.json") && !exists("bt-devices.json.migrated"));
    after = get("config/devices.json");
    assert(strcmp(after, "{\"devices\":[") == 0);
    free(after);

    printf("[*] case 7: devices.json missing: bt-devices.json kept\n");
    reset_dir();
    put("bt-devices.json", LEGACY);
    bt_migrate_legacy_state(g_dir, test_log);
    assert(exists("bt-devices.json") && !exists("config/devices.json"));

    printf("[*] case 8: bt-devices.json without usable entries is retired\n");
    reset_dir();
    put("config/devices.json", DEVICES);
    put("bt-devices.json", "{\"devices\":[]}");
    bt_migrate_legacy_state(g_dir, test_log);
    assert(!exists("bt-devices.json") && exists("bt-devices.json.migrated"));
    after = get("config/devices.json");
    assert(strcmp(after, DEVICES) == 0);
    free(after);

    printf("[*] case 9: bthid_target becomes bthid_host_target\n");
    reset_dir();
    put("bthid_target", "type=btkeyboard\nbdaddr=48:B0:2D:04:6F:6E\n");
    bt_migrate_legacy_state(g_dir, test_log);
    after = get("bthid_host_target");
    assert(after && strcmp(after, "bdaddr=48:B0:2D:04:6F:6E\n") == 0);
    assert(!exists("bthid_target") && exists("bthid_target.migrated"));
    free(after);
    bt_migrate_legacy_state(g_dir, test_log);              /* re-run */
    after = get("bthid_host_target");
    assert(strcmp(after, "bdaddr=48:B0:2D:04:6F:6E\n") == 0);
    free(after);

    printf("[*] case 10: existing bthid_host_target wins\n");
    reset_dir();
    put("bthid_host_target", "bdaddr=11:22:33:44:55:66\n");
    put("bthid_target", "type=btkeyboard\nbdaddr=48:B0:2D:04:6F:6E\n");
    bt_migrate_legacy_state(g_dir, test_log);
    after = get("bthid_host_target");
    assert(strcmp(after, "bdaddr=11:22:33:44:55:66\n") == 0);
    assert(!exists("bthid_target") && exists("bthid_target.migrated"));
    free(after);

    printf("[*] case 11: save_bthid_target() format \"btkeyboard <addr>\"\n");
    reset_dir();
    put("bthid_target", "btkeyboard aa:bb:cc:dd:ee:ff\n");
    bt_migrate_legacy_state(g_dir, test_log);
    after = get("bthid_host_target");
    assert(after && strcmp(after, "bdaddr=aa:bb:cc:dd:ee:ff\n") == 0);
    free(after);

    printf("[*] case 12: bthid_target without an address is left in place\n");
    reset_dir();
    put("bthid_target", "type=btkeyboard\nbdaddr=00:00:00:00:00:00\n");
    bt_migrate_legacy_state(g_dir, test_log);
    assert(!exists("bthid_host_target") && exists("bthid_target"));

    reset_dir();
    char p[512];
    path_of(p, sizeof(p), "config");
    rmdir(p);
    rmdir(g_dir);
    printf("[+] test_bt_state_migrate: all cases passed\n");
    return 0;
}
