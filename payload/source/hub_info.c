#include "hub_info.h"
#include "mfg_id.h"
#include "hub_network_util.h"
#include "harmony_version.h"

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

void hub_info_add_status_fields(cJSON *obj) {
    if (!obj) return;
    cJSON_AddStringToObject(obj, "hub_id", g_hub_id);
    cJSON_AddStringToObject(obj, "version", HARMONY_VERSION);
    cJSON_AddStringToObject(obj, "commit", HARMONY_COMMIT);
    cJSON_AddNumberToObject(obj, "api_version", HUB_API_VERSION);
    cJSON *caps = hub_info_capabilities();
    if (caps) cJSON_AddItemToObject(obj, "capabilities", caps);
    /* Settings of milestones 2 and 4; false until they exist */
    cJSON_AddBoolToObject(obj, "legacy_enabled", 0);
    cJSON_AddBoolToObject(obj, "mdns_enabled", 0);
}
