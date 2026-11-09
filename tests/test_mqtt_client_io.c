/*
 * Socket handling of mqtt_client.c against a local listener that plays the broker:
 *  - the connection is announced (mqtt_is_connected) only after the subscriptions and the birth
 *    message went out, so the main loop never writes to the socket while the connecting thread
 *    still does, and its packets come after them
 *  - a packet larger than the 4 KB stack buffer goes out whole, also when the broker reads slowly
 *    (the non-blocking send fills the socket buffer)
 *  - a partial packet left from a connection that was closed by mqtt_disconnect() does not
 *    prefix the next connection's packets
 */
#include "mqtt_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int g_dry_run = 0; /* defined by hub_daemon.c in the product */

static int g_listen = -1;
static int g_port = 0;
static int g_fail = 0;

static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_fail++;
}

/* Reads one MQTT packet (blocking); returns its total length, 0 on close */
static size_t read_packet(int c, uint8_t *buf, size_t cap) {
    size_t have = 0;
    while (have < 2) {
        ssize_t n = recv(c, buf + have, 2 - have, 0);
        if (n <= 0) return 0;
        have += (size_t)n;
    }
    size_t rem = 0, mult = 1, i = 1;
    for (;;) {
        rem += (size_t)(buf[i] & 127) * mult;
        mult *= 128;
        if (!(buf[i] & 128)) break;
        i++;
        while (have <= i) {
            ssize_t n = recv(c, buf + have, 1, 0);
            if (n <= 0) return 0;
            have += (size_t)n;
        }
    }
    size_t total = i + 1 + rem;
    if (total > cap) return 0;
    while (have < total) {
        ssize_t n = recv(c, buf + have, total - have, 0);
        if (n <= 0) return 0;
        have += (size_t)n;
    }
    return total;
}

static int accept_and_connack(void) {
    int c = accept(g_listen, NULL, NULL);
    if (c < 0) return -1;
    struct timeval tv = {3, 0}; /* a test that fails must not hang */
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    if (!read_packet(c, buf, sizeof(buf)) || buf[0] != 0x10) {
        close(c);
        return -1;
    }
    const uint8_t ack[4] = {0x20, 0x02, 0x00, 0x00};
    send(c, ack, sizeof(ack), 0);
    return c;
}

/* ---- 1. order: SUBSCRIBE, birth PUBLISH, then what the main loop publishes ---- */
static uint8_t g_types[8];
static int g_ntypes;

static void *broker_order(void *arg) {
    (void)arg;
    int c = accept_and_connack();
    if (c < 0) return NULL;
    static uint8_t buf[8192];
    while (g_ntypes < 3) {
        size_t n = read_packet(c, buf, sizeof(buf));
        if (!n) break;
        g_types[g_ntypes++] = buf[0] & 0xF0;
    }
    usleep(100000);
    close(c);
    return NULL;
}

/* ---- 2. a 40 KB publish read slowly by the broker ---- */
static size_t g_big_len;
static int g_big_ok;

static void *broker_big(void *arg) {
    size_t want = *(size_t *)arg;
    int c = accept_and_connack();
    if (c < 0) return NULL;
    usleep(300000); /* let the client's send buffer fill up */
    uint8_t *buf = malloc(want + 64);
    size_t n = buf ? read_packet(c, buf, want + 64) : 0;
    g_big_len = n;
    if (n && (buf[0] & 0xF0) == 0x30) {
        size_t tl = ((size_t)buf[4] << 8) | buf[5]; /* 3 length bytes for this size */
        const uint8_t *pl = buf + 6 + tl + 2;        /* QoS 1: packet id */
        size_t plen = n - (size_t)(pl - buf);
        g_big_ok = plen == want;
        for (size_t i = 0; g_big_ok && i < plen; i++) g_big_ok = pl[i] == (uint8_t)('a' + i % 26);
    }
    free(buf);
    usleep(100000);
    close(c);
    return NULL;
}

/* ---- 3. stale partial packet after mqtt_disconnect() ---- */
static char g_got_topic[128];
static void on_msg(const char *topic, const char *payload, size_t len, void *ud) {
    (void)payload;
    (void)len;
    (void)ud;
    snprintf(g_got_topic, sizeof(g_got_topic), "%s", topic);
}

static int g_stage;

static void *broker_partial(void *arg) {
    (void)arg;
    /* first connection: CONNACK, the SUBSCRIBE, then 3 bytes of a PUBLISH that never completes */
    int c = accept_and_connack();
    if (c < 0) return NULL;
    uint8_t buf[512];
    read_packet(c, buf, sizeof(buf));
    const uint8_t partial[3] = {0x30, 0x7F, 0x00};
    send(c, partial, sizeof(partial), 0);
    for (int i = 0; i < 300 && g_stage < 1; i++) usleep(10000);
    close(c);
    /* second connection: CONNACK, the SUBSCRIBE, then one whole PUBLISH on t/x */
    c = accept_and_connack();
    if (c < 0) return NULL;
    read_packet(c, buf, sizeof(buf));
    const uint8_t pub[] = {0x30, 0x07, 0x00, 0x03, 't', '/', 'x', 'h', 'i'};
    send(c, pub, sizeof(pub), 0);
    for (int i = 0; i < 300 && g_stage < 2; i++) usleep(10000);
    close(c);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    int small = 2048;
    setsockopt(g_listen, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)); /* inherited by accept */
    socklen_t al = sizeof(a);
    if (g_listen < 0 || bind(g_listen, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(g_listen, 4) < 0 ||
        getsockname(g_listen, (struct sockaddr *)&a, &al) < 0) {
        perror("listener");
        return 1;
    }
    g_port = ntohs(a.sin_port);

    /* 1 */
    pthread_t t;
    pthread_create(&t, NULL, broker_order, NULL);
    mqtt_init("127.0.0.1", g_port, "io-test", "", "");
    mqtt_subscribe("t/cmd", on_msg, NULL);
    mqtt_set_birth("t/status", "online", 1);
    mqtt_connect_async();
    for (int i = 0; i < 200 && !mqtt_is_connected(); i++) usleep(5000);
    check("connection announced", mqtt_is_connected());
    mqtt_publish("t/state", "x", 0); /* what the main loop sends as soon as it sees the connection */
    pthread_join(t, NULL);
    check("broker got SUBSCRIBE, then the birth, then the main loop's publish",
          g_ntypes == 3 && g_types[0] == 0x80 && g_types[1] == 0x30 && g_types[2] == 0x30);
    mqtt_disconnect();

    /* 2 */
    size_t want = 40000;
    char *payload = malloc(want);
    for (size_t i = 0; i < want; i++) payload[i] = (char)('a' + i % 26);
    pthread_create(&t, NULL, broker_big, &want);
    mqtt_init("127.0.0.1", g_port, "io-test", "", "");
    mqtt_set_birth("", NULL, 0);
    check("connect for the large publish", mqtt_connect() == 0 && mqtt_is_connected());
    int rc = mqtt_publish_ex("t/big", payload, want, 1, 1);
    pthread_join(t, NULL);
    check("40 KB publish sent whole to a slow reader", rc == 0 && g_big_ok);
    check("still connected after the large publish", mqtt_is_connected());
    mqtt_disconnect();
    free(payload);

    /* 3 */
    pthread_create(&t, NULL, broker_partial, NULL);
    mqtt_init("127.0.0.1", g_port, "io-test", "", "");
    mqtt_subscribe("t/x", on_msg, NULL);
    mqtt_connect();
    for (int i = 0; i < 20; i++) {
        mqtt_tick();
        usleep(10000);
    }
    mqtt_disconnect(); /* e.g. the settings were saved: the partial packet stays behind */
    g_stage = 1;
    mqtt_connect();
    for (int i = 0; i < 50 && !g_got_topic[0]; i++) {
        mqtt_tick();
        usleep(10000);
    }
    g_stage = 2;
    pthread_join(t, NULL);
    check("message after a reconnect delivered (no stale bytes in front)", strcmp(g_got_topic, "t/x") == 0);
    mqtt_disconnect();

    close(g_listen);
    printf("%s\n", g_fail ? "MQTT client I/O: FAILED" : "MQTT client I/O: all passed");
    return g_fail ? 1 : 0;
}
