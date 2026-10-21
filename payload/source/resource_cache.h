#ifndef CODEX_RESOURCE_CACHE_H
#define CODEX_RESOURCE_CACHE_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns cached in-memory cJSON root of DeviceList.json.
 * Uses mtime check to reload only when the file on disk changes.
 * NOTE: The returned pointer is BORROWED from cache. DO NOT call cJSON_Delete() on it.
 */
cJSON *get_device_list_cached(void);

/*
 * Force invalidation of the cached DeviceList.json.
 * Call this whenever DeviceList.json is written or imported.
 */
void invalidate_device_list_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* CODEX_RESOURCE_CACHE_H */
