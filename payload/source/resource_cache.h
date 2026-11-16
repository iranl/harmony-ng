#ifndef HARMONY_RESOURCE_CACHE_H
#define HARMONY_RESOURCE_CACHE_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cfg_snap;

/*
 * The in-memory devices.json tree, shared read only (config_store_devices_acquire()): no copy,
 * and a save meanwhile does not change it. Give it back with release_device_list(&snap) on every
 * path; NULL when out of memory.
 * The tree is loaded from the file at start, after invalidate_device_list_cache() and on every
 * save through the API; there is no check of the file on disk, so a devices.json changed by hand
 * is only seen after POST /api/sync or a restart.
 */
const cJSON *get_device_list_acquire(struct cfg_snap **snap);
void release_device_list(struct cfg_snap **snap);

/*
 * Force invalidation of the cached devices.json.
 * Call this whenever devices.json is written or imported.
 */
void invalidate_device_list_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RESOURCE_CACHE_H */
