/*
 * Unit test of the stock configuration builder (payload/source/hbus_config.c): built from the
 * sample configuration (tests/fixtures/ha/sample_config), the key paths of spec Appendix B.4:
 * activity[].id/label incl. the -1 PowerOff, device[].id/label, controlGroup[].function[].action
 * as a JSON-encoded string with command/type/deviceId, global.timeStampHash, the numeric id
 * substitution and its reverse mapping, and the control group names.
 *
 * Build: gcc -Wall -Wextra -O2 -Ipayload/source tests/test_hbus_config.c payload/source/hbus_config.c \
 *            payload/source/sha256.c payload/source/cJSON.c <config_store stub> -lm
 */
#include "hbus_config.h"
#include "config_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* config_store stub: the sample files, parsed once */
static cJSON *g_devs, *g_acts;
static char *slurp(const char *p) {
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t r = fread(b, 1, (size_t)n, f);
    b[r] = '\0';
    fclose(f);
    return b;
}
cJSON *config_store_get_devices_json(void) { return g_devs; }
cJSON *config_store_get_activities_json(void) { return g_acts; }

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

static const char *s_of(const cJSON *o, const char *k) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);
    return (j && cJSON_IsString(j)) ? j->valuestring : "";
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/fixtures/ha/sample_config";
    char p[512];
    snprintf(p, sizeof(p), "%s/devices.json", dir);
    char *dt = slurp(p);
    snprintf(p, sizeof(p), "%s/activities.json", dir);
    char *at = slurp(p);
    if (!dt || !at) {
        printf("sample config not found under %s\n", dir);
        return 1;
    }
    g_devs = cJSON_Parse(dt);
    g_acts = cJSON_Parse(at);
    printf("hbus_config test\n");

    /* ids */
    char num[32], back[32];
    hbus_numeric_id("123456", num, sizeof(num));
    CHECK(strcmp(num, "123456") == 0, "numeric id passes through");
    hbus_numeric_id("-1", num, sizeof(num));
    CHECK(strcmp(num, "-1") == 0, "-1 passes through");
    hbus_numeric_id("tv-living", num, sizeof(num));
    CHECK(strlen(num) > 0 && strspn(num, "0123456789") == strlen(num), "non-numeric id becomes digits");
    char num2[32];
    hbus_numeric_id("tv-living", num2, sizeof(num2));
    CHECK(strcmp(num, num2) == 0, "substitution is stable");
    hbus_numeric_id("tv-bedroom", num2, sizeof(num2));
    CHECK(strcmp(num, num2) != 0, "different ids differ");
    CHECK(hbus_resolve_id("1", back, sizeof(back)) == 1 && strcmp(back, "1") == 0, "resolve numeric device id");
    CHECK(hbus_resolve_id("10", back, sizeof(back)) == 1 && strcmp(back, "10") == 0, "resolve numeric activity id");
    CHECK(hbus_resolve_id("999999", back, sizeof(back)) == 0 && strcmp(back, "999999") == 0, "unknown id passes through");

    /* control groups */
    CHECK(strcmp(hbus_control_group("PowerOn"), "Power") == 0, "Power group");
    CHECK(strcmp(hbus_control_group("VolumeUp"), "Volume") == 0 && strcmp(hbus_control_group("Mute"), "Volume") == 0, "Volume group");
    CHECK(strcmp(hbus_control_group("ChannelUp"), "Channel") == 0, "Channel group");
    CHECK(strcmp(hbus_control_group("7"), "NumericBasic") == 0, "NumericBasic group");
    CHECK(strcmp(hbus_control_group("Play"), "TransportBasic") == 0, "TransportBasic group");
    CHECK(strcmp(hbus_control_group("DirectionUp"), "NavigationBasic") == 0 && strcmp(hbus_control_group("Select"), "NavigationBasic") == 0, "NavigationBasic group");
    CHECK(strcmp(hbus_control_group("InputHdmi1"), "Miscellaneous") == 0, "Miscellaneous group");

    /* configuration */
    cJSON *cfg = hbus_build_config("1234567", "0a1b2c3d");
    CHECK(cfg != NULL, "config built");
    cJSON *acts = cJSON_GetObjectItemCaseSensitive(cfg, "activity");
    cJSON *devs = cJSON_GetObjectItemCaseSensitive(cfg, "device");
    CHECK(cJSON_GetArraySize(acts) == 3, "2 activities + PowerOff");
    cJSON *a0 = cJSON_GetArrayItem(acts, 0);
    CHECK(strcmp(s_of(a0, "id"), "10") == 0 && strcmp(s_of(a0, "label"), "Watch TV") == 0, "activity id and label");
    cJSON *po = cJSON_GetArrayItem(acts, 2);
    CHECK(strcmp(s_of(po, "id"), "-1") == 0 && strcmp(s_of(po, "label"), "PowerOff") == 0 && strcmp(s_of(po, "type"), "PowerOff") == 0,
          "PowerOff activity last with id -1");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(a0, "isTuningDefault")), "isTuningDefault false");
    CHECK(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(a0, "sequences")) && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(a0, "rules")), "sequences and rules arrays");
    cJSON *fixit = cJSON_GetObjectItemCaseSensitive(a0, "fixit");
    cJSON *fx1 = cJSON_GetObjectItemCaseSensitive(fixit, "1");
    CHECK(fx1 && strcmp(s_of(fx1, "Power"), "On") == 0 && strcmp(s_of(fx1, "isManualPower"), "false") == 0, "fixit entry for the TV");
    /* activity 11 uses devices 1 and 2: its groups carry both devices' commands */
    cJSON *a1 = cJSON_GetArrayItem(acts, 1);
    cJSON *groups = cJSON_GetObjectItemCaseSensitive(a1, "controlGroup");
    int found_home = 0, found_vol = 0;
    cJSON *g = NULL;
    cJSON_ArrayForEach(g, groups) {
        cJSON *f = NULL;
        cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(g, "function")) {
            if (strcmp(s_of(f, "name"), "Home") == 0) {
                cJSON *act = cJSON_Parse(s_of(f, "action"));
                found_home = act && strcmp(s_of(act, "command"), "Home") == 0 && strcmp(s_of(act, "deviceId"), "2") == 0 &&
                             strcmp(s_of(act, "type"), "IRCommand") == 0;
                cJSON_Delete(act);
            }
            if (strcmp(s_of(f, "name"), "VolumeUp") == 0 && strcmp(s_of(g, "name"), "Volume") == 0) {
                cJSON *act = cJSON_Parse(s_of(f, "action"));
                found_vol = act && strcmp(s_of(act, "deviceId"), "1") == 0;
                cJSON_Delete(act);
            }
        }
    }
    CHECK(found_home, "activity group function: Home of device 2 as a JSON string action");
    CHECK(found_vol, "activity group function: VolumeUp of device 1 in the Volume group");
    /* devices */
    CHECK(cJSON_GetArraySize(devs) == 2, "2 devices");
    cJSON *d0 = cJSON_GetArrayItem(devs, 0);
    CHECK(strcmp(s_of(d0, "id"), "1") == 0 && strcmp(s_of(d0, "label"), "Living Room TV") == 0 && strcmp(s_of(d0, "manufacturer"), "Example") == 0, "device id, label, manufacturer");
    CHECK(strcmp(s_of(d0, "isManualPower"), "false") == 0 && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(d0, "IsKeyboardAssociated")) &&
          cJSON_GetObjectItemCaseSensitive(d0, "DongleRFID")->valueint == 0, "device flags");
    int nfun = 0;
    cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(d0, "controlGroup")) nfun += cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(g, "function"));
    CHECK(nfun == 5, "device 1: every command in a group (5)");
    cJSON *glob = cJSON_GetObjectItemCaseSensitive(cfg, "global");
    CHECK(strcmp(s_of(glob, "timeStampHash"), "0a1b2c3d;1234567") == 0 && strcmp(s_of(glob, "locale"), "en-US") == 0, "global.timeStampHash and locale");
    CHECK(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(cfg, "content")) && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(cfg, "sequence")), "content and sequence");
    /* the client converts ids with int(): every id must parse */
    cJSON *it = NULL;
    int ids_ok = 1;
    cJSON_ArrayForEach(it, acts) { char *e; strtol(s_of(it, "id"), &e, 10); if (*e) ids_ok = 0; }
    cJSON_ArrayForEach(it, devs) { char *e; strtol(s_of(it, "id"), &e, 10); if (*e) ids_ok = 0; }
    CHECK(ids_ok, "every id is int()-convertible");
    cJSON_Delete(cfg);

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All hbus_config checks passed\n");
    return 0;
}
