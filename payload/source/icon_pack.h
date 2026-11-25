#ifndef ICON_PACK_H
#define ICON_PACK_H

#include <stddef.h>

/* Where an archive without the pack index is unpacked (RAM). Test builds pass their own folder,
 * so the suite never touches the host's /tmp/www. */
#ifndef HARMONY_TMP_WWW
#define HARMONY_TMP_WWW "/tmp/www"
#endif

#include <sys/stat.h>

/* Pack version this firmware reads: the version byte of the 'HI' gzip field written by
 * tools/icon_pack.py (EXTRA_VERSION there; tests/test_hub_route_checks.py checks both match) */
#define ICON_PACK_VERSION 1

/* What the stock icon archive is (icon_pack_archive, icon_pack_file_kind) */
#define ICON_ARCHIVE_NONE 0       /* no archive */
#define ICON_ARCHIVE_PLAIN 1      /* a tar.gz without the pack header: callers unpack it into /tmp */
#define ICON_ARCHIVE_PACK 2       /* a pack of this version; its index may not be readable yet (a
                                   * copy in progress, cut): unpacking it into /tmp cannot help */
#define ICON_ARCHIVE_PACK_OTHER 3 /* a pack of another version (after a downgrade): still a valid
                                   * tar.gz; callers unpack it only when the RAM allows it */

/* Kind of the file at path; *pack_version (may be NULL) is the version byte of a pack, -1 for
 * the others. The pack header is the whole fixed layout of tools/icon_pack.py (gzip FEXTRA only,
 * XLEN 24, one 'HI' subfield of 20 bytes): any other gzip file is a plain archive. */
int icon_pack_file_kind(const char *path, int *pack_version);

/* The stock icon archive icon_pack reads: the first regular file of ICON_PACK_ARCHIVES. Fills
 * path (len bytes), *st and *pack_version (each may be NULL) and returns its kind, so callers
 * that unpack it look at the same file icon_pack does. */
int icon_pack_archive(char *path, size_t len, struct stat *st, int *pack_version);

/* The stock icon archive in the packed form of tools/icon_pack.py: icons are read one at a time
 * from flash instead of being unpacked into /tmp (RAM). An archive without the pack index (older
 * deploy) or of another pack version is not used here: callers unpack it into /tmp as before
 * (another pack version only when the RAM allows it). The archive is looked at
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
