#include "mqtt_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MQTT_BUF_SIZE 4096
/* Larger packets (an inventory index of a big setup) are built in a heap buffer */
#define MQTT_MAX_PACKET (64 * 1024)
#define MQTT_KEEPALIVE_SEC 30
#define MQTT_MAX_SUBS 32

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
static int publish_packet(const char *topic, const char *payload, size_t plen, int qos, int retain);
static int subscribe_packet(const char *topic);
/* Set by mqtt_connect() only after the subscriptions and the birth message went out: until then
 * the connecting thread is the only one writing to the socket (the main loop publishes as soon
 * as it sees the connection, and two threads writing one socket can mix their packets) */
static volatile int g_connected = 0;
static uint16_t g_packet_id = 1;
static time_t g_last_ping = 0;

static mqtt_sub_entry_t g_subs[MQTT_MAX_SUBS];
static size_t g_sub_count = 0;

/* Received bytes not processed yet (a packet split over several reads). Belongs to one
 * connection: emptied whenever the connection ends. */
static uint8_t g_rxbuf[MQTT_BUF_SIZE * 2];
static size_t g_rxlen = 0;

/* Writes the whole buffer on the non-blocking socket, waiting up to 2 s for room each time the
 * send buffer is full (a large packet does not fit at once). -1 when the connection failed. */
static int send_all(int fd, const uint8_t *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n > 0) {
            buf += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            if (poll(&pfd, 1, 2000) > 0 && !(pfd.revents & (POLLERR | POLLHUP))) continue;
        }
        return -1;
    }
    return 0;
}

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

/* Why the last connection attempt failed, logged once per distinct reason (the caller retries
 * with a backoff, a log line per attempt would only repeat itself) */
static char g_last_fail[128];

int mqtt_init(const char *host, int port, const char *client_id, const char *user, const char *pass) {
    /* Every call replaces all four: a cleared user or password must not survive from the last
     * configuration (the settings page reconnects through here after each save) */
    snprintf(g_host, sizeof(g_host), "%s", host ? host : "");
    g_port = port > 0 ? port : 1883;
    if (client_id && client_id[0]) snprintf(g_client_id, sizeof(g_client_id), "%s", client_id);
    snprintf(g_user, sizeof(g_user), "%s", user ? user : "");
    snprintf(g_pass, sizeof(g_pass), "%s", pass ? pass : "");
    g_last_fail[0] = '\0';

    g_sub_count = 0;
    g_connected = 0;
    g_fd = -1;
    g_rxlen = 0;
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
    g_rxlen = 0; /* a partial packet of this connection must not prefix the next one */
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

static int connect_failed(int sock, const char *fmt, ...) {
    char why[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    if (strcmp(why, g_last_fail) != 0) {
        snprintf(g_last_fail, sizeof(g_last_fail), "%s", why);
        fprintf(stderr, "[-] MQTT: cannot connect to %s:%d: %s\n", g_host, g_port, why);
    }
    if (sock >= 0) close(sock);
    return -1;
}

/* CONNACK return codes of MQTT 3.1.1 (section 3.2.2.3) */
static const char *connack_reason(uint8_t rc) {
    switch (rc) {
        case 1: return "refused: unacceptable protocol version";
        case 2: return "refused: client id rejected";
        case 3: return "refused: server unavailable";
        case 4: return "refused: bad user name or password";
        case 5: return "refused: not authorized";
        default: return "refused: unknown reason";
    }
}

int mqtt_connect(void) {
    mqtt_disconnect();

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(g_port);

    if (inet_aton(g_host, &saddr.sin_addr) == 0) {
        struct hostent *he = gethostbyname(g_host);
        if (!he || !he->h_addr_list || !he->h_addr_list[0]) return connect_failed(-1, "host name not resolved");
        memcpy(&saddr.sin_addr, he->h_addr_list[0], sizeof(saddr.sin_addr));
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return connect_failed(-1, "socket: %s", strerror(errno));

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
        if (errno != EINPROGRESS) return connect_failed(sock, "%s", strerror(errno));
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        int sel = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sel < 0) return connect_failed(sock, "select: %s", strerror(errno));
        if (sel == 0) return connect_failed(sock, "no answer within 2 s");
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            return connect_failed(sock, "%s", strerror(err ? err : errno));
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
    /* MQTT 3.1.1 3.1.2.9: no password without a user name (the broker would close the
     * connection), even if a stale password is loaded */
    int send_pass = g_user[0] && g_pass[0];
    if (g_user[0]) flags |= 0x80;
    if (send_pass) flags |= 0x40;
    var_hdr[7] = flags;

    size_t cid_len = strlen(g_client_id);
    size_t payload_len = 2 + cid_len;
    if (g_lwt_topic[0]) {
        payload_len += 2 + strlen(g_lwt_topic);
        payload_len += 2 + strlen(g_lwt_payload);
    }
    if (g_user[0]) payload_len += 2 + strlen(g_user);
    if (send_pass) payload_len += 2 + strlen(g_pass);

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
    if (send_pass) {
        size_t plen = strlen(g_pass);
        *p++ = (uint8_t)((plen >> 8) & 0xFF);
        *p++ = (uint8_t)(plen & 0xFF);
        memcpy(p, g_pass, plen);
        p += plen;
    }

    size_t total_len = (size_t)(p - pkt);
    if (send(sock, pkt, total_len, 0) != (ssize_t)total_len) {
        return connect_failed(sock, "CONNECT not sent: %s", strerror(errno));
    }

    /* Read CONNACK */
    uint8_t ack[4];
    ssize_t got = recv(sock, ack, 4, 0);
    if (got != 4 || ack[0] != 0x20) {
        return connect_failed(sock, got <= 0 ? "connection closed or no CONNACK within 2 s" : "unexpected reply to CONNECT");
    }
    if (ack[3] != 0x00) {
        return connect_failed(sock, "%s (code %u)%s", connack_reason(ack[3]), (unsigned)ack[3],
                              (ack[3] == 4 || ack[3] == 5) && !g_pass[0] ? ", no password stored" : "");
    }
    g_last_fail[0] = '\0';

    /* Switch to non-blocking */
    int f = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, f | O_NONBLOCK);

    g_rxlen = 0;
    g_fd = sock;
    g_last_ping = time(NULL);

    /* Subscriptions first, then the birth message: the broker handles one connection's packets in
     * order, so whoever sees "online" can send a command and it reaches a subscription. With the
     * birth first (clean session), a command sent right after "online" was dropped. */
    for (size_t i = 0; i < g_sub_count; i++) {
        if (subscribe_packet(g_subs[i].topic) != 0) return -1;
    }

    if (g_birth_topic[0] &&
        publish_packet(g_birth_topic, g_birth_payload, strlen(g_birth_payload), 0, g_birth_retain) != 0) {
        return -1;
    }

    /* Only now may the main loop publish (see g_connected) */
    __sync_synchronize();
    g_connected = 1;
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

int mqtt_publishing_live(void) {
    if (!g_dry_run) return 1;
#ifdef TEST_HARNESS
    const char *live = getenv("HARMONY_MQTT_LIVE");
    if (live && live[0] == '1') return 1;
#endif
    return 0;
}

int mqtt_publish_ex(const char *topic, const char *payload, size_t plen, int qos, int retain) {
    if (!mqtt_publishing_live()) {
        printf("[DRY-RUN] MQTT Publish simulated: topic=%s payload=%.*s retain=%d\n", topic ? topic : "",
               (int)(plen > 200 ? 200 : plen), payload ? payload : "", retain);
        return 0;
    }
    if (!mqtt_is_connected() || !topic) return -1;
    return publish_packet(topic, payload, plen, qos, retain);
}

/* Builds and sends one PUBLISH on the current socket (no connected check: mqtt_connect() sends
 * the birth message before the connection is announced) */
static int publish_packet(const char *topic, const char *payload, size_t plen, int qos, int retain) {
    if (g_fd < 0 || !topic) return -1;
    if (!payload) plen = 0;
    size_t tlen = strlen(topic);
    if (qos < 0) qos = 0;
    if (qos > 1) qos = 1;
    size_t rem_len = 2 + tlen + (qos ? 2 : 0) + plen;
    if (rem_len + 5 > MQTT_MAX_PACKET) {
        fprintf(stderr, "[-] MQTT: %s not published, %zu bytes is over the %d byte limit\n", topic, rem_len,
                MQTT_MAX_PACKET);
        return -1;
    }
    uint8_t stack_pkt[MQTT_BUF_SIZE];
    uint8_t *pkt = stack_pkt;
    if (rem_len + 5 > sizeof(stack_pkt)) {
        pkt = (uint8_t *)malloc(rem_len + 5);
        if (!pkt) return -1;
    }
    pkt[0] = (uint8_t)(0x30 | (qos << 1) | (retain ? 0x01 : 0x00));
    uint8_t *p = encode_remaining_len(&pkt[1], rem_len);
    *p++ = (uint8_t)((tlen >> 8) & 0xFF);
    *p++ = (uint8_t)(tlen & 0xFF);
    memcpy(p, topic, tlen);
    p += tlen;
    if (qos) {
        uint16_t pid = g_packet_id++;
        if (g_packet_id == 0) g_packet_id = 1;
        *p++ = (uint8_t)((pid >> 8) & 0xFF);
        *p++ = (uint8_t)(pid & 0xFF);
    }
    if (plen > 0) {
        memcpy(p, payload, plen);
        p += plen;
    }
    size_t total = (size_t)(p - pkt);
    int rc = send_all(g_fd, pkt, total);
    if (pkt != stack_pkt) free(pkt);
    if (rc != 0) {
        mqtt_disconnect();
        return -1;
    }
    return 0;
}

int mqtt_publish(const char *topic, const char *payload, int retain) {
    return mqtt_publish_ex(topic, payload, payload ? strlen(payload) : 0, 0, retain);
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
    return subscribe_packet(topic);
}

/* Sends one SUBSCRIBE on the current socket (mqtt_connect() resubscribes before the connection
 * is announced) */
static int subscribe_packet(const char *topic) {
    if (g_fd < 0 || !topic) return -1;
    size_t tlen = strlen(topic);
    if (tlen > 200) return -1;
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

    *p++ = 0x01; /* QoS 1: commands are delivered with PUBACK (acked in mqtt_tick) */

    size_t total = (size_t)(p - pkt);
    if (send_all(g_fd, pkt, total) != 0) {
        mqtt_disconnect();
        return -1;
    }
    return 0;
}

static int mqtt_topic_matches(const char *sub, const char *pub) {
    if (!sub || !pub) return 0;
    while (*sub && *pub) {
        if (*sub == '#') return 1;
        if (*sub == '+') {
            while (*pub && *pub != '/') pub++;
            sub++;
            continue;
        }
        if (*sub != *pub) return 0;
        sub++;
        pub++;
    }
    if (*sub == '#' || (*sub == '+' && !*pub)) return 1;
    return (*sub == '\0' && *pub == '\0');
}

int mqtt_tick(void) {
    if (!mqtt_is_connected()) {
        return 0;
    }

    time_t now = time(NULL);
    if (now - g_last_ping >= MQTT_KEEPALIVE_SEC) {
        uint8_t ping[2] = {0xC0, 0x00};
        if (send_all(g_fd, ping, 2) != 0) {
            mqtt_disconnect();
            return 0;
        }
        g_last_ping = now;
    }

    /* Read what is there and process every complete packet; a partial packet waits for the
     * next tick (one TCP read may carry several MQTT packets, or a piece of one) */
    uint8_t *rxbuf = g_rxbuf;
    const int fd = g_fd;
    ssize_t n = recv(g_fd, rxbuf + g_rxlen, sizeof(g_rxbuf) - g_rxlen, MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        mqtt_disconnect();
        return -1;
    }
    if (n == 0) {
        mqtt_disconnect();
        return -1;
    }
    g_rxlen += (size_t)n;
    size_t pos = 0;
    while (pos + 2 <= g_rxlen) {
        size_t rem_len = 0;
        size_t rlen_bytes = decode_remaining_len(rxbuf + pos + 1, g_rxlen - pos - 1, &rem_len);
        if (rlen_bytes == 0) {
            if (g_rxlen - pos < 5) break; /* the length field is not complete yet */
            g_rxlen = 0;                   /* garbage: drop the buffer */
            return 0;
        }
        size_t pkt_len = 1 + rlen_bytes + rem_len;
        if (pkt_len > sizeof(g_rxbuf)) {
            g_rxlen = 0; /* larger than we can ever hold */
            return 0;
        }
        if (pos + pkt_len > g_rxlen) break; /* incomplete: wait for the rest */
        const uint8_t *pkt = rxbuf + pos;
        uint8_t type = pkt[0] & 0xF0;
        if (type == 0x30) { /* PUBLISH */
            int qos = (pkt[0] >> 1) & 0x03;
            const uint8_t *p = pkt + 1 + rlen_bytes;
            const uint8_t *end = pkt + pkt_len;
            if (p + 2 <= end) {
                size_t tlen = ((size_t)p[0] << 8) | p[1];
                p += 2;
                if (p + tlen <= end) {
                    char topic[256] = {0};
                    size_t cplen = tlen < sizeof(topic) - 1 ? tlen : sizeof(topic) - 1;
                    memcpy(topic, p, cplen);
                    p += tlen;
                    if (qos > 0 && p + 2 <= end) {
                        uint8_t ack[4] = { 0x40, 0x02, p[0], p[1] };
                        send_all(g_fd, ack, 4);
                        p += 2;
                    }
                    size_t plen = (size_t)(end - p);
                    char *payload = (char *)malloc(plen + 1);
                    if (payload) {
                        memcpy(payload, p, plen);
                        payload[plen] = '\0';
                        for (size_t i = 0; i < g_sub_count; i++) {
                            if (mqtt_topic_matches(g_subs[i].topic, topic) && g_subs[i].cb) {
                                g_subs[i].cb(topic, payload, plen, g_subs[i].user_data);
                            }
                        }
                        free(payload);
                    }
                    /* A callback that ended the connection (settings saved over MQTT) also
                     * emptied the buffer: the rest belonged to the old connection */
                    if (g_fd != fd) return 0;
                }
            }
        }
        /* PUBACK (0x40), SUBACK (0x90), PINGRESP (0xD0): nothing to do */
        pos += pkt_len;
    }
    if (pos > 0 && pos < g_rxlen) memmove(rxbuf, rxbuf + pos, g_rxlen - pos);
    g_rxlen -= pos;
    return 1;
}
