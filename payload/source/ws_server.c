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
#define WS_SLOT_FREE (-1)
#define WS_SLOT_RESERVED (-2) /* taken during the handshake, not yet visible to anyone */
static unsigned int g_client_gen[WS_MAX_CLIENTS]; /* changes with every client in the slot */
static unsigned int g_next_gen = 1;
static int g_client_legacy[WS_MAX_CLIENTS];
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
 * from the orchestrator, HTTP and state watcher threads: guard the list. Frame writes go
 * through each client's output queue (ws_queue_frame), under that client's own lock. */
static pthread_mutex_t g_clients_lock = PTHREAD_MUTEX_INITIALIZER;

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
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) g_clients[i] = WS_SLOT_FREE;
    g_client_count = 0;
    pthread_mutex_unlock(&g_clients_lock);
    return 0;
}

int ws_is_upgrade_req(const char *http_req) {
    if (!http_req) return 0;
    return (strcasestr(http_req, "Upgrade: websocket") != NULL ||
            strcasestr(http_req, "Upgrade: WebSocket") != NULL);
}

/* Reserve a slot before the 101 is sent. Returns the slot index, or -1 when all
 * WS_MAX_CLIENTS slots are taken. A reserved slot is skipped by every reader (fd < 0). */
static int reserve_slot(void) {
    int slot = -1;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == WS_SLOT_FREE) {
            g_clients[i] = WS_SLOT_RESERVED;
            slot = (int)i;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    return slot;
}

static void release_slot(int slot) {
    if (slot < 0 || slot >= WS_MAX_CLIENTS) return;
    pthread_mutex_lock(&g_clients_lock);
    if (g_clients[slot] == WS_SLOT_RESERVED) g_clients[slot] = WS_SLOT_FREE;
    pthread_mutex_unlock(&g_clients_lock);
}

/* Publish the client with its final role and legacy flag in one step, after the 101 went
 * out: the main loop, broadcasts and the snapshot see it only from here on. */
static void commit_slot(int slot, int fd, int role, int legacy) {
    pthread_mutex_lock(&g_clients_lock);
    g_client_roles[slot] = role;
    g_client_legacy[slot] = legacy ? 1 : 0;
    g_client_pending[slot] = legacy ? 0 : 1; /* no 8089 snapshot for a legacy client */
    g_client_last_rx[slot] = mono_now();
    g_client_ping_sent[slot] = 0;
    g_client_gen[slot] = g_next_gen++;
    if (g_next_gen == 0) g_next_gen = 1;
    g_clients[slot] = fd;
    g_client_count++;
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

int ws_handle_upgrade(int client_fd, const char *http_req, int role, int legacy) {
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

    /* Reserve a slot before accepting the upgrade (a client accepted without one would never
     * get broadcasts or be closed on shutdown), send the 101, and only then publish the client
     * with its final role: before that the main loop must not read, answer or close it. */
    int slot = reserve_slot();
    if (slot < 0) {
        static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        send_all(client_fd, busy, sizeof(busy) - 1);
        return -1;
    }
    if (send_all(client_fd, resp, (size_t)len) != 0) {
        release_slot(slot);
        return -1;
    }
    commit_slot(slot, client_fd, role, legacy);
    return 0;
}

/* Output queue of each client. Frames are written with MSG_DONTWAIT; what the socket does not
 * take at once waits here and the main loop sends it on (ws_server_tick). The writes used to
 * block (2 s per send() call, and send_all() went on as long as each call moved a byte) under
 * one lock for all clients, so a client that read a few bytes now and then held the main loop
 * and every other sender. A client whose queue grows past WS_TXQ_MAX_BYTES, or that has not
 * taken its queue within WS_TXQ_MAX_AGE_S, is dropped. */
#define WS_TXQ_MAX_BYTES (256 * 1024)
#define WS_TXQ_MAX_AGE_S 5
struct ws_txq {
    pthread_mutex_t lock;
    unsigned int gen; /* client the queue belongs to (g_client_gen) */
    char *buf;
    size_t len;  /* bytes in buf */
    size_t off;  /* bytes of buf already sent */
    time_t since; /* when the queue last went from empty to not empty */
};
static struct ws_txq g_txq[WS_MAX_CLIENTS] = { [0 ... WS_MAX_CLIENTS - 1] = { .lock = PTHREAD_MUTEX_INITIALIZER } };

/* Sends what the socket takes without waiting; called with q->lock held. 0 while the client is
 * fine (queue empty or still waiting), -1 when it has to go. */
static int txq_flush(int fd, struct ws_txq *q) {
    while (q->off < q->len) {
        ssize_t n = send(fd, q->buf + q->off, q->len - q->off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) return -1;
        q->off += (size_t)n;
    }
    if (q->off == q->len) {
        free(q->buf);
        q->buf = NULL;
        q->len = q->off = 0;
        return 0;
    }
    return (mono_now() - q->since > WS_TXQ_MAX_AGE_S) ? -1 : 0;
}

/* Queues one whole frame (header and payload) for the client on fd and sends what it can.
 * -1 when fd is not a client, or the client cannot keep up (the caller drops it). */
static int ws_queue_frame(int fd, const uint8_t *hdr, size_t hlen, const void *payload, size_t plen) {
    int slot = -1;
    unsigned int gen = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == fd) {
            slot = (int)i;
            gen = g_client_gen[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    if (slot < 0) return -1;

    struct ws_txq *q = &g_txq[slot];
    int rc = 0;
    pthread_mutex_lock(&q->lock);
    if (q->gen != gen) { /* a new client in the slot: the old one's bytes are not for it */
        free(q->buf);
        q->buf = NULL;
        q->len = q->off = 0;
        q->gen = gen;
    }
    size_t pending = q->len - q->off;
    if (pending > 0 && pending + hlen + plen > WS_TXQ_MAX_BYTES) {
        rc = -1; /* one frame always fits in an empty queue; behind others it may not */
    } else {
        if (pending == 0) {
            free(q->buf);
            q->buf = NULL;
            q->len = q->off = 0;
            q->since = mono_now();
        } else if (q->off > 0) {
            memmove(q->buf, q->buf + q->off, pending);
            q->len = pending;
            q->off = 0;
        }
        char *nb = (char *)realloc(q->buf, q->len + hlen + plen);
        if (!nb) {
            rc = -1;
        } else {
            q->buf = nb;
            memcpy(q->buf + q->len, hdr, hlen);
            if (plen) memcpy(q->buf + q->len + hlen, payload, plen);
            q->len += hlen + plen;
            rc = txq_flush(fd, q);
        }
    }
    pthread_mutex_unlock(&q->lock);
    return rc;
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
        /* RFC 6455 64-bit length: a stock configuration object with many devices passes 64 KB */
        header[1] = 127;
        for (int i = 0; i < 8; i++) header[2 + i] = (uint8_t)(((uint64_t)payload_len >> (8 * (7 - i))) & 0xFF);
        hlen = 10;
    }

    return ws_queue_frame(client_fd, header, hlen, text, payload_len);
}

void ws_broadcast_text(const char *text) {
    if (!text) return;
    int fds[WS_MAX_CLIENTS];
    int n = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] >= 0 && !g_client_pending[i] && !g_client_legacy[i]) fds[n++] = g_clients[i];
    }
    pthread_mutex_unlock(&g_clients_lock);
    for (int i = 0; i < n; i++) {
        if (ws_send_text(fds[i], text) != 0) {
            ws_remove_client(fds[i]);
        }
    }
}

int ws_is_client_legacy(int client_fd) {
    int legacy = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == client_fd) {
            legacy = g_client_legacy[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    return legacy;
}

void ws_broadcast_legacy_text(const char *text) {
    if (!text) return;
    int fds[WS_MAX_CLIENTS];
    int n = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] >= 0 && g_client_legacy[i]) fds[n++] = g_clients[i];
    }
    pthread_mutex_unlock(&g_clients_lock);
    for (int i = 0; i < n; i++) {
        if (ws_send_text(fds[i], text) != 0) ws_remove_client(fds[i]);
    }
}

void ws_close_legacy_clients(void) {
    int fds[WS_MAX_CLIENTS];
    int n = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] >= 0 && g_client_legacy[i]) fds[n++] = g_clients[i];
    }
    pthread_mutex_unlock(&g_clients_lock);
    for (int i = 0; i < n; i++) ws_remove_client(fds[i]);
}

void ws_sha1(const uint8_t *data, size_t len, uint8_t digest[20]) {
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(digest, &ctx);
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

/* The frame each client is in the middle of sending. ws_read_frame() runs on the main loop and
 * takes only the bytes that are there: it used to wait in recv() for the rest of a frame, so a
 * client sending one byte every few seconds stopped the Hub (no HTTP accept, no other client,
 * no mDNS) for hours. Only the main loop touches these; a slot's buffer is dropped when a new
 * client takes the slot (generation) or when its frame is complete. */
#define WS_FRAME_HDR_MAX 14 /* 2 + 8-byte length + 4-byte mask */
static struct {
    unsigned int gen;
    uint8_t *buf;
    size_t len;
} g_client_rx[WS_MAX_CLIENTS];

/* Bytes the frame in buf needs in total, as far as the first have bytes tell; 0 when the
 * announced payload does not fit max_payload */
static size_t ws_frame_need(const uint8_t *buf, size_t have, size_t max_payload) {
    if (have < 2) return 2;
    size_t ext = (buf[1] & 0x7F) == 126 ? 2 : (buf[1] & 0x7F) == 127 ? 8 : 0;
    size_t hlen = 2 + ext + ((buf[1] & 0x80) ? 4 : 0);
    if (have < hlen) return hlen;
    uint64_t payload_len = buf[1] & 0x7F;
    if (ext) {
        payload_len = 0;
        for (size_t i = 0; i < ext; i++) payload_len = (payload_len << 8) | buf[2 + i];
    }
    if (payload_len > max_payload) return 0;
    return hlen + (size_t)payload_len;
}

int ws_read_frame(int client_fd, char *out_buf, size_t max_len, int *out_opcode) {
    if (client_fd < 0 || !out_buf || max_len == 0) return -1;

    int slot = -1;
    unsigned int gen = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == client_fd) {
            slot = (int)i;
            gen = g_client_gen[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    if (slot < 0) return -1;
    if (g_client_rx[slot].gen != gen) { /* a new client in the slot: forget the old one's bytes */
        free(g_client_rx[slot].buf);
        g_client_rx[slot].buf = NULL;
        g_client_rx[slot].len = 0;
        g_client_rx[slot].gen = gen;
    }
    if (!g_client_rx[slot].buf) {
        g_client_rx[slot].buf = (uint8_t *)malloc(WS_FRAME_HDR_MAX + max_len);
        if (!g_client_rx[slot].buf) return -1;
        g_client_rx[slot].len = 0;
    }
    uint8_t *fb = g_client_rx[slot].buf;
    size_t *have = &g_client_rx[slot].len;

    /* Read up to the end of this frame only: bytes of the next one stay in the socket, where
     * select() sees them */
    size_t need;
    while ((need = ws_frame_need(fb, *have, max_len - 1)) > *have) {
        ssize_t n = recv(client_fd, fb + *have, need - *have, MSG_DONTWAIT);
        if (n < 0) {
            /* nothing more there yet (or a signal): keep the client and the bytes so far */
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return WS_READ_AGAIN;
            return -1;
        }
        /* 0 bytes = the peer closed: report it as an error, so that an empty frame (a pong or a
         * ping without payload, which returns 0) is not mistaken for a closed connection */
        if (n == 0) return -1;
        *have += (size_t)n;
    }
    /* A frame larger than the caller's buffer used to be cut, and its rest was parsed as the
     * next frame header: drop the client instead */
    if (need == 0) return -1;

    note_client_rx(client_fd);
    uint8_t opcode = fb[0] & 0x0F;
    if (out_opcode) *out_opcode = opcode;
    int masked = (fb[1] & 0x80) != 0;
    size_t ext = (fb[1] & 0x7F) == 126 ? 2 : (fb[1] & 0x7F) == 127 ? 8 : 0;
    size_t hlen = 2 + ext + (masked ? 4 : 0);
    size_t total = need - hlen;
    memcpy(out_buf, fb + hlen, total);
    out_buf[total] = '\0';
    if (masked) {
        const uint8_t *mask = fb + 2 + ext;
        for (size_t i = 0; i < total; i++) {
            out_buf[i] ^= mask[i % 4];
        }
    }

    free(g_client_rx[slot].buf); /* frame complete: no memory kept for an idle client */
    g_client_rx[slot].buf = NULL;
    g_client_rx[slot].len = 0;
    return (int)total;
}

int ws_is_client(int client_fd) {
    int found = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == client_fd) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    return found;
}

/* Generation of the client now holding client_fd (0 when none): lets a deferred reply check
 * that the fd still belongs to the client that asked, not to a later one with the same number. */
unsigned int ws_client_gen(int client_fd) {
    unsigned int gen = 0;
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_clients[i] == client_fd) {
            gen = g_client_gen[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
    return gen;
}

int ws_get_client_role(int client_fd) {
    int role = 0; /* unknown client: no rights */
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
    return ws_queue_frame(client_fd, frame, 2, NULL, 0);
}

int ws_server_tick(void) {
    time_t now = mono_now();
    int to_ping[WS_MAX_CLIENTS], n_ping = 0;
    int to_drop[WS_MAX_CLIENTS], n_drop = 0;
    int fds[WS_MAX_CLIENTS];
    unsigned int gens[WS_MAX_CLIENTS];
    pthread_mutex_lock(&g_clients_lock);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        fds[i] = g_clients[i];
        gens[i] = g_client_gen[i];
    }
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
    /* Send on what the output queues hold; drop the clients that do not take it, and free the
     * queue of a client that is gone */
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        struct ws_txq *q = &g_txq[i];
        pthread_mutex_lock(&q->lock);
        if (q->len > q->off) {
            if (fds[i] < 0 || q->gen != gens[i]) {
                free(q->buf);
                q->buf = NULL;
                q->len = q->off = 0;
            } else if (txq_flush(fds[i], q) != 0 && n_drop < WS_MAX_CLIENTS) {
                to_drop[n_drop++] = fds[i];
            }
        }
        pthread_mutex_unlock(&q->lock);
    }
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
    return ws_queue_frame(client_fd, frame, 2, NULL, 0);
}
