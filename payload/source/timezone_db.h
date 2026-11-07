#ifndef TIMEZONE_DB_H
#define TIMEZONE_DB_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TZ_DEFAULT_POSIX "CET-1CEST,M3.5.0,M10.5.0/3"
#define TZ_MAX_RF_PREFIX_LEN 14

/**
 * Resolves an IANA timezone identifier (e.g. "Europe/Amsterdam", "America/New_York")
 * or a POSIX abbreviation/prefix into a full, valid POSIX TZ string.
 *
 * If 'in' is already a valid full POSIX string (e.g. contains DST comma rules or fixed offset),
 * it is sanitized and copied to 'out'.
 *
 * If 'in' is NULL or empty, defaults to TZ_DEFAULT_POSIX.
 */
void timezone_to_posix(const char *in, char *out, size_t out_sz);

/**
 * Extracts a safe prefix for RF transmission in RF_MSG_TIME_SYNC (0x10).
 * Maximum length is TZ_MAX_RF_PREFIX_LEN (14 bytes) to prevent overwriting
 * the update notification flags (byte 19) in the 25-byte RF payload.
 */
void timezone_get_rf_prefix(const char *posix, char *out_prefix, size_t out_sz);

/**
 * Reconstructs a full POSIX TZ string from a received RF prefix on the remote.
 * Expands short prefixes like "CET-1CEST" -> "CET-1CEST,M3.5.0,M10.5.0/3",
 * "EST5EDT" -> "EST5EDT,M3.2.0,M11.1.0", "IST-2IDT" -> "IST-2IDT,M3.4.4/26,M10.5.0".
 */
void timezone_expand_prefix(const char *prefix, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* TIMEZONE_DB_H */
