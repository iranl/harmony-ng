#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final(uint8_t digest[32], sha256_ctx_t *ctx);

/* Convenience helpers */
void sha256_hash(const uint8_t *data, size_t len, uint8_t digest[32]);
void sha256_hex(const uint8_t *data, size_t len, char hex_out[65]);

/* HMAC-SHA256 (RFC 2104) */
void sha256_hmac(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]);
void sha256_hmac_hex(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, char hex_out[65]);

#ifdef __cplusplus
}
#endif

#endif /* SHA256_H */
