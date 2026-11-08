/*
 * CONNECT flags of mqtt_client.c (MQTT 3.1.1 3.1.2.8/3.1.2.9): a password goes out only with a
 * user name. A local listener reads the CONNECT, answers CONNACK 0 and records the flags byte.
 */
#include "mqtt_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int g_dry_run = 0; /* defined by hub_daemon.c in the product */

static int g_listen = -1;
static int g_port = 0;
static int g_flags = -1;
static int g_fail = 0;

static void *broker(void *arg) {
    (void)arg;
    int c = accept(g_listen, NULL, NULL);
    if (c < 0) return NULL;
    uint8_t buf[512];
    ssize_t n = recv(c, buf, sizeof(buf), 0);
    /* fixed header (1 byte + 1 length byte for these short packets), then "\0\4MQTT", level, flags */
    if (n >= 10 && buf[0] == 0x10) g_flags = buf[9];
    const uint8_t ack[4] = {0x20, 0x02, 0x00, 0x00};
    send(c, ack, sizeof(ack), 0);
    usleep(100000);
    close(c);
    return NULL;
}

static int connect_flags(const char *user, const char *pass) {
    g_flags = -1;
    pthread_t t;
    pthread_create(&t, NULL, broker, NULL);
    mqtt_init("127.0.0.1", g_port, "flags-test", user, pass);
    mqtt_connect();
    pthread_join(t, NULL);
    mqtt_disconnect();
    return g_flags;
}

static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_fail++;
}

int main(void) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    socklen_t al = sizeof(a);
    if (g_listen < 0 || bind(g_listen, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(g_listen, 4) < 0 ||
        getsockname(g_listen, (struct sockaddr *)&a, &al) < 0) {
        perror("listener");
        return 1;
    }
    g_port = ntohs(a.sin_port);

    int f = connect_flags("user", "secret");
    check("user and password: both flags", f >= 0 && (f & 0xC0) == 0xC0);
    f = connect_flags("user", "");
    check("user only: user flag, no password flag", f >= 0 && (f & 0xC0) == 0x80);
    f = connect_flags("", "stale");
    check("password without a user name: neither flag", f >= 0 && (f & 0xC0) == 0x00);
    f = connect_flags("", "");
    check("no credentials: neither flag", f >= 0 && (f & 0xC0) == 0x00);

    close(g_listen);
    printf("%s\n", g_fail ? "MQTT CONNECT flags: FAILED" : "MQTT CONNECT flags: all passed");
    return g_fail ? 1 : 0;
}
