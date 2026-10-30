#ifndef CUSTOM_ICON_REGISTRY_H
#define CUSTOM_ICON_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUSTOM_ICON_BASE_ID 0x8001 /* 32769 (high bit set) */

/* Look up or register a unique 16-bit custom icon ID (0x8001..0xFFFF) on the Hub.
   Returns 0 if icon_name is empty or invalid. */
uint16_t custom_icon_registry_get_id(const char *icon_name);

/* Check if custom icon PNG exists and sync to remote over RF if needed.
   Saves on remote as /data/assets/custom/{custom_id}.png.
   Returns 0 on success/already synced, negative on error. */
int custom_icon_registry_sync_remote(uint8_t target_slot, uint16_t custom_id, const char *icon_name);

/* Check if custom icon file exists on Hub filesystem */
bool custom_icon_registry_has_file(const char *icon_name);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_ICON_REGISTRY_H */
