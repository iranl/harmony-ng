#ifndef HUB_INFO_H
#define HUB_INFO_H

/* Hub identity (factory unit id), API version and capability flags, see
 * docs/HA_INTEGRATION_FIRMWARE_SPEC.md 2.1, 2.2 and 3.6. */

#include "cJSON.h"
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_API_VERSION 2

/* Reads the unit id once (mfg partition, or mfg_path / HARMONY_MFG_FILE for the harness);
 * falls back to the ath0 MAC with a warning. */
void hub_info_init(const char *mfg_path);
const char *hub_info_id(void);
int hub_info_id_is_fallback(void);
/* New JSON array of the capability flags of this firmware */
cJSON *hub_info_capabilities(void);
/* Adds hub_id, version, commit, api_version, capabilities, legacy_enabled and mdns_enabled
 * (from the settings; NULL = defaults) */
void hub_info_add_status_fields(cJSON *obj, const struct hub_settings *settings);

#ifdef __cplusplus
}
#endif

#endif /* HUB_INFO_H */
