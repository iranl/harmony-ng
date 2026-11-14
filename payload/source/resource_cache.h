#ifndef HARMONY_RESOURCE_CACHE_H
#define HARMONY_RESOURCE_CACHE_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns a copy of the in-memory devices.json tree (config_store_devices_copy()), made under the
 * config store lock. The caller owns it and frees it with cJSON_Delete().
 * The tree is loaded from the file at start, after invalidate_device_list_cache() and on every
 * save through the API; there is no check of the file on disk, so a devices.json changed by hand
 * is only seen after POST /api/sync or a restart.
 */
cJSON *get_device_list_copy(void);

/*
 * Force invalidation of the cached devices.json.
 * Call this whenever devices.json is written or imported.
 */
void invalidate_device_list_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RESOURCE_CACHE_H */
