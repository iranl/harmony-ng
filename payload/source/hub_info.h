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
/* ir_capture (bool) and, while the capture mode has a problem, ir_capture_error (spec 3.14).
 * Part of the status fields: /api/status, the snapshot's hub object and MQTT <base>/hub. */
void hub_info_add_ir_capture(cJSON *obj);

/* Seconds since boot (/proc/uptime) */
long hub_info_uptime_sec(void);
/* The boot time as a Unix epoch, 0 while the clock is not set (the Hub starts at 1970 and
 * hub_sntp sets it later). Added to the status object as "boot_time" (null while 0): a client
 * reading a retained or cached object cannot derive it from uptime_sec, which is only right at
 * the moment the object was built. */
long hub_info_boot_time(void);

#ifdef __cplusplus
}
#endif

#endif /* HUB_INFO_H */
