#ifndef HARMONY_RESOURCE_CACHE_H
#define HARMONY_RESOURCE_CACHE_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns cached in-memory cJSON root of devices.json.
 * Uses mtime check to reload only when the file on disk changes.
 * NOTE: The returned pointer is BORROWED from cache. DO NOT call cJSON_Delete() on it.
 */
cJSON *get_device_list_cached(void);

/*
 * Force invalidation of the cached devices.json.
 * Call this whenever devices.json is written or imported.
 */
void invalidate_device_list_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RESOURCE_CACHE_H */
