#include "hub_info.h"
#include "mfg_id.h"
#include "hub_network_util.h"
#include "harmony_version.h"
#include "ir_i2s.h"

#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char g_hub_id[MFG_UNITID_MAX] = "unknown";
static int g_hub_id_fallback = 1;

/* Flags added by each item of the spec as it lands (2.2) */
static const char *const k_capabilities[] = {
    "events",
    "snapshot",
    "ws_auth",
    "ws_keepalive",
    "ir_raw",
    "ir_capture",
    "mdns",
    "legacy",
    "mqtt_v2",
    "rf_wake",
    NULL
};

static int mac_fallback(char *out, size_t out_len) {
    char ip[32] = {0}, mask[32] = {0}, mac[32] = {0};
    net_get_iface_ip_mask_mac("ath0", ip, sizeof(ip), mask, sizeof(mask), mac, sizeof(mac));
    size_t n = 0;
    for (const char *p = mac; *p && n + 1 < out_len; p++) {
        if (isxdigit((unsigned char)*p)) out[n++] = (char)tolower((unsigned char)*p);
    }
    out[n] = '\0';
    return (n == 12) ? 0 : -1;
}

void hub_info_init(const char *mfg_path) {
    const char *path = (mfg_path && mfg_path[0]) ? mfg_path : getenv("HARMONY_MFG_FILE");
    if (mfg_read_unitid(path, g_hub_id, sizeof(g_hub_id)) == 0) {
        g_hub_id_fallback = 0;
        printf("[+] Hub id %s (mfg partition)\n", g_hub_id);
        return;
    }
    g_hub_id_fallback = 1;
    if (mac_fallback(g_hub_id, sizeof(g_hub_id)) == 0) {
        fprintf(stderr, "[!] No unit id in the mfg partition: hub_id falls back to the ath0 MAC %s\n", g_hub_id);
    } else {
        snprintf(g_hub_id, sizeof(g_hub_id), "unknown");
        fprintf(stderr, "[!] No unit id in the mfg partition and no ath0 MAC: hub_id is \"unknown\"\n");
    }
}

const char *hub_info_id(void) {
    return g_hub_id;
}

int hub_info_id_is_fallback(void) {
    return g_hub_id_fallback;
}

cJSON *hub_info_capabilities(void) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (int i = 0; k_capabilities[i]; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(k_capabilities[i]));
    }
    return arr;
}

/* Before this the clock has not been set: 2024-01-01 */
#define HUB_CLOCK_VALID_EPOCH 1704067200L

long hub_info_uptime_sec(void) {
    double up = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) != 1) up = 0;
        fclose(f);
    }
    return (long)up;
}

long hub_info_boot_time(void) {
#ifdef TEST_HARNESS
    /* the harness runs on a host whose clock is set; this file simulates a Hub before SNTP */
    const char *unset = getenv("HARMONY_TEST_CLOCK_UNSET_FILE");
    if (unset && unset[0] && access(unset, F_OK) == 0) return 0;
#endif
    long now = (long)time(NULL);
    if (now < HUB_CLOCK_VALID_EPOCH) return 0;
    return now - hub_info_uptime_sec();
}

void hub_info_add_status_fields(cJSON *obj, const struct hub_settings *settings) {
    if (!obj) return;
    cJSON_AddStringToObject(obj, "hub_id", g_hub_id);
    cJSON_AddStringToObject(obj, "version", HARMONY_VERSION);
    cJSON_AddStringToObject(obj, "commit", HARMONY_COMMIT);
    cJSON_AddNumberToObject(obj, "api_version", HUB_API_VERSION);
    cJSON *caps = hub_info_capabilities();
    if (caps) cJSON_AddItemToObject(obj, "capabilities", caps);
    cJSON_AddBoolToObject(obj, "legacy_enabled", settings ? (settings->legacy_enabled ? 1 : 0) : 0);
    cJSON_AddBoolToObject(obj, "mdns_enabled", settings ? (settings->mdns_enabled ? 1 : 0) : 1);
    long boot = hub_info_boot_time();
    if (boot > 0) cJSON_AddNumberToObject(obj, "boot_time", (double)boot);
    else cJSON_AddNullToObject(obj, "boot_time");
    hub_info_add_ir_capture(obj);
}

void hub_info_add_ir_capture(cJSON *obj) {
    if (!obj) return;
    cJSON_AddBoolToObject(obj, "ir_capture", ir_i2s_capture_mode_enabled() ? 1 : 0);
    char err[160];
    if (ir_i2s_capture_mode_error(err, sizeof(err))) cJSON_AddStringToObject(obj, "ir_capture_error", err);
}
