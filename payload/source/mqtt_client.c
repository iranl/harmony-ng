#include "mqtt_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MQTT_BUF_SIZE 4096
#define MQTT_KEEPALIVE_SEC 30
#define MQTT_MAX_SUBS 8

typedef struct {
    char topic[128];
    mqtt_msg_cb cb;
    void *user_data;
} mqtt_sub_entry_t;

static char g_host[128] = "127.0.0.1";
static int  g_port = 1883;
static char g_client_id[64] = "harmony_hub";
static char g_user[256] = {0};
static char g_pass[256] = {0};

static char g_lwt_topic[128] = {0};
static char g_lwt_payload[128] = {0};
static int  g_lwt_retain = 0;

static char g_birth_topic[128] = {0};
static char g_birth_payload[128] = {0};
static int  g_birth_retain = 0;

static int g_fd = -1;
static int g_connected = 0;
static uint16_t g_packet_id = 1;
static time_t g_last_ping = 0;

static mqtt_sub_entry_t g_subs[MQTT_MAX_SUBS];
static size_t g_sub_count = 0;

static uint8_t *encode_remaining_len(uint8_t *p, size_t len) {
    do {
        uint8_t d = (uint8_t)(len % 128);
        len /= 128;
        if (len > 0) d |= 0x80;
        *p++ = d;
    } while (len > 0);
    return p;
}

static size_t decode_remaining_len(const uint8_t *buf, size_t buf_len, size_t *out_val) {
    size_t multiplier = 1, value = 0, idx = 0;
    uint8_t encoded_byte;
    do {
        if (idx >= buf_len) return 0;
        encoded_byte = buf[idx++];
        value += (encoded_byte & 127) * multiplier;
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128) return 0;
    } while ((encoded_byte & 128) != 0);

    *out_val = value;
    return idx;
}

int mqtt_init(const char *host, int port, const char *client_id, const char *user, const char *pass) {
    if (host && host[0]) strncpy(g_host, host, sizeof(g_host) - 1);
    g_port = port > 0 ? port : 1883;
    if (client_id && client_id[0]) strncpy(g_client_id, client_id, sizeof(g_client_id) - 1);
    if (user && user[0]) strncpy(g_user, user, sizeof(g_user) - 1);
    if (pass && pass[0]) strncpy(g_pass, pass, sizeof(g_pass) - 1);

    g_sub_count = 0;
    g_connected = 0;
    g_fd = -1;
    return 0;
}

void mqtt_set_lwt(const char *topic, const char *payload, int retain) {
    if (topic && topic[0]) {
        strncpy(g_lwt_topic, topic, sizeof(g_lwt_topic) - 1);
        if (payload) strncpy(g_lwt_payload, payload, sizeof(g_lwt_payload) - 1);
        else g_lwt_payload[0] = 0;
        g_lwt_retain = retain ? 1 : 0;
    } else {
        g_lwt_topic[0] = 0;
        g_lwt_payload[0] = 0;
        g_lwt_retain = 0;
    }
}

void mqtt_set_birth(const char *topic, const char *payload, int retain) {
    if (topic && topic[0]) {
        strncpy(g_birth_topic, topic, sizeof(g_birth_topic) - 1);
        if (payload) strncpy(g_birth_payload, payload, sizeof(g_birth_payload) - 1);
        else g_birth_payload[0] = 0;
        g_birth_retain = retain ? 1 : 0;
    } else {
        g_birth_topic[0] = 0;
        g_birth_payload[0] = 0;
        g_birth_retain = 0;
    }
}

int mqtt_is_connected(void) {
    return g_connected && (g_fd >= 0);
}

void mqtt_disconnect(void) {
    int fd = g_fd;
    g_connected = 0;
    g_fd = -1;
    if (fd >= 0) {
        uint8_t disc[2] = {0xE0, 0x00};
        send(fd, disc, 2, MSG_NOSIGNAL);
        close(fd);
    }
}

static volatile int s_connecting = 0;

int mqtt_is_connecting(void) {
    return s_connecting;
}

int mqtt_connect(void) {
    mqtt_disconnect();

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(g_port);

    if (inet_aton(g_host, &saddr.sin_addr) == 0) {
        struct hostent *he = gethostbyname(g_host);
        if (!he || !he->h_addr_list || !he->h_addr_list[0]) return -1;
        memcpy(&saddr.sin_addr, he->h_addr_list[0], sizeof(saddr.sin_addr));
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    /* Set 64KB send buffer to absorb burst discovery publishes without drop */
    int sndbuf = 65536;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* Non-blocking connect with 2-second timeout */
    int sock_flags = fcntl(sock, F_GETFL, 0);
    if (sock_flags >= 0) fcntl(sock, F_SETFL, sock_flags | O_NONBLOCK);

    int rc = connect(sock, (struct sockaddr *)&saddr, sizeof(saddr));
    if (rc < 0) {
        if (errno != EINPROGRESS) {
            close(sock);
            return -1;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        int sel = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) {
            close(sock);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            close(sock);
            return -1;
        }
    }

    if (sock_flags >= 0) fcntl(sock, F_SETFL, sock_flags & ~O_NONBLOCK);
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    /* Build MQTT 3.1.1 CONNECT packet */
    uint8_t var_hdr[] = {
        0x00, 0x04, 'M', 'Q', 'T', 'T', /* Protocol Name */
        0x04,                           /* Level 4 = 3.1.1 */
        0x02,                           /* Connect Flags: Clean Session */
        (MQTT_KEEPALIVE_SEC >> 8) & 0xFF, (MQTT_KEEPALIVE_SEC & 0xFF)
    };

    uint8_t flags = 0x02;
    if (g_lwt_topic[0]) {
        flags |= 0x04; /* Will Flag */
        if (g_lwt_retain) flags |= 0x20; /* Will Retain */
    }
    if (g_user[0]) flags |= 0x80;
    if (g_pass[0]) flags |= 0x40;
    var_hdr[7] = flags;

    size_t cid_len = strlen(g_client_id);
    size_t payload_len = 2 + cid_len;
    if (g_lwt_topic[0]) {
        payload_len += 2 + strlen(g_lwt_topic);
        payload_len += 2 + strlen(g_lwt_payload);
    }
    if (g_user[0]) payload_len += 2 + strlen(g_user);
    if (g_pass[0]) payload_len += 2 + strlen(g_pass);

    size_t rem_len = sizeof(var_hdr) + payload_len;
    uint8_t pkt[1024];
    pkt[0] = 0x10; /* CONNECT */
    uint8_t *p = encode_remaining_len(&pkt[1], rem_len);

    memcpy(p, var_hdr, sizeof(var_hdr));
    p += sizeof(var_hdr);

    /* Client ID */
    *p++ = (uint8_t)((cid_len >> 8) & 0xFF);
    *p++ = (uint8_t)(cid_len & 0xFF);
    memcpy(p, g_client_id, cid_len);
    p += cid_len;

    /* Will Topic & Message */
    if (g_lwt_topic[0]) {
        size_t wtlen = strlen(g_lwt_topic);
        *p++ = (uint8_t)((wtlen >> 8) & 0xFF);
        *p++ = (uint8_t)(wtlen & 0xFF);
        memcpy(p, g_lwt_topic, wtlen);
        p += wtlen;

        size_t wplen = strlen(g_lwt_payload);
        *p++ = (uint8_t)((wplen >> 8) & 0xFF);
        *p++ = (uint8_t)(wplen & 0xFF);
        memcpy(p, g_lwt_payload, wplen);
        p += wplen;
    }

    /* Username */
    if (g_user[0]) {
        size_t ulen = strlen(g_user);
        *p++ = (uint8_t)((ulen >> 8) & 0xFF);
        *p++ = (uint8_t)(ulen & 0xFF);
        memcpy(p, g_user, ulen);
        p += ulen;
    }

    /* Password */
    if (g_pass[0]) {
        size_t plen = strlen(g_pass);
        *p++ = (uint8_t)((plen >> 8) & 0xFF);
        *p++ = (uint8_t)(plen & 0xFF);
        memcpy(p, g_pass, plen);
        p += plen;
    }

    size_t total_len = (size_t)(p - pkt);
    if (send(sock, pkt, total_len, 0) != (ssize_t)total_len) {
        close(sock);
        return -1;
    }

    /* Read CONNACK */
    uint8_t ack[4];
    if (recv(sock, ack, 4, 0) != 4 || ack[0] != 0x20 || ack[3] != 0x00) {
        close(sock);
        return -1;
    }

    /* Switch to non-blocking */
    int f = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, f | O_NONBLOCK);

    g_fd = sock;
    g_connected = 1;
    g_last_ping = time(NULL);

    /* Publish birth message if configured */
    if (g_birth_topic[0]) {
        mqtt_publish(g_birth_topic, g_birth_payload, g_birth_retain);
    }

    /* Re-subscribe existing subscriptions */
    for (size_t i = 0; i < g_sub_count; i++) {
        mqtt_subscribe(g_subs[i].topic, g_subs[i].cb, g_subs[i].user_data);
    }

    return 0;
}

static void *mqtt_async_connect_worker(void *arg) {
    (void)arg;
    mqtt_connect();
    s_connecting = 0;
    return NULL;
}

int mqtt_connect_async(void) {
    if (s_connecting || mqtt_is_connected()) return 0;
    s_connecting = 1;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, mqtt_async_connect_worker, NULL) != 0) {
        s_connecting = 0;
        pthread_attr_destroy(&attr);
        return -1;
    }
    pthread_attr_destroy(&attr);
    return 0;
}

extern int g_dry_run;

int mqtt_publish(const char *topic, const char *payload, int retain) {
    if (g_dry_run) {
        printf("[DRY-RUN] MQTT Publish simulated: topic=%s payload=%s retain=%d\n", topic ? topic : "", payload ? payload : "", retain);
        return 0;
    }
    if (!mqtt_is_connected() || !topic) return -1;

    size_t tlen = strlen(topic);
    size_t plen = payload ? strlen(payload) : 0;
    size_t rem_len = 2 + tlen + plen;

    uint8_t pkt[MQTT_BUF_SIZE];
    pkt[0] = 0x30 | (retain ? 0x01 : 0x00); /* PUBLISH QoS 0 */
    uint8_t *p = encode_remaining_len(&pkt[1], rem_len);

    *p++ = (uint8_t)((tlen >> 8) & 0xFF);
    *p++ = (uint8_t)(tlen & 0xFF);
    memcpy(p, topic, tlen);
    p += tlen;

    if (plen > 0) {
        memcpy(p, payload, plen);
        p += plen;
    }

    size_t total = (size_t)(p - pkt);
    ssize_t sent = send(g_fd, pkt, total, MSG_NOSIGNAL);
    if (sent != (ssize_t)total) {
        mqtt_disconnect();
        return -1;
    }
    return 0;
}

int mqtt_subscribe(const char *topic, mqtt_msg_cb cb, void *user_data) {
    if (!topic) return -1;

    int found = 0;
    for (size_t i = 0; i < g_sub_count; i++) {
        if (strcmp(g_subs[i].topic, topic) == 0) {
            g_subs[i].cb = cb;
            g_subs[i].user_data = user_data;
            found = 1;
            break;
        }
    }
    if (!found && g_sub_count < MQTT_MAX_SUBS) {
        strncpy(g_subs[g_sub_count].topic, topic, sizeof(g_subs[g_sub_count].topic) - 1);
        g_subs[g_sub_count].cb = cb;
        g_subs[g_sub_count].user_data = user_data;
        g_sub_count++;
    }

    if (!mqtt_is_connected()) return 0;

    size_t tlen = strlen(topic);
    size_t rem_len = 2 /* pkt_id */ + 2 /* tlen */ + tlen + 1 /* qos */;

    uint8_t pkt[256];
    pkt[0] = 0x82; /* SUBSCRIBE */
    uint8_t *p = encode_remaining_len(&pkt[1], rem_len);

    uint16_t pid = g_packet_id++;
    *p++ = (uint8_t)((pid >> 8) & 0xFF);
    *p++ = (uint8_t)(pid & 0xFF);

    *p++ = (uint8_t)((tlen >> 8) & 0xFF);
    *p++ = (uint8_t)(tlen & 0xFF);
    memcpy(p, topic, tlen);
    p += tlen;

    *p++ = 0x00; /* QoS 0 */

    size_t total = (size_t)(p - pkt);
    ssize_t sent = send(g_fd, pkt, total, MSG_NOSIGNAL);
    if (sent != (ssize_t)total) {
        mqtt_disconnect();
        return -1;
    }
    return 0;
}

int mqtt_tick(void) {
    if (!mqtt_is_connected()) {
        return 0;
    }

    time_t now = time(NULL);
    if (now - g_last_ping >= MQTT_KEEPALIVE_SEC) {
        uint8_t ping[2] = {0xC0, 0x00};
        if (send(g_fd, ping, 2, MSG_NOSIGNAL) != 2) {
            mqtt_disconnect();
            return 0;
        }
        g_last_ping = now;
    }

    uint8_t buf[MQTT_BUF_SIZE];
    ssize_t n = recv(g_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        mqtt_disconnect();
        return -1;
    }
    if (n == 0) {
        mqtt_disconnect();
        return -1;
    }

    /* Process incoming MQTT packet */
    uint8_t type = buf[0] & 0xF0;
    if (type == 0x30) { /* PUBLISH */
        size_t rem_len = 0;
        size_t rlen_bytes = decode_remaining_len(&buf[1], (size_t)n - 1, &rem_len);
        if (rlen_bytes == 0) return 0;

        const uint8_t *p = &buf[1 + rlen_bytes];
        if (p + 2 > buf + n) return 0;

        size_t tlen = ((size_t)p[0] << 8) | p[1];
        p += 2;
        if (p + tlen > buf + n) return 0;

        char topic[128] = {0};
        size_t cplen = tlen < sizeof(topic) - 1 ? tlen : sizeof(topic) - 1;
        memcpy(topic, p, cplen);
        p += tlen;

        size_t plen = (size_t)(buf + n - p);
        char payload[1024] = {0};
        size_t cppl = plen < sizeof(payload) - 1 ? plen : sizeof(payload) - 1;
        memcpy(payload, p, cppl);

        /* Dispatch to matching subscribers */
        for (size_t i = 0; i < g_sub_count; i++) {
            if (strcmp(g_subs[i].topic, topic) == 0 && g_subs[i].cb) {
                g_subs[i].cb(topic, payload, cppl, g_subs[i].user_data);
            }
        }
    }

    return 1;
}
