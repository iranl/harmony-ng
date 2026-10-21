#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>

#include <signal.h>

#define RFSPI_DEV "/dev/rfspi"

static void sigalrm_handler(int sig) {
    (void)sig;
}

static void send_cmd(int fd, const uint8_t *cmd, size_t len, const char *name) {
    printf("[*] Query: %s -> ", name);
    for (size_t i = 0; i < len; i++) printf("%02X ", cmd[i]);
    printf("\n");
    if (write(fd, cmd, len) < 0) {
        perror("write");
        return;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_handler;
    sigaction(SIGALRM, &sa, NULL);

    alarm(1);
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, sizeof(buf));
    alarm(0);

    if (n > 0) {
        printf("[+] Response (%zd bytes): ", n);
        for (ssize_t i = 0; i < n; i++) printf("%02X ", buf[i]);
        printf("\n");
    } else {
        if (errno == EINTR) {
            printf("[-] Timeout (no response)\n");
        } else {
            perror("read");
        }
    }
}

int main(void) {
    int fd = open(RFSPI_DEV, O_RDWR);
    if (fd < 0) {
        perror("open " RFSPI_DEV);
        return 1;
    }
    printf("[+] Opened %s (fd=%d)\n", RFSPI_DEV, fd);

    // Version
    uint8_t q_ver[7] = {0x10, 0xFF, 0x81, 0xF1, 0x01, 0x00, 0x00};
    send_cmd(fd, q_ver, 7, "Firmware Version");

    // Paired Count (0x02)
    uint8_t q_cnt[7] = {0x10, 0xFF, 0x83, 0xB5, 0x02, 0x00, 0x00};
    send_cmd(fd, q_cnt, 7, "Paired Device Count (0x02)");

    // Paired List (0x03)
    uint8_t q_list[7] = {0x10, 0xFF, 0x83, 0xB5, 0x03, 0x00, 0x00};
    send_cmd(fd, q_list, 7, "Paired Device List (0x03)");

    // Device 0 (0x20)
    uint8_t q_dev0[7] = {0x10, 0xFF, 0x83, 0xB5, 0x20, 0x00, 0x00};
    send_cmd(fd, q_dev0, 7, "Paired Device 0 Info (0x20)");

    // Device 1 (0x30)
    uint8_t q_dev1[7] = {0x10, 0xFF, 0x83, 0xB5, 0x30, 0x00, 0x00};
    send_cmd(fd, q_dev1, 7, "Paired Device 1 Info (0x30)");

    // Device 2 (0x40)
    uint8_t q_dev2[7] = {0x10, 0xFF, 0x83, 0xB5, 0x40, 0x00, 0x00};
    send_cmd(fd, q_dev2, 7, "Paired Device 2 Info (0x40)");

    // Local Device Info (0x01)
    uint8_t q_local[7] = {0x10, 0xFF, 0x83, 0xB5, 0x01, 0x00, 0x00};
    send_cmd(fd, q_local, 7, "Local Device Info (0x01)");

    // Ping Slot 0
    uint8_t q_ping0[7] = {0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_cmd(fd, q_ping0, 7, "Ping Slot 0 (10 00 00 00 00 00 00)");

    // Ping Slot 1
    uint8_t q_ping1[7] = {0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_cmd(fd, q_ping1, 7, "Ping Slot 1 (10 01 00 00 00 00 00)");

    // Dongle Sync Slot 0
    uint8_t q_sync0[10] = {0x12, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_cmd(fd, q_sync0, 10, "Dongle Sync Slot 0 (12 00 01 00...)");

    // Dongle Sync Slot 1
    uint8_t q_sync1[10] = {0x12, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_cmd(fd, q_sync1, 10, "Dongle Sync Slot 1 (12 01 01 00...)");

    close(fd);
    return 0;
}
