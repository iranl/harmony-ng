#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <unistd.h>

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

int main(int argc, char **argv) {
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

    // 1. Turn off TX Checksum offload (critical on MIPS big-endian)
    eval.cmd = ETHTOOL_STXCSUM;
    eval.data = 0;
    ifr.ifr_data = (caddr_t)&eval;
    if (ioctl(fd, SIOCETHTOOL, &ifr) == 0) {
        printf("[+] %s: TX checksum offload disabled\n", ifname);
    } else {
        perror("[-] ETHTOOL_STXCSUM");
    }

    // 2. Turn off RX Checksum offload
    eval.cmd = ETHTOOL_SRXCSUM;
    eval.data = 0;
    ifr.ifr_data = (caddr_t)&eval;
    ioctl(fd, SIOCETHTOOL, &ifr);

    // 3. Turn off Scatter-Gather
    eval.cmd = ETHTOOL_SSG;
    eval.data = 0;
    ifr.ifr_data = (caddr_t)&eval;
    ioctl(fd, SIOCETHTOOL, &ifr);

    // 4. Turn off TCP Segmentation Offload
    eval.cmd = ETHTOOL_STSO;
    eval.data = 0;
    ifr.ifr_data = (caddr_t)&eval;
    ioctl(fd, SIOCETHTOOL, &ifr);

    close(fd);
    return 0;
}
