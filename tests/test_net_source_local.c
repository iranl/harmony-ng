/*
 * net_source_is_local() (hub_network_util.c): the mDNS and SSDP responders send a unicast
 * answer only to a sender on one of the Hub's links. Loopback and every address in the subnet
 * of an IPv4 interface of this host are local; an address outside all of them is not.
 */
#include "hub_network_util.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* not used by the function under test */
const char *config_store_path(const char *name) { return name; }
int write_file_content(const char *p, const char *c) { (void)p; (void)c; return -1; }
char *read_file_text_alloc(const char *p, size_t m, size_t *l) { (void)p; (void)m; if (l) *l = 0; return NULL; }

static int g_failures;
#define CHECK(c, msg) do { if (c) printf("  PASS %s\n", msg); else { printf("  FAIL %s (line %d)\n", msg, __LINE__); g_failures++; } } while (0)

static struct sockaddr_in addr_of(uint32_t host_order) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(host_order);
    return sa;
}

/* whether any IPv4 interface of this host covers a */
static int host_covers(uint32_t a) {
    struct ifaddrs *list = NULL, *i;
    int hit = 0;
    if (getifaddrs(&list) != 0) return 0;
    for (i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
        uint32_t ad = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        uint32_t m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        if (m && (a & m) == (ad & m)) hit = 1;
    }
    freeifaddrs(list);
    return hit;
}

int main(void) {
    struct sockaddr_in sa = addr_of(0x7F000005); /* 127.0.0.5 */
    CHECK(net_source_is_local(&sa), "loopback source is local");

    struct ifaddrs *list = NULL, *i;
    int checked = 0;
    if (getifaddrs(&list) == 0) {
        for (i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
            uint32_t ad = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
            uint32_t m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
            if ((ad >> 24) == 127 || m == 0xFFFFFFFFu) continue;
            sa = addr_of((ad & m) | ((ad + 1) & ~m)); /* a neighbour in the same subnet */
            CHECK(net_source_is_local(&sa), "a neighbour in an interface's subnet is local");
            checked++;
        }
        freeifaddrs(list);
    }
    if (!checked) printf("  (no non-loopback IPv4 interface on this host)\n");

    const uint32_t far[] = { 0xC6336409u /* 198.51.100.9 */, 0xCB00714Du /* 203.0.113.77 */ };
    for (unsigned k = 0; k < sizeof(far) / sizeof(far[0]); k++) {
        if (host_covers(far[k])) continue;
        sa = addr_of(far[k]);
        CHECK(!net_source_is_local(&sa), "an address outside every interface subnet is not local");
    }
    /* SSDP: private sources (also routed ones from another private network) get the answer,
     * public ones only when on a link of this host */
    const uint32_t priv[] = { 0x0A000001u, 0xAC100001u, 0xAC1FFFFEu, 0xC0A80101u, 0xA9FE0001u };
    for (unsigned k = 0; k < sizeof(priv) / sizeof(priv[0]); k++) {
        sa = addr_of(priv[k]);
        CHECK(net_source_is_private(&sa), "a private address is a private source");
    }
    const uint32_t pub[] = { 0xAC200001u /* 172.32.0.1 */, 0xC6336409u, 0x08080808u };
    for (unsigned k = 0; k < sizeof(pub) / sizeof(pub[0]); k++) {
        if (host_covers(pub[k])) continue;
        sa = addr_of(pub[k]);
        CHECK(!net_source_is_private(&sa), "a public address off our links is not");
    }
    sa = addr_of(0x7F000001u);
    CHECK(net_source_is_private(&sa), "loopback is");
    sa.sin_family = AF_INET6;
    CHECK(!net_source_is_private(&sa) && !net_source_is_private(NULL), "a non-IPv4 or missing source is not");
    sa.sin_family = AF_INET6;
    CHECK(!net_source_is_local(&sa) && !net_source_is_local(NULL), "a non-IPv4 or missing source is not local");

    if (g_failures) {
        printf("%d local source check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All local source checks passed\n");
    return 0;
}
