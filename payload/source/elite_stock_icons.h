#ifndef ELITE_STOCK_ICONS_H
#define ELITE_STOCK_ICONS_H

#include <stdint.h>
#include <stddef.h>

#define TOTAL_STOCK_ICONS 909

/* Return stock ROM path on Elite remote for stock_id (1..TOTAL_STOCK_ICONS), or NULL */
const char *get_stock_icon_path(uint16_t stock_id);

/* Look up stock icon ID (1..TOTAL_STOCK_ICONS) from icon string/GUID/alias/filename.
   Returns 0 if not a recognized stock icon. */
uint16_t lookup_stock_icon_id(const char *name_or_guid);

#endif /* ELITE_STOCK_ICONS_H */
