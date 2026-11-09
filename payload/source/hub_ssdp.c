/* SSDP responder, see hub_ssdp.h. */
#define _GNU_SOURCE
#include "hub_ssdp.h"
#include "hub_info.h"
#include "ws_server.h"
#include "hub_network_util.h"
#include "harmony_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SSDP_ALIVE_INTERVAL_S 900
#define SSDP_MAX_AGE 1800

static int g_fd = -1;
static int g_port = SSDP_PORT;
static int g_shim_port = 8088;
static int g_enabled;
static int g_initialized;
static int g_disabled;

/* The test build (hub_daemon_test, -DTEST_HARNESS) must never answer or announce on a real
 * network: a test run would show up in Home Assistant's core Harmony integration as a
 * discovered "Test Hub". There the responder runs only on a test port (not 1900) and only over
 * loopback: group membership and the multicast interface are 127.0.0.1, multicast TTL 0. The
 * Hub's build is unchanged. */
#ifdef TEST_HARNESS
#define SSDP_LOOPBACK_ONLY 1
#else
#define SSDP_LOOPBACK_ONLY 0
#endif
static struct hub_settings g_settings;
static int g_have_settings;
static char g_uuid[40] = "";
static char g_ip[32] = "127.0.0.1";
static time_t g_last_alive;

/* UUID v5 (SHA-1) of hub_id in a fixed namespace, so it never changes for a unit */
static void make_uuid(void) {
    static const uint8_t ns[16] = { 0x4f, 0x1d, 0x8a, 0x36, 0x2c, 0x9b, 0x4e, 0x7a, 0x9e, 0x3f, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    const char *id = hub_info_id();
    uint8_t buf[16 + 64];
    size_t n = strlen(id);
    if (n > 64) n = 64;
    memcpy(buf, ns, 16);
    memcpy(buf + 16, id, n);
    uint8_t d[20];
    ws_sha1(buf, 16 + n, d);
    d[6] = (uint8_t)((d[6] & 0x0F) | 0x50);
    d[8] = (uint8_t)((d[8] & 0x3F) | 0x80);
    snprintf(g_uuid, sizeof(g_uuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
}

const char *hub_ssdp_uuid(void) {
    if (!g_uuid[0]) make_uuid();
    return g_uuid;
}

static void active_ip(char *out, size_t cap) {
    char iface[32] = {0}, gw[32] = {0}, mask[32] = {0}, mac[32] = {0}, ip[32] = {0};
    net_get_network_details(iface, sizeof(iface), gw, sizeof(gw), mask, sizeof(mask), mac, sizeof(mac));
    if (iface[0]) net_get_iface_ip_mask_mac(iface, ip, sizeof(ip), mask, sizeof(mask), mac, sizeof(mac));
    if (!ip[0]) net_get_iface_ip_mask_mac("ath0", ip, sizeof(ip), mask, sizeof(mask), mac, sizeof(mac));
    snprintf(out, cap, "%s", ip[0] ? ip : "127.0.0.1");
}

int hub_ssdp_build_response(const char *st, const char *ip, int shim_port, char *out, size_t cap) {
    const char *reply_st = (st && strcasecmp(st, "ssdp:all") != 0) ? st : SSDP_URN;
    /* UPnP 1.0: the USN trails the ST it answers; for the root device that is upnp:rootdevice */
    const char *usn_tail = strcasecmp(reply_st, "upnp:rootdevice") == 0 ? "upnp:rootdevice" : SSDP_URN;
    int n = snprintf(out, cap,
                     "HTTP/1.1 200 OK\r\n"
                     "CACHE-CONTROL: max-age=%d\r\n"
                     "EXT:\r\n"
                     "LOCATION: http://%s:%d/description\r\n"
                     "SERVER: Linux/2.6 UPnP/1.0 HarmonyNG/%s\r\n"
                     "ST: %s\r\n"
                     "USN: uuid:%s::%s\r\n"
                     "\r\n",
                     SSDP_MAX_AGE, ip, shim_port, HARMONY_VERSION, reply_st, hub_ssdp_uuid(), usn_tail);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

char *hub_ssdp_description_xml(void) {
    struct hub_settings s;
    if (g_have_settings) s = g_settings;
    else config_store_get_settings(&s);
    const char *name = s.hub_name[0] ? s.hub_name : "Harmony Hub";
    /* escape the name for XML */
    char esc[192];
    size_t o = 0;
    for (const char *p = name; *p && o + 8 < sizeof(esc); p++) {
        if (*p == '&') { memcpy(esc + o, "&amp;", 5); o += 5; }
        else if (*p == '<') { memcpy(esc + o, "&lt;", 4); o += 4; }
        else if (*p == '>') { memcpy(esc + o, "&gt;", 4); o += 4; }
        else if (*p == '"') { memcpy(esc + o, "&quot;", 6); o += 6; }
        else esc[o++] = *p;
    }
    esc[o] = '\0';
    char *xml = (char *)malloc(1536);
    if (!xml) return NULL;
    snprintf(xml, 1536,
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
             "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
             "  <specVersion><major>1</major><minor>0</minor></specVersion>\n"
             "  <device>\n"
             "    <deviceType>%s</deviceType>\n"
             "    <friendlyName>%s</friendlyName>\n"
             "    <manufacturer>Logitech</manufacturer>\n"
             "    <manufacturerURL>https://github.com/iranl/harmony-ng</manufacturerURL>\n"
             "    <modelDescription>Harmony Hub running harmony-ng</modelDescription>\n"
             "    <modelName>Harmony Hub</modelName>\n"
             "    <modelNumber>%s</modelNumber>\n"
             "    <serialNumber>%s</serialNumber>\n"
             "    <UDN>uuid:%s</UDN>\n"
             "  </device>\n"
             "</root>\n",
             SSDP_URN, esc, HARMONY_VERSION, hub_info_id(), hub_ssdp_uuid());
    return xml;
}

static void send_to(const char *msg, int len, const struct sockaddr_in *dst) {
    if (g_fd < 0 || len <= 0) return;
    struct sockaddr_in to;
    if (dst) {
        to = *dst;
    } else {
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons((uint16_t)g_port);
        inet_aton(SSDP_GROUP, &to.sin_addr);
    }
    sendto(g_fd, msg, (size_t)len, MSG_NOSIGNAL, (const struct sockaddr *)&to, sizeof(to));
}

static void send_notify(int alive) {
    char msg[512];
    int n;
    if (alive) {
        n = snprintf(msg, sizeof(msg),
                     "NOTIFY * HTTP/1.1\r\nHOST: %s:%d\r\nCACHE-CONTROL: max-age=%d\r\nLOCATION: http://%s:%d/description\r\n"
                     "NT: %s\r\nNTS: ssdp:alive\r\nSERVER: Linux/2.6 UPnP/1.0 HarmonyNG/%s\r\nUSN: uuid:%s::%s\r\n\r\n",
                     SSDP_GROUP, SSDP_PORT, SSDP_MAX_AGE, g_ip, g_shim_port, SSDP_URN, HARMONY_VERSION, hub_ssdp_uuid(), SSDP_URN);
    } else {
        n = snprintf(msg, sizeof(msg),
                     "NOTIFY * HTTP/1.1\r\nHOST: %s:%d\r\nNT: %s\r\nNTS: ssdp:byebye\r\nUSN: uuid:%s::%s\r\n\r\n",
                     SSDP_GROUP, SSDP_PORT, SSDP_URN, hub_ssdp_uuid(), SSDP_URN);
    }
    send_to(msg, n, NULL);
}

static int open_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)g_port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        fprintf(stderr, "[-] SSDP: bind 0.0.0.0:%d failed: %s\n", g_port, strerror(errno));
        close(fd);
        return -1;
    }
    /* g_ip stays the advertised address (LOCATION); the socket's interface is loopback in tests */
    const char *if_ip = SSDP_LOOPBACK_ONLY ? "127.0.0.1" : g_ip;
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    inet_aton(SSDP_GROUP, &mreq.imr_multiaddr);
    inet_aton(if_ip, &mreq.imr_interface);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        fprintf(stderr, "[!] SSDP: joining %s failed: %s (unicast searches still work)\n", SSDP_GROUP, strerror(errno));
    }
    struct in_addr ifaddr;
    inet_aton(if_ip, &ifaddr);
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) != 0 && SSDP_LOOPBACK_ONLY) {
        /* never fall back to the default (LAN) interface in a test */
        fprintf(stderr, "[-] SSDP: loopback multicast interface refused: %s, responder off\n", strerror(errno));
        close(fd);
        return -1;
    }
    /* loop 1: a listener on the Hub itself (and the test suite) sees our NOTIFYs; our own
     * socket drops them in hub_ssdp_handle (not an M-SEARCH). TTL 0 in the test build. */
    unsigned char ttl = SSDP_LOOPBACK_ONLY ? 0 : 4, loop = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    return fd;
}

static void start(void) {
    if (g_fd >= 0) return;
    active_ip(g_ip, sizeof(g_ip));
    g_fd = open_socket();
    if (g_fd < 0) return;
    send_notify(1);
    g_last_alive = time(NULL);
    printf("[+] SSDP: announcing uuid:%s at http://%s:%d/description\n", hub_ssdp_uuid(), g_ip, g_shim_port);
}

static void stop(void) {
    if (g_fd < 0) return;
    send_notify(0);
    close(g_fd);
    g_fd = -1;
    printf("[*] SSDP: stopped\n");
}

int hub_ssdp_init(int port, int shim_port) {
    /* port 0 (test harness --ssdp-port 0) keeps the responder off for good: a test instance
     * must never announce itself on the real multicast group of the network it runs on */
    if (port == 0) {
        g_disabled = 1;
        g_initialized = 1;
        return 0;
    }
    g_port = port > 0 ? port : SSDP_PORT;
    g_shim_port = shim_port > 0 ? shim_port : 8088;
    g_initialized = 1;
    if (SSDP_LOOPBACK_ONLY && g_port == SSDP_PORT) {
        /* test build without --ssdp-port (or with 1900): off, whatever legacy_enabled says */
        g_disabled = 1;
        printf("[*] SSDP: off in the test build (pass --ssdp-port N, N != %d, to test it)\n", SSDP_PORT);
        return 0;
    }
    if (!g_have_settings) {
        config_store_get_settings(&g_settings);
        g_have_settings = 1;
    }
    g_enabled = g_settings.legacy_enabled;
    if (g_enabled) start();
    return (g_enabled && g_fd < 0) ? -1 : 0;
}

int hub_ssdp_fd(void) {
    return g_fd;
}

void hub_ssdp_shutdown(void) {
    stop();
}

void hub_ssdp_apply_settings(const struct hub_settings *s) {
    if (!s) return;
    g_settings = *s;
    g_have_settings = 1;
    if (!g_initialized || g_disabled) return;
    g_enabled = s->legacy_enabled;
    if (g_enabled) start();
    else stop();
}

/* M-SEARCH with an ST we serve: unicast reply to the sender */
void hub_ssdp_handle(void) {
    if (g_fd < 0) return;
    char buf[1500];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(g_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &flen);
    if (n <= 0) return;
    buf[n] = '\0';
    if (strncasecmp(buf, "M-SEARCH", 8) != 0) return;
    char st[128] = "";
    const char *p = strcasestr(buf, "\nST:");
    if (p) {
        p += 4;
        while (*p == ' ' || *p == '\t') p++;
        size_t i = 0;
        while (*p && *p != '\r' && *p != '\n' && i + 1 < sizeof(st)) st[i++] = *p++;
        st[i] = '\0';
    }
    if (strcasecmp(st, "ssdp:all") != 0 && strcasecmp(st, "upnp:rootdevice") != 0 && strcasecmp(st, SSDP_URN) != 0) return;
    char out[512];
    int len = hub_ssdp_build_response(st, g_ip, g_shim_port, out, sizeof(out));
    send_to(out, len, &from);
}

void hub_ssdp_tick(void) {
    if (g_fd < 0) return;
    time_t now = time(NULL);
    if (now - g_last_alive >= SSDP_ALIVE_INTERVAL_S) {
        char ip[32];
        active_ip(ip, sizeof(ip));
        snprintf(g_ip, sizeof(g_ip), "%s", ip);
        send_notify(1);
        g_last_alive = now;
    }
}
