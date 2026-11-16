#ifndef OTA_VERIFY_H
#define OTA_VERIFY_H

#include <stddef.h>

/* Signed OTA manifests.
 *
 * A release publishes manifest.json (version + SHA-256 of every file) and manifest.json.sig:
 * the Ed25519 signature of the exact manifest bytes, as 128 hex characters. The Hub accepts a
 * download only when the signature matches one of the release keys built into the firmware
 * (ota_keys.h) and the file's SHA-256 matches its manifest entry. The download server and TLS
 * are not trusted: busybox wget does not verify certificates. */

#define OTA_SIG_BYTES 64
#define OTA_PUBKEY_BYTES 32

/* Verify sig (64 bytes) over msg with the built-in release keys. Returns 1 when valid. */
int ota_verify_signature(const unsigned char *msg, size_t len, const unsigned char *sig);

/* Parse 128 hex characters (blanks and a trailing newline allowed) into 64 bytes. */
int ota_parse_sig_hex(const char *text, unsigned char sig[OTA_SIG_BYTES]);

/* Read manifest_path and sig_path, verify the signature. On success returns a malloc'd copy
 * of the manifest text (caller frees); NULL with err set otherwise. */
char *ota_load_signed_manifest(const char *manifest_path, const char *sig_path, char *err, size_t err_sz);

/* SHA-256 of a file as 64 lowercase hex characters. Returns 0 on success. */
int ota_file_sha256_hex(const char *path, char out_hex[65]);

/* Manifest "version" without a leading v. Returns 0 on success. */
int ota_manifest_version(const char *manifest_text, char *out, size_t out_sz);

/* Check that the file at path has the SHA-256 of manifest entry files[key]. Returns 0 when it
 * matches; -1 with err set when the entry is missing or the file differs. */
int ota_manifest_check_file(const char *manifest_text, const char *key, const char *path, char *err, size_t err_sz);

/* Number of release keys built in (0 = every manifest is refused). */
int ota_key_count(void);

#endif /* OTA_VERIFY_H */
