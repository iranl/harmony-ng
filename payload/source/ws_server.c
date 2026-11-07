#define _GNU_SOURCE
#include "ws_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static int g_clients[WS_MAX_CLIENTS];
static int g_client_roles[WS_MAX_CLIENTS];
/* 1 until the main loop sent the client its snapshot: broadcasts skip it meanwhile, so the
 * snapshot is the first frame the client receives */
static int g_client_pending[WS_MAX_CLIENTS];
/* Keepalive state: monotonic seconds of the last frame from the client, and of the ping we
 * sent and still wait to be answered (0 = none outstanding) */
static time_t g_client_last_rx[WS_MAX_CLIENTS];
static time_t g_client_ping_sent[WS_MAX_CLIENTS];

static time_t mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}
static size_t g_client_count = 0;
/* Clients are added from HTTP worker threads, polled and removed by the main loop, and sent to
 * from the orchestrator, HTTP and state watcher threads: guard the list, and serialize frame
 * writes so that two threads never interleave frames on one socket. */
static pthread_mutex_t g_clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) g_clients[i] = -1;
    g_client_count = 0;
    pthread_mutex_unlock(&g_clients_lock);
    return 0;
}

int ws_is_upgrade_req(const char *http_req) {
    if (!http_req) return 0;
    return (strcasestr(http_req, "Upgrade: websocket") != NULL ||
            strcasestr(http_req, "Upgrade: WebSocket") != NULL);
}

/* Returns 0, or -1 when all WS_MAX_CLIENTS slots are taken */
static int add_client(int fd) {
    int rc = -1;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] < 0) {
            g_clients[i] = fd;
            g_client_roles[i] = 2; /* admin until the upgrade records the token's role */
            g_client_pending[i] = 1;
            g_client_last_rx[i] = mono_now();
            g_client_ping_sent[i] = 0;
            g_client_count++;
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    return rc;
}

/* Unregister without closing (the caller still owns the socket) */
static void drop_client(int fd) {
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == fd) {
            g_clients[i] = -1;
            if (g_client_count > 0) g_client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
}

/* send() until every byte is out: a partial send would leave a truncated frame on the stream */
static int send_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, (const char *)buf + total, len - total, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

void ws_remove_client(int client_fd) {
    int found = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == client_fd) {
            g_clients[i] = -1;
            if (g_client_count > 0) g_client_count--;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    /* Close only a registered client: a broadcast and the main loop can both see a dead
     * socket, and the second close would hit an fd that may already be reused */
    if (found) close(client_fd);
}

void ws_close_all(void) {
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] >= 0) {
            close(g_clients[i]);
            g_clients[i] = -1;
        }
    }
    g_client_count = 0;
    pthread_mutex_unlock(&g_clients_lock);
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

    /* Take a slot before accepting the upgrade: a client accepted without one would never get
     * broadcasts or be closed on shutdown */
    if (add_client(client_fd) != 0) {
        static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        send_all(client_fd, busy, sizeof(busy) - 1);
        return -1;
    }
    if (send_all(client_fd, resp, (size_t)len) != 0) {
        drop_client(client_fd);
        return -1;
    }
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

    int rc = 0;
    pthread_mutex_lock(&g_send_lock);
    if (send_all(client_fd, header, hlen) != 0 || send_all(client_fd, text, payload_len) != 0) {
        rc = -1;
    }
    pthread_mutex_unlock(&g_send_lock);
    return rc;
}

void ws_broadcast_text(const char *text) {
    if (!text) return;
    int fds[WS_MAX_CLIENTS];
    int n = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] >= 0 && !g_client_pending[i]) fds[n++] = g_clients[i];
    }
    pthread_mutex_unlock(&g_clients_lock);
    for (int i = 0; i < n; i++) {
        if (ws_send_text(fds[i], text) != 0) {
            ws_remove_client(fds[i]);
        }
    }
}

int ws_take_pending_clients(int *out_fds, size_t max_count) {
    if (!out_fds || max_count == 0) return 0;
    size_t count = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS && count < max_count; i++) {
        if (g_clients[i] >= 0 && g_client_pending[i]) {
            g_client_pending[i] = 0;
            out_fds[count++] = g_clients[i];
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    return (int)count;
}

/* Any frame from the client counts as traffic for the keepalive */
static void note_client_rx(int client_fd) {
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == client_fd) {
            g_client_last_rx[i] = mono_now();
            g_client_ping_sent[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
}

int ws_read_frame(int client_fd, char *out_buf, size_t max_len, int *out_opcode) {
    if (client_fd < 0 || !out_buf || max_len == 0) return -1;

    uint8_t hdr[2];
    ssize_t n = recv(client_fd, hdr, 2, MSG_DONTWAIT);
    /* 0 bytes = the peer closed: report it as an error, so that an empty frame (a pong or a
     * ping without payload, which returns 0) is not mistaken for a closed connection */
    if (n <= 0) return -1;
    note_client_rx(client_fd);

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

void ws_set_client_role(int client_fd, int role) {
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == client_fd) {
            g_client_roles[i] = role;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
}

int ws_get_client_role(int client_fd) {
    int role = 2;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == client_fd) {
            role = g_client_roles[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    return role;
}

int ws_get_clients(int *out_fds, size_t max_count) {
    if (!out_fds || max_count == 0) return 0;
    size_t count = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS && count < max_count; i++) {
        if (g_clients[i] >= 0) {
            out_fds[count++] = g_clients[i];
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    return (int)count;
}

int ws_send_ping(int client_fd) {
    if (client_fd < 0) return -1;
    uint8_t frame[2] = {0x89, 0x00};
    pthread_mutex_lock(&g_send_lock);
    int rc = send_all(client_fd, frame, 2);
    pthread_mutex_unlock(&g_send_lock);
    return rc;
}

int ws_server_tick(void) {
    time_t now = mono_now();
    int to_ping[WS_MAX_CLIENTS], n_ping = 0;
    int to_drop[WS_MAX_CLIENTS], n_drop = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] < 0) continue;
        if (g_client_ping_sent[i]) {
            if (now - g_client_ping_sent[i] >= WS_PONG_TIMEOUT_S) to_drop[n_drop++] = g_clients[i];
        } else if (now - g_client_last_rx[i] >= WS_PING_INTERVAL_S) {
            g_client_ping_sent[i] = now;
            to_ping[n_ping++] = g_clients[i];
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    int dropped = 0;
    for (int i = 0; i < n_ping; i++) {
        if (ws_send_ping(to_ping[i]) != 0) {
            ws_remove_client(to_ping[i]);
            dropped++;
        }
    }
    for (int i = 0; i < n_drop; i++) {
        ws_remove_client(to_drop[i]);
        dropped++;
    }
    return dropped;
}

int ws_send_pong(int client_fd) {
    if (client_fd < 0) return -1;
    uint8_t frame[2] = {0x8A, 0x00};
    pthread_mutex_lock(&g_send_lock);
    int rc = send_all(client_fd, frame, 2);
    pthread_mutex_unlock(&g_send_lock);
    return rc;
}
