/* mDNS / DNS-SD responder, see hub_mdns.h. */
#define _GNU_SOURCE
#include "hub_mdns.h"
#include "hub_info.h"
#include "hub_mqtt.h"
#include "http_auth.h"
#include "hub_network_util.h"
#include "harmony_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------------------------------ */
/* Packet builder                                                                              */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    /* offsets of names written so far, for compression: each entry is a suffix starting at a
     * label boundary */
    struct { char name[256]; uint16_t off; } names[48];
    int nnames;
} pkt_t;

static int put_u8(pkt_t *p, uint8_t v) {
    if (p->len + 1 > p->cap) return -1;
    p->buf[p->len++] = v;
    return 0;
}

static int put_u16(pkt_t *p, uint16_t v) {
    if (p->len + 2 > p->cap) return -1;
    p->buf[p->len++] = (uint8_t)(v >> 8);
    p->buf[p->len++] = (uint8_t)v;
    return 0;
}

static int put_u32(pkt_t *p, uint32_t v) {
    return put_u16(p, (uint16_t)(v >> 16)) || put_u16(p, (uint16_t)v);
}

/* Writes a dotted name with compression against the names already in the packet */
static int put_name(pkt_t *p, const char *name) {
    const char *s = name;
    while (*s) {
        /* a known suffix? */
        for (int i = 0; i < p->nnames; i++) {
            if (mdns_name_eq(p->names[i].name, s)) {
                return put_u16(p, (uint16_t)(0xC000 | p->names[i].off));
            }
        }
        const char *dot = strchr(s, '.');
        size_t ll = dot ? (size_t)(dot - s) : strlen(s);
        if (ll == 0 || ll > 63) return -1;
        if (p->len < 0x3FFF && p->nnames < (int)(sizeof(p->names) / sizeof(p->names[0]))) {
            snprintf(p->names[p->nnames].name, sizeof(p->names[p->nnames].name), "%s", s);
            p->names[p->nnames].off = (uint16_t)p->len;
            p->nnames++;
        }
        if (put_u8(p, (uint8_t)ll)) return -1;
        if (p->len + ll > p->cap) return -1;
        memcpy(p->buf + p->len, s, ll);
        p->len += ll;
        s = dot ? dot + 1 : s + ll;
    }
    return put_u8(p, 0);
}

static int put_rr_head(pkt_t *p, const char *name, uint16_t type, uint16_t cls, uint32_t ttl) {
    return put_name(p, name) || put_u16(p, type) || put_u16(p, cls) || put_u32(p, ttl);
}

/* Writes an RR whose rdata is a (compressible) name */
static int put_rr_name(pkt_t *p, const char *name, uint16_t type, uint16_t cls, uint32_t ttl, const char *rdname) {
    if (put_rr_head(p, name, type, cls, ttl)) return -1;
    size_t len_off = p->len;
    if (put_u16(p, 0)) return -1;
    if (put_name(p, rdname)) return -1;
    uint16_t rdlen = (uint16_t)(p->len - len_off - 2);
    p->buf[len_off] = (uint8_t)(rdlen >> 8);
    p->buf[len_off + 1] = (uint8_t)rdlen;
    return 0;
}

static const char *service_type(void) { return MDNS_SERVICE; }

static int put_record(pkt_t *p, const mdns_info_t *info, unsigned which, int ttl_zero) {
    uint32_t ttl_s = ttl_zero ? 0 : MDNS_TTL_SHORT;
    uint32_t ttl_l = ttl_zero ? 0 : MDNS_TTL_LONG;
    switch (which) {
        case MDNS_R_PTR:
            return put_rr_name(p, service_type(), 12, 0x0001, ttl_l, info->instance);
        case MDNS_R_ENUM:
            return put_rr_name(p, MDNS_ENUM_SERVICE, 12, 0x0001, ttl_l, service_type());
        case MDNS_R_SRV: {
            if (put_rr_head(p, info->instance, 33, 0x8001, ttl_s)) return -1;
            size_t len_off = p->len;
            if (put_u16(p, 0) || put_u16(p, 0) || put_u16(p, 0) || put_u16(p, info->port)) return -1;
            if (put_name(p, info->hostname)) return -1;
            uint16_t rdlen = (uint16_t)(p->len - len_off - 2);
            p->buf[len_off] = (uint8_t)(rdlen >> 8);
            p->buf[len_off + 1] = (uint8_t)rdlen;
            return 0;
        }
        case MDNS_R_TXT:
            if (put_rr_head(p, info->instance, 16, 0x8001, ttl_l)) return -1;
            if (put_u16(p, (uint16_t)info->txt_len)) return -1;
            if (p->len + info->txt_len > p->cap) return -1;
            memcpy(p->buf + p->len, info->txt, info->txt_len);
            p->len += info->txt_len;
            return 0;
        case MDNS_R_A:
            if (put_rr_head(p, info->hostname, 1, 0x8001, ttl_s)) return -1;
            if (put_u16(p, 4)) return -1;
            if (p->len + 4 > p->cap) return -1;
            memcpy(p->buf + p->len, info->ip, 4);
            p->len += 4;
            return 0;
        default:
            return -1;
    }
}

static int count_bits(unsigned m) {
    int n = 0;
    for (unsigned b = 1; b <= MDNS_R_ENUM; b <<= 1) if (m & b) n++;
    return n;
}

static const unsigned k_order[] = { MDNS_R_ENUM, MDNS_R_PTR, MDNS_R_SRV, MDNS_R_TXT, MDNS_R_A };

int mdns_build_response(const mdns_info_t *info, unsigned answers, unsigned additionals,
                        int ttl_zero, uint16_t id, uint8_t *out, size_t cap) {
    if (!info || !out || cap < 12) return -1;
    additionals &= ~answers;
    pkt_t p = { out, cap, 0, {{{0}, 0}}, 0 };
    if (put_u16(&p, id) || put_u16(&p, 0x8400) || put_u16(&p, 0) ||
        put_u16(&p, (uint16_t)count_bits(answers)) || put_u16(&p, 0) ||
        put_u16(&p, (uint16_t)count_bits(additionals))) return -1;
    for (size_t i = 0; i < sizeof(k_order) / sizeof(k_order[0]); i++) {
        if ((answers & k_order[i]) && put_record(&p, info, k_order[i], ttl_zero)) return -1;
    }
    for (size_t i = 0; i < sizeof(k_order) / sizeof(k_order[0]); i++) {
        if ((additionals & k_order[i]) && put_record(&p, info, k_order[i], ttl_zero)) return -1;
    }
    return (int)p.len;
}

int mdns_build_probe(const mdns_info_t *info, uint8_t *out, size_t cap) {
    if (!info || !out || cap < 12) return -1;
    pkt_t p = { out, cap, 0, {{{0}, 0}}, 0 };
    if (put_u16(&p, 0) || put_u16(&p, 0) || put_u16(&p, 2) || put_u16(&p, 0) || put_u16(&p, 2) || put_u16(&p, 0)) return -1;
    /* questions: ANY, QU */
    if (put_name(&p, info->instance) || put_u16(&p, 255) || put_u16(&p, 0x8001)) return -1;
    if (put_name(&p, info->hostname) || put_u16(&p, 255) || put_u16(&p, 0x8001)) return -1;
    /* authority: the records we propose */
    if (put_record(&p, info, MDNS_R_SRV, 0) || put_record(&p, info, MDNS_R_A, 0)) return -1;
    return (int)p.len;
}

size_t mdns_build_txt(char *out, size_t cap, const char *hub_id, const char *version, int api_version,
                      int auth, int mqtt, const char *topic, int legacy, const char *name) {
    size_t n = 0;
    char item[160];
    const char *items[8];
    int cnt = 0;
    char b_id[64], b_ver[48], b_api[16], b_auth[8], b_mqtt[8], b_topic[150], b_legacy[10], b_name[80];
    snprintf(b_id, sizeof(b_id), "id=%s", hub_id ? hub_id : "");
    snprintf(b_ver, sizeof(b_ver), "ver=%s", version ? version : "");
    snprintf(b_api, sizeof(b_api), "api=%d", api_version);
    snprintf(b_auth, sizeof(b_auth), "auth=%d", auth ? 1 : 0);
    snprintf(b_mqtt, sizeof(b_mqtt), "mqtt=%d", mqtt ? 1 : 0);
    snprintf(b_topic, sizeof(b_topic), "topic=%s", topic ? topic : "");
    snprintf(b_legacy, sizeof(b_legacy), "legacy=%d", legacy ? 1 : 0);
    snprintf(b_name, sizeof(b_name), "name=%s", name ? name : "");
    items[cnt++] = b_id;
    items[cnt++] = b_ver;
    items[cnt++] = b_api;
    items[cnt++] = b_auth;
    items[cnt++] = b_mqtt;
    if (mqtt) items[cnt++] = b_topic;
    items[cnt++] = b_legacy;
    items[cnt++] = b_name;
    for (int i = 0; i < cnt; i++) {
        size_t l = strlen(items[i]);
        if (l > 255) l = 255;
        if (n + 1 + l > cap) break;
        out[n++] = (char)l;
        memcpy(out + n, items[i], l);
        n += l;
    }
    (void)item;
    return n;
}

/* ------------------------------------------------------------------------------------------ */
/* Parser                                                                                      */

int mdns_name_eq(const char *a, const char *b) {
    return a && b && strcasecmp(a, b) == 0;
}

/* Reads a (possibly compressed) name at *pos into out; advances *pos past it in the packet */
static int read_name(const uint8_t *pkt, size_t len, size_t *pos, char *out, size_t cap) {
    size_t p = *pos, o = 0;
    int jumped = 0, hops = 0;
    size_t end = 0;
    for (;;) {
        if (p >= len) return -1;
        uint8_t l = pkt[p];
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= len) return -1;
            size_t ptr = ((size_t)(l & 0x3F) << 8) | pkt[p + 1];
            if (!jumped) end = p + 2;
            jumped = 1;
            if (++hops > 16 || ptr >= len) return -1;
            p = ptr;
            continue;
        }
        p++;
        if (l == 0) break;
        if (p + l > len) return -1;
        if (o && o + 1 < cap) out[o++] = '.';
        for (size_t i = 0; i < l; i++) {
            if (o + 1 < cap) out[o++] = (char)pkt[p + i];
        }
        p += l;
    }
    out[o < cap ? o : cap - 1] = '\0';
    *pos = jumped ? end : p;
    return 0;
}

int mdns_parse_query(const uint8_t *pkt, size_t len, mdns_query_t *q) {
    if (!pkt || !q || len < 12) return -1;
    memset(q, 0, sizeof(*q));
    q->id = (uint16_t)((pkt[0] << 8) | pkt[1]);
    q->flags = (uint16_t)((pkt[2] << 8) | pkt[3]);
    int qd = (pkt[4] << 8) | pkt[5];
    int an = (pkt[6] << 8) | pkt[7];
    size_t pos = 12;
    for (int i = 0; i < qd; i++) {
        mdns_rr_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (read_name(pkt, len, &pos, tmp.name, sizeof(tmp.name))) return -1;
        if (pos + 4 > len) return -1;
        tmp.type = (uint16_t)((pkt[pos] << 8) | pkt[pos + 1]);
        uint16_t c = (uint16_t)((pkt[pos + 2] << 8) | pkt[pos + 3]);
        tmp.unicast = (c & 0x8000) != 0;
        tmp.cls = c & 0x7FFF;
        pos += 4;
        if (q->nq < 8) q->q[q->nq++] = tmp;
    }
    for (int i = 0; i < an; i++) {
        mdns_rr_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (read_name(pkt, len, &pos, tmp.name, sizeof(tmp.name))) return -1;
        if (pos + 10 > len) return -1;
        tmp.type = (uint16_t)((pkt[pos] << 8) | pkt[pos + 1]);
        tmp.cls = (uint16_t)(((pkt[pos + 2] << 8) | pkt[pos + 3]) & 0x7FFF);
        tmp.ttl = ((uint32_t)pkt[pos + 4] << 24) | ((uint32_t)pkt[pos + 5] << 16) | ((uint32_t)pkt[pos + 6] << 8) | pkt[pos + 7];
        size_t rdlen = ((size_t)pkt[pos + 8] << 8) | pkt[pos + 9];
        pos += 10;
        if (pos + rdlen > len) return -1;
        if (tmp.type == 12) {
            size_t rp = pos;
            read_name(pkt, len, &rp, tmp.rdname, sizeof(tmp.rdname));
        }
        pos += rdlen;
        if (q->nan < 8) q->an[q->nan++] = tmp;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Info                                                                                        */

static void label_safe(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        out[o++] = (isalnum(c) || c == ' ' || c == '_' || c == '-') ? (char)c : '-';
    }
    out[o] = '\0';
    /* no leading/trailing blanks, no empty name */
    while (o > 0 && out[o - 1] == ' ') out[--o] = '\0';
    size_t s = 0;
    while (out[s] == ' ') s++;
    if (s) memmove(out, out + s, o - s + 1);
    if (!out[0]) snprintf(out, cap, "Harmony Hub");
}

static void active_ip(uint8_t ip[4]) {
    char iface[32] = {0}, gw[32] = {0}, mask[32] = {0}, mac[32] = {0};
    char ipstr[32] = {0};
    net_get_network_details(iface, sizeof(iface), gw, sizeof(gw), mask, sizeof(mask), mac, sizeof(mac));
    if (iface[0]) net_get_iface_ip_mask_mac(iface, ipstr, sizeof(ipstr), mask, sizeof(mask), mac, sizeof(mac));
    if (!ipstr[0]) net_get_iface_ip_mask_mac("ath0", ipstr, sizeof(ipstr), mask, sizeof(mask), mac, sizeof(mac));
    struct in_addr a;
    if (ipstr[0] && inet_aton(ipstr, &a)) {
        memcpy(ip, &a.s_addr, 4);
    } else {
        ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1;
    }
}

/* active_ip() falls back to 127.0.0.1 when no interface has an address yet (hub_daemon
 * starts before udhcpc has finished on the Hub): nobody can discover us through loopback */
static int has_lan_address(const uint8_t ip[4]) {
    return ip[0] != 127;
}

void mdns_make_info(mdns_info_t *info, const struct hub_settings *settings, const char *name_suffix) {
    memset(info, 0, sizeof(*info));
    const char *raw = (settings && settings->hub_name[0]) ? settings->hub_name : "Harmony Hub";
    char safe[64];
    label_safe(raw, safe, sizeof(safe));
    if (name_suffix && name_suffix[0]) {
        snprintf(info->instance, sizeof(info->instance), "%s %s.%s", safe, name_suffix, MDNS_SERVICE);
    } else {
        snprintf(info->instance, sizeof(info->instance), "%s.%s", safe, MDNS_SERVICE);
    }
    snprintf(info->hostname, sizeof(info->hostname), "harmony-ng-%s.local", hub_info_id());
    info->port = 8089;
    active_ip(info->ip);
    int auth_on = 0;
    load_user_accounts(NULL, 0, &auth_on);
    char mhost[128] = {0}, mbase[128] = {0};
    int mport = 1883, mconn = 0;
    daemon_get_mqtt_info(mhost, sizeof(mhost), &mport, mbase, sizeof(mbase), &mconn);
    int mqtt = mhost[0] != '\0';
    info->txt_len = mdns_build_txt(info->txt, sizeof(info->txt), hub_info_id(), HARMONY_VERSION, HUB_API_VERSION,
                                   auth_on, mqtt, mbase, 0, raw);
}

/* ------------------------------------------------------------------------------------------ */
/* Responder                                                                                   */

static int g_fd = -1;
static int g_initialized;
static int g_port = MDNS_PORT;
static int g_off_for_good;   /* test build without a test port: never opens a socket */

/* The test build (hub_daemon_test, -DTEST_HARNESS) must never put a packet on a real network:
 * every test run would announce a "Test Hub" to Home Assistant on the developer's LAN. There
 * the responder runs only on a test port (not 5353) and only over loopback: group membership
 * and the outgoing multicast interface are 127.0.0.1 and the multicast TTL is 0. The Hub's
 * build is unchanged. */
#ifdef TEST_HARNESS
#define MDNS_LOOPBACK_ONLY 1
#else
#define MDNS_LOOPBACK_ONLY 0
#endif
static int g_enabled = 1;
static mdns_info_t g_info;
static char g_name_suffix[32];
static struct hub_settings g_settings;
static int g_have_settings;
static enum { ST_OFF, ST_PROBING, ST_ANNOUNCING, ST_RUNNING } g_state = ST_OFF;
static int g_probes_sent;
static int g_announces_sent;
static uint64_t g_next_action_ms;
static uint64_t g_last_poll_ms;
static int g_renames;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

static void send_to(const uint8_t *pkt, int len, const struct sockaddr_in *dst) {
    if (g_fd < 0 || len <= 0) return;
    struct sockaddr_in to;
    if (dst) {
        to = *dst;
    } else {
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons((uint16_t)g_port);
        inet_aton(MDNS_GROUP, &to.sin_addr);
    }
    sendto(g_fd, pkt, (size_t)len, MSG_NOSIGNAL, (const struct sockaddr *)&to, sizeof(to));
}

static void send_announce(int ttl_zero) {
    uint8_t pkt[MDNS_MAX_PACKET];
    int n = mdns_build_response(&g_info, MDNS_R_ALL | MDNS_R_ENUM, 0, ttl_zero, 0, pkt, sizeof(pkt));
    send_to(pkt, n, NULL);
}

static void send_probe(void) {
    uint8_t pkt[MDNS_MAX_PACKET];
    int n = mdns_build_probe(&g_info, pkt, sizeof(pkt));
    send_to(pkt, n, NULL);
}

static void rebuild_info(void) {
    mdns_make_info(&g_info, g_have_settings ? &g_settings : NULL, g_name_suffix);
}

static void start_probing(void) {
    rebuild_info();
    g_state = ST_PROBING;
    g_probes_sent = 0;
    g_announces_sent = 0;
    g_next_action_ms = now_ms();
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
        fprintf(stderr, "[-] mDNS: bind 0.0.0.0:%d failed: %s\n", g_port, strerror(errno));
        close(fd);
        return -1;
    }
    uint8_t if_ip[4];
    memcpy(if_ip, g_info.ip, 4);
    if (MDNS_LOOPBACK_ONLY) {
        if_ip[0] = 127; if_ip[1] = 0; if_ip[2] = 0; if_ip[3] = 1;
    }
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    inet_aton(MDNS_GROUP, &mreq.imr_multiaddr);
    memcpy(&mreq.imr_interface.s_addr, if_ip, 4);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        /* keep going: unicast and legacy queries still work (the harness has no multicast) */
        fprintf(stderr, "[!] mDNS: joining %s on %u.%u.%u.%u failed: %s\n", MDNS_GROUP,
                if_ip[0], if_ip[1], if_ip[2], if_ip[3], strerror(errno));
    }
    struct in_addr ifaddr;
    memcpy(&ifaddr.s_addr, if_ip, 4);
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) != 0 && MDNS_LOOPBACK_ONLY) {
        /* never fall back to the default (LAN) interface in a test */
        fprintf(stderr, "[-] mDNS: loopback multicast interface refused: %s, responder off\n", strerror(errno));
        close(fd);
        return -1;
    }
    /* TTL 0 in the test build: a multicast packet does not leave the host even if routed */
    unsigned char ttl = MDNS_LOOPBACK_ONLY ? 0 : 255, loop = MDNS_LOOPBACK_ONLY ? 1 : 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    return fd;
}

static int g_waiting_logged;

static void responder_start(void) {
    if (g_fd >= 0 || g_off_for_good) return;
    g_renames = 0;
    g_name_suffix[0] = '\0';
    rebuild_info();
    g_last_poll_ms = now_ms();
    if (!MDNS_LOOPBACK_ONLY && !has_lan_address(g_info.ip)) {
        /* no address yet: hub_mdns_tick() retries until the interface is up */
        if (!g_waiting_logged) printf("[*] mDNS: waiting for a network address\n");
        g_waiting_logged = 1;
        return;
    }
    g_waiting_logged = 0;
    g_fd = open_socket();
    if (g_fd < 0) return;
    start_probing();
    printf("[+] mDNS: advertising %s on port %d (%u.%u.%u.%u)\n", g_info.instance, g_port,
           g_info.ip[0], g_info.ip[1], g_info.ip[2], g_info.ip[3]);
}

static void responder_stop(int goodbye) {
    if (g_fd < 0) return;
    if (goodbye && g_state == ST_RUNNING) send_announce(1);
    close(g_fd);
    g_fd = -1;
    g_state = ST_OFF;
    printf("[*] mDNS: stopped\n");
}

int hub_mdns_init(int port) {
    g_port = port > 0 ? port : MDNS_PORT;
    g_initialized = 1;
    if (MDNS_LOOPBACK_ONLY && g_port == MDNS_PORT) {
        /* test build without --mdns-port (or with 5353): off, whatever mdns_enabled says */
        g_off_for_good = 1;
        printf("[*] mDNS: off in the test build (pass --mdns-port N, N != %d, to test it)\n", MDNS_PORT);
    }
    if (!g_have_settings) {
        config_store_get_settings(&g_settings);
        g_have_settings = 1;
    }
    g_enabled = g_settings.mdns_enabled;
    if (g_off_for_good) return 0;
    if (g_enabled) responder_start();
    return g_fd >= 0 ? 0 : (g_enabled ? -1 : 0);
}

int hub_mdns_fd(void) {
    return g_fd;
}

const char *hub_mdns_instance(void) {
    return g_info.instance;
}

void hub_mdns_shutdown(void) {
    responder_stop(1);
}

void hub_mdns_apply_settings(const struct hub_settings *s) {
    if (!s) return;
    g_settings = *s;
    g_have_settings = 1;
    /* apply_hub_settings() runs at daemon start before hub_mdns_init(): only keep the settings */
    if (!g_initialized) return;
    if (!s->mdns_enabled) {
        g_enabled = 0;
        responder_stop(1);
        return;
    }
    g_enabled = 1;
    if (g_fd < 0) {
        responder_start();
        return;
    }
    hub_mdns_notify_change();
}

void hub_mdns_notify_change(void) {
    if (g_fd < 0) return;
    mdns_info_t fresh;
    mdns_make_info(&fresh, g_have_settings ? &g_settings : NULL, g_name_suffix);
    int name_changed = !mdns_name_eq(fresh.instance, g_info.instance);
    int changed = name_changed || fresh.txt_len != g_info.txt_len ||
                  memcmp(fresh.txt, g_info.txt, fresh.txt_len) != 0 ||
                  memcmp(fresh.ip, g_info.ip, 4) != 0;
    if (!changed) return;
    int ip_changed = memcmp(fresh.ip, g_info.ip, 4) != 0;
    if (ip_changed) {
        /* the group membership and IP_MULTICAST_IF are bound to the old address: start over */
        printf("[*] mDNS: address changed to %u.%u.%u.%u, restarting the responder\n",
               fresh.ip[0], fresh.ip[1], fresh.ip[2], fresh.ip[3]);
        responder_stop(0);
        responder_start();
        return;
    }
    if (name_changed && g_state == ST_RUNNING) send_announce(1); /* goodbye for the old name */
    g_name_suffix[0] = name_changed ? '\0' : g_name_suffix[0];
    if (name_changed) start_probing();
    else {
        g_info = fresh;
        if (g_state == ST_RUNNING) {
            g_state = ST_ANNOUNCING;
            g_announces_sent = 0;
            g_next_action_ms = now_ms();
        }
    }
}

/* A response from another host that claims our instance or hostname */
static int is_conflict(const mdns_query_t *q, const struct sockaddr_in *from) {
    if (!(q->flags & 0x8000)) return 0; /* queries do not conflict */
    uint8_t self[4];
    memcpy(self, g_info.ip, 4);
    if (memcmp(&from->sin_addr.s_addr, self, 4) == 0) return 0;
    if (from->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) return 0; /* our own looped packets */
    for (int i = 0; i < q->nan; i++) {
        if ((q->an[i].type == 33 || q->an[i].type == 16) && mdns_name_eq(q->an[i].name, g_info.instance)) return 1;
        if (q->an[i].type == 1 && mdns_name_eq(q->an[i].name, g_info.hostname)) return 1;
    }
    return 0;
}

static void handle_conflict(void) {
    if (g_renames >= 2) return;
    g_renames++;
    snprintf(g_name_suffix, sizeof(g_name_suffix), "%s", hub_info_id());
    fprintf(stderr, "[!] mDNS: name conflict, renaming the instance with the hub id\n");
    start_probing();
}

void hub_mdns_handle(void) {
    if (g_fd < 0) return;
    uint8_t pkt[MDNS_MAX_PACKET + 1];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(g_fd, pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)&from, &flen);
    if (n <= 0) return;
    mdns_query_t q;
    if (mdns_parse_query(pkt, (size_t)n, &q) != 0) return;
    if (q.flags & 0x8000) {
        if (g_state != ST_OFF && is_conflict(&q, &from)) handle_conflict();
        return;
    }
    if (g_state != ST_RUNNING && g_state != ST_ANNOUNCING) return; /* not ours yet while probing */

    unsigned answers = 0, additionals = 0;
    int unicast = ntohs(from.sin_port) != g_port; /* legacy unicast query */
    for (int i = 0; i < q.nq; i++) {
        const mdns_rr_t *qq = &q.q[i];
        if (qq->unicast) unicast = 1;
        int any = (qq->type == 255);
        if (mdns_name_eq(qq->name, MDNS_SERVICE) && (qq->type == 12 || any)) {
            /* known-answer suppression: the querier already has our PTR with at least half the TTL */
            int known = 0;
            for (int k = 0; k < q.nan; k++) {
                if (q.an[k].type == 12 && mdns_name_eq(q.an[k].name, MDNS_SERVICE) &&
                    mdns_name_eq(q.an[k].rdname, g_info.instance) && q.an[k].ttl >= MDNS_TTL_LONG / 2) known = 1;
            }
            if (!known) {
                answers |= MDNS_R_PTR;
                additionals |= MDNS_R_SRV | MDNS_R_TXT | MDNS_R_A;
            }
        } else if (mdns_name_eq(qq->name, MDNS_ENUM_SERVICE) && (qq->type == 12 || any)) {
            answers |= MDNS_R_ENUM;
        } else if (mdns_name_eq(qq->name, g_info.instance)) {
            if (qq->type == 33 || any) { answers |= MDNS_R_SRV; additionals |= MDNS_R_A; }
            if (qq->type == 16 || any) answers |= MDNS_R_TXT;
        } else if (mdns_name_eq(qq->name, g_info.hostname) && (qq->type == 1 || any)) {
            answers |= MDNS_R_A;
        }
    }
    if (!answers) return;
    uint8_t out[MDNS_MAX_PACKET];
    /* legacy unicast queries get the query id back; multicast responses carry 0 */
    int len = mdns_build_response(&g_info, answers, additionals, 0, unicast ? q.id : 0, out, sizeof(out));
    send_to(out, len, unicast ? &from : NULL);
}

void hub_mdns_tick(void) {
    uint64_t now = now_ms();
    if (g_fd < 0) {
        /* enabled but not running: no address yet (or the socket failed), retry every 2 s */
        if (g_initialized && g_enabled && now - g_last_poll_ms >= 2000) {
            g_last_poll_ms = now;
            responder_start();
        }
        return;
    }
    switch (g_state) {
        case ST_PROBING:
            if (now >= g_next_action_ms) {
                if (g_probes_sent < 3) {
                    send_probe();
                    g_probes_sent++;
                    g_next_action_ms = now + 250;
                } else {
                    g_state = ST_ANNOUNCING;
                    g_announces_sent = 0;
                    g_next_action_ms = now;
                }
            }
            break;
        case ST_ANNOUNCING:
            if (now >= g_next_action_ms) {
                send_announce(0);
                g_announces_sent++;
                if (g_announces_sent >= 2) g_state = ST_RUNNING;
                else g_next_action_ms = now + 1000;
            }
            break;
        case ST_RUNNING:
        default:
            break;
    }
    /* IP change (and settings-driven TXT values) every 10 s */
    if (now - g_last_poll_ms >= 10000) {
        g_last_poll_ms = now;
        hub_mdns_notify_change();
    }
}
