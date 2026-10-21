#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 25
#endif

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC 0x63825363U

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

#define OPT_SUBNET       1
#define OPT_ROUTER       3
#define OPT_DNS          6
#define OPT_LEASE_TIME   51
#define OPT_MSG_TYPE     53
#define OPT_SERVER_ID    54
#define OPT_END          255

struct dhcp_packet {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    char     sname[64];
    char     file[128];
    uint32_t magic;
    uint8_t  options[308];
} __attribute__((packed));

static uint8_t *find_dhcp_option(uint8_t *opts, int max_len, uint8_t code) {
    int i = 0;
    while (i < max_len) {
        uint8_t c = opts[i++];
        if (c == 0) continue;
        if (c == OPT_END) break;
        if (i >= max_len) break;
        uint8_t len = opts[i++];
        if (i + len > max_len) break;
        if (c == code) return &opts[i - 2];
        i += len;
    }
    return NULL;
}

static void append_dhcp_opt(uint8_t **p, uint8_t code, const void *data, uint8_t len) {
    *(*p)++ = code;
    *(*p)++ = len;
    if (len > 0 && data) {
        memcpy(*p, data, len);
        *p += len;
    }
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <interface> <server_ip> <start_ip> <end_ip>\n", argv[0]);
        return 1;
    }

    const char *ifname = argv[1];
    struct in_addr server_addr, start_addr, end_addr;
    if (inet_aton(argv[2], &server_addr) == 0 ||
        inet_aton(argv[3], &start_addr) == 0 ||
        inet_aton(argv[4], &end_addr) == 0) {
        fprintf(stderr, "[-] Invalid IP addresses\n");
        return 1;
    }

    uint32_t start_u32 = ntohl(start_addr.s_addr);
    uint32_t end_u32 = ntohl(end_addr.s_addr);
    uint32_t pool_size = (end_u32 >= start_u32) ? (end_u32 - start_u32 + 1) : 1;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) != 0) {
        perror("SO_BINDTODEVICE");
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(DHCP_SERVER_PORT);
    srv.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) != 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    printf("[+] Clean DHCP server listening on %s (%s)...\n", ifname, argv[2]);

    while (1) {
        struct dhcp_packet req;
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        ssize_t n = recvfrom(sock, &req, sizeof(req), 0, (struct sockaddr *)&client, &clen);
        if (n < 240) continue;
        if (req.op != 1 || ntohl(req.magic) != DHCP_MAGIC) continue;

        uint8_t *msg_type_opt = find_dhcp_option(req.options, (int)(n - 240), OPT_MSG_TYPE);
        if (!msg_type_opt || msg_type_opt[1] != 1) continue;
        uint8_t msg_type = msg_type_opt[2];

        uint8_t reply_type = 0;
        if (msg_type == DHCP_DISCOVER) reply_type = DHCP_OFFER;
        else if (msg_type == DHCP_REQUEST) reply_type = DHCP_ACK;
        else continue;

        /* Pick IP based on client MAC */
        uint32_t hash = req.chaddr[3] ^ req.chaddr[4] ^ req.chaddr[5];
        uint32_t assigned_u32 = start_u32 + (hash % pool_size);

        struct dhcp_packet reply;
        memset(&reply, 0, sizeof(reply));
        reply.op = 2; /* BOOTREPLY */
        reply.htype = req.htype;
        reply.hlen = req.hlen;
        reply.xid = req.xid;
        reply.flags = req.flags;
        reply.yiaddr = htonl(assigned_u32);
        reply.siaddr = server_addr.s_addr;
        memcpy(reply.chaddr, req.chaddr, sizeof(reply.chaddr));
        reply.magic = htonl(DHCP_MAGIC);

        uint8_t *p = reply.options;
        append_dhcp_opt(&p, OPT_MSG_TYPE, &reply_type, 1);
        append_dhcp_opt(&p, OPT_SERVER_ID, &server_addr.s_addr, 4);

        uint32_t netmask = htonl(0xFFFFFF00);
        append_dhcp_opt(&p, OPT_SUBNET, &netmask, 4);
        append_dhcp_opt(&p, OPT_ROUTER, &server_addr.s_addr, 4);
        append_dhcp_opt(&p, OPT_DNS, &server_addr.s_addr, 4);

        uint32_t lease = htonl(86400);
        append_dhcp_opt(&p, OPT_LEASE_TIME, &lease, 4);
        *p++ = OPT_END;

        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(DHCP_CLIENT_PORT);
        dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        sendto(sock, &reply, (size_t)(p - (uint8_t *)&reply), 0, (struct sockaddr *)&dest, sizeof(dest));
    }

    close(sock);
    return 0;
}
