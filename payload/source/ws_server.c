#define _GNU_SOURCE
#include "ws_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static int g_clients[WS_MAX_CLIENTS];
static size_t g_client_count = 0;

/* --- Standalone SHA-1 Engine (FIPS 180-1) --- */
typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} sha1_ctx_t;

#define ROL32(val, bits) (((val) << (bits)) | ((val) >> (32 - (bits))))

static void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    uint32_t w[80];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)buffer[i * 4] << 24) |
               ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8) |
               ((uint32_t)buffer[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = ROL32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL32(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count[0] = ctx->count[1] = 0;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t i = 0, j = (ctx->count[0] >> 3) & 63;
    if ((ctx->count[0] += (uint32_t)(len << 3)) < (uint32_t)(len << 3)) {
        ctx->count[1]++;
    }
    ctx->count[1] += (uint32_t)(len >> 29);
    if ((j + len) > 63) {
        memcpy(&ctx->buffer[j], data, (i = 64 - j));
        sha1_transform(ctx->state, ctx->buffer);
        for (; i + 63 < len; i += 64) {
            sha1_transform(ctx->state, &data[i]);
        }
        j = 0;
    }
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

static void sha1_final(uint8_t digest[20], sha1_ctx_t *ctx) {
    uint8_t finalcount[8];
    for (int i = 0; i < 8; i++) {
        finalcount[i] = (uint8_t)((ctx->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    }
    sha1_update(ctx, (const uint8_t *)"\x80", 1);
    while ((ctx->count[0] & 504) != 448) {
        sha1_update(ctx, (const uint8_t *)"\x00", 1);
    }
    sha1_update(ctx, finalcount, 8);
    for (int i = 0; i < 20; i++) {
        digest[i] = (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
}

/* --- Base64 Encoding --- */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *src, size_t len, char *out) {
    size_t i = 0, j = 0;
    while (i < len) {
        size_t rem = len - i;
        uint32_t oct_a = src[i++];
        uint32_t oct_b = (rem > 1) ? src[i++] : 0;
        uint32_t oct_c = (rem > 2) ? src[i++] : 0;
        uint32_t triple = (oct_a << 16) | (oct_b << 8) | oct_c;

        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (rem > 1) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out[j++] = (rem > 2) ? b64_table[triple & 0x3F] : '=';
    }
    out[j] = '\0';
}

int ws_server_init(void) {
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) g_clients[i] = -1;
    g_client_count = 0;
    return 0;
}

int ws_is_upgrade_req(const char *http_req) {
    if (!http_req) return 0;
    return (strcasestr(http_req, "Upgrade: websocket") != NULL ||
            strcasestr(http_req, "Upgrade: WebSocket") != NULL);
}

static void add_client(int fd) {
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] < 0) {
            g_clients[i] = fd;
            g_client_count++;
            return;
        }
    }
}

void ws_remove_client(int client_fd) {
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == client_fd) {
            g_clients[i] = -1;
            if (g_client_count > 0) g_client_count--;
            break;
        }
    }
    close(client_fd);
}

void ws_close_all(void) {
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] >= 0) {
            close(g_clients[i]);
            g_clients[i] = -1;
        }
    }
    g_client_count = 0;
}

int ws_handle_upgrade(int client_fd, const char *http_req) {
    const char *key_hdr = strcasestr(http_req, "Sec-WebSocket-Key: ");
    if (!key_hdr) return -1;
    key_hdr += 19;

    char client_key[64] = {0};
    size_t ki = 0;
    while (*key_hdr && *key_hdr != '\r' && *key_hdr != '\n' && ki < sizeof(client_key) - 1) {
        client_key[ki++] = *key_hdr++;
    }

    char concat[128];
    snprintf(concat, sizeof(concat), "%s%s", client_key, WS_GUID);

    uint8_t digest[20];
    sha1_ctx_t sctx;
    sha1_init(&sctx);
    sha1_update(&sctx, (const uint8_t *)concat, strlen(concat));
    sha1_final(digest, &sctx);

    char accept_key[32];
    base64_encode(digest, 20, accept_key);

    char resp[256];
    int len = snprintf(resp, sizeof(resp),
                       "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: %s\r\n\r\n",
                       accept_key);

    if (send(client_fd, resp, (size_t)len, 0) <= 0) {
        return -1;
    }

    add_client(client_fd);
    return 0;
}

int ws_send_text(int client_fd, const char *text) {
    if (client_fd < 0 || !text) return -1;

    size_t payload_len = strlen(text);
    uint8_t header[10];
    size_t hlen = 0;

    header[0] = 0x81; /* FIN bit set, Opcode 0x01 (Text) */
    if (payload_len < 126) {
        header[1] = (uint8_t)payload_len;
        hlen = 2;
    } else if (payload_len <= 65535) {
        header[1] = 126;
        header[2] = (uint8_t)((payload_len >> 8) & 0xFF);
        header[3] = (uint8_t)(payload_len & 0xFF);
        hlen = 4;
    } else {
        return -1;
    }

    if (send(client_fd, header, hlen, MSG_NOSIGNAL) <= 0) return -1;
    if (send(client_fd, text, payload_len, MSG_NOSIGNAL) <= 0) return -1;

    return 0;
}

void ws_broadcast_text(const char *text) {
    if (!text || g_client_count == 0) return;

    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] >= 0) {
            if (ws_send_text(g_clients[i], text) != 0) {
                close(g_clients[i]);
                g_clients[i] = -1;
                if (g_client_count > 0) g_client_count--;
            }
        }
    }
}

int ws_read_frame(int client_fd, char *out_buf, size_t max_len, int *out_opcode) {
    if (client_fd < 0 || !out_buf || max_len == 0) return -1;

    uint8_t hdr[2];
    ssize_t n = recv(client_fd, hdr, 2, MSG_DONTWAIT);
    if (n <= 0) return (int)n;

    uint8_t opcode = hdr[0] & 0x0F;
    if (out_opcode) *out_opcode = opcode;

    int masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        if (recv(client_fd, ext, 2, 0) != 2) return -1;
        payload_len = ((uint64_t)ext[0] << 8) | ext[1];
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (recv(client_fd, mask, 4, 0) != 4) return -1;
    }

    if (payload_len >= max_len) payload_len = max_len - 1;

    size_t total = 0;
    while (total < payload_len) {
        ssize_t r = recv(client_fd, out_buf + total, payload_len - total, 0);
        if (r <= 0) return -1;
        total += (size_t)r;
    }
    out_buf[total] = '\0';

    if (masked) {
        for (size_t i = 0; i < total; i++) {
            out_buf[i] ^= mask[i % 4];
        }
    }

    return (int)total;
}

int ws_get_clients(int *out_fds, size_t max_count) {
    if (!out_fds || max_count == 0) return 0;
    size_t count = 0;
    for (size_t i = 0; i < WS_MAX_CLIENTS && count < max_count; i++) {
        if (g_clients[i] >= 0) {
            out_fds[count++] = g_clients[i];
        }
    }
    return (int)count;
}

int ws_send_pong(int client_fd) {
    if (client_fd < 0) return -1;
    uint8_t frame[2] = {0x8A, 0x00};
    return send(client_fd, frame, 2, MSG_NOSIGNAL) > 0 ? 0 : -1;
}
