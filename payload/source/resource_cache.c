#include "resource_cache.h"
#include "config_store.h"

const cJSON *get_device_list_acquire(struct cfg_snap **snap) {
    return config_store_devices_acquire(snap);
}

void release_device_list(struct cfg_snap **snap) {
    config_store_release(snap);
}

void invalidate_device_list_cache(void) {
    config_store_invalidate_cache();
}
