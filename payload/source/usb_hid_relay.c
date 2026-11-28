/*
 * hub_util usbhid: the USB side of the USB Wi-Fi setup on the Linux 6.18 Hub system, in place of
 * the stock /usr/bin/usbhid (linked against the stock root, which that system does not mount).
 *
 * The HID gadget 046D:C129 (build/kernel6/usb_gadget.sh, configfs f_hid) shows up as /dev/hidg0.
 * As stock usbhid did, this relays its 64-byte reports to the TCP peer on 127.0.0.1:16716
 * (hub_util usbwifi, usb_wifi.c): after connecting it sends the connection type byte 0x06, then
 * every OUT report the PC sends goes to the peer and every 64 bytes the peer sends go to the PC
 * as an IN report. When the peer goes away it connects again once a second; reports the PC sends
 * meanwhile are dropped (the PC tools retry). An IN report the PC does not collect within a
 * second is dropped too, so no write can block the relay.
 *
 *   hub_util usbhid [device [port]]     (defaults /dev/hidg0, 16716)
 */
#include "usb_hid_relay.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define REPORT 64
#define CONN_TYPE 0x06

static int connect_peer(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    unsigned char t = CONN_TYPE;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || write(fd, &t, 1) != 1) {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_all(int fd, const unsigned char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* one IN report to the PC; dropped when the PC does not take it within a second */
static void to_host(int hid, const unsigned char *rep) {
    struct pollfd p = { hid, POLLOUT, 0 };
    if (poll(&p, 1, 1000) == 1 && (p.revents & POLLOUT)) {
        if (write(hid, rep, REPORT) != REPORT)
            fprintf(stderr, "usbhid: IN report not sent: %s\n", strerror(errno));
    }
}

int usbhid_relay_main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/hidg0";
    int port = argc > 2 ? atoi(argv[2]) : 16716;
    unsigned char in[REPORT], peer_buf[REPORT];
    size_t peer_have = 0;
    int hid = -1, peer = -1;
    time_t last_try = 0;

    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        if (hid < 0) {
            hid = open(dev, O_RDWR | O_NONBLOCK);
            if (hid < 0) {
                sleep(1);
                continue;
            }
            fprintf(stderr, "usbhid: %s open\n", dev);
        }
        if (peer < 0 && time(NULL) != last_try) {
            last_try = time(NULL);
            peer = connect_peer(port);
            peer_have = 0;
            if (peer >= 0) fprintf(stderr, "usbhid: connected to 127.0.0.1:%d\n", port);
        }
        struct pollfd p[2] = { { hid, POLLIN, 0 }, { peer, POLLIN, 0 } };
        int n = poll(p, peer >= 0 ? 2 : 1, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            close(hid);
            hid = -1;
            continue;
        }
        if (p[0].revents & POLLIN) {
            ssize_t r = read(hid, in, sizeof(in));
            if (r > 0 && peer >= 0) {
                memset(in + r, 0, sizeof(in) - (size_t)r);   /* a short report is padded */
                if (write_all(peer, in, REPORT) != 0) {
                    close(peer);
                    peer = -1;
                }
            }
        }
        if (peer >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t r = read(peer, peer_buf + peer_have, REPORT - peer_have);
            if (r <= 0) {
                fprintf(stderr, "usbhid: peer closed\n");
                close(peer);
                peer = -1;
                continue;
            }
            peer_have += (size_t)r;
            if (peer_have == REPORT) {
                to_host(hid, peer_buf);
                peer_have = 0;
            }
        }
    }
}
