#include "resource_cache.h"
#include "config_store.h"

cJSON *get_device_list_copy(void) {
    return config_store_devices_copy();
}

void invalidate_device_list_cache(void) {
    config_store_invalidate_cache();
}
