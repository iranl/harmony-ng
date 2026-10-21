#include "resource_cache.h"
#include "config_store.h"

cJSON *get_device_list_cached(void) {
    return config_store_get_devices_json();
}

void invalidate_device_list_cache(void) {
    config_store_invalidate_cache();
}
