#ifndef MFG_ID_H
#define MFG_ID_H

/*
 * Factory unit id from the "mfg" MTD partition (Hub and remotes alike).
 *
 * Layout, verified on a Hub (mtd6) and a Harmony Ultimate (mtd9): at offset 0x9000 the 8-byte
 * magic "harmony!", then records of a 4-byte big-endian length and that many bytes, alternating
 * key and value: "unitid" (21 characters: 14 digits, a dash, 6 digits), "arch", "skin",
 * "usb_vendor_id", "usb_product_id", "hw_ver", "key", then a certificate. The parser scans the
 * records for the key and stops at the first record with an implausible length.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFG_MAGIC        "harmony!"
#define MFG_MAGIC_LEN    8
#define MFG_RECORDS_OFF  0x9000
#define MFG_UNITID_MAX   32   /* buffer size for a unit id (21 characters today) */

/* Finds "unitid" in a blob of the partition (the magic at MFG_RECORDS_OFF, or at 0 for a blob
 * that holds only the record area). Writes the trimmed value to out. Returns 0, or -1 when the
 * magic, the key or a plausible record chain is missing. */
int mfg_parse_unitid(const uint8_t *blob, size_t len, char *out, size_t out_len);

/* Reads the unit id from the mfg partition (found by name in /proc/mtd), or from override_path
 * when given (a file with the same layout, for the host harness). Returns 0 or -1. */
int mfg_read_unitid(const char *override_path, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* MFG_ID_H */
