#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>

#define RFSPI_DEV "/dev/rfspi"
#define RFFW_DEV  "/dev/rffw"
#define CC2544_BIN "/lib/firmware/cc2544.bin"

static volatile int g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static int init_cc2544_fw(void) {
    int f_in = open(CC2544_BIN, O_RDONLY);
    if (f_in < 0) {
        fprintf(stderr, "[-] Warning: cannot open %s: %s\n", CC2544_BIN, strerror(errno));
        return -1;
    }
    int f_out = open(RFFW_DEV, O_WRONLY);
    if (f_out < 0) {
        fprintf(stderr, "[-] Warning: cannot open %s: %s\n", RFFW_DEV, strerror(errno));
        close(f_in);
        return -1;
    }
    char buf[1024];
    ssize_t n, total = 0;
    while ((n = read(f_in, buf, sizeof(buf))) > 0) {
        if (write(f_out, buf, n) != n) {
            fprintf(stderr, "[-] Error writing to %s\n", RFFW_DEV);
            close(f_in);
            close(f_out);
            return -1;
        }
        total += n;
    }
    close(f_in);
    close(f_out);
    printf("[+] Programmed %zd bytes into CC2544 firmware\n", total);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int do_flash = 0;
    if (argc > 1 && strcmp(argv[1], "--flash") == 0) {
        do_flash = 1;
    }

    if (do_flash) {
        init_cc2544_fw();
        usleep(100000);
    }

    int fd = open(RFSPI_DEV, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[-] Cannot open %s: %s\n", RFSPI_DEV, strerror(errno));
        return 1;
    }
    printf("[+] Opened %s (fd=%d)\n", RFSPI_DEV, fd);

    // 1. Query CC2544 firmware version via HID++ 1.0 Short Report
    uint8_t pkt_ver[7] = {0x10, 0xFF, 0x81, 0xF1, 0x01, 0x00, 0x00};
    printf("[*] Sending Get Version command: ");
    for (int i=0; i<7; i++) printf("%02X ", pkt_ver[i]);
    printf("\n");
    if (write(fd, pkt_ver, 7) != 7) {
        perror("write pkt_ver");
    }
    usleep(50000);

    // 2. Enable Receiver Listening Mode
    uint8_t pkt_listen[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};
    printf("[*] Sending Enable Listen Mode command: ");
    for (int i=0; i<7; i++) printf("%02X ", pkt_listen[i]);
    printf("\n");
    if (write(fd, pkt_listen, 7) != 7) {
        perror("write pkt_listen");
    }
    usleep(50000);

    printf("[+] Listening for incoming HOT packets (press buttons on remote now!)...\n");

    uint8_t rx_buf[64];
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (ret == 0) {
            // Heartbeat
            continue;
        }

        if (FD_ISSET(fd, &rfds)) {
            memset(rx_buf, 0, sizeof(rx_buf));
            ssize_t n = read(fd, rx_buf, 32);
            if (n < 0) {
                perror("read");
                usleep(50000);
                continue;
            }

            struct timeval now;
            gettimeofday(&now, NULL);
            printf("[%ld.%03ld] RX %zd bytes: ", (long)now.tv_sec, (long)(now.tv_usec / 1000), n);
            for (ssize_t i = 0; i < n; i++) {
                printf("%02X ", rx_buf[i]);
            }
            printf("\n");
            fflush(stdout);
        }
    }

    printf("[+] Closing %s\n", RFSPI_DEV);
    close(fd);
    return 0;
}
