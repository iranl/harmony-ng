#include "sha256.h"
#include <string.h>
#include <stdio.h>

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SIGMA0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define SIGMA1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define sigma0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define sigma1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t W[64];
    for (int t = 0; t < 16; t++) {
        W[t] = ((uint32_t)block[t * 4] << 24) |
               ((uint32_t)block[t * 4 + 1] << 16) |
               ((uint32_t)block[t * 4 + 2] << 8) |
               ((uint32_t)block[t * 4 + 3]);
    }
    for (int t = 16; t < 64; t++) {
        W[t] = sigma1(W[t - 2]) + W[t - 7] + sigma0(W[t - 15]) + W[t - 16];
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int t = 0; t < 64; t++) {
        uint32_t T1 = h + SIGMA1(e) + CH(e, f, g) + K[t] + W[t];
        uint32_t T2 = SIGMA0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx) {
    if (!ctx) return;
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    if (!ctx || !data || len == 0) return;
    size_t buffer_idx = (size_t)(ctx->count & 0x3F);
    ctx->count += (uint64_t)len;

    size_t space = 64 - buffer_idx;
    if (len >= space) {
        memcpy(&ctx->buffer[buffer_idx], data, space);
        sha256_transform(ctx->state, ctx->buffer);
        size_t offset = space;
        while (offset + 64 <= len) {
            sha256_transform(ctx->state, &data[offset]);
            offset += 64;
        }
        buffer_idx = 0;
        data += offset;
        len -= offset;
    }

    if (len > 0) {
        memcpy(&ctx->buffer[buffer_idx], data, len);
    }
}

void sha256_final(uint8_t digest[32], sha256_ctx_t *ctx) {
    if (!ctx || !digest) return;
    uint64_t total_bits = ctx->count * 8;
    size_t buffer_idx = (size_t)(ctx->count & 0x3F);

    ctx->buffer[buffer_idx++] = 0x80;
    if (buffer_idx > 56) {
        memset(&ctx->buffer[buffer_idx], 0, 64 - buffer_idx);
        sha256_transform(ctx->state, ctx->buffer);
        buffer_idx = 0;
    }
    memset(&ctx->buffer[buffer_idx], 0, 56 - buffer_idx);

    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)((total_bits >> ((7 - i) * 8)) & 0xFF);
    }
    sha256_transform(ctx->state, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)((ctx->state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8)  & 0xFF);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFF);
    }
}

void sha256_hash(const uint8_t *data, size_t len, uint8_t digest[32]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(digest, &ctx);
}

void sha256_hex(const uint8_t *data, size_t len, char hex_out[65]) {
    uint8_t digest[32];
    sha256_hash(data, len, digest);
    for (int i = 0; i < 32; i++) {
        snprintf(hex_out + (i * 2), 3, "%02x", digest[i]);
    }
    hex_out[64] = '\0';
}

void sha256_hmac(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));

    if (key_len > 64) {
        sha256_hash(key, key_len, k);
    } else if (key && key_len > 0) {
        memcpy(k, key, key_len);
    }

    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    for (int i = 0; i < 64; i++) {
        k_ipad[i] = k[i] ^ 0x36;
        k_opad[i] = k[i] ^ 0x5c;
    }

    uint8_t inner_hash[32];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, 64);
    if (msg && msg_len > 0) sha256_update(&ctx, msg, msg_len);
    sha256_final(inner_hash, &ctx);

    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, 64);
    sha256_update(&ctx, inner_hash, 32);
    sha256_final(out, &ctx);
}

void sha256_hmac_hex(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, char hex_out[65]) {
    uint8_t out[32];
    sha256_hmac(key, key_len, msg, msg_len, out);
    for (int i = 0; i < 32; i++) {
        snprintf(hex_out + (i * 2), 3, "%02x", out[i]);
    }
    hex_out[64] = '\0';
}
