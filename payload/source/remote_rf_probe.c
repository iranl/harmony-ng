#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static void send_and_print(int fd, const uint8_t *cmd, size_t len, const char *label) {
    printf("[*] CMD %s: ", label);
    for (size_t i = 0; i < len; i++) printf("%02X ", cmd[i]);
    printf("\n");
    if (write(fd, cmd, len) < 0) {
        perror("write");
        return;
    }
    usleep(40000);
    uint8_t buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        printf("[+] RX (%zd bytes): ", n);
        for (ssize_t i = 0; i < n; i++) printf("%02X ", buf[i]);
        printf("\n");
    } else {
        printf("[-] No RX (n=%zd, errno=%d)\n", n, errno);
    }
}

int main(void) {
    int fd = open("/dev/rfspi", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/rfspi");
        return 1;
    }
    printf("[+] Opened /dev/rfspi (fd=%d)\n", fd);

    /* 1. Init engine */
    uint8_t c_init[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};
    send_and_print(fd, c_init, 7, "INIT");

    /* 2. Extended eQuad */
    uint8_t c_ext[7] = {0x10, 0xFF, 0x80, 0xFD, 0x01, 0x00, 0x00};
    send_and_print(fd, c_ext, 7, "EXTENDED");

    /* 3. Query Local Address (0x03) */
    uint8_t c_local[7] = {0x10, 0xFF, 0x83, 0xB5, 0x03, 0x00, 0x00};
    send_and_print(fd, c_local, 7, "LOCAL_ADDR (0x03)");

    /* 4. Query Paired Device 0 Type (0x20) */
    uint8_t c_p0_type[7] = {0x10, 0xFF, 0x83, 0xB5, 0x20, 0x00, 0x00};
    send_and_print(fd, c_p0_type, 7, "PAIRED_0_TYPE (0x20)");

    /* 5. Query Paired Device 0 Addr (0x30) */
    uint8_t c_p0_addr[7] = {0x10, 0xFF, 0x83, 0xB5, 0x30, 0x00, 0x00};
    send_and_print(fd, c_p0_addr, 7, "PAIRED_0_ADDR (0x30)");

    /* 6. Query Paired Device 1 (0x40) */
    uint8_t c_p1[7] = {0x10, 0xFF, 0x83, 0xB5, 0x40, 0x00, 0x00};
    send_and_print(fd, c_p1, 7, "PAIRED_1 (0x40)");

    /* 7. Powermode 0x20 */
    uint8_t c_pwr[7] = {0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_and_print(fd, c_pwr, 7, "POWERMODE_0x20");

    /* 8. Dongle sync slot 0 */
    uint8_t c_sync0[10] = {0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_and_print(fd, c_sync0, 10, "SYNC_SLOT_0");

    /* 9. Dongle sync slot 1 */
    uint8_t c_sync1[10] = {0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_and_print(fd, c_sync1, 10, "SYNC_SLOT_1");

    /* 10. Read any additional packets */
    for (int i = 0; i < 25; i++) {
        usleep(100000);
        uint8_t buf[64] = {0};
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            printf("[+] PUSH RX (%zd bytes): ", n);
            for (ssize_t j = 0; j < n; j++) printf("%02X ", buf[j]);
            printf("\n");
        }
    }

    close(fd);
    return 0;
}
