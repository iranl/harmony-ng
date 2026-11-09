/*
 * Unit test of the mDNS packet builder and parser (payload/source/hub_mdns.c): the response
 * bytes for PTR, SRV, TXT and A checked against a reference assembled here by hand, including
 * name compression; the probe packet; the TXT builder; the query parser (questions, QU bit,
 * known answers, compressed names).
 *
 * Build: gcc -Wall -Wextra -O2 -Ipayload/source tests/test_mdns_packet.c payload/source/hub_mdns.c \
 *            <stubs> -o test_mdns_packet
 */
#include "hub_mdns.h"
#include "hub_info.h"
#include "http_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stubs for the responder's environment (not exercised here) */
const char *hub_info_id(void) { return "20140101120000-000001"; }
void hub_info_init(const char *p) { (void)p; }
int hub_info_id_is_fallback(void) { return 0; }
cJSON *hub_info_capabilities(void) { return cJSON_CreateArray(); }
void hub_info_add_status_fields(cJSON *o, const struct hub_settings *s) { (void)o; (void)s; }
int load_user_accounts(struct user_account *u, int m, int *en) { (void)u; (void)m; if (en) *en = 0; return 0; }
void daemon_get_mqtt_info(char *h, size_t hl, int *p, char *b, size_t bl, int *c) {
    if (h && hl) h[0] = '\0';
    if (p) *p = 1883;
    if (b && bl) b[0] = '\0';
    if (c) *c = 0;
}
void net_get_network_details(char *i, size_t il, char *g, size_t gl, char *m, size_t ml, char *mac, size_t macl) {
    (void)il; (void)gl; (void)ml; (void)macl; i[0] = g[0] = m[0] = mac[0] = '\0';
}
void net_get_iface_ip_mask_mac(const char *n, char *ip, size_t il, char *m, size_t ml, char *mac, size_t macl) {
    (void)n; (void)il; (void)ml; (void)macl; ip[0] = m[0] = mac[0] = '\0';
}
int config_store_get_settings(struct hub_settings *s) { memset(s, 0, sizeof(*s)); s->mdns_enabled = 1; return 0; }

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

/* Reference packet writer (independent of the implementation) */
static size_t ref_name(uint8_t *out, const char *name) {
    size_t n = 0;
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        out[n++] = (uint8_t)l;
        memcpy(out + n, s, l);
        n += l;
        s = dot ? dot + 1 : s + l;
    }
    out[n++] = 0;
    return n;
}
static size_t ref_u16(uint8_t *o, uint16_t v) { o[0] = (uint8_t)(v >> 8); o[1] = (uint8_t)v; return 2; }
static size_t ref_u32(uint8_t *o, uint32_t v) { ref_u16(o, (uint16_t)(v >> 16)); ref_u16(o + 2, (uint16_t)v); return 4; }

static void dump_diff(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            printf("    first difference at byte %zu: got %02x, expected %02x\n", i, a[i], b[i]);
            return;
        }
    }
}

int main(void) {
    printf("mdns packet test\n");
    mdns_info_t info;
    memset(&info, 0, sizeof(info));
    snprintf(info.instance, sizeof(info.instance), "Test Hub._harmony-ng._tcp.local");
    snprintf(info.hostname, sizeof(info.hostname), "harmony-ng-20140101120000-000001.local");
    info.port = 8089;
    info.ip[0] = 192; info.ip[1] = 168; info.ip[2] = 1; info.ip[3] = 20;
    info.txt_len = mdns_build_txt(info.txt, sizeof(info.txt), "20140101120000-000001", "2.2.0", 2, 0, 1, "harmony/hub", 0, "Test Hub");

    /* TXT builder */
    const char *txt_expect = "\x18id=20140101120000-000001" "\x09ver=2.2.0" "\x05" "api=2" "\x06" "auth=0" "\x06mqtt=1"
                             "\x11topic=harmony/hub" "\x08legacy=0" "\x0dname=Test Hub";
    CHECK(info.txt_len == strlen(txt_expect) && memcmp(info.txt, txt_expect, info.txt_len) == 0, "TXT rdata (keys in order, topic with mqtt=1)");
    char txt2[MDNS_TXT_MAX];
    size_t n2 = mdns_build_txt(txt2, sizeof(txt2), "x", "1.0.0", 2, 1, 0, "ignored", 0, "N");
    CHECK(memmem(txt2, n2, "topic=", 6) == NULL && memmem(txt2, n2, "\x06" "auth=1", 7) != NULL, "no topic without mqtt; auth=1");

    /* Full response: answer PTR, additionals SRV TXT A. Reference built by hand with the
     * compression pointers the builder must use (every repeated suffix is a pointer). */
    uint8_t ref[1024];
    size_t n = 0;
    n += ref_u16(ref + n, 0); n += ref_u16(ref + n, 0x8400); n += ref_u16(ref + n, 0);
    n += ref_u16(ref + n, 1); n += ref_u16(ref + n, 0); n += ref_u16(ref + n, 3);
    /* PTR: _harmony-ng._tcp.local (offset 12) */
    size_t off_service = n;
    n += ref_name(ref + n, "_harmony-ng._tcp.local");
    n += ref_u16(ref + n, 12); n += ref_u16(ref + n, 0x0001); n += ref_u32(ref + n, 4500);
    /* rdata: "Test Hub" + pointer to the service name */
    n += ref_u16(ref + n, 1 + 8 + 2);
    size_t off_instance = n;
    ref[n++] = 8; memcpy(ref + n, "Test Hub", 8); n += 8;
    n += ref_u16(ref + n, 0xC000 | off_service);
    /* SRV: name = pointer to instance */
    n += ref_u16(ref + n, 0xC000 | off_instance);
    n += ref_u16(ref + n, 33); n += ref_u16(ref + n, 0x8001); n += ref_u32(ref + n, 120);
    /* rdata: 0 0 8089 + hostname: "harmony-ng-20140101120000-000001" + pointer to "local" */
    size_t off_local = off_service + 1 + 11 + 1 + 4; /* after "_harmony-ng" and "_tcp" labels */
    n += ref_u16(ref + n, 6 + 1 + 32 + 2);
    n += ref_u16(ref + n, 0); n += ref_u16(ref + n, 0); n += ref_u16(ref + n, 8089);
    size_t off_host = n;
    ref[n++] = 32; memcpy(ref + n, "harmony-ng-20140101120000-000001", 32); n += 32;
    n += ref_u16(ref + n, 0xC000 | off_local);
    /* TXT */
    n += ref_u16(ref + n, 0xC000 | off_instance);
    n += ref_u16(ref + n, 16); n += ref_u16(ref + n, 0x8001); n += ref_u32(ref + n, 4500);
    n += ref_u16(ref + n, (uint16_t)info.txt_len);
    memcpy(ref + n, info.txt, info.txt_len); n += info.txt_len;
    /* A */
    n += ref_u16(ref + n, 0xC000 | off_host);
    n += ref_u16(ref + n, 1); n += ref_u16(ref + n, 0x8001); n += ref_u32(ref + n, 120);
    n += ref_u16(ref + n, 4);
    ref[n++] = 192; ref[n++] = 168; ref[n++] = 1; ref[n++] = 20;

    uint8_t pkt[1024];
    int len = mdns_build_response(&info, MDNS_R_PTR, MDNS_R_SRV | MDNS_R_TXT | MDNS_R_A, 0, 0, pkt, sizeof(pkt));
    CHECK(len == (int)n, "response length");
    CHECK(len == (int)n && memcmp(pkt, ref, n) == 0, "response bytes (PTR, SRV, TXT, A with compression)");
    if (len != (int)n || memcmp(pkt, ref, n) != 0) {
        printf("    got %d bytes, expected %zu\n", len, n);
        if (len > 0) dump_diff(pkt, ref, (size_t)len < n ? (size_t)len : n);
    }
    /* goodbye: every TTL 0 */
    int glen = mdns_build_response(&info, MDNS_R_ALL, 0, 1, 0, pkt, sizeof(pkt));
    CHECK(glen > 0, "goodbye packet built");
    mdns_query_t parsed;
    CHECK(mdns_parse_query(pkt, (size_t)glen, &parsed) == 0 && parsed.nan == 4, "goodbye parses: 4 answers");
    int all_zero = parsed.nan == 4;
    for (int i = 0; i < parsed.nan; i++) if (parsed.an[i].ttl != 0) all_zero = 0;
    CHECK(all_zero, "goodbye TTLs are 0");
    /* legacy unicast: the id is echoed */
    len = mdns_build_response(&info, MDNS_R_A, 0, 0, 0xBEEF, pkt, sizeof(pkt));
    CHECK(len > 0 && pkt[0] == 0xBE && pkt[1] == 0xEF, "query id echoed");
    /* enumeration */
    len = mdns_build_response(&info, MDNS_R_ENUM, 0, 0, 0, pkt, sizeof(pkt));
    CHECK(mdns_parse_query(pkt, (size_t)len, &parsed) == 0 && parsed.nan == 1 && parsed.an[0].type == 12 &&
          mdns_name_eq(parsed.an[0].name, MDNS_ENUM_SERVICE) && mdns_name_eq(parsed.an[0].rdname, MDNS_SERVICE),
          "enumeration PTR");
    /* too small a buffer */
    CHECK(mdns_build_response(&info, MDNS_R_ALL, 0, 0, 0, pkt, 40) == -1, "small buffer refused");

    /* Probe: 2 QU ANY questions, 2 authority records */
    len = mdns_build_probe(&info, pkt, sizeof(pkt));
    CHECK(len > 0 && mdns_parse_query(pkt, (size_t)len, &parsed) == 0, "probe parses");
    CHECK(parsed.nq == 2 && parsed.q[0].type == 255 && parsed.q[0].unicast && mdns_name_eq(parsed.q[0].name, info.instance)
          && mdns_name_eq(parsed.q[1].name, info.hostname), "probe questions: instance and hostname, ANY, QU");
    CHECK(len > 0 && pkt[8] == 0 && pkt[9] == 2, "probe has 2 authority records");

    /* Query parser: a PTR question with QU and a known answer whose rdata is a compressed name */
    uint8_t q[256];
    size_t m = 0;
    m += ref_u16(q + m, 0x1234); m += ref_u16(q + m, 0); m += ref_u16(q + m, 1); m += ref_u16(q + m, 1);
    m += ref_u16(q + m, 0); m += ref_u16(q + m, 0);
    size_t qoff = m;
    m += ref_name(q + m, "_harmony-ng._tcp.local");
    m += ref_u16(q + m, 12); m += ref_u16(q + m, 0x8001);
    m += ref_u16(q + m, 0xC000 | qoff); /* answer name: pointer */
    m += ref_u16(q + m, 12); m += ref_u16(q + m, 1); m += ref_u32(q + m, 3000);
    m += ref_u16(q + m, 1 + 8 + 2);
    q[m++] = 8; memcpy(q + m, "Test Hub", 8); m += 8;
    m += ref_u16(q + m, 0xC000 | qoff);
    CHECK(mdns_parse_query(q, m, &parsed) == 0, "query parses");
    CHECK(parsed.id == 0x1234 && parsed.nq == 1 && parsed.q[0].type == 12 && parsed.q[0].unicast &&
          mdns_name_eq(parsed.q[0].name, "_harmony-ng._tcp.local"), "question fields");
    CHECK(parsed.nan == 1 && parsed.an[0].ttl == 3000 && mdns_name_eq(parsed.an[0].rdname, "Test Hub._harmony-ng._tcp.local"),
          "known answer with compressed rdata");
    /* truncated and looping packets are refused */
    CHECK(mdns_parse_query(q, 20, &parsed) == -1, "truncated query refused");
    uint8_t loop[16] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 12 };
    CHECK(mdns_parse_query(loop, sizeof(loop), &parsed) == -1, "pointer loop refused");
    CHECK(mdns_name_eq("Test Hub._HARMONY-NG._tcp.local", "test hub._harmony-ng._tcp.local"), "case-insensitive names");

    /* mdns_make_info: unsafe characters, fallback IP, suffix after a conflict */
    struct hub_settings st;
    memset(&st, 0, sizeof(st));
    snprintf(st.hub_name, sizeof(st.hub_name), "  Living/Room: Hub #1 ");
    mdns_info_t mi;
    mdns_make_info(&mi, &st, NULL);
    CHECK(strcmp(mi.instance, "Living-Room- Hub -1._harmony-ng._tcp.local") == 0, mi.instance);
    CHECK(strcmp(mi.hostname, "harmony-ng-20140101120000-000001.local") == 0, "hostname from the hub id");
    CHECK(mi.ip[0] == 127 && mi.ip[3] == 1, "loopback fallback without an interface address");
    mdns_make_info(&mi, &st, "20140101120000-000001");
    CHECK(strcmp(mi.instance, "Living-Room- Hub -1 20140101120000-000001._harmony-ng._tcp.local") == 0, "instance with the conflict suffix");
    st.hub_name[0] = '\0';
    mdns_make_info(&mi, &st, NULL);
    CHECK(strcmp(mi.instance, "Harmony Hub._harmony-ng._tcp.local") == 0, "default name");

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All mdns packet checks passed\n");
    return 0;
}
