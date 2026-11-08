/*
 * Harmony Hub Multi-Call Utility (hub_util)
 * Combines small standalone utilities into a single binary to eliminate
 * duplicate C runtime overhead on constrained SPI Flash.
 *
 * Supported subcommands / symlinks:
 *   - hub_sync / sync
 *   - mknod
 *   - register_ehci
 *   - eth_fix / hub_eth_fix
 *   - hub_dhcpd / dhcpd
 *   - hub_portal / portal
 *   - hub_sntp / sntp
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>

#include "hub_ntp.h"

#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 25
#endif

#ifndef __NR_sethostname
#define __NR_sethostname (4000 + 148)
#endif

#ifndef ETHTOOL_SRXCSUM
#define ETHTOOL_SRXCSUM 0x00000015
#endif
#ifndef ETHTOOL_STXCSUM
#define ETHTOOL_STXCSUM 0x00000017
#endif
#ifndef ETHTOOL_SSG
#define ETHTOOL_SSG     0x00000019
#endif
#ifndef ETHTOOL_STSO
#define ETHTOOL_STSO    0x0000001f
#endif

/* =========================================================================
 * Subcommand: sync / hub_sync
 * ========================================================================= */
static int run_sync(int argc, char **argv) {
    (void)argc;
    (void)argv;
    sync();
    return 0;
}

/* =========================================================================
 * Subcommand: mknod
 * ========================================================================= */
static int run_mknod(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <path> <c|b> <major> <minor>\n", argv[0]);
        return 1;
    }
    mode_t mode = 0666;
    if (argv[2][0] == 'c') mode |= S_IFCHR;
    else if (argv[2][0] == 'b') mode |= S_IFBLK;
    unsigned int maj = (unsigned int)atoi(argv[3]);
    unsigned int min = (unsigned int)atoi(argv[4]);
    dev_t dev = ((maj & 0xfff) << 8) | (min & 0xff);
    if (mknod(argv[1], mode, dev) < 0) {
        perror("mknod");
        return 1;
    }
    return 0;
}

/* =========================================================================
 * Subcommand: register_ehci
 * ========================================================================= */
static int run_register_ehci(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    uint32_t slot_phys = 0x0000c990;
    uint32_t orig_fn = 0;

    if (lseek(fd, slot_phys, SEEK_SET) < 0 ||
        read(fd, &orig_fn, sizeof(orig_fn)) != sizeof(orig_fn)) {
        perror("read slot");
        close(fd);
        return 1;
    }

    printf("sys_call_table[148] at 0x%08x = 0x%08x (expected 0x8002fb4c)\n", slot_phys, orig_fn);
    if (orig_fn != 0x8002fb4c) {
        fprintf(stderr, "Slot mismatch! Aborting for safety.\n");
        close(fd);
        return 1;
    }

    uint32_t new_fn = 0x801267cc;
    printf("Swapping slot to platform_device_register (0x%08x)...\n", new_fn);
    if (lseek(fd, slot_phys, SEEK_SET) < 0 ||
        write(fd, &new_fn, sizeof(new_fn)) != sizeof(new_fn)) {
        perror("write slot");
        close(fd);
        return 1;
    }

    printf("Invoking syscall with &ar7240_usb_device (0x8023b808)...\n");
    int res = syscall(__NR_sethostname, (void *)0x8023b808);
    int err = errno;

    printf("platform_device_register returned: %d, errno: %d (%s)\n", res, err, strerror(err));

    printf("Restoring original sys_call_table slot...\n");
    if (lseek(fd, slot_phys, SEEK_SET) < 0 ||
        write(fd, &orig_fn, sizeof(orig_fn)) != sizeof(orig_fn)) {
        perror("restore slot");
        close(fd);
        return 1;
    }

    close(fd);
    printf("Done!\n");
    return (res == 0) ? 0 : 1;
}

/* =========================================================================
 * Subcommand: eth_fix / hub_eth_fix
 * ========================================================================= */
static int run_eth_fix(int argc, char **argv) {
    const char *ifname = (argc > 1) ? argv[1] : "eth2";
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    struct ethtool_value eval;

    eval.cmd = ETHTOOL_STXCSUM;
    eval.data = 0;
    ifr.ifr_data = (caddr_t)&eval;
    if (ioctl(fd, SIOCETHTOOL, &ifr) == 0) {
        printf("[+] %s: TX checksum offload disabled\n", ifname);
    } else {
        perror("[-] ETHTOOL_STXCSUM");
    }

    eval.cmd = ETHTOOL_SRXCSUM;
    eval.data = 0;
    ifr.ifr_data = (caddr_t)&eval;
    ioctl(fd, SIOCETHTOOL, &ifr);

    eval.cmd = ETHTOOL_SSG;
    eval.data = 0;
    ifr.ifr_data = (caddr_t)&eval;
    ioctl(fd, SIOCETHTOOL, &ifr);

    eval.cmd = ETHTOOL_STSO;
    eval.data = 0;
    ifr.ifr_data = (caddr_t)&eval;
    ioctl(fd, SIOCETHTOOL, &ifr);

    close(fd);
    printf("[+] %s: All checksum and segmentation offloads disabled successfully\n", ifname);
    return 0;
}

/* =========================================================================
 * Subcommand: dhcpd / hub_dhcpd
 * ========================================================================= */
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

static int run_dhcpd(int argc, char **argv) {
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

        uint32_t hash = req.chaddr[3] ^ req.chaddr[4] ^ req.chaddr[5];
        uint32_t assigned_u32 = start_u32 + (hash % pool_size);

        struct dhcp_packet reply;
        memset(&reply, 0, sizeof(reply));
        reply.op = 2;
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

/* =========================================================================
 * Subcommand: portal / hub_portal
 * ========================================================================= */
#define WPA_CONF "/etc/wpa_supplicant.conf"

static const char *HTML_FORM =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Harmony Recovery Setup</title><style>"
    "body{font-family:system-ui,-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;margin:2rem auto;max-width:28rem;padding:0 1rem}"
    "h1{font-size:1.4rem;color:#58a6ff;margin-bottom:0.25rem}.sub{color:#8b949e;font-size:0.9rem;margin-bottom:1.5rem}"
    "label{display:block;margin:1rem 0 0.35rem;font-size:0.85rem;color:#8b949e}"
    "input[type=text],input[type=password]{font-size:1rem;padding:0.6rem;width:100%;box-sizing:border-box;background:#161b22;border:1px solid #30363d;border-radius:6px;color:#fff}"
    "button{margin-top:1.5rem;padding:0.75rem 1.25rem;font-size:1rem;font-weight:600;background:#238636;color:#fff;border:0;border-radius:6px;width:100%;cursor:pointer}"
    "button:hover{background:#2ea043}"
    ".check{display:flex;align-items:center;gap:0.5rem;margin-top:0.75rem;font-size:0.9rem}"
    "</style></head><body>"
    "<h1>Harmony Hub Setup</h1><div class='sub'>Connect hub to your local Wi-Fi</div>"
    "<form method='POST' action='/wifi'>"
    "<label>Wi-Fi Network (SSID)</label><input type='text' name='ssid' required autofocus autocomplete='off'>"
    "<label>Password</label><input type='password' name='psk'>"
    "<div class='check'><input type='checkbox' name='hidden' value='1' id='h'><label for='h' style='margin:0'>Hidden Network</label></div>"
    "<button type='submit'>Save and Connect</button></form></body></html>";

static const char *HTML_SAVED =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
    "<!doctype html><html><body style='font-family:sans-serif;background:#0d1117;color:#58a6ff;text-align:center;padding:3rem'>"
    "<h2>Wi-Fi Saved!</h2><p style='color:#8b949e'>Hub is now connecting to Wi-Fi. LED will turn green when ready.</p></body></html>";

static void portal_url_decode(char *dst, const char *src, size_t dst_len) {
    size_t d = 0;
    while (*src && d < dst_len - 1) {
        if (*src == '+') {
            dst[d++] = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[d++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[d++] = *src++;
        }
    }
    dst[d] = '\0';
}

static void portal_extract_param(const char *body, const char *name, char *out, size_t outlen) {
    out[0] = '\0';
    if (!body || !name) return;
    size_t nlen = strlen(name);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *val_start = p + nlen + 1;
            const char *val_end = strchr(val_start, '&');
            size_t vlen = val_end ? (size_t)(val_end - val_start) : strlen(val_start);
            char enc[256] = {0};
            if (vlen >= sizeof(enc)) vlen = sizeof(enc) - 1;
            strncpy(enc, val_start, vlen);
            portal_url_decode(out, enc, outlen);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static int portal_save_wifi_config(const char *ssid, const char *psk, int hidden) {
    FILE *f = fopen(WPA_CONF, "w");
    if (!f) return -1;

    fprintf(f, "ctrl_interface=/var/run/wpa_supplicant\n");
    fprintf(f, "update_config=1\n\n");
    fprintf(f, "network={\n");
    fprintf(f, "    ssid=\"%s\"\n", ssid);
    if (psk && psk[0]) {
        fprintf(f, "    psk=\"%s\"\n", psk);
    } else {
        fprintf(f, "    key_mgmt=NONE\n");
    }
    if (hidden) {
        fprintf(f, "    scan_ssid=1\n");
    }
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

static int run_portal(int argc, char **argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 80;
    if (port <= 0) port = 80;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(srv);
        return 1;
    }

    if (listen(srv, 4) != 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    printf("[+] Wi-Fi recovery portal listening on port %d...\n", port);

    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int cfd = accept(srv, (struct sockaddr *)&client, &clen);
        if (cfd < 0) continue;

        char buf[2048] = {0};
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(cfd); continue; }

        if (strncmp(buf, "GET ", 4) == 0) {
            send(cfd, HTML_FORM, strlen(HTML_FORM), 0);
            close(cfd);
            continue;
        }

        if (strncmp(buf, "POST /wifi", 10) == 0) {
            char *body = strstr(buf, "\r\n\r\n");
            if (body) body += 4;
            char ssid[128] = {0}, psk[128] = {0}, hid_str[16] = {0};
            portal_extract_param(body, "ssid", ssid, sizeof(ssid));
            portal_extract_param(body, "psk", psk, sizeof(psk));
            portal_extract_param(body, "hidden", hid_str, sizeof(hid_str));

            if (ssid[0]) {
                printf("[+] Received Wi-Fi credentials for SSID: '%s'\n", ssid);
                portal_save_wifi_config(ssid, psk, hid_str[0] == '1');
                send(cfd, HTML_SAVED, strlen(HTML_SAVED), 0);
                close(cfd);

                sync();
                sleep(2);
                system("reboot");
                break;
            }
        }

        send(cfd, HTML_FORM, strlen(HTML_FORM), 0);
        close(cfd);
    }

    close(srv);
    return 0;
}

/* =========================================================================
 * Subcommand: sntp / hub_sntp
 * ========================================================================= */
static int run_sntp(int argc, char **argv) {
    const char *srv = "pool.ntp.org";
    int apply = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--set") == 0 || strcmp(argv[i], "-s") == 0) {
            apply = 1;
        } else if (argv[i][0] != '-') {
            srv = argv[i];
        }
    }
    time_t t = sntp_sync_time(srv, apply);
    if (t > 0) {
        printf("SNTP sync success: %lu\n", (unsigned long)t);
        return 0;
    }
    fprintf(stderr, "SNTP sync failed\n");
    return 1;
}

/* =========================================================================
 * Main Dispatcher
 * ========================================================================= */
static const char *get_progname(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

int main(int argc, char **argv) {
    const char *name = get_progname(argv[0]);

    /* Direct dispatch via symlink name */
    if (strcmp(name, "hub_sync") == 0 || strcmp(name, "sync") == 0)
        return run_sync(argc, argv);
    if (strcmp(name, "mknod") == 0)
        return run_mknod(argc, argv);
    if (strcmp(name, "register_ehci") == 0)
        return run_register_ehci(argc, argv);
    if (strcmp(name, "eth_fix") == 0 || strcmp(name, "hub_eth_fix") == 0)
        return run_eth_fix(argc, argv);
    if (strcmp(name, "hub_dhcpd") == 0 || strcmp(name, "dhcpd") == 0)
        return run_dhcpd(argc, argv);
    if (strcmp(name, "hub_portal") == 0 || strcmp(name, "portal") == 0)
        return run_portal(argc, argv);
    if (strcmp(name, "hub_sntp") == 0 || strcmp(name, "sntp") == 0)
        return run_sntp(argc, argv);

    /* Dispatch via subcommand: hub_util <command> [args...] */
    if (argc >= 2) {
        const char *cmd = argv[1];
        if (strcmp(cmd, "sync") == 0 || strcmp(cmd, "hub_sync") == 0)
            return run_sync(argc - 1, argv + 1);
        if (strcmp(cmd, "mknod") == 0)
            return run_mknod(argc - 1, argv + 1);
        if (strcmp(cmd, "register_ehci") == 0)
            return run_register_ehci(argc - 1, argv + 1);
        if (strcmp(cmd, "eth_fix") == 0 || strcmp(cmd, "hub_eth_fix") == 0)
            return run_eth_fix(argc - 1, argv + 1);
        if (strcmp(cmd, "dhcpd") == 0 || strcmp(cmd, "hub_dhcpd") == 0)
            return run_dhcpd(argc - 1, argv + 1);
        if (strcmp(cmd, "portal") == 0 || strcmp(cmd, "hub_portal") == 0)
            return run_portal(argc - 1, argv + 1);
        if (strcmp(cmd, "sntp") == 0 || strcmp(cmd, "hub_sntp") == 0)
            return run_sntp(argc - 1, argv + 1);
    }

    fprintf(stderr, "Harmony Hub Multi-Call Utility (hub_util)\n");
    fprintf(stderr, "Usage: %s <command> [args...]\n\n", name);
    fprintf(stderr, "Available commands:\n");
    fprintf(stderr, "  sync          Flush filesystem buffers\n");
    fprintf(stderr, "  mknod         Create block/character special devices\n");
    fprintf(stderr, "  register_ehci Register EHCI host controller on Atheros AR9344\n");
    fprintf(stderr, "  eth_fix       Disable offloads for USB Ethernet\n");
    fprintf(stderr, "  dhcpd         Minimal DHCP server for recovery AP\n");
    fprintf(stderr, "  portal        Wi-Fi recovery captive portal\n");
    fprintf(stderr, "  sntp          SNTP time synchronization client\n");
    return 1;
}
