#ifndef ICON_PACK_H
#define ICON_PACK_H

#include <stddef.h>

/* The stock icon archive in the packed form of tools/icon_pack.py: icons are read one at a time
 * from flash instead of being unpacked into /tmp (RAM). An archive without the pack index (older
 * deploy, OTA) is not used here: callers unpack it into /tmp as before. The archive is looked at
 * (stat) at every call and read again when it changed. */

/* 1 when the stock icon archive is packed and its index could be read */
int icon_pack_available(void);

/* 1 when name ("tv.png", or "hub/wifi.png" below icons/) is in the packed archive */
int icon_pack_has(const char *name);

/* The icon's bytes (malloc, free them), *len set; NULL when not there or unreadable */
unsigned char *icon_pack_read(const char *name, size_t *len);

/* Calls cb for every icon name without a folder part, in name order. cb runs under the pack
 * lock: it must not call the icon_pack functions. */
void icon_pack_list(void (*cb)(const char *name, void *ctx), void *ctx);

#endif
