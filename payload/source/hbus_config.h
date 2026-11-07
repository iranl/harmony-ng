#ifndef HBUS_CONFIG_H
#define HBUS_CONFIG_H

/*
 * Stock Logitech configuration object for the protocol shim (spec 6.5, Appendix B.4): what
 * aioharmony and Home Assistant's core harmony integration parse. Built from the daemon's
 * devices.json and activities.json on every request, never cached.
 */

#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The daemon's ids are numeric strings when migrated from stock; a non-numeric id is replaced
 * by a stable numeric one (CRC32 of the id, as a string). */
void hbus_numeric_id(const char *id, char *out, size_t cap);
/* Maps a numeric id back to the daemon's id by scanning devices and activities (out = numeric
 * when no mapping is needed). Returns 1 when found. */
int hbus_resolve_id(const char *numeric, char *out, size_t cap);

/* First 8 hex digits of the SHA-256 over devices.json and activities.json */
void hbus_inventory_version(char out[9]);

/* The configuration object (activity, device, global, content, sequence). remote_id is the
 * activeRemoteId; version the inventory version for global.timeStampHash. */
cJSON *hbus_build_config(const char *remote_id, const char *inventory_version);

/* Control group name for a command name (Power, Volume, Channel, NumericBasic, TransportBasic,
 * NavigationBasic, Miscellaneous) */
const char *hbus_control_group(const char *command);

#ifdef __cplusplus
}
#endif

#endif /* HBUS_CONFIG_H */
